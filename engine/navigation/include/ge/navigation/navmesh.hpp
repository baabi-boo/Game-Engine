#pragma once

#include <ge/geometry/mesh.hpp>
#include <ge/navigation/pathfinding.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ge::navigation {

inline constexpr std::uint32_t no_nav_link = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t nav_area_count = 64;

enum class NavStatus : std::uint8_t {
    Success, Empty, InvalidInput, InvalidPolygon, NonManifold, CapacityExceeded,
    BudgetExceeded, NoPolygon, NoPath, StaleMesh, StaleSource, Unsupported,
    NumericFailure, RevisionExhausted, Cancelled
};

[[nodiscard]] std::string_view describe(NavStatus status) noexcept;

struct NavResult {
    NavStatus status{NavStatus::Success};
    std::size_t element{};
    std::size_t work{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == NavStatus::Success;
    }
};

// References are scoped to one NavMesh lifetime. A successful rebuild or clear
// invalidates every old reference; rejected rebuilds preserve them all.
struct NavPolygonRef {
    std::uint32_t index{};
    std::uint64_t revision{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return index != 0 && revision != 0;
    }
    auto operator<=>(const NavPolygonRef&) const = default;
};

struct NavMeshLimits {
    std::size_t max_vertices{262144};
    std::size_t max_polygons{65536};
    std::size_t max_vertices_per_polygon{32};
    std::size_t max_links{1024};
    std::size_t max_arcs{524288};
    std::size_t max_arcs_per_polygon{256};
    std::size_t max_build_work{16777216};
};

struct NavBuildOptions {
    double weld_tolerance{1.0e-5};
    // Applied to every triangle in the height fan; minimum supported value is 1e-10.
    double minimum_projected_area{1.0e-8};
    double maximum_slope_degrees{60.0};
    // Mesh baking discards downward and steep triangles. Direct polygon input
    // accepts either winding and rejects every inadmissible polygon.
    bool upward_triangles_only{true};
};

struct NavPolygonInput {
    std::vector<std::uint32_t> vertices;
    std::uint64_t flags{1};
    std::uint8_t area{};
};

struct NavOffMeshLink {
    math::Vec3 start{};
    math::Vec3 end{};
    double snap_radius{0.5};
    double traversal_cost{1.0};
    std::uint64_t flags{1};
    std::uint64_t user_id{};
    bool bidirectional{true};
};

struct NavMeshInput {
    std::vector<math::Vec3> vertices;
    std::vector<NavPolygonInput> polygons;
    std::vector<NavOffMeshLink> links;
};

// A zero mesh handle denotes caller-owned authored input. Scene sources retain
// the exact world matrix and selected submesh range, rather than approximate TRS.
struct NavMeshSource {
    geometry::MeshHandle mesh{};
    std::uint64_t mesh_revision{};
    ecs::Entity entity{};
    math::Mat4 transform{};
    std::uint32_t first_submesh{};
    std::uint32_t submesh_count{};
};

struct NavPolygon {
    NavPolygonRef reference{};
    std::vector<std::uint32_t> vertices;
    std::uint64_t flags{1};
    std::uint8_t area{};
    math::Vec3 center{};
    math::Aabb bounds{};
};

struct NavPortal {
    NavPolygonRef from{};
    NavPolygonRef to{};
    // Ordinary portals are left/right in XZ while travelling from -> to.
    // A link instead stores its snapped entry in left and exit in right.
    math::Vec3 left{};
    math::Vec3 right{};
    std::uint32_t link_index{no_nav_link};
    std::uint64_t link_user_id{};
    [[nodiscard]] bool off_mesh() const noexcept {
        return link_index != no_nav_link;
    }
};

struct NavFilter {
    std::uint64_t include_flags{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t exclude_flags{};
    std::array<double, nav_area_count> area_costs{};
    bool allow_off_mesh_links{true};
    NavFilter() noexcept;
    [[nodiscard]] bool allows(std::uint64_t flags) const noexcept;
    [[nodiscard]] bool valid() const noexcept;
};

struct NavQueryOptions {
    NavFilter filter{};
    // Search budgets and cancellation are honored. expected_revision here is
    // replaced by the private graph revision; use the navmesh field below.
    SearchOptions search{};
    std::size_t max_polygons{65536};
    std::size_t max_work{4194304};
    std::size_t max_points{65536};
    double snap_distance{2.0};
    // Positive-radius clearance needs obstacle erosion and is explicitly unsupported.
    double agent_radius{};
    std::uint64_t expected_revision{};
};

struct NavLocation {
    NavStatus status{NavStatus::NoPolygon};
    NavPolygonRef polygon{};
    math::Vec3 position{};
    double distance{};
    std::size_t work{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == NavStatus::Success;
    }
};

enum class NavPointKind : std::uint8_t { Surface, LinkEntry, LinkExit };

struct NavPathPoint {
    math::Vec3 position{};
    NavPolygonRef polygon{};
    NavPointKind kind{NavPointKind::Surface};
    std::uint64_t link_user_id{};
    std::uint32_t link_index{no_nav_link};
};

struct NavPath {
    NavStatus status{NavStatus::NoPath};
    std::uint64_t revision{};
    NavLocation start{};
    NavLocation goal{};
    std::vector<NavPolygonRef> corridor;
    std::vector<NavPortal> portals;
    std::vector<NavPathPoint> corners;
    // Shared PathResult contains authored world-space funnel points. Its node
    // list is empty because private query-graph handles cannot escape their owner.
    // graph_revision is zero; NavPath::revision identifies the mesh. This also
    // prevents accidental acceptance by Navigator::follow as a graph-owned path.
    // Cost describes the center/portal graph route, not exact funnel distance.
    // The funnel straightens XZ. Use surface projection or move_along_surface
    // while following corners to recover height changes between those corners.
    PathResult path{};
    std::size_t work{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == NavStatus::Success;
    }
};

struct NavMoveResult {
    NavStatus status{NavStatus::NoPolygon};
    NavLocation location{};
    std::vector<NavPolygonRef> visited;
    math::Vec3 wall_normal{};
    double fraction{};
    std::size_t work{};
    bool blocked{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == NavStatus::Success;
    }
};

struct NavMeshStatistics {
    std::size_t vertices{};
    std::size_t polygons{};
    std::size_t directed_portals{};
    std::size_t boundary_edges{};
    std::size_t links{};
    std::uint64_t revision{};
    bool source_stale{};
};

// Single-writer, immutable between rebuilds. XZ footprints must be strictly convex;
// heights use a vertex-zero triangle fan. Only complete welded edges connect.
// Overlapping interiors, T-junction splitting, obstacle carving and automatic
// tile stitching are not implemented. Allocations may throw; rebuild is atomic.
class NavMesh final {
public:
    explicit NavMesh(NavMeshLimits limits = {});
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;
    NavMesh(NavMesh&&) = delete;
    NavMesh& operator=(NavMesh&&) = delete;
    NavResult rebuild(const NavMeshInput& input, NavBuildOptions options = {},
                      NavMeshSource source = {});
    NavResult bake_mesh(const geometry::MeshData& mesh, const math::Mat4& transform = {},
                        NavBuildOptions options = {}, NavMeshSource source = {});
    bool clear() noexcept;
    // Call after source mutations, including origin shifts. Freshness is cached;
    // queries never access borrowed services. Registration refreshes before simulation.
    bool refresh_source(const geometry::MeshLibrary* meshes, scene::Scene* active_scene);
    [[nodiscard]] const NavMeshSource& source() const noexcept;
    [[nodiscard]] const NavMeshLimits& limits() const noexcept;
    [[nodiscard]] NavMeshStatistics statistics() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool current(const NavPath& path) const noexcept;
    [[nodiscard]] const NavPolygon* polygon(NavPolygonRef reference) const noexcept;
    [[nodiscard]] std::span<const math::Vec3> vertices() const noexcept;
    [[nodiscard]] std::span<const NavPolygon> polygons() const noexcept;
    [[nodiscard]] std::span<const NavPortal> portals() const noexcept;
    [[nodiscard]] std::span<const NavOffMeshLink> links() const noexcept;
    [[nodiscard]] NavLocation closest_point(NavPolygonRef polygon, math::Vec3 point) const;
    [[nodiscard]] NavLocation nearest(math::Vec3 point, const NavQueryOptions& options = {}) const;
    [[nodiscard]] NavLocation containing(math::Vec3 point, const NavQueryOptions& options = {}) const;
    [[nodiscard]] NavPath find_path(math::Vec3 start, math::Vec3 goal,
                                    const NavQueryOptions& options = {}) const;
    // Follows a straight XZ segment, interpolates height, and stops at the first
    // boundary/filter wall. Does not slide, traverse links or move any entity.
    [[nodiscard]] NavMoveResult move_along_surface(NavLocation start, math::Vec3 target,
                                                  const NavQueryOptions& options = {}) const;
private:
    struct Data;
    NavResult rebuild_impl(const NavMeshInput& input, NavBuildOptions options,
                           NavMeshSource source, std::size_t initial_work);
    [[nodiscard]] NavStatus query_status(const NavQueryOptions& options) const noexcept;
    [[nodiscard]] NavLocation locate(math::Vec3 point, const NavQueryOptions& options,
                                     bool containing_only) const;
    NavMeshLimits limits_;
    std::unique_ptr<Data> data_;
    NavMeshSource source_{};
    std::uint64_t revision_{1};
    bool source_stale_{};
};

void register_navmesh(core::Kernel& kernel);

} // namespace ge::navigation
