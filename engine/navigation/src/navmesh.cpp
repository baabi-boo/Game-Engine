#include <ge/navigation/navmesh.hpp>

#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace ge::navigation {
namespace {
constexpr std::size_t hard_polygons = 262144;
constexpr std::size_t hard_vertices = 1048576;
constexpr std::size_t hard_work = 67108864;
constexpr double coordinate_limit = 1.0e9;
constexpr double geometric_epsilon = 1.0e-9;
constexpr auto final_revision = std::numeric_limits<std::uint64_t>::max();

struct WorkBudget {
    std::size_t limit{};
    std::size_t used{};
    bool exhausted{};
    bool charge(std::size_t amount = 1) noexcept {
        if (used > limit || amount > limit - used) {
            exhausted = true;
            return false;
        }
        used += amount;
        return true;
    }
};

bool supported_point(math::Vec3 point) noexcept {
    return math::finite(point) && std::abs(point.x) <= coordinate_limit &&
        std::abs(point.y) <= coordinate_limit && std::abs(point.z) <= coordinate_limit;
}

double nav_distance(math::Vec3 first, math::Vec3 second) noexcept {
    return std::hypot(first.x - second.x, first.y - second.y, first.z - second.z);
}

double orient(math::Vec3 a, math::Vec3 b, math::Vec3 c) noexcept {
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

bool equal_xz(math::Vec3 first, math::Vec3 second) noexcept {
    return std::hypot(first.x - second.x, first.z - second.z) <= geometric_epsilon;
}

bool affine(const math::Mat4& value) noexcept {
    return math::finite(value) && std::abs(value(3, 0)) <= geometric_epsilon &&
        std::abs(value(3, 1)) <= geometric_epsilon &&
        std::abs(value(3, 2)) <= geometric_epsilon &&
        std::abs(value(3, 3) - 1.0) <= geometric_epsilon;
}

bool valid_build(const NavBuildOptions& options) noexcept {
    return std::isfinite(options.weld_tolerance) && options.weld_tolerance >= 1.0e-7 &&
        options.weld_tolerance <= 1.0 && std::isfinite(options.minimum_projected_area) &&
        options.minimum_projected_area >= 1.0e-10 && options.minimum_projected_area <= 1.0e12 &&
        std::isfinite(options.maximum_slope_degrees) && options.maximum_slope_degrees >= 0.0 &&
        options.maximum_slope_degrees < 90.0;
}

std::optional<double> height_at(const NavPolygon& polygon, std::span<const math::Vec3> vertices,
                                math::Vec3 point, WorkBudget& work) {
    const auto first = vertices[polygon.vertices.front()];
    for (std::size_t index = 1; index + 1 < polygon.vertices.size(); ++index) {
        if (!work.charge()) {
            return std::nullopt;
        }
        const auto second = vertices[polygon.vertices[index]];
        const auto third = vertices[polygon.vertices[index + 1]];
        const double denominator = orient(first, second, third);
        const double a = orient(point, second, third) / denominator;
        const double b = orient(first, point, third) / denominator;
        const double c = 1.0 - a - b;
        if (a >= -geometric_epsilon && b >= -geometric_epsilon && c >= -geometric_epsilon) {
            return first.y * a + second.y * b + third.y * c;
        }
    }
    return std::nullopt;
}

std::optional<math::Vec3> closest_on_polygon(const NavPolygon& polygon,
    std::span<const math::Vec3> vertices, math::Vec3 point, WorkBudget& work) {
    std::optional<math::Vec3> best;
    double best_distance = math::infinity;
    const auto first = vertices[polygon.vertices.front()];
    for (std::size_t index = 1; index + 1 < polygon.vertices.size(); ++index) {
        if (!work.charge()) {
            return std::nullopt;
        }
        const math::Triangle triangle{first, vertices[polygon.vertices[index]],
                                      vertices[polygon.vertices[index + 1]]};
        const auto candidate = math::closest_point(triangle, point);
        const double candidate_distance = nav_distance(candidate, point);
        if (candidate_distance < best_distance) {
            best = candidate;
            best_distance = candidate_distance;
        }
    }
    return best;
}

NavLocation locate_in(std::span<const NavPolygon> polygons, std::span<const math::Vec3> vertices,
    math::Vec3 point, double maximum, const NavFilter* filter, bool containing_only, WorkBudget& work) {
    NavLocation result;
    double best_distance = maximum;
    for (const auto& polygon : polygons) {
        if (!work.charge()) {
            return {NavStatus::BudgetExceeded, {}, {}, 0.0, work.used};
        }
        if (filter != nullptr && !filter->allows(polygon.flags)) {
            continue;
        }
        std::optional<math::Vec3> projected;
        if (containing_only) {
            if (const auto height = height_at(polygon, vertices, point, work)) {
                projected = math::Vec3{point.x, *height, point.z};
            }
        } else {
            projected = closest_on_polygon(polygon, vertices, point, work);
        }
        if (work.exhausted) {
            return {NavStatus::BudgetExceeded, {}, {}, 0.0, work.used};
        }
        if ((!projected && !containing_only) || (projected && !math::finite(*projected))) {
            return {NavStatus::NumericFailure, {}, {}, 0.0, work.used};
        }
        if (!projected) {
            continue;
        }
        const double candidate_distance = nav_distance(point, *projected);
        if (candidate_distance <= best_distance && (!result || candidate_distance < best_distance)) {
            result = {NavStatus::Success, polygon.reference, *projected, candidate_distance, 0};
            best_distance = candidate_distance;
        }
    }
    result.work = work.used;
    return result;
}

NavStatus search_status(PathStatus status) noexcept {
    switch (status) {
    case PathStatus::Success: return NavStatus::Success;
    case PathStatus::NoPath: return NavStatus::NoPath;
    case PathStatus::StaleGraph: return NavStatus::StaleMesh;
    case PathStatus::Cancelled: return NavStatus::Cancelled;
    case PathStatus::BudgetExceeded: return NavStatus::BudgetExceeded;
    default: return NavStatus::InvalidInput;
    }
}

bool append_corner(std::vector<NavPathPoint>& output, NavPathPoint point, std::size_t maximum) {
    if (!output.empty() && nav_distance(output.back().position, point.position) <= geometric_epsilon) {
        if (point.kind == NavPointKind::Surface) {
            return true;
        }
        if (output.back().kind == NavPointKind::Surface) {
            output.back() = point;
            return true;
        }
    }
    if (output.size() >= maximum) {
        return false;
    }
    output.push_back(point);
    return true;
}

NavStatus funnel(std::span<const NavPortal> portals, NavPathPoint start, NavPathPoint goal,
    std::vector<NavPathPoint>& output, std::size_t maximum, WorkBudget& work) {
    if (!append_corner(output, start, maximum)) {
        return NavStatus::BudgetExceeded;
    }
    auto apex = start.position;
    auto left = apex;
    auto right = apex;
    std::size_t apex_index = 0;
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    const auto reference = [&](std::size_t index) {
        return index == 0 ? start.polygon : index <= portals.size() ? portals[index - 1].to : goal.polygon;
    };
    for (std::size_t index = 1; index <= portals.size() + 1; ++index) {
        if (!work.charge()) {
            return NavStatus::BudgetExceeded;
        }
        const auto next_left = index <= portals.size() ? portals[index - 1].left : goal.position;
        const auto next_right = index <= portals.size() ? portals[index - 1].right : goal.position;
        if (orient(apex, right, next_right) >= 0.0) {
            if (equal_xz(apex, right) || orient(apex, left, next_right) < 0.0) {
                right = next_right;
                right_index = index;
            } else {
                if (!append_corner(output, {left, reference(left_index)}, maximum)) {
                    return NavStatus::BudgetExceeded;
                }
                apex = left;
                apex_index = left_index;
                left = apex;
                right = apex;
                left_index = apex_index;
                right_index = apex_index;
                index = apex_index;
                continue;
            }
        }
        if (orient(apex, left, next_left) <= 0.0) {
            if (equal_xz(apex, left) || orient(apex, right, next_left) > 0.0) {
                left = next_left;
                left_index = index;
            } else {
                if (!append_corner(output, {right, reference(right_index)}, maximum)) {
                    return NavStatus::BudgetExceeded;
                }
                apex = right;
                apex_index = right_index;
                left = apex;
                right = apex;
                left_index = apex_index;
                right_index = apex_index;
                index = apex_index;
            }
        }
    }
    return append_corner(output, goal, maximum) ? NavStatus::Success : NavStatus::BudgetExceeded;
}

template<class T>
bool parse_number(std::string_view text, T& output) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return false;
    }
    output = value;
    return true;
}

bool parse_point(core::CommandArgs args, std::size_t offset, math::Vec3& point) {
    return offset <= args.size() && args.size() - offset >= 3 &&
        parse_number(args[offset], point.x) && parse_number(args[offset + 1], point.y) &&
        parse_number(args[offset + 2], point.z) && supported_point(point);
}

std::string point_text(math::Vec3 point) {
    return std::to_string(point.x) + " " + std::to_string(point.y) + " " + std::to_string(point.z);
}

std::string reference_text(NavPolygonRef reference) {
    return std::to_string(reference.index) + ":" + std::to_string(reference.revision);
}

struct NavCommandState {
    NavPath last_path;
};
}

struct NavMesh::Data {
    struct Arc {
        std::uint32_t destination{};
        std::uint32_t portal{};
        double cost{};
    };
    std::vector<math::Vec3> vertices;
    std::vector<NavPolygon> polygons;
    std::vector<NavPortal> portals;
    std::vector<NavOffMeshLink> links;
    std::vector<std::vector<Arc>> arcs;
    std::vector<std::vector<std::uint32_t>> edge_portals;
    std::size_t boundary_edges{};
};

std::string_view describe(NavStatus status) noexcept {
    switch (status) {
    case NavStatus::Success: return "success";
    case NavStatus::Empty: return "empty navmesh";
    case NavStatus::InvalidInput: return "invalid input";
    case NavStatus::InvalidPolygon: return "invalid convex polygon";
    case NavStatus::NonManifold: return "nonmanifold or overlapping shared edge";
    case NavStatus::CapacityExceeded: return "capacity exceeded";
    case NavStatus::BudgetExceeded: return "query or build budget exceeded";
    case NavStatus::NoPolygon: return "no polygon within query bounds";
    case NavStatus::NoPath: return "no traversable corridor";
    case NavStatus::StaleMesh: return "stale navmesh revision";
    case NavStatus::StaleSource: return "baked source has changed";
    case NavStatus::Unsupported: return "unsupported navigation operation";
    case NavStatus::NumericFailure: return "numeric operation rejected";
    case NavStatus::RevisionExhausted: return "revision exhausted";
    case NavStatus::Cancelled: return "query cancelled";
    }
    return "unknown navigation status";
}

NavFilter::NavFilter() noexcept {
    area_costs.fill(1.0);
}

bool NavFilter::allows(std::uint64_t flags) const noexcept {
    return (flags & include_flags) != 0 && (flags & exclude_flags) == 0;
}

bool NavFilter::valid() const noexcept {
    return std::all_of(area_costs.begin(), area_costs.end(), [](double cost) {
        return std::isfinite(cost) && cost > 0.0 && cost <= 1.0e12;
    });
}

NavMesh::NavMesh(NavMeshLimits limits) : limits_(limits) {
    if (limits.max_vertices == 0 || limits.max_vertices > hard_vertices ||
        limits.max_polygons == 0 || limits.max_polygons > hard_polygons ||
        limits.max_vertices_per_polygon < 3 || limits.max_vertices_per_polygon > 64 ||
        limits.max_links > 65536 || limits.max_arcs > hard_work ||
        limits.max_arcs_per_polygon == 0 || limits.max_arcs_per_polygon > 1024 ||
        limits.max_build_work == 0 || limits.max_build_work > hard_work) {
        throw std::invalid_argument("navmesh limits exceed supported bounds");
    }
}

NavMesh::~NavMesh() = default;

NavResult NavMesh::rebuild(const NavMeshInput& input, NavBuildOptions options, NavMeshSource source) {
    return rebuild_impl(input, options, source, 0);
}

NavResult NavMesh::rebuild_impl(const NavMeshInput& input, NavBuildOptions options,
                               NavMeshSource source, std::size_t initial_work) {
    WorkBudget work{limits_.max_build_work, initial_work};
    const auto failure = [&](NavStatus status, std::size_t element = 0) {
        return NavResult{status, element, work.used};
    };
    if (revision_ == final_revision) {
        return failure(NavStatus::RevisionExhausted);
    }
    if (!valid_build(options) || !affine(source.transform) ||
        ((source.mesh.index != 0 || source.mesh.generation != 0) && !source.mesh) ||
        ((source.entity.index != 0 || source.entity.generation != 0) && !source.entity) ||
        (source.mesh && source.mesh_revision == 0) || (source.entity && !source.mesh)) {
        return failure(NavStatus::InvalidInput);
    }
    if (input.vertices.size() > limits_.max_vertices || input.polygons.size() > limits_.max_polygons ||
        input.links.size() > limits_.max_links) {
        return failure(NavStatus::CapacityExceeded);
    }
    if (input.polygons.empty() || input.vertices.empty()) {
        return failure(NavStatus::Empty);
    }
    auto candidate = std::make_unique<Data>();
    candidate->vertices.reserve(input.vertices.size());
    candidate->polygons.reserve(input.polygons.size());
    std::vector<std::uint32_t> remap;
    remap.reserve(input.vertices.size());
    using Cell = std::array<std::int64_t, 3>;
    std::map<Cell, std::vector<std::uint32_t>> cells;
    for (std::size_t index = 0; index < input.vertices.size(); ++index) {
        const auto vertex = input.vertices[index];
        if (!supported_point(vertex)) {
            return failure(NavStatus::InvalidInput, index);
        }
        const Cell cell{static_cast<std::int64_t>(std::floor(vertex.x / options.weld_tolerance)),
                        static_cast<std::int64_t>(std::floor(vertex.y / options.weld_tolerance)),
                        static_cast<std::int64_t>(std::floor(vertex.z / options.weld_tolerance))};
        std::uint32_t found = no_nav_link;
        for (std::int64_t x = -1; x <= 1; ++x) {
            for (std::int64_t y = -1; y <= 1; ++y) {
                for (std::int64_t z = -1; z <= 1; ++z) {
                    if (!work.charge()) {
                        return failure(NavStatus::BudgetExceeded, index);
                    }
                    const auto bucket = cells.find({cell[0] + x, cell[1] + y, cell[2] + z});
                    if (bucket == cells.end()) {
                        continue;
                    }
                    for (const auto match : bucket->second) {
                        if (!work.charge()) {
                            return failure(NavStatus::BudgetExceeded, index);
                        }
                        if (nav_distance(vertex, candidate->vertices[match]) <= options.weld_tolerance) {
                            found = std::min(found, match);
                        }
                    }
                }
            }
        }
        if (found == no_nav_link) {
            found = static_cast<std::uint32_t>(candidate->vertices.size());
            candidate->vertices.push_back(vertex);
            cells[cell].push_back(found);
        }
        remap.push_back(found);
    }
    const double slope_cosine = std::cos(options.maximum_slope_degrees * math::pi / 180.0);
    for (std::size_t index = 0; index < input.polygons.size(); ++index) {
        const auto& authored = input.polygons[index];
        if (authored.vertices.size() < 3 || authored.vertices.size() > limits_.max_vertices_per_polygon ||
            static_cast<std::size_t>(authored.area) >= nav_area_count) {
            return failure(NavStatus::InvalidPolygon, index);
        }
        NavPolygon polygon;
        polygon.reference = {static_cast<std::uint32_t>(index + 1), revision_ + 1};
        polygon.flags = authored.flags;
        polygon.area = authored.area;
        for (const auto vertex : authored.vertices) {
            if (!work.charge()) {
                return failure(NavStatus::BudgetExceeded, index);
            }
            if (vertex >= remap.size() || std::find(polygon.vertices.begin(), polygon.vertices.end(),
                                                  remap[vertex]) != polygon.vertices.end()) {
                return failure(NavStatus::InvalidPolygon, index);
            }
            polygon.vertices.push_back(remap[vertex]);
        }
        double area = 0.0;
        const auto origin = candidate->vertices[polygon.vertices.front()];
        for (std::size_t vertex = 1; vertex + 1 < polygon.vertices.size(); ++vertex) {
            area += orient(origin, candidate->vertices[polygon.vertices[vertex]],
                            candidate->vertices[polygon.vertices[vertex + 1]]);
        }
        if (std::abs(area) < options.minimum_projected_area * 2.0) {
            return failure(NavStatus::InvalidPolygon, index);
        }
        if (area < 0.0) {
            std::reverse(polygon.vertices.begin(), polygon.vertices.end());
        }
        for (std::size_t edge = 0; edge < polygon.vertices.size(); ++edge) {
            const auto a = candidate->vertices[polygon.vertices[edge]];
            const auto b = candidate->vertices[polygon.vertices[(edge + 1) % polygon.vertices.size()]];
            polygon.center += a / static_cast<double>(polygon.vertices.size());
            polygon.bounds = math::include(polygon.bounds, a);
            for (std::size_t other = 0; other < polygon.vertices.size(); ++other) {
                if (!work.charge()) {
                    return failure(NavStatus::BudgetExceeded, index);
                }
                if (other != edge && other != (edge + 1) % polygon.vertices.size() &&
                    orient(a, b, candidate->vertices[polygon.vertices[other]]) <= 0.0) {
                    return failure(NavStatus::InvalidPolygon, index);
                }
            }
        }
        const auto first = candidate->vertices[polygon.vertices.front()];
        for (std::size_t triangle = 1; triangle + 1 < polygon.vertices.size(); ++triangle) {
            if (!work.charge()) {
                return failure(NavStatus::BudgetExceeded, index);
            }
            const auto normal = math::cross(candidate->vertices[polygon.vertices[triangle]] - first,
                candidate->vertices[polygon.vertices[triangle + 1]] - first);
            const double normal_length = std::hypot(normal.x, normal.y, normal.z);
            if (std::abs(normal.y) < options.minimum_projected_area * 2.0 || normal_length <= 0.0 ||
                std::abs(normal.y) / normal_length + geometric_epsilon < slope_cosine) {
                return failure(NavStatus::InvalidPolygon, index);
            }
        }
        const auto center_height = height_at(polygon, candidate->vertices, polygon.center, work);
        if (!center_height) {
            return failure(work.exhausted ? NavStatus::BudgetExceeded : NavStatus::NumericFailure, index);
        }
        polygon.center.y = *center_height;
        candidate->polygons.push_back(std::move(polygon));
    }
    candidate->arcs.resize(candidate->polygons.size());
    candidate->edge_portals.resize(candidate->polygons.size());
    const auto add_portal = [&](std::size_t from, std::size_t to, math::Vec3 left, math::Vec3 right,
                                double cost, std::uint32_t link_index, std::uint64_t user_id) {
        if (candidate->portals.size() >= limits_.max_arcs ||
            candidate->arcs[from].size() >= limits_.max_arcs_per_polygon || !std::isfinite(cost)) {
            return false;
        }
        const auto portal_index = static_cast<std::uint32_t>(candidate->portals.size());
        candidate->portals.push_back({candidate->polygons[from].reference, candidate->polygons[to].reference,
                                     left, right, link_index, user_id});
        candidate->arcs[from].push_back({static_cast<std::uint32_t>(to), portal_index, cost});
        return true;
    };
    struct EdgeOwner {
        std::size_t polygon{};
        std::size_t edge{};
        std::uint32_t from{};
        bool paired{};
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, EdgeOwner> edges;
    for (std::size_t index = 0; index < candidate->polygons.size(); ++index) {
        const auto& polygon = candidate->polygons[index];
        candidate->edge_portals[index].assign(polygon.vertices.size(), no_nav_link);
        for (std::size_t edge = 0; edge < polygon.vertices.size(); ++edge) {
            if (!work.charge()) {
                return failure(NavStatus::BudgetExceeded, index);
            }
            const auto a = polygon.vertices[edge];
            const auto b = polygon.vertices[(edge + 1) % polygon.vertices.size()];
            const auto key = std::minmax(a, b);
            auto [position, inserted] = edges.try_emplace({key.first, key.second}, EdgeOwner{index, edge, a, false});
            if (inserted) {
                ++candidate->boundary_edges;
                continue;
            }
            auto& owner = position->second;
            if (owner.paired || owner.from == a) {
                return failure(NavStatus::NonManifold, index);
            }
            owner.paired = true;
            --candidate->boundary_edges;
            const auto midpoint = (candidate->vertices[a] + candidate->vertices[b]) * 0.5;
            const double cost = nav_distance(polygon.center, midpoint) +
                nav_distance(candidate->polygons[owner.polygon].center, midpoint);
            const auto first_portal = static_cast<std::uint32_t>(candidate->portals.size());
            if (!add_portal(index, owner.polygon, candidate->vertices[b], candidate->vertices[a], cost, no_nav_link, 0) ||
                !add_portal(owner.polygon, index, candidate->vertices[a], candidate->vertices[b], cost, no_nav_link, 0)) {
                return failure(NavStatus::CapacityExceeded, index);
            }
            candidate->edge_portals[index][edge] = first_portal;
            candidate->edge_portals[owner.polygon][owner.edge] = first_portal + 1;
        }
    }
    candidate->links = input.links;
    for (std::size_t index = 0; index < candidate->links.size(); ++index) {
        const auto& link = candidate->links[index];
        if (!supported_point(link.start) || !supported_point(link.end) ||
            !std::isfinite(link.snap_radius) || link.snap_radius < 0.0 ||
            !std::isfinite(link.traversal_cost) || link.traversal_cost < 0.0) {
            return failure(NavStatus::InvalidInput, index);
        }
        const auto start = locate_in(candidate->polygons, candidate->vertices, link.start,
                                      link.snap_radius, nullptr, false, work);
        const auto end = locate_in(candidate->polygons, candidate->vertices, link.end,
                                    link.snap_radius, nullptr, false, work);
        if (!start || !end) {
            return failure(work.exhausted ? NavStatus::BudgetExceeded : NavStatus::NoPolygon, index);
        }
        if (start.polygon == end.polygon) {
            return failure(NavStatus::Unsupported, index);
        }
        const auto from = static_cast<std::size_t>(start.polygon.index - 1);
        const auto to = static_cast<std::size_t>(end.polygon.index - 1);
        const double cost = nav_distance(candidate->polygons[from].center, start.position) +
            link.traversal_cost + nav_distance(end.position, candidate->polygons[to].center);
        if (!add_portal(from, to, start.position, end.position, cost, static_cast<std::uint32_t>(index), link.user_id) ||
            (link.bidirectional && !add_portal(to, from, end.position, start.position, cost,
                                               static_cast<std::uint32_t>(index), link.user_id))) {
            return failure(NavStatus::CapacityExceeded, index);
        }
    }
    data_.swap(candidate);
    source_ = source;
    source_stale_ = false;
    ++revision_;
    return {NavStatus::Success, data_->polygons.size(), work.used};
}

NavResult NavMesh::bake_mesh(const geometry::MeshData& mesh, const math::Mat4& transform,
                            NavBuildOptions options, NavMeshSource source) {
    if (!valid_build(options) || !affine(transform)) {
        return {NavStatus::InvalidInput};
    }
    if (mesh.vertices.size() > limits_.max_vertices || mesh.indices.size() / 3 > limits_.max_polygons) {
        return {NavStatus::CapacityExceeded};
    }
    WorkBudget work{limits_.max_build_work};
    if (!work.charge(mesh.vertices.size()) || !work.charge(mesh.indices.size())) {
        return {NavStatus::BudgetExceeded, 0, work.used};
    }
    const auto validation = geometry::validate_mesh(mesh);
    if (!validation) {
        return {NavStatus::InvalidInput, validation.element, work.used};
    }
    const std::size_t range_count = mesh.submeshes.empty() ? 1 : mesh.submeshes.size();
    const auto first_range = static_cast<std::size_t>(source.first_submesh);
    const auto selected_ranges = source.submesh_count == 0 && first_range <= range_count
        ? range_count - first_range : static_cast<std::size_t>(source.submesh_count);
    if (first_range >= range_count || selected_ranges == 0 || selected_ranges > range_count - first_range) {
        return {NavStatus::InvalidInput, first_range, work.used};
    }
    const std::size_t first_index = mesh.submeshes.empty() ? 0 : mesh.submeshes[first_range].first_index;
    const std::size_t final_range = first_range + selected_ranges - 1;
    const std::size_t end_index = mesh.submeshes.empty() ? mesh.indices.size() :
        static_cast<std::size_t>(mesh.submeshes[final_range].first_index) + mesh.submeshes[final_range].index_count;
    NavMeshInput input;
    input.vertices.reserve(mesh.vertices.size());
    input.polygons.reserve((end_index - first_index) / 3);
    for (const auto& vertex : mesh.vertices) {
        if (!work.charge()) {
            return {NavStatus::BudgetExceeded, input.vertices.size(), work.used};
        }
        const auto position = math::transform_point(transform, vertex.position);
        if (!supported_point(position)) {
            return {NavStatus::NumericFailure, input.vertices.size(), work.used};
        }
        input.vertices.push_back(position);
    }
    const double slope_cosine = std::cos(math::radians(options.maximum_slope_degrees));
    for (std::size_t index = first_index; index < end_index; index += 3) {
        if (!work.charge()) {
            return {NavStatus::BudgetExceeded, index / 3, work.used};
        }
        const auto a = mesh.indices[index];
        const auto b = mesh.indices[index + 1];
        const auto c = mesh.indices[index + 2];
        const auto normal = math::cross(input.vertices[b] - input.vertices[a], input.vertices[c] - input.vertices[a]);
        const double magnitude = std::hypot(normal.x, normal.y, normal.z);
        if (magnitude <= 0.0 || std::abs(normal.y) < options.minimum_projected_area * 2.0 ||
            (options.upward_triangles_only && normal.y <= 0.0) ||
            std::abs(normal.y) / magnitude + geometric_epsilon < slope_cosine) {
            continue;
        }
        input.polygons.push_back({{a, b, c}, 1, 0});
    }
    source.transform = transform;
    return rebuild_impl(input, options, source, work.used);
}

bool NavMesh::clear() noexcept {
    if (revision_ == final_revision) {
        return false;
    }
    data_.reset();
    source_ = {};
    source_stale_ = false;
    ++revision_;
    return true;
}

bool NavMesh::refresh_source(const geometry::MeshLibrary* meshes, scene::Scene* active_scene) {
    source_stale_ = false;
    if (!source_.mesh) {
        return true;
    }
    const auto info = meshes == nullptr ? std::optional<geometry::MeshInfo>{} : meshes->inspect(source_.mesh);
    source_stale_ = !info || info->revision != source_.mesh_revision;
    if (source_.entity) {
        if (active_scene == nullptr || !active_scene->contains(source_.entity)) {
            source_stale_ = true;
        } else {
            const auto matrix = active_scene->world_matrix(source_.entity);
            const auto* component = active_scene->registry().get<geometry::MeshComponent>(source_.entity);
            source_stale_ = source_stale_ || !matrix || *matrix != source_.transform || component == nullptr ||
                component->mesh != source_.mesh || component->first_submesh != source_.first_submesh ||
                component->submesh_count != source_.submesh_count;
        }
    }
    return !source_stale_;
}

const NavMeshSource& NavMesh::source() const noexcept { return source_; }
const NavMeshLimits& NavMesh::limits() const noexcept { return limits_; }
std::uint64_t NavMesh::revision() const noexcept { return revision_; }

NavMeshStatistics NavMesh::statistics() const noexcept {
    return {data_ ? data_->vertices.size() : 0, data_ ? data_->polygons.size() : 0,
            data_ ? data_->portals.size() : 0, data_ ? data_->boundary_edges : 0,
            data_ ? data_->links.size() : 0, revision_, source_stale_};
}

bool NavMesh::current(const NavPath& path) const noexcept {
    return data_ && !source_stale_ && path.status == NavStatus::Success && path.revision == revision_;
}

const NavPolygon* NavMesh::polygon(NavPolygonRef reference) const noexcept {
    if (!data_ || !reference || reference.revision != revision_ || reference.index > data_->polygons.size()) {
        return nullptr;
    }
    return &data_->polygons[reference.index - 1];
}

std::span<const math::Vec3> NavMesh::vertices() const noexcept {
    return data_ ? std::span<const math::Vec3>(data_->vertices) : std::span<const math::Vec3>{};
}

std::span<const NavPolygon> NavMesh::polygons() const noexcept {
    return data_ ? std::span<const NavPolygon>(data_->polygons) : std::span<const NavPolygon>{};
}

std::span<const NavPortal> NavMesh::portals() const noexcept {
    return data_ ? std::span<const NavPortal>(data_->portals) : std::span<const NavPortal>{};
}

std::span<const NavOffMeshLink> NavMesh::links() const noexcept {
    return data_ ? std::span<const NavOffMeshLink>(data_->links) : std::span<const NavOffMeshLink>{};
}

NavStatus NavMesh::query_status(const NavQueryOptions& options) const noexcept {
    if (!options.filter.valid() || options.max_polygons == 0 || options.max_polygons > hard_polygons ||
        options.max_work == 0 || options.max_work > hard_work || options.max_points == 0 ||
        options.max_points > hard_vertices || std::isnan(options.snap_distance) || options.snap_distance < 0.0 ||
        !std::isfinite(options.agent_radius) || options.agent_radius < 0.0) {
        return NavStatus::InvalidInput;
    }
    if (options.agent_radius > 0.0) {
        return NavStatus::Unsupported;
    }
    if (options.expected_revision != 0 && options.expected_revision != revision_) {
        return NavStatus::StaleMesh;
    }
    if (options.search.cancellation.stop_requested()) {
        return NavStatus::Cancelled;
    }
    if (source_stale_) {
        return NavStatus::StaleSource;
    }
    return data_ ? NavStatus::Success : NavStatus::Empty;
}

NavLocation NavMesh::closest_point(NavPolygonRef reference, math::Vec3 point) const {
    if (!supported_point(point)) {
        return {NavStatus::InvalidInput};
    }
    if (source_stale_) {
        return {NavStatus::StaleSource};
    }
    const auto* selected = polygon(reference);
    if (selected == nullptr) {
        return {NavStatus::StaleMesh};
    }
    WorkBudget work{limits_.max_vertices_per_polygon};
    const auto projected = closest_on_polygon(*selected, data_->vertices, point, work);
    if (!projected || !math::finite(*projected)) {
        return {NavStatus::NumericFailure, {}, {}, 0.0, work.used};
    }
    return {NavStatus::Success, reference, *projected, nav_distance(point, *projected), work.used};
}

NavLocation NavMesh::locate(math::Vec3 point, const NavQueryOptions& options, bool containing_only) const {
    const auto status = query_status(options);
    if (status != NavStatus::Success) {
        return {status};
    }
    if (!supported_point(point)) {
        return {NavStatus::InvalidInput};
    }
    if (data_->polygons.size() > options.max_polygons) {
        return {NavStatus::BudgetExceeded};
    }
    WorkBudget work{options.max_work};
    return locate_in(data_->polygons, data_->vertices, point, options.snap_distance,
                     &options.filter, containing_only, work);
}

NavLocation NavMesh::nearest(math::Vec3 point, const NavQueryOptions& options) const {
    return locate(point, options, false);
}

NavLocation NavMesh::containing(math::Vec3 point, const NavQueryOptions& options) const {
    return locate(point, options, true);
}

NavPath NavMesh::find_path(math::Vec3 start, math::Vec3 goal, const NavQueryOptions& options) const {
    NavPath result;
    result.revision = revision_;
    result.status = query_status(options);
    const auto fail = [&](NavStatus status, std::size_t work) {
        result.status = status;
        result.work = work;
        result.path.nodes.clear();
        result.path.points.clear();
        result.path.start = {};
        result.path.goal = {};
        result.path.graph_revision = 0;
        if (status == NavStatus::BudgetExceeded) {
            result.path.status = PathStatus::BudgetExceeded;
        } else if (status == NavStatus::Cancelled) {
            result.path.status = PathStatus::Cancelled;
        } else if (status == NavStatus::NoPath) {
            result.path.status = PathStatus::NoPath;
        } else {
            result.path.status = PathStatus::InvalidInput;
        }
        return result;
    };
    if (result.status != NavStatus::Success) {
        return fail(result.status, 0);
    }
    if (!supported_point(start) || !supported_point(goal)) {
        return fail(NavStatus::InvalidInput, 0);
    }
    if (data_->polygons.size() > options.max_polygons) {
        return fail(NavStatus::BudgetExceeded, 0);
    }
    WorkBudget work{options.max_work};
    result.start = locate_in(data_->polygons, data_->vertices, start, options.snap_distance, &options.filter, false, work);
    if (!result.start) {
        return fail(result.start.status, work.used);
    }
    result.goal = locate_in(data_->polygons, data_->vertices, goal, options.snap_distance, &options.filter, false, work);
    if (!result.goal) {
        return fail(result.goal.status, work.used);
    }
    NavigationGraph graph({static_cast<std::uint32_t>(data_->polygons.size()),
                           limits_.max_arcs, limits_.max_arcs_per_polygon});
    std::vector<NodeId> nodes;
    nodes.reserve(data_->polygons.size());
    for (const auto& polygon_value : data_->polygons) {
        if (!work.charge()) {
            return fail(NavStatus::BudgetExceeded, work.used);
        }
        const auto node = graph.add_node(polygon_value.center, options.filter.allows(polygon_value.flags));
        if (!node) {
            return fail(NavStatus::CapacityExceeded, work.used);
        }
        nodes.push_back(node);
    }
    using Pair = std::pair<std::uint32_t, std::uint32_t>;
    struct Choice { double cost{}; std::uint32_t portal{}; };
    std::map<Pair, Choice> choices;
    for (std::size_t from = 0; from < data_->arcs.size(); ++from) {
        if (!options.filter.allows(data_->polygons[from].flags)) {
            continue;
        }
        for (const auto& arc : data_->arcs[from]) {
            if (!work.charge()) {
                return fail(NavStatus::BudgetExceeded, work.used);
            }
            const auto& destination = data_->polygons[arc.destination];
            const auto& portal = data_->portals[arc.portal];
            if (!options.filter.allows(destination.flags) || (portal.off_mesh() &&
                (!options.filter.allow_off_mesh_links || !options.filter.allows(data_->links[portal.link_index].flags)))) {
                continue;
            }
            const double cost = arc.cost * options.filter.area_costs[destination.area];
            if (!std::isfinite(cost)) {
                return fail(NavStatus::NumericFailure, work.used);
            }
            const Pair pair{static_cast<std::uint32_t>(from), arc.destination};
            const auto [entry, inserted] = choices.try_emplace(pair, Choice{cost, arc.portal});
            if (!inserted && cost < entry->second.cost) {
                entry->second = {cost, arc.portal};
            }
        }
    }
    for (const auto& [pair, choice] : choices) {
        if (!work.charge()) {
            return fail(NavStatus::BudgetExceeded, work.used);
        }
        if (!graph.connect(nodes[pair.first], nodes[pair.second], choice.cost)) {
            return fail(NavStatus::CapacityExceeded, work.used);
        }
    }
    if (work.used == work.limit) {
        return fail(NavStatus::BudgetExceeded, work.used);
    }
    auto search_options = options.search;
    search_options.expected_revision = graph.revision();
    search_options.max_work = std::min(search_options.max_work, work.limit - work.used);
    search_options.max_output = std::min(search_options.max_output, options.max_polygons);
    result.path = ge::navigation::find_path(graph, nodes[result.start.polygon.index - 1],
                                           nodes[result.goal.polygon.index - 1], search_options);
    if (!work.charge(result.path.work)) {
        return fail(NavStatus::BudgetExceeded, work.used);
    }
    if (!result.path) {
        return fail(search_status(result.path.status), work.used);
    }
    for (std::size_t index = 0; index < result.path.nodes.size(); ++index) {
        if (!work.charge()) {
            return fail(NavStatus::BudgetExceeded, work.used);
        }
        const auto polygon_index = result.path.nodes[index].index - 1;
        result.corridor.push_back(data_->polygons[polygon_index].reference);
        if (index > 0) {
            const Pair pair{result.path.nodes[index - 1].index - 1, polygon_index};
            const auto selected = choices.find(pair);
            if (selected == choices.end()) {
                return fail(NavStatus::NumericFailure, work.used);
            }
            result.portals.push_back(data_->portals[selected->second.portal]);
        }
    }
    NavPathPoint segment_start{result.start.position, result.start.polygon};
    std::size_t segment_begin = 0;
    const std::span<const NavPortal> route(result.portals);
    for (std::size_t index = 0; index < route.size(); ++index) {
        const auto& portal = route[index];
        if (!portal.off_mesh()) {
            continue;
        }
        const NavPathPoint entry{portal.left, portal.from, NavPointKind::LinkEntry,
                                 portal.link_user_id, portal.link_index};
        result.status = funnel(route.subspan(segment_begin, index - segment_begin), segment_start, entry,
                                result.corners, options.max_points, work);
        if (result.status != NavStatus::Success || !append_corner(result.corners,
            {portal.right, portal.to, NavPointKind::LinkExit, portal.link_user_id, portal.link_index}, options.max_points)) {
            return fail(NavStatus::BudgetExceeded, work.used);
        }
        segment_start = {portal.right, portal.to};
        segment_begin = index + 1;
    }
    result.status = funnel(route.subspan(segment_begin), segment_start,
        {result.goal.position, result.goal.polygon}, result.corners, options.max_points, work);
    if (result.status != NavStatus::Success) {
        return fail(result.status, work.used);
    }
    result.path.nodes.clear();
    result.path.start = {};
    result.path.goal = {};
    result.path.points.clear();
    result.path.points.reserve(result.corners.size());
    for (const auto& corner : result.corners) {
        result.path.points.push_back(corner.position);
    }
    result.path.graph_revision = 0;
    result.work = work.used;
    return result;
}

NavMoveResult NavMesh::move_along_surface(NavLocation start, math::Vec3 target,
                                         const NavQueryOptions& options) const {
    NavMoveResult result;
    result.location = start;
    result.status = query_status(options);
    if (result.status != NavStatus::Success) {
        return result;
    }
    if (!start || !supported_point(start.position) || !supported_point(target)) {
        result.status = NavStatus::InvalidInput;
        return result;
    }
    const auto* initial = polygon(start.polygon);
    if (initial == nullptr) {
        result.status = NavStatus::StaleMesh;
        return result;
    }
    if (!options.filter.allows(initial->flags)) {
        result.status = NavStatus::NoPolygon;
        return result;
    }
    WorkBudget work{options.max_work};
    const auto initial_height = height_at(*initial, data_->vertices, start.position, work);
    if (!initial_height || std::abs(start.position.y - *initial_height) > 1.0e-5) {
        result.status = work.exhausted ? NavStatus::BudgetExceeded : NavStatus::InvalidInput;
        result.work = work.used;
        return result;
    }
    auto current_polygon = start.polygon;
    double progress = 0.0;
    const auto finish = [&](NavStatus status) {
        result.status = status;
        result.work = work.used;
        result.location.work = work.used;
        return result;
    };
    while (result.visited.size() < options.max_polygons) {
        if (!work.charge()) {
            return finish(NavStatus::BudgetExceeded);
        }
        if (options.search.cancellation.stop_requested()) {
            return finish(NavStatus::Cancelled);
        }
        const auto polygon_index = static_cast<std::size_t>(current_polygon.index - 1);
        const auto& current_polygon_value = data_->polygons[polygon_index];
        result.visited.push_back(current_polygon);
        const auto target_height = height_at(current_polygon_value, data_->vertices, target, work);
        if (work.exhausted) {
            return finish(NavStatus::BudgetExceeded);
        }
        if (target_height) {
            result.location = {NavStatus::Success, current_polygon, {target.x, *target_height, target.z}, 0.0, 0};
            result.fraction = 1.0;
            return finish(NavStatus::Success);
        }
        double exit_fraction = 1.0;
        std::size_t exit_edge = current_polygon_value.vertices.size();
        for (std::size_t edge = 0; edge < current_polygon_value.vertices.size(); ++edge) {
            if (!work.charge()) {
                return finish(NavStatus::BudgetExceeded);
            }
            const auto a = data_->vertices[current_polygon_value.vertices[edge]];
            const auto b = data_->vertices[current_polygon_value.vertices[(edge + 1) % current_polygon_value.vertices.size()]];
            const double initial_side = orient(a, b, start.position);
            const double change = orient(a, b, target) - initial_side;
            if (change >= 0.0) {
                continue;
            }
            const double fraction = -initial_side / change;
            if (fraction >= progress - geometric_epsilon && fraction < exit_fraction) {
                exit_fraction = std::max(progress, fraction);
                exit_edge = edge;
            }
        }
        if (exit_edge == current_polygon_value.vertices.size()) {
            return finish(NavStatus::NumericFailure);
        }
        auto position = math::lerp(start.position, target, exit_fraction);
        const auto surface_height = height_at(current_polygon_value, data_->vertices, position, work);
        if (!surface_height) {
            return finish(work.exhausted ? NavStatus::BudgetExceeded : NavStatus::NumericFailure);
        }
        position.y = *surface_height;
        result.location = {NavStatus::Success, current_polygon, position, 0.0, 0};
        result.fraction = exit_fraction;
        const auto portal_index = data_->edge_portals[polygon_index][exit_edge];
        const bool has_neighbor = portal_index != no_nav_link;
        const auto neighbor = has_neighbor ? data_->portals[portal_index].to : NavPolygonRef{};
        const bool allowed = has_neighbor && options.filter.allows(data_->polygons[neighbor.index - 1].flags);
        if (!allowed || (exit_fraction <= progress + geometric_epsilon && result.visited.size() > 1)) {
            const auto a = data_->vertices[current_polygon_value.vertices[exit_edge]];
            const auto b = data_->vertices[current_polygon_value.vertices[(exit_edge + 1) % current_polygon_value.vertices.size()]];
            result.wall_normal = math::normalized(math::Vec3{a.z - b.z, 0.0, b.x - a.x});
            result.blocked = true;
            return finish(NavStatus::Success);
        }
        progress = exit_fraction;
        current_polygon = neighbor;
    }
    return finish(NavStatus::BudgetExceeded);
}

void register_navmesh(core::Kernel& kernel) {
    if (kernel.services().find<NavMesh>() != nullptr) {
        return;
    }
    NavMeshLimits limits;
    const auto polygon_limit = kernel.config().integer("navmesh.max_polygons", 65536);
    if (polygon_limit > 0 && polygon_limit <= static_cast<std::int64_t>(hard_polygons)) {
        limits.max_polygons = static_cast<std::size_t>(polygon_limit);
    }
    (void)kernel.services().emplace<NavMesh>(limits);
    (void)kernel.services().emplace<NavCommandState>();
    (void)kernel.register_module({"navigation.navmesh", "Convex polygon navigation, surface queries and funnels", 1});
    (void)kernel.add_system({"navmesh.source", core::Phase::PreSimulation, -8600, {"scene.transforms.pre"}},
        [](core::Kernel& host, const core::Frame&) {
            auto& mesh = host.services().get<NavMesh>();
            (void)mesh.refresh_source(host.services().find<geometry::MeshLibrary>(), host.services().find<scene::Scene>());
            const auto statistics = mesh.statistics();
            host.metric("navmesh.polygons", static_cast<double>(statistics.polygons));
            host.metric("navmesh.portals", static_cast<double>(statistics.directed_portals));
            host.metric("navmesh.source_stale", statistics.source_stale ? 1.0 : 0.0);
        });
    (void)kernel.add_command("navmesh.bake", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto* library = host.services().find<geometry::MeshLibrary>();
        auto* active_scene = host.services().find<scene::Scene>();
        NavBuildOptions options;
        if (library == nullptr || (args.size() != 2 && args.size() != 3) ||
            (args.size() == 3 && !parse_number(args[2], options.maximum_slope_degrees))) {
            return {false, "usage: navmesh.bake mesh mesh_name [slope_degrees] | scene scene_path [slope_degrees]"};
        }
        NavMeshSource source;
        if (args[0] == "mesh") {
            source.mesh = library->find(args[1]);
        } else if (args[0] == "scene" && active_scene != nullptr) {
            const auto found = active_scene->find_path(args[1]);
            if (!found) {
                return {false, "scene source path was not found"};
            }
            const auto* component = active_scene->registry().get<geometry::MeshComponent>(found.entity);
            const auto matrix = active_scene->world_matrix(found.entity);
            if (component == nullptr || !matrix) {
                return {false, "source needs a MeshComponent and valid world matrix"};
            }
            source.mesh = component->mesh;
            source.entity = found.entity;
            source.transform = *matrix;
            source.first_submesh = component->first_submesh;
            source.submesh_count = component->submesh_count;
        } else {
            return {false, "source kind must be mesh or scene"};
        }
        const auto info = library->inspect(source.mesh);
        const auto* geometry_data = library->get(source.mesh);
        if (!info || geometry_data == nullptr) {
            return {false, "mesh source is unavailable"};
        }
        source.mesh_revision = info->revision;
        auto& mesh = host.services().get<NavMesh>();
        const auto result = mesh.bake_mesh(*geometry_data, source.transform, options, source);
        if (!result) {
            return {false, std::string(describe(result.status)) + " at element " + std::to_string(result.element)};
        }
        host.services().get<NavCommandState>().last_path = {};
        return {true, "baked " + std::to_string(mesh.statistics().polygons) + " polygons at revision " +
            std::to_string(mesh.revision()) + ", work " + std::to_string(result.work)};
    }, "Bake upward walkable triangles from MeshLibrary geometry or an exact Scene mesh transform");
    (void)kernel.add_command("navmesh.nearest", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        math::Vec3 point;
        NavQueryOptions options;
        if ((args.size() != 3 && args.size() != 4) || !parse_point(args, 0, point) ||
            (args.size() == 4 && !parse_number(args[3], options.snap_distance))) {
            return {false, "usage: navmesh.nearest x y z [maximum_distance]"};
        }
        auto& mesh = host.services().get<NavMesh>();
        (void)mesh.refresh_source(host.services().find<geometry::MeshLibrary>(), host.services().find<scene::Scene>());
        const auto result = mesh.nearest(point, options);
        return {static_cast<bool>(result), result ? reference_text(result.polygon) + " at " + point_text(result.position) +
            ", distance " + std::to_string(result.distance) : std::string(describe(result.status))};
    }, "Find the closest point on an allowed navigation polygon");
    (void)kernel.add_command("navmesh.path", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& mesh = host.services().get<NavMesh>();
        auto& state = host.services().get<NavCommandState>();
        (void)mesh.refresh_source(host.services().find<geometry::MeshLibrary>(), host.services().find<scene::Scene>());
        if (!args.empty() && args[0] == "points") {
            std::size_t offset = 0;
            std::size_t count = 32;
            if ((args.size() != 1 && args.size() != 3) ||
                (args.size() == 3 && (!parse_number(args[1], offset) || !parse_number(args[2], count))) || count > 256) {
                return {false, "usage: navmesh.path points [offset count], count <= 256"};
            }
            if (!mesh.current(state.last_path)) {
                return {false, "no current retained path"};
            }
            if (offset > state.last_path.corners.size()) {
                return {false, "point offset exceeds retained path"};
            }
            const auto end = offset + std::min(count, state.last_path.corners.size() - offset);
            std::ostringstream output;
            for (std::size_t index = offset; index < end; ++index) {
                const auto& point = state.last_path.corners[index];
                output << index << ": " << point_text(point.position) << " polygon " << reference_text(point.polygon)
                       << " kind " << static_cast<unsigned>(point.kind) << " link " << point.link_user_id
                       << " link_index " << point.link_index << '\n';
            }
            return {true, output.str()};
        }
        math::Vec3 start;
        math::Vec3 goal;
        NavQueryOptions options;
        if ((args.size() != 6 && args.size() != 7) || !parse_point(args, 0, start) || !parse_point(args, 3, goal) ||
            (args.size() == 7 && !parse_number(args[6], options.filter.include_flags))) {
            return {false, "usage: navmesh.path start_x start_y start_z goal_x goal_y goal_z [include_mask]"};
        }
        auto result = mesh.find_path(start, goal, options);
        if (!result) {
            return {false, std::string(describe(result.status)) + ", search " + std::string(describe(result.path.status))};
        }
        state.last_path = std::move(result);
        return {true, "corridor " + std::to_string(state.last_path.corridor.size()) + " polygons, " +
            std::to_string(state.last_path.corners.size()) + " corners, cost " + std::to_string(state.last_path.path.cost) +
            "; navmesh.path points displays the retained route"};
    }, "Search a filtered polygon graph and retain an XZ funnel route with explicit off-mesh actions");
    (void)kernel.add_command("navmesh.move", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        math::Vec3 start;
        math::Vec3 target;
        if (args.size() != 6 || !parse_point(args, 0, start) || !parse_point(args, 3, target)) {
            return {false, "usage: navmesh.move start_x start_y start_z target_x target_y target_z"};
        }
        auto& mesh = host.services().get<NavMesh>();
        (void)mesh.refresh_source(host.services().find<geometry::MeshLibrary>(), host.services().find<scene::Scene>());
        const auto location = mesh.nearest(start);
        if (!location) {
            return {false, std::string(describe(location.status))};
        }
        const auto result = mesh.move_along_surface(location, target);
        return {static_cast<bool>(result), result ? point_text(result.location.position) + " fraction " +
            std::to_string(result.fraction) + (result.blocked ? " blocked" : " reached") : std::string(describe(result.status))};
    }, "Trace a point-agent segment over the navmesh without modifying scene or physics state");
    (void)kernel.add_command("navmesh.info", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) {
            return {false, "usage: navmesh.info"};
        }
        auto& mesh = host.services().get<NavMesh>();
        (void)mesh.refresh_source(host.services().find<geometry::MeshLibrary>(), host.services().find<scene::Scene>());
        const auto statistics = mesh.statistics();
        return {true, "revision " + std::to_string(statistics.revision) + ", polygons " + std::to_string(statistics.polygons) +
            ", portals " + std::to_string(statistics.directed_portals) + ", boundaries " + std::to_string(statistics.boundary_edges) +
            ", links " + std::to_string(statistics.links) + (statistics.source_stale ? ", source stale" : ", source current")};
    }, "Inspect navmesh topology and source freshness");
    (void)kernel.add_command("navmesh.clear", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) {
            return {false, "usage: navmesh.clear"};
        }
        const bool cleared = host.services().get<NavMesh>().clear();
        if (cleared) {
            host.services().get<NavCommandState>().last_path = {};
        }
        return {cleared, cleared ? "navmesh cleared and references invalidated" : "navmesh revision exhausted"};
    }, "Discard the mesh and invalidate all polygon references");
}

} // namespace ge::navigation
