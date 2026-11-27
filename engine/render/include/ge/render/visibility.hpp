#pragma once

#include <ge/render/graph.hpp>
#include <ge/render/rasterizer.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }
namespace ge::spatial { class SpatialIndex; }

namespace ge::render {

class LightingService;
inline constexpr std::uint64_t visibility_callback_owner = 0x4745564953494231ULL;

enum class LodMetric : std::uint8_t { Distance, ProjectedDiameter };
enum class MissingLod : std::uint8_t { BaseMesh, Reject };
enum class VisibilityError : std::uint8_t {
    None,
    InvalidLimits,
    InvalidSettings,
    InvalidEntity,
    InvalidComponent,
    InvalidView,
    MissingService,
    SceneRejected,
    EntityLimit,
    HierarchyLimit,
    ViewLimit,
    DrawLimit,
    TriangleLimit,
    Incomplete,
    AllocationFailure,
    BuilderConflict,
    BuilderRejected,
    GraphConflict,
    GraphRejected,
    RasterRejected,
    Busy,
    Stopped
};

[[nodiscard]] std::string_view describe(VisibilityError error) noexcept;

// The MeshComponent is level zero. Each subsequent level begins at threshold:
// ascending metres for Distance, descending screen pixels for ProjectedDiameter.
// Mesh handles are checked against the shared MeshLibrary on every collection.
struct VisibilityLod {
    geometry::MeshHandle mesh{};
    std::uint32_t first_submesh{};
    std::uint32_t submesh_count{};
    double threshold{25.0};
};

struct VisibilityComponent {
    bool enabled{true};
    bool frustum_culling{true};
    std::uint64_t view_mask{~std::uint64_t{0}};
    std::uint64_t light_mask{~std::uint64_t{0}};
    double minimum_distance{};
    double maximum_distance{1.0e12};
    double minimum_projected_diameter{};
    double lod_bias{1.0};
    double hysteresis{0.1};
    LodMetric metric{LodMetric::Distance};
    MissingLod missing_lod{MissingLod::BaseMesh};
    std::optional<std::size_t> forced_lod;
    std::vector<VisibilityLod> levels;
};

struct VisibilityLimits {
    std::size_t scene_entities{65536};
    std::size_t hierarchy_depth{256};
    std::size_t views{8};
    std::size_t draws_per_view{4096};
    std::size_t total_draws{32768};
    std::size_t query_hits{65536};
    std::size_t query_visits{1048576};
    std::size_t lod_levels{8};
    std::size_t history_entries{262144};
    std::uint64_t history_frames{120};
};

struct VisibilitySettings {
    bool enabled{true};
    bool use_spatial_index{true};
    bool render_incomplete{};
    bool refresh_camera_matrices{true};
    double distance_scale{1.0};
    double projected_size_scale{1.0};
};

[[nodiscard]] bool valid(const VisibilityLimits& limits) noexcept;
[[nodiscard]] bool valid(const VisibilitySettings& settings) noexcept;
[[nodiscard]] bool valid(const VisibilityComponent& component,
                         const VisibilityLimits& limits = {}) noexcept;

struct VisibilityQuery {
    std::size_t visited{};
    std::size_t matched{};
    std::size_t written{};
    std::size_t direct_tests{};
    bool used_index{};
    bool truncated{};
    bool stale{};
    bool invalid{};
    bool recovered_by_direct_tests{};
};

// Entries borrow immutable library objects only until the next collection or
// resource mutation. A builder runs synchronously before any raster call and
// must not mutate Scene, Registry, resources, camera state or the renderer.
struct VisibilityEntry {
    RasterDraw draw{};
    geometry::MeshHandle mesh{};
    MaterialHandle material{};
    std::uint64_t mesh_revision{};
    std::size_t lod{};
    std::size_t ordinal{};
    std::uint32_t material_slot{};
    math::Aabb world_bounds{};
    double distance{};
    double projected_diameter{};
};

struct VisibilityView {
    CameraView camera{};
    std::vector<VisibilityEntry> entries;
    VisibilityQuery query{};
    std::size_t considered{};
    std::size_t culled{};
    std::size_t uncertain{};
    bool complete{true};
};

struct VisibilityBuildContext {
    const scene::Scene& scene;
    const geometry::MeshLibrary& meshes;
    const MaterialService& materials;
    const VisibilityView& view;
    const RasterizerSettings& raster_settings;
    const RasterizerLimits& raster_limits;
    std::size_t maximum_draws{};
};

// Storage owns all temporary resources used by a submission. Original library
// borrows remain stable because collection/build/draw are one serial operation.
// Each view is submitted exactly once; builders never invoke the renderer.
struct VisibilitySubmission {
    std::vector<RasterDraw> draws;
    std::vector<std::shared_ptr<const geometry::MeshData>> owned_meshes;
    std::vector<std::shared_ptr<const PbrMaterial>> owned_materials;
};

struct VisibilityBuildResult {
    bool success{true};
    bool complete{true};
    bool uncertain{};
};

class VisibilitySubmissionBuilder {
public:
    virtual ~VisibilitySubmissionBuilder() = default;
    [[nodiscard]] virtual VisibilityBuildResult build(const VisibilityBuildContext& context,
                                                       VisibilitySubmission& output) = 0;
};

struct VisibilityStatistics {
    std::uint64_t collections{};
    std::uint64_t scene_revision{};
    std::size_t views{};
    std::size_t examined{};
    std::size_t disabled{};
    std::size_t invalid{};
    std::size_t layer_culled{};
    std::size_t frustum_culled{};
    std::size_t distance_culled{};
    std::size_t size_culled{};
    std::size_t selected_draws{};
    std::size_t submitted_draws{};
    std::size_t lod_changes{};
    std::size_t lod_fallbacks{};
    std::size_t history_dropped{};
    std::size_t refreshed_cameras{};
    std::size_t query_fallbacks{};
    std::size_t uncertain{};
    std::size_t input_triangles{};
    std::size_t rendered_draws{};
    std::size_t fragment_visits{};
    std::size_t fragments_written{};
    std::size_t cleared_pixels{};
    bool complete{true};
    bool framebuffer_changed{};
    VisibilityError error{VisibilityError::None};
    RasterError raster_error{RasterError::None};
};

struct VisibilityResult {
    VisibilityError error{VisibilityError::None};
    bool complete{true};
    bool uncertain{};
    bool framebuffer_changed{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == VisibilityError::None && complete;
    }
};

// Owned provenance from a successful scheduled scene.raster submission. Consumers
// must match tick, callback stamp, raster serial and target identity/dimensions.
// Manual renderer calls are not published here and advance the raster serial.
struct VisibilityRenderedFrame {
    std::uint64_t tick{};
    GraphPass pass{};
    std::uint64_t callback_owner{};
    std::uint64_t callback_revision{};
    std::uint64_t raster_frame{};
    const Framebuffer* target{};
    std::uint32_t width{};
    std::uint32_t height{};
    RasterizerSettings raster_settings{};
    std::vector<CameraView> views;
};

// Single-writer, borrowing Scene. The destructor never touches borrowed services.
// capture_camera_bases belongs immediately after camera.update and before late
// interaction writes. Refresh preserves sampled shake/recoil, but does not run
// follow controllers a second time. collect/render refresh Scene and SpatialIndex.
class VisibilityService final {
public:
    explicit VisibilityService(scene::Scene& scene, VisibilityLimits limits = {});
    ~VisibilityService();
    VisibilityService(const VisibilityService&) = delete;
    VisibilityService& operator=(const VisibilityService&) = delete;
    VisibilityService(VisibilityService&&) = delete;
    VisibilityService& operator=(VisibilityService&&) = delete;
    bool configure(const VisibilitySettings& settings) noexcept;
    [[nodiscard]] const VisibilitySettings& settings() const noexcept;
    [[nodiscard]] const VisibilityLimits& limits() const noexcept;
    [[nodiscard]] const VisibilityStatistics& statistics() const noexcept;
    [[nodiscard]] std::optional<VisibilityRenderedFrame> rendered_frame() const;
    [[nodiscard]] std::span<const VisibilityView> views() const noexcept;
    [[nodiscard]] const VisibilityView* view(std::size_t slot) const noexcept;
    bool attach(ecs::Entity entity, const VisibilityComponent& component = {});
    bool detach(ecs::Entity entity);
    [[nodiscard]] const VisibilityComponent* component(ecs::Entity entity) const noexcept;
    void reset_history() noexcept;
    void capture_camera_bases(const CameraService& cameras);
    [[nodiscard]] VisibilityResult collect(const geometry::MeshLibrary& meshes,
                                           const CameraService& cameras,
                                           const MaterialService& materials,
                                           const SoftwareRenderer& renderer,
                                           spatial::SpatialIndex* spatial = nullptr);
    [[nodiscard]] VisibilityResult render(const geometry::MeshLibrary& meshes,
                                          const CameraService& cameras,
                                          const MaterialService& materials,
                                          SoftwareRenderer& renderer,
                                          ImageService& images,
                                          spatial::SpatialIndex* spatial = nullptr,
                                          const LightingService* lighting = nullptr);
    // One named builder owns the extension. Replacement requires the same owner;
    // release requires that owner. Shared ownership survives synchronous rendering.
    bool set_submission_builder(std::string_view owner,
                                std::shared_ptr<VisibilitySubmissionBuilder> builder);
    bool clear_submission_builder(std::string_view owner) noexcept;
    [[nodiscard]] std::string_view submission_builder_owner() const noexcept;
    bool install_graph(core::Kernel& kernel);
    void shutdown() noexcept;

private:
    friend void register_visibility(core::Kernel& kernel);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

void register_visibility(core::Kernel& kernel);

} // namespace ge::render
