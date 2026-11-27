#include <ge/render/image.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <new>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ge::render {
namespace {

double clean(double value) noexcept {
    return std::isfinite(value) ? value : 0.0;
}

double unit(double value) noexcept {
    return std::clamp(clean(value), 0.0, 1.0);
}

std::uint32_t quantize(double value) noexcept {
    return static_cast<std::uint32_t>(std::floor(unit(value) * 255.0 + 0.5));
}

std::optional<std::uint32_t> addressed(std::int64_t index, std::uint32_t dimension,
                                      AddressMode mode) noexcept {
    if (dimension == 0) {
        return std::nullopt;
    }
    const auto extent = static_cast<std::int64_t>(dimension);
    if (mode == AddressMode::Clamp) {
        return static_cast<std::uint32_t>(std::clamp(index, std::int64_t{0}, extent - 1));
    }
    if (mode == AddressMode::Repeat) {
        const auto wrapped = index % extent;
        return static_cast<std::uint32_t>(wrapped < 0 ? wrapped + extent : wrapped);
    }
    if (mode == AddressMode::Mirror) {
        const auto period = extent * 2;
        const auto remainder = index % period;
        const auto wrapped = remainder < 0 ? remainder + period : remainder;
        return static_cast<std::uint32_t>(wrapped < extent ? wrapped : period - 1 - wrapped);
    }
    if (index < 0 || index >= extent) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(index);
}

double normalized_coordinate(double value, AddressMode mode) noexcept {
    if (mode == AddressMode::Repeat) {
        const double phase = std::fmod(value, 1.0);
        return phase < 0.0 ? phase + 1.0 : phase;
    }
    if (mode == AddressMode::Mirror) {
        double phase = std::fmod(value, 2.0);
        if (phase < 0.0) {
            phase += 2.0;
        }
        return phase <= 1.0 ? phase : 2.0 - phase;
    }
    return mode == AddressMode::Clamp ? std::clamp(value, 0.0, 1.0)
                                     : std::clamp(value, -1.0, 2.0);
}

Color texel(const Image& image, std::int64_t column, std::int64_t row_index,
            const SamplerState& sampler) noexcept {
    const auto x = addressed(column, image.width(), sampler.address_u);
    const auto y = addressed(row_index, image.height(), sampler.address_v);
    if (!x || !y) {
        return sanitize(sampler.border);
    }
    const auto offset = static_cast<std::size_t>(*y) * image.width() + *x;
    return sanitize(image.pixels()[offset]);
}

bool depth_passes(double incoming, double stored, DepthCompare comparison) noexcept {
    switch (comparison) {
    case DepthCompare::Never: return false;
    case DepthCompare::Less: return incoming < stored;
    case DepthCompare::LessEqual: return incoming <= stored;
    case DepthCompare::Equal: return incoming == stored;
    case DepthCompare::GreaterEqual: return incoming >= stored;
    case DepthCompare::Greater: return incoming > stored;
    case DepthCompare::NotEqual: return incoming != stored;
    case DepthCompare::Always: return true;
    }
    return false;
}

std::vector<Image> make_levels(Image base, bool generate_mips) {
    if (base.empty()) {
        throw std::invalid_argument("texture base image is empty");
    }
    std::vector<Image> result;
    result.push_back(std::move(base));
    std::size_t bytes = result.front().byte_size();
    while (generate_mips && (result.back().width() > 1 || result.back().height() > 1)) {
        const auto width = std::max(std::uint32_t{1}, result.back().width() / 2U);
        const auto height = std::max(std::uint32_t{1}, result.back().height() / 2U);
        const auto count = static_cast<std::size_t>(width) * height;
        if (count > (std::numeric_limits<std::size_t>::max() - bytes) / sizeof(Color)) {
            throw std::length_error("texture mip storage overflows address space");
        }
        Image next = resample(result.back(), width, height, ResampleFilter::Area,
                              result.back().limits());
        bytes += next.byte_size();
        result.push_back(std::move(next));
    }
    return result;
}

std::uint64_t library_owner() {
    static std::atomic<std::uint64_t> next{1};
    auto candidate = next.load(std::memory_order_relaxed);
    while (candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(candidate, candidate + 1,
                                        std::memory_order_relaxed)) {
            return candidate;
        }
    }
    throw std::overflow_error("texture library identity space exhausted");
}

template<class Value>
std::optional<Value> number(std::string_view text) noexcept {
    Value value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    if constexpr (std::is_floating_point_v<Value>) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
    }
    return value;
}

std::optional<Color> command_color(core::CommandArgs args, std::size_t offset) noexcept {
    if (offset > args.size() || args.size() - offset < 4) {
        return std::nullopt;
    }
    const auto red = number<double>(args[offset]);
    const auto green = number<double>(args[offset + 1]);
    const auto blue = number<double>(args[offset + 2]);
    const auto alpha = number<double>(args[offset + 3]);
    if (!red || !green || !blue || !alpha || *alpha < 0.0 || *alpha > 1.0) {
        return std::nullopt;
    }
    return Color{*red, *green, *blue, *alpha};
}

std::string describe(Color value) {
    std::ostringstream stream;
    stream << std::setprecision(9) << value.r << ' ' << value.g << ' '
           << value.b << ' ' << value.a;
    return stream.str();
}

} // namespace

bool finite(Color value) noexcept {
    return std::isfinite(value.r) && std::isfinite(value.g) &&
           std::isfinite(value.b) && std::isfinite(value.a);
}

Color sanitize(Color value) noexcept {
    return {clean(value.r), clean(value.g), clean(value.b), unit(value.a)};
}

Color clamped(Color value) noexcept {
    return {unit(value.r), unit(value.g), unit(value.b), unit(value.a)};
}

Color premultiplied(Color value) noexcept {
    value = sanitize(value);
    return {value.r * value.a, value.g * value.a, value.b * value.a, value.a};
}

Color unpremultiplied(Color value) noexcept {
    value = sanitize(value);
    if (value.a == 0.0) {
        return Color::transparent();
    }
    return sanitize({value.r / value.a, value.g / value.a, value.b / value.a, value.a});
}

Color interpolate(Color first, Color second, double amount) noexcept {
    const double weight = unit(amount);
    return unpremultiplied(premultiplied(first) * (1.0 - weight) +
                          premultiplied(second) * weight);
}

double luminance(Color value) noexcept {
    value = sanitize(value);
    return clean(value.r * 0.2126 + value.g * 0.7152 + value.b * 0.0722);
}

double srgb_to_linear(double value) noexcept {
    value = unit(value);
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double value) noexcept {
    value = unit(value);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

Color from_srgb(Color value) noexcept {
    return {srgb_to_linear(value.r), srgb_to_linear(value.g), srgb_to_linear(value.b), unit(value.a)};
}

Color to_srgb(Color value) noexcept {
    return {linear_to_srgb(value.r), linear_to_srgb(value.g), linear_to_srgb(value.b), unit(value.a)};
}

Color blend(Color source, Color destination, BlendMode mode) noexcept {
    source = sanitize(source);
    destination = sanitize(destination);
    if (mode == BlendMode::Replace) {
        return source;
    }
    const double opacity = source.a + destination.a * (1.0 - source.a);
    if (opacity == 0.0) {
        return Color::transparent();
    }
    if (mode == BlendMode::Add) {
        const Color sum = premultiplied(source) + premultiplied(destination);
        return unpremultiplied({sum.r, sum.g, sum.b, opacity});
    }
    if (mode == BlendMode::Multiply) {
        const Color mixed = source * destination;
        const Color sum = premultiplied(source) * (1.0 - destination.a) +
                          premultiplied(destination) * (1.0 - source.a) +
                          mixed * (source.a * destination.a);
        return unpremultiplied({sum.r, sum.g, sum.b, opacity});
    }
    const Color sum = premultiplied(source) + premultiplied(destination) * (1.0 - source.a);
    return unpremultiplied({sum.r, sum.g, sum.b, opacity});
}

std::uint32_t pack_rgba8(Color value, ColorEncoding encoding) noexcept {
    value = encoding == ColorEncoding::Srgb ? to_srgb(value) : clamped(value);
    return quantize(value.r) | (quantize(value.g) << 8U) |
           (quantize(value.b) << 16U) | (quantize(value.a) << 24U);
}

Color unpack_rgba8(std::uint32_t value, ColorEncoding encoding) noexcept {
    constexpr double scale = 1.0 / 255.0;
    const Color unpacked{static_cast<double>(value & 255U) * scale,
                         static_cast<double>((value >> 8U) & 255U) * scale,
                         static_cast<double>((value >> 16U) & 255U) * scale,
                         static_cast<double>((value >> 24U) & 255U) * scale};
    return encoding == ColorEncoding::Srgb ? from_srgb(unpacked) : unpacked;
}

bool Rect::empty() const noexcept {
    return width <= 0 || height <= 0;
}

bool Rect::contains(std::int32_t column, std::int32_t row_index) const noexcept {
    return !empty() && column >= x && row_index >= y &&
           static_cast<std::int64_t>(column) < static_cast<std::int64_t>(x) + width &&
           static_cast<std::int64_t>(row_index) < static_cast<std::int64_t>(y) + height;
}

Rect intersection(Rect first, Rect second) noexcept {
    if (first.empty() || second.empty()) {
        return {};
    }
    const auto left = std::max(first.x, second.x);
    const auto top = std::max(first.y, second.y);
    const auto right = std::min(static_cast<std::int64_t>(first.x) + first.width,
                                static_cast<std::int64_t>(second.x) + second.width);
    const auto bottom = std::min(static_cast<std::int64_t>(first.y) + first.height,
                                 static_cast<std::int64_t>(second.y) + second.height);
    if (right <= left || bottom <= top) {
        return {};
    }
    return {left, top, static_cast<std::int32_t>(right - left),
            static_cast<std::int32_t>(bottom - top)};
}

Image::Image(ImageLimits limits) noexcept : limits_(limits) {}

Image::Image(std::uint32_t width, std::uint32_t height, Color fill, ImageLimits limits)
    : limits_(limits), width_(width), height_(height) {
    if (!valid_dimensions(width, height, limits)) {
        throw std::length_error("image dimensions exceed limits");
    }
    pixels_.assign(static_cast<std::size_t>(width) * height, sanitize(fill));
}

Image& Image::operator=(const Image& other) {
    if (this != &other) {
        Image replacement(other);
        swap(replacement);
    }
    return *this;
}

Image::Image(Image&& other) noexcept : limits_(other.limits_) {
    swap(other);
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        Image replacement(std::move(other));
        swap(replacement);
    }
    return *this;
}

bool Image::valid_dimensions(std::uint32_t width, std::uint32_t height,
                             ImageLimits limits) noexcept {
    if (width == 0 || height == 0) {
        return width == 0 && height == 0;
    }
    constexpr auto coordinate_limit = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (width > limits.max_dimension || height > limits.max_dimension ||
        width > coordinate_limit || height > coordinate_limit) {
        return false;
    }
    const auto allowed = std::min(limits.max_pixels,
                                  std::numeric_limits<std::size_t>::max() / sizeof(Color));
    return static_cast<std::size_t>(height) <= allowed / width;
}

bool Image::resize(std::uint32_t width, std::uint32_t height, Color fill) {
    try {
        Image replacement(width, height, fill, limits_);
        swap(replacement);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

std::uint32_t Image::width() const noexcept { return width_; }
std::uint32_t Image::height() const noexcept { return height_; }
std::size_t Image::size() const noexcept { return pixels_.size(); }
std::size_t Image::byte_size() const noexcept { return pixels_.size() * sizeof(Color); }
bool Image::empty() const noexcept { return pixels_.empty(); }
ImageLimits Image::limits() const noexcept { return limits_; }

Rect Image::bounds() const noexcept {
    return {0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)};
}

bool Image::contains(std::int32_t column, std::int32_t row_index) const noexcept {
    return column >= 0 && row_index >= 0 && static_cast<std::uint32_t>(column) < width_ &&
           static_cast<std::uint32_t>(row_index) < height_;
}

std::span<Color> Image::pixels() noexcept { return pixels_; }
std::span<const Color> Image::pixels() const noexcept { return pixels_; }

std::span<Color> Image::row(std::uint32_t index) {
    if (index >= height_) {
        throw std::out_of_range("image row is outside the image");
    }
    return pixels().subspan(static_cast<std::size_t>(index) * width_, width_);
}

std::span<const Color> Image::row(std::uint32_t index) const {
    if (index >= height_) {
        throw std::out_of_range("image row is outside the image");
    }
    return pixels().subspan(static_cast<std::size_t>(index) * width_, width_);
}

Color& Image::at(std::uint32_t column, std::uint32_t row_index) {
    if (column >= width_ || row_index >= height_) {
        throw std::out_of_range("pixel is outside the image");
    }
    return pixels_[static_cast<std::size_t>(row_index) * width_ + column];
}

const Color& Image::at(std::uint32_t column, std::uint32_t row_index) const {
    if (column >= width_ || row_index >= height_) {
        throw std::out_of_range("pixel is outside the image");
    }
    return pixels_[static_cast<std::size_t>(row_index) * width_ + column];
}

std::optional<Color> Image::pixel(std::int32_t column, std::int32_t row_index) const noexcept {
    if (!contains(column, row_index)) {
        return std::nullopt;
    }
    return pixels_[static_cast<std::size_t>(row_index) * width_ + static_cast<std::size_t>(column)];
}

bool Image::set_pixel(std::int32_t column, std::int32_t row_index, Color value, BlendMode mode) noexcept {
    if (!contains(column, row_index)) {
        return false;
    }
    auto& destination = pixels_[static_cast<std::size_t>(row_index) * width_ + static_cast<std::size_t>(column)];
    destination = blend(value, destination, mode);
    return true;
}

void Image::clear(Color value) noexcept {
    std::fill(pixels_.begin(), pixels_.end(), sanitize(value));
}

std::size_t Image::fill_rect(Rect rectangle, Color value, BlendMode mode) noexcept {
    rectangle = intersection(rectangle, bounds());
    const auto width = static_cast<std::size_t>(rectangle.width);
    const auto height = static_cast<std::size_t>(rectangle.height);
    for (std::size_t y = 0; y < height; ++y) {
        const auto offset = (static_cast<std::size_t>(rectangle.y) + y) * width_ +
                            static_cast<std::size_t>(rectangle.x);
        for (std::size_t x = 0; x < width; ++x) {
            pixels_[offset + x] = blend(value, pixels_[offset + x], mode);
        }
    }
    return width * height;
}

std::size_t Image::blit(const Image& source, Rect source_rectangle,
                        std::int32_t destination_x, std::int32_t destination_y, BlendMode mode) {
    if (source_rectangle.empty() || empty() || source.empty()) {
        return 0;
    }
    const auto sx = static_cast<std::int64_t>(source_rectangle.x);
    const auto sy = static_cast<std::int64_t>(source_rectangle.y);
    const auto dx = static_cast<std::int64_t>(destination_x);
    const auto dy = static_cast<std::int64_t>(destination_y);
    const auto left = std::max({std::int64_t{0}, -sx, -dx});
    const auto top = std::max({std::int64_t{0}, -sy, -dy});
    const auto right = std::min({static_cast<std::int64_t>(source_rectangle.width),
                                 static_cast<std::int64_t>(source.width_) - sx,
                                 static_cast<std::int64_t>(width_) - dx});
    const auto bottom = std::min({static_cast<std::int64_t>(source_rectangle.height),
                                  static_cast<std::int64_t>(source.height_) - sy,
                                  static_cast<std::int64_t>(height_) - dy});
    if (right <= left || bottom <= top) {
        return 0;
    }
    const auto copy_width = static_cast<std::size_t>(right - left);
    const auto copy_height = static_cast<std::size_t>(bottom - top);
    const auto source_x = static_cast<std::size_t>(sx + left);
    const auto source_y = static_cast<std::size_t>(sy + top);
    std::vector<Color> snapshot;
    if (this == &source) {
        snapshot.resize(copy_width * copy_height);
        for (std::size_t y = 0; y < copy_height; ++y) {
            for (std::size_t x = 0; x < copy_width; ++x) {
                snapshot[y * copy_width + x] = source.pixels_[(source_y + y) * source.width_ + source_x + x];
            }
        }
    }
    for (std::size_t y = 0; y < copy_height; ++y) {
        const auto destination_offset = (static_cast<std::size_t>(dy + top) + y) * width_ +
                                        static_cast<std::size_t>(dx + left);
        for (std::size_t x = 0; x < copy_width; ++x) {
            const Color value = snapshot.empty() ? source.pixels_[(source_y + y) * source.width_ + source_x + x]
                                                  : snapshot[y * copy_width + x];
            pixels_[destination_offset + x] = blend(value, pixels_[destination_offset + x], mode);
        }
    }
    return copy_width * copy_height;
}

Image Image::copy_region(Rect rectangle) const {
    rectangle = intersection(rectangle, bounds());
    Image result(static_cast<std::uint32_t>(rectangle.width),
                  static_cast<std::uint32_t>(rectangle.height), Color::transparent(), limits_);
    static_cast<void>(result.blit(*this, rectangle, 0, 0));
    return result;
}

void Image::flip_horizontal() noexcept {
    for (std::uint32_t y = 0; y < height_; ++y) {
        const auto offset = static_cast<std::size_t>(y) * width_;
        for (std::uint32_t x = 0; x < width_ / 2U; ++x) {
            std::swap(pixels_[offset + x], pixels_[offset + width_ - 1U - x]);
        }
    }
}

void Image::flip_vertical() noexcept {
    for (std::uint32_t y = 0; y < height_ / 2U; ++y) {
        const auto first = static_cast<std::size_t>(y) * width_;
        const auto second = static_cast<std::size_t>(height_ - 1U - y) * width_;
        for (std::uint32_t x = 0; x < width_; ++x) {
            std::swap(pixels_[first + x], pixels_[second + x]);
        }
    }
}

void Image::swap(Image& other) noexcept {
    std::swap(limits_, other.limits_);
    std::swap(width_, other.width_);
    std::swap(height_, other.height_);
    pixels_.swap(other.pixels_);
}

Color sample_image(const Image& image, math::Vec2 uv, const SamplerState& sampler) noexcept {
    if (image.empty() || !math::finite(uv)) {
        return sanitize(sampler.border);
    }
    const double u = normalized_coordinate(uv.x, sampler.address_u);
    const double v = normalized_coordinate(uv.y, sampler.address_v);
    if (sampler.filter == FilterMode::Nearest) {
        return texel(image, static_cast<std::int64_t>(std::floor(u * image.width())),
                      static_cast<std::int64_t>(std::floor(v * image.height())), sampler);
    }
    const double x = u * image.width() - 0.5;
    const double y = v * image.height() - 0.5;
    const double left = std::floor(x);
    const double top = std::floor(y);
    const auto column = static_cast<std::int64_t>(left);
    const auto row_index = static_cast<std::int64_t>(top);
    const double tx = x - left;
    const double ty = y - top;
    const Color a = premultiplied(texel(image, column, row_index, sampler));
    const Color b = premultiplied(texel(image, column + 1, row_index, sampler));
    const Color c = premultiplied(texel(image, column, row_index + 1, sampler));
    const Color d = premultiplied(texel(image, column + 1, row_index + 1, sampler));
    return unpremultiplied(a * ((1.0 - tx) * (1.0 - ty)) + b * (tx * (1.0 - ty)) +
                          c * ((1.0 - tx) * ty) + d * (tx * ty));
}

Image resample(const Image& image, std::uint32_t width, std::uint32_t height,
                ResampleFilter filter, ImageLimits limits) {
    Image result(width, height, Color::transparent(), limits);
    if (result.empty()) {
        return result;
    }
    if (image.empty()) {
        throw std::invalid_argument("cannot resample an empty image");
    }
    const double scale_x = static_cast<double>(image.width()) / width;
    const double scale_y = static_cast<double>(image.height()) / height;
    SamplerState sampler;
    sampler.filter = filter == ResampleFilter::Nearest ? FilterMode::Nearest : FilterMode::Bilinear;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (filter != ResampleFilter::Area) {
                const math::Vec2 uv{(static_cast<double>(x) + 0.5) / width,
                                     (static_cast<double>(y) + 0.5) / height};
                result.at(x, y) = sample_image(image, uv, sampler);
                continue;
            }
            const double left = static_cast<double>(x) * scale_x;
            const double top = static_cast<double>(y) * scale_y;
            const double right = std::min(static_cast<double>(x + 1U) * scale_x,
                                           static_cast<double>(image.width()));
            const double bottom = std::min(static_cast<double>(y + 1U) * scale_y,
                                            static_cast<double>(image.height()));
            Color sum = Color::transparent();
            const double area = (right - left) * (bottom - top);
            const auto end_x = static_cast<std::uint32_t>(std::ceil(right));
            const auto end_y = static_cast<std::uint32_t>(std::ceil(bottom));
            for (auto sy = static_cast<std::uint32_t>(std::floor(top)); sy < end_y; ++sy) {
                const double weight_y = std::min(bottom, static_cast<double>(sy) + 1.0) -
                                         std::max(top, static_cast<double>(sy));
                for (auto sx = static_cast<std::uint32_t>(std::floor(left)); sx < end_x; ++sx) {
                    const double weight_x = std::min(right, static_cast<double>(sx) + 1.0) -
                                             std::max(left, static_cast<double>(sx));
                    sum = sum + premultiplied(image.at(sx, sy)) * (weight_x * weight_y / area);
                }
            }
            result.at(x, y) = unpremultiplied(sum);
        }
    }
    return result;
}

Framebuffer::Framebuffer(ImageLimits limits) : color_(limits) {}

Framebuffer::Framebuffer(std::uint32_t width, std::uint32_t height, bool depth_enabled,
                          ImageLimits limits)
    : color_(width, height, Color::transparent(), limits), depth_enabled_(depth_enabled),
      depth_width_(width), depth_height_(height) {
    if (depth_enabled_) {
        depth_.assign(color_.size(), 1.0);
    }
}

bool Framebuffer::resize(std::uint32_t width, std::uint32_t height, bool depth_enabled) {
    try {
        Framebuffer replacement(width, height, depth_enabled, color_.limits());
        color_.swap(replacement.color_);
        depth_.swap(replacement.depth_);
        std::swap(depth_enabled_, replacement.depth_enabled_);
        std::swap(depth_width_, replacement.depth_width_);
        std::swap(depth_height_, replacement.depth_height_);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

std::uint32_t Framebuffer::width() const noexcept { return color_.width(); }
std::uint32_t Framebuffer::height() const noexcept { return color_.height(); }
bool Framebuffer::has_depth() const noexcept { return depth_enabled_ && valid(); }
bool Framebuffer::valid() const noexcept {
    return depth_enabled_ ? depth_.size() == color_.size() && depth_width_ == color_.width() &&
                            depth_height_ == color_.height() : depth_.empty();
}
std::size_t Framebuffer::byte_size() const noexcept {
    const auto depth_bytes = depth_.size() * sizeof(double);
    const auto room = std::numeric_limits<std::size_t>::max() - color_.byte_size();
    return color_.byte_size() + std::min(room, depth_bytes);
}
Image& Framebuffer::color() noexcept { return color_; }
const Image& Framebuffer::color() const noexcept { return color_; }
std::span<double> Framebuffer::depth() noexcept { return depth_; }
std::span<const double> Framebuffer::depth() const noexcept { return depth_; }

std::optional<double> Framebuffer::depth_at(std::int32_t column, std::int32_t row_index) const noexcept {
    if (!has_depth() || !color_.contains(column, row_index)) {
        return std::nullopt;
    }
    return depth_[static_cast<std::size_t>(row_index) * width() + static_cast<std::size_t>(column)];
}

void Framebuffer::clear(Color value, double depth_value) noexcept {
    color_.clear(value);
    clear_depth(depth_value);
}

void Framebuffer::clear_depth(double value) noexcept {
    const double depth_value = std::isfinite(value) ? unit(value) : 1.0;
    std::fill(depth_.begin(), depth_.end(), depth_value);
}

std::size_t Framebuffer::clear_region(Rect rectangle, Color value, double depth_value) noexcept {
    if (!valid()) {
        return 0;
    }
    rectangle = intersection(rectangle, color_.bounds());
    const auto changed = color_.fill_rect(rectangle, value);
    if (!depth_enabled_) {
        return changed;
    }
    depth_value = std::isfinite(depth_value) ? unit(depth_value) : 1.0;
    for (std::int32_t y = 0; y < rectangle.height; ++y) {
        const auto offset = static_cast<std::size_t>(rectangle.y + y) * width() +
                            static_cast<std::size_t>(rectangle.x);
        for (std::int32_t x = 0; x < rectangle.width; ++x) {
            depth_[offset + static_cast<std::size_t>(x)] = depth_value;
        }
    }
    return changed;
}

bool Framebuffer::write(std::int32_t column, std::int32_t row_index, double depth_value,
                        Color value, DepthState state, BlendMode mode) noexcept {
    if (!valid() || !color_.contains(column, row_index) || !finite(value) ||
        !std::isfinite(depth_value) || depth_value < 0.0 || depth_value > 1.0) {
        return false;
    }
    const auto offset = static_cast<std::size_t>(row_index) * width() + static_cast<std::size_t>(column);
    if (depth_enabled_) {
        const double stored = std::isfinite(depth_[offset]) ? unit(depth_[offset]) : 1.0;
        if (state.test_enabled && !depth_passes(depth_value, stored, state.compare)) {
            return false;
        }
        if (state.write_enabled) {
            depth_[offset] = depth_value;
        }
    }
    return color_.set_pixel(column, row_index, value, mode);
}

Texture::Texture(Image base, bool generate_mips)
    : levels_(make_levels(std::move(base), generate_mips)) {}

bool Texture::reset(Image base, bool generate_mips) {
    try {
        auto replacement = make_levels(std::move(base), generate_mips);
        levels_.swap(replacement);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

bool Texture::empty() const noexcept { return levels_.empty(); }
std::uint32_t Texture::width() const noexcept { return empty() ? 0 : levels_.front().width(); }
std::uint32_t Texture::height() const noexcept { return empty() ? 0 : levels_.front().height(); }
std::size_t Texture::level_count() const noexcept { return levels_.size(); }

std::size_t Texture::byte_size() const noexcept {
    std::size_t result{};
    for (const auto& image : levels_) {
        result += image.byte_size();
    }
    return result;
}

const Image* Texture::level(std::size_t index) const noexcept {
    return index < levels_.size() ? &levels_[index] : nullptr;
}

Color Texture::sample(math::Vec2 uv, double lod, const SamplerState& sampler) const noexcept {
    if (empty()) {
        return sanitize(sampler.border);
    }
    const double largest = static_cast<double>(levels_.size() - 1);
    const double minimum = std::clamp(clean(sampler.min_lod), 0.0, largest);
    const double maximum = std::clamp(clean(sampler.max_lod), minimum, largest);
    const double selected = std::clamp(clean(lod) + clean(sampler.lod_bias), minimum, maximum);
    if (sampler.filter != FilterMode::Trilinear) {
        const auto index = static_cast<std::size_t>(std::floor(selected + 0.5));
        return sample_image(levels_[index], uv, sampler);
    }
    const auto first = static_cast<std::size_t>(std::floor(selected));
    const auto second = std::min(first + 1, levels_.size() - 1);
    return interpolate(sample_image(levels_[first], uv, sampler),
                        sample_image(levels_[second], uv, sampler), selected - std::floor(selected));
}

Color Texture::sample_grad(math::Vec2 uv, math::Vec2 uv_dx, math::Vec2 uv_dy,
                           const SamplerState& sampler) const noexcept {
    if (empty()) {
        return sanitize(sampler.border);
    }
    if (!math::finite(uv_dx) || !math::finite(uv_dy)) {
        return sample(uv, static_cast<double>(level_count() - 1), sampler);
    }
    const double dx = std::hypot(uv_dx.x * width(), uv_dx.y * height());
    const double dy = std::hypot(uv_dy.x * width(), uv_dy.y * height());
    const double footprint = std::max(dx, dy);
    const double lod = std::isfinite(footprint) ? std::log2(std::max(1.0, footprint))
                                               : static_cast<double>(level_count() - 1);
    return sample(uv, lod, sampler);
}

TextureHandle::operator bool() const noexcept {
    return owner != 0 && generation != 0 && index != std::numeric_limits<std::uint32_t>::max();
}

TextureLibrary::TextureLibrary(std::size_t capacity, std::size_t byte_budget)
    : owner_(library_owner()), capacity_(capacity), byte_budget_(byte_budget) {
    if (capacity_ > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("texture library capacity exceeds handle index range");
    }
}

std::optional<TextureHandle> TextureLibrary::create(std::string name, Image base, bool generate_mips) {
    collect_retired();
    if (base.empty() || name.empty() || name.size() > 128 || find(name)) {
        return std::nullopt;
    }
    std::size_t estimated{};
    auto width = base.width();
    auto height = base.height();
    for (;;) {
        const auto count = static_cast<std::size_t>(width) * height;
        if (count > (byte_budget_ - allocated_bytes_ - estimated) / sizeof(Color)) {
            return std::nullopt;
        }
        estimated += count * sizeof(Color);
        if (!generate_mips || (width == 1 && height == 1)) {
            break;
        }
        width = std::max(std::uint32_t{1}, width / 2U);
        height = std::max(std::uint32_t{1}, height / 2U);
    }
    try {
        return insert(std::move(name), std::make_shared<const Texture>(std::move(base), generate_mips));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

std::optional<TextureHandle> TextureLibrary::insert(std::string name,
                                                    std::shared_ptr<const Texture> texture) {
    collect_retired();
    if (!texture || texture->empty() || name.empty() || name.size() > 128 || find(name) ||
        texture->byte_size() > byte_budget_ - allocated_bytes_) {
        return std::nullopt;
    }
    std::size_t index = slots_.size();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].texture.get() == texture.get()) {
            return std::nullopt;
        }
        if (!slots_[i].texture && slots_[i].generation < std::numeric_limits<std::uint32_t>::max() &&
            index == slots_.size()) {
            index = i;
        }
    }
    if (index == slots_.size()) {
        if (slots_.size() >= capacity_) {
            return std::nullopt;
        }
        try {
            slots_.emplace_back();
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        } catch (const std::length_error&) {
            return std::nullopt;
        }
    }
    auto& slot = slots_[index];
    slot.name = std::move(name);
    slot.texture = std::move(texture);
    slot.charged_bytes = slot.texture->byte_size();
    slot.active = true;
    allocated_bytes_ += slot.charged_bytes;
    ++active_count_;
    return TextureHandle{owner_, static_cast<std::uint32_t>(index), slot.generation};
}

std::optional<TextureHandle> TextureLibrary::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].active && slots_[i].name == name) {
            return TextureHandle{owner_, static_cast<std::uint32_t>(i), slots_[i].generation};
        }
    }
    return std::nullopt;
}

const TextureLibrary::Slot* TextureLibrary::resolve(TextureHandle handle) const noexcept {
    if (handle.owner != owner_ || handle.index >= slots_.size()) {
        return nullptr;
    }
    const auto& slot = slots_[handle.index];
    return slot.active && slot.generation == handle.generation ? &slot : nullptr;
}

const Texture* TextureLibrary::get(TextureHandle handle) const noexcept {
    const auto* slot = resolve(handle);
    return slot ? slot->texture.get() : nullptr;
}

std::shared_ptr<const Texture> TextureLibrary::acquire(TextureHandle handle) const noexcept {
    const auto* slot = resolve(handle);
    return slot ? slot->texture : std::shared_ptr<const Texture>{};
}

bool TextureLibrary::erase(TextureHandle handle) noexcept {
    if (!resolve(handle)) {
        return false;
    }
    auto& slot = slots_[handle.index];
    slot.active = false;
    slot.name.clear();
    if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
        ++slot.generation;
    }
    --active_count_;
    collect_retired();
    return true;
}

void TextureLibrary::clear() noexcept {
    for (auto& slot : slots_) {
        if (slot.active) {
            slot.active = false;
            slot.name.clear();
            if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
                ++slot.generation;
            }
        }
    }
    active_count_ = 0;
    collect_retired();
}

void TextureLibrary::collect_retired() noexcept {
    for (auto& slot : slots_) {
        if (!slot.active && slot.texture && slot.texture.use_count() == 1) {
            allocated_bytes_ -= slot.charged_bytes;
            slot.charged_bytes = 0;
            slot.texture.reset();
        }
    }
}

std::size_t TextureLibrary::size() const noexcept { return active_count_; }
std::size_t TextureLibrary::allocated_bytes() const noexcept { return allocated_bytes_; }
std::size_t TextureLibrary::byte_budget() const noexcept { return byte_budget_; }

std::vector<std::byte> encode_ppm(const Image& image, const PpmOptions& options) {
    if (image.empty()) {
        throw std::invalid_argument("cannot export an empty image");
    }
    const std::string header = "P6\n" + std::to_string(image.width()) + " " +
                               std::to_string(image.height()) + "\n255\n";
    if (image.size() > (std::numeric_limits<std::size_t>::max() - header.size()) / 3U) {
        throw std::length_error("PPM output length overflows address space");
    }
    std::vector<std::byte> result(header.size() + image.size() * 3U);
    for (std::size_t i = 0; i < header.size(); ++i) {
        result[i] = static_cast<std::byte>(static_cast<unsigned char>(header[i]));
    }
    Color background = sanitize(options.background);
    background.a = 1.0;
    std::size_t cursor = header.size();
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        const auto source_y = options.flip_y ? image.height() - 1U - y : y;
        for (const Color pixel : image.row(source_y)) {
            const auto packed = pack_rgba8(blend(pixel, background), options.encoding);
            result[cursor++] = static_cast<std::byte>(packed & 255U);
            result[cursor++] = static_cast<std::byte>((packed >> 8U) & 255U);
            result[cursor++] = static_cast<std::byte>((packed >> 16U) & 255U);
        }
    }
    return result;
}

bool write_ppm(std::ostream& stream, const Image& image, const PpmOptions& options) {
    try {
        const auto encoded = encode_ppm(image, options);
        std::size_t offset{};
        constexpr std::size_t chunk_size = 4096;
        while (offset < encoded.size()) {
            const auto count = std::min(chunk_size, encoded.size() - offset);
            stream.write(reinterpret_cast<const char*>(encoded.data() + offset),
                          static_cast<std::streamsize>(count));
            if (!stream) {
                return false;
            }
            offset += count;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Framebuffer& ImageService::framebuffer() noexcept { return framebuffer_; }
const Framebuffer& ImageService::framebuffer() const noexcept { return framebuffer_; }
TextureLibrary& ImageService::textures() noexcept { return textures_; }
const TextureLibrary& ImageService::textures() const noexcept { return textures_; }

bool ImageService::export_ppm(const PpmOptions& options) {
    try {
        auto encoded = encode_ppm(framebuffer_.color(), options);
        export_.swap(encoded);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::span<const std::byte> ImageService::last_export() const noexcept { return export_; }

void register_image(core::Kernel& kernel) {
    if (!kernel.register_module({"render.image", "Linear images, depth buffers and mipmapped textures", 1})) {
        return;
    }
    auto& service = kernel.services().emplace<ImageService>();
    const auto width = kernel.config().integer("image.width", 640);
    const auto height = kernel.config().integer("image.height", 360);
    if (width < 1 || height < 1 || width > 16384 || height > 16384 ||
        !service.framebuffer().resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height))) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "render.image", "Initial framebuffer dimensions rejected");
    }
    kernel.add_command("image.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) {
            return {false, "usage: image.create width height"};
        }
        const auto new_width = number<std::uint32_t>(args[0]);
        const auto new_height = number<std::uint32_t>(args[1]);
        if (!new_width || !new_height || *new_width == 0 || *new_height == 0) {
            return {false, "dimensions must be positive unsigned integers"};
        }
        auto& target = host.services().get<ImageService>().framebuffer();
        return target.resize(*new_width, *new_height) ? core::CommandResult{true, "framebuffer created"}
                                                     : core::CommandResult{false, "image dimensions or allocation rejected"};
    }, "Create a transparent image with normalized depth storage");
    kernel.add_command("image.clear", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4 && args.size() != 5) {
            return {false, "usage: image.clear r g b a [depth]"};
        }
        const auto color = command_color(args, 0);
        const auto depth_value = args.size() == 5 ? number<double>(args[4]) : std::optional<double>{1.0};
        if (!color || !depth_value || *depth_value < 0.0 || *depth_value > 1.0) {
            return {false, "finite colors and alpha/depth in [0,1] required"};
        }
        host.services().get<ImageService>().framebuffer().clear(*color, *depth_value);
        return {true, "color and depth cleared"};
    }, "Clear the shared framebuffer using linear RGBA");
    kernel.add_command("image.pixel", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2 && args.size() != 6) {
            return {false, "usage: image.pixel x y [r g b a]"};
        }
        const auto x = number<std::int32_t>(args[0]);
        const auto y = number<std::int32_t>(args[1]);
        auto& target = host.services().get<ImageService>().framebuffer().color();
        if (!x || !y || !target.contains(*x, *y)) {
            return {false, "pixel coordinates outside the image"};
        }
        if (args.size() == 6) {
            const auto value = command_color(args, 2);
            if (!value) {
                return {false, "finite RGBA with alpha in [0,1] required"};
            }
            static_cast<void>(target.set_pixel(*x, *y, *value));
        }
        return {true, describe(*target.pixel(*x, *y))};
    }, "Read or replace one pixel using top-left integer coordinates");
    kernel.add_command("image.sample", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 4) {
            return {false, "usage: image.sample u v [texture-name [lod]]"};
        }
        const auto u = number<double>(args[0]);
        const auto v = number<double>(args[1]);
        if (!u || !v) {
            return {false, "finite UV coordinates required"};
        }
        auto& images = host.services().get<ImageService>();
        if (args.size() == 2) {
            if (images.framebuffer().color().empty()) {
                return {false, "framebuffer is empty"};
            }
            return {true, describe(sample_image(images.framebuffer().color(), {*u, *v}))};
        }
        const auto handle = images.textures().find(args[2]);
        const auto lod = args.size() == 4 ? number<double>(args[3]) : std::optional<double>{0.0};
        if (!handle || !lod) {
            return {false, "unknown texture or invalid level of detail"};
        }
        SamplerState sampler;
        sampler.filter = FilterMode::Trilinear;
        return {true, describe(images.textures().get(*handle)->sample({*u, *v}, *lod, sampler))};
    }, "Sample the framebuffer or a named mipmapped texture");
    kernel.add_command("image.texture", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) {
            return {false, "usage: image.texture name"};
        }
        auto& images = host.services().get<ImageService>();
        const auto handle = images.textures().create(args[0], images.framebuffer().color());
        return handle ? core::CommandResult{true, "immutable mipmapped texture created"}
                       : core::CommandResult{false, "texture name, capacity or memory budget rejected"};
    }, "Snapshot the shared image into the bounded texture library");
    kernel.add_command("image.texture.release", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) {
            return {false, "usage: image.texture.release name"};
        }
        auto& textures = host.services().get<ImageService>().textures();
        const auto handle = textures.find(args[0]);
        return handle && textures.erase(*handle) ? core::CommandResult{true, "texture retired"}
                                                 : core::CommandResult{false, "texture does not exist"};
    }, "Invalidate a texture handle; pinned storage stays charged until released");
    kernel.add_command("image.export", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() > 1 || (!args.empty() && args[0] != "srgb" && args[0] != "linear")) {
            return {false, "usage: image.export [srgb|linear]"};
        }
        PpmOptions options;
        options.encoding = !args.empty() && args[0] == "linear" ? ColorEncoding::Linear : ColorEncoding::Srgb;
        auto& images = host.services().get<ImageService>();
        if (!images.export_ppm(options)) {
            return {false, "PPM encoding failed; previous export retained"};
        }
        return {true, "PPM bytes available in ImageService::last_export(): " + std::to_string(images.last_export().size())};
    }, "Encode an in-memory binary PPM export");
    kernel.add_system({"render.image.metrics", core::Phase::EndFrame, 900, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& images = host.services().get<ImageService>();
            images.textures().collect_retired();
            host.metric("image.pixels", static_cast<double>(images.framebuffer().color().size()));
            host.metric("image.framebuffer_bytes", static_cast<double>(images.framebuffer().byte_size()));
            host.metric("image.textures", static_cast<double>(images.textures().size()));
            host.metric("image.texture_bytes", static_cast<double>(images.textures().allocated_bytes()));
            host.metric("image.export_bytes", static_cast<double>(images.last_export().size()));
        });
}

} // namespace ge::render
