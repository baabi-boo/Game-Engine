#include <ge/physics/constraints.hpp>
#include <ge/core/runtime.hpp>
#include <ge/scene/partition.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ge::physics {
namespace {

constexpr std::string_view solver_name = "physics.constraints";
constexpr std::array<math::Vec3, 3> basis{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

bool bounded(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool bounded(math::Vec3 value, double maximum) noexcept {
    return math::finite(value) && std::abs(value.x) <= maximum &&
        std::abs(value.y) <= maximum && std::abs(value.z) <= maximum;
}

bool world_endpoint(BodyHandle body) noexcept {
    return body.index == 0 && body.generation == 0;
}

bool usable(const BodyState* body) noexcept {
    return !body || (body->enabled && body->scene_enabled);
}

bool dynamic(const BodyState* body) noexcept {
    return body && usable(body) && body->mode == BodyMode::Dynamic;
}

bool moving(const BodyState* body) noexcept {
    if (!body || !usable(body) || body->mode == BodyMode::Static) {
        return false;
    }
    return (body->mode == BodyMode::Dynamic && body->awake) ||
        math::length_squared(body->linear_velocity) > 1.0e-12 ||
        math::length_squared(body->angular_velocity) > 1.0e-12;
}

bool valid_frame(const JointFrame& frame, double maximum) noexcept {
    return bounded(frame.anchor, maximum) && math::finite(frame.orientation) &&
        bounded(math::dot(frame.orientation, frame.orientation), 1.0e-12, 1.0e12);
}

bool valid_limit(const JointLimit& limit, double low, double high) noexcept {
    return bounded(limit.lower, low, high) && bounded(limit.upper, low, high) &&
        limit.lower <= limit.upper;
}

math::Vec3 rotation_vector(math::Quat rotation) noexcept {
    rotation = math::normalized(rotation);
    if (rotation.w < 0.0) {
        rotation = -rotation;
    }
    const math::Vec3 vector{rotation.x, rotation.y, rotation.z};
    const double sine = math::length(vector);
    if (sine < 1.0e-9) {
        return vector * 2.0;
    }
    return vector * (2.0 * std::atan2(sine, math::clamp(rotation.w, 0.0, 1.0)) / sine);
}

double dead_zone(double error, double slop) noexcept {
    return error > slop ? error - slop : (error < -slop ? error + slop : 0.0);
}

double limit_error(double coordinate, const JointLimit& limit) noexcept {
    if (!limit.enabled) {
        return 0.0;
    }
    return coordinate - math::clamp(coordinate, limit.lower, limit.upper);
}

struct WorldFrame {
    math::Vec3 anchor{};
    math::Vec3 offset{};
    math::Quat orientation{};
    math::Vec3 axis{1.0, 0.0, 0.0};
    math::Vec3 up{0.0, 1.0, 0.0};
    math::Vec3 side{0.0, 0.0, 1.0};
};

WorldFrame frame_in_world(const BodyState* body, const JointFrame& frame) noexcept {
    WorldFrame result;
    result.anchor = body ? math::transform_point(body->pose, frame.anchor) : frame.anchor;
    result.orientation = math::normalized(body ? body->pose.rotation * frame.orientation : frame.orientation);
    result.offset = body ? result.anchor - center_of_mass(*body) : math::Vec3{};
    result.axis = math::rotate(result.orientation, basis[0]);
    result.up = math::rotate(result.orientation, basis[1]);
    result.side = math::rotate(result.orientation, basis[2]);
    return result;
}

std::optional<JointFrame> frame_in_body(const BodyState* body, math::Vec3 anchor,
                                      math::Quat orientation) noexcept {
    if (!body) {
        return JointFrame{anchor, orientation};
    }
    const auto local = math::inverse_transform_point(body->pose, anchor);
    if (!local) {
        return std::nullopt;
    }
    return JointFrame{*local, math::normalized(math::conjugate(body->pose.rotation) * orientation)};
}

double twist_coordinate(const WorldFrame& first, const WorldFrame& second) noexcept {
    const auto projected = math::normalized(math::reject(second.up, first.axis), first.up);
    return std::atan2(math::dot(first.axis, math::cross(first.up, projected)),
                      math::clamp(math::dot(first.up, projected), -1.0, 1.0));
}

math::Vec3 swing_error(const WorldFrame& first, const WorldFrame& second) noexcept {
    const double cosine = math::clamp(math::dot(first.axis, second.axis), -1.0, 1.0);
    const auto cross = math::cross(first.axis, second.axis);
    const double sine = math::length(cross);
    if (sine < 1.0e-9) {
        return cosine < 0.0 ? first.up * math::pi : cross;
    }
    return cross * (std::atan2(sine, cosine) / sine);
}

} // namespace

std::string_view joint_kind_name(JointKind kind) noexcept {
    switch (kind) {
    case JointKind::Distance: return "distance";
    case JointKind::Spring: return "spring";
    case JointKind::BallSocket: return "ball";
    case JointKind::Hinge: return "hinge";
    case JointKind::Fixed: return "fixed";
    case JointKind::Slider: return "slider";
    }
    return "invalid";
}

std::string_view joint_status_name(JointStatus status) noexcept {
    switch (status) {
    case JointStatus::Ready: return "ready";
    case JointStatus::Disabled: return "disabled";
    case JointStatus::Sleeping: return "sleeping";
    case JointStatus::InactiveBody: return "inactive-body";
    case JointStatus::Broken: return "broken";
    case JointStatus::PendingRemoval: return "pending-removal";
    case JointStatus::InvalidGeometry: return "invalid-geometry";
    case JointStatus::OriginInconsistent: return "origin-inconsistent";
    }
    return "invalid";
}

bool valid_joint_description(const JointDescription& value, double maximum) noexcept {
    if (!bounded(maximum, 1.0, 1.0e12) || joint_kind_name(value.kind) == "invalid" ||
        (!value.first.valid() && !world_endpoint(value.first)) ||
        (!value.second.valid() && !world_endpoint(value.second)) || value.first == value.second ||
        !valid_frame(value.first_frame, maximum) || !valid_frame(value.second_frame, maximum)) {
        return false;
    }
    if (!bounded(value.rest_length, 0.0, maximum) ||
        !bounded(value.spring_stiffness, 0.0, 1.0e9) ||
        !bounded(value.spring_damping, 0.0, 1.0e9) ||
        (value.kind == JointKind::Spring && value.spring_stiffness + value.spring_damping < 1.0e-9)) {
        return false;
    }
    const bool distance = value.kind == JointKind::Distance || value.kind == JointKind::Spring;
    const bool slider = value.kind == JointKind::Slider;
    const bool hinge = value.kind == JointKind::Hinge;
    if (!valid_limit(value.linear_limit, distance ? 0.0 : -maximum, maximum) ||
        !valid_limit(value.angular_limit, -math::pi, math::pi) ||
        (value.linear_limit.enabled && !distance && !slider) ||
        (value.angular_limit.enabled && !hinge)) {
        return false;
    }
    const auto& motor = value.motor;
    if (motor.mode != JointMotorMode::Disabled && motor.mode != JointMotorMode::Velocity &&
        motor.mode != JointMotorMode::Position) {
        return false;
    }
    if ((motor.mode != JointMotorMode::Disabled && !slider && !hinge) ||
        !bounded(motor.velocity, -1.0e6, 1.0e6) || !bounded(motor.target, -maximum, maximum) ||
        !bounded(motor.gain, 0.0, 1.0e6) || !bounded(motor.maximum_speed, 0.0, 1.0e6) ||
        !bounded(motor.maximum_force, 0.0, 1.0e12) ||
        (hinge && motor.mode == JointMotorMode::Position && !bounded(motor.target, -math::pi, math::pi))) {
        return false;
    }
    const auto& tune = value.tuning;
    return bounded(tune.velocity_bias, 0.0, 1.0) && bounded(tune.position_fraction, 0.0, 1.0) &&
        bounded(tune.linear_slop, 0.0, 1.0) && bounded(tune.angular_slop, 0.0, math::pi) &&
        bounded(tune.maximum_bias_speed, 0.0, 1.0e6) &&
        bounded(tune.maximum_position_delta, 0.0, 1.0e3) &&
        bounded(tune.maximum_angle_delta, 0.0, 0.5) &&
        bounded(tune.linear_compliance, 0.0, 1.0e9) &&
        bounded(tune.angular_compliance, 0.0, 1.0e9) &&
        bounded(tune.maximum_impulse, 0.0, 1.0e9) &&
        bounded(tune.break_force, 0.0, 1.0e18) && bounded(tune.break_torque, 0.0, 1.0e24);
}

struct JointService::Impl {
    struct Row {
        math::Vec3 linear{};
        math::Vec3 first_angular{};
        math::Vec3 second_angular{};
        double error{};
        double weight{};
        double mass{};
        double gamma{};
        double bias{};
        double impulse{};
        double lower{};
        double upper{};
        std::uint32_t key{};
        bool angular{};
        bool project{true};
    };
    struct Record {
        JointState state;
        WorldFrame first;
        WorldFrame second;
        std::array<Row, 8> rows{};
        std::size_t row_count{};
        double previous_delta{};
        JointEventKind removal_kind{JointEventKind::Removed};
        bool pending{};
        bool broken{};
        bool eligible{};
        bool active{};
    };
    struct Slot {
        std::uint64_t generation{1};
        std::optional<Record> record;
    };
    struct Endpoint {
        BodyHandle body;
        std::uint32_t joint{};
    };
    PhysicsWorld* world{};
    JointLimits limits;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_slots;
    std::vector<std::uint32_t> parents;
    std::vector<std::uint8_t> seeds;
    std::vector<Endpoint> endpoints;
    std::vector<JointEvent> events;
    JointStatistics counters;
    scene::OriginState origin;
    bool origin_bound{};
    bool connected{};

    Impl(PhysicsWorld& borrowed, JointLimits configured) : world(&borrowed), limits(configured) {
        if (limits.capacity > 65536 || limits.event_capacity > 1048576 ||
            !bounded(limits.coordinate_bound, 1.0, 1.0e12) ||
            !bounded(limits.minimum_delta, 1.0e-9, 0.01)) {
            throw std::invalid_argument("joint service limits exceed supported bounds");
        }
        const auto count = static_cast<std::size_t>(limits.capacity) + 1;
        slots.resize(count);
        parents.resize(count);
        seeds.resize(count);
        free_slots.reserve(limits.capacity);
        endpoints.reserve(static_cast<std::size_t>(limits.capacity) * 2);
        events.reserve(limits.event_capacity);
        for (std::uint32_t index = limits.capacity; index != 0; --index) {
            free_slots.push_back(index);
        }
    }

    Record* get(JointHandle handle) noexcept {
        if (!handle || handle.index >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[handle.index];
        return slot.generation == handle.generation && slot.record ? &*slot.record : nullptr;
    }

    const Record* get(JointHandle handle) const noexcept {
        if (!handle || handle.index >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[handle.index];
        return slot.generation == handle.generation && slot.record ? &*slot.record : nullptr;
    }

    bool mutable_now() const noexcept {
        return connected && world && !world->stepping();
    }

    bool bodies_valid(const JointDescription& description) const noexcept {
        if (!world) {
            return false;
        }
        const auto* first = world->get(description.first);
        const auto* second = world->get(description.second);
        return (!description.first || first) && (!description.second || second) &&
            ((first && first->mode == BodyMode::Dynamic) || (second && second->mode == BodyMode::Dynamic));
    }

    bool reject() noexcept {
        increment(counters.rejected_operations);
        return false;
    }

    void emit(JointEventKind kind, const Record& record) {
        if (events.size() >= limits.event_capacity) {
            increment(counters.dropped_events);
            return;
        }
        const auto& state = record.state;
        events.push_back({kind, state.handle, state.description.first, state.description.second,
            counters.substeps, state.force, state.torque});
    }

    void reset_cache(Record& record) noexcept {
        record.row_count = 0;
        record.previous_delta = 0.0;
        record.state.solver_rows = 0;
        record.state.linear_impulse = {};
        record.state.first_angular_impulse = {};
        record.state.second_angular_impulse = {};
        record.state.force = 0.0;
        record.state.torque = 0.0;
        record.active = false;
    }

    void mark_removed(Record& record, JointEventKind kind) noexcept {
        if (!record.pending) {
            record.pending = true;
            record.removal_kind = kind;
            record.state.status = JointStatus::PendingRemoval;
            reset_cache(record);
            if (kind == JointEventKind::BodyLost) {
                increment(counters.lost_bodies);
            }
        }
    }

    bool sample(Record& record) {
        const auto& description = record.state.description;
        const auto* first = world->get(description.first);
        const auto* second = world->get(description.second);
        if ((description.first && !first) || (description.second && !second)) {
            mark_removed(record, JointEventKind::BodyLost);
            return false;
        }
        record.first = frame_in_world(first, description.first_frame);
        record.second = frame_in_world(second, description.second_frame);
        if (!bounded(record.first.anchor, limits.coordinate_bound) ||
            !bounded(record.second.anchor, limits.coordinate_bound) ||
            !bounded(record.first.offset, limits.coordinate_bound) ||
            !bounded(record.second.offset, limits.coordinate_bound)) {
            record.state.status = JointStatus::InvalidGeometry;
            return false;
        }
        const auto displacement = record.second.anchor - record.first.anchor;
        const double distance = math::length(displacement);
        const auto rotation = rotation_vector(record.second.orientation * math::conjugate(record.first.orientation));
        record.state.first_anchor = record.first.anchor;
        record.state.second_anchor = record.second.anchor;
        record.state.coordinate = distance;
        record.state.linear_error = distance;
        record.state.angular_error = 0.0;
        switch (description.kind) {
        case JointKind::Distance:
            record.state.linear_error = std::abs(description.linear_limit.enabled ?
                limit_error(distance, description.linear_limit) : distance - description.rest_length);
            break;
        case JointKind::Spring:
            record.state.linear_error = std::max(std::abs(distance - description.rest_length),
                                          std::abs(limit_error(distance, description.linear_limit)));
            break;
        case JointKind::BallSocket:
            break;
        case JointKind::Hinge:
            record.state.coordinate = twist_coordinate(record.first, record.second);
            record.state.angular_error = std::max(math::length(swing_error(record.first, record.second)),
                std::abs(limit_error(record.state.coordinate, description.angular_limit)));
            break;
        case JointKind::Fixed:
            record.state.angular_error = math::length(rotation);
            break;
        case JointKind::Slider:
            record.state.coordinate = math::dot(displacement, record.first.axis);
            record.state.linear_error = std::max(math::length(math::reject(displacement, record.first.axis)),
                std::abs(limit_error(record.state.coordinate, description.linear_limit)));
            record.state.angular_error = math::length(rotation);
            break;
        }
        record.state.error = std::max(record.state.linear_error, record.state.angular_error);
        if (!std::isfinite(record.state.error) || !std::isfinite(record.state.coordinate)) {
            record.state.status = JointStatus::InvalidGeometry;
            return false;
        }
        if (!usable(first) || !usable(second) || (!dynamic(first) && !dynamic(second))) {
            record.state.status = JointStatus::InactiveBody;
            return false;
        }
        record.state.status = JointStatus::Ready;
        return true;
    }

    double row_weight(const Record& record, const Row& row) const noexcept {
        double result{};
        const auto* first = world->get(record.state.description.first);
        const auto* second = world->get(record.state.description.second);
        if (dynamic(first)) {
            result += first->inverse_mass * math::length_squared(row.linear) +
                math::dot(row.first_angular, inverse_inertia_world(*first) * row.first_angular);
        }
        if (dynamic(second)) {
            result += second->inverse_mass * math::length_squared(row.linear) +
                math::dot(row.second_angular, inverse_inertia_world(*second) * row.second_angular);
        }
        return result;
    }

    std::size_t build_rows(const Record& record, double delta, std::array<Row, 8>& output) {
        const auto& description = record.state.description;
        const auto& tune = description.tuning;
        const auto displacement = record.second.anchor - record.first.anchor;
        std::size_t count{};
        auto append = [&](math::Vec3 axis, double error, std::uint32_t key, bool angular,
                          int side, bool reference_rotates, bool spring, bool motor) {
            if (count == output.size()) {
                increment(counters.invalid_rows);
                return;
            }
            Row row;
            row.key = key;
            row.angular = angular;
            row.lower = side > 0 ? 0.0 : -tune.maximum_impulse;
            row.upper = side < 0 ? 0.0 : tune.maximum_impulse;
            row.error = dead_zone(error, angular ? tune.angular_slop : tune.linear_slop);
            if (angular) {
                row.first_angular = -axis;
                row.second_angular = axis;
            } else {
                row.linear = axis;
                row.first_angular = -math::cross(record.first.offset, axis);
                row.second_angular = math::cross(record.second.offset, axis);
                if (reference_rotates) {
                    row.first_angular -= math::cross(displacement, axis);
                }
            }
            double beta = tune.velocity_bias;
            const double compliance = angular ? tune.angular_compliance : tune.linear_compliance;
            if (compliance > 0.0) {
                row.gamma = compliance / (delta * delta);
                beta = 1.0;
                row.project = false;
            }
            if (spring) {
                const double denominator = description.spring_damping + delta * description.spring_stiffness;
                row.gamma = 1.0 / (delta * denominator);
                beta = delta * description.spring_stiffness / denominator;
                row.project = false;
            }
            row.bias = math::clamp(beta * row.error / delta,
                                   -tune.maximum_bias_speed, tune.maximum_bias_speed);
            if (motor) {
                const auto& drive = description.motor;
                const auto& limit = angular ? description.angular_limit : description.linear_limit;
                double target = drive.target;
                if (limit.enabled) {
                    target = math::clamp(target, limit.lower, limit.upper);
                }
                double difference = target - record.state.coordinate;
                if (angular && !limit.enabled) {
                    difference = math::delta_angle(record.state.coordinate, target);
                }
                double speed = drive.mode == JointMotorMode::Position ? difference * drive.gain : drive.velocity;
                speed = math::clamp(speed, -drive.maximum_speed, drive.maximum_speed);
                if (limit.enabled) {
                    speed = math::clamp(speed, std::min(0.0, (limit.lower - record.state.coordinate) / delta),
                                        std::max(0.0, (limit.upper - record.state.coordinate) / delta));
                }
                const double cap = std::min(tune.maximum_impulse, drive.maximum_force * delta);
                row.lower = -cap;
                row.upper = cap;
                row.gamma = 0.0;
                row.bias = -speed;
                row.project = false;
            }
            row.weight = row_weight(record, row);
            const double denominator = row.weight + row.gamma;
            if (!std::isfinite(denominator) || denominator <= 1.0e-14 || !std::isfinite(row.bias)) {
                increment(counters.invalid_rows);
                return;
            }
            row.mass = 1.0 / denominator;
            output[count++] = row;
        };
        auto add_limit = [&](const JointLimit& limit, math::Vec3 axis, bool angular, bool reference_rotates) {
            if (!limit.enabled) {
                return;
            }
            const double slop = angular ? tune.angular_slop : tune.linear_slop;
            if (limit.upper - limit.lower <= 2.0 * slop) {
                append(axis, record.state.coordinate - (limit.lower + limit.upper) * 0.5,
                       12, angular, 0, reference_rotates, false, false);
            } else if (record.state.coordinate <= limit.lower + slop) {
                append(axis, std::min(0.0, record.state.coordinate - limit.lower),
                       10, angular, 1, reference_rotates, false, false);
            } else if (record.state.coordinate >= limit.upper - slop) {
                append(axis, std::max(0.0, record.state.coordinate - limit.upper),
                       11, angular, -1, reference_rotates, false, false);
            }
        };
        const bool distance = description.kind == JointKind::Distance || description.kind == JointKind::Spring;
        if (distance) {
            const auto axis = math::normalized(displacement, record.first.axis);
            if (!description.linear_limit.enabled || description.kind == JointKind::Spring) {
                append(axis, record.state.coordinate - description.rest_length, 1, false, 0,
                       false, description.kind == JointKind::Spring, false);
            }
            add_limit(description.linear_limit, axis, false, false);
            return count;
        }
        if (description.kind == JointKind::Slider) {
            append(record.first.up, math::dot(displacement, record.first.up), 2, false, 0, true, false, false);
            append(record.first.side, math::dot(displacement, record.first.side), 3, false, 0, true, false, false);
        } else {
            for (std::size_t axis = 0; axis < basis.size(); ++axis) {
                append(basis[axis], displacement[axis], static_cast<std::uint32_t>(axis) + 2,
                       false, 0, false, false, false);
            }
        }
        if (description.kind == JointKind::Hinge) {
            const auto error = swing_error(record.first, record.second);
            append(record.first.up, math::dot(error, record.first.up), 5, true, 0, false, false, false);
            append(record.first.side, math::dot(error, record.first.side), 6, true, 0, false, false, false);
            add_limit(description.angular_limit, record.first.axis, true, false);
        } else if (description.kind == JointKind::Fixed || description.kind == JointKind::Slider) {
            const auto error = rotation_vector(record.second.orientation * math::conjugate(record.first.orientation));
            for (std::size_t axis = 0; axis < basis.size(); ++axis) {
                append(basis[axis], error[axis], static_cast<std::uint32_t>(axis) + 5,
                       true, 0, false, false, false);
            }
            if (description.kind == JointKind::Slider) {
                add_limit(description.linear_limit, record.first.axis, false, true);
            }
        }
        if (description.motor.mode != JointMotorMode::Disabled) {
            append(record.first.axis, 0.0, 20, description.kind == JointKind::Hinge, 0,
                   description.kind == JointKind::Slider, false, true);
        }
        return count;
    }

    double velocity(const Record& record, const Row& row) const noexcept {
        double result{};
        const auto* first = world->get(record.state.description.first);
        const auto* second = world->get(record.state.description.second);
        if (first && first->mode != BodyMode::Static && usable(first)) {
            result -= math::dot(row.linear, first->linear_velocity);
            result += math::dot(row.first_angular, first->angular_velocity);
        }
        if (second && second->mode != BodyMode::Static && usable(second)) {
            result += math::dot(row.linear, second->linear_velocity);
            result += math::dot(row.second_angular, second->angular_velocity);
        }
        return result;
    }

    void apply(const Record& record, const Row& row, double impulse) {
        if (impulse == 0.0) {
            return;
        }
        const auto& description = record.state.description;
        auto endpoint = [&](BodyHandle handle, math::Vec3 linear, math::Vec3 angular) {
            if (!dynamic(world->get(handle))) {
                return;
            }
            if (math::length_squared(linear) > 0.0 && !world->apply_impulse(handle, linear * impulse)) {
                increment(counters.invalid_rows);
            }
            if (math::length_squared(angular) > 0.0 && !world->apply_angular_impulse(handle, angular * impulse)) {
                increment(counters.invalid_rows);
            }
        };
        endpoint(description.first, -row.linear, row.first_angular);
        endpoint(description.second, row.linear, row.second_angular);
    }

    std::uint32_t root(std::uint32_t index) noexcept {
        auto result = index;
        while (parents[result] != result) {
            result = parents[result];
        }
        while (parents[index] != index) {
            const auto next = parents[index];
            parents[index] = result;
            index = next;
        }
        return result;
    }

    void prepare(double delta) {
        increment(counters.substeps);
        endpoints.clear();
        std::fill(seeds.begin(), seeds.end(), std::uint8_t{});
        for (std::uint32_t index = 1; index <= limits.capacity; ++index) {
            if (!slots[index].record) {
                continue;
            }
            auto& record = *slots[index].record;
            record.active = false;
            record.eligible = false;
            if (record.pending || record.broken) {
                continue;
            }
            if (!record.state.description.enabled || !counters.origin_consistent) {
                record.state.status = counters.origin_consistent ? JointStatus::Disabled : JointStatus::OriginInconsistent;
                reset_cache(record);
                continue;
            }
            if (!sample(record)) {
                reset_cache(record);
                continue;
            }
            record.eligible = true;
            parents[index] = index;
            const auto& description = record.state.description;
            const auto* first = world->get(description.first);
            const auto* second = world->get(description.second);
            seeds[index] = static_cast<std::uint8_t>(moving(first) || moving(second) ||
                description.motor.mode != JointMotorMode::Disabled ||
                record.state.linear_error > 2.0 * description.tuning.linear_slop ||
                record.state.angular_error > 2.0 * description.tuning.angular_slop);
            if (dynamic(first)) {
                endpoints.push_back({description.first, index});
            }
            if (dynamic(second)) {
                endpoints.push_back({description.second, index});
            }
        }
        std::sort(endpoints.begin(), endpoints.end(), [](const Endpoint& first, const Endpoint& second) {
            return first.body < second.body || (first.body == second.body && first.joint < second.joint);
        });
        for (std::size_t index = 1; index < endpoints.size(); ++index) {
            if (endpoints[index - 1].body == endpoints[index].body) {
                const auto first = root(endpoints[index - 1].joint);
                const auto second = root(endpoints[index].joint);
                parents[std::max(first, second)] = std::min(first, second);
            }
        }
        for (std::uint32_t index = 1; index <= limits.capacity; ++index) {
            if (slots[index].record && slots[index].record->eligible && seeds[index] != 0) {
                seeds[root(index)] = 1;
            }
        }
        for (std::uint32_t index = 1; index <= limits.capacity; ++index) {
            if (!slots[index].record || !slots[index].record->eligible) {
                continue;
            }
            auto& record = *slots[index].record;
            if (seeds[root(index)] == 0) {
                record.state.status = JointStatus::Sleeping;
                reset_cache(record);
                continue;
            }
            const auto& description = record.state.description;
            for (const auto handle : {description.first, description.second}) {
                const auto* body = world->get(handle);
                if (dynamic(body) && !body->awake) {
                    (void)world->wake(handle);
                }
            }
            const auto previous = record.rows;
            const auto previous_count = record.row_count;
            record.row_count = build_rows(record, delta, record.rows);
            record.state.solver_rows = record.row_count;
            record.active = true;
            const double ratio = record.previous_delta > 0.0 ? delta / record.previous_delta : 0.0;
            if (description.warm_start && ratio >= 0.25 && ratio <= 4.0) {
                for (std::size_t row_index = 0; row_index < record.row_count; ++row_index) {
                    auto& row = record.rows[row_index];
                    for (std::size_t old_index = 0; old_index < previous_count; ++old_index) {
                        const auto& old = previous[old_index];
                        const double alignment = math::dot(row.angular ? row.second_angular : row.linear,
                                                           old.angular ? old.second_angular : old.linear);
                        const double weight_ratio = old.weight > 0.0 ? row.weight / old.weight : 0.0;
                        if (row.key == old.key && alignment > 0.8 && weight_ratio >= 0.25 && weight_ratio <= 4.0) {
                            row.impulse = math::clamp(old.impulse * ratio * alignment, row.lower, row.upper);
                            apply(record, row, row.impulse);
                            break;
                        }
                    }
                }
            }
            record.previous_delta = delta;
        }
    }

    void solve_velocity() {
        for (auto& slot : slots) {
            if (!slot.record || !slot.record->active || slot.record->pending || slot.record->broken) {
                continue;
            }
            auto& record = *slot.record;
            for (std::size_t index = 0; index < record.row_count; ++index) {
                auto& row = record.rows[index];
                const double change = -row.mass * (velocity(record, row) + row.bias + row.gamma * row.impulse);
                if (!std::isfinite(change)) {
                    increment(counters.invalid_rows);
                    continue;
                }
                const double next = math::clamp(row.impulse + change, row.lower, row.upper);
                apply(record, row, next - row.impulse);
                row.impulse = next;
            }
        }
    }

    void solve_position(double delta) {
        for (auto& slot : slots) {
            if (!slot.record || !slot.record->active || slot.record->pending || slot.record->broken) {
                continue;
            }
            auto& record = *slot.record;
            if (!sample(record)) {
                record.active = false;
                continue;
            }
            std::array<Row, 8> rows{};
            const auto count = build_rows(record, delta, rows);
            const auto& description = record.state.description;
            const auto& tune = description.tuning;
            for (std::size_t index = 0; index < count; ++index) {
                const auto& row = rows[index];
                if (!row.project || row.weight <= 1.0e-14 || row.error == 0.0) {
                    continue;
                }
                const double maximum = row.angular ? tune.maximum_angle_delta : tune.maximum_position_delta;
                const double correction = -tune.position_fraction * math::clamp(row.error, -maximum, maximum);
                const double impulse = math::clamp(correction / row.weight, row.lower, row.upper);
                auto endpoint = [&](BodyHandle handle, math::Vec3 linear, math::Vec3 angular) {
                    const auto* body = world->get(handle);
                    if (!dynamic(body)) {
                        return;
                    }
                    const auto translation = math::clamp_length(linear * (body->inverse_mass * impulse),
                                                                tune.maximum_position_delta);
                    const auto rotation = math::clamp_length((inverse_inertia_world(*body) * angular) * impulse,
                                                             tune.maximum_angle_delta);
                    if (!world->apply_position_delta(handle, translation, rotation)) {
                        increment(counters.invalid_rows);
                    }
                };
                endpoint(description.first, -row.linear, row.first_angular);
                endpoint(description.second, row.linear, row.second_angular);
            }
        }
    }

    void finish(double delta) {
        for (auto& slot : slots) {
            if (!slot.record || !slot.record->active || slot.record->pending) {
                continue;
            }
            auto& record = *slot.record;
            if (!sample(record)) {
                record.active = false;
                continue;
            }
            auto& state = record.state;
            state.linear_impulse = {};
            state.first_angular_impulse = {};
            state.second_angular_impulse = {};
            for (std::size_t index = 0; index < record.row_count; ++index) {
                const auto& row = record.rows[index];
                state.linear_impulse += row.linear * row.impulse;
                state.first_angular_impulse += row.first_angular * row.impulse;
                state.second_angular_impulse += row.second_angular * row.impulse;
            }
            state.force = math::length(state.linear_impulse) / delta;
            state.torque = std::max(math::length(state.first_angular_impulse),
                                    math::length(state.second_angular_impulse)) / delta;
            state.solved_substep = counters.substeps;
            const auto& tune = state.description.tuning;
            if ((tune.break_force > 0.0 && state.force > tune.break_force) ||
                (tune.break_torque > 0.0 && state.torque > tune.break_torque)) {
                record.broken = true;
                record.active = false;
                state.status = JointStatus::Broken;
                increment(counters.breaks);
                emit(JointEventKind::Broken, record);
            }
        }
    }

    void solve(const ConstraintContext& context) {
        if (!bounded(context.delta_seconds, limits.minimum_delta, 1.0)) {
            if (context.stage == ConstraintStage::Prepare) {
                for (auto& slot : slots) {
                    if (slot.record) {
                        reset_cache(*slot.record);
                    }
                }
                increment(counters.invalid_rows);
            }
            return;
        }
        switch (context.stage) {
        case ConstraintStage::Prepare: prepare(context.delta_seconds); break;
        case ConstraintStage::Velocity: solve_velocity(); break;
        case ConstraintStage::Position: solve_position(context.delta_seconds); break;
        case ConstraintStage::Finish: finish(context.delta_seconds); break;
        }
    }
};

JointService::JointService(PhysicsWorld& world, JointLimits configured)
    : impl_(std::make_shared<Impl>(world, configured)) {
    const std::weak_ptr<Impl> weak = impl_;
    if (!world.add_constraint_solver(std::string(solver_name),
        [weak](PhysicsWorld& owner, const ConstraintContext& context) {
            if (const auto state = weak.lock(); state && state->connected && state->world == &owner) {
                state->solve(context);
            }
        })) {
        throw std::runtime_error("cannot register the physics.constraints solver");
    }
    impl_->connected = true;
}

JointService::~JointService() = default;
const JointLimits& JointService::limits() const noexcept { return impl_->limits; }

JointHandle JointService::create(const JointDescription& description) {
    if (!impl_->mutable_now() || !impl_->counters.origin_consistent ||
        !valid_joint_description(description, impl_->limits.coordinate_bound) ||
        !impl_->bodies_valid(description) || impl_->free_slots.empty()) {
        (void)impl_->reject();
        return {};
    }
    const auto index = impl_->free_slots.back();
    auto& slot = impl_->slots[index];
    Impl::Record record;
    record.state.handle = {index, slot.generation};
    record.state.description = description;
    record.state.description.first_frame.orientation = math::normalized(description.first_frame.orientation);
    record.state.description.second_frame.orientation = math::normalized(description.second_frame.orientation);
    if (!impl_->sample(record) && record.state.status != JointStatus::InactiveBody) {
        (void)impl_->reject();
        return {};
    }
    if (!description.enabled) {
        record.state.status = JointStatus::Disabled;
    }
    slot.record = std::move(record);
    impl_->free_slots.pop_back();
    impl_->emit(JointEventKind::Created, *slot.record);
    return slot.record->state.handle;
}

JointHandle JointService::attach(JointKind kind, BodyHandle first, BodyHandle second,
                                  math::Vec3 first_anchor, math::Vec3 second_anchor, math::Vec3 axis) {
    if (!impl_->mutable_now() || !bounded(first_anchor, impl_->limits.coordinate_bound) ||
        !bounded(second_anchor, impl_->limits.coordinate_bound) || !bounded(axis, 1.0e6) ||
        !math::normalize(axis)) {
        (void)impl_->reject();
        return {};
    }
    const auto* first_body = impl_->world->get(first);
    const auto* second_body = impl_->world->get(second);
    const auto rotation = math::from_to(basis[0], axis);
    const auto first_frame = frame_in_body(first_body, first_anchor, rotation);
    const auto second_frame = frame_in_body(second_body, second_anchor, rotation);
    if (!first_frame || !second_frame) {
        (void)impl_->reject();
        return {};
    }
    JointDescription description;
    description.kind = kind;
    description.first = first;
    description.second = second;
    description.first_frame = *first_frame;
    description.second_frame = *second_frame;
    description.rest_length = math::distance(first_anchor, second_anchor);
    return create(description);
}

bool JointService::configure(JointHandle handle, const JointDescription& description) {
    auto* record = impl_->get(handle);
    if (!impl_->mutable_now() || !record || record->pending ||
        !valid_joint_description(description, impl_->limits.coordinate_bound) || !impl_->bodies_valid(description)) {
        return impl_->reject();
    }
    Impl::Record replacement;
    replacement.state.handle = handle;
    replacement.state.description = description;
    replacement.state.description.first_frame.orientation = math::normalized(description.first_frame.orientation);
    replacement.state.description.second_frame.orientation = math::normalized(description.second_frame.orientation);
    replacement.broken = record->broken;
    if (!impl_->sample(replacement) && replacement.state.status != JointStatus::InactiveBody) {
        return impl_->reject();
    }
    if (replacement.broken) {
        replacement.state.status = JointStatus::Broken;
    } else if (!description.enabled) {
        replacement.state.status = JointStatus::Disabled;
    }
    *record = std::move(replacement);
    impl_->emit(JointEventKind::Reconfigured, *record);
    return true;
}

bool JointService::set_enabled(JointHandle handle, bool enabled) {
    const auto state = inspect(handle);
    if (!state) {
        return impl_->reject();
    }
    auto description = state->description;
    description.enabled = enabled;
    return configure(handle, description);
}

bool JointService::set_motor(JointHandle handle, const JointMotor& motor) {
    const auto state = inspect(handle);
    if (!state) {
        return impl_->reject();
    }
    auto description = state->description;
    description.motor = motor;
    return configure(handle, description);
}

bool JointService::set_limit(JointHandle handle, const JointLimit& limit) {
    const auto state = inspect(handle);
    if (!state) {
        return impl_->reject();
    }
    auto description = state->description;
    if (description.kind == JointKind::Hinge) {
        description.angular_limit = limit;
    } else {
        description.linear_limit = limit;
    }
    return configure(handle, description);
}

bool JointService::repair(JointHandle handle) {
    auto* record = impl_->get(handle);
    if (!impl_->mutable_now() || !record || record->pending || !record->broken ||
        !impl_->bodies_valid(record->state.description)) {
        return impl_->reject();
    }
    record->broken = false;
    impl_->reset_cache(*record);
    record->state.status = record->state.description.enabled ? JointStatus::Ready : JointStatus::Disabled;
    impl_->emit(JointEventKind::Repaired, *record);
    return true;
}

bool JointService::remove(JointHandle handle) {
    auto* record = impl_->get(handle);
    if (!impl_->connected || !record) {
        return impl_->reject();
    }
    impl_->mark_removed(*record, JointEventKind::Removed);
    if (!impl_->world->stepping()) {
        (void)maintenance();
    }
    return true;
}

void JointService::clear() {
    if (!impl_->connected) {
        return;
    }
    for (auto& slot : impl_->slots) {
        if (slot.record) {
            impl_->mark_removed(*slot.record, JointEventKind::Removed);
        }
    }
    if (!impl_->world->stepping()) {
        (void)maintenance();
    }
}

std::size_t JointService::maintenance() {
    if (!impl_->mutable_now()) {
        return 0;
    }
    std::size_t removed{};
    for (std::uint32_t index = 1; index <= impl_->limits.capacity; ++index) {
        auto& slot = impl_->slots[index];
        if (!slot.record) {
            continue;
        }
        auto& record = *slot.record;
        const auto& description = record.state.description;
        if ((description.first && !impl_->world->get(description.first)) ||
            (description.second && !impl_->world->get(description.second))) {
            impl_->mark_removed(record, JointEventKind::BodyLost);
        }
        if (!record.pending) {
            continue;
        }
        impl_->emit(record.removal_kind, record);
        slot.record.reset();
        if (slot.generation != std::numeric_limits<std::uint64_t>::max()) {
            ++slot.generation;
            impl_->free_slots.push_back(index);
        }
        ++removed;
    }
    return removed;
}

std::optional<JointState> JointService::inspect(JointHandle handle) const {
    const auto* record = impl_->get(handle);
    return record ? std::optional<JointState>{record->state} : std::nullopt;
}

std::vector<JointHandle> JointService::joints() const {
    std::vector<JointHandle> result;
    result.reserve(static_cast<std::size_t>(impl_->limits.capacity) - impl_->free_slots.size());
    for (const auto& slot : impl_->slots) {
        if (slot.record) {
            result.push_back(slot.record->state.handle);
        }
    }
    return result;
}

JointStatistics JointService::statistics() const noexcept {
    auto result = impl_->counters;
    result.connected = impl_->connected;
    for (const auto& slot : impl_->slots) {
        if (!slot.record) {
            continue;
        }
        const auto& record = *slot.record;
        ++result.joints;
        result.active += static_cast<std::size_t>(record.active);
        result.sleeping += static_cast<std::size_t>(record.state.status == JointStatus::Sleeping);
        result.broken += static_cast<std::size_t>(record.broken);
        result.pending_removals += static_cast<std::size_t>(record.pending);
        result.rows += record.active ? record.row_count : 0;
        result.largest_error = std::max(result.largest_error, record.state.error);
        result.largest_force = std::max(result.largest_force, record.state.force);
        result.largest_torque = std::max(result.largest_torque, record.state.torque);
    }
    return result;
}

std::span<const JointEvent> JointService::events() const noexcept { return impl_->events; }
void JointService::clear_events() noexcept { impl_->events.clear(); }

bool JointService::shift_origin(math::Vec3 offset) {
    if (!impl_->mutable_now() || !bounded(offset, impl_->limits.coordinate_bound) ||
        (impl_->origin_bound && !math::finite(impl_->origin.absolute_origin + offset))) {
        return impl_->reject();
    }
    if (offset == math::Vec3{}) {
        return true;
    }
    for (const auto& slot : impl_->slots) {
        if (!slot.record) {
            continue;
        }
        const auto& description = slot.record->state.description;
        if ((!description.first && !bounded(description.first_frame.anchor - offset, impl_->limits.coordinate_bound)) ||
            (!description.second && !bounded(description.second_frame.anchor - offset, impl_->limits.coordinate_bound))) {
            return impl_->reject();
        }
    }
    for (auto& slot : impl_->slots) {
        if (!slot.record) {
            continue;
        }
        auto& record = *slot.record;
        auto& description = record.state.description;
        if (!description.first) {
            description.first_frame.anchor -= offset;
        }
        if (!description.second) {
            description.second_frame.anchor -= offset;
        }
        record.state.first_anchor -= offset;
        record.state.second_anchor -= offset;
        record.first.anchor -= offset;
        record.second.anchor -= offset;
        impl_->reset_cache(record);
    }
    if (impl_->origin_bound) {
        impl_->origin.absolute_origin += offset;
    }
    increment(impl_->counters.origin_shifts);
    if (impl_->events.size() < impl_->limits.event_capacity) {
        impl_->events.push_back({JointEventKind::OriginShifted, {}, {}, {}, impl_->counters.substeps, 0.0, 0.0});
    } else {
        increment(impl_->counters.dropped_events);
    }
    return true;
}

bool JointService::synchronize_origin(const scene::WorldPartition& partition) {
    if (!impl_->mutable_now()) {
        return impl_->reject();
    }
    const auto current = partition.origin();
    if (!current.consistent) {
        impl_->counters.origin_consistent = false;
        return false;
    }
    if (!impl_->origin_bound) {
        impl_->origin = current;
        impl_->origin_bound = true;
        impl_->counters.origin_consistent = true;
        return true;
    }
    if (current.epoch == impl_->origin.epoch) {
        impl_->counters.origin_consistent = true;
        return true;
    }
    const auto history = partition.shifts_since(impl_->origin.epoch);
    const auto offset = history.latest.absolute_origin - impl_->origin.absolute_origin;
    if (!history.latest.consistent || !shift_origin(offset)) {
        impl_->counters.origin_consistent = false;
        return false;
    }
    impl_->origin = history.latest;
    impl_->counters.origin_consistent = true;
    return true;
}

bool JointService::shutdown() {
    if (!impl_->connected) {
        return true;
    }
    if (impl_->world->stepping()) {
        return impl_->reject();
    }
    (void)impl_->world->remove_constraint_solver(solver_name);
    impl_->connected = false;
    impl_->world = nullptr;
    for (auto& slot : impl_->slots) {
        if (slot.record) {
            impl_->reset_cache(*slot.record);
        }
    }
    return true;
}

namespace {

std::optional<double> parse_number(std::string_view text) noexcept {
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value) ?
        std::optional<double>{value} : std::nullopt;
}

template<class Handle>
std::optional<Handle> parse_handle(std::string_view text) noexcept {
    const auto separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == text.size()) {
        return std::nullopt;
    }
    Handle result;
    const auto index = std::from_chars(text.data(), text.data() + separator, result.index);
    const auto generation = std::from_chars(text.data() + separator + 1, text.data() + text.size(), result.generation);
    if (index.ec != std::errc{} || index.ptr != text.data() + separator ||
        generation.ec != std::errc{} || generation.ptr != text.data() + text.size() || !result) {
        return std::nullopt;
    }
    return result;
}

std::optional<math::Vec3> parse_vector(core::CommandArgs args, std::size_t start) noexcept {
    if (start > args.size() || args.size() - start < 3) {
        return std::nullopt;
    }
    const auto x = parse_number(args[start]);
    const auto y = parse_number(args[start + 1]);
    const auto z = parse_number(args[start + 2]);
    return x && y && z ? std::optional<math::Vec3>{math::Vec3{*x, *y, *z}} : std::nullopt;
}

std::optional<JointKind> parse_kind(std::string_view text) noexcept {
    for (const auto kind : {JointKind::Distance, JointKind::Spring, JointKind::BallSocket,
                            JointKind::Hinge, JointKind::Fixed, JointKind::Slider}) {
        if (joint_kind_name(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string handle_text(JointHandle handle) {
    return std::to_string(handle.index) + ":" + std::to_string(handle.generation);
}

core::CommandResult configure_command(JointService& service, core::CommandArgs args) {
    if (args.size() < 3 || args.size() > 4) {
        return {false, "usage: joint.configure handle rest|impulse value; spring|bias|compliance|break first second"};
    }
    const auto handle = parse_handle<JointHandle>(args[0]);
    const auto state = handle ? service.inspect(*handle) : std::nullopt;
    const auto first = parse_number(args[2]);
    const auto second = args.size() == 4 ? parse_number(args[3]) : std::optional<double>{0.0};
    if (!state || !first || !second) {
        return {false, "joint or finite numeric values are invalid"};
    }
    auto description = state->description;
    if (args[1] == "rest" && args.size() == 3) {
        description.rest_length = *first;
    } else if (args[1] == "impulse" && args.size() == 3) {
        description.tuning.maximum_impulse = *first;
    } else if (args[1] == "spring" && args.size() == 4) {
        description.spring_stiffness = *first;
        description.spring_damping = *second;
    } else if (args[1] == "bias" && args.size() == 4) {
        description.tuning.velocity_bias = *first;
        description.tuning.position_fraction = *second;
    } else if (args[1] == "compliance" && args.size() == 4) {
        description.tuning.linear_compliance = *first;
        description.tuning.angular_compliance = *second;
    } else if (args[1] == "break" && args.size() == 4) {
        description.tuning.break_force = *first;
        description.tuning.break_torque = *second;
    } else {
        return {false, "unknown joint setting or incorrect value count"};
    }
    const bool changed = service.configure(*handle, description);
    return {changed, changed ? "joint configured" : "joint configuration rejected"};
}

core::CommandResult motor_command(JointService& service, core::CommandArgs args) {
    if (args.size() < 2) {
        return {false, "usage: joint.motor handle off|velocity speed force|position target gain max-speed force"};
    }
    const auto handle = parse_handle<JointHandle>(args[0]);
    JointMotor motor;
    if (!handle) {
        return {false, "invalid generation handle"};
    }
    if (args[1] == "off" && args.size() == 2) {
        motor.mode = JointMotorMode::Disabled;
    } else if (args[1] == "velocity" && args.size() == 4) {
        const auto speed = parse_number(args[2]);
        const auto force = parse_number(args[3]);
        if (!speed || !force) {
            return {false, "motor speed and force must be finite"};
        }
        motor.mode = JointMotorMode::Velocity;
        motor.velocity = *speed;
        motor.maximum_speed = std::abs(*speed);
        motor.maximum_force = *force;
    } else if (args[1] == "position" && args.size() == 6) {
        const auto target = parse_number(args[2]);
        const auto gain = parse_number(args[3]);
        const auto speed = parse_number(args[4]);
        const auto force = parse_number(args[5]);
        if (!target || !gain || !speed || !force) {
            return {false, "motor parameters must be finite"};
        }
        motor.mode = JointMotorMode::Position;
        motor.target = *target;
        motor.gain = *gain;
        motor.maximum_speed = *speed;
        motor.maximum_force = *force;
    } else {
        return {false, "unknown motor mode or incorrect parameter count"};
    }
    const bool changed = service.set_motor(*handle, motor);
    return {changed, changed ? "joint motor configured" : "motor configuration rejected"};
}

} // namespace

void register_constraints(core::Kernel& kernel) {
    if (kernel.services().find<JointService>()) {
        return;
    }
    auto* physics = kernel.services().find<PhysicsWorld>();
    if (!physics) {
        throw std::logic_error("constraints registration requires PhysicsWorld");
    }
    JointLimits limits;
    limits.capacity = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        kernel.config().integer("physics.joint_capacity", 4096), 0, 65536));
    limits.event_capacity = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("physics.joint_event_capacity", 8192), 0, 1048576));
    auto& service = kernel.services().emplace<JointService>(*physics, limits);
    if (const auto* partition = kernel.services().find<scene::WorldPartition>()) {
        (void)service.synchronize_origin(*partition);
    }
    (void)kernel.register_module({"physics.constraints", "Shared-body joints, limits, motors and bounded impulses", 1});
    (void)kernel.add_shutdown("physics.constraints", [owned = &service] { (void)owned->shutdown(); });
    (void)kernel.add_system({"physics.constraints.prepare", core::Phase::PreSimulation, -8700, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& joints = host.services().get<JointService>();
            (void)joints.maintenance();
            if (const auto* partition = host.services().find<scene::WorldPartition>()) {
                if (!joints.synchronize_origin(*partition)) {
                    host.diagnose(core::Diagnostic::Severity::Warning, "physics.constraints", "joint origin synchronization rejected");
                }
            }
        });
    (void)kernel.add_system({"physics.constraints.publish", core::Phase::PostSimulation, 1100, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& joints = host.services().get<JointService>();
            (void)joints.maintenance();
            const auto stats = joints.statistics();
            host.metric("physics.joints", static_cast<double>(stats.joints));
            host.metric("physics.joints.active", static_cast<double>(stats.active));
            host.metric("physics.joints.rows", static_cast<double>(stats.rows));
            host.metric("physics.joints.broken", static_cast<double>(stats.broken));
            host.metric("physics.joints.error", stats.largest_error);
            host.metric("physics.joints.force", stats.largest_force);
            host.metric("physics.joints.torque", stats.largest_torque);
            host.metric("physics.joints.invalid_rows", static_cast<double>(stats.invalid_rows));
            host.metric("physics.joints.dropped_events", static_cast<double>(stats.dropped_events));
            host.metric("physics.joints.origin_consistent", stats.origin_consistent ? 1.0 : 0.0);
            std::size_t unpublished{};
            for (const auto& event : joints.events()) {
                unpublished += static_cast<std::size_t>(!host.events().publish(event));
            }
            host.metric("physics.joints.unpublished_events", static_cast<double>(unpublished));
            joints.clear_events();
        });
    (void)kernel.add_command("joint.attach", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 9 && args.size() != 12) {
            return core::CommandResult{false, "usage: joint.attach kind body|world body|world ax ay az bx by bz [axis-x axis-y axis-z]"};
        }
        const auto kind = parse_kind(args[0]);
        const auto first = args[1] == "world" ? std::optional<BodyHandle>{BodyHandle{}} : parse_handle<BodyHandle>(args[1]);
        const auto second = args[2] == "world" ? std::optional<BodyHandle>{BodyHandle{}} : parse_handle<BodyHandle>(args[2]);
        const auto first_anchor = parse_vector(args, 3);
        const auto second_anchor = parse_vector(args, 6);
        const auto axis = args.size() == 12 ? parse_vector(args, 9) : std::optional<math::Vec3>{basis[0]};
        if (!kind || !first || !second || !first_anchor || !second_anchor || !axis) {
            return core::CommandResult{false, "invalid joint kind, body generation handle, or coordinates"};
        }
        auto& joints = host.services().get<JointService>();
        if (const auto* partition = host.services().find<scene::WorldPartition>()) {
            if (!joints.synchronize_origin(*partition)) {
                return core::CommandResult{false, "origin is inconsistent"};
            }
        }
        const auto handle = joints.attach(*kind, *first, *second, *first_anchor, *second_anchor, *axis);
        return core::CommandResult{static_cast<bool>(handle), handle ? "joint " + handle_text(handle) : "joint attachment rejected"};
    }, "Attach runtime anchors using generation body handles; world is a fixed endpoint.");
    (void)kernel.add_command("joint.configure", [](core::Kernel& host, core::CommandArgs args) {
        return configure_command(host.services().get<JointService>(), args);
    }, "Set joint rest length, spring constants, compliance, bias, impulse cap or break thresholds.");
    (void)kernel.add_command("joint.motor", [](core::Kernel& host, core::CommandArgs args) {
        return motor_command(host.services().get<JointService>(), args);
    }, "Configure a hinge or slider velocity/position motor; angles are radians.");
    (void)kernel.add_command("joint.limit", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2 && args.size() != 3) {
            return core::CommandResult{false, "usage: joint.limit handle off|lower upper"};
        }
        const auto handle = parse_handle<JointHandle>(args[0]);
        JointLimit limit;
        if (args.size() == 3) {
            const auto lower = parse_number(args[1]);
            const auto upper = parse_number(args[2]);
            if (!lower || !upper) {
                return core::CommandResult{false, "limits must be finite"};
            }
            limit = {true, *lower, *upper};
        } else if (args[1] != "off") {
            return core::CommandResult{false, "expected off or two bounds"};
        }
        const bool changed = handle && host.services().get<JointService>().set_limit(*handle, limit);
        return core::CommandResult{changed, changed ? "joint limit configured" : "joint limit rejected"};
    }, "Set distance/slider bounds in metres or hinge twist bounds in radians.");
    (void)kernel.add_command("joint.enable", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2 || (args[1] != "true" && args[1] != "false")) {
            return core::CommandResult{false, "usage: joint.enable handle true|false"};
        }
        const auto handle = parse_handle<JointHandle>(args[0]);
        const bool changed = handle && host.services().get<JointService>().set_enabled(*handle, args[1] == "true");
        return core::CommandResult{changed, changed ? "joint enable flag changed" : "joint enable change rejected"};
    });
    (void)kernel.add_command("joint.remove", [](core::Kernel& host, core::CommandArgs args) {
        const auto handle = args.size() == 1 ? parse_handle<JointHandle>(args[0]) : std::nullopt;
        const bool removed = handle && host.services().get<JointService>().remove(*handle);
        return core::CommandResult{removed, removed ? "joint removal accepted" : "usage: joint.remove valid-index:generation"};
    });
    (void)kernel.add_command("joint.repair", [](core::Kernel& host, core::CommandArgs args) {
        const auto handle = args.size() == 1 ? parse_handle<JointHandle>(args[0]) : std::nullopt;
        const bool repaired = handle && host.services().get<JointService>().repair(*handle);
        return core::CommandResult{repaired, repaired ? "broken joint repaired" : "repair requires a broken joint and live bodies"};
    });
    (void)kernel.add_command("joint.state", [](core::Kernel& host, core::CommandArgs args) {
        auto& joints = host.services().get<JointService>();
        if (args.empty()) {
            const auto stats = joints.statistics();
            return core::CommandResult{true, "joints=" + std::to_string(stats.joints) + " active=" +
                std::to_string(stats.active) + " broken=" + std::to_string(stats.broken) +
                " error=" + std::to_string(stats.largest_error)};
        }
        const auto handle = args.size() == 1 ? parse_handle<JointHandle>(args[0]) : std::nullopt;
        const auto state = handle ? joints.inspect(*handle) : std::nullopt;
        if (!state) {
            return core::CommandResult{false, "usage: joint.state [valid-index:generation]"};
        }
        return core::CommandResult{true, "joint " + handle_text(state->handle) + " " +
            std::string(joint_kind_name(state->description.kind)) + " " + std::string(joint_status_name(state->status)) +
            " coordinate=" + std::to_string(state->coordinate) + " linear-error=" + std::to_string(state->linear_error) +
            " angular-error=" + std::to_string(state->angular_error) +
            " force=" + std::to_string(state->force) + " torque=" + std::to_string(state->torque)};
    }, "Inspect solved coordinates, reaction force, reaction torque, and joint lifecycle state.");
}

} // namespace ge::physics
