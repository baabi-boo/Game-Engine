#include <ge/audio/dsp.hpp>
#include <ge/audio/spatial.hpp>
#include <ge/core/runtime.hpp>
#include <ge/math/math.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ge::audio {
namespace {

constexpr std::size_t control_count = static_cast<std::size_t>(DspControl::Count);
constexpr double signal_ceiling = 64.0;
constexpr std::array<double, 4> comb_times{0.0253, 0.0269, 0.0289, 0.0307};
constexpr std::array<double, 2> diffusion_times{0.0051, 0.0126};

bool between(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

void increment(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}

bool fail(std::string* reason, std::string_view message) {
    if (reason) { *reason = message; }
    return false;
}

double amplitude(double db) noexcept {
    return std::pow(10.0, db / 20.0);
}

double decibels(double gain) noexcept {
    return 20.0 * std::log10(std::max(gain, 1.0e-12));
}

std::size_t index_of(DspControl control) noexcept {
    return static_cast<std::size_t>(control);
}

double damping(double hz, std::uint32_t sample_rate) noexcept {
    const double cutoff = std::min(hz, 0.45 * static_cast<double>(sample_rate));
    return std::exp(-2.0 * math::pi * cutoff / static_cast<double>(sample_rate));
}

bool valid_control(DspControl control, double value) noexcept {
    switch (control) {
    case DspControl::InputGainDb:
    case DspControl::OutputGainDb: return between(value, -60.0, 24.0);
    case DspControl::CompressorThresholdDb: return between(value, -96.0, 0.0);
    case DspControl::DelayFeedback: return between(value, 0.0, 0.95);
    case DspControl::Wet:
    case DspControl::DelayWet:
    case DspControl::ReverbWet:
    case DspControl::AcousticHighFrequencyGain:
    case DspControl::Bypass: return between(value, 0.0, 1.0);
    case DspControl::Count: return false;
    }
    return false;
}

double stored_value(DspControl control, double value) noexcept {
    return control == DspControl::InputGainDb || control == DspControl::OutputGainDb
        ? amplitude(value) : value;
}

struct Ramp {
    double current{};
    double target{};
    double step{};
    std::uint32_t remaining{};
    void set(double value, std::uint32_t frames) noexcept {
        target = value;
        remaining = frames;
        step = frames == 0 ? 0.0 : (value - current) / static_cast<double>(frames);
        if (frames == 0) { current = value; }
    }
    double next() noexcept {
        if (remaining != 0) {
            --remaining;
            current = remaining == 0 ? target : current + step;
        }
        return current;
    }
};

struct SampleLine {
    std::vector<float> samples;
    std::size_t cursor{};
    double damped{};
    double feedback{};
    void clear() noexcept {
        std::fill(samples.begin(), samples.end(), 0.0F);
        cursor = 0;
        damped = 0.0;
    }
    double front() const noexcept { return samples[cursor]; }
    void push(float sample) noexcept {
        samples[cursor] = sample;
        cursor = cursor + 1 == samples.size() ? 0 : cursor + 1;
    }
    double tap(double delay) const noexcept {
        const double length = static_cast<double>(samples.size());
        delay = std::clamp(delay, 1.0, length - 2.0);
        const auto whole = static_cast<std::size_t>(delay);
        const auto first = (cursor + samples.size() - whole) % samples.size();
        const auto second = first == 0 ? samples.size() - 1 : first - 1;
        const double fraction = delay - static_cast<double>(whole);
        return samples[first] + (samples[second] - samples[first]) * fraction;
    }
};

struct FilterState {
    double first{};
    double second{};
};

} // namespace

bool BiquadCoefficients::stable() const noexcept {
    return std::isfinite(b0) && std::isfinite(b1) && std::isfinite(b2) &&
        std::isfinite(a1) && std::isfinite(a2) && std::abs(a2) < 1.0 &&
        1.0 + a1 + a2 > 1.0e-12 && 1.0 - a1 + a2 > 1.0e-12;
}

double BiquadCoefficients::magnitude(double frequency_hz, std::uint32_t sample_rate) const noexcept {
    if (!stable() || sample_rate == 0 || !between(frequency_hz, 0.0, 0.5 * sample_rate)) {
        return 0.0;
    }
    const double angle = 2.0 * math::pi * frequency_hz / sample_rate;
    const double real_n = b0 + b1 * std::cos(angle) + b2 * std::cos(2.0 * angle);
    const double imag_n = b1 * std::sin(angle) + b2 * std::sin(2.0 * angle);
    const double real_d = 1.0 + a1 * std::cos(angle) + a2 * std::cos(2.0 * angle);
    const double imag_d = a1 * std::sin(angle) + a2 * std::sin(2.0 * angle);
    const double denominator = real_d * real_d + imag_d * imag_d;
    const double response = denominator <= 1.0e-24 ? 0.0 :
        std::sqrt((real_n * real_n + imag_n * imag_n) / denominator);
    return std::isfinite(response) ? response : 0.0;
}

std::optional<BiquadCoefficients> design_biquad(const DspFilter& filter,
                                               std::uint32_t sample_rate) noexcept {
    if (sample_rate < 8000 || sample_rate > 192000 ||
        !between(filter.frequency_hz, 10.0, 0.49 * sample_rate) ||
        !between(filter.quality, 0.1, 12.0) || !between(filter.gain_db, -24.0, 24.0)) {
        return std::nullopt;
    }
    const double angle = 2.0 * math::pi * filter.frequency_hz / sample_rate;
    const double cosine = std::cos(angle);
    const double alpha = std::sin(angle) / (2.0 * filter.quality);
    const double gain = std::pow(10.0, filter.gain_db / 40.0);
    const double beta = 2.0 * std::sqrt(gain) * alpha;
    double a0 = 1.0 + alpha;
    BiquadCoefficients result{1.0, 0.0, 0.0, -2.0 * cosine, 1.0 - alpha};
    switch (filter.kind) {
    case DspFilterKind::LowPass:
        result.b0 = (1.0 - cosine) * 0.5;
        result.b1 = 1.0 - cosine;
        result.b2 = result.b0;
        break;
    case DspFilterKind::HighPass:
        result.b0 = (1.0 + cosine) * 0.5;
        result.b1 = -(1.0 + cosine);
        result.b2 = result.b0;
        break;
    case DspFilterKind::BandPass:
        result.b0 = alpha;
        result.b1 = 0.0;
        result.b2 = -alpha;
        break;
    case DspFilterKind::Notch:
        result.b0 = 1.0;
        result.b1 = -2.0 * cosine;
        result.b2 = 1.0;
        break;
    case DspFilterKind::AllPass:
        result.b0 = 1.0 - alpha;
        result.b1 = -2.0 * cosine;
        result.b2 = 1.0 + alpha;
        break;
    case DspFilterKind::Peak:
        result.b0 = 1.0 + alpha * gain;
        result.b1 = -2.0 * cosine;
        result.b2 = 1.0 - alpha * gain;
        a0 = 1.0 + alpha / gain;
        result.a2 = 1.0 - alpha / gain;
        break;
    case DspFilterKind::LowShelf:
        result.b0 = gain * (gain + 1.0 - (gain - 1.0) * cosine + beta);
        result.b1 = 2.0 * gain * (gain - 1.0 - (gain + 1.0) * cosine);
        result.b2 = gain * (gain + 1.0 - (gain - 1.0) * cosine - beta);
        a0 = gain + 1.0 + (gain - 1.0) * cosine + beta;
        result.a1 = -2.0 * (gain - 1.0 + (gain + 1.0) * cosine);
        result.a2 = gain + 1.0 + (gain - 1.0) * cosine - beta;
        break;
    case DspFilterKind::HighShelf:
        result.b0 = gain * (gain + 1.0 + (gain - 1.0) * cosine + beta);
        result.b1 = -2.0 * gain * (gain - 1.0 + (gain + 1.0) * cosine);
        result.b2 = gain * (gain + 1.0 + (gain - 1.0) * cosine - beta);
        a0 = gain + 1.0 - (gain - 1.0) * cosine + beta;
        result.a1 = 2.0 * (gain - 1.0 - (gain + 1.0) * cosine);
        result.a2 = gain + 1.0 - (gain - 1.0) * cosine - beta;
        break;
    default: return std::nullopt;
    }
    if (!std::isfinite(a0) || std::abs(a0) < 1.0e-12) { return std::nullopt; }
    result.b0 /= a0;
    result.b1 /= a0;
    result.b2 /= a0;
    result.a1 /= a0;
    result.a2 /= a0;
    return result.stable() ? std::optional<BiquadCoefficients>{result} : std::nullopt;
}

bool validate_dsp(const DspFormat& format, const DspSettings& settings, std::string* reason) {
    if (format.sample_rate < 8000 || format.sample_rate > 192000 ||
        (format.channels != 1 && format.channels != 2) ||
        format.max_block_frames == 0 || format.max_block_frames > 4096) {
        return fail(reason, "DSP requires 8-192 kHz mono/stereo and 1-4096 block frames");
    }
    if (settings.band_count > dsp_max_bands) { return fail(reason, "DSP equalizer exceeds eight bands"); }
    for (std::size_t band = 0; band < settings.band_count; ++band) {
        if (!design_biquad(settings.equalizer[band], format.sample_rate)) {
            return fail(reason, "DSP equalizer frequency, Q, gain or pole stability is invalid");
        }
    }
    if (!between(settings.input_gain_db, -60.0, 24.0) ||
        !between(settings.output_gain_db, -60.0, 24.0) || !between(settings.wet, 0.0, 1.0) ||
        !between(settings.acoustic_crossover_hz, 20.0, 20000.0)) {
        return fail(reason, "DSP gain, wet mix or acoustic crossover is invalid");
    }
    const auto& delay = settings.delay;
    const double minimum_delay = 1.0 / static_cast<double>(format.sample_rate);
    if (!between(delay.maximum_seconds, 0.001, 5.0) ||
        !between(delay.left_seconds, minimum_delay, delay.maximum_seconds) ||
        !between(delay.right_seconds, minimum_delay, delay.maximum_seconds) ||
        !between(delay.feedback, 0.0, 0.95) || !between(delay.crossfeed, 0.0, 1.0) ||
        !between(delay.damping_hz, 20.0, 20000.0) || !between(delay.wet, 0.0, 1.0)) {
        return fail(reason, "DSP delay exceeds its duration, feedback or damping bounds");
    }
    const auto& reverb = settings.reverb;
    if (!between(reverb.room_scale, 0.25, 2.0) || !between(reverb.decay_seconds, 0.1, 20.0) ||
        !between(reverb.predelay_seconds, 0.0, 0.25) || !between(reverb.damping_hz, 20.0, 20000.0) ||
        !between(reverb.stereo_width, 0.0, 1.0) || !between(reverb.wet, 0.0, 1.0)) {
        return fail(reason, "DSP reverb size, decay, predelay or mix is invalid");
    }
    const auto& compressor = settings.compressor;
    if (!between(compressor.threshold_db, -96.0, 0.0) || !between(compressor.ratio, 1.0, 40.0) ||
        !between(compressor.knee_db, 0.0, 24.0) || !between(compressor.attack_seconds, 0.00005, 1.0) ||
        !between(compressor.release_seconds, 0.001, 10.0) || !between(compressor.makeup_db, -24.0, 24.0)) {
        return fail(reason, "DSP compressor threshold, envelope or ratio is invalid");
    }
    if (reason) { reason->clear(); }
    return true;
}

DspSettings dsp_preset(DspPreset preset, std::uint32_t sample_rate) noexcept {
    DspSettings result;
    const double high = 0.45 * static_cast<double>(std::clamp(sample_rate, 8000U, 192000U));
    switch (preset) {
    case DspPreset::Neutral: break;
    case DspPreset::Room:
        result.reverb.enabled = true;
        result.reverb.room_scale = 0.65;
        result.reverb.decay_seconds = 0.65;
        result.reverb.predelay_seconds = 0.008;
        break;
    case DspPreset::Hall:
        result.reverb.enabled = true;
        result.reverb.room_scale = 1.8;
        result.reverb.decay_seconds = 3.5;
        result.reverb.predelay_seconds = 0.035;
        result.reverb.wet = 0.28;
        break;
    case DspPreset::Echo:
        result.delay.enabled = true;
        result.delay.feedback = 0.45;
        result.delay.wet = 0.3;
        break;
    case DspPreset::Radio:
        result.band_count = 3;
        result.equalizer[0] = {DspFilterKind::HighPass, 320.0, 0.707, 0.0};
        result.equalizer[1] = {DspFilterKind::LowPass, std::min(3200.0, high), 0.707, 0.0};
        result.equalizer[2] = {DspFilterKind::Peak, 1300.0, 1.2, 4.0};
        result.compressor.enabled = true;
        result.compressor.ratio = 6.0;
        result.compressor.makeup_db = 3.0;
        break;
    case DspPreset::Speech:
        result.band_count = 2;
        result.equalizer[0] = {DspFilterKind::HighPass, 90.0, 0.707, 0.0};
        result.equalizer[1] = {DspFilterKind::Peak, std::min(2500.0, high), 0.8, 2.0};
        result.compressor.enabled = true;
        result.compressor.ratio = 2.5;
        result.compressor.makeup_db = 2.0;
        break;
    }
    return result;
}

struct DspProcessor::Impl {
    DspFormat format;
    DspSettings settings;
    DspStatistics stats;
    std::array<BiquadCoefficients, dsp_max_bands> coefficients{};
    std::array<std::array<FilterState, 2>, dsp_max_bands> filters{};
    std::array<Ramp, control_count> controls{};
    std::array<DspAutomation, dsp_max_automation> events{};
    std::size_t event_head{};
    std::size_t event_count{};
    std::array<SampleLine, 2> delays;
    std::array<SampleLine, 2> predelays;
    std::array<std::array<SampleLine, 4>, 2> combs;
    std::array<std::array<SampleLine, 2>, 2> diffusers;
    std::array<double, 2> acoustic_low{};
    double acoustic_damping{};
    double delay_damping{};
    double reverb_damping{};
    double attack{};
    double release{};
    double envelope{};
    double makeup{};
    bool clock_observed{};

    Impl(DspFormat value, const DspSettings& configuration, std::size_t budget)
        : format(value), settings(configuration) {
        for (std::size_t band = 0; band < settings.band_count; ++band) {
            coefficients[band] = *design_biquad(settings.equalizer[band], format.sample_rate);
        }
        const std::array<double, control_count> initial{
            amplitude(settings.input_gain_db), amplitude(settings.output_gain_db), settings.wet,
            settings.delay.wet, settings.delay.feedback, settings.reverb.wet,
            settings.compressor.threshold_db, 1.0, 0.0};
        for (std::size_t index = 0; index < controls.size(); ++index) { controls[index].set(initial[index], 0); }
        acoustic_damping = damping(settings.acoustic_crossover_hz, format.sample_rate);
        delay_damping = damping(settings.delay.damping_hz, format.sample_rate);
        reverb_damping = damping(settings.reverb.damping_hz, format.sample_rate);
        attack = std::exp(-1.0 / (format.sample_rate * settings.compressor.attack_seconds));
        release = std::exp(-1.0 / (format.sample_rate * settings.compressor.release_seconds));
        makeup = amplitude(settings.compressor.makeup_db);
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            if (settings.delay.enabled) {
                allocate(delays[channel], static_cast<std::size_t>(std::ceil(
                    settings.delay.maximum_seconds * format.sample_rate)) + 2, budget);
            }
            if (!settings.reverb.enabled) { continue; }
            if (settings.reverb.predelay_seconds > 0.0) {
                allocate(predelays[channel], static_cast<std::size_t>(std::ceil(
                    settings.reverb.predelay_seconds * format.sample_rate)) + 2, budget);
            }
            for (std::size_t index = 0; index < comb_times.size(); ++index) {
                const double seconds = comb_times[index] * settings.reverb.room_scale +
                    static_cast<double>(channel) * 0.000521;
                auto& line = combs[channel][index];
                allocate(line, static_cast<std::size_t>(std::ceil(seconds * format.sample_rate)), budget);
                line.feedback = std::exp(-std::log(1000.0) * static_cast<double>(line.samples.size()) /
                    (format.sample_rate * settings.reverb.decay_seconds));
            }
            for (std::size_t index = 0; index < diffusion_times.size(); ++index) {
                const double seconds = diffusion_times[index] * settings.reverb.room_scale +
                    static_cast<double>(channel) * 0.000317;
                allocate(diffusers[channel][index],
                         static_cast<std::size_t>(std::ceil(seconds * format.sample_rate)), budget);
            }
        }
    }

    void allocate(SampleLine& line, std::size_t count, std::size_t budget) {
        if (count > budget - stats.resident_samples) { throw std::length_error("DSP sample budget exhausted"); }
        line.samples.resize(count);
        if (line.samples.capacity() > budget - stats.resident_samples) {
            throw std::length_error("DSP allocator capacity exceeds sample budget");
        }
        stats.resident_samples += line.samples.capacity();
    }

    double clean(double value) noexcept {
        if (!std::isfinite(value)) {
            increment(stats.sanitized_samples);
            return 0.0;
        }
        if (std::abs(value) > signal_ceiling) {
            increment(stats.bounded_samples);
            return std::clamp(value, -signal_ceiling, signal_ceiling);
        }
        return std::abs(value) < 1.0e-20 ? 0.0 : value;
    }

    bool control(DspControl parameter, double value, std::uint32_t frames) noexcept {
        if (!valid_control(parameter, value) || frames > format.sample_rate * 60U) { return false; }
        auto& ramp = controls[index_of(parameter)];
        const double converted = stored_value(parameter, value);
        if (ramp.target != converted || (frames == 0 && ramp.current != converted)) {
            ramp.set(converted, frames);
            increment(stats.applied_controls);
        }
        return true;
    }

    void apply_events(std::uint64_t frame) noexcept {
        while (event_count != 0 && events[event_head].output_frame <= frame) {
            const auto event = events[event_head];
            if (event.output_frame < frame) { increment(stats.late_controls); }
            (void)control(event.control, event.value, event.ramp_frames);
            event_head = (event_head + 1) % events.size();
            --event_count;
        }
    }

    double equalize(double input, std::size_t channel) noexcept {
        for (std::size_t band = 0; band < settings.band_count; ++band) {
            const auto& coefficient = coefficients[band];
            auto& state = filters[band][channel];
            const double output = clean(coefficient.b0 * input + state.first);
            state.first = clean(coefficient.b1 * input - coefficient.a1 * output + state.second);
            state.second = clean(coefficient.b2 * input - coefficient.a2 * output);
            input = output;
        }
        return input;
    }

    double compress(double peak, double threshold) noexcept {
        const double smoothing = peak > envelope ? attack : release;
        envelope = clean(peak + smoothing * (envelope - peak));
        const double over = decibels(envelope) - threshold;
        const double knee = settings.compressor.knee_db;
        const double slope = 1.0 / settings.compressor.ratio - 1.0;
        double reduction = 0.0;
        if (knee == 0.0 || over >= knee * 0.5) {
            reduction = slope * std::max(over, 0.0);
        } else if (over > -knee * 0.5) {
            const double knee_position = over + knee * 0.5;
            reduction = slope * knee_position * knee_position / (2.0 * knee);
        }
        stats.gain_reduction_db = std::max(stats.gain_reduction_db, -reduction);
        return amplitude(reduction) * makeup;
    }

    void delay(std::array<double, 2>& signal, double wet, double feedback) noexcept {
        std::array<double, 2> echo{};
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            const double seconds = channel == 0 ? settings.delay.left_seconds : settings.delay.right_seconds;
            echo[channel] = delays[channel].tap(seconds * format.sample_rate);
            auto& filtered = delays[channel].damped;
            filtered = clean(echo[channel] + delay_damping * (filtered - echo[channel]));
        }
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            const std::size_t other = format.channels == 2 ? 1 - channel : channel;
            const double returned = delays[channel].damped + settings.delay.crossfeed *
                (delays[other].damped - delays[channel].damped);
            delays[channel].push(static_cast<float>(clean(signal[channel] + returned * feedback)));
            signal[channel] = clean(signal[channel] + wet * (echo[channel] - signal[channel]));
        }
    }

    void reverberate(std::array<double, 2>& signal, double wet) noexcept {
        std::array<double, 2> reverberant{};
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            double input = signal[channel];
            if (!predelays[channel].samples.empty()) {
                input = predelays[channel].tap(settings.reverb.predelay_seconds * format.sample_rate);
                predelays[channel].push(static_cast<float>(signal[channel]));
            }
            double sum = 0.0;
            for (auto& line : combs[channel]) {
                const double delayed = line.front();
                line.damped = clean(delayed + reverb_damping * (line.damped - delayed));
                line.push(static_cast<float>(clean(input + line.feedback * line.damped)));
                sum += delayed * 0.25;
            }
            for (auto& line : diffusers[channel]) {
                const double delayed = line.front();
                const double output = clean(delayed - 0.5 * sum);
                line.push(static_cast<float>(clean(sum + 0.5 * output)));
                sum = output;
            }
            reverberant[channel] = sum;
        }
        if (format.channels == 2) {
            const double middle = (reverberant[0] + reverberant[1]) * 0.5;
            const double side = (reverberant[0] - reverberant[1]) * 0.5 * settings.reverb.stereo_width;
            reverberant = {middle + side, middle - side};
        }
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            signal[channel] = clean(signal[channel] + wet * (reverberant[channel] - signal[channel]));
        }
    }
};

DspProcessor::DspProcessor(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {}
DspProcessor::~DspProcessor() = default;

std::shared_ptr<DspProcessor> DspProcessor::prepare(DspFormat format, const DspSettings& settings,
                                                   std::size_t sample_budget, std::string* reason) {
    if (!validate_dsp(format, settings, reason)) { return {}; }
    try {
        auto implementation = std::make_unique<Impl>(format, settings, sample_budget);
        return std::shared_ptr<DspProcessor>(new DspProcessor(std::move(implementation)));
    } catch (const std::bad_alloc&) {
        (void)fail(reason, "DSP allocation failed; previous processor remains installed");
    } catch (const std::length_error& error) {
        (void)fail(reason, error.what());
    }
    return {};
}

void DspProcessor::process(const AudioProcessContext& context, std::span<float> samples) noexcept {
    auto& state = *impl_;
    const auto channels = state.format.channels;
    if (context.sample_rate != state.format.sample_rate || context.channels != channels ||
        samples.size() % channels != 0 || samples.size() / channels > state.format.max_block_frames ||
        samples.size() / channels > std::numeric_limits<std::uint64_t>::max() - context.first_output_frame) {
        increment(state.stats.rejected_blocks);
        return;
    }
    if (samples.empty()) { return; }
    if (state.clock_observed && state.stats.next_output_frame != context.first_output_frame) {
        increment(state.stats.discontinuities);
    }
    state.clock_observed = true;
    state.stats.input_peak = 0.0;
    state.stats.output_peak = 0.0;
    state.stats.gain_reduction_db = 0.0;
    double energy = 0.0;
    const std::size_t frames = samples.size() / channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        state.apply_events(context.first_output_frame + static_cast<std::uint64_t>(frame));
        std::array<double, control_count> values{};
        for (std::size_t index = 0; index < values.size(); ++index) { values[index] = state.controls[index].next(); }
        std::array<double, 2> original{};
        std::array<double, 2> signal{};
        double detector = 0.0;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            original[channel] = state.clean(samples[frame * channels + channel]);
            state.stats.input_peak = std::max(state.stats.input_peak, std::abs(original[channel]));
            const double filtered = state.equalize(state.clean(original[channel] *
                values[index_of(DspControl::InputGainDb)]), channel);
            auto& low = state.acoustic_low[channel];
            low = state.clean(filtered + state.acoustic_damping * (low - filtered));
            signal[channel] = low + (filtered - low) * values[index_of(DspControl::AcousticHighFrequencyGain)];
            detector = std::max(detector, std::abs(signal[channel]));
        }
        if (state.settings.compressor.enabled) {
            const double gain = state.compress(detector, values[index_of(DspControl::CompressorThresholdDb)]);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                signal[channel] = state.clean(signal[channel] * gain);
            }
        }
        if (state.settings.delay.enabled) {
            state.delay(signal, values[index_of(DspControl::DelayWet)], values[index_of(DspControl::DelayFeedback)]);
        }
        if (state.settings.reverb.enabled) { state.reverberate(signal, values[index_of(DspControl::ReverbWet)]); }
        const double wet = values[index_of(DspControl::Wet)] * (1.0 - values[index_of(DspControl::Bypass)]);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const double processed = signal[channel] * values[index_of(DspControl::OutputGainDb)];
            const double output = state.clean(original[channel] + wet * (processed - original[channel]));
            samples[frame * channels + channel] = static_cast<float>(output);
            state.stats.output_peak = std::max(state.stats.output_peak, std::abs(output));
            energy += output * output;
        }
    }
    state.stats.output_rms = std::sqrt(energy / static_cast<double>(samples.size()));
    state.stats.next_output_frame = context.first_output_frame + static_cast<std::uint64_t>(frames);
    increment(state.stats.processed_blocks);
    increment(state.stats.processed_frames, static_cast<std::uint64_t>(frames));
}

bool DspProcessor::set_control(DspControl control, double value, std::uint32_t ramp_frames) noexcept {
    return impl_->control(control, value, ramp_frames);
}

bool DspProcessor::schedule(DspAutomation event) noexcept {
    auto& state = *impl_;
    if (!valid_control(event.control, event.value) || event.ramp_frames > state.format.sample_rate * 60U ||
        state.event_count == state.events.size()) { return false; }
    std::size_t insertion = state.event_count;
    while (insertion != 0) {
        const auto previous = (state.event_head + insertion - 1) % state.events.size();
        if (state.events[previous].output_frame <= event.output_frame) { break; }
        state.events[(state.event_head + insertion) % state.events.size()] = state.events[previous];
        --insertion;
    }
    state.events[(state.event_head + insertion) % state.events.size()] = event;
    ++state.event_count;
    return true;
}

void DspProcessor::cancel_automation() noexcept {
    impl_->event_count = 0;
    impl_->event_head = 0;
}

std::size_t DspProcessor::cancel_automation(DspControl control) noexcept {
    auto& state = *impl_;
    std::size_t kept = 0;
    for (std::size_t index = 0; index < state.event_count; ++index) {
        const auto event = state.events[(state.event_head + index) % state.events.size()];
        if (event.control != control) {
            state.events[(state.event_head + kept) % state.events.size()] = event;
            ++kept;
        }
    }
    const auto removed = state.event_count - kept;
    state.event_count = kept;
    return removed;
}

void DspProcessor::reset() noexcept {
    auto& state = *impl_;
    const auto clock = state.stats.next_output_frame;
    const auto resident = state.stats.resident_samples;
    state.stats = {};
    state.stats.next_output_frame = clock;
    state.stats.resident_samples = resident;
    state.filters = {};
    state.acoustic_low = {};
    state.envelope = 0.0;
    for (auto& ramp : state.controls) { ramp.set(ramp.target, 0); }
    for (auto& line : state.delays) { line.clear(); }
    for (auto& line : state.predelays) { line.clear(); }
    for (auto& bank : state.combs) { for (auto& line : bank) { line.clear(); } }
    for (auto& bank : state.diffusers) { for (auto& line : bank) { line.clear(); } }
    cancel_automation();
}

std::optional<double> DspProcessor::target(DspControl control) const noexcept {
    if (index_of(control) >= control_count) { return std::nullopt; }
    const double value = impl_->controls[index_of(control)].target;
    return control == DspControl::InputGainDb || control == DspControl::OutputGainDb ? decibels(value) : value;
}

const DspSettings& DspProcessor::settings() const noexcept { return impl_->settings; }
const DspFormat& DspProcessor::format() const noexcept { return impl_->format; }
DspStatistics DspProcessor::statistics() const noexcept {
    auto result = impl_->stats;
    result.queued_controls = impl_->event_count;
    return result;
}

struct DspService::Impl {
    struct Slot {
        std::shared_ptr<DspProcessor> processor;
        double acoustic_gain{1.0};
        std::size_t contributors{};
        bool following{};
        bool lost{};
    };
    AudioMixer& mixer;
    DspServiceConfig configuration;
    std::array<Slot, dsp_max_buses> slots{};
    std::array<std::shared_ptr<DspProcessor>, dsp_max_buses> retiring{};
    std::array<VoiceInfo, 512> voices{};
    DspServiceStatistics totals{};
    std::string_view error;

    Impl(AudioMixer& source, DspServiceConfig config) : mixer(source), configuration(config) {
        if (config.max_processors == 0 || config.max_processors > dsp_max_buses ||
            config.max_resident_samples > 64U * 1024U * 1024U ||
            config.acoustic_ramp_frames > source.config().sample_rate * 60U) {
            throw std::invalid_argument("invalid DSP service capacity or smoothing duration");
        }
    }
    bool reject(std::string_view message) noexcept {
        error = message;
        increment(totals.rejected_requests);
        return false;
    }
    bool owns(BusId id) const noexcept {
        return !totals.stopped && id < slots.size() && slots[id].processor &&
            !slots[id].lost && mixer.bus_processor(id) == slots[id].processor;
    }
    bool feeds(BusId source, BusId destination) const noexcept {
        for (std::size_t depth = 0; depth < dsp_max_buses; ++depth) {
            if (source == destination) { return true; }
            const auto current = mixer.bus(source);
            if (!current || source == master_bus) { break; }
            source = current->parent;
        }
        return false;
    }
    bool retirement_available(const std::shared_ptr<DspProcessor>& processor) const noexcept {
        return !processor || processor.use_count() <= 2 ||
            std::any_of(retiring.begin(), retiring.end(), [](const auto& value) { return !value; });
    }
    bool retire(std::shared_ptr<DspProcessor>& processor) noexcept {
        if (!processor || processor.use_count() == 1) {
            processor.reset();
            return true;
        }
        for (auto& entry : retiring) {
            if (!entry) {
                entry = std::move(processor);
                return true;
            }
        }
        return false;
    }
};

DspService::DspService(AudioMixer& mixer, DspServiceConfig configuration)
    : impl_(std::make_unique<Impl>(mixer, configuration)) {}
DspService::~DspService() = default;

bool DspService::install(BusId id, const DspSettings& settings) {
    auto& state = *impl_;
    if (state.totals.stopped || id >= state.slots.size() || !state.mixer.bus(id)) {
        return state.reject("DSP service is stopped or bus does not exist");
    }
    collect();
    auto& slot = state.slots[id];
    if ((slot.processor && !state.owns(id)) ||
        (!slot.processor && state.mixer.bus_processor(id))) {
        return state.reject("DSP cannot overwrite another owner's processor");
    }
    const auto before = statistics();
    if ((!slot.processor && before.processors >= state.configuration.max_processors) ||
        !state.retirement_available(slot.processor)) {
        return state.reject("DSP processor or retirement capacity exhausted");
    }
    const auto& config = state.mixer.config();
    const DspFormat format{config.sample_rate, 2, config.max_block_frames};
    const auto remaining = state.configuration.max_resident_samples - before.resident_samples;
    auto replacement = DspProcessor::prepare(format, settings, remaining);
    if (!replacement) { return state.reject("DSP preparation failed: invalid settings or resident sample budget"); }
    if (!state.mixer.replace_bus_processor(id, slot.processor, replacement)) {
        return state.reject("DSP ownership changed before installation");
    }
    const bool following = slot.following;
    (void)state.retire(slot.processor);
    slot = {};
    slot.processor = std::move(replacement);
    slot.following = following;
    state.error = {};
    return true;
}

bool DspService::remove(BusId id) noexcept {
    auto& state = *impl_;
    if (!state.owns(id)) { return state.reject("DSP bus has no currently owned processor"); }
    auto& slot = state.slots[id];
    if (!state.retirement_available(slot.processor)) { return state.reject("DSP retirement capacity exhausted"); }
    if (!state.mixer.replace_bus_processor(id, slot.processor, {})) {
        return state.reject("DSP ownership changed before removal");
    }
    slot.lost = true;
    slot.following = false;
    if (state.retire(slot.processor)) { slot = {}; }
    state.error = {};
    return true;
}

bool DspService::set_control(BusId id, DspControl control, double value, std::uint32_t ramp_frames) noexcept {
    auto& state = *impl_;
    if (!state.owns(id)) { return state.reject("DSP bus has no currently owned processor"); }
    if (control == DspControl::AcousticHighFrequencyGain && state.slots[id].following) {
        return state.reject("Disable acoustic following before manually controlling HF gain");
    }
    if (!state.slots[id].processor->set_control(control, value, ramp_frames)) {
        return state.reject("DSP control value or ramp is outside its bounds");
    }
    state.error = {};
    return true;
}

bool DspService::schedule(BusId id, DspAutomation event) noexcept {
    auto& state = *impl_;
    if (!state.owns(id)) { return state.reject("DSP bus has no currently owned processor"); }
    if (event.control == DspControl::AcousticHighFrequencyGain && state.slots[id].following) {
        return state.reject("Disable acoustic following before scheduling HF gain");
    }
    if (!state.slots[id].processor->schedule(event)) { return state.reject("DSP automation is invalid or queue is full"); }
    state.error = {};
    return true;
}

bool DspService::reset(BusId id) noexcept {
    if (!impl_->owns(id)) { return impl_->reject("DSP bus has no currently owned processor"); }
    impl_->slots[id].processor->reset();
    impl_->error = {};
    return true;
}

bool DspService::follow_acoustics(BusId id, bool enabled) noexcept {
    auto& state = *impl_;
    if (!state.owns(id)) { return state.reject("DSP bus has no currently owned processor"); }
    if (enabled) {
        for (std::size_t index = 0; index < state.slots.size(); ++index) {
            const auto other = static_cast<BusId>(index);
            if (other != id && state.slots[index].following && state.owns(other) &&
                (state.feeds(other, id) || state.feeds(id, other))) {
                return state.reject("Nested DSP acoustic followers would filter the same signal twice");
            }
        }
    }
    auto& slot = state.slots[id];
    slot.following = enabled;
    if (enabled) { (void)slot.processor->cancel_automation(DspControl::AcousticHighFrequencyGain); }
    slot.acoustic_gain = 1.0;
    slot.contributors = 0;
    (void)slot.processor->set_control(DspControl::AcousticHighFrequencyGain, 1.0,
                                      enabled ? state.configuration.acoustic_ramp_frames : 0);
    state.error = {};
    return true;
}

void DspService::collect() noexcept {
    auto& state = *impl_;
    if (state.totals.stopped) { return; }
    for (auto& processor : state.retiring) {
        if (processor && processor.use_count() == 1) { processor.reset(); }
    }
    for (std::size_t index = 0; index < state.slots.size(); ++index) {
        auto& slot = state.slots[index];
        if (!slot.processor || state.mixer.bus_processor(static_cast<BusId>(index)) == slot.processor) { continue; }
        if (!slot.lost) { increment(state.totals.ownership_losses); }
        slot.lost = true;
        slot.following = false;
        if (state.retire(slot.processor)) { slot = {}; }
    }
}

void DspService::update(const SpatialAudio* spatial) noexcept {
    auto& state = *impl_;
    if (state.totals.stopped) { return; }
    collect();
    const auto count = spatial ? state.mixer.copy_voices(state.voices) : 0;
    for (std::size_t index = 0; index < state.slots.size(); ++index) {
        auto& slot = state.slots[index];
        if (!slot.processor || slot.lost || !slot.following) { continue; }
        const auto id = static_cast<BusId>(index);
        bool conflict = false;
        for (std::size_t other = 0; other < state.slots.size(); ++other) {
            if (other != index && state.slots[other].processor && !state.slots[other].lost &&
                state.slots[other].following && state.feeds(static_cast<BusId>(other), id)) { conflict = true; }
        }
        slot.acoustic_gain = 1.0;
        slot.contributors = 0;
        if (conflict) { increment(state.totals.acoustic_conflicts); }
        for (std::size_t voice_index = 0; !conflict && voice_index < count; ++voice_index) {
            const auto& voice = state.voices[voice_index];
            if (voice.state != VoiceState::Playing || std::max(voice.gain, voice.target_gain) <= 0.000001F ||
                !state.feeds(voice.bus, id)) { continue; }
            const auto acoustic = spatial->voice(voice.handle);
            if (acoustic && between(acoustic->high_frequency_gain, 0.0, 1.0)) {
                slot.acoustic_gain = std::min(slot.acoustic_gain, acoustic->high_frequency_gain);
                ++slot.contributors;
            }
        }
        (void)slot.processor->set_control(DspControl::AcousticHighFrequencyGain, slot.acoustic_gain,
                                          conflict ? 0 : state.configuration.acoustic_ramp_frames);
    }
}

void DspService::shutdown() noexcept {
    auto& state = *impl_;
    if (state.totals.stopped) { return; }
    for (std::size_t index = 0; index < state.slots.size(); ++index) {
        auto& slot = state.slots[index];
        if (slot.processor) {
            if (!slot.lost) {
                (void)state.mixer.replace_bus_processor(static_cast<BusId>(index), slot.processor, {});
            }
            slot = {};
        }
    }
    for (auto& processor : state.retiring) { processor.reset(); }
    state.totals.stopped = true;
}

std::optional<DspBusState> DspService::bus(BusId id) const noexcept {
    if (!impl_->owns(id)) { return std::nullopt; }
    const auto& slot = impl_->slots[id];
    return DspBusState{id, slot.processor->statistics(), slot.acoustic_gain, slot.contributors, slot.following};
}

std::size_t DspService::copy_buses(std::span<DspBusState> output) const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < impl_->slots.size() && count < output.size(); ++index) {
        if (const auto value = bus(static_cast<BusId>(index))) { output[count++] = *value; }
    }
    return count;
}

DspServiceStatistics DspService::statistics() const noexcept {
    auto result = impl_->totals;
    for (const auto& slot : impl_->slots) {
        if (!slot.processor) { continue; }
        result.resident_samples += slot.processor->statistics().resident_samples;
        if (slot.lost) { ++result.retiring_processors; } else { ++result.processors; }
    }
    for (const auto& processor : impl_->retiring) {
        if (!processor) { continue; }
        ++result.retiring_processors;
        result.resident_samples += processor->statistics().resident_samples;
    }
    return result;
}

std::string_view DspService::last_error() const noexcept { return impl_->error; }

namespace {

template<class Number>
bool parse_number(std::string_view text, Number& result) noexcept {
    if (text.empty()) { return false; }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::optional<DspPreset> parse_preset(std::string_view text) noexcept {
    constexpr std::array<std::string_view, 6> names{"neutral", "room", "hall", "echo", "radio", "speech"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == text) { return static_cast<DspPreset>(index); }
    }
    return std::nullopt;
}

std::optional<DspControl> parse_control(std::string_view text) noexcept {
    constexpr std::array<std::string_view, control_count> names{
        "input_db", "output_db", "wet", "delay_wet", "feedback", "reverb_wet", "threshold_db", "hf", "bypass"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == text) { return static_cast<DspControl>(index); }
    }
    return std::nullopt;
}

core::CommandResult dsp_command(core::Kernel& host, core::CommandArgs args) {
    auto& service = host.services().get<DspService>();
    auto& mixer = host.services().get<AudioMixer>();
    if (args.empty() || (args.size() == 1 && args[0] == "status")) {
        const auto stats = service.statistics();
        return {true, "processors=" + std::to_string(stats.processors) +
            " retiring=" + std::to_string(stats.retiring_processors) +
            " samples=" + std::to_string(stats.resident_samples) +
            " rejected=" + std::to_string(stats.rejected_requests)};
    }
    if (args.size() < 2) { return {false, "usage: audio.dsp action bus [parameters]"}; }
    auto bus = mixer.find_bus(args[1]);
    if (!bus) {
        BusId numeric{};
        if (parse_number(args[1], numeric) && mixer.bus(numeric)) { bus = numeric; }
    }
    if (!bus) { return {false, "unknown audio bus name or id"}; }
    const auto reply = [&service](bool success) -> core::CommandResult {
        return {success, success ? "DSP bus updated" : std::string(service.last_error())};
    };
    if (args[0] == "install" && args.size() == 3) {
        const auto preset = parse_preset(args[2]);
        if (!preset) { return {false, "presets: neutral room hall echo radio speech"}; }
        return reply(service.install(*bus, dsp_preset(*preset, mixer.config().sample_rate)));
    }
    if (args[0] == "remove" && args.size() == 2) { return reply(service.remove(*bus)); }
    if (args[0] == "reset" && args.size() == 2) { return reply(service.reset(*bus)); }
    if (args[0] == "acoustic" && args.size() == 3) {
        if (args[2] != "on" && args[2] != "off") { return {false, "acoustic expects on or off"}; }
        return reply(service.follow_acoustics(*bus, args[2] == "on"));
    }
    if (args[0] == "status" && args.size() == 2) {
        const auto value = service.bus(*bus);
        if (!value) { return {false, "bus has no owned DSP processor"}; }
        return {true, "next_frame=" + std::to_string(value->processing.next_output_frame) +
            " peak=" + std::to_string(value->processing.output_peak) +
            " rms=" + std::to_string(value->processing.output_rms) +
            " reduction_db=" + std::to_string(value->processing.gain_reduction_db) +
            " queued=" + std::to_string(value->processing.queued_controls) +
            " acoustic_hf=" + std::to_string(value->acoustic_gain)};
    }
    const bool scheduled = args[0] == "schedule";
    const std::size_t required = scheduled ? 5 : 4;
    if ((!scheduled && args[0] != "control") || args.size() < required || args.size() > required + 1) {
        return {false, "actions: install bus preset; remove|reset|status bus; acoustic bus on|off; "
            "control bus parameter value [ramp_ms]; schedule bus parameter value output_frame [ramp_ms]"};
    }
    const auto parameter = parse_control(args[2]);
    double value{};
    double ramp_ms{};
    if (!parameter || !parse_number(args[3], value) ||
        (args.size() > required && !parse_number(args[required], ramp_ms)) || !between(ramp_ms, 0.0, 60000.0)) {
        return {false, "parameters: input_db output_db wet delay_wet feedback reverb_wet threshold_db hf bypass; ramp 0-60000 ms"};
    }
    const auto ramp = static_cast<std::uint32_t>(std::round(ramp_ms * mixer.config().sample_rate / 1000.0));
    if (!scheduled) { return reply(service.set_control(*bus, *parameter, value, ramp)); }
    std::uint64_t frame{};
    if (!parse_number(args[4], frame)) { return {false, "output_frame must be an unsigned sample-frame index"}; }
    return reply(service.schedule(*bus, {*parameter, value, frame, ramp}));
}

} // namespace

void register_dsp(core::Kernel& kernel) {
    auto* mixer = kernel.services().find<AudioMixer>();
    if (!mixer) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "audio.dsp", "AudioMixer must be registered before DSP");
        return;
    }
    if (!kernel.register_module({"audio.dsp", "Prepared bus EQ, delay, reverb, dynamics and automation", 1})) { return; }
    DspServiceConfig configuration;
    configuration.max_processors = static_cast<std::size_t>(std::clamp(
        kernel.config().integer("audio.dsp.processors", 16), std::int64_t{1}, std::int64_t{32}));
    configuration.max_resident_samples = static_cast<std::size_t>(std::clamp(
        kernel.config().integer("audio.dsp.samples", 8 * 1024 * 1024), std::int64_t{0}, std::int64_t{64 * 1024 * 1024}));
    double smoothing = kernel.config().number("audio.dsp.acoustic_smoothing", 0.02);
    if (!between(smoothing, 0.0, 1.0)) { smoothing = 0.02; }
    configuration.acoustic_ramp_frames = static_cast<std::uint32_t>(std::round(smoothing * mixer->config().sample_rate));
    auto& service = kernel.services().emplace<DspService>(*mixer, configuration);
    (void)kernel.add_shutdown("audio.dsp", [owner = &service] { owner->shutdown(); });
    (void)kernel.add_system({"audio.dsp", core::Phase::Audio, 50, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& owner = host.services().get<DspService>();
            owner.update(host.services().find<SpatialAudio>());
            const auto stats = owner.statistics();
            host.metric("audio.dsp.processors", static_cast<double>(stats.processors));
            host.metric("audio.dsp.samples", static_cast<double>(stats.resident_samples));
            host.metric("audio.dsp.retiring", static_cast<double>(stats.retiring_processors));
            host.metric("audio.dsp.rejected", static_cast<double>(stats.rejected_requests));
        });
    (void)kernel.order_after("audio.dsp", "audio.spatial");
    (void)kernel.order_after("audio.mixer", "audio.dsp");
    (void)kernel.add_command("audio.dsp", dsp_command, "Configure owned bus effects; audio.dsp with no arguments reports capacity");
    if (kernel.config().boolean("audio.dsp.master", false)) {
        const auto preset = parse_preset(kernel.config().text("audio.dsp.preset", "neutral"));
        if (!preset || !service.install(master_bus, dsp_preset(*preset, mixer->config().sample_rate))) {
            kernel.diagnose(core::Diagnostic::Severity::Warning, "audio.dsp", "Requested master DSP preset could not be installed");
        }
    }
}

} // namespace ge::audio
