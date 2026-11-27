#include <ge/traffic/simulation.hpp>

#include <ge/core/runtime.hpp>
#include <ge/scene/partition.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace ge::traffic {
namespace {

bool between(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

double braking_speed(double distance, double deceleration) noexcept {
    return std::sqrt(2.0 * deceleration * std::max(0.0, distance));
}

bool valid_limits(const TrafficLimits& limits) noexcept {
    return limits.agents <= 16384 && limits.junctions <= 4096 &&
        limits.phases_per_junction > 0 && limits.phases_per_junction <= 128 &&
        limits.movements_per_phase > 0 && limits.movements_per_phase <= 1024 &&
        limits.route_legs > 0 && limits.route_legs <= 16383 &&
        limits.route_work > 0 && limits.route_work <= 1048576 &&
        limits.route_queries_per_frame <= 1024 && limits.projection_samples >= 2 &&
        limits.projection_samples <= 4096 && limits.planning_samples >= 2 &&
        limits.planning_samples <= 4096 && limits.sensor_queries_per_frame <= 1048576 &&
        limits.sensor_candidates > 0 && limits.sensor_candidates <= 4096 &&
        between(limits.maximum_delta, 0.001, 0.25);
}

bool valid_rule(JunctionRule rule) noexcept {
    return rule == JunctionRule::Priority || rule == JunctionRule::Stop ||
        rule == JunctionRule::Yield || rule == JunctionRule::Signals;
}

math::Aabb envelope(const LaneSample& point, const TrafficAgentDescription& description,
                    math::Vec3 origin, double padding) noexcept {
    const auto center = point.position - origin +
        point.up * (description.sensor_ground_clearance + description.height * 0.5);
    const auto extent = math::abs(point.right) * (description.width * 0.5 + description.side_margin + padding) +
        math::abs(point.tangent) * (description.length * 0.5 + padding) +
        math::abs(point.up) * (description.height * 0.5);
    return {center - extent, center + extent};
}

math::Aabb combined(math::Aabb first, math::Aabb second) noexcept {
    return {{std::min(first.min.x, second.min.x), std::min(first.min.y, second.min.y),
             std::min(first.min.z, second.min.z)},
            {std::max(first.max.x, second.max.x), std::max(first.max.y, second.max.y),
             std::max(first.max.z, second.max.z)}};
}

std::optional<double> parse_real(std::string_view text) noexcept {
    if (text.empty() || text.size() > 64) { return std::nullopt; }
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::size_t> parse_index(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) { return std::nullopt; }
    std::size_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ?
        std::optional<std::size_t>{value} : std::nullopt;
}

} // namespace

std::string_view describe(TrafficError error) noexcept {
    switch (error) {
    case TrafficError::None: return "ok";
    case TrafficError::InvalidInput: return "invalid traffic parameters";
    case TrafficError::InvalidVehicle: return "missing or invalid physical vehicle";
    case TrafficError::WrongAuthority: return "traffic requires an explicitly attached AI engine vehicle";
    case TrafficError::AlreadyAttached: return "vehicle already has a traffic owner";
    case TrafficError::Capacity: return "traffic capacity exhausted";
    case TrafficError::InvalidRoute: return "route geometry, dimensions or access rejected";
    case TrafficError::StaleRoute: return "route needs replanning after a network edit";
    case TrafficError::RouteLost: return "vehicle cannot be located on its bounded route window";
    case TrafficError::MissingControl: return "junction has no valid enabled traffic control";
    case TrafficError::SensorIncomplete: return "insufficient complete obstacle observations";
    case TrafficError::InconsistentOrigin: return "absolute coordinate conversion unavailable";
    case TrafficError::InputRejected: return "vehicle rejected traffic input";
    case TrafficError::Closed: return "traffic simulation is shut down";
    case TrafficError::Busy: return "traffic or registry mutation is busy";
    }
    return "unknown traffic error";
}

bool valid_traffic_agent(const TrafficAgentDescription& value) noexcept {
    return between(value.length, 0.5, 40.0) && between(value.width, 0.3, 8.0) &&
        between(value.height, 0.3, 8.0) && between(value.sensor_ground_clearance, 0.01, 2.0) &&
        between(value.maximum_speed, 0.1, 80.0) && between(value.comfortable_acceleration, 0.1, 15.0) &&
        between(value.comfortable_braking, 0.1, 20.0) &&
        between(value.emergency_braking, value.comfortable_braking, 40.0) &&
        between(value.lateral_acceleration, 0.1, 20.0) && between(value.time_headway, 0.2, 10.0) &&
        between(value.minimum_gap, 0.1, 30.0) && between(value.side_margin, 0.0, 3.0) &&
        between(value.lookahead_minimum, 0.5, 30.0) && between(value.lookahead_seconds, 0.0, 5.0) &&
        between(value.lookahead_maximum, value.lookahead_minimum, 100.0) &&
        between(value.planning_distance, value.lookahead_maximum, 1000.0) &&
        between(value.planning_spacing, 0.25, 10.0) && between(value.projection_radius, 0.5, 50.0) &&
        between(value.projection_backtrack, 0.0, 20.0) && between(value.projection_forward, 1.0, 500.0) &&
        between(value.arrival_tolerance, 0.05, 3.0) && between(value.stopped_speed, 0.01, 1.0) &&
        between(value.stalled_seconds, 1.0, 600.0) && between(value.recovery_interval, 0.1, 60.0) &&
        value.access_mask != 0 && value.obstacle_filter.layers != 0 && value.obstacle_filter.mask != 0;
}

struct TrafficSimulation::Impl {
    struct Agent {
        TrafficAgentState state;
        TrafficAgentDescription description;
        RoadRoute route;
        physics::BodyHandle body{};
        math::Vec3 forward{0.0, 0.0, -1.0};
        math::Vec3 right{1.0, 0.0, 0.0};
        TrafficMovement movement{};
        JunctionHandle requested_junction{};
        double stop_distance{};
        double connector_exit{};
        double stopped_seconds{};
        double recovery_elapsed{};
        double planned_speed{};
        double steering{};
        double obstacle_distance{};
        double signed_speed{};
        double wheelbase{};
        double maximum_steering{};
        std::uint32_t priority{};
        bool paused{};
        bool replan{};
        bool explicit_replan{};
        bool submitted_input{};
        bool ready{};
        bool request_ready{};
    };
    struct Control {
        JunctionControl description;
        SignalSnapshot state;
        std::uint64_t checked_revision{};
    };
    struct Reservation {
        IntersectionReservation state;
        physics::BodyHandle body{};
        double release_margin{};
        double lease{};
    };
    RoadNetwork& roads;
    physics::VehicleSystem& vehicles;
    physics::PhysicsWorld& physics;
    TrafficLimits limits;
    TrafficStatistics statistics;
    std::map<physics::VehicleHandle, Agent> agents;
    std::map<JunctionHandle, Control> controls;
    std::map<JunctionHandle, Reservation> reservations;
    std::vector<LaneOccupancy> occupancy;
    std::vector<physics::ColliderHandle> candidates;
    math::Vec3 origin{};
    std::uint64_t last_tick{};
    std::size_t rotation{};
    bool has_tick{};
    bool closed{};
    bool busy{};

    Impl(RoadNetwork& network, physics::VehicleSystem& fleet, physics::PhysicsWorld& world,
         TrafficLimits configuration) : roads(network), vehicles(fleet), physics(world), limits(configuration) {
        if (!valid_limits(limits)) { throw std::invalid_argument("invalid traffic limits"); }
        candidates.resize(limits.sensor_candidates);
        occupancy.reserve(limits.agents);
    }

    TrafficError editable() const noexcept {
        if (closed) { return TrafficError::Closed; }
        return busy || physics.registry().iterating() || physics.stepping() ? TrafficError::Busy : TrafficError::None;
    }

    Reservation* held(physics::VehicleHandle vehicle) noexcept {
        for (auto& entry : reservations) {
            if (entry.second.state.vehicle == vehicle) { return &entry.second; }
        }
        return nullptr;
    }

    bool route_valid(const RoadRoute& path, const TrafficAgentDescription& description) const {
        if (path.legs.empty() || path.legs.size() > limits.route_legs || !roads.route_current(path)) { return false; }
        for (std::size_t index = 0; index < path.legs.size(); ++index) {
            const auto& leg = path.legs[index];
            const auto lane = roads.lane(leg.lane);
            if (!lane || !lane->specification.enabled ||
                (lane->specification.access_mask & description.access_mask) == 0 ||
                lane->specification.width < description.width + 2.0 * description.side_margin ||
                leg.lane != path.lanes[index / 2]) { return false; }
            if (index % 2 == 0) {
                if (leg.connector_to || (index != 0 && leg.begin_distance != 0.0) ||
                    (index + 1 != path.legs.size() && leg.end_distance != lane->length)) { return false; }
            } else {
                if (leg.connector_to != path.lanes[index / 2 + 1]) { return false; }
                const auto edge = roads.connection(leg.lane, leg.connector_to);
                if (!edge || !edge->specification.enabled || leg.begin_distance != 0.0 ||
                    leg.end_distance != edge->length ||
                    (edge->specification.access_mask & description.access_mask) == 0) { return false; }
            }
        }
        return true;
    }

    bool control_valid(const JunctionControl& control) const {
        if (!roads.junction(control.junction) || !valid_rule(control.rule) ||
            !between(control.stop_offset, 0.1, 20.0) || !between(control.stopped_seconds, 0.1, 30.0) ||
            !between(control.clearance_seconds, 0.1, 30.0) || !between(control.admission_seconds, 0.5, 120.0) ||
            control.phases.size() > limits.phases_per_junction) { return false; }
        if ((control.rule == JunctionRule::Signals) != !control.phases.empty()) { return false; }
        for (const auto& phase : control.phases) {
            if (phase.movements.empty() || phase.movements.size() > limits.movements_per_phase ||
                !between(phase.green_seconds, 1.0, 3600.0) || !between(phase.amber_seconds, 0.1, 60.0) ||
                !between(phase.all_red_seconds, 0.1, 60.0)) { return false; }
            for (std::size_t index = 0; index < phase.movements.size(); ++index) {
                const auto movement = phase.movements[index];
                const auto connection = roads.connection(movement.from, movement.to);
                if (!connection || !connection->specification.enabled ||
                    connection->specification.junction != control.junction) { return false; }
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (phase.movements[previous] == movement) { return false; }
                }
            }
        }
        return true;
    }

    SignalAspect aspect(JunctionHandle junction, TrafficMovement movement) const noexcept {
        const auto found = controls.find(junction);
        if (found == controls.end()) { return SignalAspect::Red; }
        const auto& control = found->second;
        if (!control.state.enabled || control.state.faulted || control.state.clearance_remaining > 0.0) {
            return SignalAspect::Red;
        }
        if (control.description.rule == JunctionRule::Priority) { return SignalAspect::Green; }
        if (control.description.rule == JunctionRule::Yield) { return SignalAspect::Yield; }
        if (control.description.rule != JunctionRule::Signals ||
            control.state.phase >= control.description.phases.size()) { return SignalAspect::Red; }
        const auto& phase = control.description.phases[control.state.phase];
        if (std::find(phase.movements.begin(), phase.movements.end(), movement) == phase.movements.end()) {
            return SignalAspect::Red;
        }
        if (control.state.phase_seconds < phase.green_seconds) { return SignalAspect::Green; }
        return control.state.phase_seconds < phase.green_seconds + phase.amber_seconds ?
            SignalAspect::Amber : SignalAspect::Red;
    }

    void advance_controls(double delta) {
        for (auto& entry : controls) {
            auto& control = entry.second;
            const auto junction = roads.junction(entry.first);
            if (control.checked_revision != roads.revision()) {
                control.state.faulted = !control_valid(control.description);
                control.checked_revision = roads.revision();
            }
            control.state.enabled = control.description.enabled && junction && junction->enabled;
            control.state.clearance_remaining = std::max(0.0, control.state.clearance_remaining - delta);
            if (!control.state.enabled || control.state.faulted || control.description.rule != JunctionRule::Signals) { continue; }
            control.state.phase_seconds += delta;
            const auto& phase = control.description.phases[control.state.phase];
            const double duration = phase.green_seconds + phase.amber_seconds + phase.all_red_seconds;
            if (control.state.phase_seconds >= duration) {
                control.state.phase_seconds -= duration;
                control.state.phase = (control.state.phase + 1) % control.description.phases.size();
            }
        }
    }

    bool still_owned(Agent& agent) {
        if (!agent.state.control_owned) { return false; }
        const auto vehicle = vehicles.state(agent.state.vehicle);
        const auto* component = physics.registry().get<TrafficAgentComponent>(agent.state.entity);
        const bool replaced_input = vehicle && agent.submitted_input && !vehicle->input_expired &&
            (vehicle->input.throttle != agent.state.input.throttle || vehicle->input.steering != agent.state.input.steering ||
             vehicle->input.brake != agent.state.input.brake || vehicle->input.handbrake != agent.state.input.handbrake);
        if (!vehicle || vehicle->entity != agent.state.entity || !component || component->vehicle != agent.state.vehicle ||
            vehicle->authority != physics::VehicleAuthority::AI ||
            vehicle->drive_mode != physics::VehicleDriveMode::SimpleEngine || replaced_input) {
            agent.state.control_owned = false;
            agent.state.status = TrafficStatus::Relinquished;
            agent.state.error = TrafficError::WrongAuthority;
            agent.ready = false;
            return false;
        }
        return true;
    }

    void submit(Agent& agent, physics::DriveInput input) {
        if (!still_owned(agent)) { return; }
        if (!vehicles.set_input(agent.state.vehicle, input)) {
            agent.state.error = TrafficError::InputRejected;
            agent.state.status = TrafficStatus::Relinquished;
            agent.state.control_owned = false;
            ++statistics.input_rejections;
            return;
        }
        agent.state.input = input;
        agent.submitted_input = true;
    }

    void emergency(Agent& agent, TrafficError error) {
        agent.ready = false;
        agent.request_ready = false;
        agent.state.sensor_complete = false;
        agent.state.target_speed = 0.0;
        if (!still_owned(agent)) { return; }
        agent.state.error = error;
        agent.state.status = TrafficStatus::EmergencyStop;
        submit(agent, {0.0, 0.0, 1.0, 0.0});
    }

    void update_reservations(double delta) {
        for (auto iterator = reservations.begin(); iterator != reservations.end();) {
            auto& reservation = iterator->second;
            reservation.state.age_seconds = std::min(1.0e9, reservation.state.age_seconds + delta);
            const auto body = physics.state(reservation.body);
            bool release = false;
            if (body && body->entity == reservation.state.entity && math::finite(body->pose.position)) {
                const double distance = math::distance(body->pose.position + origin, reservation.state.absolute_center);
                if (std::isfinite(distance)) {
                    if (distance <= reservation.state.radius) { reservation.state.entered = true; }
                    const bool clear = distance > reservation.state.radius + reservation.release_margin;
                    release = clear && (reservation.state.entered || reservation.state.age_seconds > reservation.lease);
                } else { reservation.state.quarantined = true; }
            } else { reservation.state.quarantined = true; }
            if (reservation.state.entered && reservation.state.age_seconds > reservation.lease * 4.0) {
                reservation.state.quarantined = true;
            }
            if (release) {
                const auto control = controls.find(iterator->first);
                if (control != controls.end()) {
                    control->second.state.clearance_remaining = control->second.description.clearance_seconds;
                }
                iterator = reservations.erase(iterator);
            } else { ++iterator; }
        }
    }

    bool project(Agent& agent) {
        const auto& description = agent.description;
        const double begin = std::max(0.0, agent.state.route_distance - description.projection_backtrack);
        const double end = std::min(agent.route.length, agent.state.route_distance + description.projection_forward);
        const double intervals_real = std::max(1.0, std::ceil(end - begin));
        if (intervals_real > static_cast<double>(limits.projection_samples - 1)) { return false; }
        const auto intervals = static_cast<std::size_t>(intervals_real);
        auto previous = roads.sample_route(agent.route, begin);
        if (!previous) { return false; }
        double best_squared = description.projection_radius * description.projection_radius;
        std::optional<double> best;
        double previous_distance = begin;
        for (std::size_t index = 1; index <= intervals; ++index) {
            const double distance = begin + (end - begin) * static_cast<double>(index) / static_cast<double>(intervals);
            const auto point = roads.sample_route(agent.route, distance);
            if (!point) { return false; }
            const auto edge = point->geometry.position - previous->geometry.position;
            const double square = math::length_squared(edge);
            const double fraction = square > 1.0e-12 ? std::clamp(
                math::dot(agent.state.absolute_position - previous->geometry.position, edge) / square, 0.0, 1.0) : 0.0;
            const auto nearest = previous->geometry.position + edge * fraction;
            const double error = math::length_squared(agent.state.absolute_position - nearest);
            if (error <= best_squared && math::dot(point->geometry.tangent, agent.forward) > -0.25) {
                best_squared = error;
                best = previous_distance + (distance - previous_distance) * fraction;
            }
            previous = point;
            previous_distance = distance;
        }
        if (!best) { return false; }
        const auto point = roads.sample_route(agent.route, *best);
        if (!point) { return false; }
        agent.state.route_distance = *best;
        agent.state.remaining_distance = agent.route.length - *best;
        agent.state.lane = point->lane;
        agent.state.connector_to = point->connector_to;
        agent.state.lateral_error = math::dot(agent.state.absolute_position - point->geometry.position, point->geometry.right);
        const auto lane = roads.lane(point->lane);
        if (!lane || std::abs(agent.state.lateral_error) + description.width * 0.5 > lane->specification.width * 0.5) {
            return false;
        }
        occupancy.push_back({agent.state.vehicle, agent.state.entity, point->lane, point->connector_to,
            agent.state.absolute_position, point->geometry.distance, agent.state.speed, description.length, description.width});
        return true;
    }

    bool recover(Agent& agent) {
        if (held(agent.state.vehicle) || statistics.route_queries >= limits.route_queries_per_frame) { return false; }
        ++statistics.route_queries;
        const auto projection = roads.nearest(agent.state.absolute_position, agent.description.projection_radius,
                                              agent.description.access_mask, limits.route_work);
        if (!projection) { return false; }
        const auto goal = roads.lane(agent.state.destination.lane);
        if (!goal || agent.state.destination.distance > goal->length) { return false; }
        RouteOptions options;
        options.access_mask = agent.description.access_mask;
        options.max_lanes = (limits.route_legs + 1) / 2;
        options.max_work = limits.route_work;
        auto replacement = roads.route(projection->lane, projection->sample.distance,
                                      agent.state.destination.lane, agent.state.destination.distance, options);
        if (!route_valid(replacement, agent.description)) { return false; }
        agent.route = std::move(replacement);
        agent.state.route_revision = agent.route.network_revision;
        agent.state.route_distance = 0.0;
        agent.state.remaining_distance = agent.route.length;
        agent.state.stalled_seconds = 0.0;
        agent.stopped_seconds = 0.0;
        agent.replan = false;
        agent.explicit_replan = false;
        ++statistics.recovered;
        return true;
    }

    void prepare(Agent& agent, double delta, std::uint64_t tick) {
        agent.ready = false;
        agent.request_ready = false;
        agent.requested_junction = {};
        agent.state.tick = tick;
        agent.state.sensor_complete = false;
        const auto reservation = held(agent.state.vehicle);
        agent.state.reservation = reservation != nullptr;
        agent.state.junction = reservation ? reservation->state.junction : JunctionHandle{};
        const auto body = physics.state(agent.body);
        const auto vehicle = vehicles.state(agent.state.vehicle);
        if (!body || !vehicle || body->entity != agent.state.entity || vehicle->chassis != agent.body ||
            !math::finite(body->pose.position) || !math::finite(body->pose.rotation) || !math::finite(body->linear_velocity)) {
            emergency(agent, TrafficError::InvalidVehicle);
            return;
        }
        agent.state.absolute_position = body->pose.position + origin;
        agent.forward = math::rotate(body->pose.rotation, {0.0, 0.0, -1.0});
        agent.right = math::rotate(body->pose.rotation, {1.0, 0.0, 0.0});
        agent.signed_speed = math::dot(body->linear_velocity, agent.forward);
        agent.state.speed = std::max(0.0, agent.signed_speed);
        const bool owned = still_owned(agent);
        const bool current = roads.route_current(agent.route);
        agent.recovery_elapsed = std::min(1.0e6, agent.recovery_elapsed + delta);
        if (owned && (agent.replan || !current) && (agent.explicit_replan || agent.description.automatic_recovery) &&
            agent.recovery_elapsed >= agent.description.recovery_interval) {
            agent.recovery_elapsed = 0.0;
            (void)recover(agent);
        }
        if (!roads.route_current(agent.route) || agent.replan) {
            emergency(agent, TrafficError::StaleRoute);
            return;
        }
        if (!project(agent)) {
            agent.replan = true;
            emergency(agent, TrafficError::RouteLost);
            return;
        }
        if (!owned) { return; }
        if (agent.paused || !vehicle->enabled || !body->enabled || !body->scene_enabled) {
            agent.state.status = TrafficStatus::Paused;
            agent.state.target_speed = 0.0;
            submit(agent, {0.0, 0.0, 1.0, 0.0});
            return;
        }
        const auto tuning = vehicles.description(agent.state.vehicle);
        if (!tuning || body->mode != physics::BodyMode::Dynamic || !math::finite(agent.state.absolute_position)) {
            emergency(agent, TrafficError::InvalidVehicle);
            return;
        }
        agent.wheelbase = tuning->tuning.wheelbase;
        agent.maximum_steering = tuning->tuning.maximum_steering_angle;
        if (!between(agent.maximum_steering, 0.001, 1.6) || !between(agent.wheelbase, 0.1, 40.0)) {
            emergency(agent, TrafficError::InvalidVehicle);
            return;
        }
        agent.state.error = TrafficError::None;
        agent.state.status = TrafficStatus::Following;
        agent.ready = true;
    }

    void find_approach(Agent& agent, double delta) {
        const auto& description = agent.description;
        agent.stop_distance = std::numeric_limits<double>::infinity();
        agent.connector_exit = 0.0;
        for (const auto& leg : agent.route.legs) {
            if (!leg.connector_to || leg.route_distance + leg.end_distance - leg.begin_distance +
                description.length * 0.5 < agent.state.route_distance) { continue; }
            const auto connection = roads.connection(leg.lane, leg.connector_to);
            if (!connection || !connection->specification.junction) { continue; }
            const auto junction = connection->specification.junction;
            const auto control = controls.find(junction);
            const TrafficMovement movement{leg.lane, leg.connector_to};
            if (agent.movement != movement) { agent.stopped_seconds = 0.0; }
            agent.movement = movement;
            agent.requested_junction = junction;
            if (!agent.state.reservation) { agent.state.junction = junction; }
            agent.priority = connection->specification.priority;
            const double offset = control != controls.end() ? control->second.description.stop_offset : 1.0;
            agent.stop_distance = leg.route_distance - description.length * 0.5 - offset;
            agent.connector_exit = leg.route_distance + leg.end_distance - leg.begin_distance;
            if (control == controls.end() || !control->second.state.enabled || control->second.state.faulted) {
                agent.state.error = TrafficError::MissingControl;
                return;
            }
            const auto& policy = control->second.description;
            const double gap = agent.stop_distance - agent.state.route_distance;
            if (agent.state.speed <= description.stopped_speed &&
                gap <= std::max(0.75, description.arrival_tolerance + 0.25) && gap >= -description.length - offset) {
                agent.stopped_seconds = std::min(1.0e6, agent.stopped_seconds + delta);
            } else { agent.stopped_seconds = 0.0; }
            const auto light = aspect(junction, movement);
            const bool dwell = policy.rule == JunctionRule::Stop && agent.stopped_seconds >= policy.stopped_seconds;
            const bool yield = policy.rule == JunctionRule::Yield && agent.state.speed <= 3.0;
            const bool go = policy.rule == JunctionRule::Priority ||
                (policy.rule == JunctionRule::Signals && light == SignalAspect::Green);
            agent.request_ready = (dwell || yield || go) && control->second.state.clearance_remaining == 0.0 &&
                gap <= std::max(12.0, agent.state.speed * description.time_headway) &&
                agent.route.length >= agent.connector_exit + description.length * 0.5 + description.minimum_gap;
            return;
        }
        agent.movement = {};
        agent.stopped_seconds = 0.0;
    }

    bool scan(Agent& agent, double horizon) {
        const auto& description = agent.description;
        const double spacing = std::min(description.planning_spacing, description.length * 0.5);
        const double intervals_real = std::max(1.0, std::ceil(horizon / spacing));
        if (intervals_real > static_cast<double>(limits.planning_samples - 1)) { return false; }
        const auto intervals = static_cast<std::size_t>(intervals_real);
        if (intervals > limits.sensor_queries_per_frame - statistics.sensor_queries) { return false; }
        auto previous = roads.sample_route(agent.route, agent.state.route_distance);
        if (!previous) { return false; }
        agent.obstacle_distance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index <= intervals; ++index) {
            const double ahead = horizon * static_cast<double>(index) / static_cast<double>(intervals);
            const auto point = roads.sample_route(agent.route, agent.state.route_distance + ahead);
            if (!point || !std::isfinite(point->geometry.recommended_speed) ||
                !std::isfinite(point->geometry.curvature)) { return false; }
            const double curve = std::abs(point->geometry.curvature);
            const double curve_speed = curve > 1.0e-8 ? std::sqrt(description.lateral_acceleration / curve) : description.maximum_speed;
            const double limit = std::min({description.maximum_speed, point->geometry.recommended_speed, curve_speed});
            if (limit < 0.0) { return false; }
            agent.planned_speed = std::min(agent.planned_speed,
                std::sqrt(limit * limit + 2.0 * description.comfortable_braking * ahead));
            if (index == 0) { previous = point; continue; }
            const double segment_length = horizon / static_cast<double>(intervals);
            const auto bounds = combined(envelope(previous->geometry, description, origin, segment_length * 0.5),
                                         envelope(point->geometry, description, origin, segment_length * 0.5));
            const auto result = physics.collision().query(bounds, candidates, description.obstacle_filter, agent.state.entity);
            ++statistics.sensor_queries;
            if (result.invalid_input || result.truncated) { return false; }
            for (std::size_t candidate = 0; candidate < result.written; ++candidate) {
                const auto* collider = physics.collision().get(candidates[candidate]);
                if (collider == nullptr) { return false; }
                if (collider->description.trigger) { continue; }
                if (collider->bounds && !math::overlaps(bounds, *collider->bounds)) { continue; }
                agent.obstacle_distance = std::min(agent.obstacle_distance, std::max(0.0, ahead - segment_length));
            }
            previous = point;
        }
        agent.state.sensor_complete = true;
        return true;
    }

    void plan(Agent& agent, double delta) {
        if (!agent.ready) { return; }
        const auto& description = agent.description;
        find_approach(agent, delta);
        const double stopping = agent.state.speed * agent.state.speed / (2.0 * description.comfortable_braking);
        double required = std::max(description.lookahead_maximum,
            stopping + agent.state.speed * description.time_headway + description.length + description.minimum_gap);
        if (agent.request_ready) {
            required = std::max(required, agent.connector_exit - agent.state.route_distance +
                description.length * 0.5 + description.minimum_gap);
        }
        required = std::min(required, agent.state.remaining_distance);
        if (required > description.planning_distance) {
            ++statistics.incomplete_sensors;
            emergency(agent, TrafficError::SensorIncomplete);
            return;
        }
        agent.planned_speed = std::min(description.maximum_speed,
            braking_speed(agent.state.remaining_distance - description.arrival_tolerance, description.comfortable_braking));
        if (!scan(agent, required)) {
            ++statistics.incomplete_sensors;
            emergency(agent, TrafficError::SensorIncomplete);
            return;
        }
        agent.planned_speed = std::min(agent.planned_speed,
            braking_speed(agent.obstacle_distance - description.minimum_gap, description.comfortable_braking));
        const auto current = roads.sample_route(agent.route, agent.state.route_distance);
        if (!current) { emergency(agent, TrafficError::StaleRoute); return; }
        for (const auto& other : occupancy) {
            if (other.vehicle == agent.state.vehicle || other.lane != current->lane ||
                other.connector_to != current->connector_to || other.distance < current->geometry.distance) { continue; }
            const double gap = other.distance - current->geometry.distance - (description.length + other.length) * 0.5;
            const double following = other.speed + (gap - description.minimum_gap -
                agent.state.speed * description.time_headway) / description.time_headway;
            agent.planned_speed = std::min(agent.planned_speed, std::max(0.0, following));
        }
        const double lookahead = std::clamp(description.lookahead_minimum + agent.state.speed * description.lookahead_seconds,
                                             description.lookahead_minimum, description.lookahead_maximum);
        const auto target = roads.sample_route(agent.route, agent.state.route_distance + lookahead);
        if (!target) { emergency(agent, TrafficError::StaleRoute); return; }
        const auto relative = target->geometry.position - agent.state.absolute_position;
        const double lateral = math::dot(relative, agent.right);
        const double forward = math::dot(relative, agent.forward);
        const double square = lateral * lateral + forward * forward;
        if (forward < -description.arrival_tolerance && agent.state.remaining_distance > description.arrival_tolerance) {
            agent.replan = true;
            emergency(agent, TrafficError::RouteLost);
            return;
        }
        agent.steering = square > 1.0e-8 ? std::clamp(
            std::atan(agent.wheelbase * 2.0 * lateral / square) / agent.maximum_steering, -1.0, 1.0) : 0.0;
        const double required_exit = agent.connector_exit - agent.state.route_distance + description.length * 0.5 + description.minimum_gap;
        agent.request_ready = agent.request_ready && agent.obstacle_distance > required_exit && !held(agent.state.vehicle);
    }

    void admit(std::vector<Agent*>& requests) {
        std::sort(requests.begin(), requests.end(), [](const Agent* first, const Agent* second) {
            const auto first_age = std::floor(first->state.waiting_seconds / 5.0);
            const auto second_age = std::floor(second->state.waiting_seconds / 5.0);
            if (first_age != second_age) { return first_age > second_age; }
            if (first->priority != second->priority) { return first->priority < second->priority; }
            return first->state.vehicle < second->state.vehicle;
        });
        for (auto* agent : requests) {
            if (!agent->request_ready || reservations.size() >= limits.junctions ||
                reservations.contains(agent->requested_junction) || held(agent->state.vehicle)) { continue; }
            const auto junction = roads.junction(agent->requested_junction);
            const auto control = controls.find(agent->requested_junction);
            if (!junction || !junction->enabled || control == controls.end() || !still_owned(*agent)) { continue; }
            Reservation reservation;
            reservation.state.junction = junction->handle;
            reservation.state.vehicle = agent->state.vehicle;
            reservation.state.entity = agent->state.entity;
            reservation.state.movement = agent->movement;
            reservation.state.absolute_center = junction->absolute_position;
            reservation.state.radius = junction->radius + std::hypot(agent->description.length, agent->description.width) * 0.5;
            reservation.state.entered = math::distance(agent->state.absolute_position, junction->absolute_position) <= reservation.state.radius;
            reservation.body = agent->body;
            reservation.release_margin = agent->description.minimum_gap;
            reservation.lease = control->second.description.admission_seconds;
            reservations.emplace(junction->handle, std::move(reservation));
            agent->state.reservation = true;
            agent->state.junction = junction->handle;
        }
    }

    void drive(Agent& agent, double delta) {
        if (!agent.ready) { return; }
        const auto& description = agent.description;
        double target = agent.planned_speed;
        const auto reservation = held(agent.state.vehicle);
        const bool admitted = reservation && reservation->state.junction == agent.requested_junction;
        if (agent.requested_junction && !admitted) {
            target = std::min(target, braking_speed(agent.stop_distance - agent.state.route_distance -
                description.arrival_tolerance, description.comfortable_braking));
        }
        agent.state.target_speed = target;
        agent.state.waiting_seconds = target < 0.5 ? std::min(1.0e9, agent.state.waiting_seconds + delta) : 0.0;
        agent.state.status = target < 0.5 ? TrafficStatus::Waiting : TrafficStatus::Following;
        const auto goal = roads.sample_route(agent.route, agent.route.length);
        if (goal && agent.state.remaining_distance <= description.arrival_tolerance && agent.state.speed <= description.stopped_speed) {
            const auto offset = agent.state.absolute_position - goal->geometry.position;
            const auto surface_offset = offset - goal->geometry.up * math::dot(offset, goal->geometry.up);
            if (math::length(surface_offset) <= description.arrival_tolerance) {
                agent.state.status = TrafficStatus::Arrived;
                agent.state.target_speed = 0.0;
                submit(agent, {0.0, 0.0, 1.0, 1.0});
                return;
            }
            agent.replan = true;
            emergency(agent, TrafficError::RouteLost);
            return;
        }
        agent.state.stalled_seconds = target > 0.5 && agent.state.speed < description.stopped_speed ?
            std::min(1.0e6, agent.state.stalled_seconds + delta) : 0.0;
        if (agent.state.stalled_seconds >= description.stalled_seconds) {
            agent.replan = true;
            agent.state.status = TrafficStatus::Recovering;
            agent.state.target_speed = 0.0;
            submit(agent, {0.0, agent.steering, 1.0, 0.0});
            return;
        }
        const double acceleration = (target - agent.signed_speed) / std::max(0.25, description.time_headway * 0.5);
        physics::DriveInput input;
        input.steering = agent.steering;
        input.throttle = std::clamp(acceleration / description.comfortable_acceleration, 0.0, 1.0);
        input.brake = std::clamp(-acceleration / description.comfortable_braking, 0.0, 1.0);
        const double hard_stop = agent.state.speed * agent.state.speed / (2.0 * description.emergency_braking);
        if (agent.signed_speed < -description.stopped_speed || agent.obstacle_distance <= hard_stop + description.minimum_gap ||
            (target < description.stopped_speed && agent.state.speed < description.stopped_speed)) {
            input.throttle = 0.0;
            input.brake = 1.0;
        }
        submit(agent, input);
    }

    void summarize() {
        statistics.agents = agents.size();
        statistics.reservations = reservations.size();
        for (const auto& entry : reservations) { if (entry.second.state.quarantined) { ++statistics.quarantined; } }
        for (const auto& entry : agents) {
            const auto& state = entry.second.state;
            if (state.control_owned) { ++statistics.controlled; }
            switch (state.status) {
            case TrafficStatus::Waiting: ++statistics.waiting; break;
            case TrafficStatus::Arrived: ++statistics.arrived; break;
            case TrafficStatus::Relinquished: ++statistics.relinquished; break;
            case TrafficStatus::EmergencyStop: ++statistics.emergency_stops; break;
            default: break;
            }
        }
    }
};

TrafficSimulation::TrafficSimulation(RoadNetwork& roads, physics::VehicleSystem& vehicles,
    physics::PhysicsWorld& physics, TrafficLimits limits) : impl_(std::make_unique<Impl>(roads, vehicles, physics, limits)) {
    (void)physics.registry().register_component<TrafficAgentComponent>("traffic.agent");
}
TrafficSimulation::~TrafficSimulation() = default;

TrafficError TrafficSimulation::attach(physics::VehicleHandle handle, const RoadRoute& path, TrafficAgentDescription description) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    if (!valid_traffic_agent(description)) { return TrafficError::InvalidInput; }
    const auto vehicle = impl_->vehicles.state(handle);
    if (!vehicle) { return TrafficError::InvalidVehicle; }
    const auto body = impl_->physics.state(vehicle->chassis);
    if (!body || body->entity != vehicle->entity || body->mode != physics::BodyMode::Dynamic) { return TrafficError::InvalidVehicle; }
    if (vehicle->authority != physics::VehicleAuthority::AI || vehicle->drive_mode != physics::VehicleDriveMode::SimpleEngine) {
        return TrafficError::WrongAuthority;
    }
    if (impl_->agents.contains(handle) || impl_->physics.registry().has<TrafficAgentComponent>(vehicle->entity)) {
        return TrafficError::AlreadyAttached;
    }
    if (impl_->agents.size() >= impl_->limits.agents) { return TrafficError::Capacity; }
    if (!impl_->route_valid(path, description)) { return TrafficError::InvalidRoute; }
    Impl::Agent agent;
    agent.state.vehicle = handle;
    agent.state.entity = vehicle->entity;
    agent.state.control_owned = true;
    agent.state.route_revision = path.network_revision;
    agent.state.destination = {path.lanes.back(), path.legs.back().end_distance};
    agent.state.remaining_distance = path.length;
    agent.description = description;
    agent.route = path;
    agent.body = vehicle->chassis;
    agent.recovery_elapsed = description.recovery_interval;
    impl_->agents.emplace(handle, std::move(agent));
    try { (void)impl_->physics.registry().emplace<TrafficAgentComponent>(vehicle->entity, TrafficAgentComponent{handle}); }
    catch (...) { impl_->agents.erase(handle); throw; }
    return TrafficError::None;
}

TrafficError TrafficSimulation::detach(physics::VehicleHandle handle) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    const auto found = impl_->agents.find(handle);
    if (found == impl_->agents.end()) { return TrafficError::InvalidVehicle; }
    impl_->submit(found->second, {0.0, 0.0, 1.0, 0.0});
    const auto entity = found->second.state.entity;
    const auto* component = impl_->physics.registry().get<TrafficAgentComponent>(entity);
    if (component && component->vehicle == handle) { (void)impl_->physics.registry().remove<TrafficAgentComponent>(entity); }
    impl_->agents.erase(found);
    return TrafficError::None;
}

TrafficError TrafficSimulation::set_route(physics::VehicleHandle handle, const RoadRoute& path) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    const auto found = impl_->agents.find(handle);
    if (found == impl_->agents.end()) { return TrafficError::InvalidVehicle; }
    auto& agent = found->second;
    if (!impl_->still_owned(agent)) { return TrafficError::WrongAuthority; }
    if (!impl_->route_valid(path, agent.description)) { return TrafficError::InvalidRoute; }
    auto replacement = path;
    agent.route = std::move(replacement);
    agent.state.destination = {path.lanes.back(), path.legs.back().end_distance};
    agent.state.route_revision = path.network_revision;
    agent.state.route_distance = 0.0;
    agent.state.remaining_distance = path.length;
    agent.state.stalled_seconds = 0.0;
    agent.state.waiting_seconds = 0.0;
    agent.stopped_seconds = 0.0;
    agent.replan = false;
    agent.explicit_replan = false;
    return TrafficError::None;
}

TrafficError TrafficSimulation::set_destination(physics::VehicleHandle handle, TrafficDestination destination) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    const auto found = impl_->agents.find(handle);
    if (found == impl_->agents.end()) { return TrafficError::InvalidVehicle; }
    if (!impl_->still_owned(found->second)) { return TrafficError::WrongAuthority; }
    const auto lane = impl_->roads.lane(destination.lane);
    if (!lane || !between(destination.distance, 0.0, lane->length)) { return TrafficError::InvalidInput; }
    found->second.state.destination = destination;
    found->second.replan = true;
    found->second.explicit_replan = true;
    found->second.recovery_elapsed = found->second.description.recovery_interval;
    return TrafficError::None;
}

TrafficError TrafficSimulation::set_paused(physics::VehicleHandle handle, bool paused) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    const auto found = impl_->agents.find(handle);
    if (found == impl_->agents.end()) { return TrafficError::InvalidVehicle; }
    if (!impl_->still_owned(found->second)) { return TrafficError::WrongAuthority; }
    found->second.paused = paused;
    if (paused) { impl_->submit(found->second, {0.0, 0.0, 1.0, 0.0}); }
    return TrafficError::None;
}

TrafficError TrafficSimulation::retry(physics::VehicleHandle handle) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    const auto found = impl_->agents.find(handle);
    if (found == impl_->agents.end()) { return TrafficError::InvalidVehicle; }
    if (!impl_->still_owned(found->second)) { return TrafficError::WrongAuthority; }
    found->second.replan = true;
    found->second.explicit_replan = true;
    found->second.recovery_elapsed = found->second.description.recovery_interval;
    return TrafficError::None;
}

std::optional<TrafficAgentState> TrafficSimulation::state(physics::VehicleHandle handle) const noexcept {
    const auto found = impl_->agents.find(handle);
    return found != impl_->agents.end() ? std::optional<TrafficAgentState>{found->second.state} : std::nullopt;
}
std::optional<RoadRoute> TrafficSimulation::route(physics::VehicleHandle handle) const {
    const auto found = impl_->agents.find(handle);
    return found != impl_->agents.end() ? std::optional<RoadRoute>{found->second.route} : std::nullopt;
}
std::vector<physics::VehicleHandle> TrafficSimulation::agents() const {
    std::vector<physics::VehicleHandle> result;
    result.reserve(impl_->agents.size());
    for (const auto& entry : impl_->agents) { result.push_back(entry.first); }
    return result;
}
std::span<const LaneOccupancy> TrafficSimulation::occupancy() const noexcept { return impl_->occupancy; }
std::vector<IntersectionReservation> TrafficSimulation::reservations() const {
    std::vector<IntersectionReservation> result;
    result.reserve(impl_->reservations.size());
    for (const auto& entry : impl_->reservations) { result.push_back(entry.second.state); }
    return result;
}

TrafficError TrafficSimulation::configure_junction(JunctionControl description) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    if (!impl_->control_valid(description)) { return TrafficError::InvalidInput; }
    if (!impl_->controls.contains(description.junction) && impl_->controls.size() >= impl_->limits.junctions) {
        return TrafficError::Capacity;
    }
    Impl::Control control;
    control.state.junction = description.junction;
    control.state.rule = description.rule;
    control.state.enabled = description.enabled;
    control.state.clearance_remaining = description.clearance_seconds;
    control.checked_revision = impl_->roads.revision();
    control.description = std::move(description);
    impl_->controls.insert_or_assign(control.state.junction, std::move(control));
    return TrafficError::None;
}

TrafficError TrafficSimulation::remove_junction_control(JunctionHandle junction) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    return impl_->controls.erase(junction) != 0 ? TrafficError::None : TrafficError::MissingControl;
}
TrafficError TrafficSimulation::set_junction_enabled(JunctionHandle junction, bool enabled) {
    if (const auto error = impl_->editable(); error != TrafficError::None) { return error; }
    const auto found = impl_->controls.find(junction);
    if (found == impl_->controls.end()) { return TrafficError::MissingControl; }
    found->second.description.enabled = enabled;
    found->second.state.enabled = enabled;
    if (enabled) { found->second.state.clearance_remaining = found->second.description.clearance_seconds; }
    return TrafficError::None;
}
std::optional<SignalSnapshot> TrafficSimulation::signal(JunctionHandle junction) const noexcept {
    const auto found = impl_->controls.find(junction);
    return found != impl_->controls.end() ? std::optional<SignalSnapshot>{found->second.state} : std::nullopt;
}
SignalAspect TrafficSimulation::aspect(JunctionHandle junction, TrafficMovement movement) const noexcept {
    return impl_->aspect(junction, movement);
}
const TrafficLimits& TrafficSimulation::limits() const noexcept { return impl_->limits; }
const TrafficStatistics& TrafficSimulation::statistics() const noexcept { return impl_->statistics; }

void TrafficSimulation::update(double delta, std::uint64_t tick, const scene::WorldPartition* partition) {
    if (impl_->closed || impl_->busy) { return; }
    impl_->statistics = {};
    impl_->statistics.tick = tick;
    if (impl_->has_tick && tick <= impl_->last_tick) {
        impl_->statistics.duplicate_tick = true;
        impl_->summarize();
        return;
    }
    impl_->has_tick = true;
    impl_->last_tick = tick;
    impl_->busy = true;
    struct Guard { bool& busy; ~Guard() { busy = false; } } guard{impl_->busy};
    const auto coordinates = partition ? partition->origin() : scene::OriginState{};
    impl_->statistics.invalid_delta = !between(delta, 0.000001, impl_->limits.maximum_delta);
    impl_->statistics.inconsistent_origin = !coordinates.consistent || !math::finite(coordinates.absolute_origin);
    impl_->occupancy.clear();
    if (impl_->statistics.invalid_delta || impl_->statistics.inconsistent_origin) {
        for (auto& entry : impl_->agents) {
            impl_->emergency(entry.second, impl_->statistics.inconsistent_origin ?
                TrafficError::InconsistentOrigin : TrafficError::InvalidInput);
        }
        impl_->summarize();
        return;
    }
    impl_->origin = coordinates.absolute_origin;
    impl_->advance_controls(delta);
    impl_->update_reservations(delta);
    if (impl_->agents.empty()) { impl_->summarize(); return; }
    const auto synchronized = impl_->physics.collision().sync(impl_->physics.scene());
    if (synchronized.scene_busy || synchronized.rejected != 0) {
        for (auto& entry : impl_->agents) { impl_->emergency(entry.second, TrafficError::SensorIncomplete); }
        impl_->statistics.incomplete_sensors = impl_->agents.size();
        impl_->summarize();
        return;
    }
    std::vector<Impl::Agent*> order;
    order.reserve(impl_->agents.size());
    for (auto& entry : impl_->agents) { order.push_back(&entry.second); }
    if (!order.empty()) {
        impl_->rotation %= order.size();
        std::rotate(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(impl_->rotation), order.end());
        impl_->rotation = (impl_->rotation + 1) % order.size();
    }
    for (auto* agent : order) { impl_->prepare(*agent, delta, tick); }
    std::vector<Impl::Agent*> requests;
    requests.reserve(order.size());
    for (auto* agent : order) {
        impl_->plan(*agent, delta);
        if (agent->ready && agent->request_ready) { requests.push_back(agent); }
    }
    impl_->admit(requests);
    for (auto* agent : order) { impl_->drive(*agent, delta); }
    impl_->summarize();
}

void TrafficSimulation::shutdown() {
    if (impl_->closed || impl_->busy) { return; }
    for (auto& entry : impl_->agents) {
        impl_->submit(entry.second, {0.0, 0.0, 1.0, 0.0});
        const auto entity = entry.second.state.entity;
        const auto* component = impl_->physics.registry().get<TrafficAgentComponent>(entity);
        if (component && component->vehicle == entry.first && !impl_->physics.registry().iterating()) {
            (void)impl_->physics.registry().remove<TrafficAgentComponent>(entity);
        }
    }
    impl_->agents.clear();
    impl_->controls.clear();
    impl_->reservations.clear();
    impl_->occupancy.clear();
    impl_->closed = true;
}

namespace {

physics::VehicleHandle command_vehicle(core::Kernel& host, std::string_view name) {
    auto& registry = host.services().get<physics::PhysicsWorld>().registry();
    auto entity = ecs::parse_entity(name).value_or(ecs::Entity{});
    if (!entity) {
        const auto matches = registry.find_all_named(name);
        if (matches.size() == 1) { entity = matches.front(); }
    }
    return host.services().get<physics::VehicleSystem>().find(entity);
}

std::optional<LaneHandle> command_lane(RoadNetwork& roads, std::string_view name, std::string_view ordinal) {
    const auto index = parse_index(ordinal);
    const auto lanes = roads.lanes(roads.find(name));
    return index && *index < lanes.size() ? std::optional<LaneHandle>{lanes[*index]} : std::nullopt;
}

core::CommandResult command_result(TrafficError error) {
    return {error == TrafficError::None, std::string(describe(error))};
}

} // namespace

void register_simulation(core::Kernel& kernel) {
    if (kernel.services().find<TrafficSimulation>()) { return; }
    auto* roads = kernel.services().find<RoadNetwork>();
    auto* vehicles = kernel.services().find<physics::VehicleSystem>();
    auto* physics = kernel.services().find<physics::PhysicsWorld>();
    if (!roads || !vehicles || !physics) { throw std::logic_error("traffic simulation requires roads, vehicles and physics"); }
    TrafficLimits limits;
    limits.agents = static_cast<std::size_t>(std::clamp<std::int64_t>(kernel.config().integer("traffic.agents", 1024), 0, 16384));
    limits.sensor_queries_per_frame = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("traffic.sensor_queries", 8192), 0, 1048576));
    limits.route_queries_per_frame = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("traffic.route_queries", 8), 0, 1024));
    auto& service = kernel.services().emplace<TrafficSimulation>(*roads, *vehicles, *physics, limits);
    (void)kernel.register_module({"traffic.simulation", "Bounded physical vehicle traffic and exclusive junction admission", 1});
    (void)kernel.add_shutdown("traffic.simulation", [pointer = &service] { pointer->shutdown(); });
    (void)kernel.add_system({"traffic.simulation.control", core::Phase::PreSimulation, -6100, {"physics.collision"}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& simulation = host.services().get<TrafficSimulation>();
            simulation.update(frame.delta_seconds, frame.tick, host.services().find<scene::WorldPartition>());
            const auto& statistics = simulation.statistics();
            host.metric("traffic.agents", static_cast<double>(statistics.agents));
            host.metric("traffic.controlled", static_cast<double>(statistics.controlled));
            host.metric("traffic.waiting", static_cast<double>(statistics.waiting));
            host.metric("traffic.arrived", static_cast<double>(statistics.arrived));
            host.metric("traffic.reservations", static_cast<double>(statistics.reservations));
            host.metric("traffic.quarantined", static_cast<double>(statistics.quarantined));
            host.metric("traffic.incomplete_sensors", static_cast<double>(statistics.incomplete_sensors));
            host.metric("traffic.input_rejections", static_cast<double>(statistics.input_rejections));
        });
    (void)kernel.add_command("traffic.attach", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 7) { return {false, "usage: traffic.attach ENTITY FROM_ROAD FROM_LANE TO_ROAD TO_LANE LENGTH WIDTH"}; }
        auto& network = host.services().get<RoadNetwork>();
        auto& simulation = host.services().get<TrafficSimulation>();
        const auto vehicle = command_vehicle(host, args[0]);
        const auto from = command_lane(network, args[1], args[2]);
        const auto to = command_lane(network, args[3], args[4]);
        const auto length = parse_real(args[5]);
        const auto width = parse_real(args[6]);
        const auto state = host.services().get<physics::VehicleSystem>().state(vehicle);
        if (!from || !to || !length || !width || !state) { return {false, "live vehicle, lane ordinals and finite dimensions required"}; }
        const auto body = host.services().get<physics::PhysicsWorld>().state(state->chassis);
        if (!body) { return command_result(TrafficError::InvalidVehicle); }
        auto position = std::optional<math::Vec3>{body->pose.position};
        if (const auto* partition = host.services().find<scene::WorldPartition>()) { position = partition->to_absolute(body->pose.position); }
        const auto projection = position ? network.project(*from, *position) : std::nullopt;
        const auto destination = network.lane(*to);
        TrafficAgentDescription description;
        description.length = *length;
        description.width = *width;
        if (!valid_traffic_agent(description) || !projection || !destination ||
            projection->squared_distance > description.projection_radius * description.projection_radius) {
            return command_result(TrafficError::InvalidInput);
        }
        RouteOptions options;
        options.max_lanes = (simulation.limits().route_legs + 1) / 2;
        options.max_work = simulation.limits().route_work;
        const auto path = network.route(*from, projection->sample.distance, *to, destination->length, options);
        return command_result(simulation.attach(vehicle, path, description));
    }, "Attach an existing AI vehicle to a physical road route with explicit chassis dimensions");
    (void)kernel.add_command("traffic.agent", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: traffic.agent ENTITY pause|resume|retry|remove"}; }
        auto& simulation = host.services().get<TrafficSimulation>();
        const auto vehicle = command_vehicle(host, args[0]);
        if (args[1] == "pause" || args[1] == "resume") { return command_result(simulation.set_paused(vehicle, args[1] == "pause")); }
        if (args[1] == "retry") { return command_result(simulation.retry(vehicle)); }
        if (args[1] == "remove") { return command_result(simulation.detach(vehicle)); }
        return {false, "unknown traffic operation"};
    }, "Pause, resume, replan or detach traffic without changing vehicle authority");
    (void)kernel.add_command("traffic.destination", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4) { return {false, "usage: traffic.destination ENTITY ROAD LANE DISTANCE"}; }
        const auto lane = command_lane(host.services().get<RoadNetwork>(), args[1], args[2]);
        const auto distance = parse_real(args[3]);
        if (!lane || !distance) { return command_result(TrafficError::InvalidInput); }
        return command_result(host.services().get<TrafficSimulation>().set_destination(command_vehicle(host, args[0]), {*lane, *distance}));
    }, "Queue a destination change for replanning with the next frame's absolute physics pose");
    (void)kernel.add_command("traffic.junction", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 5) { return {false, "usage: traffic.junction FROM_ROAD FROM_LANE TO_ROAD TO_LANE stop|yield|priority|red|green"}; }
        auto& network = host.services().get<RoadNetwork>();
        const auto from = command_lane(network, args[0], args[1]);
        const auto to = command_lane(network, args[2], args[3]);
        const auto connection = from && to ? network.connection(*from, *to) : std::nullopt;
        if (!connection || !connection->specification.junction) { return {false, "connection must belong to an authored junction"}; }
        JunctionControl control;
        control.junction = connection->specification.junction;
        if (args[4] == "yield") { control.rule = JunctionRule::Yield; }
        else if (args[4] == "priority") { control.rule = JunctionRule::Priority; }
        else if (args[4] == "red") { control.enabled = false; }
        else if (args[4] == "green") {
            control.rule = JunctionRule::Signals;
            SignalPhase phase;
            phase.movements.push_back({*from, *to});
            control.phases.push_back(std::move(phase));
        } else if (args[4] != "stop") { return {false, "unknown junction rule"}; }
        return command_result(host.services().get<TrafficSimulation>().configure_junction(std::move(control)));
    }, "Set junction admission or a repeating single-movement signal; author multi-phase signals through the service API");
    (void)kernel.add_command("traffic.status", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: traffic.status"}; }
        const auto& statistics = host.services().get<TrafficSimulation>().statistics();
        return {true, "agents=" + std::to_string(statistics.agents) + " controlled=" + std::to_string(statistics.controlled) +
            " waiting=" + std::to_string(statistics.waiting) + " arrived=" + std::to_string(statistics.arrived) +
            " reservations=" + std::to_string(statistics.reservations) + " quarantined=" + std::to_string(statistics.quarantined) +
            " incomplete_sensors=" + std::to_string(statistics.incomplete_sensors)};
    }, "Inspect the most recent traffic control frame");
}

} // namespace ge::traffic
