#include <ge/traffic/roads.hpp>

#include <ge/core/runtime.hpp>
#include <ge/scene/partition.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ge::traffic {
namespace {

constexpr double minimum_segment = 1.0e-5;
constexpr math::Vec3 world_up{0.0, 1.0, 0.0};

template<class Handle>
ge::RuntimeHandle token(Handle handle) noexcept {
    return {handle.index, handle.generation};
}

bool finite_position(math::Vec3 position, double limit) noexcept {
    return math::finite(position) && std::abs(position.x) <= limit &&
        std::abs(position.y) <= limit && std::abs(position.z) <= limit;
}

bool valid_name(std::string_view name, std::size_t maximum) noexcept {
    return !name.empty() && name.size() <= maximum && name.find('\0') == std::string_view::npos;
}

bool valid_speed(double speed) noexcept {
    return std::isfinite(speed) && speed >= 0.1 && speed <= 200.0;
}

double speed_at(double curvature, double limit, double acceleration) noexcept {
    const double magnitude = std::abs(curvature);
    return magnitude > 1.0e-12 ? std::min(limit, std::sqrt(acceleration / magnitude)) : limit;
}

RoadResult finish_geometry(std::vector<LaneSample>& points, double speed,
                           double acceleration, double coordinate_limit) {
    if (points.size() < 2) {
        return {RoadError::DegenerateGeometry};
    }
    double distance = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (!finite_position(points[index].position, coordinate_limit)) {
            return {RoadError::InvalidInput, index};
        }
        if (index != 0) {
            const double chord = math::distance(points[index - 1].position, points[index].position);
            if (!std::isfinite(chord) || chord < minimum_segment) {
                return {RoadError::DegenerateGeometry, index};
            }
            distance += chord;
        }
        auto& point = points[index];
        point.distance = distance;
        const std::size_t previous = index == 0 ? index : index - 1;
        const std::size_t next = index + 1 == points.size() ? index : index + 1;
        point.tangent = math::normalized(points[next].position - points[previous].position);
        point.right = math::cross(point.tangent, world_up);
        if (math::length_squared(point.right) < 1.0e-8 || !math::normalize(point.right)) {
            return {RoadError::DegenerateGeometry, index};
        }
        point.up = math::normalized(math::cross(point.right, point.tangent));
        if (index != 0 && index + 1 < points.size()) {
            const math::Vec3 first = point.position - points[index - 1].position;
            const math::Vec3 second = points[index + 1].position - point.position;
            const double denominator = math::length(first) * math::length(second) * math::length(first + second);
            if (denominator <= 1.0e-15) {
                return {RoadError::DegenerateGeometry, index};
            }
            const math::Vec3 bend = math::cross(first, second);
            point.curvature = std::copysign(2.0 * math::length(bend) / denominator, bend.y);
        }
    }
    if (points.size() > 2) {
        points.front().curvature = points[1].curvature;
        points.back().curvature = points[points.size() - 2].curvature;
    }
    for (auto& point : points) {
        point.recommended_speed = speed_at(point.curvature, speed, acceleration);
        if (!std::isfinite(point.recommended_speed) || point.recommended_speed <= 0.0) {
            return {RoadError::DegenerateGeometry};
        }
    }
    return {};
}

std::optional<LaneSample> interpolate(std::span<const LaneSample> points, double distance) noexcept {
    if (points.empty() || !std::isfinite(distance)) {
        return std::nullopt;
    }
    const double bounded = std::clamp(distance, 0.0, points.back().distance);
    const auto upper = std::lower_bound(points.begin(), points.end(), bounded,
        [](const LaneSample& point, double value) { return point.distance < value; });
    if (upper == points.begin()) {
        return points.front();
    }
    if (upper == points.end()) {
        return points.back();
    }
    const auto& first = *(upper - 1);
    const auto& second = *upper;
    const double span = second.distance - first.distance;
    const double fraction = span > 0.0 ? (bounded - first.distance) / span : 1.0;
    LaneSample result;
    result.position = math::lerp(first.position, second.position, fraction);
    result.tangent = math::normalized(math::lerp(first.tangent, second.tangent, fraction), first.tangent);
    result.right = math::normalized(math::cross(result.tangent, world_up), first.right);
    result.up = math::normalized(math::cross(result.right, result.tangent), first.up);
    result.distance = bounded;
    result.curvature = first.curvature + (second.curvature - first.curvature) * fraction;
    result.recommended_speed = first.recommended_speed +
        (second.recommended_speed - first.recommended_speed) * fraction;
    return result;
}

double travel_time(std::span<const LaneSample> points, double begin, double end) noexcept {
    double seconds = 0.0;
    for (std::size_t index = 1; index < points.size(); ++index) {
        const auto& first = points[index - 1];
        const auto& second = points[index];
        const double left = std::max(begin, first.distance);
        const double right = std::min(end, second.distance);
        if (right <= left) {
            continue;
        }
        const double span = second.distance - first.distance;
        const double difference = second.recommended_speed - first.recommended_speed;
        const double left_speed = first.recommended_speed + difference * ((left - first.distance) / span);
        const double right_speed = first.recommended_speed + difference * ((right - first.distance) / span);
        seconds += (right - left) * 0.5 * (1.0 / left_speed + 1.0 / right_speed);
    }
    return seconds;
}

std::optional<LaneProjection> project_points(LaneHandle lane,
    std::span<const LaneSample> points, math::Vec3 position) noexcept {
    if (!math::finite(position) || points.size() < 2) {
        return std::nullopt;
    }
    LaneProjection best;
    best.lane = lane;
    best.squared_distance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 1; index < points.size(); ++index) {
        const auto& first = points[index - 1];
        const auto& second = points[index];
        const math::Vec3 direction = second.position - first.position;
        const double denominator = math::length_squared(direction);
        const double fraction = denominator > 0.0 ?
            std::clamp(math::dot(position - first.position, direction) / denominator, 0.0, 1.0) : 0.0;
        const math::Vec3 projected = first.position + direction * fraction;
        const double squared = math::distance_squared(position, projected);
        if (squared < best.squared_distance) {
            best.squared_distance = squared;
            best.sample = *interpolate(points, first.distance +
                (second.distance - first.distance) * fraction);
            best.lateral_offset = math::dot(position - projected, best.sample.right);
        }
    }
    return std::isfinite(best.squared_distance) ? std::optional<LaneProjection>{best} : std::nullopt;
}

std::optional<double> parse_real(std::string_view text) noexcept {
    double value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value)
        ? std::optional<double>{value} : std::nullopt;
}

std::optional<std::size_t> parse_index(std::string_view text) noexcept {
    std::size_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
        ? std::optional<std::size_t>{value} : std::nullopt;
}

} // namespace

struct RoadNetwork::Impl {
    struct RoadRecord {
        RoadHandle handle;
        RoadDescriptor descriptor;
        std::vector<LaneSample> center;
        std::vector<LaneHandle> lanes;
        math::Aabb bounds{};
        std::size_t sample_count{};
        std::uint64_t revision{};
    };
    struct LaneRecord {
        LaneInfo info;
        std::vector<LaneSample> points;
    };
    struct ConnectionRecord {
        LaneConnection info;
        std::vector<LaneSample> points;
    };
    struct Prepared {
        std::unique_ptr<RoadRecord> road;
        std::vector<std::unique_ptr<LaneRecord>> lanes;
    };
    RoadLimits limits;
    GenerationPool road_ids;
    GenerationPool lane_ids;
    GenerationPool junction_ids;
    std::vector<std::unique_ptr<RoadRecord>> roads;
    std::vector<std::unique_ptr<LaneRecord>> lanes;
    std::vector<std::unique_ptr<JunctionInfo>> junctions;
    std::vector<ConnectionRecord> connections;
    std::vector<std::optional<RoadBinding>> bindings;
    std::unique_ptr<navigation::NavigationGraph> graph;
    std::vector<navigation::NodeId> lane_nodes;
    std::vector<LaneHandle> node_lanes;
    std::uint64_t revision{1};
    std::uint64_t graph_network_revision{};
    std::uint64_t graph_mask{};
    std::uint64_t graph_epoch{};
    std::size_t sample_count{};
    std::size_t sync_cursor{1};
    scene::Scene* bound_scene{};
    geometry::MeshLibrary* bound_meshes{};

    explicit Impl(RoadLimits configuration)
        : limits(configuration), road_ids(configuration.max_roads), lane_ids(configuration.max_lanes),
          junction_ids(configuration.max_junctions) {
        if (limits.max_roads == 0 || limits.max_roads > 65536 || limits.max_lanes == 0 ||
            limits.max_lanes > 262144 || limits.max_junctions > 65536 ||
            limits.max_lanes_per_road == 0 || limits.max_lanes_per_road > 64 ||
            limits.max_samples_per_road < 2 || limits.max_samples_per_road > 16384 ||
            limits.max_total_samples < 2 || limits.max_total_samples > 4194304 ||
            limits.max_connections > 1048576 || limits.max_outgoing == 0 || limits.max_outgoing > 256 ||
            limits.max_connector_samples < 2 || limits.max_connector_samples > 4096 ||
            limits.max_route_lanes == 0 || limits.max_route_lanes > 262144 ||
            limits.max_route_work == 0 || limits.max_route_work > 16777216 ||
            limits.max_name_bytes == 0 || limits.max_name_bytes > 1024 ||
            !std::isfinite(limits.coordinate_limit) || limits.coordinate_limit < 1.0 ||
            limits.coordinate_limit > 1.0e12 || !std::isfinite(limits.max_road_length) ||
            limits.max_road_length < 1.0 || limits.max_road_length > 100000000.0) {
            throw std::invalid_argument("invalid road network limits");
        }
        roads.resize(static_cast<std::size_t>(limits.max_roads) + 1);
        lanes.resize(static_cast<std::size_t>(limits.max_lanes) + 1);
        junctions.resize(static_cast<std::size_t>(limits.max_junctions) + 1);
        bindings.resize(roads.size());
    }

    RoadRecord* road(RoadHandle handle) const noexcept {
        return road_ids.alive(token(handle)) ? roads[handle.index].get() : nullptr;
    }
    LaneRecord* lane(LaneHandle handle) const noexcept {
        return lane_ids.alive(token(handle)) ? lanes[handle.index].get() : nullptr;
    }
    JunctionInfo* junction(JunctionHandle handle) const noexcept {
        return junction_ids.alive(token(handle)) ? junctions[handle.index].get() : nullptr;
    }
    ConnectionRecord* connection(LaneHandle from, LaneHandle to) noexcept {
        const auto key = std::pair{from, to};
        const auto found = std::lower_bound(connections.begin(), connections.end(), key,
            [](const auto& entry, const auto& target) { return std::pair{entry.info.from, entry.info.to} < target; });
        return found != connections.end() && found->info.from == from && found->info.to == to ? &*found : nullptr;
    }
    bool editable() const noexcept {
        return revision != std::numeric_limits<std::uint64_t>::max();
    }
    void changed() noexcept {
        ++revision;
        graph_network_revision = 0;
    }
    bool accessible(const LaneRecord& record, std::uint64_t mask) const noexcept {
        return record.info.specification.enabled && (record.info.specification.access_mask & mask) != 0;
    }
    bool connected(const ConnectionRecord& record, std::uint64_t mask) const noexcept {
        const auto& specification = record.info.specification;
        const auto* at_junction = junction(specification.junction);
        return specification.enabled && (specification.access_mask & mask) != 0 &&
            (!specification.junction || (at_junction != nullptr && at_junction->enabled));
    }

    RoadResult prepare(RoadDescriptor descriptor, Prepared& output) const {
        if (!valid_name(descriptor.name, limits.max_name_bytes) || descriptor.lanes.empty() ||
            descriptor.lanes.size() > limits.max_lanes_per_road ||
            !std::isfinite(descriptor.sample_spacing) || descriptor.sample_spacing < 0.05 ||
            descriptor.sample_spacing > 1000.0 || descriptor.arc_samples < 2 ||
            descriptor.arc_samples > limits.max_samples_per_road ||
            !std::isfinite(descriptor.shoulder_width) || descriptor.shoulder_width < 0.0 ||
            descriptor.shoulder_width > 100.0 || !std::isfinite(descriptor.texture_metres) ||
            descriptor.texture_metres < 0.01 || descriptor.texture_metres > 10000.0) {
            return {RoadError::InvalidInput};
        }
        if (!descriptor.centerline.valid() || descriptor.centerline.keys().size() < 2 ||
            descriptor.centerline.keys().size() > limits.max_samples_per_road) {
            return {RoadError::InvalidCurve};
        }
        for (const auto& key : descriptor.centerline.keys()) {
            if (!finite_position(key.value, limits.coordinate_limit) ||
                key.interpolation == animation::CurveInterpolation::Step) {
                return {RoadError::InvalidCurve};
            }
        }
        double left_edge = std::numeric_limits<double>::infinity();
        double right_edge = -std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < descriptor.lanes.size(); ++index) {
            const auto& specification = descriptor.lanes[index];
            if (!std::isfinite(specification.offset) || std::abs(specification.offset) > 1000.0 ||
                !std::isfinite(specification.width) || specification.width < 0.25 ||
                specification.width > 30.0 || !valid_speed(specification.speed_limit) ||
                !std::isfinite(specification.lateral_acceleration) ||
                specification.lateral_acceleration < 0.1 || specification.lateral_acceleration > 50.0 ||
                specification.access_mask == 0) {
                return {RoadError::InvalidInput, index};
            }
            const double left = specification.offset - specification.width * 0.5;
            const double right = specification.offset + specification.width * 0.5;
            left_edge = std::min(left_edge, left);
            right_edge = std::max(right_edge, right);
            for (std::size_t previous = 0; previous < index; ++previous) {
                const auto& other = descriptor.lanes[previous];
                if (std::min(right, other.offset + other.width * 0.5) -
                    std::max(left, other.offset - other.width * 0.5) > 1.0e-6) {
                    return {RoadError::InvalidInput, index};
                }
            }
        }
        animation::CurveArcTable arc;
        const auto built = arc.build(descriptor.centerline, descriptor.centerline.begin_time(),
            descriptor.centerline.end_time(), descriptor.arc_samples);
        if (!built || !std::isfinite(arc.length()) || arc.length() < minimum_segment ||
            arc.length() > limits.max_road_length) {
            return {RoadError::InvalidCurve};
        }
        const double intervals = std::ceil(arc.length() / descriptor.sample_spacing);
        if (intervals > static_cast<double>(limits.max_samples_per_road - 1)) {
            return {RoadError::Capacity};
        }
        const auto count = static_cast<std::size_t>(intervals) + 1;
        if (count > limits.max_total_samples / (descriptor.lanes.size() + 1)) {
            return {RoadError::Capacity};
        }
        output.road = std::make_unique<RoadRecord>();
        auto& prepared = *output.road;
        prepared.center.resize(count);
        prepared.lanes.resize(descriptor.lanes.size());
        prepared.sample_count = count * (descriptor.lanes.size() + 1);
        for (std::size_t index = 0; index < count; ++index) {
            const double distance = arc.length() * static_cast<double>(index) / static_cast<double>(count - 1);
            const auto time = arc.time_at_distance(distance);
            const auto sampled = time ? descriptor.centerline.sample(*time) : std::nullopt;
            if (!sampled || sampled->discontinuous) {
                return {RoadError::InvalidCurve, index};
            }
            prepared.center[index].position = sampled->value;
        }
        const auto finalized = finish_geometry(prepared.center, 200.0, 50.0, limits.coordinate_limit);
        if (!finalized) {
            return finalized;
        }
        left_edge -= descriptor.shoulder_width;
        right_edge += descriptor.shoulder_width;
        prepared.bounds.min = prepared.center.front().position;
        prepared.bounds.max = prepared.bounds.min;
        for (const auto& point : prepared.center) {
            for (const double offset : {left_edge, right_edge}) {
                const math::Vec3 edge = point.position + point.right * offset;
                if (!finite_position(edge, limits.coordinate_limit)) {
                    return {RoadError::InvalidInput};
                }
                prepared.bounds.min = math::min(prepared.bounds.min, edge);
                prepared.bounds.max = math::max(prepared.bounds.max, edge);
            }
        }
        output.lanes.reserve(descriptor.lanes.size());
        for (std::size_t index = 0; index < descriptor.lanes.size(); ++index) {
            const auto& specification = descriptor.lanes[index];
            auto prepared_lane = std::make_unique<LaneRecord>();
            prepared_lane->info.ordinal = static_cast<std::uint32_t>(index);
            prepared_lane->info.specification = specification;
            prepared_lane->points.resize(count);
            for (std::size_t sample_index = 0; sample_index < count; ++sample_index) {
                const std::size_t source = specification.reverse ? count - 1 - sample_index : sample_index;
                const auto& center = prepared.center[source];
                if (std::abs(center.curvature * specification.offset) >= 0.9) {
                    return {RoadError::DegenerateGeometry, index};
                }
                prepared_lane->points[sample_index].position = center.position + center.right * specification.offset;
            }
            const auto ready = finish_geometry(prepared_lane->points, specification.speed_limit,
                specification.lateral_acceleration, limits.coordinate_limit);
            if (!ready) {
                return ready;
            }
            prepared_lane->info.length = prepared_lane->points.back().distance;
            if (prepared_lane->info.length > limits.max_road_length) {
                return {RoadError::Capacity, index};
            }
            prepared_lane->info.travel_seconds = travel_time(prepared_lane->points, 0.0, prepared_lane->info.length);
            output.lanes.push_back(std::move(prepared_lane));
        }
        prepared.descriptor = std::move(descriptor);
        return {};
    }

    bool acquire_lanes(Prepared& prepared) {
        if (prepared.lanes.size() > lane_ids.available_slots()) {
            return false;
        }
        std::size_t acquired = 0;
        try {
            for (auto& handle : prepared.road->lanes) {
                const auto allocated = lane_ids.acquire();
                if (!allocated) {
                    break;
                }
                handle = {allocated->index, allocated->generation};
                ++acquired;
            }
        } catch (...) {
            for (const auto handle : prepared.road->lanes) {
                (void)lane_ids.release(token(handle));
            }
            throw;
        }
        if (acquired != prepared.lanes.size()) {
            for (const auto handle : prepared.road->lanes) {
                (void)lane_ids.release(token(handle));
            }
            return false;
        }
        return true;
    }

    void retire_lanes(RoadRecord& record) noexcept {
        std::erase_if(connections, [&](const auto& candidate) {
            const bool incident = std::find(record.lanes.begin(), record.lanes.end(), candidate.info.from) !=
                record.lanes.end() || std::find(record.lanes.begin(), record.lanes.end(), candidate.info.to) != record.lanes.end();
            if (incident) {
                sample_count -= candidate.points.size();
            }
            return incident;
        });
        for (const auto handle : record.lanes) {
            lanes[handle.index].reset();
            (void)lane_ids.release(token(handle));
        }
        sample_count -= record.sample_count;
    }

    void commit(Prepared prepared, RoadHandle handle) noexcept {
        prepared.road->handle = handle;
        prepared.road->revision = revision + 1;
        for (std::size_t index = 0; index < prepared.lanes.size(); ++index) {
            const auto lane_handle = prepared.road->lanes[index];
            auto& prepared_lane = prepared.lanes[index];
            prepared_lane->info.handle = lane_handle;
            prepared_lane->info.road = handle;
            prepared_lane->info.revision = revision + 1;
            lanes[lane_handle.index] = std::move(prepared_lane);
        }
        sample_count += prepared.road->sample_count;
        roads[handle.index] = std::move(prepared.road);
        changed();
    }
};

std::string_view describe(RoadError error) noexcept {
    switch (error) {
    case RoadError::None: return "ok";
    case RoadError::InvalidInput: return "invalid road input";
    case RoadError::InvalidHandle: return "invalid or retired road/lane/junction handle";
    case RoadError::Capacity: return "road storage or tessellation budget exceeded";
    case RoadError::InvalidCurve: return "invalid or discontinuous centerline curve";
    case RoadError::DegenerateGeometry: return "degenerate, vertical or folded road geometry";
    case RoadError::DuplicateName: return "road name already exists";
    case RoadError::MissingConnection: return "directed lane connection does not exist";
    case RoadError::BoundRoad: return "road has a visual binding";
    case RoadError::MissingService: return "road visual services missing or incompatible";
    case RoadError::SceneRejected: return "scene rejected the road binding";
    case RoadError::MeshRejected: return "mesh library rejected road geometry";
    case RoadError::InconsistentOrigin: return "world origin is inconsistent";
    case RoadError::RevisionExhausted: return "road revision exhausted";
    case RoadError::StaleRoute: return "route predates a network edit";
    case RoadError::NoPath: return "no directed lane route";
    case RoadError::RouteBudget: return "route search or output budget exceeded";
    case RoadError::AccessDenied: return "lane is disabled or inaccessible";
    }
    return "unknown road error";
}

RoadNetwork::RoadNetwork(RoadLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
RoadNetwork::~RoadNetwork() = default;
const RoadLimits& RoadNetwork::limits() const noexcept { return impl_->limits; }
std::uint64_t RoadNetwork::revision() const noexcept { return impl_->revision; }

RoadHandle RoadNetwork::create(RoadDescriptor descriptor, RoadResult* result) {
    const auto reject = [result](RoadError error) {
        if (result != nullptr) {
            *result = {error};
        }
        return RoadHandle{};
    };
    if (!impl_->editable()) {
        return reject(RoadError::RevisionExhausted);
    }
    if (find(descriptor.name)) {
        return reject(RoadError::DuplicateName);
    }
    Impl::Prepared prepared;
    const auto validation = impl_->prepare(std::move(descriptor), prepared);
    if (!validation) {
        if (result != nullptr) {
            *result = validation;
        }
        return {};
    }
    if (prepared.road->sample_count > impl_->limits.max_total_samples - impl_->sample_count) {
        return reject(RoadError::Capacity);
    }
    const auto allocated = impl_->road_ids.acquire();
    if (!allocated) {
        return reject(RoadError::Capacity);
    }
    try {
        if (!impl_->acquire_lanes(prepared)) {
            (void)impl_->road_ids.release(*allocated);
            return reject(RoadError::Capacity);
        }
    } catch (...) {
        (void)impl_->road_ids.release(*allocated);
        throw;
    }
    const RoadHandle handle{allocated->index, allocated->generation};
    impl_->commit(std::move(prepared), handle);
    if (result != nullptr) {
        *result = {};
    }
    return handle;
}

RoadResult RoadNetwork::replace(RoadHandle road, RoadDescriptor descriptor) {
    auto* existing = impl_->road(road);
    if (existing == nullptr) {
        return {RoadError::InvalidHandle};
    }
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    const auto named = find(descriptor.name);
    if (named && named != road) {
        return {RoadError::DuplicateName};
    }
    Impl::Prepared prepared;
    const auto validation = impl_->prepare(std::move(descriptor), prepared);
    if (!validation) {
        return validation;
    }
    const std::size_t retained = impl_->sample_count - existing->sample_count;
    if (prepared.road->sample_count > impl_->limits.max_total_samples - retained ||
        !impl_->acquire_lanes(prepared)) {
        return {RoadError::Capacity};
    }
    impl_->retire_lanes(*existing);
    impl_->commit(std::move(prepared), road);
    return {};
}

RoadResult RoadNetwork::erase(RoadHandle road) {
    auto* existing = impl_->road(road);
    if (existing == nullptr) {
        return {RoadError::InvalidHandle};
    }
    if (impl_->bindings[road.index]) {
        return {RoadError::BoundRoad};
    }
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    impl_->retire_lanes(*existing);
    impl_->roads[road.index].reset();
    (void)impl_->road_ids.release(token(road));
    impl_->changed();
    return {};
}

RoadHandle RoadNetwork::find(std::string_view name) const noexcept {
    for (const auto& record : impl_->roads) {
        if (record && record->descriptor.name == name) {
            return record->handle;
        }
    }
    return {};
}

std::optional<RoadInfo> RoadNetwork::inspect(RoadHandle road) const {
    const auto* record = impl_->road(road);
    if (record == nullptr) {
        return std::nullopt;
    }
    return RoadInfo{road, record->descriptor.name, record->center.back().distance,
        record->center.size(), record->lanes, record->bounds, record->revision};
}

std::optional<LaneInfo> RoadNetwork::lane(LaneHandle handle) const noexcept {
    const auto* record = impl_->lane(handle);
    return record != nullptr ? std::optional<LaneInfo>{record->info} : std::nullopt;
}

std::vector<RoadHandle> RoadNetwork::roads() const {
    std::vector<RoadHandle> result;
    result.reserve(impl_->road_ids.size());
    for (const auto& record : impl_->roads) {
        if (record) {
            result.push_back(record->handle);
        }
    }
    return result;
}

std::vector<LaneHandle> RoadNetwork::lanes(RoadHandle road) const {
    const auto* record = impl_->road(road);
    return record != nullptr ? record->lanes : std::vector<LaneHandle>{};
}

std::span<const LaneSample> RoadNetwork::samples(LaneHandle handle) const noexcept {
    const auto* record = impl_->lane(handle);
    return record != nullptr ? std::span<const LaneSample>{record->points} : std::span<const LaneSample>{};
}

std::optional<LaneSample> RoadNetwork::sample(LaneHandle handle, double distance) const noexcept {
    return interpolate(samples(handle), distance);
}

std::optional<LaneProjection> RoadNetwork::project(LaneHandle handle, math::Vec3 absolute) const noexcept {
    return project_points(handle, samples(handle), absolute);
}

std::optional<LaneProjection> RoadNetwork::nearest(math::Vec3 absolute, double radius,
    std::uint64_t access_mask, std::size_t segment_budget) const noexcept {
    if (!finite_position(absolute, impl_->limits.coordinate_limit) || !std::isfinite(radius) ||
        radius < 0.0 || radius > impl_->limits.coordinate_limit || access_mask == 0) {
        return std::nullopt;
    }
    std::size_t required = 0;
    for (const auto& record : impl_->lanes) {
        if (record && impl_->accessible(*record, access_mask)) {
            const auto count = record->points.size() - 1;
            if (count > segment_budget - required) {
                return std::nullopt;
            }
            required += count;
        }
    }
    std::optional<LaneProjection> best;
    double maximum = radius * radius;
    for (const auto& record : impl_->lanes) {
        if (record && impl_->accessible(*record, access_mask)) {
            const auto projected = project_points(record->info.handle, record->points, absolute);
            if (projected && projected->squared_distance <= maximum) {
                best = projected;
                maximum = projected->squared_distance;
            }
        }
    }
    return best;
}

RoadResult RoadNetwork::set_lane_enabled(LaneHandle handle, bool enabled) {
    auto* record = impl_->lane(handle);
    if (record == nullptr) {
        return {RoadError::InvalidHandle};
    }
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    if (record->info.specification.enabled != enabled) {
        record->info.specification.enabled = enabled;
        record->info.revision = impl_->revision + 1;
        auto* parent = impl_->road(record->info.road);
        parent->descriptor.lanes[record->info.ordinal].enabled = enabled;
        parent->revision = impl_->revision + 1;
        impl_->changed();
    }
    return {};
}

RoadResult RoadNetwork::set_speed_limit(LaneHandle handle, double metres_per_second) {
    auto* record = impl_->lane(handle);
    if (record == nullptr) {
        return {RoadError::InvalidHandle};
    }
    if (!valid_speed(metres_per_second)) {
        return {RoadError::InvalidInput};
    }
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    record->info.specification.speed_limit = metres_per_second;
    for (auto& point : record->points) {
        point.recommended_speed = speed_at(point.curvature, metres_per_second,
            record->info.specification.lateral_acceleration);
    }
    record->info.travel_seconds = travel_time(record->points, 0.0, record->info.length);
    record->info.revision = impl_->revision + 1;
    auto* parent = impl_->road(record->info.road);
    parent->descriptor.lanes[record->info.ordinal].speed_limit = metres_per_second;
    parent->revision = impl_->revision + 1;
    for (auto& edge : impl_->connections) {
        if (edge.info.from == handle || edge.info.to == handle) {
            const auto& first = impl_->lane(edge.info.from)->info.specification;
            const auto& second = impl_->lane(edge.info.to)->info.specification;
            const double speed = std::min({edge.info.specification.speed_limit, first.speed_limit, second.speed_limit});
            for (auto& point : edge.points) {
                point.recommended_speed = speed_at(point.curvature, speed,
                    std::min(first.lateral_acceleration, second.lateral_acceleration));
            }
            edge.info.travel_seconds = travel_time(edge.points, 0.0, edge.info.length) +
                edge.info.specification.delay_seconds;
        }
    }
    impl_->changed();
    return {};
}

JunctionHandle RoadNetwork::create_junction(std::string name, math::Vec3 absolute, double radius) {
    if (!impl_->editable() || !valid_name(name, impl_->limits.max_name_bytes) ||
        !finite_position(absolute, impl_->limits.coordinate_limit) || !std::isfinite(radius) ||
        radius < 0.1 || radius > impl_->limits.max_road_length) {
        return {};
    }
    for (const auto& record : impl_->junctions) {
        if (record && record->name == name) {
            return {};
        }
    }
    auto prepared = std::make_unique<JunctionInfo>();
    prepared->name = std::move(name);
    prepared->absolute_position = absolute;
    prepared->radius = radius;
    const auto allocated = impl_->junction_ids.acquire();
    if (!allocated) {
        return {};
    }
    const JunctionHandle handle{allocated->index, allocated->generation};
    prepared->handle = handle;
    impl_->junctions[handle.index] = std::move(prepared);
    impl_->changed();
    return handle;
}

std::optional<JunctionInfo> RoadNetwork::junction(JunctionHandle handle) const {
    const auto* record = impl_->junction(handle);
    return record != nullptr ? std::optional<JunctionInfo>{*record} : std::nullopt;
}

RoadResult RoadNetwork::set_junction_enabled(JunctionHandle handle, bool enabled) {
    auto* record = impl_->junction(handle);
    if (record == nullptr) {
        return {RoadError::InvalidHandle};
    }
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    if (record->enabled != enabled) {
        record->enabled = enabled;
        impl_->changed();
    }
    return {};
}

RoadResult RoadNetwork::erase_junction(JunctionHandle handle) {
    if (impl_->junction(handle) == nullptr) {
        return {RoadError::InvalidHandle};
    }
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    std::erase_if(impl_->connections, [&](const auto& record) {
        if (record.info.specification.junction != handle) {
            return false;
        }
        impl_->sample_count -= record.points.size();
        return true;
    });
    impl_->junctions[handle.index].reset();
    (void)impl_->junction_ids.release(token(handle));
    impl_->changed();
    return {};
}

RoadResult RoadNetwork::connect(LaneHandle from, LaneHandle to, ConnectionSpecification specification) {
    const auto* source = impl_->lane(from);
    const auto* destination = impl_->lane(to);
    const auto* at_junction = impl_->junction(specification.junction);
    if (source == nullptr || destination == nullptr ||
        (specification.junction && at_junction == nullptr)) {
        return {RoadError::InvalidHandle};
    }
    if (from == to || !valid_speed(specification.speed_limit) ||
        !std::isfinite(specification.delay_seconds) || specification.delay_seconds < 0.0 ||
        specification.delay_seconds > 86400.0 || !std::isfinite(specification.tangent_scale) ||
        specification.tangent_scale < 0.0 || specification.tangent_scale > 2.0 || specification.access_mask == 0) {
        return {RoadError::InvalidInput};
    }
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    auto* existing = impl_->connection(from, to);
    const auto outgoing_count = static_cast<std::size_t>(std::count_if(impl_->connections.begin(),
        impl_->connections.end(), [from](const auto& record) { return record.info.from == from; }));
    if (existing == nullptr && (impl_->connections.size() >= impl_->limits.max_connections ||
        outgoing_count >= impl_->limits.max_outgoing)) {
        return {RoadError::Capacity};
    }
    const auto& start = source->points.back();
    const auto& finish = destination->points.front();
    const double chord = math::distance(start.position, finish.position);
    if (chord > impl_->limits.max_road_length ||
        (at_junction != nullptr && (math::distance(start.position, at_junction->absolute_position) > at_junction->radius ||
         math::distance(finish.position, at_junction->absolute_position) > at_junction->radius))) {
        return {RoadError::InvalidInput};
    }
    Impl::ConnectionRecord prepared;
    prepared.info = {from, to, specification, 0.0, 0.0};
    const double speed = std::min({specification.speed_limit, source->info.specification.speed_limit,
                                   destination->info.specification.speed_limit});
    const double acceleration = std::min(source->info.specification.lateral_acceleration,
                                          destination->info.specification.lateral_acceleration);
    if (chord < minimum_segment) {
        if (math::dot(start.tangent, finish.tangent) < 0.95) {
            return {RoadError::DegenerateGeometry};
        }
        prepared.points = {start, finish};
        for (auto& point : prepared.points) {
            point.distance = 0.0;
            point.curvature = 0.0;
            point.recommended_speed = speed;
        }
    } else {
        animation::CurveKey<math::Vec3> first;
        first.value = start.position;
        first.outgoing = start.tangent * (chord * specification.tangent_scale);
        first.interpolation = animation::CurveInterpolation::Hermite;
        first.tangents = animation::CurveTangentPolicy::Manual;
        animation::CurveKey<math::Vec3> last;
        last.time = 1.0;
        last.value = finish.position;
        last.incoming = finish.tangent * (chord * specification.tangent_scale);
        last.tangents = animation::CurveTangentPolicy::Manual;
        animation::VectorCurve curve;
        if (!curve.assign({first, last})) {
            return {RoadError::InvalidCurve};
        }
        animation::CurveArcTable arc;
        if (!arc.build(curve, 0.0, 1.0, impl_->limits.max_connector_samples)) {
            return {RoadError::InvalidCurve};
        }
        const double intervals = std::clamp(std::ceil(arc.length()), 1.0,
            static_cast<double>(impl_->limits.max_connector_samples - 1));
        const auto count = static_cast<std::size_t>(intervals) + 1;
        prepared.points.resize(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto time = arc.time_at_distance(arc.length() * static_cast<double>(index) /
                static_cast<double>(count - 1));
            const auto point = time ? curve.sample(*time) : std::nullopt;
            if (!point) {
                return {RoadError::InvalidCurve};
            }
            prepared.points[index].position = point->value;
            if (at_junction != nullptr && math::distance(point->value, at_junction->absolute_position) > at_junction->radius) {
                return {RoadError::InvalidInput, index};
            }
        }
        const auto finalized = finish_geometry(prepared.points, speed, acceleration, impl_->limits.coordinate_limit);
        if (!finalized) {
            return finalized;
        }
    }
    prepared.info.length = prepared.points.back().distance;
    if (prepared.info.length > impl_->limits.max_road_length) {
        return {RoadError::Capacity};
    }
    prepared.info.travel_seconds = travel_time(prepared.points, 0.0, prepared.info.length) + specification.delay_seconds;
    const std::size_t replaced_samples = existing != nullptr ? existing->points.size() : 0;
    const std::size_t retained = impl_->sample_count - replaced_samples;
    if (prepared.points.size() > impl_->limits.max_total_samples - retained) {
        return {RoadError::Capacity};
    }
    const std::size_t added_samples = prepared.points.size();
    if (existing != nullptr) {
        *existing = std::move(prepared);
    } else {
        const auto insertion = std::lower_bound(impl_->connections.begin(), impl_->connections.end(), std::pair{from, to},
            [](const auto& entry, const auto& target) { return std::pair{entry.info.from, entry.info.to} < target; });
        impl_->connections.insert(insertion, std::move(prepared));
    }
    impl_->sample_count = retained + added_samples;
    impl_->changed();
    return {};
}

RoadResult RoadNetwork::disconnect(LaneHandle from, LaneHandle to) {
    if (!impl_->editable()) {
        return {RoadError::RevisionExhausted};
    }
    const auto found = std::lower_bound(impl_->connections.begin(), impl_->connections.end(), std::pair{from, to},
        [](const auto& entry, const auto& target) { return std::pair{entry.info.from, entry.info.to} < target; });
    if (found == impl_->connections.end() || found->info.from != from || found->info.to != to) {
        return {RoadError::MissingConnection};
    }
    impl_->sample_count -= found->points.size();
    impl_->connections.erase(found);
    impl_->changed();
    return {};
}

std::optional<LaneConnection> RoadNetwork::connection(LaneHandle from, LaneHandle to) const noexcept {
    const auto* record = impl_->connection(from, to);
    return record != nullptr ? std::optional<LaneConnection>{record->info} : std::nullopt;
}

std::vector<LaneConnection> RoadNetwork::outgoing(LaneHandle from) const {
    std::vector<LaneConnection> result;
    auto cursor = std::lower_bound(impl_->connections.begin(), impl_->connections.end(), std::pair{from, LaneHandle{}},
        [](const auto& entry, const auto& target) { return std::pair{entry.info.from, entry.info.to} < target; });
    while (cursor != impl_->connections.end() && cursor->info.from == from) {
        result.push_back(cursor->info);
        ++cursor;
    }
    return result;
}

std::optional<LaneSample> RoadNetwork::sample_connection(LaneHandle from, LaneHandle to, double distance) const noexcept {
    const auto* record = impl_->connection(from, to);
    return record != nullptr ? interpolate(record->points, distance) : std::nullopt;
}

RoadResult RoadNetwork::rebuild_graph(std::uint64_t access_mask) {
    if (access_mask == 0) {
        return {RoadError::InvalidInput};
    }
    if (impl_->graph && impl_->graph_network_revision == impl_->revision && impl_->graph_mask == access_mask) {
        return {};
    }
    if (impl_->graph_epoch == std::numeric_limits<std::uint64_t>::max()) {
        return {RoadError::RevisionExhausted};
    }
    navigation::GraphLimits graph_limits;
    graph_limits.max_nodes = impl_->limits.max_lanes;
    graph_limits.max_edges = impl_->limits.max_connections;
    graph_limits.max_edges_per_node = impl_->limits.max_outgoing;
    auto prepared = std::make_unique<navigation::NavigationGraph>(graph_limits);
    std::vector<navigation::NodeId> lane_nodes(impl_->lanes.size());
    std::vector<LaneHandle> node_lanes(impl_->lanes.size());
    for (const auto& record : impl_->lanes) {
        if (record && impl_->accessible(*record, access_mask)) {
            const auto node = prepared->add_node(record->points.front().position);
            if (!node) {
                return {RoadError::Capacity};
            }
            lane_nodes[record->info.handle.index] = node;
            node_lanes[node.index] = record->info.handle;
        }
    }
    for (const auto& record : impl_->connections) {
        const auto from = lane_nodes[record.info.from.index];
        const auto to = lane_nodes[record.info.to.index];
        if (from && to && impl_->connected(record, access_mask)) {
            const double cost = impl_->lane(record.info.from)->info.travel_seconds + record.info.travel_seconds;
            if (!prepared->connect(from, to, cost)) {
                return {RoadError::Capacity};
            }
        }
    }
    impl_->graph = std::move(prepared);
    impl_->lane_nodes = std::move(lane_nodes);
    impl_->node_lanes = std::move(node_lanes);
    impl_->graph_network_revision = impl_->revision;
    impl_->graph_mask = access_mask;
    ++impl_->graph_epoch;
    return {};
}

const navigation::NavigationGraph* RoadNetwork::graph() const noexcept {
    return impl_->graph_network_revision == impl_->revision ? impl_->graph.get() : nullptr;
}

RoadRoute RoadNetwork::route(LaneHandle from, double from_distance, LaneHandle to,
                            double to_distance, RouteOptions options) {
    RoadRoute result;
    result.network_revision = impl_->revision;
    result.access_mask = options.access_mask;
    const auto* source = impl_->lane(from);
    const auto* destination = impl_->lane(to);
    if (source == nullptr || destination == nullptr) {
        result.error = RoadError::InvalidHandle;
        return result;
    }
    if (!std::isfinite(from_distance) || !std::isfinite(to_distance) || from_distance < 0.0 ||
        to_distance < 0.0 || from_distance > source->info.length || to_distance > destination->info.length ||
        (from == to && to_distance < from_distance) || options.access_mask == 0 || options.max_lanes == 0 ||
        options.max_work == 0 || std::isnan(options.max_seconds) || options.max_seconds < 0.0) {
        result.error = RoadError::InvalidInput;
        return result;
    }
    if (!impl_->accessible(*source, options.access_mask) || !impl_->accessible(*destination, options.access_mask)) {
        result.error = RoadError::AccessDenied;
        return result;
    }
    const auto built = rebuild_graph(options.access_mask);
    if (!built) {
        result.error = built.error;
        return result;
    }
    result.graph_epoch = impl_->graph_epoch;
    result.graph_revision = impl_->graph->revision();
    navigation::SearchOptions search_options;
    search_options.max_visited = impl_->limits.max_lanes;
    search_options.max_frontier = impl_->limits.max_connections + static_cast<std::size_t>(impl_->limits.max_lanes) + 1;
    search_options.max_output = std::min(options.max_lanes, impl_->limits.max_route_lanes);
    search_options.max_storage_nodes = impl_->limits.max_lanes;
    search_options.max_work = std::min(options.max_work, impl_->limits.max_route_work);
    search_options.max_cost = options.max_seconds + travel_time(source->points, 0.0, from_distance);
    search_options.expected_revision = result.graph_revision;
    navigation::PathSearch search;
    const auto started = search.begin(*impl_->graph, impl_->lane_nodes[from.index], impl_->lane_nodes[to.index], search_options);
    const auto status = started == navigation::PathStatus::Running ? search.step(search_options.max_work + 1) : started;
    result.search_work = search.result().work;
    if (status != navigation::PathStatus::Success) {
        result.error = status == navigation::PathStatus::NoPath ? RoadError::NoPath : RoadError::RouteBudget;
        return result;
    }
    const auto& path = search.result();
    result.lanes.reserve(path.nodes.size());
    result.legs.reserve(path.nodes.size() * 2);
    for (std::size_t index = 0; index < path.nodes.size(); ++index) {
        const auto handle = impl_->node_lanes[path.nodes[index].index];
        const auto* record = impl_->lane(handle);
        const double begin = index == 0 ? from_distance : 0.0;
        const double end = index + 1 == path.nodes.size() ? to_distance : record->info.length;
        const double seconds = travel_time(record->points, begin, end);
        result.lanes.push_back(handle);
        result.legs.push_back({handle, {}, begin, end, result.length, seconds});
        result.length += end - begin;
        result.travel_seconds += seconds;
        if (index + 1 < path.nodes.size()) {
            const auto next = impl_->node_lanes[path.nodes[index + 1].index];
            const auto* edge = impl_->connection(handle, next);
            if (edge == nullptr) {
                result.error = RoadError::StaleRoute;
                result.lanes.clear();
                result.legs.clear();
                return result;
            }
            result.legs.push_back({handle, next, 0.0, edge->info.length, result.length, edge->info.travel_seconds});
            result.length += edge->info.length;
            result.travel_seconds += edge->info.travel_seconds;
        }
    }
    result.error = result.travel_seconds <= options.max_seconds ? RoadError::None : RoadError::RouteBudget;
    return result;
}

bool RoadNetwork::route_current(const RoadRoute& route_value) const noexcept {
    if (!route_value || route_value.network_revision != impl_->revision || route_value.lanes.empty() ||
        route_value.lanes.size() > impl_->limits.max_route_lanes ||
        route_value.legs.size() != route_value.lanes.size() * 2 - 1 ||
        !std::isfinite(route_value.length) || route_value.length < 0.0 ||
        !std::isfinite(route_value.travel_seconds) || route_value.travel_seconds < 0.0) {
        return false;
    }
    double distance = 0.0;
    for (const auto& leg : route_value.legs) {
        if (!std::isfinite(leg.begin_distance) || !std::isfinite(leg.end_distance) ||
            leg.begin_distance < 0.0 || leg.end_distance < leg.begin_distance ||
            leg.route_distance != distance || impl_->lane(leg.lane) == nullptr) {
            return false;
        }
        const auto* edge = leg.connector_to ? impl_->connection(leg.lane, leg.connector_to) : nullptr;
        const double maximum = leg.connector_to ? (edge != nullptr ? edge->info.length : -1.0) :
            impl_->lane(leg.lane)->info.length;
        if (leg.end_distance > maximum) {
            return false;
        }
        distance += leg.end_distance - leg.begin_distance;
    }
    if (distance != route_value.length) {
        return false;
    }
    return std::all_of(route_value.lanes.begin(), route_value.lanes.end(),
        [this](LaneHandle handle) { return impl_->lane(handle) != nullptr; });
}

std::optional<RouteSample> RoadNetwork::sample_route(const RoadRoute& route_value, double distance) const noexcept {
    if (!route_current(route_value) || route_value.legs.empty() || !std::isfinite(distance)) {
        return std::nullopt;
    }
    const double bounded = std::clamp(distance, 0.0, route_value.length);
    for (std::size_t index = 0; index < route_value.legs.size(); ++index) {
        const auto& leg = route_value.legs[index];
        const double end = leg.route_distance + leg.end_distance - leg.begin_distance;
        if (bounded < end || index + 1 == route_value.legs.size()) {
            const double local = leg.begin_distance + bounded - leg.route_distance;
            const auto point = leg.connector_to ? sample_connection(leg.lane, leg.connector_to, local) : sample(leg.lane, local);
            if (!point) {
                return std::nullopt;
            }
            return RouteSample{*point, leg.lane, leg.connector_to, index,
                route_value.length - bounded, bounded >= route_value.length};
        }
    }
    return std::nullopt;
}

std::optional<RoadMesh> RoadNetwork::build_mesh(RoadHandle road) const {
    const auto* record = impl_->road(road);
    if (record == nullptr) {
        return std::nullopt;
    }
    double left = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    for (const auto& specification : record->descriptor.lanes) {
        left = std::min(left, specification.offset - specification.width * 0.5);
        right = std::max(right, specification.offset + specification.width * 0.5);
    }
    left -= record->descriptor.shoulder_width;
    right += record->descriptor.shoulder_width;
    RoadMesh result;
    result.absolute_anchor = record->center.front().position;
    result.road_revision = record->revision;
    result.mesh.vertices.reserve(record->center.size() * 2);
    result.mesh.indices.reserve((record->center.size() - 1) * 6);
    for (const auto& point : record->center) {
        if (std::abs(point.curvature) * std::max(std::abs(left), std::abs(right)) >= 0.9) {
            return std::nullopt;
        }
        for (const double offset : {left, right}) {
            geometry::Vertex vertex;
            vertex.position = point.position - result.absolute_anchor + point.right * offset;
            vertex.normal = point.up;
            vertex.tangent = {point.right.x, point.right.y, point.right.z, 1.0};
            vertex.uv = {(offset - left) / record->descriptor.texture_metres,
                         point.distance / record->descriptor.texture_metres};
            result.mesh.vertices.push_back(vertex);
        }
    }
    for (std::size_t index = 1; index < record->center.size(); ++index) {
        const auto base = static_cast<geometry::MeshIndex>((index - 1) * 2);
        result.mesh.indices.insert(result.mesh.indices.end(),
            {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    }
    result.mesh.submeshes.push_back({0, static_cast<geometry::MeshIndex>(result.mesh.indices.size()), 0});
    if (!geometry::validate_mesh(result.mesh, {}, true)) {
        return std::nullopt;
    }
    return result;
}

RoadResult RoadNetwork::bind(scene::Scene& scene_graph, geometry::MeshLibrary& meshes,
    render::MaterialService* materials, const scene::WorldPartition* partition,
    RoadHandle road, render::MaterialHandle material) {
    const auto* record = impl_->road(road);
    if (record == nullptr) {
        return {RoadError::InvalidHandle};
    }
    if (impl_->bindings[road.index]) {
        return {RoadError::BoundRoad};
    }
    if (materials == nullptr || (impl_->bound_scene != nullptr && impl_->bound_scene != &scene_graph) ||
        (impl_->bound_meshes != nullptr && impl_->bound_meshes != &meshes)) {
        return {RoadError::MissingService};
    }
    auto generated = build_mesh(road);
    if (!generated) {
        return {RoadError::MeshRejected};
    }
    const auto runtime_anchor = partition != nullptr ? partition->to_local(generated->absolute_anchor) :
        std::optional<math::Vec3>{generated->absolute_anchor};
    if (!runtime_anchor) {
        return {RoadError::InconsistentOrigin};
    }
    auto& material_library = materials->materials();
    if (!material) {
        material = material_library.find("traffic.roads.asphalt");
        if (!material) {
            render::PbrMaterial asphalt;
            asphalt.albedo = {0.055, 0.06, 0.065, 1.0};
            asphalt.roughness = 0.92;
            material = material_library.create("traffic.roads.asphalt", asphalt);
        }
    }
    if (!material || material_library.get(material) == nullptr) {
        return {RoadError::MissingService};
    }
    const auto bounds = geometry::mesh_bounds(generated->mesh);
    if (!bounds) {
        return {RoadError::MeshRejected};
    }
    const auto mesh = meshes.create(std::move(generated->mesh), "traffic.road." +
        std::to_string(road.index) + "." + std::to_string(road.generation));
    if (!mesh) {
        return {RoadError::MeshRejected};
    }
    ecs::Entity entity{};
    try {
        math::Transform transform;
        transform.position = *runtime_anchor;
        entity = scene_graph.create("road:" + record->descriptor.name, {}, transform);
        if (!entity || !meshes.assign(scene_graph.registry(), entity, mesh) ||
            !material_library.assign(scene_graph.registry(), entity, material) ||
            !scene_graph.set_local_bounds(entity, *bounds)) {
            if (entity) {
                (void)scene_graph.destroy(entity);
            }
            (void)meshes.destroy(mesh);
            return {RoadError::SceneRejected};
        }
        (void)scene_graph.registry().emplace<RoadComponent>(entity, RoadComponent{road});
    } catch (...) {
        if (entity) {
            (void)scene_graph.destroy(entity);
        }
        (void)meshes.destroy(mesh);
        throw;
    }
    impl_->bindings[road.index] = RoadBinding{road, entity, mesh, material, record->revision,
        partition != nullptr ? partition->origin().epoch : 0, RoadError::None};
    impl_->bound_scene = &scene_graph;
    impl_->bound_meshes = &meshes;
    return {};
}

RoadResult RoadNetwork::unbind(scene::Scene& scene_graph, geometry::MeshLibrary& meshes, RoadHandle road) {
    if (impl_->road(road) == nullptr) {
        return {RoadError::InvalidHandle};
    }
    auto& record = impl_->bindings[road.index];
    if (!record) {
        return {};
    }
    if (impl_->bound_scene != &scene_graph || impl_->bound_meshes != &meshes) {
        return {RoadError::MissingService};
    }
    if (scene_graph.registry().alive(record->entity)) {
        const auto* ownership = scene_graph.registry().get<RoadComponent>(record->entity);
        if (!scene_graph.contains(record->entity) || ownership == nullptr || ownership->road != road ||
            !scene_graph.destroy(record->entity, scene::DestroyPolicy::DetachChildren)) {
            return {RoadError::SceneRejected};
        }
    }
    (void)meshes.destroy(record->mesh);
    record.reset();
    if (std::none_of(impl_->bindings.begin(), impl_->bindings.end(), [](const auto& value) { return value.has_value(); })) {
        impl_->bound_scene = nullptr;
        impl_->bound_meshes = nullptr;
    }
    return {};
}

void RoadNetwork::clear_bindings(scene::Scene& scene_graph, geometry::MeshLibrary& meshes) {
    for (const auto& record : impl_->roads) {
        if (record) {
            (void)unbind(scene_graph, meshes, record->handle);
        }
    }
}

std::optional<RoadBinding> RoadNetwork::binding(RoadHandle road) const noexcept {
    return impl_->road(road) != nullptr ? impl_->bindings[road.index] : std::nullopt;
}

RoadSync RoadNetwork::sync(scene::Scene& scene_graph, geometry::MeshLibrary& meshes,
    const scene::WorldPartition* partition, std::size_t budget) {
    RoadSync result;
    if (impl_->bound_scene != &scene_graph || impl_->bound_meshes != &meshes || budget == 0) {
        return result;
    }
    const auto origin = partition != nullptr ? partition->origin() : scene::OriginState{};
    for (std::size_t scanned = 0; scanned + 1 < impl_->bindings.size() && result.inspected < budget; ++scanned) {
        if (impl_->sync_cursor >= impl_->bindings.size()) {
            impl_->sync_cursor = 1;
        }
        auto& binding_value = impl_->bindings[impl_->sync_cursor++];
        if (!binding_value) {
            continue;
        }
        ++result.inspected;
        auto& bound = *binding_value;
        if (!scene_graph.registry().alive(bound.entity)) {
            (void)meshes.destroy(bound.mesh);
            binding_value.reset();
            ++result.retired;
            continue;
        }
        const auto reject = [&](RoadError error) {
            bound.last_error = error;
            ++result.rejected;
        };
        const auto* ownership = scene_graph.registry().get<RoadComponent>(bound.entity);
        const auto* assigned = scene_graph.registry().get<geometry::MeshComponent>(bound.entity);
        const auto* road_record = impl_->road(bound.road);
        const auto old_transform = scene_graph.local_transform(bound.entity);
        const auto old_mesh = meshes.inspect(bound.mesh);
        if (ownership == nullptr || ownership->road != bound.road || assigned == nullptr ||
            assigned->mesh != bound.mesh || road_record == nullptr || !old_transform || !old_mesh ||
            scene_graph.parent(bound.entity)) {
            reject(RoadError::SceneRejected);
            continue;
        }
        if (!origin.consistent) {
            reject(RoadError::InconsistentOrigin);
            continue;
        }
        const auto anchor = road_record->center.front().position;
        const auto runtime_anchor = partition != nullptr ? partition->to_local(anchor) : std::optional<math::Vec3>{anchor};
        if (!runtime_anchor) {
            reject(RoadError::InconsistentOrigin);
            continue;
        }
        const bool changed_geometry = bound.road_revision != road_record->revision;
        const bool changed_transform = old_transform->position != *runtime_anchor ||
            old_transform->rotation != math::Quat{} || old_transform->scale != math::Vec3{1.0, 1.0, 1.0};
        if (!changed_geometry && !changed_transform && bound.origin_epoch == origin.epoch) {
            bound.last_error = RoadError::None;
            continue;
        }
        auto generated = changed_geometry ? build_mesh(bound.road) : std::optional<RoadMesh>{};
        if (changed_geometry && !generated) {
            reject(RoadError::MeshRejected);
            continue;
        }
        const auto new_bounds = generated ? geometry::mesh_bounds(generated->mesh) :
            std::optional<math::Aabb>{old_mesh->bounds};
        math::Transform desired;
        desired.position = *runtime_anchor;
        if (!new_bounds || !scene_graph.set_local_transform(bound.entity, desired)) {
            reject(RoadError::SceneRejected);
            continue;
        }
        const auto restore = [&] {
            (void)scene_graph.set_local_transform(bound.entity, *old_transform);
            (void)scene_graph.set_local_bounds(bound.entity, old_mesh->bounds);
        };
        if (!scene_graph.set_local_bounds(bound.entity, *new_bounds)) {
            restore();
            reject(RoadError::SceneRejected);
            continue;
        }
        try {
            if (generated && !meshes.replace(bound.mesh, std::move(generated->mesh))) {
                restore();
                reject(RoadError::MeshRejected);
                continue;
            }
        } catch (...) {
            restore();
            bound.last_error = RoadError::MeshRejected;
            throw;
        }
        bound.road_revision = road_record->revision;
        bound.origin_epoch = origin.epoch;
        bound.last_error = RoadError::None;
        ++result.updated;
    }
    if (std::none_of(impl_->bindings.begin(), impl_->bindings.end(), [](const auto& value) { return value.has_value(); })) {
        impl_->bound_scene = nullptr;
        impl_->bound_meshes = nullptr;
    }
    return result;
}

RoadStatistics RoadNetwork::statistics() const noexcept {
    RoadStatistics result;
    result.roads = impl_->road_ids.size();
    result.lanes = impl_->lane_ids.size();
    result.junctions = impl_->junction_ids.size();
    result.connections = impl_->connections.size();
    result.samples = impl_->sample_count;
    result.bindings = static_cast<std::size_t>(std::count_if(impl_->bindings.begin(), impl_->bindings.end(),
        [](const auto& value) { return value.has_value(); }));
    result.revision = impl_->revision;
    result.graph_epoch = impl_->graph_epoch;
    result.graph_current = impl_->graph && impl_->graph_network_revision == impl_->revision;
    return result;
}

namespace {

std::optional<LaneHandle> command_lane(const RoadNetwork& roads, std::string_view name, std::string_view index_text) {
    const auto index = parse_index(index_text);
    const auto handles = roads.lanes(roads.find(name));
    return index && *index < handles.size() ? std::optional<LaneHandle>{handles[*index]} : std::nullopt;
}

core::CommandResult command_result(RoadResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error))};
}

} // namespace

void register_roads(core::Kernel& kernel) {
    (void)kernel.register_module({"traffic.roads", "Absolute road geometry, lane connections and bounded routing", 1});
    (void)kernel.services().emplace<RoadNetwork>();
    (void)kernel.add_command("roads.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 7) {
            return {false, "usage: roads.create NAME X0 Z0 X1 Z1 WIDTH SPEED_MPS"};
        }
        const auto x0 = parse_real(args[1]);
        const auto z0 = parse_real(args[2]);
        const auto x1 = parse_real(args[3]);
        const auto z1 = parse_real(args[4]);
        const auto width = parse_real(args[5]);
        const auto speed = parse_real(args[6]);
        if (!x0 || !z0 || !x1 || !z1 || !width || !speed) {
            return {false, "finite absolute coordinates, width and speed required"};
        }
        RoadDescriptor descriptor;
        descriptor.name = args[0];
        animation::CurveKey<math::Vec3> first;
        first.value = {*x0, 0.0, *z0};
        animation::CurveKey<math::Vec3> last;
        last.time = 1.0;
        last.value = {*x1, 0.0, *z1};
        if (!descriptor.centerline.assign({first, last})) {
            return {false, "invalid centerline"};
        }
        LaneSpecification lane_specification;
        lane_specification.width = *width;
        lane_specification.speed_limit = *speed;
        descriptor.lanes.push_back(lane_specification);
        RoadResult outcome;
        const auto road = host.services().get<RoadNetwork>().create(std::move(descriptor), &outcome);
        return {static_cast<bool>(road), road ? "road created in absolute coordinates" : std::string(describe(outcome.error))};
    }, "Create a directed straight road; author curved multilane roads through RoadDescriptor");
    (void)kernel.add_command("roads.connect", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4) {
            return {false, "usage: roads.connect FROM_ROAD FROM_LANE TO_ROAD TO_LANE"};
        }
        auto& roads = host.services().get<RoadNetwork>();
        const auto from = command_lane(roads, args[0], args[1]);
        const auto to = command_lane(roads, args[2], args[3]);
        return from && to ? command_result(roads.connect(*from, *to)) : core::CommandResult{false, "unknown road or lane ordinal"};
    }, "Create or replace a directed, sampled lane connector");
    (void)kernel.add_command("roads.speed", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 3) {
            return {false, "usage: roads.speed ROAD LANE SPEED_MPS"};
        }
        auto& roads = host.services().get<RoadNetwork>();
        const auto lane_handle = command_lane(roads, args[0], args[1]);
        const auto speed = parse_real(args[2]);
        return lane_handle && speed ? command_result(roads.set_speed_limit(*lane_handle, *speed)) :
            core::CommandResult{false, "unknown lane or invalid speed"};
    }, "Update lane and incident connector speeds, invalidating existing routes");
    (void)kernel.add_command("roads.sample", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 3) {
            return {false, "usage: roads.sample ROAD LANE DISTANCE_METRES"};
        }
        const auto& roads = host.services().get<RoadNetwork>();
        const auto lane_handle = command_lane(roads, args[0], args[1]);
        const auto distance = parse_real(args[2]);
        const auto point = lane_handle && distance ? roads.sample(*lane_handle, *distance) : std::nullopt;
        return {point.has_value(), point ? "absolute=" + std::to_string(point->position.x) + "," +
            std::to_string(point->position.y) + "," + std::to_string(point->position.z) +
            " curvature=" + std::to_string(point->curvature) + " speed=" + std::to_string(point->recommended_speed) :
            "invalid lane or distance"};
    }, "Sample clamped lane distance in travel direction, returning absolute position");
    (void)kernel.add_command("roads.route", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4) {
            return {false, "usage: roads.route FROM_ROAD FROM_LANE TO_ROAD TO_LANE"};
        }
        auto& roads = host.services().get<RoadNetwork>();
        const auto from = command_lane(roads, args[0], args[1]);
        const auto to = command_lane(roads, args[2], args[3]);
        if (!from || !to) {
            return {false, "unknown road or lane ordinal"};
        }
        const auto route_value = roads.route(*from, 0.0, *to, roads.lane(*to)->length);
        return {static_cast<bool>(route_value), route_value ? "lanes=" + std::to_string(route_value.lanes.size()) +
            " metres=" + std::to_string(route_value.length) + " seconds=" + std::to_string(route_value.travel_seconds) :
            std::string(describe(route_value.error))};
    }, "Find a bounded minimum estimated travel-time route through directed lanes");
    (void)kernel.add_command("roads.bind", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) {
            return {false, "usage: roads.bind ROAD"};
        }
        auto* scene_graph = host.services().find<scene::Scene>();
        auto* meshes = host.services().find<geometry::MeshLibrary>();
        if (scene_graph == nullptr || meshes == nullptr) {
            return {false, "Scene and MeshLibrary required"};
        }
        auto& roads = host.services().get<RoadNetwork>();
        return command_result(roads.bind(*scene_graph, *meshes, host.services().find<render::MaterialService>(),
            host.services().find<scene::WorldPartition>(), roads.find(args[0])));
    }, "Publish actual mesh, material, bounds and Registry-owned Scene entity");
    (void)kernel.add_command("roads.erase", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) {
            return {false, "usage: roads.erase ROAD"};
        }
        auto& roads = host.services().get<RoadNetwork>();
        const auto road = roads.find(args[0]);
        if (roads.binding(road)) {
            auto* scene_graph = host.services().find<scene::Scene>();
            auto* meshes = host.services().find<geometry::MeshLibrary>();
            if (scene_graph == nullptr || meshes == nullptr) {
                return {false, "Scene and MeshLibrary required to release binding"};
            }
            const auto released = roads.unbind(*scene_graph, *meshes, road);
            if (!released) {
                return command_result(released);
            }
        }
        return command_result(roads.erase(road));
    }, "Release road visuals, retire lanes and remove incident connections");
    (void)kernel.add_command("roads.stats", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) {
            return {false, "usage: roads.stats"};
        }
        const auto stats = host.services().get<RoadNetwork>().statistics();
        return {true, "roads=" + std::to_string(stats.roads) + " lanes=" + std::to_string(stats.lanes) +
            " connections=" + std::to_string(stats.connections) + " samples=" + std::to_string(stats.samples) +
            " revision=" + std::to_string(stats.revision) + " bindings=" + std::to_string(stats.bindings)};
    }, "Inspect road geometry, graph and binding resource counts");
    (void)kernel.add_system({"traffic.roads.sync", core::Phase::PreSimulation, -8500, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& roads = host.services().get<RoadNetwork>();
            auto* scene_graph = host.services().find<scene::Scene>();
            auto* meshes = host.services().find<geometry::MeshLibrary>();
            if (scene_graph != nullptr && meshes != nullptr) {
                try {
                    const auto report = roads.sync(*scene_graph, *meshes, host.services().find<scene::WorldPartition>(), 16);
                    host.metric("traffic.roads.binding_rejections", static_cast<double>(report.rejected));
                } catch (const std::exception& error) {
                    host.diagnose(core::Diagnostic::Severity::Warning, "traffic.roads", error.what());
                }
            }
            const auto stats = roads.statistics();
            host.metric("traffic.roads.count", static_cast<double>(stats.roads));
            host.metric("traffic.roads.lanes", static_cast<double>(stats.lanes));
            host.metric("traffic.roads.samples", static_cast<double>(stats.samples));
        });
    (void)kernel.order_after("traffic.roads.sync", "scene.partition.update");
    (void)kernel.add_shutdown("traffic.roads.release", [host = &kernel] {
        auto* scene_graph = host->services().find<scene::Scene>();
        auto* meshes = host->services().find<geometry::MeshLibrary>();
        if (scene_graph != nullptr && meshes != nullptr) {
            host->services().get<RoadNetwork>().clear_bindings(*scene_graph, *meshes);
        }
    });
}

} // namespace ge::traffic
