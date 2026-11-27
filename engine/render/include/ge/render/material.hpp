#pragma once

#include <ge/core/identity.hpp>
#include <ge/render/image.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::ecs { class Registry; }

namespace ge::render {

inline constexpr double material_hdr_limit = 1.0e12;
inline constexpr std::size_t material_light_limit = 4096;

enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };
enum class ShadingModel : std::uint8_t { MetallicRoughness, Lambert, Unlit };
enum class TextureChannel : std::uint8_t { Red, Green, Blue, Alpha };

// Images already contain linear color. Data maps must also be stored as linear
// values; decoding sRGB belongs to image import, before texture filtering.
struct MaterialTexture {
    TextureHandle texture{};
    SamplerState sampler{};
    math::Vec2 scale{1.0, 1.0};
    math::Vec2 offset{};
    double rotation{};
    TextureChannel channel{TextureChannel::Red};
};

struct PbrMaterial {
    Color albedo{Color::white()};
    Color emissive{Color::black()};
    double metalness{};
    double roughness{0.5};
    double dielectric_f0{0.04};
    double emissive_strength{1.0};
    double normal_scale{1.0};
    double occlusion_strength{1.0};
    double alpha_cutoff{0.5};
    AlphaMode alpha_mode{AlphaMode::Opaque};
    ShadingModel shading_model{ShadingModel::MetallicRoughness};
    bool double_sided{};
    bool use_vertex_color{true};
    MaterialTexture albedo_texture{};
    MaterialTexture metalness_texture{};
    MaterialTexture roughness_texture{};
    MaterialTexture normal_texture{};
    MaterialTexture emissive_texture{};
    MaterialTexture occlusion_texture{};
};

using Material = PbrMaterial;

struct MaterialValidation {
    bool valid{true};
    std::string_view field{};
    std::string_view message{};
    [[nodiscard]] explicit operator bool() const noexcept { return valid; }
};

[[nodiscard]] MaterialValidation validate_material(const PbrMaterial& material) noexcept;
[[nodiscard]] MaterialValidation validate_texture_binding(const MaterialTexture& binding) noexcept;

struct MaterialHandle {
    std::uint64_t owner{};
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return owner != 0 && index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const MaterialHandle&) const = default;
};

// A SubmeshRange::material_slot indexes slots. Missing/stale entries select the
// service fallback. This component accompanies geometry::MeshComponent.
struct MaterialComponent {
    std::vector<MaterialHandle> slots;
    [[nodiscard]] MaterialHandle at(std::uint32_t slot) const noexcept;
};

struct MaterialLimits {
    std::uint32_t capacity{4096};
    std::uint32_t slots_per_entity{256};
    std::size_t lights{256};
};

struct MaterialInfo {
    MaterialHandle handle{};
    std::string_view name{};
    std::uint64_t revision{};
    const PbrMaterial* material{};
};

// Single-writer owner. Read-only borrows survive until replace/erase/clear of
// that material. Handles are generation checked and scoped to this instance.
class MaterialLibrary final {
public:
    explicit MaterialLibrary(MaterialLimits limits = {});
    ~MaterialLibrary();
    MaterialLibrary(const MaterialLibrary&) = delete;
    MaterialLibrary& operator=(const MaterialLibrary&) = delete;
    MaterialLibrary(MaterialLibrary&&) = delete;
    MaterialLibrary& operator=(MaterialLibrary&&) = delete;
    [[nodiscard]] MaterialHandle create(std::string_view name, const PbrMaterial& material = {});
    bool replace(MaterialHandle handle, const PbrMaterial& material);
    bool erase(MaterialHandle handle) noexcept;
    void clear() noexcept;
    [[nodiscard]] MaterialHandle find(std::string_view name) const noexcept;
    [[nodiscard]] const PbrMaterial* get(MaterialHandle handle) const noexcept;
    [[nodiscard]] std::optional<MaterialInfo> inspect(MaterialHandle handle) const noexcept;
    [[nodiscard]] std::vector<MaterialHandle> handles() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const MaterialLimits& limits() const noexcept;
    bool assign(ecs::Registry& registry, ge::RuntimeHandle entity,
                MaterialHandle material, std::uint32_t slot = 0) const;
    bool unassign(ecs::Registry& registry, ge::RuntimeHandle entity,
                  std::uint32_t slot) const;
    [[nodiscard]] MaterialHandle assigned(const ecs::Registry& registry,
                                           ge::RuntimeHandle entity,
                                           std::uint32_t slot = 0) const noexcept;

private:
    struct Record;
    [[nodiscard]] bool owns(MaterialHandle handle) const noexcept;
    MaterialLimits limits_;
    std::uint64_t owner_;
    ge::GenerationPool generations_;
    std::vector<std::unique_ptr<Record>> records_;
    std::uint64_t revision_{};
};

// Rasterizers supply interpolated world-space attributes. UVs and derivatives
// retain the image system's top-left origin. Tangent.w selects handedness by
// sign; zero uses +1, allowing interpolation without an exact +/-1 requirement.
struct SurfaceSample {
    math::Vec3 world_position{};
    math::Vec3 world_normal{0.0, 1.0, 0.0};
    math::Vec4 world_tangent{1.0, 0.0, 0.0, 1.0};
    math::Vec2 uv{};
    math::Vec2 uv_dx{};
    math::Vec2 uv_dy{};
    Color vertex_color{Color::white()};
    double lod{};
    bool front_facing{true};
    bool gradients_valid{};
};

struct MaterialSample {
    math::Vec3 world_position{};
    math::Vec3 geometric_normal{0.0, 1.0, 0.0};
    math::Vec3 normal{0.0, 1.0, 0.0};
    Color albedo{Color::white()};
    Color emissive{Color::black()};
    double metalness{};
    double roughness{0.5};
    double dielectric_f0{0.04};
    double occlusion{1.0};
    double opacity{1.0};
    AlphaMode alpha_mode{AlphaMode::Opaque};
    ShadingModel shading_model{ShadingModel::MetallicRoughness};
    std::uint8_t missing_textures{};
    bool discarded{true};
    bool valid{};
};

enum class LightType : std::uint8_t { Directional, Point, Spot };

// Shared with lighting and rasterization. Direction points in the direction
// light travels. Directional intensity is irradiance; local light intensity is
// radiant intensity with inverse-square attenuation. Cone angles are radians.
struct Light {
    LightType type{LightType::Directional};
    math::Vec3 position{};
    math::Vec3 direction{0.0, -1.0, 0.0};
    Color color{Color::white()};
    double intensity{1.0};
    double range{10.0};
    double radius{0.01};
    double inner_cone{math::radians(20.0)};
    double outer_cone{math::radians(30.0)};
    std::uint64_t layers{std::numeric_limits<std::uint64_t>::max()};
    bool enabled{true};
    bool casts_shadow{true};
};

struct LightSample {
    math::Vec3 direction{};
    Color radiance{Color::black()};
    double distance{std::numeric_limits<double>::infinity()};
    bool valid{};
};

[[nodiscard]] MaterialValidation validate_light(const Light& light) noexcept;
[[nodiscard]] LightSample sample_light(const Light& light, math::Vec3 world_position) noexcept;

// Return visibility in [0,1]. The callback is synchronous, must not mutate its
// borrowed inputs, and must not throw. A nonfinite result suppresses that light.
using ShadowFactor = double (*)(const Light&, const MaterialSample&,
                               const LightSample&, const void*) noexcept;

struct ShadingContext {
    // Direction from the surface toward the eye; normalization is performed here.
    math::Vec3 view_direction{0.0, 0.0, 1.0};
    Color ambient{0.03, 0.03, 0.03, 1.0};
    std::span<const Light> lights{};
    ShadowFactor shadow_factor{};
    const void* shadow_user{};
    std::uint64_t light_mask{std::numeric_limits<std::uint64_t>::max()};
    std::size_t max_lights{256};
};

struct BrdfSample {
    Color diffuse{Color::black()};
    Color specular{Color::black()};
};

struct ShadingResult {
    Color color{Color::transparent()};
    std::size_t lights_evaluated{};
    std::uint8_t missing_textures{};
    bool discarded{true};
    bool valid{};
};

[[nodiscard]] MaterialSample sample_material(const PbrMaterial& material,
                                             const SurfaceSample& surface,
                                             const TextureLibrary* textures = nullptr) noexcept;
[[nodiscard]] BrdfSample evaluate_brdf(const MaterialSample& surface,
                                      math::Vec3 view_direction,
                                      math::Vec3 light_direction) noexcept;
[[nodiscard]] ShadingResult shade_surface(const MaterialSample& surface,
                                          const ShadingContext& context) noexcept;
[[nodiscard]] ShadingResult shade_material(const PbrMaterial& material,
                                           const SurfaceSample& surface,
                                           const ShadingContext& context,
                                           const TextureLibrary* textures = nullptr) noexcept;

// Light spans are borrowed until the next light mutation. Readers must finish
// shading before a writer changes materials, textures, components or lights.
class MaterialService final {
public:
    explicit MaterialService(MaterialLimits limits = {});
    [[nodiscard]] MaterialLibrary& materials() noexcept;
    [[nodiscard]] const MaterialLibrary& materials() const noexcept;
    [[nodiscard]] std::span<const Light> lights() const noexcept;
    bool set_lights(std::span<const Light> incoming_lights);
    [[nodiscard]] std::optional<std::size_t> add_light(const Light& light);
    bool set_light(std::size_t index, const Light& light) noexcept;
    // Erasure shifts later indices. They are array indices, not persistent IDs.
    bool erase_light(std::size_t index) noexcept;
    void clear_lights() noexcept;
    [[nodiscard]] Color ambient() const noexcept;
    bool set_ambient(Color irradiance) noexcept;
    [[nodiscard]] ShadingContext context(math::Vec3 view_direction) const noexcept;
    [[nodiscard]] const PbrMaterial& resolve(const ecs::Registry& registry,
                                            ge::RuntimeHandle entity,
                                            std::uint32_t slot = 0) const noexcept;
    [[nodiscard]] const PbrMaterial& fallback() const noexcept;
    bool set_fallback(const PbrMaterial& material) noexcept;
    [[nodiscard]] std::uint64_t lighting_revision() const noexcept;

private:
    MaterialLibrary materials_;
    std::vector<Light> lights_;
    PbrMaterial fallback_{};
    Color ambient_{0.03, 0.03, 0.03, 1.0};
    std::uint64_t lighting_revision_{};
};

void register_material(core::Kernel& kernel);

} // namespace ge::render
