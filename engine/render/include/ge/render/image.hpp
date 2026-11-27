#pragma once

#include <ge/math/math.hpp>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::render {

// Linear RGB, straight alpha, and HDR-capable components. Pixels and UVs have
// a top-left origin; storage is contiguous row-major without row padding.
struct Color {
    double r{};
    double g{};
    double b{};
    double a{1.0};
    [[nodiscard]] constexpr math::Vec4 vector() const noexcept { return {r, g, b, a}; }
    [[nodiscard]] constexpr math::Vec3 rgb() const noexcept { return {r, g, b}; }
    [[nodiscard]] static constexpr Color transparent() noexcept { return {0.0, 0.0, 0.0, 0.0}; }
    [[nodiscard]] static constexpr Color black() noexcept { return {}; }
    [[nodiscard]] static constexpr Color white() noexcept { return {1.0, 1.0, 1.0, 1.0}; }
    [[nodiscard]] static constexpr Color from_vector(math::Vec4 value) noexcept {
        return {value.x, value.y, value.z, value.w};
    }
    constexpr bool operator==(const Color&) const = default;
};

[[nodiscard]] constexpr Color operator+(Color lhs, Color rhs) noexcept {
    return {lhs.r + rhs.r, lhs.g + rhs.g, lhs.b + rhs.b, lhs.a + rhs.a};
}
[[nodiscard]] constexpr Color operator*(Color value, double factor) noexcept {
    return {value.r * factor, value.g * factor, value.b * factor, value.a * factor};
}
[[nodiscard]] constexpr Color operator*(Color lhs, Color rhs) noexcept {
    return {lhs.r * rhs.r, lhs.g * rhs.g, lhs.b * rhs.b, lhs.a * rhs.a};
}
[[nodiscard]] bool finite(Color value) noexcept;
[[nodiscard]] Color sanitize(Color value) noexcept;
[[nodiscard]] Color clamped(Color value) noexcept;
[[nodiscard]] Color premultiplied(Color value) noexcept;
[[nodiscard]] Color unpremultiplied(Color value) noexcept;
[[nodiscard]] Color interpolate(Color first, Color second, double amount) noexcept;
[[nodiscard]] double luminance(Color value) noexcept;
[[nodiscard]] double srgb_to_linear(double value) noexcept;
[[nodiscard]] double linear_to_srgb(double value) noexcept;
[[nodiscard]] Color from_srgb(Color value) noexcept;
[[nodiscard]] Color to_srgb(Color value) noexcept;

enum class ColorEncoding : std::uint8_t { Linear, Srgb };
enum class BlendMode : std::uint8_t { Replace, SourceOver, Add, Multiply };
[[nodiscard]] Color blend(Color source, Color destination,
                          BlendMode mode = BlendMode::SourceOver) noexcept;
// Packed integers use R in bits 0..7 and A in bits 24..31, independent of host endian.
[[nodiscard]] std::uint32_t pack_rgba8(Color value,
                                    ColorEncoding encoding = ColorEncoding::Srgb) noexcept;
[[nodiscard]] Color unpack_rgba8(std::uint32_t value,
                                 ColorEncoding encoding = ColorEncoding::Srgb) noexcept;

struct Rect {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(std::int32_t column, std::int32_t row) const noexcept;
    constexpr bool operator==(const Rect&) const = default;
};

[[nodiscard]] Rect intersection(Rect first, Rect second) noexcept;

struct ImageLimits {
    std::uint32_t max_dimension{16384};
    std::size_t max_pixels{16U * 1024U * 1024U};
};

class Image final {
public:
    explicit Image(ImageLimits limits = {}) noexcept;
    Image(std::uint32_t width, std::uint32_t height,
          Color fill = Color::transparent(), ImageLimits limits = {});
    Image(const Image&) = default;
    Image& operator=(const Image& other);
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    [[nodiscard]] static bool valid_dimensions(std::uint32_t width, std::uint32_t height,
                                               ImageLimits limits = {}) noexcept;
    // Both zero dimensions clear storage. Failure leaves the old image intact.
    bool resize(std::uint32_t width, std::uint32_t height,
                Color fill = Color::transparent());
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t byte_size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] ImageLimits limits() const noexcept;
    [[nodiscard]] Rect bounds() const noexcept;
    [[nodiscard]] bool contains(std::int32_t column, std::int32_t row) const noexcept;
    // Mutable views permit direct raster writes; callers must preserve finite RGBA.
    [[nodiscard]] std::span<Color> pixels() noexcept;
    [[nodiscard]] std::span<const Color> pixels() const noexcept;
    [[nodiscard]] std::span<Color> row(std::uint32_t index);
    [[nodiscard]] std::span<const Color> row(std::uint32_t index) const;
    [[nodiscard]] Color& at(std::uint32_t column, std::uint32_t row);
    [[nodiscard]] const Color& at(std::uint32_t column, std::uint32_t row) const;
    [[nodiscard]] std::optional<Color> pixel(std::int32_t column, std::int32_t row) const noexcept;
    bool set_pixel(std::int32_t column, std::int32_t row, Color value,
                   BlendMode mode = BlendMode::Replace) noexcept;
    void clear(Color value = Color::transparent()) noexcept;
    std::size_t fill_rect(Rect rectangle, Color value,
                          BlendMode mode = BlendMode::Replace) noexcept;
    // Source and destination are clipped together; overlapping self-blits snapshot first.
    std::size_t blit(const Image& source, Rect source_rectangle,
                     std::int32_t destination_x, std::int32_t destination_y,
                     BlendMode mode = BlendMode::Replace);
    [[nodiscard]] Image copy_region(Rect rectangle) const;
    void flip_horizontal() noexcept;
    void flip_vertical() noexcept;
    void swap(Image& other) noexcept;

private:
    ImageLimits limits_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<Color> pixels_;
};

enum class AddressMode : std::uint8_t { Clamp, Repeat, Mirror, Border };
enum class FilterMode : std::uint8_t { Nearest, Bilinear, Trilinear };
enum class ResampleFilter : std::uint8_t { Nearest, Bilinear, Area };

struct SamplerState {
    AddressMode address_u{AddressMode::Clamp};
    AddressMode address_v{AddressMode::Clamp};
    FilterMode filter{FilterMode::Bilinear};
    Color border{Color::transparent()};
    double lod_bias{};
    double min_lod{};
    double max_lod{32.0};
};

// Texel centres lie at ((x + 0.5) / width, (y + 0.5) / height).
// Filtering operates on premultiplied samples and returns straight alpha.
[[nodiscard]] Color sample_image(const Image& image, math::Vec2 uv,
                                  const SamplerState& sampler = {}) noexcept;
[[nodiscard]] Image resample(const Image& image, std::uint32_t width,
                             std::uint32_t height,
                             ResampleFilter filter = ResampleFilter::Bilinear,
                             ImageLimits limits = {});

enum class DepthCompare : std::uint8_t {
    Never, Less, LessEqual, Equal, GreaterEqual, Greater, NotEqual, Always
};

struct DepthState {
    DepthCompare compare{DepthCompare::Less};
    bool test_enabled{true};
    bool write_enabled{true};
};

class Framebuffer final {
public:
    explicit Framebuffer(ImageLimits limits = {});
    Framebuffer(std::uint32_t width, std::uint32_t height, bool depth = true,
                ImageLimits limits = {});
    bool resize(std::uint32_t width, std::uint32_t height, bool depth = true);
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] bool has_depth() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t byte_size() const noexcept;
    // Resize attachments as a unit. Changing color() dimensions independently
    // invalidates a depth attachment; writes then fail safely until resized together.
    [[nodiscard]] Image& color() noexcept;
    [[nodiscard]] const Image& color() const noexcept;
    [[nodiscard]] std::span<double> depth() noexcept;
    [[nodiscard]] std::span<const double> depth() const noexcept;
    [[nodiscard]] std::optional<double> depth_at(std::int32_t column,
                                                std::int32_t row) const noexcept;
    void clear(Color value = Color::transparent(), double depth = 1.0) noexcept;
    void clear_depth(double value = 1.0) noexcept;
    std::size_t clear_region(Rect rectangle, Color value, double depth = 1.0) noexcept;
    // Depth is normalized [0,1]. Nonfinite/out-of-range fragments are rejected.
    bool write(std::int32_t column, std::int32_t row, double depth, Color value,
               DepthState state = {}, BlendMode mode = BlendMode::Replace) noexcept;

private:
    Image color_;
    std::vector<double> depth_;
    bool depth_enabled_{};
    std::uint32_t depth_width_{};
    std::uint32_t depth_height_{};
};

class Texture final {
public:
    Texture() = default;
    explicit Texture(Image base, bool generate_mips = true);
    bool reset(Image base, bool generate_mips = true);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t level_count() const noexcept;
    [[nodiscard]] std::size_t byte_size() const noexcept;
    [[nodiscard]] const Image* level(std::size_t index) const noexcept;
    [[nodiscard]] Color sample(math::Vec2 uv, double lod = 0.0,
                               const SamplerState& sampler = {}) const noexcept;
    [[nodiscard]] Color sample_grad(math::Vec2 uv, math::Vec2 uv_dx, math::Vec2 uv_dy,
                                    const SamplerState& sampler = {}) const noexcept;

private:
    std::vector<Image> levels_;
};

struct TextureHandle {
    std::uint64_t owner{};
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept;
    constexpr bool operator==(const TextureHandle&) const = default;
};

// Serialized owner access. Acquired immutable resources can outlive removal;
// their slots and bytes remain charged until collect_retired sees no borrowers.
class TextureLibrary final {
public:
    explicit TextureLibrary(std::size_t capacity = 1024,
                             std::size_t byte_budget = 256U * 1024U * 1024U);
    TextureLibrary(const TextureLibrary&) = delete;
    TextureLibrary& operator=(const TextureLibrary&) = delete;
    TextureLibrary(TextureLibrary&&) = delete;
    TextureLibrary& operator=(TextureLibrary&&) = delete;
    [[nodiscard]] std::optional<TextureHandle> create(std::string name, Image base,
                                                      bool generate_mips = true);
    // Inserted objects must remain immutable through every external alias.
    // The same object cannot occupy two slots in this library.
    [[nodiscard]] std::optional<TextureHandle> insert(std::string name,
                                                      std::shared_ptr<const Texture> texture);
    [[nodiscard]] std::optional<TextureHandle> find(std::string_view name) const noexcept;
    [[nodiscard]] const Texture* get(TextureHandle handle) const noexcept;
    [[nodiscard]] std::shared_ptr<const Texture> acquire(TextureHandle handle) const noexcept;
    bool erase(TextureHandle handle) noexcept;
    void clear() noexcept;
    void collect_retired() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t allocated_bytes() const noexcept;
    [[nodiscard]] std::size_t byte_budget() const noexcept;

private:
    struct Slot {
        std::string name;
        std::shared_ptr<const Texture> texture;
        std::size_t charged_bytes{};
        std::uint32_t generation{1};
        bool active{};
    };
    [[nodiscard]] const Slot* resolve(TextureHandle handle) const noexcept;
    std::uint64_t owner_;
    std::size_t capacity_;
    std::size_t byte_budget_;
    std::size_t allocated_bytes_{};
    std::size_t active_count_{};
    std::vector<Slot> slots_;
};

struct PpmOptions {
    ColorEncoding encoding{ColorEncoding::Srgb};
    Color background{Color::black()};
    bool flip_y{};
};

[[nodiscard]] std::vector<std::byte> encode_ppm(const Image& image,
                                               const PpmOptions& options = {});
bool write_ppm(std::ostream& stream, const Image& image, const PpmOptions& options = {});

class ImageService final {
public:
    [[nodiscard]] Framebuffer& framebuffer() noexcept;
    [[nodiscard]] const Framebuffer& framebuffer() const noexcept;
    [[nodiscard]] TextureLibrary& textures() noexcept;
    [[nodiscard]] const TextureLibrary& textures() const noexcept;
    bool export_ppm(const PpmOptions& options = {});
    [[nodiscard]] std::span<const std::byte> last_export() const noexcept;

private:
    Framebuffer framebuffer_;
    TextureLibrary textures_;
    std::vector<std::byte> export_;
};

void register_image(core::Kernel& kernel);

} // namespace ge::render
