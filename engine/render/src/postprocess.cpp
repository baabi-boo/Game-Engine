#include <ge/render/postprocess.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace ge::render {
namespace {

constexpr double maximum_input = 1.0e12;
constexpr double coverage_epsilon = 1.0e-12;
constexpr std::uint32_t hard_radius = 64;
using KernelWeights = std::array<double, 2U * hard_radius + 1U>;

bool within(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool add_product(std::size_t& total, std::size_t count, std::size_t size,
                 std::size_t limit) noexcept {
    if (total > limit || (size != 0 && count > (limit - total) / size)) {
        return false;
    }
    total += count * size;
    return true;
}

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

double positive_luminance(Color value) noexcept {
    return std::max(value.r, 0.0) * 0.2126 + std::max(value.g, 0.0) * 0.7152 +
           std::max(value.b, 0.0) * 0.0722;
}

Color scale_rgb(Color value, double scale) noexcept {
    return {value.r * scale, value.g * scale, value.b * scale, value.a};
}

bool valid_tone(const ToneSettings& settings) noexcept {
    switch (settings.curve) {
    case ToneCurve::Linear:
    case ToneCurve::Reinhard:
    case ToneCurve::WhiteReinhard:
    case ToneCurve::Exponential:
    case ToneCurve::SoftShoulder:
        break;
    default:
        return false;
    }
    return within(settings.white_point, 0.01, 65504.0) &&
           within(settings.shoulder_start, 0.0, 0.95);
}

double curve_value(double value, const ToneSettings& settings) noexcept {
    switch (settings.curve) {
    case ToneCurve::Linear:
        return value;
    case ToneCurve::Reinhard:
        return value / (1.0 + value);
    case ToneCurve::WhiteReinhard:
        return (value / (1.0 + value)) *
               (1.0 + value / (settings.white_point * settings.white_point));
    case ToneCurve::Exponential:
        return -std::expm1(-value);
    case ToneCurve::SoftShoulder: {
        const double knee = settings.shoulder_start;
        if (value <= knee) {
            return value;
        }
        return knee + (1.0 - knee) * -std::expm1(-(value - knee) / (1.0 - knee));
    }
    }
    return value;
}

bool detail_blur(const DetailSettings& settings) noexcept {
    return settings.blur_mix > 0.0 || settings.sharpen > 0.0;
}

bool detail_buffer(const DetailSettings& settings) noexcept {
    return detail_blur(settings) || settings.edge_strength > 0.0;
}

bool bloom_active(const BloomSettings& settings) noexcept {
    return settings.enabled && settings.strength > 0.0;
}

PostprocessPlan make_plan(std::uint32_t width, std::uint32_t height,
                          const PostprocessSettings& settings,
                          const PostprocessLimits& limits) noexcept {
    PostprocessPlan result;
    if (width == 0 || height == 0 || width > limits.max_dimension ||
        height > limits.max_dimension) {
        result.error = PostprocessError::InvalidImage;
        return result;
    }
    if (!add_product(result.pixels, width, height, limits.max_pixels)) {
        result.error = PostprocessError::PixelLimit;
        return result;
    }
    if (!settings.enabled) {
        return result;
    }
    const std::size_t full_buffers = detail_buffer(settings.detail) ? 2U : 1U;
    if (!add_product(result.scratch_bytes, result.pixels, sizeof(Color) * full_buffers,
                     limits.max_scratch_bytes)) {
        result.error = PostprocessError::ScratchLimit;
        return result;
    }
    if (!add_product(result.sample_visits, result.pixels, 12, limits.max_sample_visits)) {
        result.error = PostprocessError::SampleLimit;
        return result;
    }
    if (bloom_active(settings.bloom)) {
        const auto factor = settings.bloom.downsample;
        result.bloom_width = 1U + (width - 1U) / factor;
        result.bloom_height = 1U + (height - 1U) / factor;
        result.bloom_pixels = static_cast<std::size_t>(result.bloom_width) * result.bloom_height;
        if (!add_product(result.scratch_bytes, result.bloom_pixels, sizeof(Color) * 2U,
                         limits.max_scratch_bytes)) {
            result.error = PostprocessError::ScratchLimit;
            return result;
        }
        const std::size_t taps = 2U * static_cast<std::size_t>(settings.bloom.radius) + 2U;
        const std::size_t passes = 2U * static_cast<std::size_t>(settings.bloom.iterations);
        if (!add_product(result.sample_visits, result.pixels, 6, limits.max_sample_visits) ||
            !add_product(result.sample_visits, result.bloom_pixels, taps * passes,
                         limits.max_sample_visits)) {
            result.error = PostprocessError::SampleLimit;
            return result;
        }
    }
    if (detail_blur(settings.detail)) {
        const std::size_t taps = 2U * static_cast<std::size_t>(settings.detail.radius) + 2U;
        if (!add_product(result.sample_visits, result.pixels, taps * 2U,
                         limits.max_sample_visits)) {
            result.error = PostprocessError::SampleLimit;
            return result;
        }
    }
    if (settings.detail.edge_strength > 0.0 &&
        !add_product(result.sample_visits, result.pixels, 12, limits.max_sample_visits)) {
        result.error = PostprocessError::SampleLimit;
    }
    return result;
}

double histogram_log_mean(const LuminanceHistogram& histogram,
                           double lower, double upper) noexcept {
    const double start = lower * histogram.total_weight;
    const double end = upper * histogram.total_weight;
    const double step = (histogram.maximum_log_luminance - histogram.minimum_log_luminance) /
                        static_cast<double>(postprocess_histogram_bins);
    double position = 0.0;
    double weighted_log = 0.0;
    double accepted = 0.0;
    for (std::size_t index = 0; index < histogram.weights.size(); ++index) {
        const double next = position + histogram.weights[index];
        const double overlap = std::max(0.0, std::min(next, end) - std::max(position, start));
        const double center = histogram.minimum_log_luminance +
                              (static_cast<double>(index) + 0.5) * step;
        weighted_log += overlap * center;
        accepted += overlap;
        position = next;
    }
    return accepted > 0.0 ? weighted_log / accepted : 0.0;
}

PostprocessError scan_histogram(const Image& image, const ExposureSettings& settings,
                                LuminanceHistogram& histogram,
                                std::size_t& clipped_pixels) noexcept {
    histogram = {};
    histogram.minimum_log_luminance = settings.minimum_log_luminance;
    histogram.maximum_log_luminance = settings.maximum_log_luminance;
    const double span = settings.maximum_log_luminance - settings.minimum_log_luminance;
    const double floor = std::exp2(settings.minimum_log_luminance);
    double log_sum = 0.0;
    double linear_sum = 0.0;
    histogram.minimum_luminance = std::numeric_limits<double>::max();
    const auto pixels = image.pixels();
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            const auto pixel = pixels[static_cast<std::size_t>(y) * image.width() + x];
            if (!finite(pixel) || pixel.a < 0.0 || pixel.a > 1.0 ||
                std::abs(pixel.r) > maximum_input || std::abs(pixel.g) > maximum_input ||
                std::abs(pixel.b) > maximum_input) {
                return PostprocessError::InvalidPixel;
            }
            if (pixel.r < 0.0 || pixel.g < 0.0 || pixel.b < 0.0) {
                ++clipped_pixels;
            }
            const double nx = 2.0 * (static_cast<double>(x) + 0.5) / image.width() - 1.0;
            const double ny = 2.0 * (static_cast<double>(y) + 0.5) / image.height() - 1.0;
            const double center = std::max(0.0, 1.0 - 0.5 * (nx * nx + ny * ny));
            double weight = 1.0 + settings.center_weight * (center - 1.0);
            weight *= settings.alpha_weighted ? pixel.a : 1.0;
            if (weight <= 0.0) {
                continue;
            }
            const double light = positive_luminance(pixel);
            const double log_light = std::log2(std::max(light, floor));
            const double coordinate = std::clamp((log_light - settings.minimum_log_luminance) / span,
                                                  0.0, 1.0);
            const auto index = std::min(postprocess_histogram_bins - 1U,
                static_cast<std::size_t>(coordinate * static_cast<double>(postprocess_histogram_bins)));
            histogram.weights[index] += weight;
            histogram.total_weight += weight;
            histogram.minimum_luminance = std::min(histogram.minimum_luminance, light);
            histogram.maximum_luminance = std::max(histogram.maximum_luminance, light);
            histogram.low_clipped_pixels += light < floor ? 1U : 0U;
            histogram.high_clipped_pixels += log_light > settings.maximum_log_luminance ? 1U : 0U;
            ++histogram.contributing_pixels;
            log_sum += weight * log_light;
            linear_sum += weight * light;
        }
    }
    if (histogram.total_weight > 0.0) {
        histogram.arithmetic_mean = linear_sum / histogram.total_weight;
        histogram.geometric_mean = std::exp2(log_sum / histogram.total_weight);
        histogram.trimmed_log_mean = histogram_log_mean(histogram, settings.lower_percentile,
                                                        settings.upper_percentile);
    } else {
        histogram.minimum_luminance = 0.0;
    }
    return PostprocessError::None;
}

KernelWeights gaussian_weights(std::uint32_t radius, double sigma) noexcept {
    KernelWeights result{};
    double total = 0.0;
    const auto signed_radius = static_cast<std::int32_t>(radius);
    for (std::int32_t offset = -signed_radius; offset <= signed_radius; ++offset) {
        const double position = static_cast<double>(offset) / sigma;
        const double weight = std::exp(-0.5 * position * position);
        result[static_cast<std::size_t>(offset + signed_radius)] = weight;
        total += weight;
    }
    for (std::uint32_t index = 0; index <= 2U * radius; ++index) {
        result[index] /= total;
    }
    return result;
}

std::size_t sample_index(std::int64_t x, std::int64_t y,
                         std::uint32_t width, std::uint32_t height) noexcept {
    const auto column = static_cast<std::size_t>(std::clamp<std::int64_t>(x, 0, width - 1U));
    const auto row = static_cast<std::size_t>(std::clamp<std::int64_t>(y, 0, height - 1U));
    return row * width + column;
}

Color blur_sample(std::span<const Color> source, std::uint32_t width,
                   std::uint32_t height, std::uint32_t x, std::uint32_t y,
                   const KernelWeights& weights, std::uint32_t radius,
                   bool horizontal, bool straight) noexcept {
    Color result = Color::transparent();
    const auto extent = static_cast<std::int32_t>(radius);
    for (std::int32_t offset = -extent; offset <= extent; ++offset) {
        const auto column = static_cast<std::int64_t>(x) + (horizontal ? offset : 0);
        const auto row = static_cast<std::int64_t>(y) + (horizontal ? 0 : offset);
        auto value = source[sample_index(column, row, width, height)];
        if (straight) {
            value = scale_rgb(value, value.a);
        }
        result = result + value * weights[static_cast<std::size_t>(offset + extent)];
    }
    return result;
}

void blur_axis(std::span<const Color> source, std::span<Color> target,
                std::uint32_t width, std::uint32_t height,
                const KernelWeights& weights, std::uint32_t radius,
                bool horizontal, bool straight) noexcept {
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            target[static_cast<std::size_t>(y) * width + x] =
                blur_sample(source, width, height, x, y, weights, radius, horizontal, straight);
        }
    }
}

void extract_bloom(std::span<const Color> source, std::uint32_t width,
                   std::uint32_t height, std::span<Color> target,
                   const PostprocessPlan& plan, const BloomSettings& settings) noexcept {
    const double knee = settings.threshold * settings.soft_knee;
    for (std::uint32_t y = 0; y < plan.bloom_height; ++y) {
        for (std::uint32_t x = 0; x < plan.bloom_width; ++x) {
            Color sum = Color::transparent();
            std::size_t count = 0;
            const auto end_y = std::min(height, (y + 1U) * settings.downsample);
            const auto end_x = std::min(width, (x + 1U) * settings.downsample);
            for (auto row = y * settings.downsample; row < end_y; ++row) {
                for (auto column = x * settings.downsample; column < end_x; ++column) {
                    const auto pixel = source[static_cast<std::size_t>(row) * width + column];
                    const double light = positive_luminance(pixel);
                    double excess = std::max(light - settings.threshold, 0.0);
                    if (knee > 0.0) {
                        const double soft = std::clamp(light - settings.threshold + knee, 0.0, 2.0 * knee);
                        excess = std::max(excess, soft * soft / (4.0 * knee));
                    }
                    const double factor = light > 0.0 ? std::min(excess / light, 1.0) : 0.0;
                    sum = sum + scale_rgb(pixel, factor * pixel.a);
                    ++count;
                }
            }
            target[static_cast<std::size_t>(y) * plan.bloom_width + x] =
                sum * (1.0 / static_cast<double>(count));
        }
    }
}

Color bloom_sample(std::span<const Color> pixels, const PostprocessPlan& plan,
                    double x, double y) noexcept {
    const double base_x = std::floor(x);
    const double base_y = std::floor(y);
    const auto ix = static_cast<std::int64_t>(base_x);
    const auto iy = static_cast<std::int64_t>(base_y);
    const double fx = x - base_x;
    const double fy = y - base_y;
    const auto a = pixels[sample_index(ix, iy, plan.bloom_width, plan.bloom_height)];
    const auto b = pixels[sample_index(ix + 1, iy, plan.bloom_width, plan.bloom_height)];
    const auto c = pixels[sample_index(ix, iy + 1, plan.bloom_width, plan.bloom_height)];
    const auto d = pixels[sample_index(ix + 1, iy + 1, plan.bloom_width, plan.bloom_height)];
    return (a * (1.0 - fx) + b * fx) * (1.0 - fy) +
           (c * (1.0 - fx) + d * fx) * fy;
}

void composite_bloom(std::span<Color> pixels, std::span<const Color> bloom,
                     std::uint32_t width, std::uint32_t height,
                     const PostprocessPlan& plan, double strength) noexcept {
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const double bx = (static_cast<double>(x) + 0.5) * plan.bloom_width / width - 0.5;
            const double by = (static_cast<double>(y) + 0.5) * plan.bloom_height / height - 0.5;
            const auto glow = bloom_sample(bloom, plan, bx, by);
            auto& target = pixels[static_cast<std::size_t>(y) * width + x];
            if (glow.a > coverage_epsilon && target.a > 0.0) {
                const double scale = strength / glow.a;
                target.r += glow.r * scale;
                target.g += glow.g * scale;
                target.b += glow.b * scale;
            }
        }
    }
}

struct GradeTransform {
    double red_gain{};
    double green_gain{};
    double blue_gain{};
    double diagonal{};
    double clockwise{};
    double counterclockwise{};
};

GradeTransform grade_transform(const ColorAdjustments& settings) noexcept {
    const double cosine = std::cos(settings.hue_radians);
    const double sine = std::sin(settings.hue_radians) / std::sqrt(3.0);
    const double neutral = (1.0 - cosine) / 3.0;
    return {settings.gain.r * std::exp2(0.5 * settings.temperature - 0.25 * settings.tint),
            settings.gain.g * std::exp2(0.5 * settings.tint),
            settings.gain.b * std::exp2(-0.5 * settings.temperature - 0.25 * settings.tint),
            cosine + neutral, neutral - sine, neutral + sine};
}

Color grade_color(Color value, const PostprocessSettings& settings,
                   const GradeTransform& transform) noexcept {
    if (settings.tone.preserve_chromaticity) {
        const double light = positive_luminance(value);
        if (light > 0.0) {
            value = scale_rgb(value, curve_value(light, settings.tone) / light);
        }
    } else {
        value.r = curve_value(value.r, settings.tone);
        value.g = curve_value(value.g, settings.tone);
        value.b = curve_value(value.b, settings.tone);
    }
    value.r = value.r * transform.red_gain + settings.color.lift.r;
    value.g = value.g * transform.green_gain + settings.color.lift.g;
    value.b = value.b * transform.blue_gain + settings.color.lift.b;
    const double red = transform.diagonal * value.r + transform.clockwise * value.g + transform.counterclockwise * value.b;
    const double green = transform.counterclockwise * value.r + transform.diagonal * value.g + transform.clockwise * value.b;
    const double blue = transform.clockwise * value.r + transform.counterclockwise * value.g + transform.diagonal * value.b;
    value = {std::max(red, 0.0), std::max(green, 0.0), std::max(blue, 0.0), value.a};
    if (settings.color.contrast != 1.0) {
        const double pivot = settings.color.contrast_pivot;
        value.r = std::max(0.0, pivot + (red - pivot) * settings.color.contrast);
        value.g = std::max(0.0, pivot + (green - pivot) * settings.color.contrast);
        value.b = std::max(0.0, pivot + (blue - pivot) * settings.color.contrast);
    }
    const double peak = std::max({value.r, value.g, value.b});
    const double trough = std::min({value.r, value.g, value.b});
    const double chroma = peak > 0.0 ? (peak - trough) / peak : 0.0;
    const double saturation = settings.color.saturation * (1.0 + settings.color.vibrance * (1.0 - chroma));
    if (saturation != 1.0) {
        const double light = positive_luminance(value);
        value.r = light + (value.r - light) * saturation;
        value.g = light + (value.g - light) * saturation;
        value.b = light + (value.b - light) * saturation;
    }
    return value;
}

void apply_grade(std::span<Color> pixels, std::uint32_t width, std::uint32_t height,
                  const PostprocessSettings& settings) noexcept {
    const auto transform = grade_transform(settings.color);
    const auto& vignette = settings.vignette;
    const double aspect = static_cast<double>(width) / height;
    const double scale_x = 1.0 + (aspect - 1.0) * vignette.roundness;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
            pixel = grade_color(pixel, settings, transform);
            if (vignette.strength > 0.0) {
                const double nx = 2.0 * ((static_cast<double>(x) + 0.5) / width - vignette.center_x) * scale_x;
                const double ny = 2.0 * ((static_cast<double>(y) + 0.5) / height - vignette.center_y);
                const double distance = std::hypot(nx, ny);
                const double t = std::clamp((distance - vignette.inner_radius) /
                    (vignette.outer_radius - vignette.inner_radius), 0.0, 1.0);
                pixel = scale_rgb(pixel, 1.0 - vignette.strength * t * t * (3.0 - 2.0 * t));
            }
        }
    }
}

void apply_detail(std::span<Color> pixels, std::span<Color> scratch,
                   std::uint32_t width, std::uint32_t height,
                   const DetailSettings& settings) noexcept {
    if (detail_blur(settings)) {
        const auto weights = gaussian_weights(settings.radius, settings.sigma);
        blur_axis(pixels, scratch, width, height, weights, settings.radius, true, true);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                auto& original = pixels[static_cast<std::size_t>(y) * width + x];
                auto smooth = blur_sample(scratch, width, height, x, y, weights, settings.radius, false, false);
                smooth = smooth.a > coverage_epsilon ? scale_rgb(smooth, 1.0 / smooth.a) : original;
                const double difference = std::abs(positive_luminance(original) - positive_luminance(smooth));
                const double sharpen = difference > settings.sharpen_threshold ? settings.sharpen : 0.0;
                const double amount = settings.blur_mix - sharpen;
                original.r += (smooth.r - original.r) * amount;
                original.g += (smooth.g - original.g) * amount;
                original.b += (smooth.b - original.b) * amount;
            }
        }
    }
    if (settings.edge_strength <= 0.0) {
        return;
    }
    std::copy(pixels.begin(), pixels.end(), scratch.begin());
    const auto sample = [&](std::int64_t x, std::int64_t y) noexcept {
        const auto color = scratch[sample_index(x, y, width, height)];
        return positive_luminance(color) * color.a;
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto ix = static_cast<std::int64_t>(x);
            const auto iy = static_cast<std::int64_t>(y);
            const double tl = sample(ix - 1, iy - 1);
            const double tc = sample(ix, iy - 1);
            const double tr = sample(ix + 1, iy - 1);
            const double ml = sample(ix - 1, iy);
            const double mr = sample(ix + 1, iy);
            const double bl = sample(ix - 1, iy + 1);
            const double bc = sample(ix, iy + 1);
            const double br = sample(ix + 1, iy + 1);
            const double dx = tr + 2.0 * mr + br - tl - 2.0 * ml - bl;
            const double dy = bl + 2.0 * bc + br - tl - 2.0 * tc - tr;
            const double edge = std::clamp((std::hypot(dx, dy) * 0.25 - settings.edge_threshold) /
                                           std::max(settings.edge_threshold, 1.0e-6), 0.0, 1.0);
            auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
            pixel = scale_rgb(pixel, 1.0 - settings.edge_strength * edge);
        }
    }
}

} // namespace

std::string_view describe(ToneCurve curve) noexcept {
    switch (curve) {
    case ToneCurve::Linear: return "linear";
    case ToneCurve::Reinhard: return "reinhard";
    case ToneCurve::WhiteReinhard: return "white-reinhard";
    case ToneCurve::Exponential: return "exponential";
    case ToneCurve::SoftShoulder: return "soft-shoulder";
    }
    return "invalid";
}

std::string_view describe(PostprocessError error) noexcept {
    switch (error) {
    case PostprocessError::None: return "none";
    case PostprocessError::InvalidLimits: return "invalid limits";
    case PostprocessError::InvalidSettings: return "invalid settings";
    case PostprocessError::InvalidImage: return "invalid image";
    case PostprocessError::InvalidPixel: return "invalid pixel";
    case PostprocessError::InvalidTime: return "invalid frame time";
    case PostprocessError::PixelLimit: return "pixel budget exceeded";
    case PostprocessError::ScratchLimit: return "scratch budget exceeded";
    case PostprocessError::SampleLimit: return "sample budget exceeded";
    case PostprocessError::AllocationFailure: return "scratch allocation failed";
    case PostprocessError::MissingGraph: return "standard graph anchors unavailable";
    case PostprocessError::GraphConflict: return "postprocess pass belongs to another owner";
    case PostprocessError::GraphRejected: return "graph insertion rejected";
    case PostprocessError::Stopped: return "postprocess stopped";
    }
    return "unknown";
}

PostprocessPlan::operator bool() const noexcept { return error == PostprocessError::None; }
PostprocessResult::operator bool() const noexcept { return error == PostprocessError::None; }

bool valid(const PostprocessLimits& limits) noexcept {
    return limits.max_dimension > 0 && limits.max_dimension <= 32768U &&
           limits.max_pixels > 0 && limits.max_pixels <= 16U * 1024U * 1024U &&
           limits.max_scratch_bytes >= sizeof(Color) && limits.max_scratch_bytes <= 1024U * 1024U * 1024U &&
           limits.max_sample_visits > 0 && limits.max_sample_visits <= 1024U * 1024U * 1024U &&
           limits.max_blur_radius > 0 && limits.max_blur_radius <= hard_radius &&
           limits.max_blur_iterations > 0 && limits.max_blur_iterations <= 4U;
}

bool valid(const PostprocessSettings& settings, const PostprocessLimits& limits) noexcept {
    if (!valid(limits) || !valid_tone(settings.tone)) {
        return false;
    }
    const auto& exposure = settings.exposure;
    if ((exposure.meter != ExposureMeter::LogAverage && exposure.meter != ExposureMeter::Histogram) ||
        !within(exposure.manual_ev, -24.0, 24.0) || !within(exposure.compensation_ev, -24.0, 24.0) ||
        !within(exposure.minimum_ev, -24.0, 24.0) || !within(exposure.maximum_ev, exposure.minimum_ev, 24.0) ||
        !within(exposure.middle_gray, 0.001, 1.0) || !within(exposure.minimum_log_luminance, -40.0, 39.0) ||
        !within(exposure.maximum_log_luminance, exposure.minimum_log_luminance + 0.001, 40.0) ||
        !within(exposure.lower_percentile, 0.0, 0.99) ||
        !within(exposure.upper_percentile, exposure.lower_percentile + 0.001, 1.0) ||
        !within(exposure.center_weight, 0.0, 1.0) || !within(exposure.brighten_speed, 0.0, 100.0) ||
        !within(exposure.darken_speed, 0.0, 100.0) || !within(exposure.maximum_delta_seconds, 0.001, 1.0)) {
        return false;
    }
    const auto& bloom = settings.bloom;
    if (bloom.downsample == 0 || bloom.downsample > 16U ||
        (bloom.downsample & (bloom.downsample - 1U)) != 0 ||
        bloom.radius == 0 || bloom.radius > limits.max_blur_radius ||
        bloom.iterations == 0 || bloom.iterations > limits.max_blur_iterations ||
        !within(bloom.sigma, 0.1, 64.0) || !within(bloom.threshold, 0.0, 65504.0) ||
        !within(bloom.soft_knee, 0.0, 1.0) || !within(bloom.strength, 0.0, 16.0)) {
        return false;
    }
    const auto& color = settings.color;
    if (!finite(color.gain) || !finite(color.lift) ||
        !within(color.gain.r, 0.0, 16.0) || !within(color.gain.g, 0.0, 16.0) ||
        !within(color.gain.b, 0.0, 16.0) || !within(color.lift.r, -1.0, 1.0) ||
        !within(color.lift.g, -1.0, 1.0) || !within(color.lift.b, -1.0, 1.0) ||
        !within(color.contrast, 0.0, 4.0) || !within(color.contrast_pivot, 0.0, 1.0) ||
        !within(color.saturation, 0.0, 4.0) || !within(color.vibrance, -1.0, 1.0) ||
        !within(color.temperature, -1.0, 1.0) || !within(color.tint, -1.0, 1.0) ||
        !within(color.hue_radians, -3.141592653589793, 3.141592653589793)) {
        return false;
    }
    const auto& detail = settings.detail;
    const auto& vignette = settings.vignette;
    return detail.radius > 0 && detail.radius <= limits.max_blur_radius &&
           within(detail.sigma, 0.1, 64.0) && within(detail.blur_mix, 0.0, 1.0) &&
           within(detail.sharpen, 0.0, 4.0) && within(detail.sharpen_threshold, 0.0, 65504.0) &&
           within(detail.edge_strength, 0.0, 1.0) && within(detail.edge_threshold, 0.0, 65504.0) &&
           within(vignette.strength, 0.0, 1.0) && within(vignette.inner_radius, 0.0, 2.0) &&
           within(vignette.outer_radius, vignette.inner_radius + 0.001, 4.0) &&
           within(vignette.roundness, 0.0, 1.0) && within(vignette.center_x, 0.0, 1.0) &&
           within(vignette.center_y, 0.0, 1.0) && within(settings.output_ceiling, 1.0, maximum_input);
}

double histogram_percentile(const LuminanceHistogram& histogram, double percentile) noexcept {
    if (!within(percentile, 0.0, 1.0) || !within(histogram.total_weight, coverage_epsilon, 16777216.0) ||
        !within(histogram.minimum_log_luminance, -40.0, 39.0) ||
        !within(histogram.maximum_log_luminance, histogram.minimum_log_luminance + 0.001, 40.0) ||
        !within(histogram.maximum_luminance, 0.0, maximum_input)) {
        return 0.0;
    }
    double total = 0.0;
    for (const double weight : histogram.weights) {
        if (!within(weight, 0.0, histogram.total_weight)) { return 0.0; }
        total += weight;
    }
    if (std::abs(total - histogram.total_weight) > std::max(1.0, histogram.total_weight) * 1.0e-8) {
        return 0.0;
    }
    const double target = percentile * histogram.total_weight;
    const double step = (histogram.maximum_log_luminance - histogram.minimum_log_luminance) /
                        static_cast<double>(postprocess_histogram_bins);
    double accumulated = 0.0;
    for (std::size_t index = 0; index < histogram.weights.size(); ++index) {
        const double weight = histogram.weights[index];
        if (weight > 0.0 && accumulated + weight >= target) {
            const double fraction = std::clamp((target - accumulated) / weight, 0.0, 1.0);
            return std::exp2(histogram.minimum_log_luminance +
                            (static_cast<double>(index) + fraction) * step);
        }
        accumulated += weight;
    }
    return histogram.maximum_luminance;
}

double tone_map_value(double value, const ToneSettings& settings) noexcept {
    if (!within(value, 0.0, maximum_input) || !valid_tone(settings)) {
        return 0.0;
    }
    return curve_value(value, settings);
}

struct PostprocessService::Impl {
    PostprocessLimits limits;
    PostprocessSettings settings;
    PostprocessStatistics statistics;
    LuminanceHistogram histogram;
    PostprocessError error{PostprocessError::None};
    PostprocessError graph_error{PostprocessError::None};
    // Identity only; the graph may be destroyed before this service.
    const RenderGraph* graph_identity{};
    GraphPass graph_pass{};
    GraphResource graph_color{};
    std::uint64_t callback_revision{};
    double exposure_ev{};
    bool initialized{};
    bool connected{};
    bool active{true};

    explicit Impl(PostprocessLimits value) : limits(value) {
        if (!valid(limits)) {
            throw std::invalid_argument("invalid postprocess limits");
        }
        settings.bloom.radius = std::min(settings.bloom.radius, limits.max_blur_radius);
    }

    void begin(const PostprocessPlan& plan) noexcept {
        increment(statistics.attempts);
        statistics.pixels = plan.pixels;
        statistics.scratch_bytes = plan.scratch_bytes;
        statistics.sample_visits = plan.sample_visits;
        statistics.input_clipped_pixels = 0;
        statistics.output_clipped_pixels = 0;
        statistics.image_changed = false;
    }

    PostprocessResult reject(PostprocessError reason) noexcept {
        error = reason;
        increment(statistics.rejected);
        return {reason, false};
    }

    PostprocessResult finish(bool changed) noexcept {
        error = PostprocessError::None;
        statistics.image_changed = changed;
        increment(statistics.completed);
        return {PostprocessError::None, changed};
    }

    PostprocessResult measure(const Image& image) noexcept {
        auto measuring = settings;
        measuring.enabled = false;
        auto estimate = make_plan(image.width(), image.height(), measuring, limits);
        estimate.sample_visits = estimate.pixels;
        begin(estimate);
        if (!active) { return reject(PostprocessError::Stopped); }
        if (!estimate) { return reject(estimate.error); }
        if (image.size() != estimate.pixels) { return reject(PostprocessError::InvalidImage); }
        if (estimate.sample_visits > limits.max_sample_visits) {
            return reject(PostprocessError::SampleLimit);
        }
        LuminanceHistogram candidate;
        const auto result = scan_histogram(image, settings.exposure, candidate, statistics.input_clipped_pixels);
        if (result != PostprocessError::None) { return reject(result); }
        histogram = candidate;
        return finish(false);
    }

    PostprocessResult run(Image& image, double delta_seconds) noexcept {
        const auto estimate = make_plan(image.width(), image.height(), settings, limits);
        begin(estimate);
        if (!active) { return reject(PostprocessError::Stopped); }
        if (!within(delta_seconds, 0.0, 60.0)) { return reject(PostprocessError::InvalidTime); }
        if (!estimate) { return reject(estimate.error); }
        if (image.size() != estimate.pixels) { return reject(PostprocessError::InvalidImage); }
        if (!settings.enabled) { return finish(false); }
        LuminanceHistogram candidate_histogram;
        const auto measured = scan_histogram(image, settings.exposure, candidate_histogram,
                                             statistics.input_clipped_pixels);
        if (measured != PostprocessError::None) { return reject(measured); }
        const auto& exposure = settings.exposure;
        double target = std::clamp(exposure.manual_ev + exposure.compensation_ev,
                                   exposure.minimum_ev, exposure.maximum_ev);
        if (exposure.automatic && candidate_histogram.total_weight > 0.0) {
            const double measured_log = exposure.meter == ExposureMeter::Histogram ?
                candidate_histogram.trimmed_log_mean : std::log2(candidate_histogram.geometric_mean);
            target = std::clamp(std::log2(exposure.middle_gray) - measured_log + exposure.compensation_ev,
                                exposure.minimum_ev, exposure.maximum_ev);
        } else if (exposure.automatic && initialized) {
            target = std::clamp(exposure_ev, exposure.minimum_ev, exposure.maximum_ev);
        }
        double next_ev = target;
        if (exposure.automatic && initialized) {
            const double previous = std::clamp(exposure_ev, exposure.minimum_ev, exposure.maximum_ev);
            const double speed = target > previous ? exposure.brighten_speed : exposure.darken_speed;
            const double fraction = -std::expm1(-speed * std::min(delta_seconds, exposure.maximum_delta_seconds));
            next_ev = previous + (target - previous) * fraction;
        }
        const double multiplier = std::exp2(next_ev);
        try {
            auto output = std::make_unique<Color[]>(estimate.pixels);
            std::unique_ptr<Color[]> detail;
            std::unique_ptr<Color[]> bloom;
            std::unique_ptr<Color[]> bloom_scratch;
            if (detail_buffer(settings.detail)) {
                detail = std::make_unique<Color[]>(estimate.pixels);
            }
            if (estimate.bloom_pixels != 0) {
                bloom = std::make_unique<Color[]>(estimate.bloom_pixels);
                bloom_scratch = std::make_unique<Color[]>(estimate.bloom_pixels);
            }
            const auto input = std::as_const(image).pixels();
            const std::span<Color> pixels(output.get(), estimate.pixels);
            for (std::size_t index = 0; index < pixels.size(); ++index) {
                const auto value = input[index];
                pixels[index] = {std::max(value.r, 0.0) * multiplier,
                                 std::max(value.g, 0.0) * multiplier,
                                 std::max(value.b, 0.0) * multiplier, value.a};
            }
            if (estimate.bloom_pixels != 0) {
                const std::span<Color> bright(bloom.get(), estimate.bloom_pixels);
                const std::span<Color> temporary(bloom_scratch.get(), estimate.bloom_pixels);
                extract_bloom(pixels, image.width(), image.height(), bright, estimate, settings.bloom);
                const auto weights = gaussian_weights(settings.bloom.radius, settings.bloom.sigma);
                for (std::uint32_t iteration = 0; iteration < settings.bloom.iterations; ++iteration) {
                    blur_axis(bright, temporary, estimate.bloom_width, estimate.bloom_height,
                              weights, settings.bloom.radius, true, false);
                    blur_axis(temporary, bright, estimate.bloom_width, estimate.bloom_height,
                              weights, settings.bloom.radius, false, false);
                }
                composite_bloom(pixels, bright, image.width(), image.height(), estimate, settings.bloom.strength);
            }
            apply_grade(pixels, image.width(), image.height(), settings);
            if (detail) {
                apply_detail(pixels, {detail.get(), estimate.pixels}, image.width(), image.height(), settings.detail);
            }
            bool changed = false;
            for (std::size_t index = 0; index < pixels.size(); ++index) {
                auto& pixel = pixels[index];
                if (!finite(pixel)) { return reject(PostprocessError::InvalidPixel); }
                const Color bounded{std::clamp(pixel.r, 0.0, settings.output_ceiling),
                                    std::clamp(pixel.g, 0.0, settings.output_ceiling),
                                    std::clamp(pixel.b, 0.0, settings.output_ceiling), input[index].a};
                statistics.output_clipped_pixels += pixel != bounded ? 1U : 0U;
                pixel = bounded;
                changed = changed || pixel != input[index];
            }
            if (changed) {
                std::copy(pixels.begin(), pixels.end(), image.pixels().begin());
            }
            histogram = candidate_histogram;
            exposure_ev = next_ev;
            initialized = initialized || !exposure.automatic || histogram.total_weight > 0.0;
            statistics.exposure_ev = exposure_ev;
            statistics.target_exposure_ev = target;
            statistics.exposure_multiplier = multiplier;
            statistics.exposure_initialized = initialized;
            return finish(changed);
        } catch (const std::bad_alloc&) {
            return reject(PostprocessError::AllocationFailure);
        }
    }
};

PostprocessService::PostprocessService(PostprocessLimits limits) : impl_(std::make_shared<Impl>(limits)) {}
PostprocessService::~PostprocessService() = default;
const PostprocessLimits& PostprocessService::limits() const noexcept { return impl_->limits; }
const PostprocessSettings& PostprocessService::settings() const noexcept { return impl_->settings; }
const PostprocessStatistics& PostprocessService::statistics() const noexcept { return impl_->statistics; }
const LuminanceHistogram& PostprocessService::histogram() const noexcept { return impl_->histogram; }
PostprocessError PostprocessService::last_error() const noexcept { return impl_->error; }
PostprocessError PostprocessService::attachment_error() const noexcept { return impl_->graph_error; }

bool PostprocessService::configure(const PostprocessSettings& settings) noexcept {
    if (!valid(settings, impl_->limits)) {
        impl_->error = PostprocessError::InvalidSettings;
        return false;
    }
    if (impl_->settings.exposure.automatic != settings.exposure.automatic) {
        reset_exposure();
    }
    impl_->settings = settings;
    impl_->error = PostprocessError::None;
    return true;
}

void PostprocessService::reset_exposure() noexcept {
    impl_->initialized = false;
    impl_->statistics.exposure_initialized = false;
}

PostprocessPlan PostprocessService::plan(std::uint32_t width, std::uint32_t height) const noexcept {
    return make_plan(width, height, impl_->settings, impl_->limits);
}

PostprocessResult PostprocessService::measure(const Image& image) noexcept { return impl_->measure(image); }
PostprocessResult PostprocessService::process(Image& image, double delta_seconds) noexcept {
    return impl_->run(image, delta_seconds);
}

bool PostprocessService::attached(const RenderGraph& graph) const noexcept {
    const auto pass = graph.find_pass(graph_postprocess_pass);
    const auto info = graph.pass_info(impl_->graph_pass);
    return impl_->active && impl_->connected && impl_->graph_identity == &graph &&
           pass && *pass == impl_->graph_pass &&
           info && info->callback_owner == graph_postprocess_callback_owner &&
           info->callback_revision == impl_->callback_revision &&
           graph.resource_info(impl_->graph_color).has_value();
}

PostprocessError PostprocessService::attach(RenderGraph& graph) {
    if (!impl_->active) { return impl_->graph_error = PostprocessError::Stopped; }
    const auto color = graph.find_resource(graph_main_color);
    const auto scene = graph.find_pass(graph_scene_pass);
    const auto complete = graph.find_pass(graph_complete_pass);
    if (!color || !scene || !complete) {
        return impl_->graph_error = PostprocessError::MissingGraph;
    }
    const auto resource = graph.resource_info(*color);
    if (!resource || resource->kind != GraphResourceKind::Image || !resource->imported) {
        return impl_->graph_error = PostprocessError::MissingGraph;
    }
    const auto existing = graph.find_pass(graph_postprocess_pass);
    const auto stamp = existing ? graph.pass_info(*existing) : std::nullopt;
    if (existing && (*existing != impl_->graph_pass || *color != impl_->graph_color || !stamp ||
        stamp->callback_owner != graph_postprocess_callback_owner ||
        stamp->callback_revision != impl_->callback_revision)) {
        return impl_->graph_error = PostprocessError::GraphConflict;
    }
    if (attached(graph)) { return impl_->graph_error = PostprocessError::None; }
    try {
        const std::weak_ptr<Impl> weak = impl_;
        const GraphCallback callback = [weak, target = *color](GraphContext& context) -> GraphPassResult {
            const auto state = weak.lock();
            if (!state || !state->active || !state->connected ||
                context.pass() != state->graph_pass || target != state->graph_color) { return {}; }
            auto* image = context.write_image(target);
            if (!image) { return {false, false, "postprocess image unavailable"}; }
            const auto result = state->run(*image, context.frame().delta_seconds);
            return {static_cast<bool>(result), result.image_changed, {}};
        };
        GraphPass inserted;
        if (existing) {
            if (graph.replace_callback_if(*existing, graph_postprocess_callback_owner,
                impl_->callback_revision, graph_postprocess_callback_owner, callback)) { inserted = *existing; }
        } else {
            inserted = graph.insert_before(*complete, {std::string(graph_postprocess_pass),
                {{*color, GraphAccess::ReadWrite}}, {*scene}, 0, true, graph_postprocess_callback_owner}, callback);
        }
        if (!inserted) { return impl_->graph_error = PostprocessError::GraphRejected; }
        impl_->graph_identity = &graph;
        impl_->graph_pass = inserted;
        impl_->graph_color = *color;
        impl_->callback_revision = graph.pass_info(inserted)->callback_revision;
        impl_->connected = true;
        return impl_->graph_error = PostprocessError::None;
    } catch (const std::bad_alloc&) {
        return impl_->graph_error = PostprocessError::AllocationFailure;
    } catch (const std::length_error&) {
        return impl_->graph_error = PostprocessError::AllocationFailure;
    }
}

bool PostprocessService::detach(RenderGraph& graph) {
    if (impl_->graph_identity && impl_->graph_identity != &graph) {
        impl_->graph_error = PostprocessError::GraphConflict;
        return false;
    }
    if (!graph.pass_info(impl_->graph_pass)) {
        impl_->connected = false;
        impl_->graph_error = PostprocessError::None;
        return true;
    }
    if (!graph.replace_callback_if(impl_->graph_pass, graph_postprocess_callback_owner,
        impl_->callback_revision, graph_postprocess_callback_owner,
        [](GraphContext&) -> GraphPassResult { return {}; })) {
        impl_->graph_error = PostprocessError::GraphConflict;
        return false;
    }
    impl_->callback_revision = graph.pass_info(impl_->graph_pass)->callback_revision;
    impl_->connected = false;
    impl_->graph_error = PostprocessError::None;
    return true;
}

void PostprocessService::stop() noexcept {
    impl_->active = false;
    impl_->connected = false;
}

namespace {

std::optional<double> number(std::string_view text) noexcept {
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> boolean(std::string_view text) noexcept {
    if (text == "on" || text == "true" || text == "1") { return true; }
    if (text == "off" || text == "false" || text == "0") { return false; }
    return std::nullopt;
}

std::string statistics_text(const PostprocessService& service) {
    const auto& stats = service.statistics();
    std::ostringstream output;
    output << "error=" << describe(service.last_error()) << " graph=" << describe(service.attachment_error())
           << " enabled=" << service.settings().enabled << " curve=" << describe(service.settings().tone.curve)
           << " attempts=" << stats.attempts << " completed=" << stats.completed << " rejected=" << stats.rejected
           << " pixels=" << stats.pixels << " scratch_bytes=" << stats.scratch_bytes
           << " planned_samples=" << stats.sample_visits << " exposure_ev=" << stats.exposure_ev
           << " target_ev=" << stats.target_exposure_ev << " input_clipped=" << stats.input_clipped_pixels
           << " output_clipped=" << stats.output_clipped_pixels << " changed=" << stats.image_changed;
    return output.str();
}

core::CommandResult set_command(core::Kernel& kernel, core::CommandArgs arguments) {
    if (arguments.size() != 2) { return {false, "usage: render.postprocess.set <setting> <value>"}; }
    auto& service = kernel.services().get<PostprocessService>();
    auto settings = service.settings();
    const std::string_view key = arguments[0];
    const std::string_view text = arguments[1];
    bool found = false;
    if (key == "curve") {
        for (const auto curve : {ToneCurve::Linear, ToneCurve::Reinhard, ToneCurve::WhiteReinhard,
                                 ToneCurve::Exponential, ToneCurve::SoftShoulder}) {
            if (text == describe(curve)) { settings.tone.curve = curve; found = true; }
        }
    } else if (key == "enabled" || key == "automatic" || key == "bloom" || key == "alpha_weighted") {
        const auto value = boolean(text);
        if (!value) { return {false, "expected on or off"}; }
        if (key == "enabled") { settings.enabled = *value; }
        if (key == "automatic") { settings.exposure.automatic = *value; }
        if (key == "bloom") { settings.bloom.enabled = *value; }
        if (key == "alpha_weighted") { settings.exposure.alpha_weighted = *value; }
        found = true;
    } else if (key == "meter") {
        if (text == "histogram") { settings.exposure.meter = ExposureMeter::Histogram; found = true; }
        if (text == "log-average") { settings.exposure.meter = ExposureMeter::LogAverage; found = true; }
    } else {
        const auto value = number(text);
        if (!value) { return {false, "expected a finite number"}; }
        const std::pair<std::string_view, double*> fields[] = {
            {"exposure", &settings.exposure.manual_ev}, {"compensation", &settings.exposure.compensation_ev},
            {"minimum_ev", &settings.exposure.minimum_ev}, {"maximum_ev", &settings.exposure.maximum_ev},
            {"middle_gray", &settings.exposure.middle_gray}, {"center_weight", &settings.exposure.center_weight},
            {"lower_percentile", &settings.exposure.lower_percentile}, {"upper_percentile", &settings.exposure.upper_percentile},
            {"brighten_speed", &settings.exposure.brighten_speed}, {"darken_speed", &settings.exposure.darken_speed},
            {"white_point", &settings.tone.white_point}, {"shoulder_start", &settings.tone.shoulder_start},
            {"bloom_threshold", &settings.bloom.threshold}, {"bloom_strength", &settings.bloom.strength},
            {"bloom_knee", &settings.bloom.soft_knee}, {"bloom_sigma", &settings.bloom.sigma},
            {"contrast", &settings.color.contrast}, {"saturation", &settings.color.saturation},
            {"vibrance", &settings.color.vibrance}, {"temperature", &settings.color.temperature},
            {"tint", &settings.color.tint}, {"hue", &settings.color.hue_radians},
            {"red_gain", &settings.color.gain.r}, {"green_gain", &settings.color.gain.g},
            {"blue_gain", &settings.color.gain.b}, {"blur", &settings.detail.blur_mix},
            {"sharpen", &settings.detail.sharpen}, {"sharpen_threshold", &settings.detail.sharpen_threshold},
            {"edge_strength", &settings.detail.edge_strength}, {"edge_threshold", &settings.detail.edge_threshold},
            {"vignette", &settings.vignette.strength}, {"output_ceiling", &settings.output_ceiling}
        };
        for (const auto& [name, field] : fields) {
            if (key == name) { *field = *value; found = true; break; }
        }
        if (!found && (key == "bloom_radius" || key == "bloom_iterations" ||
                       key == "downsample" || key == "detail_radius")) {
            if (!within(*value, 1.0, 64.0) || std::floor(*value) != *value) {
                return {false, "expected an integer in [1,64]"};
            }
            const auto integer = static_cast<std::uint32_t>(*value);
            if (key == "bloom_radius") { settings.bloom.radius = integer; }
            if (key == "bloom_iterations") { settings.bloom.iterations = integer; }
            if (key == "downsample") { settings.bloom.downsample = integer; }
            if (key == "detail_radius") { settings.detail.radius = integer; }
            found = true;
        }
    }
    if (!found) { return {false, "unknown postprocess setting or enum value"}; }
    if (!service.configure(settings)) { return {false, std::string(describe(service.last_error()))}; }
    return {true, "postprocess settings committed"};
}

core::CommandResult image_command(core::Kernel& kernel, core::CommandArgs arguments, bool measurement) {
    if (!arguments.empty()) { return {false, "this command takes no arguments"}; }
    auto* images = kernel.services().find<ImageService>();
    if (!images) { return {false, "image service unavailable"}; }
    auto& service = kernel.services().get<PostprocessService>();
    const auto result = measurement ? service.measure(images->framebuffer().color()) :
        service.process(images->framebuffer().color(), kernel.frame().delta_seconds);
    if (!result || !measurement) { return {static_cast<bool>(result), statistics_text(service)}; }
    const auto& histogram = service.histogram();
    std::ostringstream output;
    output << "weight=" << histogram.total_weight << " contributing=" << histogram.contributing_pixels
           << " min=" << histogram.minimum_luminance << " max=" << histogram.maximum_luminance
           << " mean=" << histogram.arithmetic_mean << " geometric_mean=" << histogram.geometric_mean
           << " p50=" << histogram_percentile(histogram, 0.5)
           << " p90=" << histogram_percentile(histogram, 0.9)
           << " p99=" << histogram_percentile(histogram, 0.99);
    return {true, output.str()};
}

} // namespace

void register_postprocess(core::Kernel& kernel) {
    if (!kernel.register_module({"render.postprocess", "Bounded linear CPU image postprocessing", 1})) { return; }
    auto& service = kernel.services().emplace<PostprocessService>();
    auto settings = service.settings();
    settings.enabled = kernel.config().boolean("render.postprocess.enabled", true);
    settings.exposure.automatic = kernel.config().boolean("render.postprocess.automatic", false);
    settings.exposure.manual_ev = kernel.config().number("render.postprocess.exposure", 0.0);
    if (!service.configure(settings)) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "render.postprocess", "invalid configuration; defaults retained");
    }
    (void)kernel.add_system({"render.postprocess.install", core::Phase::Render, -60, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto* graph = host.services().find<RenderGraph>();
            if (!graph) { return; }
            auto& current = host.services().get<PostprocessService>();
            const auto previous = current.attachment_error();
            const auto error = current.attach(*graph);
            if (error != PostprocessError::None && error != previous) {
                host.diagnose(core::Diagnostic::Severity::Warning, "render.postprocess", std::string(describe(error)));
            }
        });
    (void)kernel.order_after("render.graph.frame", "render.postprocess.install");
    (void)kernel.add_system({"render.postprocess.metrics", core::Phase::Render, 60, {"render.postprocess.install"}},
        [](core::Kernel& host, const core::Frame&) {
            const auto& current = host.services().get<PostprocessService>();
            const auto& stats = current.statistics();
            host.metric("render.postprocess.error", static_cast<double>(current.last_error()));
            host.metric("render.postprocess.graph_error", static_cast<double>(current.attachment_error()));
            host.metric("render.postprocess.exposure_ev", stats.exposure_ev);
            host.metric("render.postprocess.scratch_bytes", static_cast<double>(stats.scratch_bytes));
            host.metric("render.postprocess.sample_visits", static_cast<double>(stats.sample_visits));
        });
    (void)kernel.order_after("render.postprocess.metrics", "render.graph.frame");
    if (auto* graph = kernel.services().find<RenderGraph>()) { (void)service.attach(*graph); }
    (void)kernel.add_command("render.postprocess.set", set_command, "Set validated exposure, curve, bloom, color, detail or vignette controls");
    (void)kernel.add_command("render.postprocess.stats", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: render.postprocess.stats"}; }
        return {true, statistics_text(host.services().get<PostprocessService>())};
    }, "Inspect the last attempt and graph attachment status");
    (void)kernel.add_command("render.postprocess.apply", [](core::Kernel& host, core::CommandArgs args) {
        return image_command(host, args, false);
    }, "Apply again to the current image; repeated calls compound processing");
    (void)kernel.add_command("render.postprocess.measure", [](core::Kernel& host, core::CommandArgs args) {
        return image_command(host, args, true);
    }, "Measure the current linear image without changing its pixels or exposure history");
    (void)kernel.add_command("render.postprocess.reset", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: render.postprocess.reset"}; }
        host.services().get<PostprocessService>().reset_exposure();
        return {true, "next metered frame initializes automatic exposure"};
    }, "Discard temporal exposure history after a scene or camera cut");
    (void)kernel.add_command("render.postprocess.plan", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: render.postprocess.plan"}; }
        const auto* images = host.services().find<ImageService>();
        if (!images) { return {false, "image service unavailable"}; }
        const auto& image = images->framebuffer().color();
        const auto plan = host.services().get<PostprocessService>().plan(image.width(), image.height());
        std::ostringstream output;
        output << describe(plan.error) << " pixels=" << plan.pixels << " scratch_bytes=" << plan.scratch_bytes
               << " sample_visits=" << plan.sample_visits << " bloom=" << plan.bloom_width << 'x' << plan.bloom_height;
        return {static_cast<bool>(plan), output.str()};
    }, "Preflight the current dimensions and settings without allocating scratch");
    (void)kernel.add_command("render.postprocess.attach", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: render.postprocess.attach"}; }
        auto* graph = host.services().find<RenderGraph>();
        if (!graph) { return {false, "render graph unavailable"}; }
        const auto result = host.services().get<PostprocessService>().attach(*graph);
        return {result == PostprocessError::None, std::string(describe(result))};
    }, "Attach after an explicit graph reset before a manual graph run");
    (void)kernel.add_shutdown("render.postprocess", [host = &kernel] {
        auto* current = host->services().find<PostprocessService>();
        if (!current) { return; }
        if (auto* graph = host->services().find<RenderGraph>()) { (void)current->detach(*graph); }
        current->stop();
    });
}

} // namespace ge::render
