#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/math/math.hpp>
#include <ge/render/camera.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }
namespace ge::spatial { class SpatialIndex; }

namespace ge::editor {

enum class SelectionMode : std::uint8_t { Replace, Add, Toggle, Subtract };
enum class SelectionHierarchy : std::uint8_t { Entity, Root, MarkedRoot };
enum class SelectionPrecision : std::uint8_t { Triangles, Bounds };
enum class SelectionContainment : std::uint8_t { IntersectBounds, ContainBounds, Origin };
enum class SelectionReason : std::uint8_t { Edit, Primary, Hover, Prune, Undo, Redo, Restore };
enum class SelectionError : std::uint8_t {
    None,
    InvalidArgument,
    InvalidEntity,
    Capacity,
    InvalidCamera,
    Incomplete,
    Busy,
    HistoryEmpty,
    Inactive,
    Captured
};

[[nodiscard]] std::string_view describe(SelectionError error) noexcept;

// Editor metadata only. Hidden does not alter rendering; locked does not alter
// physics. By default an ancestor's lock, hidden state or disabled state applies.
struct SelectionComponent {
    bool selectable{true};
    bool locked{};
    bool hidden{};
    bool group_root{};
};

struct SelectionFilter {
    std::uint64_t layers{~std::uint64_t{0}};
    ecs::Entity subtree{};
    std::vector<std::string> required_tags;
    std::vector<std::string> excluded_tags;
    bool include_disabled{};
    bool include_locked{};
    bool include_hidden{};
    bool inherit_restrictions{true};
};

struct SelectionLimits {
    std::size_t selected{4096};
    std::size_t scene_entities{65536};
    std::size_t candidates{65536};
    std::size_t triangles{2097152};
    std::size_t hits{4096};
    std::size_t query_visits{1048576};
    std::size_t hierarchy_depth{4096};
    std::size_t undo_entries{64};
    std::size_t change_entries{128};
    std::size_t observers{32};
    std::size_t pointer_requests{16};
};

struct SelectionOptions {
    SelectionFilter filter{};
    SelectionHierarchy hierarchy{SelectionHierarchy::Entity};
    SelectionPrecision precision{SelectionPrecision::Triangles};
    double minimum_distance{};
    double maximum_distance{1000000000.0};
    bool cull_backfaces{};
    bool bounds_for_non_mesh{true};
};

struct SelectionHit {
    ecs::Entity entity{};
    ecs::Entity target{};
    geometry::MeshHandle mesh{};
    std::uint64_t mesh_revision{};
    std::size_t triangle{};
    std::size_t submesh{};
    double distance{};
    math::Vec3 point{};
    math::Vec3 geometric_normal{};
    math::Vec3 normal{};
    math::Vec3 barycentric{};
    math::Vec2 uv{};
    bool bounds_only{};
};

struct SelectionQueryReport {
    SelectionError error{SelectionError::None};
    std::uint64_t scene_revision{};
    std::size_t candidates_tested{};
    std::size_t triangles_tested{};
    std::size_t rejected{};
    std::size_t invalid_geometry{};
    std::size_t direct_tests{};
    bool used_index{};
    bool index_incomplete{};
    bool candidates_truncated{};
    bool triangles_truncated{};
    bool hits_truncated{};
    bool complete{true};
};

// These are current Scene geometry queries, never shaded-pixel visibility.
// MeshComponent's base mesh/ranges are used, without skinning, LOD, material
// alpha/discard, raster depth, shader displacement or custom submission builders.
struct SelectionPick {
    std::vector<SelectionHit> hits;
    SelectionQueryReport report{};
    SelectionFilter filter{};
    math::Ray ray{};
    ecs::Entity camera{};
    std::uint64_t camera_revision{};
};

struct SelectionVolume {
    std::vector<ecs::Entity> entities;
    SelectionQueryReport report{};
};

// Owned copies remain readable after later service edits. Handles must still be
// checked against Registry before use; this snapshot does not pin entity life.
struct SelectionSnapshot {
    std::vector<ecs::Entity> entities;
    ecs::Entity primary{};
    ecs::Entity hover{};
    std::uint64_t revision{};
};

struct SelectionChange {
    std::uint64_t revision{};
    SelectionReason reason{SelectionReason::Edit};
    std::vector<ecs::Entity> added;
    std::vector<ecs::Entity> removed;
    ecs::Entity previous_primary{};
    ecs::Entity primary{};
    ecs::Entity previous_hover{};
    ecs::Entity hover{};
};

struct SelectionChanges {
    std::vector<SelectionChange> entries;
    std::uint64_t next_cursor{};
    std::uint64_t newest_revision{};
    bool history_lost{};
};

struct SelectionResult {
    SelectionError error{SelectionError::None};
    std::size_t accepted{};
    std::size_t rejected{};
    bool changed{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == SelectionError::None;
    }
};

struct SelectionPointerRequest {
    math::Vec2 pixel{};
    std::size_t view_slot{};
    SelectionMode mode{SelectionMode::Replace};
    int cycle_steps{};
    bool hover_only{};
    bool ui_captured{};
};

struct SelectionStatistics {
    std::size_t selected{};
    std::size_t undo_entries{};
    std::size_t redo_entries{};
    std::size_t queued_requests{};
    std::uint64_t revision{};
    std::uint64_t observer_failures{};
    std::uint64_t incomplete_queries{};
    bool interaction_enabled{};
    bool pointer_captured{};
};

// Single writer borrowing Scene. Its destructor never touches borrowed services.
// Observer calls are synchronous and must not edit Scene/resources; reentrant
// selection edits are rejected. Independent
// changes_since cursors do not consume changes or another tool's undo history.
class SelectionService final {
public:
    using Observer = std::function<void(const SelectionChange&)>;
    explicit SelectionService(scene::Scene& scene, SelectionLimits limits = {});
    ~SelectionService();
    SelectionService(const SelectionService&) = delete;
    SelectionService& operator=(const SelectionService&) = delete;
    SelectionService(SelectionService&&) = delete;
    SelectionService& operator=(SelectionService&&) = delete;

    [[nodiscard]] SelectionSnapshot snapshot() const;
    // Borrowed span expires on any selection/primary/hover mutation; use an
    // owned snapshot across callbacks, frames or edits by another editor tool.
    [[nodiscard]] std::span<const ecs::Entity> selected() const noexcept;
    [[nodiscard]] ecs::Entity primary() const noexcept;
    [[nodiscard]] ecs::Entity hover() const noexcept;
    [[nodiscard]] bool contains(ecs::Entity entity) const noexcept;
    [[nodiscard]] bool eligible(ecs::Entity entity, const SelectionFilter& filter = {}) const;
    [[nodiscard]] const SelectionLimits& limits() const noexcept;
    [[nodiscard]] SelectionStatistics statistics() const noexcept;
    bool set_policy(ecs::Entity entity, SelectionComponent policy);
    [[nodiscard]] SelectionComponent policy(ecs::Entity entity) const noexcept;
    SelectionResult apply(std::span<const ecs::Entity> entities,
                          SelectionMode mode = SelectionMode::Replace,
                          const SelectionFilter& filter = {},
                          SelectionHierarchy hierarchy = SelectionHierarchy::Entity);
    SelectionResult clear();
    SelectionResult set_primary(ecs::Entity entity);
    SelectionResult set_hover(ecs::Entity entity, const SelectionFilter& filter = {});
    SelectionResult restore(const SelectionSnapshot& snapshot, const SelectionFilter& filter = {});
    SelectionResult prune();
    SelectionResult undo();
    SelectionResult redo();
    void clear_history() noexcept;
    [[nodiscard]] SelectionChanges changes_since(std::uint64_t cursor,
                                                 std::size_t limit = 128) const;
    [[nodiscard]] std::uint64_t subscribe(Observer observer);
    bool unsubscribe(std::uint64_t token) noexcept;
    [[nodiscard]] std::vector<ecs::Entity> selection_roots() const;
    [[nodiscard]] std::optional<math::Aabb> bounds(bool include_descendants = false);
    SelectionResult select_all(const SelectionFilter& filter = {});
    SelectionResult invert(const SelectionFilter& filter = {});
    SelectionResult select_parents(const SelectionFilter& filter = {});
    SelectionResult select_children(bool recursive, const SelectionFilter& filter = {});

    [[nodiscard]] SelectionPick raycast(const math::Ray& ray,
                                        const geometry::MeshLibrary& meshes,
                                        spatial::SpatialIndex* spatial = nullptr,
                                        const SelectionOptions& options = {});
    // Continuous top-left framebuffer pixels; add 0.5 to address texel centres.
    // Distance starts at the near plane. Far clipping uses unprojected depth 1.
    [[nodiscard]] SelectionPick pick(const render::CameraView& camera, math::Vec2 pixel,
                                     const geometry::MeshLibrary& meshes,
                                     spatial::SpatialIndex* spatial = nullptr,
                                     const SelectionOptions& options = {});
    [[nodiscard]] SelectionVolume query_frustum(const math::Frustum& frustum,
                                               const geometry::MeshLibrary& meshes,
                                               spatial::SpatialIndex* spatial = nullptr,
                                               const SelectionFilter& filter = {},
                                               SelectionContainment containment = SelectionContainment::IntersectBounds);
    [[nodiscard]] SelectionVolume marquee(const render::CameraView& camera,
                                          math::Vec2 start, math::Vec2 end,
                                          const geometry::MeshLibrary& meshes,
                                          spatial::SpatialIndex* spatial = nullptr,
                                          const SelectionFilter& filter = {},
                                          SelectionContainment containment = SelectionContainment::IntersectBounds);
    // Synchronizes pending Registry edits before validating the Scene revision.
    // Choose promptly before MeshLibrary edits: query geometry is not pinned,
    // and generation and Scene revision checks cannot pin a mesh revision.
    // Cycling retains identity among the same sorted target set. Incomplete
    // query results are rejected, so an unknown nearest hit is never committed.
    SelectionResult choose(const SelectionPick& pick, int cycle_steps = 0,
                           SelectionMode mode = SelectionMode::Replace);
    void reset_cycle() noexcept;

    // No InputSystem polling or action binding. A host explicitly enables this
    // service, updates capture, and queues requests after its UI arbitration.
    void set_interaction_enabled(bool enabled) noexcept;
    void set_pointer_captured(bool captured) noexcept;
    bool set_pointer_options(const SelectionOptions& options);
    SelectionResult queue_pointer(SelectionPointerRequest request);
    void frame(const render::CameraService* cameras, const geometry::MeshLibrary* meshes,
               spatial::SpatialIndex* spatial = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_selection(core::Kernel& kernel);

} // namespace ge::editor
