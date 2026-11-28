#pragma once

#include <ge/render/visibility.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::render {

inline constexpr std::string_view batching_builder_owner = "render.batching";

enum class BatchOrder : std::uint8_t { Preserve, DisjointOpaque };
enum class BatchError : std::uint8_t {
    None,
    InvalidLimits,
    InvalidSettings,
    InvalidAppearance,
    InvalidEntity,
    InvalidDraw,
    StaleSelection,
    InstanceLimit,
    GroupLimit,
    PreparationLimit,
    AllocationFailure,
    BuilderConflict,
    MissingVisibility,
    Busy,
    Stopped
};

[[nodiscard]] std::string_view describe(BatchError error) noexcept;

// Authored controls are independent of vegetation density/LOD. They are applied
// to immutable material copies; the material library is never modified here.
struct BatchAppearanceComponent {
    Color tint{Color::white()};
    double opacity{1.0};
    double emissive_scale{1.0};
    std::optional<double> roughness;
    std::optional<double> metalness;
    std::optional<AlphaMode> alpha_mode;
    std::optional<std::uint32_t> material_slot;
    std::uint64_t group_tag{};
    bool enabled{true};
    bool preserve_order{};
    bool isolate{};
};

struct BatchingLimits {
    std::size_t instances_per_view{4096};
    std::size_t groups_per_view{4096};
    std::size_t instances_per_group{256};
    std::size_t mesh_cache_entries{128};
    std::size_t material_cache_entries{1024};
    std::size_t cache_bytes{32U * 1024U * 1024U};
    std::size_t preparation_indices{262144};
    std::size_t preparation_vertices{131072};
    std::size_t preparation_bytes{16U * 1024U * 1024U};
    std::size_t reorder_window{64};
    std::size_t reorder_comparisons{262144};
    std::uint64_t cache_idle_builds{120};
};

struct BatchingSettings {
    bool enabled{true};
    bool appearance_enabled{true};
    bool prepare_submesh_ranges{true};
    bool retain_prepared_resources{true};
    BatchOrder order{BatchOrder::DisjointOpaque};
};

[[nodiscard]] bool valid(const BatchingLimits& limits) noexcept;
[[nodiscard]] bool valid(const BatchingSettings& settings) noexcept;
[[nodiscard]] bool valid(const BatchAppearanceComponent& appearance) noexcept;

// Metadata contains no library pointers and remains inspectable after resource
// replacement. Ordinals refer to VisibilityEntry order before safe regrouping.
struct BatchInstance {
    ge::RuntimeHandle entity{};
    geometry::MeshHandle mesh{};
    MaterialHandle material{};
    math::Mat4 model{};
    math::Aabb world_bounds{};
    std::uint64_t mesh_revision{};
    std::uint64_t material_revision{};
    std::size_t selected_ordinal{};
    std::size_t lod{};
    std::size_t group{};
    std::size_t output_draw{};
    std::uint32_t material_slot{};
    bool appearance_applied{};
    bool geometry_prepared{};
    bool transparent{};
};

struct BatchGroup {
    geometry::MeshHandle mesh{};
    MaterialHandle material{};
    std::uint64_t mesh_revision{};
    std::uint64_t material_revision{};
    std::uint64_t group_tag{};
    std::uint64_t visibility_mask{};
    std::uint64_t light_mask{};
    std::size_t first_instance{};
    std::size_t instance_count{};
    std::size_t first_index{};
    std::size_t index_count{};
    std::size_t input_triangles{};
    bool transparent{};
    bool ordered{};
    bool geometry_prepared{};
};

struct BatchCacheStatistics {
    std::size_t meshes{};
    std::size_t materials{};
    std::size_t bytes{};
    std::size_t externally_retained{};
};

// Per-view counters describe the latest attempt. Instance/group spans describe
// the last successful build, and are invalidated by the next successful build.
struct BatchingStatistics {
    std::uint64_t builds{};
    std::uint64_t successful_builds{};
    ge::RuntimeHandle camera{};
    std::size_t view_slot{};
    std::size_t selected{};
    std::size_t emitted{};
    std::size_t groups{};
    std::size_t shared_instances{};
    std::size_t ordered_instances{};
    std::size_t transparent_instances{};
    std::size_t appearance_instances{};
    std::size_t mesh_cache_hits{};
    std::size_t material_cache_hits{};
    std::size_t cache_evictions{};
    std::size_t preparation_fallbacks{};
    std::size_t prepared_vertices{};
    std::size_t prepared_indices{};
    std::size_t prepared_bytes{};
    std::size_t reorder_comparisons{};
    std::size_t reordered_instances{};
    std::size_t input_triangles{};
    BatchError error{BatchError::None};
    bool order_work_limited{};
    bool installed{};
};

// Synchronous single-writer owner. The shared builder can outlive this facade;
// it borrows no services between calls. Its destructor touches no external owner.
// Prepared mesh cache keys retain only the library's standalone lifetime token.
// Output is replaced transactionally; failure preserves the caller's submission.
// Original library borrows still require the synchronous visibility lifetime.
// Cache bytes exclude output-owned resources after eviction and vector slack;
// per-build preparation budgets and entry counts bound those separate costs.
// Groups feed actual contiguous RasterDraw arrays, with one draw per instance:
// this is CPU submission batching, not a GPU instancing implementation.
class BatchingService final {
public:
    explicit BatchingService(BatchingLimits limits = {});
    ~BatchingService();
    BatchingService(const BatchingService&) = delete;
    BatchingService& operator=(const BatchingService&) = delete;
    BatchingService(BatchingService&&) = delete;
    BatchingService& operator=(BatchingService&&) = delete;
    bool configure(const BatchingSettings& settings) noexcept;
    [[nodiscard]] const BatchingSettings& settings() const noexcept;
    [[nodiscard]] const BatchingLimits& limits() const noexcept;
    [[nodiscard]] const BatchingStatistics& statistics() const noexcept;
    [[nodiscard]] BatchCacheStatistics cache_statistics() const noexcept;
    [[nodiscard]] std::span<const BatchInstance> instances() const noexcept;
    [[nodiscard]] std::span<const BatchGroup> groups() const noexcept;
    [[nodiscard]] std::span<const BatchInstance> group_instances(std::size_t group) const noexcept;
    [[nodiscard]] std::shared_ptr<VisibilitySubmissionBuilder> submission_builder() const noexcept;
    [[nodiscard]] VisibilityBuildResult build(const VisibilityBuildContext& context,
                                               VisibilitySubmission& output);
    bool install(VisibilityService& visibility);
    bool uninstall(VisibilityService& visibility) noexcept;
    void shutdown(VisibilityService* visibility = nullptr) noexcept;
    void clear_cache() noexcept;
    bool invalidate(geometry::MeshHandle mesh) noexcept;
    bool invalidate(MaterialHandle material) noexcept;
    bool set_appearance(scene::Scene& scene, ge::RuntimeHandle entity,
                        const BatchAppearanceComponent& appearance);
    bool remove_appearance(scene::Scene& scene, ge::RuntimeHandle entity);
    [[nodiscard]] const BatchAppearanceComponent* appearance(const scene::Scene& scene,
                                                             ge::RuntimeHandle entity) const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

void register_batching(core::Kernel& kernel);

} // namespace ge::render
