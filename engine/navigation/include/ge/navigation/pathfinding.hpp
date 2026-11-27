#pragma once

#include <ge/core/identity.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::navigation {

using NodeId = ge::RuntimeHandle;
using RequestId = std::uint64_t;

struct GraphLimits {
    std::uint32_t max_nodes{65536};
    std::size_t max_edges{524288};
    std::size_t max_edges_per_node{256};
};

struct NavigationEdge {
    NodeId target{};
    double cost{1.0};
};

struct NavigationNode {
    NodeId id{};
    math::Vec3 position{};
    bool enabled{true};
};

// Node handles belong to one graph lifetime. Mutations invalidate borrowed spans
// and change revision; clear releases through the original generation pool.
class NavigationGraph final {
public:
    explicit NavigationGraph(GraphLimits limits = {});
    NavigationGraph(const NavigationGraph&) = delete;
    NavigationGraph& operator=(const NavigationGraph&) = delete;
    NavigationGraph(NavigationGraph&&) = delete;
    NavigationGraph& operator=(NavigationGraph&&) = delete;
    [[nodiscard]] NodeId add_node(math::Vec3 position, bool enabled = true);
    bool remove_node(NodeId node);
    bool set_position(NodeId node, math::Vec3 position);
    bool set_enabled(NodeId node, bool enabled);
    bool connect(NodeId from, NodeId to, double cost, bool bidirectional = false);
    bool disconnect(NodeId from, NodeId to, bool bidirectional = false);
    bool clear();
    [[nodiscard]] const NavigationNode* node(NodeId id) const noexcept;
    [[nodiscard]] NodeId node_at(std::uint32_t index) const noexcept;
    [[nodiscard]] std::span<const NavigationEdge> edges(NodeId from) const noexcept;
    [[nodiscard]] const GraphLimits& limits() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t edge_count() const noexcept;
    [[nodiscard]] std::size_t slot_count() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] double heuristic_scale() const noexcept;
    // Returns null when the complete slot scan would exceed scan_budget.
    [[nodiscard]] NodeId nearest(math::Vec3 position, double radius = math::infinity,
                                 std::size_t scan_budget = 65536) const noexcept;
    [[nodiscard]] std::uint32_t retired_slots() const noexcept;

private:
    struct Record {
        NavigationNode value{};
        std::vector<NavigationEdge> outgoing;
    };
    [[nodiscard]] Record* record(NodeId id) noexcept;
    void observe_edge(NodeId from, NodeId to, double cost) noexcept;
    GraphLimits limits_;
    GenerationPool handles_;
    std::vector<Record> slots_;
    std::size_t edge_count_{};
    std::uint64_t revision_{1};
    double heuristic_scale_{math::infinity};
};

enum class PathStatus : std::uint8_t {
    Idle, Queued, Running, Success, NoPath, InvalidInput, StaleGraph, Cancelled,
    BudgetExceeded
};

enum class SearchLimit : std::uint8_t {
    None, Visited, Frontier, Output, Cost, Storage, Work
};

enum class SearchAlgorithm : std::uint8_t { AStar, Dijkstra };

struct SearchOptions {
    SearchAlgorithm algorithm{SearchAlgorithm::AStar};
    std::size_t max_visited{65536};
    std::size_t max_frontier{262144};
    std::size_t max_output{65536};
    std::size_t max_storage_nodes{65536};
    std::size_t max_work{2097152};
    double max_cost{math::infinity};
    std::uint64_t expected_revision{};
    std::stop_token cancellation{};
};

struct PathResult {
    PathStatus status{PathStatus::Idle};
    SearchLimit limit{SearchLimit::None};
    std::uint64_t graph_revision{};
    NodeId start{};
    NodeId goal{};
    std::vector<NodeId> nodes;
    std::vector<math::Vec3> points;
    double cost{};
    std::size_t visited{};
    std::size_t work{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == PathStatus::Success;
    }
};

[[nodiscard]] std::string_view describe(PathStatus status) noexcept;
[[nodiscard]] std::string_view describe(SearchLimit limit) noexcept;

// Single writer. The graph must outlive this search. Every popped heap entry and
// inspected edge consumes one work unit, including obsolete frontier entries.
class PathSearch final {
public:
    PathSearch() = default;
    // Rejected input leaves the previous search and its result untouched.
    PathStatus begin(const NavigationGraph& graph, NodeId start, NodeId goal,
                     const SearchOptions& options = {});
    PathStatus step(std::size_t work_budget = 1024);
    void cancel() noexcept;
    [[nodiscard]] const PathResult& result() const noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    struct Visit {
        double cost{math::infinity};
        NodeId parent{};
        bool closed{};
    };
    struct Frontier {
        double estimate{};
        double cost{};
        NodeId node{};
    };
    struct Later {
        bool operator()(const Frontier& left, const Frontier& right) const noexcept;
    };
    void finish(PathStatus status, SearchLimit limit = SearchLimit::None) noexcept;
    void reconstruct();
    [[nodiscard]] double estimate(NodeId node) const noexcept;
    const NavigationGraph* graph_{};
    SearchOptions options_{};
    PathResult result_{};
    std::vector<Visit> visits_;
    std::vector<Frontier> frontier_;
    NodeId expanding_{};
    std::size_t edge_cursor_{};
    bool cost_pruned_{};
};

[[nodiscard]] PathResult find_path(const NavigationGraph& graph, NodeId start, NodeId goal,
                                    const SearchOptions& options = {});

struct DistanceSample {
    NodeId node{};
    NodeId predecessor{};
    double cost{math::infinity};
};

struct DistanceField {
    PathStatus status{PathStatus::Idle};
    SearchLimit limit{SearchLimit::None};
    std::uint64_t graph_revision{};
    NodeId source{};
    std::vector<DistanceSample> samples;
    std::size_t visited{};
    std::size_t work{};
    [[nodiscard]] std::optional<double> distance(NodeId node) const noexcept;
};

// Fields follow outgoing directed edges. A budget-limited field contains only
// finalized samples and explicitly reports that it is incomplete.
[[nodiscard]] DistanceField distance_field(const NavigationGraph& graph, NodeId source,
                                           const SearchOptions& options = {});
[[nodiscard]] NodeId nearest_reachable(const NavigationGraph& graph, const DistanceField& field,
                                       math::Vec3 position, double radius = math::infinity) noexcept;

struct ComponentMap {
    PathStatus status{PathStatus::Idle};
    std::uint64_t graph_revision{};
    std::vector<std::uint32_t> labels;
    std::size_t components{};
};

// Weak connectivity is a rejection aid; equal labels do not imply a directed path.
[[nodiscard]] ComponentMap connected_components(const NavigationGraph& graph,
                                                std::size_t node_budget = 65536,
                                                std::size_t edge_budget = 524288);

struct GridCoordinate {
    std::int32_t x{};
    std::int32_t y{};
    bool operator==(const GridCoordinate&) const = default;
};

enum class GridNeighbors : std::uint8_t { Four, Eight };
enum class DiagonalRule : std::uint8_t { Allow, AtLeastOneOpen, BothOpen };

struct GridCell {
    bool walkable{true};
    double cost{1.0};
};

struct GridNeighbor {
    GridCoordinate coordinate{};
    double cost{};
};

// Grid cells cover the X/Z plane; origin is the lower coordinate corner, and
// node positions lie at cell centers. Destination terrain multiplies edge cost.
class GridMap final {
public:
    explicit GridMap(std::size_t max_cells = 65536);
    bool configure(std::uint32_t width, std::uint32_t height, double cell_size = 1.0,
                   math::Vec3 origin = {}, GridNeighbors neighbors = GridNeighbors::Eight,
                   DiagonalRule diagonals = DiagonalRule::BothOpen);
    bool set_cell(GridCoordinate coordinate, GridCell value);
    bool fill(GridCell value);
    [[nodiscard]] const GridCell* cell(GridCoordinate coordinate) const noexcept;
    [[nodiscard]] std::optional<GridCoordinate> coordinate(math::Vec3 position) const noexcept;
    [[nodiscard]] math::Vec3 center(GridCoordinate coordinate) const noexcept;
    [[nodiscard]] std::size_t neighbors(GridCoordinate coordinate,
                                       std::span<GridNeighbor> output) const noexcept;
    [[nodiscard]] bool line_of_sight(GridCoordinate from, GridCoordinate to,
                                     std::size_t cell_budget = 65536) const noexcept;
    [[nodiscard]] bool line_of_sight(math::Vec3 from, math::Vec3 to,
                                     std::size_t cell_budget = 65536) const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    // Invalid input leaves output intact. Allocation failures propagate.
    bool bake(NavigationGraph& output, std::vector<NodeId>& cell_nodes) const;

private:
    [[nodiscard]] std::optional<std::size_t> offset(GridCoordinate coordinate) const noexcept;
    [[nodiscard]] bool open(GridCoordinate coordinate) const noexcept;
    [[nodiscard]] bool diagonal_open(GridCoordinate from, GridCoordinate to) const noexcept;
    std::size_t max_cells_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    double cell_size_{1.0};
    math::Vec3 origin_{};
    GridNeighbors neighbors_{GridNeighbors::Eight};
    DiagonalRule diagonals_{DiagonalRule::BothOpen};
    std::vector<GridCell> cells_;
    std::uint64_t revision_{1};
};

// Smoothing preserves endpoints and metadata. Budget/input failures preserve
// the original points. Costs remain the original graph route's traversal cost.
bool smooth_path(PathResult& path, const GridMap& grid, std::size_t sight_budget = 65536);

enum class FollowerState : std::uint8_t {
    Idle, Following, Arrived, Paused, StalePath, InvalidPath, ControllerRequired, SceneRejected
};

struct PathFollower {
    // Authored world-space paths may opt out of graph revision enforcement.
    std::vector<math::Vec3> points;
    std::uint64_t graph_revision{};
    std::size_t waypoint{};
    double speed{3.0};
    double stopping_distance{0.05};
    double waypoint_distance{0.01};
    bool pause{};
    bool require_current_graph{true};
    bool face_movement{};
    FollowerState state{FollowerState::Idle};
    math::Vec3 desired_velocity{};
};

struct NavigatorLimits {
    GraphLimits graph{};
    std::size_t max_requests{64};
    std::size_t max_results{128};
    std::size_t work_per_frame{8192};
    std::size_t work_per_request{512};
    std::size_t follower_waypoints_per_frame{128};
};

// Queue requests retain the submission revision. Graph changes make pending
// requests stale. Completed results remain available until taken or evicted.
class Navigator final {
public:
    explicit Navigator(NavigatorLimits limits = {});
    [[nodiscard]] NavigationGraph& graph() noexcept;
    [[nodiscard]] const NavigationGraph& graph() const noexcept;
    [[nodiscard]] GridMap& grid() noexcept;
    [[nodiscard]] const GridMap& grid() const noexcept;
    // Grid edits become navigation topology only after an explicit bake.
    bool bake_grid();
    [[nodiscard]] NodeId grid_node(GridCoordinate coordinate) const noexcept;
    [[nodiscard]] RequestId submit(NodeId start, NodeId goal, SearchOptions options = {});
    bool cancel(RequestId request);
    [[nodiscard]] const PathResult* result(RequestId request) const noexcept;
    [[nodiscard]] std::optional<PathResult> take_result(RequestId request);
    [[nodiscard]] std::size_t pending() const noexcept;
    [[nodiscard]] std::size_t update(std::size_t work_budget = 0);
    bool follow(ecs::Registry& registry, ecs::Entity entity, const PathResult& path,
                double speed = 3.0);
    std::size_t update_followers(scene::Scene& scene, double delta_seconds);

private:
    struct Request {
        RequestId id{};
        NodeId start{};
        NodeId goal{};
        SearchOptions options{};
        std::unique_ptr<PathSearch> search;
    };
    struct Completed {
        RequestId id{};
        PathResult path;
    };
    NavigatorLimits limits_;
    NavigationGraph graph_;
    GridMap grid_;
    std::vector<NodeId> grid_nodes_;
    std::uint64_t baked_grid_revision_{};
    std::uint64_t baked_graph_revision_{};
    std::deque<Request> requests_;
    std::deque<Completed> completed_;
    RequestId next_request_{1};
};

void register_pathfinding(core::Kernel& kernel);

} // namespace ge::navigation
