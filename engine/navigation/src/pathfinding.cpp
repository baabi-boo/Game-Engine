#include <ge/navigation/pathfinding.hpp>

#include <ge/core/runtime.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace ge::navigation {
namespace {
constexpr std::size_t hard_node_limit = 1048576;
constexpr std::size_t hard_work_limit = 67108864;
constexpr auto last_revision = std::numeric_limits<std::uint64_t>::max();

bool valid_options(const SearchOptions& options) noexcept {
    return options.max_visited > 0 && options.max_visited <= hard_node_limit &&
        options.max_frontier > 0 && options.max_frontier <= hard_work_limit &&
        options.max_output > 0 && options.max_output <= hard_node_limit &&
        options.max_storage_nodes > 0 && options.max_storage_nodes <= hard_node_limit &&
        options.max_work > 0 && options.max_work <= hard_work_limit &&
        !std::isnan(options.max_cost) && options.max_cost >= 0.0 &&
        (options.algorithm == SearchAlgorithm::AStar || options.algorithm == SearchAlgorithm::Dijkstra);
}

bool active(const NavigationGraph& graph, NodeId id) noexcept {
    const auto* value = graph.node(id);
    return value != nullptr && value->enabled;
}

double safe_distance(math::Vec3 left, math::Vec3 right) noexcept {
    return std::hypot(left.x - right.x, left.y - right.y, left.z - right.z);
}

template<class T>
bool parse_number(std::string_view text, T& output) {
    T candidate{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), candidate);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return false;
    }
    output = candidate;
    return true;
}

bool parse_handle(std::string_view text, NodeId& output) {
    const auto colon = text.find(':');
    return colon != std::string_view::npos && parse_number(text.substr(0, colon), output.index) &&
        parse_number(text.substr(colon + 1), output.generation) && output.valid();
}

std::string handle_text(NodeId id) {
    return std::to_string(id.index) + ":" + std::to_string(id.generation);
}
}

NavigationGraph::NavigationGraph(GraphLimits limits) : limits_(limits), handles_(limits.max_nodes) {
    if (limits.max_nodes > hard_node_limit || limits.max_edges > hard_work_limit ||
        limits.max_edges_per_node > hard_work_limit) {
        throw std::invalid_argument("navigation graph limits exceed supported bounds");
    }
    slots_.emplace_back();
}

NavigationGraph::Record* NavigationGraph::record(NodeId id) noexcept {
    return handles_.alive(id) && id.index < slots_.size() ? &slots_[id.index] : nullptr;
}

NodeId NavigationGraph::add_node(math::Vec3 position, bool enabled) {
    if (!math::finite(position) || revision_ == last_revision) {
        return {};
    }
    const auto acquired = handles_.acquire();
    if (!acquired) {
        return {};
    }
    try {
        if (slots_.size() <= acquired->index) {
            slots_.resize(static_cast<std::size_t>(acquired->index) + 1);
        }
    } catch (...) {
        (void)handles_.release(*acquired);
        throw;
    }
    auto& destination = slots_[acquired->index];
    destination.value = {*acquired, position, enabled};
    destination.outgoing.clear();
    ++revision_;
    return *acquired;
}

bool NavigationGraph::remove_node(NodeId id) {
    auto* value = record(id);
    if (value == nullptr || revision_ == last_revision) {
        return false;
    }
    edge_count_ -= value->outgoing.size();
    value->outgoing.clear();
    for (auto& slot : slots_) {
        edge_count_ -= std::erase_if(slot.outgoing, [id](const auto& edge) { return edge.target == id; });
    }
    value->value = {};
    (void)handles_.release(id);
    ++revision_;
    return true;
}

bool NavigationGraph::set_position(NodeId id, math::Vec3 position) {
    auto* value = record(id);
    if (value == nullptr || !math::finite(position) || revision_ == last_revision) {
        return false;
    }
    if (value->value.position != position) {
        value->value.position = position;
        heuristic_scale_ = 0.0;
        ++revision_;
    }
    return true;
}

bool NavigationGraph::set_enabled(NodeId id, bool enabled) {
    auto* value = record(id);
    if (value == nullptr || revision_ == last_revision) {
        return false;
    }
    if (value->value.enabled != enabled) {
        value->value.enabled = enabled;
        ++revision_;
    }
    return true;
}

void NavigationGraph::observe_edge(NodeId from, NodeId to, double cost) noexcept {
    const double distance = safe_distance(slots_[from.index].value.position, slots_[to.index].value.position);
    if (!std::isfinite(distance)) {
        heuristic_scale_ = 0.0;
    } else if (distance > 0.0) {
        heuristic_scale_ = std::min(heuristic_scale_, cost / distance);
    }
}

bool NavigationGraph::connect(NodeId from, NodeId to, double cost, bool bidirectional) {
    auto* first = record(from);
    auto* second = record(to);
    if (first == nullptr || second == nullptr || !std::isfinite(cost) || cost < 0.0 || revision_ == last_revision) {
        return false;
    }
    bidirectional = bidirectional && from != to;
    const auto locate = [](auto& edges, NodeId target) {
        return std::find_if(edges.begin(), edges.end(), [target](const auto& edge) { return edge.target == target; });
    };
    const bool add_first = locate(first->outgoing, to) == first->outgoing.end();
    const bool add_second = bidirectional && locate(second->outgoing, from) == second->outgoing.end();
    const auto additions = static_cast<std::size_t>(add_first) + static_cast<std::size_t>(add_second);
    if (additions > limits_.max_edges - edge_count_ ||
        (add_first && first->outgoing.size() >= limits_.max_edges_per_node) ||
        (add_second && second->outgoing.size() >= limits_.max_edges_per_node)) {
        return false;
    }
    if (add_first) {
        first->outgoing.reserve(first->outgoing.size() + 1);
    }
    if (add_second) {
        second->outgoing.reserve(second->outgoing.size() + 1);
    }
    if (add_first) {
        first->outgoing.push_back({to, cost});
    } else {
        locate(first->outgoing, to)->cost = cost;
    }
    if (add_second) {
        second->outgoing.push_back({from, cost});
    } else if (bidirectional) {
        locate(second->outgoing, from)->cost = cost;
    }
    edge_count_ += additions;
    observe_edge(from, to, cost);
    ++revision_;
    return true;
}

bool NavigationGraph::disconnect(NodeId from, NodeId to, bool bidirectional) {
    auto* first = record(from);
    auto* second = record(to);
    if (first == nullptr || second == nullptr || revision_ == last_revision) {
        return false;
    }
    auto removed = std::erase_if(first->outgoing, [to](const auto& edge) { return edge.target == to; });
    if (bidirectional && from != to) {
        removed += std::erase_if(second->outgoing, [from](const auto& edge) { return edge.target == from; });
    }
    if (removed == 0) {
        return false;
    }
    edge_count_ -= removed;
    ++revision_;
    return true;
}

bool NavigationGraph::clear() {
    if (revision_ == last_revision) {
        return false;
    }
    if (handles_.size() == 0) {
        return true;
    }
    for (auto& slot : slots_) {
        if (slot.value.id) {
            (void)handles_.release(slot.value.id);
            slot.value = {};
            slot.outgoing.clear();
        }
    }
    edge_count_ = 0;
    heuristic_scale_ = math::infinity;
    ++revision_;
    return true;
}

const NavigationNode* NavigationGraph::node(NodeId id) const noexcept {
    return handles_.alive(id) && id.index < slots_.size() ? &slots_[id.index].value : nullptr;
}
NodeId NavigationGraph::node_at(std::uint32_t index) const noexcept {
    return index < slots_.size() ? slots_[index].value.id : NodeId{};
}
std::span<const NavigationEdge> NavigationGraph::edges(NodeId from) const noexcept {
    return node(from) ? std::span<const NavigationEdge>(slots_[from.index].outgoing) : std::span<const NavigationEdge>{};
}
const GraphLimits& NavigationGraph::limits() const noexcept { return limits_; }
std::size_t NavigationGraph::size() const noexcept { return handles_.size(); }
std::size_t NavigationGraph::edge_count() const noexcept { return edge_count_; }
std::size_t NavigationGraph::slot_count() const noexcept { return slots_.size() - 1; }
std::uint64_t NavigationGraph::revision() const noexcept { return revision_; }
std::uint32_t NavigationGraph::retired_slots() const noexcept { return handles_.retired_slots(); }
double NavigationGraph::heuristic_scale() const noexcept {
    return std::isfinite(heuristic_scale_) ? heuristic_scale_ : 0.0;
}

NodeId NavigationGraph::nearest(math::Vec3 position, double radius, std::size_t scan_budget) const noexcept {
    if (!math::finite(position) || std::isnan(radius) || radius < 0.0 || slot_count() > scan_budget) {
        return {};
    }
    NodeId best{};
    const auto last = std::min(slot_count(), scan_budget);
    for (std::size_t index = 1; index <= last; ++index) {
        const auto& candidate = slots_[index].value;
        if (!candidate.id || !candidate.enabled) {
            continue;
        }
        const auto distance = safe_distance(candidate.position, position);
        if (std::isfinite(distance) && distance <= radius && (!best || distance < radius)) {
            best = candidate.id;
            radius = distance;
        }
    }
    return best;
}

std::string_view describe(PathStatus status) noexcept {
    switch (status) {
    case PathStatus::Idle: return "idle";
    case PathStatus::Queued: return "queued";
    case PathStatus::Running: return "running";
    case PathStatus::Success: return "success";
    case PathStatus::NoPath: return "no path";
    case PathStatus::InvalidInput: return "invalid input";
    case PathStatus::StaleGraph: return "stale graph";
    case PathStatus::Cancelled: return "cancelled";
    case PathStatus::BudgetExceeded: return "budget exceeded";
    }
    return "invalid status";
}
std::string_view describe(SearchLimit limit) noexcept {
    switch (limit) {
    case SearchLimit::None: return "none";
    case SearchLimit::Visited: return "visited";
    case SearchLimit::Frontier: return "frontier";
    case SearchLimit::Output: return "output";
    case SearchLimit::Cost: return "cost";
    case SearchLimit::Storage: return "storage";
    case SearchLimit::Work: return "work";
    }
    return "invalid limit";
}

bool PathSearch::Later::operator()(const Frontier& left, const Frontier& right) const noexcept {
    if (left.estimate != right.estimate) {
        return left.estimate > right.estimate;
    }
    if (left.cost != right.cost) {
        return left.cost > right.cost;
    }
    return left.node > right.node;
}

PathStatus PathSearch::begin(const NavigationGraph& graph, NodeId start, NodeId goal, const SearchOptions& options) {
    if (!valid_options(options)) {
        return PathStatus::InvalidInput;
    }
    if (options.expected_revision != 0 && options.expected_revision != graph.revision()) {
        return PathStatus::StaleGraph;
    }
    if (!active(graph, start) || !active(graph, goal)) {
        return PathStatus::InvalidInput;
    }
    if (graph.slot_count() > options.max_storage_nodes) {
        return PathStatus::BudgetExceeded;
    }
    std::vector<Visit> visits(graph.slot_count() + 1);
    std::vector<Frontier> frontier;
    frontier.reserve(std::min(graph.slot_count(), options.max_frontier));
    frontier.push_back({0.0, 0.0, start});
    visits[start.index].cost = 0.0;
    visits_.swap(visits);
    frontier_.swap(frontier);
    graph_ = &graph;
    options_ = options;
    result_ = {};
    result_.status = PathStatus::Running;
    result_.graph_revision = graph.revision();
    result_.start = start;
    result_.goal = goal;
    expanding_ = {};
    edge_cursor_ = 0;
    cost_pruned_ = false;
    return result_.status;
}

void PathSearch::finish(PathStatus status, SearchLimit limit) noexcept {
    result_.status = status;
    result_.limit = limit;
    expanding_ = {};
    frontier_.clear();
}
void PathSearch::cancel() noexcept {
    if (running()) {
        finish(PathStatus::Cancelled);
    }
}
const PathResult& PathSearch::result() const noexcept { return result_; }
bool PathSearch::running() const noexcept { return result_.status == PathStatus::Running; }

double PathSearch::estimate(NodeId id) const noexcept {
    if (options_.algorithm == SearchAlgorithm::Dijkstra) {
        return 0.0;
    }
    const auto distance = safe_distance(graph_->node(id)->position, graph_->node(result_.goal)->position);
    const auto bound = distance * graph_->heuristic_scale();
    return std::isfinite(bound) ? bound : 0.0;
}

void PathSearch::reconstruct() {
    std::vector<NodeId> route;
    auto cursor = result_.goal;
    while (cursor) {
        if (options_.cancellation.stop_requested()) {
            finish(PathStatus::Cancelled);
            return;
        }
        if (route.size() >= options_.max_output) {
            finish(PathStatus::BudgetExceeded, SearchLimit::Output);
            return;
        }
        route.push_back(cursor);
        if (cursor == result_.start) {
            break;
        }
        cursor = visits_[cursor.index].parent;
    }
    if (route.empty() || route.back() != result_.start) {
        finish(PathStatus::NoPath);
        return;
    }
    std::reverse(route.begin(), route.end());
    std::vector<math::Vec3> points;
    points.reserve(route.size());
    for (const auto id : route) {
        points.push_back(graph_->node(id)->position);
    }
    result_.nodes.swap(route);
    result_.points.swap(points);
    result_.cost = visits_[result_.goal.index].cost;
    finish(PathStatus::Success);
}

PathStatus PathSearch::step(std::size_t work_budget) {
    if (!running()) {
        return result_.status;
    }
    if (graph_->revision() != result_.graph_revision) {
        finish(PathStatus::StaleGraph);
    } else if (options_.cancellation.stop_requested()) {
        finish(PathStatus::Cancelled);
    }
    while (running() && work_budget > 0) {
        if (options_.cancellation.stop_requested()) {
            finish(PathStatus::Cancelled);
            break;
        }
        if (expanding_ && edge_cursor_ == graph_->edges(expanding_).size()) {
            expanding_ = {};
        }
        if (!expanding_ && frontier_.empty()) {
            finish(cost_pruned_ ? PathStatus::BudgetExceeded : PathStatus::NoPath,
                   cost_pruned_ ? SearchLimit::Cost : SearchLimit::None);
            break;
        }
        if (result_.work >= options_.max_work) {
            finish(PathStatus::BudgetExceeded, SearchLimit::Work);
            break;
        }
        if (expanding_) {
            const auto outgoing = graph_->edges(expanding_);
            const auto edge = outgoing[edge_cursor_++];
            ++result_.work;
            --work_budget;
            if (!active(*graph_, edge.target)) {
                continue;
            }
            auto& next = visits_[edge.target.index];
            const double cost = visits_[expanding_.index].cost + edge.cost;
            if (!std::isfinite(cost) || cost > options_.max_cost) {
                cost_pruned_ = true;
                continue;
            }
            if (cost >= next.cost) {
                continue;
            }
            if (frontier_.size() >= options_.max_frontier) {
                finish(PathStatus::BudgetExceeded, SearchLimit::Frontier);
                break;
            }
            const auto heuristic = estimate(edge.target);
            const auto estimate_value = cost > std::numeric_limits<double>::max() - heuristic ? cost : cost + heuristic;
            frontier_.push_back({estimate_value, cost, edge.target});
            std::push_heap(frontier_.begin(), frontier_.end(), Later{});
            next.cost = cost;
            next.parent = expanding_;
            next.closed = false;
            continue;
        }
        std::pop_heap(frontier_.begin(), frontier_.end(), Later{});
        const auto current = frontier_.back();
        frontier_.pop_back();
        ++result_.work;
        --work_budget;
        auto& visit = visits_[current.node.index];
        if (visit.closed || current.cost != visit.cost) {
            continue;
        }
        if (result_.visited >= options_.max_visited) {
            finish(PathStatus::BudgetExceeded, SearchLimit::Visited);
            break;
        }
        visit.closed = true;
        ++result_.visited;
        if (current.node == result_.goal) {
            reconstruct();
            break;
        }
        expanding_ = current.node;
        edge_cursor_ = 0;
    }
    if (running()) {
        if (expanding_ && edge_cursor_ == graph_->edges(expanding_).size()) {
            expanding_ = {};
        }
        if (!expanding_ && frontier_.empty()) {
            finish(cost_pruned_ ? PathStatus::BudgetExceeded : PathStatus::NoPath,
                   cost_pruned_ ? SearchLimit::Cost : SearchLimit::None);
        } else if (result_.work >= options_.max_work) {
            finish(PathStatus::BudgetExceeded, SearchLimit::Work);
        }
    }
    return result_.status;
}

PathResult find_path(const NavigationGraph& graph, NodeId start, NodeId goal, const SearchOptions& options) {
    PathSearch search;
    const auto status = search.begin(graph, start, goal, options);
    if (status != PathStatus::Running) {
        PathResult rejected;
        rejected.status = status;
        rejected.graph_revision = options.expected_revision != 0 ? options.expected_revision : graph.revision();
        rejected.start = start;
        rejected.goal = goal;
        rejected.limit = status == PathStatus::BudgetExceeded ? SearchLimit::Storage : SearchLimit::None;
        return rejected;
    }
    while (search.running()) {
        (void)search.step(options.max_work);
    }
    return search.result();
}

std::optional<double> DistanceField::distance(NodeId id) const noexcept {
    const auto found = std::lower_bound(samples.begin(), samples.end(), id,
        [](const DistanceSample& sample, NodeId target) { return sample.node < target; });
    return found != samples.end() && found->node == id ? std::optional<double>{found->cost} : std::nullopt;
}

DistanceField distance_field(const NavigationGraph& graph, NodeId source, const SearchOptions& options) {
    DistanceField field;
    field.graph_revision = graph.revision();
    field.source = source;
    if (!valid_options(options)) {
        field.status = PathStatus::InvalidInput;
        return field;
    }
    if (options.expected_revision != 0 && options.expected_revision != graph.revision()) {
        field.status = PathStatus::StaleGraph;
        field.graph_revision = options.expected_revision;
        return field;
    }
    if (!active(graph, source)) {
        field.status = PathStatus::InvalidInput;
        return field;
    }
    if (graph.slot_count() > options.max_storage_nodes) {
        field.status = PathStatus::BudgetExceeded;
        field.limit = SearchLimit::Storage;
        return field;
    }
    struct Entry { double cost; NodeId node; };
    const auto later = [](const Entry& left, const Entry& right) {
        return left.cost == right.cost ? left.node > right.node : left.cost > right.cost;
    };
    std::vector<double> costs(graph.slot_count() + 1, math::infinity);
    std::vector<NodeId> parents(graph.slot_count() + 1);
    std::vector<bool> settled(graph.slot_count() + 1);
    std::vector<Entry> frontier{{0.0, source}};
    costs[source.index] = 0.0;
    field.status = PathStatus::Running;
    bool cost_pruned = false;
    const auto consume = [&]() {
        if (options.cancellation.stop_requested()) {
            field.status = PathStatus::Cancelled;
            return false;
        }
        if (field.work >= options.max_work) {
            field.status = PathStatus::BudgetExceeded;
            field.limit = SearchLimit::Work;
            return false;
        }
        ++field.work;
        return true;
    };
    while (!frontier.empty() && field.status == PathStatus::Running && consume()) {
        std::pop_heap(frontier.begin(), frontier.end(), later);
        const auto current = frontier.back();
        frontier.pop_back();
        if (settled[current.node.index] || current.cost != costs[current.node.index]) {
            continue;
        }
        if (field.visited >= options.max_visited || field.samples.size() >= options.max_output) {
            field.status = PathStatus::BudgetExceeded;
            field.limit = field.visited >= options.max_visited ? SearchLimit::Visited : SearchLimit::Output;
            break;
        }
        settled[current.node.index] = true;
        ++field.visited;
        field.samples.push_back({current.node, parents[current.node.index], current.cost});
        for (const auto edge : graph.edges(current.node)) {
            if (!consume()) {
                break;
            }
            if (!active(graph, edge.target) || settled[edge.target.index]) {
                continue;
            }
            const auto cost = current.cost + edge.cost;
            if (!std::isfinite(cost) || cost > options.max_cost) {
                cost_pruned = true;
                continue;
            }
            if (cost >= costs[edge.target.index]) {
                continue;
            }
            if (frontier.size() >= options.max_frontier) {
                field.status = PathStatus::BudgetExceeded;
                field.limit = SearchLimit::Frontier;
                break;
            }
            frontier.push_back({cost, edge.target});
            std::push_heap(frontier.begin(), frontier.end(), later);
            costs[edge.target.index] = cost;
            parents[edge.target.index] = current.node;
        }
    }
    if (field.status == PathStatus::Running) {
        field.status = cost_pruned ? PathStatus::BudgetExceeded : PathStatus::Success;
        field.limit = cost_pruned ? SearchLimit::Cost : SearchLimit::None;
    }
    std::sort(field.samples.begin(), field.samples.end(), [](const auto& left, const auto& right) {
        return left.node < right.node;
    });
    return field;
}

NodeId nearest_reachable(const NavigationGraph& graph, const DistanceField& field,
                         math::Vec3 position, double radius) noexcept {
    if (field.graph_revision != graph.revision() || !math::finite(position) || std::isnan(radius) || radius < 0.0 ||
        (field.status != PathStatus::Success && field.status != PathStatus::BudgetExceeded)) {
        return {};
    }
    NodeId best{};
    for (const auto& sample : field.samples) {
        const auto* node = graph.node(sample.node);
        if (node == nullptr || !node->enabled) {
            continue;
        }
        const auto distance = safe_distance(node->position, position);
        if (std::isfinite(distance) && distance <= radius && (!best || distance < radius)) {
            best = sample.node;
            radius = distance;
        }
    }
    return best;
}

ComponentMap connected_components(const NavigationGraph& graph, std::size_t node_budget, std::size_t edge_budget) {
    ComponentMap result;
    result.graph_revision = graph.revision();
    if (graph.slot_count() > node_budget || graph.edge_count() > edge_budget) {
        result.status = PathStatus::BudgetExceeded;
        return result;
    }
    std::vector<std::uint32_t> parents(graph.slot_count() + 1);
    for (std::size_t index = 1; index < parents.size(); ++index) {
        parents[index] = static_cast<std::uint32_t>(index);
    }
    const auto root = [&](std::uint32_t index) {
        while (parents[index] != index) {
            parents[index] = parents[parents[index]];
            index = parents[index];
        }
        return index;
    };
    for (std::size_t index = 1; index < parents.size(); ++index) {
        const auto id = graph.node_at(static_cast<std::uint32_t>(index));
        if (!active(graph, id)) {
            continue;
        }
        for (const auto edge : graph.edges(id)) {
            if (active(graph, edge.target)) {
                const auto first = root(id.index);
                const auto second = root(edge.target.index);
                parents[std::max(first, second)] = std::min(first, second);
            }
        }
    }
    result.labels.resize(parents.size());
    for (std::size_t index = 1; index < parents.size(); ++index) {
        if (!active(graph, graph.node_at(static_cast<std::uint32_t>(index)))) {
            continue;
        }
        const auto representative = root(static_cast<std::uint32_t>(index));
        if (result.labels[representative] == 0) {
            result.labels[representative] = static_cast<std::uint32_t>(++result.components);
        }
        result.labels[index] = result.labels[representative];
    }
    result.status = PathStatus::Success;
    return result;
}

GridMap::GridMap(std::size_t max_cells) : max_cells_(max_cells) {
    if (max_cells > hard_node_limit) {
        throw std::invalid_argument("navigation grid exceeds supported cell capacity");
    }
}

bool GridMap::configure(std::uint32_t width, std::uint32_t height, double cell_size,
                        math::Vec3 origin, GridNeighbors neighbors, DiagonalRule diagonals) {
    if (width == 0 || height == 0 || width > hard_node_limit || height > hard_node_limit ||
        static_cast<std::size_t>(width) > max_cells_ / height || !std::isfinite(cell_size) || cell_size <= 0.0 ||
        !math::finite(origin) || origin.x + cell_size * 0.5 <= origin.x || origin.z + cell_size * 0.5 <= origin.z ||
        !std::isfinite(origin.x + static_cast<double>(width) * cell_size) ||
        !std::isfinite(origin.z + static_cast<double>(height) * cell_size) ||
        !std::isfinite(cell_size * std::sqrt(2.0)) || revision_ == last_revision ||
        (neighbors != GridNeighbors::Four && neighbors != GridNeighbors::Eight) ||
        (diagonals != DiagonalRule::Allow && diagonals != DiagonalRule::AtLeastOneOpen && diagonals != DiagonalRule::BothOpen)) {
        return false;
    }
    std::vector<GridCell> replacement(static_cast<std::size_t>(width) * height);
    cells_.swap(replacement);
    width_ = width;
    height_ = height;
    cell_size_ = cell_size;
    origin_ = origin;
    neighbors_ = neighbors;
    diagonals_ = diagonals;
    ++revision_;
    return true;
}

std::optional<std::size_t> GridMap::offset(GridCoordinate coordinate_value) const noexcept {
    if (coordinate_value.x < 0 || coordinate_value.y < 0 || static_cast<std::uint32_t>(coordinate_value.x) >= width_ ||
        static_cast<std::uint32_t>(coordinate_value.y) >= height_) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(coordinate_value.y) * width_ + static_cast<std::size_t>(coordinate_value.x);
}
const GridCell* GridMap::cell(GridCoordinate coordinate_value) const noexcept {
    const auto index = offset(coordinate_value);
    return index ? &cells_[*index] : nullptr;
}
bool GridMap::open(GridCoordinate coordinate_value) const noexcept {
    const auto* value = cell(coordinate_value);
    return value != nullptr && value->walkable;
}
bool GridMap::set_cell(GridCoordinate coordinate_value, GridCell value) {
    const auto index = offset(coordinate_value);
    if (!index || !std::isfinite(value.cost) || value.cost <= 0.0 ||
        !std::isfinite(value.cost * cell_size_ * std::sqrt(2.0)) || revision_ == last_revision) {
        return false;
    }
    if (cells_[*index].walkable != value.walkable || cells_[*index].cost != value.cost) {
        cells_[*index] = value;
        ++revision_;
    }
    return true;
}
bool GridMap::fill(GridCell value) {
    if (!std::isfinite(value.cost) || value.cost <= 0.0 ||
        !std::isfinite(value.cost * cell_size_ * std::sqrt(2.0)) || revision_ == last_revision) {
        return false;
    }
    std::fill(cells_.begin(), cells_.end(), value);
    ++revision_;
    return true;
}
std::optional<GridCoordinate> GridMap::coordinate(math::Vec3 position) const noexcept {
    if (!math::finite(position)) {
        return std::nullopt;
    }
    const auto x = std::floor((position.x - origin_.x) / cell_size_);
    const auto y = std::floor((position.z - origin_.z) / cell_size_);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0 || x >= width_ || y >= height_) {
        return std::nullopt;
    }
    return GridCoordinate{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
}
math::Vec3 GridMap::center(GridCoordinate coordinate_value) const noexcept {
    return {origin_.x + (static_cast<double>(coordinate_value.x) + 0.5) * cell_size_, origin_.y,
            origin_.z + (static_cast<double>(coordinate_value.y) + 0.5) * cell_size_};
}
bool GridMap::diagonal_open(GridCoordinate from, GridCoordinate to) const noexcept {
    if (diagonals_ == DiagonalRule::Allow) {
        return true;
    }
    const bool first = open({to.x, from.y});
    const bool second = open({from.x, to.y});
    return diagonals_ == DiagonalRule::BothOpen ? first && second : first || second;
}

std::size_t GridMap::neighbors(GridCoordinate from, std::span<GridNeighbor> output) const noexcept {
    if (!open(from)) {
        return 0;
    }
    constexpr std::array<GridCoordinate, 8> directions{{{1, 0}, {0, 1}, {-1, 0}, {0, -1},
                                                     {1, 1}, {-1, 1}, {-1, -1}, {1, -1}}};
    const std::size_t count = neighbors_ == GridNeighbors::Four ? 4 : 8;
    std::size_t written = 0;
    for (std::size_t index = 0; index < count && written < output.size(); ++index) {
        const GridCoordinate next{from.x + directions[index].x, from.y + directions[index].y};
        if (!open(next) || (index >= 4 && !diagonal_open(from, next))) {
            continue;
        }
        const double cost = cell(next)->cost * cell_size_ * (index >= 4 ? std::sqrt(2.0) : 1.0);
        output[written++] = {next, cost};
    }
    return written;
}

bool GridMap::line_of_sight(GridCoordinate from, GridCoordinate to, std::size_t cell_budget) const noexcept {
    return open(from) && open(to) && line_of_sight(center(from), center(to), cell_budget);
}
bool GridMap::line_of_sight(math::Vec3 from, math::Vec3 to, std::size_t cell_budget) const noexcept {
    const auto first = coordinate(from);
    const auto last = coordinate(to);
    if (!first || !last || !open(*first) || !open(*last) || cell_budget == 0) {
        return false;
    }
    auto current = *first;
    const double dx = to.x - from.x;
    const double dz = to.z - from.z;
    if (!std::isfinite(dx) || !std::isfinite(dz)) {
        return false;
    }
    const std::int32_t sx = dx > 0.0 ? 1 : (dx < 0.0 ? -1 : 0);
    const std::int32_t sy = dz > 0.0 ? 1 : (dz < 0.0 ? -1 : 0);
    const double tx = sx == 0 ? math::infinity : cell_size_ / std::abs(dx);
    const double ty = sy == 0 ? math::infinity : cell_size_ / std::abs(dz);
    const double boundary_x = origin_.x + static_cast<double>(current.x + (sx > 0 ? 1 : 0)) * cell_size_;
    const double boundary_y = origin_.z + static_cast<double>(current.y + (sy > 0 ? 1 : 0)) * cell_size_;
    double next_x = sx == 0 ? math::infinity : (boundary_x - from.x) / dx;
    double next_y = sy == 0 ? math::infinity : (boundary_y - from.z) / dz;
    while (current != *last) {
        if (--cell_budget == 0) {
            return false;
        }
        auto next = current;
        if (next_x == next_y) {
            next.x += sx;
            next.y += sy;
            if (!diagonal_open(current, next)) {
                return false;
            }
            next_x += tx;
            next_y += ty;
        } else if (next_x < next_y) {
            next.x += sx;
            next_x += tx;
        } else {
            next.y += sy;
            next_y += ty;
        }
        if (!open(next) || next == current) {
            return false;
        }
        current = next;
    }
    return true;
}
std::uint32_t GridMap::width() const noexcept { return width_; }
std::uint32_t GridMap::height() const noexcept { return height_; }
std::size_t GridMap::size() const noexcept { return cells_.size(); }
std::uint64_t GridMap::revision() const noexcept { return revision_; }

bool GridMap::bake(NavigationGraph& output, std::vector<NodeId>& cell_nodes) const {
    if (cells_.empty()) {
        return false;
    }
    struct Link { std::size_t from; std::size_t to; double cost; };
    std::vector<NodeId> mapping(cells_.size());
    std::vector<Link> links;
    std::size_t nodes = 0;
    std::size_t retiring = output.retired_slots();
    for (std::size_t index = 1; index <= output.slot_count(); ++index) {
        if (output.node_at(static_cast<std::uint32_t>(index)).generation == last_revision) {
            ++retiring;
        }
    }
    const auto& limits = output.limits();
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        if (!cells_[index].walkable) {
            continue;
        }
        ++nodes;
        const GridCoordinate from{static_cast<std::int32_t>(index % width_), static_cast<std::int32_t>(index / width_)};
        std::array<GridNeighbor, 8> adjacent{};
        const auto count = neighbors(from, adjacent);
        if (nodes > limits.max_nodes - retiring || count > limits.max_edges_per_node || count > limits.max_edges - links.size()) {
            return false;
        }
        for (std::size_t neighbor = 0; neighbor < count; ++neighbor) {
            links.push_back({index, *offset(adjacent[neighbor].coordinate), adjacent[neighbor].cost});
        }
    }
    const auto changes = static_cast<std::uint64_t>(nodes) + static_cast<std::uint64_t>(links.size()) + 1;
    if (output.revision() > last_revision - changes) {
        return false;
    }
    if (!output.clear()) {
        return false;
    }
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        if (cells_[index].walkable) {
            const GridCoordinate location{static_cast<std::int32_t>(index % width_), static_cast<std::int32_t>(index / width_)};
            mapping[index] = output.add_node(center(location));
            if (!mapping[index]) {
                throw std::logic_error("validated grid node capacity changed during bake");
            }
        }
    }
    for (const auto& link : links) {
        if (!output.connect(mapping[link.from], mapping[link.to], link.cost)) {
            throw std::logic_error("validated grid edge capacity changed during bake");
        }
    }
    cell_nodes.swap(mapping);
    return true;
}

bool smooth_path(PathResult& path, const GridMap& grid, std::size_t sight_budget) {
    if (path.status != PathStatus::Success || path.points.empty() || path.points.size() > hard_node_limit) {
        return false;
    }
    std::vector<GridCoordinate> coordinates;
    coordinates.reserve(path.points.size());
    for (const auto point : path.points) {
        const auto location = grid.coordinate(point);
        if (!location || !grid.cell(*location)->walkable) {
            return false;
        }
        coordinates.push_back(*location);
    }
    std::vector<math::Vec3> replacement{path.points.front()};
    std::size_t anchor = 0;
    while (anchor + 1 < path.points.size()) {
        std::size_t candidate = path.points.size() - 1;
        for (;;) {
            const auto from = coordinates[anchor];
            const auto to = coordinates[candidate];
            const auto cells = static_cast<std::size_t>(std::abs(to.x - from.x)) +
                static_cast<std::size_t>(std::abs(to.y - from.y)) + 1;
            if (cells > sight_budget) {
                return false;
            }
            sight_budget -= cells;
            if (grid.line_of_sight(path.points[anchor], path.points[candidate], cells)) {
                break;
            }
            if (candidate == anchor + 1) {
                return false;
            }
            --candidate;
        }
        replacement.push_back(path.points[candidate]);
        anchor = candidate;
    }
    path.points.swap(replacement);
    return true;
}

Navigator::Navigator(NavigatorLimits limits)
    : limits_(limits), graph_(limits.graph), grid_(limits.graph.max_nodes) {
    if (limits.max_requests == 0 || limits.max_requests > 4096 || limits.max_results == 0 ||
        limits.max_results > 4096 || limits.work_per_frame == 0 || limits.work_per_frame > hard_work_limit ||
        limits.work_per_request == 0 || limits.work_per_request > hard_work_limit ||
        limits.follower_waypoints_per_frame == 0 || limits.follower_waypoints_per_frame > hard_node_limit) {
        throw std::invalid_argument("invalid navigator budgets");
    }
}
NavigationGraph& Navigator::graph() noexcept { return graph_; }
const NavigationGraph& Navigator::graph() const noexcept { return graph_; }
GridMap& Navigator::grid() noexcept { return grid_; }
const GridMap& Navigator::grid() const noexcept { return grid_; }

bool Navigator::bake_grid() {
    if (!grid_.bake(graph_, grid_nodes_)) {
        return false;
    }
    baked_grid_revision_ = grid_.revision();
    baked_graph_revision_ = graph_.revision();
    return true;
}
NodeId Navigator::grid_node(GridCoordinate coordinate_value) const noexcept {
    if (baked_grid_revision_ != grid_.revision() || baked_graph_revision_ != graph_.revision() ||
        grid_.cell(coordinate_value) == nullptr) {
        return {};
    }
    const auto index = static_cast<std::size_t>(coordinate_value.y) * grid_.width() +
        static_cast<std::size_t>(coordinate_value.x);
    return index < grid_nodes_.size() ? grid_nodes_[index] : NodeId{};
}

RequestId Navigator::submit(NodeId start, NodeId goal, SearchOptions options) {
    if (!valid_options(options) || requests_.size() >= limits_.max_requests || next_request_ == last_revision ||
        !active(graph_, start) || !active(graph_, goal) || graph_.slot_count() > options.max_storage_nodes ||
        (options.expected_revision != 0 && options.expected_revision != graph_.revision())) {
        return 0;
    }
    options.expected_revision = graph_.revision();
    requests_.push_back({next_request_, start, goal, options, {}});
    return next_request_++;
}

bool Navigator::cancel(RequestId request_id) {
    const auto found = std::find_if(requests_.begin(), requests_.end(),
        [request_id](const auto& request) { return request.id == request_id; });
    if (found == requests_.end()) {
        return false;
    }
    PathResult path;
    path.status = PathStatus::Cancelled;
    path.graph_revision = found->options.expected_revision;
    path.start = found->start;
    path.goal = found->goal;
    completed_.push_back({request_id, std::move(path)});
    requests_.erase(found);
    if (completed_.size() > limits_.max_results) {
        completed_.pop_front();
    }
    return true;
}
const PathResult* Navigator::result(RequestId request_id) const noexcept {
    const auto found = std::find_if(completed_.begin(), completed_.end(),
        [request_id](const auto& completed) { return completed.id == request_id; });
    return found == completed_.end() ? nullptr : &found->path;
}
std::optional<PathResult> Navigator::take_result(RequestId request_id) {
    const auto found = std::find_if(completed_.begin(), completed_.end(),
        [request_id](const auto& completed) { return completed.id == request_id; });
    if (found == completed_.end()) {
        return std::nullopt;
    }
    auto path = std::move(found->path);
    completed_.erase(found);
    if (path.graph_revision != graph_.revision() && path.status == PathStatus::Success) {
        path.status = PathStatus::StaleGraph;
    }
    return path;
}
std::size_t Navigator::pending() const noexcept { return requests_.size(); }

std::size_t Navigator::update(std::size_t work_budget) {
    std::size_t remaining = work_budget == 0 ? limits_.work_per_frame : std::min(work_budget, limits_.work_per_frame);
    std::size_t used = 0;
    while (!requests_.empty() && remaining > 0) {
        auto& request = requests_.front();
        if (!request.search) {
            request.search = std::make_unique<PathSearch>();
            const auto status = request.search->begin(graph_, request.start, request.goal, request.options);
            if (status != PathStatus::Running) {
                PathResult rejected;
                rejected.status = request.options.expected_revision != graph_.revision() ? PathStatus::StaleGraph : status;
                rejected.graph_revision = request.options.expected_revision;
                rejected.start = request.start;
                rejected.goal = request.goal;
                rejected.limit = status == PathStatus::BudgetExceeded ? SearchLimit::Storage : SearchLimit::None;
                completed_.push_back({request.id, std::move(rejected)});
                requests_.pop_front();
                if (completed_.size() > limits_.max_results) {
                    completed_.pop_front();
                }
                continue;
            }
        }
        const auto previous = request.search->result().work;
        const auto status = request.search->step(std::min(remaining, limits_.work_per_request));
        const auto consumed = request.search->result().work - previous;
        remaining -= consumed;
        used += consumed;
        if (status == PathStatus::Running) {
            if (consumed == 0) {
                break;
            }
            continue;
        }
        completed_.push_back({request.id, request.search->result()});
        requests_.pop_front();
        if (completed_.size() > limits_.max_results) {
            completed_.pop_front();
        }
    }
    return used;
}

bool Navigator::follow(ecs::Registry& registry, ecs::Entity entity, const PathResult& path, double speed) {
    if (!registry.alive(entity) || registry.iterating() || path.status != PathStatus::Success ||
        path.graph_revision != graph_.revision() || path.points.empty() || path.points.size() > hard_node_limit ||
        !std::isfinite(speed) || speed < 0.0 ||
        std::any_of(path.points.begin(), path.points.end(), [](math::Vec3 point) { return !math::finite(point); })) {
        return false;
    }
    PathFollower follower;
    follower.points = path.points;
    follower.graph_revision = path.graph_revision;
    follower.speed = speed;
    follower.state = FollowerState::Following;
    registry.emplace_or_replace<PathFollower>(entity, std::move(follower));
    return true;
}

std::size_t Navigator::update_followers(scene::Scene& scene, double delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0) {
        return 0;
    }
    auto& registry = scene.registry();
    std::size_t moved = 0;
    for (const auto entity : registry.view<PathFollower>()) {
        auto& follower = *registry.get<PathFollower>(entity);
        follower.desired_velocity = {};
        if (follower.pause) {
            follower.state = FollowerState::Paused;
            (void)registry.mark_changed<PathFollower>(entity);
            continue;
        }
        if (follower.require_current_graph && follower.graph_revision != graph_.revision()) {
            follower.state = FollowerState::StalePath;
            (void)registry.mark_changed<PathFollower>(entity);
            continue;
        }
        if (!std::isfinite(follower.speed) || follower.speed < 0.0 ||
            !std::isfinite(follower.stopping_distance) || follower.stopping_distance < 0.0 ||
            !std::isfinite(follower.waypoint_distance) || follower.waypoint_distance < 0.0 ||
            follower.points.empty() || follower.points.size() > hard_node_limit || follower.waypoint > follower.points.size() ||
            !std::isfinite(follower.speed * delta_seconds)) {
            follower.state = FollowerState::InvalidPath;
            (void)registry.mark_changed<PathFollower>(entity);
            continue;
        }
        if (!scene.contains(entity) || !scene.effective_enabled(entity)) {
            follower.state = FollowerState::Paused;
            (void)registry.mark_changed<PathFollower>(entity);
            continue;
        }
        const auto transform = scene.world_transform(entity);
        if (!transform) {
            follower.state = FollowerState::SceneRejected;
            (void)registry.mark_changed<PathFollower>(entity);
            continue;
        }
        const bool body_owned = registry.has<physics::BodyComponent>(entity);
        const auto origin = transform->position;
        auto position = origin;
        auto waypoint = follower.waypoint;
        double available = follower.speed * delta_seconds;
        std::size_t transitions = 0;
        follower.state = FollowerState::Following;
        while (waypoint < follower.points.size() && transitions < limits_.follower_waypoints_per_frame) {
            const auto target = follower.points[waypoint];
            const double distance = safe_distance(position, target);
            if (!math::finite(target) || !std::isfinite(distance)) {
                follower.state = FollowerState::InvalidPath;
                break;
            }
            const bool last = waypoint + 1 == follower.points.size();
            const double tolerance = last ? follower.stopping_distance : follower.waypoint_distance;
            if (distance <= tolerance) {
                ++waypoint;
                ++transitions;
                continue;
            }
            const auto direction = (target - position) / distance;
            const double movement = std::min(available, last ? distance - tolerance : distance);
            if (body_owned) {
                follower.desired_velocity = direction * (movement / delta_seconds);
                follower.state = FollowerState::ControllerRequired;
                break;
            }
            position += direction * movement;
            available -= movement;
            if (movement >= (last ? distance - tolerance : distance)) {
                ++waypoint;
                ++transitions;
            } else {
                break;
            }
        }
        if (follower.state == FollowerState::InvalidPath) {
            (void)registry.mark_changed<PathFollower>(entity);
            continue;
        }
        if (!body_owned && position != origin) {
            auto pose = *transform;
            pose.position = position;
            if (follower.face_movement) {
                pose.rotation = math::look_rotation(position - origin);
            }
            const auto changed = follower.face_movement ? scene.set_world_transform(entity, pose) : scene.set_world_position(entity, position);
            if (!changed) {
                follower.state = FollowerState::SceneRejected;
                (void)registry.mark_changed<PathFollower>(entity);
                continue;
            }
            follower.desired_velocity = (position - origin) / delta_seconds;
            ++moved;
        }
        follower.waypoint = waypoint;
        if (waypoint == follower.points.size()) {
            follower.state = FollowerState::Arrived;
            follower.desired_velocity = {};
        } else if (body_owned && follower.state != FollowerState::ControllerRequired) {
            follower.state = FollowerState::ControllerRequired;
        }
        (void)registry.mark_changed<PathFollower>(entity);
    }
    return moved;
}

void register_pathfinding(core::Kernel& kernel) {
    if (kernel.services().find<Navigator>() != nullptr) {
        return;
    }
    (void)kernel.services().emplace<Navigator>();
    (void)kernel.register_module({"navigation.pathfinding", "Bounded graph and grid navigation", 1});
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        (void)registry->register_component<PathFollower>("navigation.path_follower");
    }
    (void)kernel.add_system({"navigation.update", core::Phase::PreSimulation, -8500, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& navigation = host.services().get<Navigator>();
            const auto work = navigation.update();
            std::size_t followers = 0;
            if (auto* active_scene = host.services().find<scene::Scene>()) {
                followers = navigation.update_followers(*active_scene, frame.delta_seconds);
            }
            host.metric("navigation.work", static_cast<double>(work));
            host.metric("navigation.pending", static_cast<double>(navigation.pending()));
            host.metric("navigation.followers_moved", static_cast<double>(followers));
        });
    (void)kernel.add_command("navigation.grid", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& navigation = host.services().get<Navigator>();
        if (args.size() == 3 && args[0] == "node") {
            GridCoordinate location;
            if (!parse_number(args[1], location.x) || !parse_number(args[2], location.y)) {
                return {false, "invalid grid coordinate"};
            }
            const auto id = navigation.grid_node(location);
            return {id.valid(), id ? handle_text(id) : "grid node unavailable; check walkability and bake revision"};
        }
        if (args.size() == 5 && args[0] == "cell") {
            GridCoordinate location;
            std::uint32_t walkable = 0;
            double cost = 0.0;
            if (!parse_number(args[1], location.x) || !parse_number(args[2], location.y) ||
                !parse_number(args[3], walkable) || walkable > 1 || !parse_number(args[4], cost) ||
                !navigation.grid().set_cell(location, {walkable != 0, cost})) {
                return {false, "invalid grid cell"};
            }
        } else {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            double size = 1.0;
            if ((args.size() != 2 && args.size() != 3) || !parse_number(args[0], width) ||
                !parse_number(args[1], height) || (args.size() == 3 && !parse_number(args[2], size)) ||
                !navigation.grid().configure(width, height, size)) {
                return {false, "usage: navigation.grid width height [cell_size] | cell x y walkable cost | node x y"};
            }
        }
        if (!navigation.bake_grid()) {
            return {false, "grid exceeds graph capacity; previous graph retained"};
        }
        return {true, "grid baked: " + std::to_string(navigation.graph().size()) + " nodes, revision " +
            std::to_string(navigation.graph().revision())};
    }, "Create a grid or change a cell, then bake the navigation graph");
    (void)kernel.add_command("navigation.node", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& graph = host.services().get<Navigator>().graph();
        math::Vec3 position;
        if (args.size() != 3 || !parse_number(args[0], position.x) || !parse_number(args[1], position.y) ||
            !parse_number(args[2], position.z)) {
            return {false, "usage: navigation.node x y z"};
        }
        const auto id = graph.add_node(position);
        return {id.valid(), id ? handle_text(id) : "node rejected"};
    }, "Add an enabled navigation node and return index:generation");
    (void)kernel.add_command("navigation.edge", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        NodeId from;
        NodeId to;
        double cost = 0.0;
        if ((args.size() != 3 && args.size() != 4) || !parse_handle(args[0], from) || !parse_handle(args[1], to) ||
            !parse_number(args[2], cost) || (args.size() == 4 && args[3] != "both")) {
            return {false, "usage: navigation.edge from_index:generation to_index:generation cost [both]"};
        }
        const bool connected = host.services().get<Navigator>().graph().connect(from, to, cost, args.size() == 4);
        return {connected, connected ? "edge connected" : "edge rejected"};
    }, "Connect generation-checked nodes with a nonnegative traversal cost");
    (void)kernel.add_command("navigation.find", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& navigation = host.services().get<Navigator>();
        if (args.size() == 2 && args[0] == "cancel") {
            RequestId request = 0;
            const bool cancelled = parse_number(args[1], request) && navigation.cancel(request);
            return {cancelled, cancelled ? "request cancelled" : "request not pending"};
        }
        if (args.size() == 2 && args[0] == "result") {
            RequestId request = 0;
            if (!parse_number(args[1], request)) {
                return {false, "invalid request ID"};
            }
            const auto* path = navigation.result(request);
            if (path == nullptr) {
                return {false, "request pending, unknown, or evicted"};
            }
            const auto status = path->status == PathStatus::Success && path->graph_revision != navigation.graph().revision()
                ? PathStatus::StaleGraph : path->status;
            return {status == PathStatus::Success, std::string(describe(status)) + ", limit " + std::string(describe(path->limit)) +
                ", points " + std::to_string(path->points.size()) + ", cost " + std::to_string(path->cost)};
        }
        NodeId start;
        NodeId goal;
        if (args.size() != 2 || !parse_handle(args[0], start) || !parse_handle(args[1], goal)) {
            return {false, "usage: navigation.find start_index:generation goal_index:generation | result request_id | cancel request_id"};
        }
        const auto request = navigation.submit(start, goal);
        return {request != 0, request != 0 ? "queued request " + std::to_string(request) : "request rejected"};
    }, "Queue a bounded A* request or inspect its completed result");
    (void)kernel.add_command("navigation.follow", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto* active_scene = host.services().find<scene::Scene>();
        RequestId request = 0;
        double speed = 3.0;
        if (active_scene == nullptr || (args.size() != 2 && args.size() != 3) || !parse_number(args[1], request) ||
            (args.size() == 3 && !parse_number(args[2], speed))) {
            return {false, "usage: navigation.follow scene_path request_id [speed]"};
        }
        const auto entity = active_scene->find_path(args[0]);
        auto& navigation = host.services().get<Navigator>();
        const auto* path = navigation.result(request);
        const bool accepted = entity && path != nullptr && navigation.follow(active_scene->registry(), entity.entity, *path, speed);
        return {accepted, accepted ? "path follower attached; physics bodies expose desired velocity to controllers" : "follower rejected"};
    }, "Attach a completed path to an existing scene entity");
}

} // namespace ge::navigation
