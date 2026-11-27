#pragma once

#include "ge/math/math.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core {
class Kernel;
}

namespace ge::audio {

inline constexpr std::uint32_t invalid_audio_slot = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t infinite_repeats = std::numeric_limits<std::uint32_t>::max();
using BusId = std::uint32_t;
inline constexpr BusId master_bus = 0;

// Interleaved, normalized float PCM. Publication copies/moves this value into
// immutable owned storage; no source span or caller-owned sample memory survives.
struct AudioBuffer {
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{1};
    std::vector<float> samples;

    [[nodiscard]] std::size_t frame_count() const noexcept;
    [[nodiscard]] double duration_seconds() const noexcept;
    [[nodiscard]] bool validate(std::string* reason = nullptr) const;
};

enum class Waveform : std::uint8_t { Sine, Triangle, Saw, Square, Noise };

struct ToneSpec {
    Waveform waveform{Waveform::Sine};
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{1};
    double frequency_hz{440.0};
    double duration_seconds{0.25};
    double attack_seconds{0.005};
    double release_seconds{0.01};
    float amplitude{0.25F};
    std::uint32_t noise_seed{1};
};

[[nodiscard]] std::optional<AudioBuffer> generate_tone(const ToneSpec& specification,
                                                      std::string* reason = nullptr);

// Handles are local to their creating library or mixer instance.
struct SoundHandle {
    std::uint32_t slot{invalid_audio_slot};
    std::uint32_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept;
    friend bool operator==(SoundHandle, SoundHandle) = default;
};

struct SoundLibraryConfig {
    std::size_t max_sounds{256};
    std::size_t max_resident_samples{32U * 1024U * 1024U};
};

struct SoundLibraryStatistics {
    std::size_t live_sounds{};
    std::size_t retiring_sounds{};
    std::size_t resident_samples{}; // Allocated float capacity, including slack.
};

// Removing a sound invalidates its handle immediately. Its slot and sample
// budget remain charged until all acquired owners (including voices) release it.
class SoundLibrary final {
public:
    explicit SoundLibrary(SoundLibraryConfig config = {});
    ~SoundLibrary();
    SoundLibrary(const SoundLibrary&) = delete;
    SoundLibrary& operator=(const SoundLibrary&) = delete;
    [[nodiscard]] SoundHandle create(std::string name, AudioBuffer buffer);
    [[nodiscard]] SoundHandle find(std::string_view name) const noexcept;
    [[nodiscard]] std::shared_ptr<const AudioBuffer> acquire(SoundHandle handle) const noexcept;
    [[nodiscard]] bool contains(SoundHandle handle) const noexcept;
    bool remove(SoundHandle handle) noexcept;
    void clear() noexcept;
    void collect() noexcept;
    [[nodiscard]] SoundLibraryStatistics statistics() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct VoiceHandle {
    std::uint32_t slot{invalid_audio_slot};
    std::uint32_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept;
    friend bool operator==(VoiceHandle, VoiceHandle) = default;
};

// The half-open source-frame region may have an intro and an outro. repeat_count
// counts additional traversals; infinite_repeats is the explicit endless value.
struct LoopRegion {
    std::uint64_t begin_frame{};
    std::uint64_t end_frame{};
    std::uint32_t repeat_count{infinite_repeats};
};

enum class VoiceStealing : std::uint8_t { Reject, Oldest, Quietest, LowestPriority };
enum class VoiceState : std::uint8_t { Playing, Paused };

struct VoiceOptions {
    BusId bus{master_bus};
    float gain{1.0F};
    float pan{};
    float pitch{1.0F};
    double start_frame{};
    std::optional<LoopRegion> loop;
    std::uint32_t fade_in_frames{}; // Output-rate frames, as are all gain ramps.
    std::uint16_t priority{};
    std::uint64_t owner{};
    bool protect_from_stealing{};
    ge::math::Vec3 position{};
};

struct VoiceInfo {
    VoiceHandle handle;
    SoundHandle sound;
    VoiceState state{VoiceState::Playing};
    BusId bus{master_bus};
    double source_frame{};
    double source_seconds{};
    float gain{};
    float target_gain{};
    float pan{};
    float pitch{};
    float last_peak{}; // Pre-bus peak used by Quietest voice stealing.
    std::uint32_t remaining_repeats{};
    std::uint16_t priority{};
    std::uint64_t owner{};
    bool stopping{};
    bool protect_from_stealing{};
    ge::math::Vec3 position{};
};

struct AudioProcessContext {
    BusId bus{master_bus};
    std::uint32_t sample_rate{};
    std::uint32_t channels{2};
    std::uint64_t first_output_frame{};
};

// Called in deterministic child-before-parent order, before this bus's gain.
// Implementations must be bounded, noexcept, must not retain the span, mutate
// the mixer, allocate, lock, or call an audio device from process().
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void process(const AudioProcessContext& context,
                         std::span<float> interleaved_samples) noexcept = 0;
};

struct BusInfo {
    BusId id{master_bus};
    BusId parent{master_bus};
    std::string_view name;
    float gain{};
    float target_gain{};
    float peak{};
    bool mute{};
    bool solo{};
    bool has_processor{};
};

struct LimiterSettings {
    bool enabled{true};
    float ceiling{0.98F};
    double release_seconds{0.05};
};

struct MixerConfig {
    std::uint32_t sample_rate{48000};
    std::size_t max_voices{128};
    std::size_t max_buses{16};
    std::size_t max_block_frames{1024};
    std::size_t max_blocks_per_tick{16};
    VoiceStealing stealing{VoiceStealing::LowestPriority};
    SoundLibraryConfig sounds;
    LimiterSettings limiter;
};

struct MixerStatistics {
    std::uint64_t requested_frames{};
    std::uint64_t rendered_frames{};
    std::uint64_t dropped_frames{};
    std::uint64_t finished_voices{};
    std::uint64_t stolen_voices{};
    std::uint64_t rejected_voices{};
    std::uint64_t clipped_samples{};
    std::uint64_t over_range_samples{};
    std::uint64_t sanitized_samples{};
    std::uint64_t limited_frames{};
    std::size_t active_voices{};
    std::size_t paused_voices{};
    std::size_t last_block_frames{}; // Final bounded DSP block, before concatenation.
    float pre_limit_peak{};
    float output_peak{};
    double output_rms{};
    float limiter_gain{1.0F};
};

// Serialized caller-thread service, with no internal locks or device backend.
// A full generation token authorizes control; owner values only group voices.
// mix()/advance() use preallocated storage. Configuration/control may allocate.
class AudioMixer final {
public:
    explicit AudioMixer(MixerConfig config = {});
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    [[nodiscard]] SoundLibrary& sounds() noexcept;
    [[nodiscard]] const SoundLibrary& sounds() const noexcept;
    [[nodiscard]] const MixerConfig& config() const noexcept;
    [[nodiscard]] VoiceHandle play(SoundHandle sound, const VoiceOptions& options = {});
    bool stop(VoiceHandle voice, std::uint32_t fade_frames = 0) noexcept;
    std::size_t stop_owner(std::uint64_t owner, std::uint32_t fade_frames = 0) noexcept;
    void stop_all() noexcept;
    bool pause(VoiceHandle voice, bool paused = true) noexcept;
    bool seek_frames(VoiceHandle voice, double source_frame) noexcept;
    bool seek_seconds(VoiceHandle voice, double seconds) noexcept;
    bool set_gain(VoiceHandle voice, float gain, std::uint32_t ramp_frames = 0) noexcept;
    bool set_pan(VoiceHandle voice, float pan) noexcept;
    bool set_pitch(VoiceHandle voice, float pitch) noexcept;
    bool set_position(VoiceHandle voice, ge::math::Vec3 position) noexcept;
    bool set_loop(VoiceHandle voice, std::optional<LoopRegion> loop) noexcept;
    bool route(VoiceHandle voice, BusId bus) noexcept;
    [[nodiscard]] std::optional<VoiceInfo> voice(VoiceHandle handle) const noexcept;
    [[nodiscard]] std::size_t copy_voices(std::span<VoiceInfo> destination) const noexcept;
    [[nodiscard]] std::optional<BusId> create_bus(std::string name, BusId parent = master_bus);
    [[nodiscard]] std::optional<BusId> find_bus(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<BusInfo> bus(BusId id) const noexcept;
    bool set_bus_parent(BusId bus, BusId parent) noexcept;
    bool set_bus_gain(BusId bus, float gain, std::uint32_t ramp_frames = 0) noexcept;
    bool set_bus_mute(BusId bus, bool mute) noexcept;
    bool set_bus_solo(BusId bus, bool solo) noexcept;
    bool set_bus_processor(BusId bus, std::shared_ptr<AudioProcessor> processor) noexcept;
    // Serialized control-plane access only; never call from process(). An empty
    // result means no processor or an invalid bus. Use bus() to distinguish.
    [[nodiscard]] std::shared_ptr<AudioProcessor> bus_processor(BusId bus) const noexcept;
    // Cooperative ownership: replace only the exact currently installed object.
    // An empty expected value claims an unoccupied slot. Failure preserves it.
    bool replace_bus_processor(BusId bus, const std::shared_ptr<AudioProcessor>& expected,
                               std::shared_ptr<AudioProcessor> replacement) noexcept;
    bool set_limiter(LimiterSettings settings) noexcept;
    [[nodiscard]] std::span<const float> mix(std::size_t frames) noexcept;
    // Drops excess time demand above the configured block budget and counts it.
    // Only rendered frames advance voice cursors. output() retains all admitted
    // frames from this advance; mix() retains one requested block instead.
    bool advance(double delta_seconds) noexcept;
    // Borrowed stereo interleaved view, replaced by the next successful mix or
    // advance. Invalid production requests preserve the previous retained data.
    [[nodiscard]] std::span<const float> output() const noexcept;
    [[nodiscard]] const MixerStatistics& statistics() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;
    void reset_statistics() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_mixer(ge::core::Kernel& kernel);

} // namespace ge::audio
