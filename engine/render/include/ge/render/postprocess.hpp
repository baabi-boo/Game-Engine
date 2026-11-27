#pragma once

#include <ge/render/graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace ge::core { class Kernel; }

namespace ge::render {

inline constexpr std::string_view graph_postprocess_pass = "image.postprocess";
inline constexpr std::uint64_t graph_postprocess_callback_owner = 0x4745504f53543031ULL;
inline constexpr std::size_t postprocess_histogram_bins = 256;

enum class ToneCurve : std::uint8_t {
    Linear,
    Reinhard,
    WhiteReinhard,
    Exponential,
    SoftShoulder
};

enum class ExposureMeter : std::uint8_t { LogAverage, Histogram };

enum class PostprocessError : std::uint8_t {
    None,
    InvalidLimits,
    InvalidSettings,
    InvalidImage,
    InvalidPixel,
    InvalidTime,
    PixelLimit,
    ScratchLimit,
    SampleLimit,
    AllocationFailure,
    MissingGraph,
    GraphConflict,
    GraphRejected,
    Stopped
};

[[nodiscard]] std::string_view describe(ToneCurve curve) noexcept;
[[nodiscard]] std::string_view describe(PostprocessError error) noexcept;

struct PostprocessLimits {
    std::uint32_t max_dimension{16384};
    std::size_t max_pixels{4U * 1024U * 1024U};
    std::size_t max_scratch_bytes{256U * 1024U * 1024U};
    std::size_t max_sample_visits{256U * 1024U * 1024U};
    std::uint32_t max_blur_radius{64};
    std::uint32_t max_blur_iterations{4};
};

struct ExposureSettings {
    bool automatic{};
    bool alpha_weighted{true};
    ExposureMeter meter{ExposureMeter::Histogram};
    double manual_ev{};
    double compensation_ev{};
    double minimum_ev{-16.0};
    double maximum_ev{16.0};
    double middle_gray{0.18};
    double minimum_log_luminance{-16.0};
    double maximum_log_luminance{16.0};
    double lower_percentile{0.02};
    double upper_percentile{0.98};
    double center_weight{};
    double brighten_speed{2.0};
    double darken_speed{1.0};
    double maximum_delta_seconds{0.25};
};

struct ToneSettings {
    ToneCurve curve{ToneCurve::Reinhard};
    bool preserve_chromaticity{true};
    double white_point{4.0};
    double shoulder_start{0.5};
};

struct BloomSettings {
    bool enabled{};
    std::uint32_t downsample{4};
    std::uint32_t radius{5};
    std::uint32_t iterations{1};
    double sigma{2.0};
    double threshold{1.0};
    double soft_knee{0.5};
    double strength{0.15};
};

struct ColorAdjustments {
    // Gain/lift use RGB only; alpha remains the incoming image alpha.
    Color gain{1.0, 1.0, 1.0, 1.0};
    Color lift{0.0, 0.0, 0.0, 1.0};
    double contrast{1.0};
    double contrast_pivot{0.18};
    double saturation{1.0};
    double vibrance{};
    double temperature{};
    double tint{};
    double hue_radians{};
};

struct DetailSettings {
    std::uint32_t radius{1};
    double sigma{1.0};
    double blur_mix{};
    double sharpen{};
    double sharpen_threshold{0.01};
    double edge_strength{};
    double edge_threshold{0.1};
};

struct VignetteSettings {
    double strength{};
    double inner_radius{0.3};
    double outer_radius{1.0};
    double roundness{1.0};
    double center_x{0.5};
    double center_y{0.5};
};

struct PostprocessSettings {
    bool enabled{true};
    ExposureSettings exposure{};
    ToneSettings tone{};
    BloomSettings bloom{};
    ColorAdjustments color{};
    DetailSettings detail{};
    VignetteSettings vignette{};
    double output_ceiling{65504.0};
};

struct LuminanceHistogram {
    std::array<double, postprocess_histogram_bins> weights{};
    double total_weight{};
    double minimum_luminance{};
    double maximum_luminance{};
    double arithmetic_mean{};
    double geometric_mean{};
    double trimmed_log_mean{};
    double minimum_log_luminance{-16.0};
    double maximum_log_luminance{16.0};
    std::size_t contributing_pixels{};
    std::size_t low_clipped_pixels{};
    std::size_t high_clipped_pixels{};
};

struct PostprocessPlan {
    PostprocessError error{PostprocessError::None};
    std::size_t pixels{};
    std::size_t scratch_bytes{};
    // Conservative pixel/sample work admission, including publication sweeps.
    std::size_t sample_visits{};
    std::uint32_t bloom_width{};
    std::uint32_t bloom_height{};
    std::size_t bloom_pixels{};
    [[nodiscard]] explicit operator bool() const noexcept;
};

struct PostprocessStatistics {
    std::uint64_t attempts{};
    std::uint64_t completed{};
    std::uint64_t rejected{};
    std::size_t pixels{};
    std::size_t scratch_bytes{};
    std::size_t sample_visits{};
    std::size_t input_clipped_pixels{};
    std::size_t output_clipped_pixels{};
    double exposure_ev{};
    double target_exposure_ev{};
    double exposure_multiplier{1.0};
    bool exposure_initialized{};
    bool image_changed{};
};

struct PostprocessResult {
    PostprocessError error{PostprocessError::None};
    bool image_changed{};
    [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] bool valid(const PostprocessLimits& limits) noexcept;
[[nodiscard]] bool valid(const PostprocessSettings& settings,
                         const PostprocessLimits& limits = {}) noexcept;
// Percentiles interpolate within log-luminance bins; these are estimates.
[[nodiscard]] double histogram_percentile(const LuminanceHistogram& histogram,
                                           double percentile) noexcept;
[[nodiscard]] double tone_map_value(double value, const ToneSettings& settings) noexcept;

// Serial CPU service. Input/output are linear RGB with straight alpha and a
// top-left origin. Input RGB must be finite with magnitude <= 1e12 and alpha in
// [0,1]. Negative RGB is clipped for processing. No display transfer is applied.
// Scratch payload and sample visits are preflighted. Rejection preserves pixels
// and exposure history; success copies pixels without replacing Image storage.
// Bloom and detail filtering use premultiplied samples and preserve input alpha.
// Destruction never dereferences borrowed services. Graph callbacks hold weak
// state and become harmless after service destruction or stop().
class PostprocessService final {
public:
    explicit PostprocessService(PostprocessLimits limits = {});
    ~PostprocessService();
    PostprocessService(const PostprocessService&) = delete;
    PostprocessService& operator=(const PostprocessService&) = delete;
    PostprocessService(PostprocessService&&) = delete;
    PostprocessService& operator=(PostprocessService&&) = delete;
    [[nodiscard]] const PostprocessLimits& limits() const noexcept;
    [[nodiscard]] const PostprocessSettings& settings() const noexcept;
    [[nodiscard]] const PostprocessStatistics& statistics() const noexcept;
    [[nodiscard]] const LuminanceHistogram& histogram() const noexcept;
    [[nodiscard]] PostprocessError last_error() const noexcept;
    [[nodiscard]] PostprocessError attachment_error() const noexcept;
    bool configure(const PostprocessSettings& settings) noexcept;
    void reset_exposure() noexcept;
    [[nodiscard]] PostprocessPlan plan(std::uint32_t width, std::uint32_t height) const noexcept;
    [[nodiscard]] PostprocessResult measure(const Image& image) noexcept;
    [[nodiscard]] PostprocessResult process(Image& image, double delta_seconds) noexcept;
    // Requires standard main.color, scene.raster, and frame.complete anchors.
    // Reattaches after reset; never replaces a same-named pass owned by others.
    [[nodiscard]] PostprocessError attach(RenderGraph& graph);
    [[nodiscard]] bool attached(const RenderGraph& graph) const noexcept;
    bool detach(RenderGraph& graph);
    // Invalidates weak callback work without touching a possibly dead graph.
    void stop() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

void register_postprocess(core::Kernel& kernel);

} // namespace ge::render
