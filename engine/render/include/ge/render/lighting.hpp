#pragma once

#include <ge/render/material.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }
namespace ge::geometry { class MeshLibrary; }

namespace ge::render {

class CameraService;
class SoftwareRenderer;
struct CameraView;

// AppendScene preserves the current manually authored list as a prefix. SceneOnly
// temporarily replaces that list and restores it on release. An external list
// edit while either mode owns publication relinquishes ownership to Manual,
// preserving the external edit exactly; ambient-only edits do not conflict.
enum class LightPublication : std::uint8_t { Manual, AppendScene, SceneOnly };
enum class LightingError : std::uint8_t {
    None,
    InvalidConfiguration,
    InvalidLight,
    InvalidEntity,
    SceneRejected,
    Capacity,
    OwnershipLost,
    UnsupportedProjection,
    UnsupportedMaterial,
    RasterRejected,
    WorkLimit,
    AllocationFailure
};

[[nodiscard]] std::string_view describe(LightingError error) noexcept;

struct ShadowSettings {
    std::uint32_t resolution{128};
    std::uint32_t pcf_radius{1};
    double constant_bias{0.0008};
    double slope_bias{0.003};
    double normal_offset{0.005};
    double near_plane{0.05};
    double directional_distance{60.0};
    double directional_padding{5.0};
    double fallback_extent{15.0};
    double strength{1.0};
    std::uint64_t caster_mask{~std::uint64_t{0}};
    bool enabled{true};
};

// Position and travel direction are local to the node. Exact world matrices
// transform both; nonuniform scale does not rescale the authored range/radius.
struct LightComponent {
    Light light{};
    ShadowSettings shadow{};
    bool inherit_entity_layers{};
};

// Optional mesh component; absence means an enabled, two-sided shadow caster.
// Alpha-mask materials are sampled using the shared material/texture APIs.
// Blend materials are skipped because depth maps cannot represent transmission.
struct ShadowCasterComponent {
    bool enabled{true};
    bool two_sided{true};
};

struct LightingLimits {
    std::size_t scene_entities{65536};
    std::size_t hierarchy_depth{256};
    std::size_t lights{256};
    std::size_t shadow_lights{8};
    std::size_t shadow_maps{24};
    std::size_t shadow_pixels{1'048'576};
    std::size_t caster_draws{4096};
    std::size_t triangles_per_map{131072};
    std::size_t fragments_per_map{4'194'304};
    std::size_t maps_per_update{8};
    std::uint32_t maximum_resolution{512};
};

struct LightingSettings {
    LightPublication publication{LightPublication::AppendScene};
    ShadowSettings manual_shadows{};
    bool enabled{true};
    bool shadows{true};
    bool point_shadows{true};
    bool allow_stale_camera{};
};

struct PublishedLight {
    std::size_t slot{};
    ge::RuntimeHandle entity{};
    Light light{};
    ShadowSettings shadow{};
    bool manual{};
};

struct ShadowMapInfo {
    std::size_t light_slot{};
    std::size_t face{};
    std::uint32_t resolution{};
    std::uint64_t source_signature{};
    std::uint64_t built_update{};
    std::size_t caster_draws{};
    bool ready{};
    LightingError error{LightingError::None};
};

struct LightingStatistics {
    std::uint64_t updates{};
    std::uint64_t publication_revision{};
    std::size_t scene_lights{};
    std::size_t manual_lights{};
    std::size_t published_lights{};
    std::size_t rejected_lights{};
    std::size_t truncated_lights{};
    std::size_t caster_draws{};
    std::size_t blended_casters_skipped{};
    std::size_t invalid_casters{};
    std::size_t requested_maps{};
    std::size_t ready_maps{};
    std::size_t rebuilt_maps{};
    std::size_t reused_maps{};
    std::size_t deferred_maps{};
    std::size_t rejected_maps{};
    std::size_t resident_pixels{};
    std::size_t fragment_visits{};
    std::size_t input_triangles{};
    std::size_t unsupported_lights{};
    bool publication_owned{};
    bool partial{};
    LightingError error{LightingError::None};
};

[[nodiscard]] bool valid(const ShadowSettings& settings) noexcept;
[[nodiscard]] bool valid(const LightingLimits& limits) noexcept;
[[nodiscard]] bool valid(const LightingSettings& settings) noexcept;

// Serial service. update() prepares lighting without drawing the main target.
// Maps and published metadata are borrowed until update/configure/shutdown.
// Call after camera.update and spatial.sync, before a raster or render-graph pass.
// Cache keys include exact transforms, mesh revisions and alpha texture identity;
// TextureLibrary's immutable-object contract is required. Directional coverage
// fits the current camera up to directional_distance, with bounded extrusion.
// Other views and receivers outside a map are unshadowed. Cube PCF border taps
// are unshadowed; filtering is bounded to 49 comparisons and is not seamless.
class LightingService final {
public:
    explicit LightingService(scene::Scene& scene, MaterialService& materials,
                              LightingLimits limits = {});
    ~LightingService();
    LightingService(const LightingService&) = delete;
    LightingService& operator=(const LightingService&) = delete;
    LightingService(LightingService&&) = delete;
    LightingService& operator=(LightingService&&) = delete;

    bool configure(const LightingSettings& settings);
    [[nodiscard]] const LightingSettings& settings() const noexcept;
    [[nodiscard]] const LightingLimits& limits() const noexcept;
    [[nodiscard]] const LightingStatistics& statistics() const noexcept;
    [[nodiscard]] LightingError last_error() const noexcept;
    [[nodiscard]] ge::RuntimeHandle create(std::string_view name,
                                          const LightComponent& component = {});
    bool attach(ge::RuntimeHandle entity, const LightComponent& component = {});
    bool detach(ge::RuntimeHandle entity);
    bool set_light(ge::RuntimeHandle entity, const Light& light);
    bool set_shadow(ge::RuntimeHandle entity, const ShadowSettings& settings);
    [[nodiscard]] const LightComponent* component(ge::RuntimeHandle entity) const noexcept;

    // Binding installs this service's callback; shutdown clears it while the
    // renderer and MaterialService still live. The destructor touches neither.
    void bind_renderer(SoftwareRenderer& renderer) noexcept;
    void shutdown() noexcept;
    void invalidate() noexcept;
    bool update(const geometry::MeshLibrary& meshes, const CameraService* cameras = nullptr,
                const TextureLibrary* textures = nullptr);
    [[nodiscard]] std::span<const PublishedLight> published() const noexcept;
    [[nodiscard]] std::span<const ShadowMapInfo> shadow_maps() const noexcept;
    [[nodiscard]] const Framebuffer* shadow_framebuffer(std::size_t map) const noexcept;
    [[nodiscard]] const CameraView* shadow_view(std::size_t map) const noexcept;
    [[nodiscard]] double visibility(std::size_t light_slot, const MaterialSample& surface,
                                    const LightSample& sample) const noexcept;
    [[nodiscard]] static double shadow_factor(const Light& light,
                                              const MaterialSample& surface,
                                              const LightSample& sample,
                                              const void* user) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_lighting(core::Kernel& kernel);

} // namespace ge::render
