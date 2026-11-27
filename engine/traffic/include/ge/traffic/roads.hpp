#pragma once

#include <ge/animation/curves.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/navigation/pathfinding.hpp>
#include <ge/render/material.hpp>

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

namespace ge::scene { class WorldPartition; }

namespace ge::traffic {

struct RoadHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0 && generation != 0; }
    auto operator<=>(const RoadHandle&) const = default;
};

struct LaneHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0 && generation != 0; }
    auto operator<=>(const LaneHandle&) const = default;
};

struct JunctionHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0 && generation != 0; }
    auto operator<=>(const JunctionHandle&) const = default;
};

enum class RoadError : std::uint8_t {
    None, InvalidInput, InvalidHandle, Capacity, InvalidCurve, DegenerateGeometry,
    DuplicateName, MissingConnection, BoundRoad, MissingService, SceneRejected,
    MeshRejected, InconsistentOrigin, RevisionExhausted, StaleRoute, NoPath,
    RouteBudget, AccessDenied
};

[[nodiscard]] std::string_view describe(RoadError error) noexcept;

struct RoadResult {
    RoadError error{RoadError::None};
    std::size_t element{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == RoadError::None; }
};

struct RoadLimits {
    std::uint32_t max_roads{2048};
    std::uint32_t max_lanes{8192};
    std::uint32_t max_junctions{4096};
    std::size_t max_lanes_per_road{16};
    std::size_t max_samples_per_road{4096};
    std::size_t max_total_samples{1048576};
    std::size_t max_connections{32768};
    std::size_t max_outgoing{64};
    std::size_t max_connector_samples{128};
    std::size_t max_route_lanes{8192};
    std::size_t max_route_work{1048576};
    std::size_t max_name_bytes{128};
    double coordinate_limit{1.0e12};
    double max_road_length{1000000.0};
};

struct LaneSpecification {
    // Signed metres to the right of the authored centerline, before reverse.
    double offset{};
    double width{3.5};
    double speed_limit{13.8888888889};
    double lateral_acceleration{2.5};
    std::uint64_t access_mask{std::numeric_limits<std::uint64_t>::max()};
    bool reverse{};
    bool enabled{true};
};

struct RoadDescriptor {
    std::string name;
    animation::VectorCurve centerline;
    std::vector<LaneSpecification> lanes;
    double sample_spacing{2.0};
    std::size_t arc_samples{512};
    double shoulder_width{0.5};
    double texture_metres{4.0};
};

struct LaneSample {
    math::Vec3 position{};
    math::Vec3 tangent{0.0, 0.0, -1.0};
    math::Vec3 right{1.0, 0.0, 0.0};
    math::Vec3 up{0.0, 1.0, 0.0};
    double distance{};
    double curvature{};
    double recommended_speed{};
};

struct LaneInfo {
    LaneHandle handle{};
    RoadHandle road{};
    std::uint32_t ordinal{};
    LaneSpecification specification;
    double length{};
    double travel_seconds{};
    std::uint64_t revision{};
};

struct RoadInfo {
    RoadHandle handle{};
    std::string name;
    double centerline_length{};
    std::size_t samples{};
    std::vector<LaneHandle> lanes;
    math::Aabb absolute_bounds{};
    std::uint64_t revision{};
};

struct JunctionInfo {
    JunctionHandle handle{};
    std::string name;
    math::Vec3 absolute_position{};
    double radius{32.0};
    bool enabled{true};
};

struct ConnectionSpecification {
    JunctionHandle junction{};
    double speed_limit{8.0};
    double delay_seconds{};
    double tangent_scale{0.5};
    std::uint32_t priority{};
    std::uint64_t access_mask{std::numeric_limits<std::uint64_t>::max()};
    bool enabled{true};
};

struct LaneConnection {
    LaneHandle from{};
    LaneHandle to{};
    ConnectionSpecification specification;
    double length{};
    double travel_seconds{};
};

struct LaneProjection {
    LaneHandle lane{};
    LaneSample sample;
    double squared_distance{};
    double lateral_offset{};
};

struct RouteOptions {
    std::uint64_t access_mask{std::numeric_limits<std::uint64_t>::max()};
    std::size_t max_lanes{8192};
    std::size_t max_work{1048576};
    double max_seconds{std::numeric_limits<double>::infinity()};
};

struct RouteLeg {
    LaneHandle lane{};
    // A nonzero destination selects this lane's connector to that destination.
    LaneHandle connector_to{};
    double begin_distance{};
    double end_distance{};
    double route_distance{};
    double travel_seconds{};
};

struct RoadRoute {
    RoadError error{RoadError::NoPath};
    std::uint64_t network_revision{};
    std::uint64_t graph_epoch{};
    std::uint64_t graph_revision{};
    std::uint64_t access_mask{};
    std::vector<LaneHandle> lanes;
    std::vector<RouteLeg> legs;
    double length{};
    double travel_seconds{};
    std::size_t search_work{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == RoadError::None; }
};

struct RouteSample {
    LaneSample geometry;
    LaneHandle lane{};
    LaneHandle connector_to{};
    std::size_t leg{};
    double remaining_distance{};
    bool at_end{};
};

struct RoadMesh {
    geometry::MeshData mesh;
    math::Vec3 absolute_anchor{};
    std::uint64_t road_revision{};
};

struct RoadBinding {
    RoadHandle road{};
    ecs::Entity entity{};
    geometry::MeshHandle mesh{};
    render::MaterialHandle material{};
    std::uint64_t road_revision{};
    std::uint64_t origin_epoch{};
    RoadError last_error{RoadError::None};
};

struct RoadComponent {
    RoadHandle road{};
};

struct RoadStatistics {
    std::size_t roads{};
    std::size_t lanes{};
    std::size_t junctions{};
    std::size_t connections{};
    std::size_t samples{};
    std::size_t bindings{};
    std::uint64_t revision{};
    std::uint64_t graph_epoch{};
    bool graph_current{};
};

struct RoadSync {
    std::size_t inspected{};
    std::size_t updated{};
    std::size_t rejected{};
    std::size_t retired{};
};

// Single writer. Handles belong to this network lifetime. All authored points,
// lane samples, projections, route samples and navigation graph nodes are in
// ABSOLUTE coordinates. Only mesh vertices are anchor-relative; Scene bindings
// place that anchor at absolute_anchor - WorldPartition::origin().absolute_origin.
// Origin rebases do not change topology revision or invalidate absolute routes.
// Every successful network edit invalidates all routes conservatively. Replacing
// a road retires its lane handles and incident connections, preserving the road
// handle and visual binding. Spans expire on any network edit; graph borrows also
// expire on a route query with another access mask. Destruction touches no other
// service. Call clear_bindings while borrowed services are still alive.
// Geometry is a bounded chord approximation; no collision surface is installed.
// Samples clamp finite distances to the lane/route extent. Routes cannot wrap
// backward on the same lane. Replacement needs spare lane slots while staged.
// Binding roots are owned and remain unparented; released roots preserve children.
// Allocator and shared-library exceptions can propagate without a new binding.
class RoadNetwork final {
public:
    explicit RoadNetwork(RoadLimits limits = {});
    ~RoadNetwork();
    RoadNetwork(const RoadNetwork&) = delete;
    RoadNetwork& operator=(const RoadNetwork&) = delete;
    RoadNetwork(RoadNetwork&&) = delete;
    RoadNetwork& operator=(RoadNetwork&&) = delete;
    [[nodiscard]] const RoadLimits& limits() const noexcept;
    [[nodiscard]] RoadHandle create(RoadDescriptor descriptor, RoadResult* result = nullptr);
    [[nodiscard]] RoadResult replace(RoadHandle road, RoadDescriptor descriptor);
    [[nodiscard]] RoadResult erase(RoadHandle road);
    [[nodiscard]] RoadHandle find(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<RoadInfo> inspect(RoadHandle road) const;
    [[nodiscard]] std::optional<LaneInfo> lane(LaneHandle lane) const noexcept;
    [[nodiscard]] std::vector<RoadHandle> roads() const;
    [[nodiscard]] std::vector<LaneHandle> lanes(RoadHandle road) const;
    [[nodiscard]] std::span<const LaneSample> samples(LaneHandle lane) const noexcept;
    [[nodiscard]] std::optional<LaneSample> sample(LaneHandle lane, double distance) const noexcept;
    [[nodiscard]] std::optional<LaneProjection> project(LaneHandle lane, math::Vec3 absolute) const noexcept;
    [[nodiscard]] std::optional<LaneProjection> nearest(math::Vec3 absolute, double radius,
        std::uint64_t access_mask = std::numeric_limits<std::uint64_t>::max(),
        std::size_t segment_budget = 1048576) const noexcept;
    [[nodiscard]] RoadResult set_lane_enabled(LaneHandle lane, bool enabled);
    [[nodiscard]] RoadResult set_speed_limit(LaneHandle lane, double metres_per_second);
    [[nodiscard]] JunctionHandle create_junction(std::string name, math::Vec3 absolute,
                                                 double radius = 32.0);
    [[nodiscard]] std::optional<JunctionInfo> junction(JunctionHandle junction) const;
    [[nodiscard]] RoadResult set_junction_enabled(JunctionHandle junction, bool enabled);
    [[nodiscard]] RoadResult erase_junction(JunctionHandle junction);
    [[nodiscard]] RoadResult connect(LaneHandle from, LaneHandle to,
                                     ConnectionSpecification specification = {});
    [[nodiscard]] RoadResult disconnect(LaneHandle from, LaneHandle to);
    [[nodiscard]] std::optional<LaneConnection> connection(LaneHandle from, LaneHandle to) const noexcept;
    [[nodiscard]] std::vector<LaneConnection> outgoing(LaneHandle from) const;
    [[nodiscard]] std::optional<LaneSample> sample_connection(LaneHandle from, LaneHandle to,
                                                            double distance) const noexcept;
    [[nodiscard]] RoadRoute route(LaneHandle from, double from_distance, LaneHandle to,
                                   double to_distance, RouteOptions options = {});
    [[nodiscard]] bool route_current(const RoadRoute& route) const noexcept;
    [[nodiscard]] std::optional<RouteSample> sample_route(const RoadRoute& route,
                                                        double distance) const noexcept;
    [[nodiscard]] const navigation::NavigationGraph* graph() const noexcept;
    [[nodiscard]] RoadResult rebuild_graph(std::uint64_t access_mask =
        std::numeric_limits<std::uint64_t>::max());
    [[nodiscard]] std::optional<RoadMesh> build_mesh(RoadHandle road) const;
    [[nodiscard]] RoadResult bind(scene::Scene& scene_graph, geometry::MeshLibrary& meshes,
        render::MaterialService* materials, const scene::WorldPartition* partition,
        RoadHandle road, render::MaterialHandle material = {});
    [[nodiscard]] RoadResult unbind(scene::Scene& scene_graph, geometry::MeshLibrary& meshes,
                                    RoadHandle road);
    void clear_bindings(scene::Scene& scene_graph, geometry::MeshLibrary& meshes);
    [[nodiscard]] std::optional<RoadBinding> binding(RoadHandle road) const noexcept;
    [[nodiscard]] RoadSync sync(scene::Scene& scene_graph, geometry::MeshLibrary& meshes,
        const scene::WorldPartition* partition, std::size_t budget = 16);
    [[nodiscard]] RoadStatistics statistics() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using Roads = RoadNetwork;

void register_roads(core::Kernel& kernel);

} // namespace ge::traffic
