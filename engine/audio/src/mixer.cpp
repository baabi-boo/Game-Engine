#include "ge/audio/mixer.hpp"

#include "ge/core/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace ge::audio {
namespace {

constexpr std::size_t maximum_pcm_samples = 16U * 1024U * 1024U;
constexpr float maximum_gain = 16.0F;
constexpr float intermediate_ceiling = 1000000.0F;

bool valid_rate(std::uint32_t rate) noexcept {
    return rate >= 8000U && rate <= 192000U;
}

bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 128 &&
           std::all_of(name.begin(), name.end(), [](char value) {
               const auto byte = static_cast<unsigned char>(value);
               return byte >= 33U && byte <= 126U;
           });
}

bool valid_gain(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= maximum_gain;
}

bool valid_pitch(float value) noexcept {
    return std::isfinite(value) && value >= 0.03125F && value <= 8.0F;
}

bool valid_position(ge::math::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool valid_limiter(const LimiterSettings& value) noexcept {
    return std::isfinite(value.ceiling) && value.ceiling > 0.0F && value.ceiling <= 1.0F &&
           std::isfinite(value.release_seconds) && value.release_seconds >= 0.001 &&
           value.release_seconds <= 10.0;
}

bool valid_loop(const std::optional<LoopRegion>& loop, std::size_t frames) noexcept {
    return !loop || (loop->begin_frame < loop->end_frame && loop->end_frame <= frames);
}

void add_count(std::uint64_t& counter, std::uint64_t amount = 1) noexcept {
    const auto room = std::numeric_limits<std::uint64_t>::max() - counter;
    counter += std::min(room, amount);
}

bool fail(std::string* reason, const char* message) {
    if (reason != nullptr) {
        *reason = message;
    }
    return false;
}

double poly_blep(double phase, double step) noexcept {
    if (phase < step) {
        const double time = phase / step;
        return time + time - time * time - 1.0;
    }
    if (phase > 1.0 - step) {
        const double time = (phase - 1.0) / step;
        return time * time + time + time + 1.0;
    }
    return 0.0;
}

template<class T>
bool parse_number(std::string_view text, T& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::optional<VoiceHandle> parse_voice(std::string_view text) noexcept {
    const auto separator = text.find(':');
    VoiceHandle handle;
    if (separator == std::string_view::npos ||
        !parse_number(text.substr(0, separator), handle.slot) ||
        !parse_number(text.substr(separator + 1), handle.generation) || !handle) {
        return std::nullopt;
    }
    return handle;
}

std::string voice_text(VoiceHandle handle) {
    return std::to_string(handle.slot) + ":" + std::to_string(handle.generation);
}

} // namespace

std::size_t AudioBuffer::frame_count() const noexcept {
    return channels == 0U ? 0 : samples.size() / channels;
}

double AudioBuffer::duration_seconds() const noexcept {
    return sample_rate == 0U ? 0.0 : static_cast<double>(frame_count()) / sample_rate;
}

bool AudioBuffer::validate(std::string* reason) const {
    if (!valid_rate(sample_rate)) {
        return fail(reason, "sample rate must be in [8000,192000]");
    }
    if (channels != 1U && channels != 2U) {
        return fail(reason, "PCM must have one or two channels");
    }
    if (samples.empty() || samples.capacity() > maximum_pcm_samples || samples.size() % channels != 0) {
        return fail(reason, "PCM sample count is empty, unaligned, or exceeds the buffer cap");
    }
    for (const float sample : samples) {
        if (!std::isfinite(sample) || std::abs(sample) > 1.0F) {
            return fail(reason, "PCM samples must be finite and normalized to [-1,1]");
        }
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

std::optional<AudioBuffer> generate_tone(const ToneSpec& specification, std::string* reason) {
    const auto& tone = specification;
    if (!valid_rate(tone.sample_rate) || (tone.channels != 1U && tone.channels != 2U) ||
        !std::isfinite(tone.duration_seconds) || tone.duration_seconds <= 0.0 ||
        tone.duration_seconds > 30.0 || !std::isfinite(tone.frequency_hz) ||
        tone.frequency_hz <= 0.0 || tone.frequency_hz >= 0.5 * tone.sample_rate ||
        !std::isfinite(tone.amplitude) || tone.amplitude < 0.0F || tone.amplitude > 1.0F ||
        !std::isfinite(tone.attack_seconds) || tone.attack_seconds < 0.0 ||
        !std::isfinite(tone.release_seconds) || tone.release_seconds < 0.0 ||
        tone.attack_seconds > tone.duration_seconds || tone.release_seconds > tone.duration_seconds ||
        static_cast<unsigned>(tone.waveform) > static_cast<unsigned>(Waveform::Noise)) {
        fail(reason, "invalid tone format, frequency, duration, envelope, or waveform");
        return std::nullopt;
    }
    const auto frames = static_cast<std::size_t>(std::floor(tone.duration_seconds * tone.sample_rate));
    if (frames == 0 || frames > maximum_pcm_samples / tone.channels) {
        fail(reason, "tone frame count exceeds the PCM limit or is shorter than one frame");
        return std::nullopt;
    }
    AudioBuffer buffer{tone.sample_rate, tone.channels, std::vector<float>(frames * tone.channels)};
    double phase = 0.0;
    const double step = tone.frequency_hz / tone.sample_rate;
    std::uint32_t noise = tone.noise_seed == 0U ? 1U : tone.noise_seed;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        double value = 0.0;
        switch (tone.waveform) {
        case Waveform::Sine:
            value = std::sin(phase * 2.0 * std::numbers::pi);
            break;
        case Waveform::Triangle:
            value = 1.0 - 4.0 * std::abs(phase - 0.5);
            break;
        case Waveform::Saw:
            value = 2.0 * phase - 1.0 - poly_blep(phase, step);
            break;
        case Waveform::Square:
            value = (phase < 0.5 ? 1.0 : -1.0) + poly_blep(phase, step) -
                    poly_blep(std::fmod(phase + 0.5, 1.0), step);
            break;
        case Waveform::Noise:
            noise ^= noise << 13U;
            noise ^= noise >> 17U;
            noise ^= noise << 5U;
            value = static_cast<double>(noise) / 2147483647.5 - 1.0;
            break;
        }
        double envelope = 1.0;
        if (tone.attack_seconds > 0.0) {
            envelope = std::min(envelope, static_cast<double>(frame) /
                                           (tone.attack_seconds * tone.sample_rate));
        }
        if (tone.release_seconds > 0.0) {
            envelope = std::min(envelope, static_cast<double>(frames - frame - 1) /
                                           (tone.release_seconds * tone.sample_rate));
        }
        const float sample = static_cast<float>(std::clamp(value, -1.0, 1.0) * envelope * tone.amplitude);
        for (std::uint32_t channel = 0; channel < tone.channels; ++channel) {
            buffer.samples[frame * tone.channels + channel] = sample;
        }
        phase += step;
        phase -= std::floor(phase);
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return buffer;
}

SoundHandle::operator bool() const noexcept {
    return slot != invalid_audio_slot && generation != 0U;
}

VoiceHandle::operator bool() const noexcept {
    return slot != invalid_audio_slot && generation != 0U;
}

struct SoundLibrary::Impl {
    struct Slot {
        std::uint32_t generation{1};
        bool live{};
        bool exhausted{};
        std::string name;
        std::shared_ptr<const AudioBuffer> buffer;
    };
    SoundLibraryConfig config;
    std::vector<Slot> slots;
    std::size_t resident_samples{};
    std::string error;

    explicit Impl(SoundLibraryConfig value) : config(value) {
        if (config.max_sounds == 0 || config.max_sounds > 4096 ||
            config.max_resident_samples == 0 || config.max_resident_samples > 256U * 1024U * 1024U) {
            throw std::invalid_argument("invalid sound library capacity");
        }
        slots.resize(config.max_sounds);
    }
};

SoundLibrary::SoundLibrary(SoundLibraryConfig config) : impl_(std::make_unique<Impl>(config)) {}
SoundLibrary::~SoundLibrary() = default;

SoundHandle SoundLibrary::create(std::string name, AudioBuffer buffer) {
    if (!valid_name(name) || find(name)) {
        impl_->error = "sound name is invalid or already exists";
        return {};
    }
    if (!buffer.validate(&impl_->error)) {
        return {};
    }
    collect();
    if (buffer.samples.capacity() > impl_->config.max_resident_samples - impl_->resident_samples) {
        impl_->error = "sound resident sample budget exhausted";
        return {};
    }
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        auto& slot = impl_->slots[index];
        if (!slot.buffer && !slot.exhausted) {
            std::string bounded_name(name.data(), name.size());
            auto owned = std::make_shared<const AudioBuffer>(std::move(buffer));
            slot.name.swap(bounded_name);
            slot.buffer = std::move(owned);
            slot.live = true;
            impl_->resident_samples += slot.buffer->samples.capacity();
            impl_->error.clear();
            return {static_cast<std::uint32_t>(index), slot.generation};
        }
    }
    impl_->error = "sound slots exhausted, including retained retired buffers";
    return {};
}

SoundHandle SoundLibrary::find(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        const auto& slot = impl_->slots[index];
        if (slot.live && slot.name == name) {
            return {static_cast<std::uint32_t>(index), slot.generation};
        }
    }
    return {};
}

bool SoundLibrary::contains(SoundHandle handle) const noexcept {
    return handle.slot < impl_->slots.size() && impl_->slots[handle.slot].live &&
           impl_->slots[handle.slot].generation == handle.generation;
}

std::shared_ptr<const AudioBuffer> SoundLibrary::acquire(SoundHandle handle) const noexcept {
    return contains(handle) ? impl_->slots[handle.slot].buffer : nullptr;
}

bool SoundLibrary::remove(SoundHandle handle) noexcept {
    if (!contains(handle)) {
        return false;
    }
    auto& slot = impl_->slots[handle.slot];
    slot.live = false;
    if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
        slot.exhausted = true;
    } else {
        ++slot.generation;
    }
    collect();
    return true;
}

void SoundLibrary::clear() noexcept {
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        if (impl_->slots[index].live) {
            remove({static_cast<std::uint32_t>(index), impl_->slots[index].generation});
        }
    }
}

void SoundLibrary::collect() noexcept {
    for (auto& slot : impl_->slots) {
        if (!slot.live && slot.buffer && slot.buffer.use_count() == 1) {
            impl_->resident_samples -= slot.buffer->samples.capacity();
            slot.buffer.reset();
            slot.name.clear();
        }
    }
}

SoundLibraryStatistics SoundLibrary::statistics() const noexcept {
    SoundLibraryStatistics result;
    result.resident_samples = impl_->resident_samples;
    for (const auto& slot : impl_->slots) {
        result.live_sounds += slot.live ? 1U : 0U;
        result.retiring_sounds += !slot.live && slot.buffer ? 1U : 0U;
    }
    return result;
}

std::string_view SoundLibrary::last_error() const noexcept {
    return impl_->error;
}

struct AudioMixer::Impl {
    struct Ramp {
        float current{1.0F};
        float target{1.0F};
        std::uint32_t remaining{};
        void set(float value, std::uint32_t frames) noexcept {
            target = value;
            remaining = frames;
            if (frames == 0U) {
                current = value;
            }
        }
        float next() noexcept {
            if (remaining != 0U) {
                current += (target - current) / static_cast<float>(remaining);
                --remaining;
                if (remaining == 0U) {
                    current = target;
                }
            }
            return current;
        }
    };
    struct Voice {
        std::uint32_t generation{1};
        bool active{};
        bool paused{};
        bool stopping{};
        std::uint64_t serial{};
        SoundHandle sound;
        std::shared_ptr<const AudioBuffer> buffer;
        VoiceOptions options;
        Ramp gain;
        double cursor{};
        std::uint32_t repeats{};
        float peak{};
    };
    struct Bus {
        std::string name;
        BusId parent{master_bus};
        Ramp gain;
        bool mute{};
        bool solo{};
        bool direct_audible{true};
        bool transport_audible{true};
        float peak{};
        std::shared_ptr<AudioProcessor> processor;
    };
    MixerConfig config;
    SoundLibrary library;
    std::vector<Voice> voices;
    std::vector<Bus> buses;
    std::vector<BusId> order;
    std::vector<float> scratch;
    std::vector<float> output;
    MixerStatistics stats;
    std::size_t output_frames{};
    std::uint64_t timeline{};
    std::uint64_t serial{};
    double fractional_frames{};
    float release_coefficient{};
    float limiter_gain{1.0F};
    bool mixing{};
    const char* error{""};

    explicit Impl(MixerConfig value) : config(value), library(value.sounds) {
        if (!valid_rate(config.sample_rate) || config.max_voices == 0 || config.max_voices > 512 ||
            config.max_buses == 0 || config.max_buses > 32 || config.max_block_frames == 0 ||
            config.max_block_frames > 4096 || config.max_blocks_per_tick == 0 ||
            config.max_blocks_per_tick > 64 || !valid_limiter(config.limiter) ||
            static_cast<unsigned>(config.stealing) > static_cast<unsigned>(VoiceStealing::LowestPriority)) {
            throw std::invalid_argument("invalid audio mixer capacity or output format");
        }
        voices.resize(config.max_voices);
        buses.reserve(config.max_buses);
        Bus root;
        root.name = "master";
        buses.push_back(std::move(root));
        order.resize(config.max_buses);
        scratch.resize(config.max_buses * config.max_block_frames * 2);
        output.resize(config.max_block_frames * config.max_blocks_per_tick * 2);
        update_release();
    }

    void update_release() noexcept {
        release_coefficient = static_cast<float>(1.0 - std::exp(-1.0 /
            (static_cast<double>(config.sample_rate) * config.limiter.release_seconds)));
    }

    Voice* lookup(VoiceHandle handle) noexcept {
        if (handle.slot >= voices.size()) {
            return nullptr;
        }
        auto& value = voices[handle.slot];
        return value.active && value.generation == handle.generation ? &value : nullptr;
    }

    bool ramp_valid(std::uint32_t frames) const noexcept {
        return frames <= config.sample_rate * 60U;
    }

    void retire(std::size_t index, bool stolen = false) noexcept {
        auto& value = voices[index];
        value.active = false;
        value.buffer.reset();
        value.generation = value.generation == std::numeric_limits<std::uint32_t>::max()
                               ? 0U : value.generation + 1U;
        add_count(stolen ? stats.stolen_voices : stats.finished_voices);
    }

    void refresh_counts() noexcept {
        stats.active_voices = 0;
        stats.paused_voices = 0;
        for (const auto& value : voices) {
            stats.active_voices += value.active ? 1U : 0U;
            stats.paused_voices += value.active && value.paused ? 1U : 0U;
        }
    }

    std::size_t select_slot(std::uint16_t priority) const noexcept {
        std::size_t best = voices.size();
        for (std::size_t index = 0; index < voices.size(); ++index) {
            const auto& candidate = voices[index];
            if (!candidate.active && candidate.generation != 0U) {
                return index;
            }
            if (!candidate.active || candidate.options.protect_from_stealing ||
                candidate.options.priority > priority || config.stealing == VoiceStealing::Reject ||
                candidate.generation == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            bool prefer = best == voices.size();
            if (!prefer) {
                const auto& previous = voices[best];
                if (config.stealing == VoiceStealing::LowestPriority &&
                    candidate.options.priority != previous.options.priority) {
                    prefer = candidate.options.priority < previous.options.priority;
                } else if (config.stealing == VoiceStealing::Quietest && candidate.peak != previous.peak) {
                    prefer = candidate.peak < previous.peak;
                } else {
                    prefer = candidate.serial < previous.serial;
                }
            }
            if (prefer) {
                best = index;
            }
        }
        return best;
    }

    VoiceInfo info(std::size_t index) const noexcept {
        const auto& value = voices[index];
        return {{static_cast<std::uint32_t>(index), value.generation}, value.sound,
                value.paused ? VoiceState::Paused : VoiceState::Playing, value.options.bus,
                value.cursor, value.cursor / value.buffer->sample_rate, value.gain.current,
                value.gain.target, value.options.pan, value.options.pitch, value.peak,
                value.repeats, value.options.priority, value.options.owner, value.stopping,
                value.options.protect_from_stealing, value.options.position};
    }

    std::size_t depth(BusId id) const noexcept {
        std::size_t result = 0;
        while (id != master_bus && result < buses.size()) {
            id = buses[id].parent;
            ++result;
        }
        return result;
    }

    void rebuild_order() noexcept {
        for (std::size_t index = 0; index < buses.size(); ++index) {
            const auto id = static_cast<BusId>(index);
            std::size_t insertion = index;
            while (insertion > 0 && depth(order[insertion - 1]) < depth(id)) {
                order[insertion] = order[insertion - 1];
                --insertion;
            }
            order[insertion] = id;
        }
    }

    void prepare_solo() noexcept {
        const bool any_solo = std::any_of(buses.begin(), buses.end(), [](const Bus& value) {
            return value.solo;
        });
        for (std::size_t index = 0; index < buses.size(); ++index) {
            auto& value = buses[index];
            value.direct_audible = !any_solo;
            value.transport_audible = !any_solo;
            BusId current = static_cast<BusId>(index);
            for (std::size_t visited = 0; visited < buses.size(); ++visited) {
                value.direct_audible = value.direct_audible || buses[current].solo;
                if (current == master_bus) {
                    break;
                }
                current = buses[current].parent;
            }
            value.transport_audible = value.direct_audible;
        }
        for (std::size_t index = 0; index < buses.size(); ++index) {
            if (buses[index].solo) {
                BusId current = static_cast<BusId>(index);
                for (std::size_t visited = 0; visited < buses.size(); ++visited) {
                    buses[current].transport_audible = true;
                    if (current == master_bus) {
                        break;
                    }
                    current = buses[current].parent;
                }
            }
        }
    }

    void normalize_cursor(Voice& value) noexcept {
        if (value.options.loop && value.repeats != 0U) {
            const auto& loop = *value.options.loop;
            const double end = static_cast<double>(loop.end_frame);
            if (value.cursor >= end) {
                const double length = static_cast<double>(loop.end_frame - loop.begin_frame);
                const double traversals = std::floor((value.cursor - end) / length) + 1.0;
                if (value.repeats == infinite_repeats) {
                    value.cursor = static_cast<double>(loop.begin_frame) + std::fmod(value.cursor - end, length);
                } else {
                    const auto consumed = static_cast<std::uint32_t>(std::min(traversals,
                                                      static_cast<double>(value.repeats)));
                    value.cursor -= static_cast<double>(consumed) * length;
                    value.repeats -= consumed;
                }
            }
        }
    }

    void render_voice(std::size_t index, std::size_t frames) noexcept {
        auto& value = voices[index];
        if (!value.active || value.paused) {
            return;
        }
        value.peak = 0.0F;
        const auto& source = *value.buffer;
        const double step = static_cast<double>(value.options.pitch) * source.sample_rate / config.sample_rate;
        const double angle = (static_cast<double>(value.options.pan) + 1.0) * std::numbers::pi / 4.0;
        const float left_pan = source.channels == 1U ? static_cast<float>(std::cos(angle)) :
            static_cast<float>(std::cos(std::max(0.0F, value.options.pan) * std::numbers::pi / 2.0));
        const float right_pan = source.channels == 1U ? static_cast<float>(std::sin(angle)) :
            static_cast<float>(std::cos(std::max(0.0F, -value.options.pan) * std::numbers::pi / 2.0));
        float* destination = scratch.data() + value.options.bus * config.max_block_frames * 2;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            normalize_cursor(value);
            if (value.cursor >= static_cast<double>(source.frame_count())) {
                retire(index);
                break;
            }
            const auto first = static_cast<std::size_t>(value.cursor);
            auto second = first + 1;
            if (value.options.loop && value.repeats != 0U &&
                second == value.options.loop->end_frame) {
                second = static_cast<std::size_t>(value.options.loop->begin_frame);
            } else {
                second = std::min(second, source.frame_count() - 1);
            }
            const float fraction = static_cast<float>(value.cursor - static_cast<double>(first));
            const auto interpolate = [&](std::uint32_t channel) {
                const float start = source.samples[first * source.channels + channel];
                const float end = source.samples[second * source.channels + channel];
                return start + (end - start) * fraction;
            };
            const float gain = value.gain.next();
            const float left = interpolate(0) * gain * left_pan;
            const float right = interpolate(source.channels - 1U) * gain * right_pan;
            value.peak = std::max({value.peak, std::abs(left), std::abs(right)});
            if (buses[value.options.bus].direct_audible) {
                destination[frame * 2] += left;
                destination[frame * 2 + 1] += right;
            }
            value.cursor += step;
            normalize_cursor(value);
            if ((value.stopping && value.gain.remaining == 0U) ||
                value.cursor >= static_cast<double>(source.frame_count())) {
                retire(index);
                break;
            }
        }
    }

    float sanitize(double sample) noexcept {
        if (!std::isfinite(sample)) {
            add_count(stats.sanitized_samples);
            return 0.0F;
        }
        if (std::abs(sample) > intermediate_ceiling) {
            add_count(stats.sanitized_samples);
            return sample < 0.0 ? -intermediate_ceiling : intermediate_ceiling;
        }
        return static_cast<float>(sample);
    }

    void render(std::size_t frames, std::size_t destination_frame = 0) noexcept {
        mixing = true;
        prepare_solo();
        const auto stride = config.max_block_frames * 2;
        for (std::size_t index = 0; index < buses.size(); ++index) {
            std::fill_n(scratch.data() + index * stride, frames * 2, 0.0F);
        }
        for (std::size_t index = 0; index < voices.size(); ++index) {
            render_voice(index, frames);
        }
        for (std::size_t position = 0; position < buses.size(); ++position) {
            const BusId id = order[position];
            auto& value = buses[id];
            float* samples = scratch.data() + id * stride;
            if (value.processor) {
                value.processor->process({id, config.sample_rate, 2, timeline}, {samples, frames * 2});
            }
            value.peak = 0.0F;
            float* parent = scratch.data() + value.parent * stride;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const float gain = value.gain.next();
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    const auto offset = frame * 2 + channel;
                    const float sample = sanitize(static_cast<double>(sanitize(samples[offset])) * gain);
                    samples[offset] = value.mute || !value.transport_audible ? 0.0F : sample;
                    value.peak = std::max(value.peak, std::abs(samples[offset]));
                    if (id != master_bus) {
                        parent[offset] += samples[offset];
                    }
                }
            }
        }
        stats.pre_limit_peak = 0.0F;
        stats.output_peak = 0.0F;
        double energy = 0.0;
        float* rendered = output.data() + destination_frame * 2;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const float peak = std::max(std::abs(scratch[frame * 2]), std::abs(scratch[frame * 2 + 1]));
            stats.pre_limit_peak = std::max(stats.pre_limit_peak, peak);
            if (config.limiter.enabled) {
                const float desired = peak > config.limiter.ceiling ? config.limiter.ceiling / peak : 1.0F;
                limiter_gain = std::min(desired, limiter_gain + (1.0F - limiter_gain) * release_coefficient);
                if (limiter_gain < 0.999999F) {
                    add_count(stats.limited_frames);
                }
            } else {
                limiter_gain = 1.0F;
            }
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto offset = frame * 2 + channel;
                if (std::abs(scratch[offset]) > 1.0F) {
                    add_count(stats.over_range_samples);
                }
                const float sample = scratch[offset] * limiter_gain;
                if (std::abs(sample) > 1.0F) {
                    add_count(stats.clipped_samples);
                }
                rendered[offset] = std::clamp(sample, -1.0F, 1.0F);
                stats.output_peak = std::max(stats.output_peak, std::abs(rendered[offset]));
                energy += static_cast<double>(rendered[offset]) * rendered[offset];
            }
        }
        output_frames = destination_frame + frames;
        stats.last_block_frames = frames;
        stats.output_rms = frames == 0 ? 0.0 : std::sqrt(energy / static_cast<double>(frames * 2));
        stats.limiter_gain = limiter_gain;
        add_count(stats.rendered_frames, static_cast<std::uint64_t>(frames));
        add_count(timeline, static_cast<std::uint64_t>(frames));
        refresh_counts();
        mixing = false;
        library.collect();
    }
};

AudioMixer::AudioMixer(MixerConfig config) : impl_(std::make_unique<Impl>(config)) {}
AudioMixer::~AudioMixer() = default;
SoundLibrary& AudioMixer::sounds() noexcept { return impl_->library; }
const SoundLibrary& AudioMixer::sounds() const noexcept { return impl_->library; }
const MixerConfig& AudioMixer::config() const noexcept { return impl_->config; }
const MixerStatistics& AudioMixer::statistics() const noexcept { return impl_->stats; }
std::string_view AudioMixer::last_error() const noexcept { return impl_->error; }

VoiceHandle AudioMixer::play(SoundHandle sound, const VoiceOptions& options) {
    auto buffer = impl_->library.acquire(sound);
    if (impl_->mixing || !buffer || options.bus >= impl_->buses.size() || !valid_gain(options.gain) ||
        !std::isfinite(options.pan) || std::abs(options.pan) > 1.0F || !valid_pitch(options.pitch) ||
        !std::isfinite(options.start_frame) || options.start_frame < 0.0 ||
        options.start_frame >= static_cast<double>(buffer->frame_count()) ||
        !valid_loop(options.loop, buffer->frame_count()) || !impl_->ramp_valid(options.fade_in_frames) ||
        !valid_position(options.position)) {
        impl_->error = "invalid sound, voice parameters, or reentrant play";
        add_count(impl_->stats.rejected_voices);
        return {};
    }
    const auto index = impl_->select_slot(options.priority);
    if (index == impl_->voices.size()) {
        impl_->error = "voice capacity exhausted or every candidate is protected";
        add_count(impl_->stats.rejected_voices);
        return {};
    }
    auto& value = impl_->voices[index];
    if (value.active) {
        impl_->retire(index, true);
    }
    value.active = true;
    value.paused = false;
    value.stopping = false;
    value.sound = sound;
    value.buffer = std::move(buffer);
    value.options = options;
    value.cursor = options.start_frame;
    value.repeats = options.loop ? options.loop->repeat_count : 0U;
    value.gain.current = options.fade_in_frames == 0U ? options.gain : 0.0F;
    value.gain.set(options.gain, options.fade_in_frames);
    value.peak = options.gain;
    add_count(impl_->serial);
    value.serial = impl_->serial;
    impl_->refresh_counts();
    impl_->error = "";
    return {static_cast<std::uint32_t>(index), value.generation};
}

bool AudioMixer::stop(VoiceHandle voice_handle, std::uint32_t fade_frames) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || !impl_->ramp_valid(fade_frames)) { return false; }
    if (fade_frames == 0U || value->paused) {
        impl_->retire(voice_handle.slot);
        impl_->refresh_counts();
    } else {
        value->gain.set(0.0F, fade_frames);
        value->stopping = true;
    }
    return true;
}

std::size_t AudioMixer::stop_owner(std::uint64_t owner, std::uint32_t fade_frames) noexcept {
    if (impl_->mixing || !impl_->ramp_valid(fade_frames)) { return 0; }
    std::size_t count = 0;
    for (std::size_t index = 0; index < impl_->voices.size(); ++index) {
        const auto& value = impl_->voices[index];
        if (value.active && value.options.owner == owner &&
            stop({static_cast<std::uint32_t>(index), value.generation}, fade_frames)) { ++count; }
    }
    return count;
}

void AudioMixer::stop_all() noexcept {
    if (impl_->mixing) { return; }
    for (std::size_t index = 0; index < impl_->voices.size(); ++index) {
        if (impl_->voices[index].active) { impl_->retire(index); }
    }
    impl_->refresh_counts();
    impl_->library.collect();
}

bool AudioMixer::pause(VoiceHandle voice_handle, bool paused) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr) { return false; }
    value->paused = paused;
    value->peak = 0.0F;
    impl_->refresh_counts();
    return true;
}

bool AudioMixer::seek_frames(VoiceHandle voice_handle, double source_frame) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || !std::isfinite(source_frame) || source_frame < 0.0 ||
        source_frame > static_cast<double>(value->buffer->frame_count())) { return false; }
    value->cursor = source_frame;
    value->repeats = value->options.loop ? value->options.loop->repeat_count : 0U;
    impl_->normalize_cursor(*value);
    return true;
}

bool AudioMixer::seek_seconds(VoiceHandle voice_handle, double seconds) noexcept {
    const auto* value = impl_->lookup(voice_handle);
    return value != nullptr && std::isfinite(seconds) && seconds >= 0.0 &&
           seek_frames(voice_handle, seconds * value->buffer->sample_rate);
}

bool AudioMixer::set_gain(VoiceHandle voice_handle, float gain, std::uint32_t ramp_frames) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || value->stopping || !valid_gain(gain) ||
        !impl_->ramp_valid(ramp_frames)) { return false; }
    value->gain.set(gain, ramp_frames);
    value->options.gain = gain;
    return true;
}

bool AudioMixer::set_pan(VoiceHandle voice_handle, float pan) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || !std::isfinite(pan) || std::abs(pan) > 1.0F) { return false; }
    value->options.pan = pan;
    return true;
}

bool AudioMixer::set_pitch(VoiceHandle voice_handle, float pitch) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || !valid_pitch(pitch)) { return false; }
    value->options.pitch = pitch;
    return true;
}

bool AudioMixer::set_position(VoiceHandle voice_handle, ge::math::Vec3 position) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || !valid_position(position)) { return false; }
    value->options.position = position;
    return true;
}

bool AudioMixer::set_loop(VoiceHandle voice_handle, std::optional<LoopRegion> loop) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || !valid_loop(loop, value->buffer->frame_count())) { return false; }
    value->options.loop = loop;
    value->repeats = loop ? loop->repeat_count : 0U;
    impl_->normalize_cursor(*value);
    return true;
}

bool AudioMixer::route(VoiceHandle voice_handle, BusId bus_id) noexcept {
    auto* value = impl_->lookup(voice_handle);
    if (impl_->mixing || value == nullptr || bus_id >= impl_->buses.size()) { return false; }
    value->options.bus = bus_id;
    return true;
}

std::optional<VoiceInfo> AudioMixer::voice(VoiceHandle handle) const noexcept {
    return impl_->lookup(handle) == nullptr ? std::nullopt : std::optional<VoiceInfo>{impl_->info(handle.slot)};
}

std::size_t AudioMixer::copy_voices(std::span<VoiceInfo> destination) const noexcept {
    std::size_t copied = 0;
    for (std::size_t index = 0; index < impl_->voices.size() && copied < destination.size(); ++index) {
        if (impl_->voices[index].active) { destination[copied++] = impl_->info(index); }
    }
    return copied;
}

std::optional<BusId> AudioMixer::create_bus(std::string name, BusId parent) {
    if (impl_->mixing || !valid_name(name) || find_bus(name) || parent >= impl_->buses.size() ||
        impl_->buses.size() >= impl_->config.max_buses) { return std::nullopt; }
    Impl::Bus value;
    value.name.assign(name.data(), name.size());
    value.parent = parent;
    const auto id = static_cast<BusId>(impl_->buses.size());
    impl_->buses.push_back(std::move(value));
    impl_->rebuild_order();
    return id;
}

std::optional<BusId> AudioMixer::find_bus(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < impl_->buses.size(); ++index) {
        if (impl_->buses[index].name == name) { return static_cast<BusId>(index); }
    }
    return std::nullopt;
}

std::optional<BusInfo> AudioMixer::bus(BusId id) const noexcept {
    if (id >= impl_->buses.size()) { return std::nullopt; }
    const auto& value = impl_->buses[id];
    return BusInfo{id, value.parent, value.name, value.gain.current, value.gain.target,
                   value.peak, value.mute, value.solo, static_cast<bool>(value.processor)};
}

bool AudioMixer::set_bus_parent(BusId id, BusId parent) noexcept {
    if (impl_->mixing || id == master_bus || id >= impl_->buses.size() || parent >= impl_->buses.size()) {
        return false;
    }
    BusId current = parent;
    for (std::size_t visited = 0; visited < impl_->buses.size(); ++visited) {
        if (current == id) { return false; }
        if (current == master_bus) {
            impl_->buses[id].parent = parent;
            impl_->rebuild_order();
            return true;
        }
        current = impl_->buses[current].parent;
    }
    return false;
}

bool AudioMixer::set_bus_gain(BusId id, float gain, std::uint32_t ramp_frames) noexcept {
    if (impl_->mixing || id >= impl_->buses.size() || !valid_gain(gain) || !impl_->ramp_valid(ramp_frames)) {
        return false;
    }
    impl_->buses[id].gain.set(gain, ramp_frames);
    return true;
}

bool AudioMixer::set_bus_mute(BusId id, bool mute) noexcept {
    if (impl_->mixing || id >= impl_->buses.size()) { return false; }
    impl_->buses[id].mute = mute;
    return true;
}

bool AudioMixer::set_bus_solo(BusId id, bool solo) noexcept {
    if (impl_->mixing || id >= impl_->buses.size()) { return false; }
    impl_->buses[id].solo = solo;
    return true;
}

bool AudioMixer::set_bus_processor(BusId id, std::shared_ptr<AudioProcessor> processor) noexcept {
    if (impl_->mixing || id >= impl_->buses.size()) { return false; }
    impl_->buses[id].processor = std::move(processor);
    return true;
}

std::shared_ptr<AudioProcessor> AudioMixer::bus_processor(BusId id) const noexcept {
    if (impl_->mixing || id >= impl_->buses.size()) { return {}; }
    return impl_->buses[id].processor;
}

bool AudioMixer::replace_bus_processor(BusId id,
                                      const std::shared_ptr<AudioProcessor>& expected,
                                      std::shared_ptr<AudioProcessor> replacement) noexcept {
    if (impl_->mixing || id >= impl_->buses.size() ||
        impl_->buses[id].processor != expected) {
        return false;
    }
    impl_->buses[id].processor = std::move(replacement);
    return true;
}

bool AudioMixer::set_limiter(LimiterSettings settings) noexcept {
    if (impl_->mixing || !valid_limiter(settings)) { return false; }
    impl_->config.limiter = settings;
    impl_->update_release();
    impl_->limiter_gain = 1.0F;
    return true;
}

std::span<const float> AudioMixer::mix(std::size_t frames) noexcept {
    if (impl_->mixing || frames > impl_->config.max_block_frames) {
        impl_->error = "mix block exceeds capacity or mixing is reentrant";
        return {};
    }
    add_count(impl_->stats.requested_frames, static_cast<std::uint64_t>(frames));
    impl_->render(frames);
    impl_->error = "";
    return output();
}

bool AudioMixer::advance(double delta_seconds) noexcept {
    if (impl_->mixing || !std::isfinite(delta_seconds) || delta_seconds < 0.0 || delta_seconds > 86400.0) {
        impl_->error = "frame duration is invalid or mixing is reentrant";
        return false;
    }
    const double demand = delta_seconds * impl_->config.sample_rate + impl_->fractional_frames;
    const auto requested = static_cast<std::uint64_t>(std::floor(demand));
    impl_->fractional_frames = demand - static_cast<double>(requested);
    const auto capacity = impl_->config.max_block_frames * impl_->config.max_blocks_per_tick;
    const auto admitted = std::min(requested, static_cast<std::uint64_t>(capacity));
    add_count(impl_->stats.requested_frames, requested);
    add_count(impl_->stats.dropped_frames, requested - admitted);
    std::size_t remaining = static_cast<std::size_t>(admitted);
    std::size_t written = 0;
    float pre_limit_peak = 0.0F;
    float output_peak = 0.0F;
    double energy = 0.0;
    if (remaining == 0) { impl_->render(0); }
    while (remaining != 0) {
        const auto block = std::min(remaining, impl_->config.max_block_frames);
        impl_->render(block, written);
        pre_limit_peak = std::max(pre_limit_peak, impl_->stats.pre_limit_peak);
        output_peak = std::max(output_peak, impl_->stats.output_peak);
        energy += impl_->stats.output_rms * impl_->stats.output_rms * static_cast<double>(block * 2);
        written += block;
        remaining -= block;
    }
    impl_->stats.pre_limit_peak = pre_limit_peak;
    impl_->stats.output_peak = output_peak;
    impl_->stats.output_rms = written == 0 ? 0.0 : std::sqrt(energy / static_cast<double>(written * 2));
    impl_->error = "";
    return true;
}

std::span<const float> AudioMixer::output() const noexcept {
    return {impl_->output.data(), impl_->output_frames * 2};
}

void AudioMixer::reset_statistics() noexcept {
    if (impl_->mixing) { return; }
    const auto last_block_frames = impl_->stats.last_block_frames;
    impl_->stats = {};
    impl_->stats.limiter_gain = impl_->limiter_gain;
    impl_->stats.last_block_frames = last_block_frames;
    impl_->refresh_counts();
}

void register_mixer(ge::core::Kernel& kernel) {
    if (!kernel.register_module({"audio.mixer", "Owned PCM, bounded voices, bus mixing and CPU output", 1})) {
        return;
    }
    MixerConfig config;
    const auto setting = [&](std::string_view key, std::int64_t fallback, std::int64_t low, std::int64_t high) {
        return std::clamp(kernel.config().integer(key, fallback), low, high);
    };
    config.sample_rate = static_cast<std::uint32_t>(setting("audio.sample_rate", 48000, 8000, 192000));
    config.max_voices = static_cast<std::size_t>(setting("audio.voices", 128, 1, 512));
    config.max_block_frames = static_cast<std::size_t>(setting("audio.block_frames", 1024, 1, 4096));
    config.max_blocks_per_tick = static_cast<std::size_t>(setting("audio.blocks_per_tick", 16, 1, 64));
    auto& mixer = kernel.services().emplace<AudioMixer>(config);
    kernel.add_system({"audio.mixer", ge::core::Phase::Audio, 100, {}},
        [](ge::core::Kernel& host, const ge::core::Frame& frame) {
            auto& owner = host.services().get<AudioMixer>();
            if (!owner.advance(frame.delta_seconds)) {
                host.diagnose(ge::core::Diagnostic::Severity::Warning, "audio.mixer", std::string(owner.last_error()));
            }
            const auto& stats = owner.statistics();
            host.metric("audio.active_voices", static_cast<double>(stats.active_voices));
            host.metric("audio.rendered_frames", static_cast<double>(stats.rendered_frames));
            host.metric("audio.dropped_frames", static_cast<double>(stats.dropped_frames));
            host.metric("audio.output_peak", stats.output_peak);
            host.metric("audio.output_rms", stats.output_rms);
            host.metric("audio.clipped_samples", static_cast<double>(stats.clipped_samples));
        });
    kernel.add_shutdown("audio.mixer", [owner = &mixer] { owner->stop_all(); });
    kernel.add_command("sound.create", [](ge::core::Kernel& host, ge::core::CommandArgs args) -> ge::core::CommandResult {
        if (args.empty() || args.size() > 4) { return {false, "usage: sound.create name [hz] [seconds] [waveform]"}; }
        auto& owner = host.services().get<AudioMixer>();
        ToneSpec tone;
        tone.sample_rate = owner.config().sample_rate;
        if ((args.size() > 1 && !parse_number(args[1], tone.frequency_hz)) ||
            (args.size() > 2 && !parse_number(args[2], tone.duration_seconds))) { return {false, "invalid tone number"}; }
        if (args.size() > 3) {
            if (args[3] == "sine") { tone.waveform = Waveform::Sine; }
            else if (args[3] == "triangle") { tone.waveform = Waveform::Triangle; }
            else if (args[3] == "saw") { tone.waveform = Waveform::Saw; }
            else if (args[3] == "square") { tone.waveform = Waveform::Square; }
            else if (args[3] == "noise") { tone.waveform = Waveform::Noise; }
            else { return {false, "waveform must be sine, triangle, saw, square, or noise"}; }
        }
        tone.attack_seconds = std::min(tone.attack_seconds, tone.duration_seconds);
        tone.release_seconds = std::min(tone.release_seconds, tone.duration_seconds);
        std::string error;
        auto buffer = generate_tone(tone, &error);
        if (!buffer) { return {false, error}; }
        const auto sound = owner.sounds().create(args[0], std::move(*buffer));
        return sound ? ge::core::CommandResult{true, "created " + args[0]} :
                       ge::core::CommandResult{false, std::string(owner.sounds().last_error())};
    }, "Create original procedural PCM: name [hz] [seconds] [waveform]");
    kernel.add_command("sound.play", [](ge::core::Kernel& host, ge::core::CommandArgs args) -> ge::core::CommandResult {
        if (args.empty() || args.size() > 5) { return {false, "usage: sound.play name [gain] [pitch] [bus] [loop]"}; }
        auto& owner = host.services().get<AudioMixer>();
        VoiceOptions options;
        if ((args.size() > 1 && !parse_number(args[1], options.gain)) ||
            (args.size() > 2 && !parse_number(args[2], options.pitch))) { return {false, "invalid voice number"}; }
        if (args.size() > 3) {
            const auto bus_id = owner.find_bus(args[3]);
            if (!bus_id) { return {false, "unknown bus"}; }
            options.bus = *bus_id;
        }
        const auto sound = owner.sounds().find(args[0]);
        const auto buffer = owner.sounds().acquire(sound);
        if (!buffer) { return {false, "unknown sound"}; }
        if (args.size() > 4) {
            if (args[4] == "loop") { options.loop = LoopRegion{0, static_cast<std::uint64_t>(buffer->frame_count()), infinite_repeats}; }
            else if (args[4] != "once") { return {false, "play mode must be loop or once"}; }
        }
        const auto handle = owner.play(sound, options);
        return handle ? ge::core::CommandResult{true, voice_text(handle)} :
                        ge::core::CommandResult{false, std::string(owner.last_error())};
    }, "Start a voice and return its slot:generation token");
    kernel.add_command("sound.stop", [](ge::core::Kernel& host, ge::core::CommandArgs args) -> ge::core::CommandResult {
        if (args.empty() || args.size() > 2) { return {false, "usage: sound.stop token|all [fade_frames]"}; }
        auto& owner = host.services().get<AudioMixer>();
        if (args[0] == "all" && args.size() == 1) { owner.stop_all(); return {true, "stopped all voices"}; }
        const auto handle = parse_voice(args[0]);
        std::uint32_t fade = 0;
        if (!handle || (args.size() == 2 && !parse_number(args[1], fade))) { return {false, "invalid token or fade"}; }
        const bool stopped = owner.stop(*handle, fade);
        return {stopped, stopped ? "voice stop scheduled" : "stale voice or fade exceeds sixty seconds"};
    }, "Stop an exact voice token with an optional sample-frame fade, or stop all");
    kernel.add_command("sound.bus", [](ge::core::Kernel& host, ge::core::CommandArgs args) -> ge::core::CommandResult {
        if (args.size() < 2 || args.size() > 4) { return {false, "usage: sound.bus create|gain|mute|solo|parent name [value] [ramp_frames]"}; }
        auto& owner = host.services().get<AudioMixer>();
        if (args[0] == "create") {
            if (args.size() > 3) { return {false, "usage: sound.bus create name [parent]"}; }
            const auto parent = args.size() == 3 ? owner.find_bus(args[2]) : std::optional<BusId>{master_bus};
            if (!parent) { return {false, "unknown parent bus"}; }
            const auto id = owner.create_bus(args[1], *parent);
            return id ? ge::core::CommandResult{true, "bus " + std::to_string(*id)} :
                        ge::core::CommandResult{false, "invalid name, duplicate, or bus capacity exhausted"};
        }
        const auto id = owner.find_bus(args[1]);
        if (!id || args.size() < 3) { return {false, "unknown bus or missing value"}; }
        bool changed = false;
        if (args[0] == "gain") {
            float gain = 0.0F;
            std::uint32_t ramp = 0;
            if (!parse_number(args[2], gain) || (args.size() == 4 && !parse_number(args[3], ramp))) {
                return {false, "invalid gain or ramp"};
            }
            changed = owner.set_bus_gain(*id, gain, ramp);
        } else if (args.size() == 3 && args[0] == "parent") {
            const auto parent = owner.find_bus(args[2]);
            changed = parent && owner.set_bus_parent(*id, *parent);
        } else if (args.size() == 3 && (args[0] == "mute" || args[0] == "solo")) {
            if (args[2] != "on" && args[2] != "off") { return {false, "value must be on or off"}; }
            changed = args[0] == "mute" ? owner.set_bus_mute(*id, args[2] == "on") :
                                           owner.set_bus_solo(*id, args[2] == "on");
        }
        return {changed, changed ? "bus updated" : "invalid bus operation, cycle, gain, or ramp"};
    }, "Create or route buses; control bounded gain ramps, mute, and solo");
}

} // namespace ge::audio
