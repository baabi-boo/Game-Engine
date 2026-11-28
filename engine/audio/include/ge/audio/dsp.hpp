#pragma once

#include <ge/audio/mixer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ge::core { class Kernel; }

namespace ge::audio {

class SpatialAudio;

inline constexpr std::size_t dsp_max_bands = 8;
inline constexpr std::size_t dsp_max_automation = 128;
inline constexpr std::size_t dsp_max_buses = 32;

enum class DspFilterKind : std::uint8_t {
    LowPass,
    HighPass,
    BandPass,
    Notch,
    LowShelf,
    HighShelf,
    Peak,
    AllPass
};

struct DspFilter {
    DspFilterKind kind{DspFilterKind::Peak};
    double frequency_hz{1000.0};
    double quality{0.7071067811865476};
    double gain_db{};
};

struct BiquadCoefficients {
    double b0{1.0};
    double b1{};
    double b2{};
    double a1{};
    double a2{};
    [[nodiscard]] bool stable() const noexcept;
    [[nodiscard]] double magnitude(double frequency_hz, std::uint32_t sample_rate) const noexcept;
};

[[nodiscard]] std::optional<BiquadCoefficients> design_biquad(
    const DspFilter& filter, std::uint32_t sample_rate) noexcept;

struct DspDelaySettings {
    bool enabled{};
    double maximum_seconds{1.0};
    double left_seconds{0.25};
    double right_seconds{0.375};
    double feedback{0.3};
    double crossfeed{0.35};
    double damping_hz{6000.0};
    double wet{0.2};
};

struct DspReverbSettings {
    bool enabled{};
    double room_scale{1.0};
    double decay_seconds{1.4};
    double predelay_seconds{0.015};
    double damping_hz{5000.0};
    double stereo_width{0.85};
    double wet{0.18};
};

struct DspCompressorSettings {
    bool enabled{};
    double threshold_db{-18.0};
    double ratio{4.0};
    double knee_db{6.0};
    double attack_seconds{0.01};
    double release_seconds{0.15};
    double makeup_db{};
};

struct DspSettings {
    std::array<DspFilter, dsp_max_bands> equalizer{};
    std::size_t band_count{};
    DspDelaySettings delay{};
    DspReverbSettings reverb{};
    DspCompressorSettings compressor{};
    double input_gain_db{};
    double output_gain_db{};
    double wet{1.0};
    double acoustic_crossover_hz{1200.0};
};

struct DspFormat {
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    std::size_t max_block_frames{1024};
};

enum class DspPreset : std::uint8_t { Neutral, Room, Hall, Echo, Radio, Speech };

[[nodiscard]] DspSettings dsp_preset(DspPreset preset, std::uint32_t sample_rate = 48000) noexcept;
[[nodiscard]] bool validate_dsp(const DspFormat& format, const DspSettings& settings,
                              std::string* reason = nullptr);

enum class DspControl : std::uint8_t {
    InputGainDb,
    OutputGainDb,
    Wet,
    DelayWet,
    DelayFeedback,
    ReverbWet,
    CompressorThresholdDb,
    AcousticHighFrequencyGain,
    Bypass,
    Count
};

struct DspAutomation {
    DspControl control{DspControl::Wet};
    double value{1.0};
    std::uint64_t output_frame{};
    std::uint32_t ramp_frames{};
};

struct DspStatistics {
    std::uint64_t processed_blocks{};
    std::uint64_t processed_frames{};
    std::uint64_t rejected_blocks{};
    std::uint64_t sanitized_samples{};
    std::uint64_t bounded_samples{};
    std::uint64_t applied_controls{};
    std::uint64_t late_controls{};
    std::uint64_t discontinuities{};
    std::uint64_t next_output_frame{};
    std::size_t queued_controls{};
    std::size_t resident_samples{};
    double input_peak{};
    double output_peak{};
    double output_rms{};
    double gain_reduction_db{};
};

// All control and inspection require the mixer's serialized caller thread.
// Prepare owns every sample buffer before publication. process() is bounded by
// the prepared format, never allocates/locks, and never calls another service.
// Changes to filter topology or delay/reverb resources require a new processor.
class DspProcessor final : public AudioProcessor {
public:
    [[nodiscard]] static std::shared_ptr<DspProcessor> prepare(
        DspFormat format, const DspSettings& settings,
        std::size_t sample_budget = 4U * 1024U * 1024U, std::string* reason = nullptr);
    ~DspProcessor() override;
    DspProcessor(const DspProcessor&) = delete;
    DspProcessor& operator=(const DspProcessor&) = delete;
    void process(const AudioProcessContext& context, std::span<float> samples) noexcept override;
    bool set_control(DspControl control, double value, std::uint32_t ramp_frames = 0) noexcept;
    // Equal-time events retain insertion order. Overdue events execute on the
    // next processed sample; they are counted, never silently discarded.
    bool schedule(DspAutomation event) noexcept;
    void cancel_automation() noexcept;
    std::size_t cancel_automation(DspControl control) noexcept;
    // Control-plane operation; clears tails, meters and queued controls while
    // preserving the current control targets and the observed output clock.
    void reset() noexcept;
    [[nodiscard]] std::optional<double> target(DspControl control) const noexcept;
    [[nodiscard]] const DspSettings& settings() const noexcept;
    [[nodiscard]] const DspFormat& format() const noexcept;
    [[nodiscard]] DspStatistics statistics() const noexcept;

private:
    struct Impl;
    explicit DspProcessor(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> impl_;
};

struct DspServiceConfig {
    std::size_t max_processors{16};
    std::size_t max_resident_samples{8U * 1024U * 1024U};
    std::uint32_t acoustic_ramp_frames{960};
};

struct DspBusState {
    BusId bus{master_bus};
    DspStatistics processing{};
    double acoustic_gain{1.0};
    std::size_t spatial_contributors{};
    bool follows_acoustics{};
};

struct DspServiceStatistics {
    std::size_t processors{};
    std::size_t retiring_processors{};
    std::size_t resident_samples{};
    std::uint64_t rejected_requests{};
    std::uint64_t ownership_losses{};
    std::uint64_t acoustic_conflicts{};
    bool stopped{};
};

// Owns only processor slots it successfully claims. Reconfiguration compares the
// exact installed object and resets its signal history. External processor owners
// are retained/charged until released; preparation charges old plus new storage.
// The destructor never touches the borrowed mixer. Call shutdown() while it lives.
class DspService final {
public:
    explicit DspService(AudioMixer& mixer, DspServiceConfig configuration = {});
    ~DspService();
    DspService(const DspService&) = delete;
    DspService& operator=(const DspService&) = delete;
    bool install(BusId bus, const DspSettings& settings);
    bool remove(BusId bus) noexcept;
    bool set_control(BusId bus, DspControl control, double value,
                     std::uint32_t ramp_frames = 0) noexcept;
    bool schedule(BusId bus, DspAutomation event) noexcept;
    bool reset(BusId bus) noexcept;
    // Explicit bus-wide approximation: minimum HF gain among playing spatial
    // voices routed here or through descendants. Other voices share the filter.
    // Enabling cancels pending HF automation. Nested followers are rejected;
    // changed bus ancestry suppresses the broader follower so the same signal
    // is not acoustically filtered twice. Disabling restores unity immediately.
    bool follow_acoustics(BusId bus, bool enabled) noexcept;
    void update(const SpatialAudio* spatial = nullptr) noexcept;
    void collect() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] std::optional<DspBusState> bus(BusId bus) const noexcept;
    [[nodiscard]] std::size_t copy_buses(std::span<DspBusState> output) const noexcept;
    [[nodiscard]] DspServiceStatistics statistics() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_dsp(ge::core::Kernel& kernel);

} // namespace ge::audio
