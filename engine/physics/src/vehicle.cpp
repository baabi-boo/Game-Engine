#include <ge/physics/vehicle.hpp>
#include <ge/core/runtime.hpp>
#include <ge/input/actions.hpp>
#include <ge/physics/character.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace ge::physics {
namespace {

bool range(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool bounded_vector(math::Vec3 value, double maximum) noexcept {
    return math::finite(value) && std::abs(value.x) <= maximum &&
        std::abs(value.y) <= maximum && std::abs(value.z) <= maximum;
}

bool valid_tuning(const VehicleTuning& value) noexcept {
    return range(value.engine_torque, 0.0, 1.0e7) &&
        range(value.reverse_torque_factor, 0.0, 1.0) &&
        range(value.maximum_wheel_torque, 0.01, 1.0e8) &&
        range(value.maximum_wheel_speed, 1.0, 10000.0) &&
        range(value.maximum_steering_angle, 0.0, 1.4) &&
        range(value.steering_rate, 0.01, 100.0) &&
        range(value.ackermann_blend, 0.0, 1.0) &&
        range(value.wheelbase, 0.01, 100.0) &&
        range(value.minimum_ground_alignment, 0.05, 1.0) &&
        range(value.air_density, 0.0, 10.0) &&
        range(value.drag_coefficient, 0.0, 10.0) &&
        range(value.frontal_area, 0.0, 1000.0) &&
        range(value.downforce_coefficient, 0.0, 10.0) &&
        range(value.maximum_aero_force, 0.0, 1.0e9) &&
        range(value.ai_input_timeout, 0.0, 3600.0) &&
        bounded_vector(value.aerodynamic_center, 1000.0) &&
        bounded_vector(value.wind_velocity, 1000.0);
}

bool valid_authority(VehicleAuthority authority) noexcept {
    return authority == VehicleAuthority::Manual || authority == VehicleAuthority::Human ||
        authority == VehicleAuthority::AI;
}

math::Vec3 support_velocity(const BodyState* body, math::Vec3 point) noexcept {
    return body && body->enabled && body->scene_enabled && body->mode != BodyMode::Static
        ? point_velocity(*body, point) : math::Vec3{};
}

double effective_inverse_mass(const BodyState* body, math::Vec3 point,
                              math::Vec3 direction) noexcept {
    if (!body || !body->enabled || !body->scene_enabled || body->mode != BodyMode::Dynamic) {
        return 0.0;
    }
    const auto moment = math::cross(point - center_of_mass(*body), direction);
    return std::max(0.0, body->inverse_mass + math::dot(moment, inverse_inertia_world(*body) * moment));
}

double wheel_steering(const WheelDescription& wheel, const VehicleTuning& tuning,
                      double steering) noexcept {
    const double angle = std::clamp(steering * wheel.steering_ratio, -1.4, 1.4);
    if (std::abs(angle) < 1.0e-6 || tuning.ackermann_blend == 0.0) {
        return angle;
    }
    const double radius = tuning.wheelbase / std::tan(std::abs(angle));
    const double lateral_offset = std::copysign(1.0, angle) * wheel.mount.x;
    const double corrected = std::copysign(std::atan2(tuning.wheelbase,
        std::max(0.01, radius - lateral_offset)), angle);
    return std::clamp(math::lerp(angle, corrected, tuning.ackermann_blend), -1.4, 1.4);
}

std::optional<double> number(std::string_view text) noexcept {
    double result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

std::optional<std::size_t> wheel_index(std::string_view text) noexcept {
    std::size_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        result < maximum_vehicle_wheels ? std::optional<std::size_t>{result} : std::nullopt;
}

ecs::Entity resolve_entity(PhysicsWorld& physics, std::string_view text) {
    if (const auto entity = ecs::parse_entity(text)) {
        return physics.registry().alive(*entity) ? *entity : ecs::Entity{};
    }
    const auto matches = physics.registry().find_all_named(text);
    return matches.size() == 1 ? matches.front() : ecs::Entity{};
}

struct BusyScope {
    bool& busy;
    explicit BusyScope(bool& value) : busy(value) { busy = true; }
    ~BusyScope() { busy = false; }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;
};

} // namespace

std::string_view describe(VehicleError error) noexcept {
    switch (error) {
    case VehicleError::None: return "none";
    case VehicleError::InvalidEntity: return "invalid scene entity";
    case VehicleError::InvalidBody: return "requires an existing dynamic unit-scale chassis";
    case VehicleError::InvalidDescription: return "invalid vehicle parameters";
    case VehicleError::AlreadyAttached: return "vehicle or character already owns this entity";
    case VehicleError::Capacity: return "vehicle capacity exhausted";
    case VehicleError::Busy: return "vehicle, registry, or physics is busy";
    case VehicleError::InvalidHandle: return "stale or invalid vehicle handle";
    case VehicleError::InvalidInput: return "invalid input or wheel torque";
    case VehicleError::InvalidWheel: return "wheel index is out of range";
    case VehicleError::InvalidVisual: return "wheel visual is invalid or already owned";
    case VehicleError::WrongAuthority: return "input authority must be explicitly selected";
    case VehicleError::InputUnavailable: return "vehicle input context unavailable";
    case VehicleError::QueryIncomplete: return "suspension query budget exhausted";
    case VehicleError::ForceRejected: return "physics rejected a vehicle force";
    case VehicleError::Closed: return "vehicle service is shut down";
    }
    return "unknown vehicle error";
}

std::optional<DriveInput> bounded_drive_input(DriveInput input) noexcept {
    if (!std::isfinite(input.throttle) || !std::isfinite(input.steering) ||
        !std::isfinite(input.brake) || !std::isfinite(input.handbrake)) {
        return std::nullopt;
    }
    input.throttle = std::clamp(input.throttle, -1.0, 1.0);
    input.steering = std::clamp(input.steering, -1.0, 1.0);
    input.brake = std::clamp(input.brake, 0.0, 1.0);
    input.handbrake = std::clamp(input.handbrake, 0.0, 1.0);
    return input;
}

bool valid_wheel(const WheelDescription& wheel) noexcept {
    return bounded_vector(wheel.mount, 1000.0) && range(wheel.radius, 0.01, 10.0) &&
        range(wheel.inertia, 0.001, 1.0e5) && range(wheel.rest_length, 0.01, 10.0) &&
        range(wheel.compression_travel, 0.0, wheel.rest_length - 0.001) &&
        range(wheel.droop_travel, 0.0, 10.0) && range(wheel.spring_rate, 0.0, 1.0e8) &&
        range(wheel.compression_damping, 0.0, 1.0e7) && range(wheel.rebound_damping, 0.0, 1.0e7) &&
        range(wheel.bump_stop_rate, 0.0, 1.0e9) && range(wheel.maximum_suspension_force, 0.0, 1.0e9) &&
        range(wheel.longitudinal_stiffness, 0.0, 1.0e8) && range(wheel.lateral_stiffness, 0.0, 1.0e8) &&
        range(wheel.friction, 0.0, 10.0) && range(wheel.rolling_resistance, 0.0, 1.0) &&
        range(wheel.bearing_drag, 0.0, 1.0e4) && range(wheel.maximum_brake_torque, 0.0, 1.0e7) &&
        range(wheel.handbrake_torque, 0.0, 1.0e7) && range(wheel.steering_ratio, -2.0, 2.0) &&
        math::finite(wheel.visual_rotation) && range(math::dot(wheel.visual_rotation,
            wheel.visual_rotation), 1.0e-8, 1.0e4) &&
        range(wheel.visual_scale.x, 0.001, 100.0) && range(wheel.visual_scale.y, 0.001, 100.0) &&
        range(wheel.visual_scale.z, 0.001, 100.0);
}

bool valid_vehicle(const VehicleDescription& description) noexcept {
    return valid_tuning(description.tuning) && !description.wheels.empty() &&
        description.wheels.size() <= maximum_vehicle_wheels &&
        std::all_of(description.wheels.begin(), description.wheels.end(), valid_wheel);
}

VehicleDescription four_wheel_vehicle() {
    VehicleDescription result;
    result.wheels.resize(4);
    for (std::size_t index = 0; index < result.wheels.size(); ++index) {
        auto& wheel = result.wheels[index];
        const bool front = index < 2;
        wheel.mount = {index % 2 == 0 ? -0.8 : 0.8, 0.0, front ? -1.35 : 1.35};
        wheel.steering_ratio = front ? 1.0 : 0.0;
        wheel.driven = !front;
        wheel.handbrake = !front;
    }
    return result;
}

struct VehicleSystem::Impl {
    struct Record {
        VehicleDescription description;
        VehicleState state;
        std::vector<WheelState> wheels;
        std::vector<WheelTorque> torques;
        std::vector<std::optional<math::Transform>> original_visuals;
        std::array<double, maximum_vehicle_wheels> previous_omega{};
        math::Vec3 initial_velocity{};
        VehicleAuthority previous_authority{VehicleAuthority::Manual};
        bool integrated{};
    };

    PhysicsWorld& physics;
    ecs::Registry& registry;
    VehicleLimits limits;
    ge::GenerationPool handles;
    std::vector<std::optional<Record>> slots;
    std::vector<ColliderHandle> candidates;
    VehicleStatistics statistics{};
    VehicleError error{VehicleError::None};
    VehicleHandle human{};
    input::InputSystem* input{};
    std::uint64_t context{};
    std::uint64_t tick{};
    std::uint64_t physics_start{};
    double delta{};
    bool have_tick{};
    bool prepared{};
    bool busy{};
    bool closed{};

    Impl(PhysicsWorld& world, VehicleLimits configuration)
        : physics(world), registry(world.registry()), limits(configuration), handles(configuration.vehicles) {
        if (limits.vehicles > 16384 || limits.query_candidates == 0 ||
            limits.query_candidates > 4096 || limits.queries_per_frame > 1048576) {
            throw std::invalid_argument("vehicle limits exceed bounded storage or work limits");
        }
        candidates.resize(limits.query_candidates);
        (void)registry.register_component<VehicleComponent>("physics.vehicle");
    }

    bool reject(VehicleError reason) noexcept {
        error = reason;
        return false;
    }

    bool editable() noexcept {
        if (closed) { return reject(VehicleError::Closed); }
        if (busy || physics.stepping() || registry.iterating()) { return reject(VehicleError::Busy); }
        error = VehicleError::None;
        return true;
    }

    Record* raw(VehicleHandle handle) noexcept {
        if (!handles.alive({handle.index, handle.generation})) { return nullptr; }
        auto& slot = slots[static_cast<std::size_t>(handle.index - 1)];
        return slot ? &*slot : nullptr;
    }

    const Record* raw(VehicleHandle handle) const noexcept {
        return const_cast<Impl*>(this)->raw(handle);
    }

    bool owned(const Record& record) const noexcept {
        const auto* component = registry.get<VehicleComponent>(record.state.entity);
        const auto* body = physics.get(record.state.chassis);
        return component && component->vehicle == record.state.handle &&
            component->chassis == record.state.chassis && body && body->entity == record.state.entity &&
            physics.find(record.state.entity) == record.state.chassis &&
            !registry.has<CharacterComponent>(record.state.entity);
    }

    Record* edit(VehicleHandle handle) noexcept {
        if (!editable()) { return nullptr; }
        auto* record = raw(handle);
        if (!record || !owned(*record)) {
            (void)reject(VehicleError::InvalidHandle);
            return nullptr;
        }
        return record;
    }

    bool visual_allowed(ecs::Entity visual, ecs::Entity chassis, VehicleHandle owner = {},
                        std::size_t wheel = maximum_vehicle_wheels) {
        if (!visual) { return true; }
        auto& scene = physics.scene();
        const auto* colliders = registry.get<ColliderComponent>(visual);
        if (!scene.contains(visual) || visual == chassis || scene.is_ancestor(visual, chassis) ||
            physics.find(visual) || (colliders && !colliders->colliders.empty())) {
            return false;
        }
        for (const auto& slot : slots) {
            if (!slot) { continue; }
            for (std::size_t index = 0; index < slot->description.wheels.size(); ++index) {
                if (slot->state.handle == owner && index == wheel) { continue; }
                const auto other = slot->description.wheels[index].visual;
                if (other && (other == visual || scene.is_ancestor(other, visual) || scene.is_ancestor(visual, other))) {
                    return false;
                }
            }
        }
        return scene.local_transform(visual).has_value();
    }

    void restore_visual(Record& record, std::size_t index) {
        const auto visual = record.description.wheels[index].visual;
        if (visual && record.original_visuals[index] && visual_allowed(visual,
                record.state.entity, record.state.handle, index)) {
            if (!physics.scene().set_local_transform(visual, *record.original_visuals[index])) {
                ++statistics.rejected_visuals;
            }
        }
    }

    void release_human() {
        if (input && context != 0) { (void)input->set_context_enabled(context, false); }
        if (auto* record = raw(human)) {
            record->state.authority = record->previous_authority;
            record->state.input = {};
            std::fill(record->torques.begin(), record->torques.end(), WheelTorque{});
        }
        human = {};
    }

    void erase(VehicleHandle handle) {
        auto* record = raw(handle);
        if (!record) { return; }
        if (human == handle) { release_human(); }
        for (std::size_t index = 0; index < record->wheels.size(); ++index) { restore_visual(*record, index); }
        const auto* component = registry.get<VehicleComponent>(record->state.entity);
        if (component && component->vehicle == handle && component->chassis == record->state.chassis) {
            (void)registry.remove<VehicleComponent>(record->state.entity);
        }
        slots[static_cast<std::size_t>(handle.index - 1)].reset();
        (void)handles.release({handle.index, handle.generation});
    }

    bool create_input(input::InputSystem& source) {
        if (input && input != &source) { return false; }
        if (context != 0 && source.context(context)) { return true; }
        const auto created = source.create_context("vehicle-driving", 10000);
        if (created == 0) { return false; }
        struct ContextRollback {
            input::InputSystem& owner;
            std::uint64_t id;
            bool committed{};
            ~ContextRollback() { if (!committed) { (void)owner.remove_context(id); } }
        } rollback{source, created, false};
        bool valid = source.set_context_enabled(created, false) && source.set_context_blocking(created, true);
        const std::array<std::string, 4> names{
            "vehicle.throttle", "vehicle.steering", "vehicle.brake", "vehicle.handbrake"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            valid = source.define_action(created, {names[index], index < 2 ?
                input::ActionKind::Axis : input::ActionKind::Button}) && valid;
        }
        for (std::size_t index = 0; index < 8; ++index) {
            input::Binding binding;
            const std::size_t action = index % 4;
            binding.action = names[action];
            binding.kind = action < 2 ? input::ActionKind::Axis : input::ActionKind::Button;
            binding.response.deadzone = index < 4 ? 0.0 : 0.15;
            if (index < 2) {
                binding.control_count = 2;
                binding.controls[0] = input::key(static_cast<std::uint16_t>(index == 0 ? 83 : 65));
                binding.controls[1] = input::key(static_cast<std::uint16_t>(index == 0 ? 87 : 68));
            } else if (index < 4) {
                binding.controls[0] = input::key(static_cast<std::uint16_t>(index == 2 ? 32 : 16));
            } else if (index < 6) {
                binding.controls[0] = input::gamepad_axis(static_cast<std::uint16_t>(index == 4 ? 1 : 0));
                binding.response.invert = index == 4;
            } else {
                binding.controls[0] = input::gamepad_button(static_cast<std::uint16_t>(index == 6 ? 4 : 5));
            }
            valid = source.bind(created, std::move(binding)) != 0 && valid;
        }
        if (!valid) { return false; }
        input = &source;
        context = created;
        rollback.committed = true;
        return true;
    }

    void neutral_contact(WheelState& state, const WheelDescription& wheel) noexcept {
        state.collider = {};
        state.support_body = {};
        state.support = {};
        state.grounded = false;
        state.query_incomplete = false;
        state.suspension_length = wheel.rest_length + wheel.droop_travel;
        state.compression = -wheel.droop_travel;
        state.suspension_velocity = 0.0;
        state.normal_load = 0.0;
        state.longitudinal_speed = 0.0;
        state.lateral_speed = 0.0;
        state.longitudinal_slip = 0.0;
        state.slip_angle = 0.0;
        state.longitudinal_force = 0.0;
        state.lateral_force = 0.0;
        state.friction_utilization = 0.0;
    }

    const Collider* probe(Record& record, std::size_t index, const BodyState& body) {
        auto& state = record.wheels[index];
        const auto& wheel = record.description.wheels[index];
        const auto& tuning = record.description.tuning;
        const auto up = math::rotate(body.pose.rotation, {0.0, 1.0, 0.0});
        const auto steering = math::from_axis_angle({0.0, 1.0, 0.0}, -state.steering_angle);
        const auto forward = math::rotate(body.pose.rotation * steering, {0.0, 0.0, -1.0});
        const math::Ray ray{math::transform_point(body.pose, wheel.mount), -up};
        const double maximum = wheel.rest_length + wheel.droop_travel + wheel.radius;
        const auto previous_collider = state.collider;
        state.query_incomplete = false;
        if (statistics.queries >= limits.queries_per_frame) {
            neutral_contact(state, wheel);
            state.query_incomplete = true;
            ++statistics.incomplete_queries;
            record.state.last_error = VehicleError::QueryIncomplete;
            return nullptr;
        }
        ++statistics.queries;
        const math::Aabb bounds = math::expanded({math::min(ray.origin, ray.point(maximum)),
            math::max(ray.origin, ray.point(maximum))}, 0.001);
        const auto query = physics.collision().query(bounds, candidates, tuning.suspension_filter, record.state.entity);
        if (query.invalid_input || query.truncated) {
            neutral_contact(state, wheel);
            state.query_incomplete = true;
            ++statistics.incomplete_queries;
            record.state.last_error = VehicleError::QueryIncomplete;
            return nullptr;
        }
        const Collider* best{};
        std::optional<math::RayHit> nearest;
        for (std::size_t candidate = 0; candidate < query.written; ++candidate) {
            const auto* collider = physics.collision().get(candidates[candidate]);
            if (!collider || !collider->active || collider->description.trigger) { continue; }
            auto hit = std::visit([&](const auto& shape) {
                return math::intersect(ray, shape, 0.0, nearest ? nearest->distance : maximum);
            }, collider->world_shape);
            if (hit && std::holds_alternative<math::Triangle>(collider->world_shape) &&
                math::dot(hit->normal, up) < 0.0) { hit->normal = -hit->normal; }
            if (!hit || hit->inside || math::dot(hit->normal, up) < tuning.minimum_ground_alignment) { continue; }
            if (nearest && hit->distance == nearest->distance && best->handle < collider->handle) { continue; }
            nearest = hit;
            best = collider;
        }
        if (!best || !nearest) {
            neutral_contact(state, wheel);
            state.contact_point = ray.point(maximum);
            state.contact_normal = up;
            state.forward = forward;
            state.right = math::cross(forward, up);
            return nullptr;
        }
        state.grounded = true;
        state.collider = best->handle;
        state.support = best->entity;
        state.support_body = physics.find(best->entity);
        state.contact_point = nearest->point;
        state.contact_normal = nearest->normal;
        state.forward = math::normalized(math::reject(forward, nearest->normal), forward);
        state.right = math::normalized(math::cross(state.forward, nearest->normal));
        state.suspension_length = std::clamp(nearest->distance - wheel.radius,
            wheel.rest_length - wheel.compression_travel, wheel.rest_length + wheel.droop_travel);
        state.compression = wheel.rest_length - state.suspension_length;
        const auto relative = point_velocity(body, state.contact_point) -
            support_velocity(physics.get(state.support_body), state.contact_point);
        state.suspension_velocity = math::dot(relative, nearest->normal) / math::dot(up, nearest->normal);
        state.longitudinal_speed = math::dot(relative, state.forward);
        state.lateral_speed = math::dot(relative, state.right);
        state.longitudinal_slip = (state.angular_speed * wheel.radius - state.longitudinal_speed) /
            std::max({1.0, std::abs(state.longitudinal_speed), std::abs(state.angular_speed * wheel.radius)});
        state.slip_angle = std::atan2(state.lateral_speed, std::max(0.5, std::abs(state.longitudinal_speed)));
        if (previous_collider != state.collider) {
            state.normal_load = 0.0;
            state.longitudinal_force = 0.0;
            state.lateral_force = 0.0;
        }
        return best;
    }

    bool apply(Record& record, math::Vec3 force, math::Vec3 point, BodyHandle support = {}) {
        const auto can_apply = [&](const BodyState* body, math::Vec3 value) {
            return body && body->mode == BodyMode::Dynamic && bounded_vector(point, 1.0e12) &&
                bounded_vector(value, 1.0e12) && bounded_vector(body->force + value, 1.0e12) &&
                bounded_vector(body->torque + math::cross(point - center_of_mass(*body), value), 1.0e24);
        };
        const auto* ground = physics.get(support);
        const bool react = record.description.tuning.react_on_support && ground &&
            ground->mode == BodyMode::Dynamic && ground->enabled && ground->scene_enabled;
        if (!can_apply(physics.get(record.state.chassis), force) || (react && !can_apply(ground, -force))) {
            ++statistics.rejected_forces;
            record.state.last_error = VehicleError::ForceRejected;
            return false;
        }
        if (!physics.apply_force(record.state.chassis, force, point)) {
            ++statistics.rejected_forces;
            record.state.last_error = VehicleError::ForceRejected;
            return false;
        }
        if (react && !physics.apply_force(support, -force, point)) {
            (void)physics.apply_force(record.state.chassis, -force, point);
            ++statistics.rejected_forces;
            record.state.last_error = VehicleError::ForceRejected;
            return false;
        }
        return true;
    }

    void wheel_force(Record& record, std::size_t index, const BodyState& body, std::size_t driven) {
        auto& state = record.wheels[index];
        const auto& wheel = record.description.wheels[index];
        const auto& tuning = record.description.tuning;
        const auto* ground = probe(record, index, body);
        double drive = 0.0;
        if (record.state.drive_mode == VehicleDriveMode::WheelTorques) {
            drive = wheel.driven ? record.torques[index].drive : 0.0;
        } else if (wheel.driven && driven != 0) {
            drive = tuning.engine_torque * record.state.input.throttle / static_cast<double>(driven);
            if (drive < 0.0) { drive *= tuning.reverse_torque_factor; }
        }
        state.drive_torque = std::clamp(drive, -tuning.maximum_wheel_torque, tuning.maximum_wheel_torque);
        double brake = record.state.input.brake * wheel.maximum_brake_torque;
        if (wheel.handbrake) { brake += record.state.input.handbrake * wheel.handbrake_torque; }
        if (record.state.drive_mode == VehicleDriveMode::WheelTorques) { brake += record.torques[index].brake; }
        state.brake_torque = std::min(brake, tuning.maximum_wheel_torque);
        double resistance = wheel.bearing_drag * std::abs(state.angular_speed);
        state.longitudinal_force = 0.0;
        state.lateral_force = 0.0;
        state.friction_utilization = 0.0;
        if (ground) {
            const auto up = math::rotate(body.pose.rotation, {0.0, 1.0, 0.0});
            const auto mount = math::transform_point(body.pose, wheel.mount);
            const double raw_length = math::dot(state.contact_point - mount, -up) - wheel.radius;
            const double minimum_length = wheel.rest_length - wheel.compression_travel;
            const double damping = state.suspension_velocity < 0.0 ?
                wheel.compression_damping : wheel.rebound_damping;
            const double spring = wheel.spring_rate * state.compression +
                wheel.bump_stop_rate * std::max(0.0, minimum_length - raw_length);
            state.normal_load = std::clamp((spring - damping * state.suspension_velocity) /
                math::dot(state.contact_normal, up), 0.0, wheel.maximum_suspension_force);
            const auto* support = tuning.react_on_support ? physics.get(state.support_body) : nullptr;
            // Independent wheels share the chassis response budget, avoiding
            // repeated full-mass velocity cancellation at long frame deltas.
            const double response_share = static_cast<double>(record.wheels.size());
            const double longitudinal_mass = response_share * (effective_inverse_mass(&body, state.contact_point, state.forward) +
                effective_inverse_mass(support, state.contact_point, state.forward));
            const double lateral_mass = response_share * (effective_inverse_mass(&body, state.contact_point, state.right) +
                effective_inverse_mass(support, state.contact_point, state.right));
            resistance += wheel.rolling_resistance * state.normal_load * wheel.radius;
            const double total_brake = state.brake_torque + resistance;
            const double stiffness = wheel.longitudinal_stiffness;
            const double locked_force = -stiffness * state.longitudinal_speed /
                (1.0 + stiffness * delta * longitudinal_mass);
            const double brake_to_lock = state.angular_speed * wheel.inertia / delta +
                state.drive_torque - locked_force * wheel.radius;
            if (std::abs(brake_to_lock) <= total_brake) {
                state.longitudinal_force = locked_force;
            } else {
                const double free_torque = state.drive_torque - std::copysign(total_brake, brake_to_lock);
                const double predicted_slip = state.angular_speed * wheel.radius +
                    delta * free_torque * wheel.radius / wheel.inertia - state.longitudinal_speed;
                state.longitudinal_force = stiffness * predicted_slip / (1.0 + stiffness * delta *
                    (longitudinal_mass + wheel.radius * wheel.radius / wheel.inertia));
            }
            state.lateral_force = -wheel.lateral_stiffness * state.lateral_speed /
                (1.0 + wheel.lateral_stiffness * delta * lateral_mass);
            const double friction = std::min(10.0, std::sqrt(wheel.friction * ground->description.material.friction));
            const double capacity = friction * state.normal_load;
            const double demand = std::hypot(state.longitudinal_force, state.lateral_force);
            if (demand > capacity && demand > 0.0) {
                const double scale = capacity / demand;
                state.longitudinal_force *= scale;
                state.lateral_force *= scale;
            }
            state.friction_utilization = capacity > 0.0 ? std::min(1.0, demand / capacity) : 0.0;
            const auto force = state.contact_normal * state.normal_load +
                state.forward * state.longitudinal_force + state.right * state.lateral_force;
            if (!apply(record, force, state.contact_point, state.support_body)) {
                state.normal_load = 0.0;
                state.longitudinal_force = 0.0;
                state.lateral_force = 0.0;
                state.friction_utilization = 0.0;
                resistance = wheel.bearing_drag * std::abs(state.angular_speed);
            }
        }
        const double unbraked = state.angular_speed +
            delta * (state.drive_torque - wheel.radius * state.longitudinal_force) / wheel.inertia;
        state.angular_speed = std::clamp(math::move_towards(unbraked, 0.0,
            delta * (state.brake_torque + resistance) / wheel.inertia),
            -tuning.maximum_wheel_speed, tuning.maximum_wheel_speed);
    }

    void aerodynamic_force(Record& record, const BodyState& body) {
        const auto& tuning = record.description.tuning;
        const auto relative = body.linear_velocity - tuning.wind_velocity;
        const double speed = math::length(relative);
        record.state.aerodynamic_force = {};
        if (speed < 1.0e-8 || tuning.maximum_aero_force == 0.0) { return; }
        const double pressure_area = 0.5 * tuning.air_density * speed * speed * tuning.frontal_area;
        const double drag = std::min({pressure_area * tuning.drag_coefficient,
            tuning.maximum_aero_force, body.mass.mass * speed / delta});
        const double downforce = std::min(pressure_area * tuning.downforce_coefficient, tuning.maximum_aero_force);
        const auto down = math::rotate(body.pose.rotation, {0.0, -1.0, 0.0});
        const auto force = math::clamp_length(relative * (-drag / speed) + down * downforce,
            tuning.maximum_aero_force);
        if (apply(record, force, math::transform_point(body.pose, tuning.aerodynamic_center))) {
            record.state.aerodynamic_force = force;
        }
    }

    void publish_visual(Record& record, std::size_t index, const BodyState& body) {
        const auto& wheel = record.description.wheels[index];
        auto& state = record.wheels[index];
        state.visual_transform.position = math::transform_point(body.pose,
            wheel.mount + math::Vec3{0.0, -state.suspension_length, 0.0});
        state.visual_transform.rotation = math::normalized(body.pose.rotation *
            math::from_axis_angle({0.0, 1.0, 0.0}, -state.steering_angle) *
            math::from_axis_angle({1.0, 0.0, 0.0}, -state.rotation_angle) * wheel.visual_rotation);
        state.visual_transform.scale = wheel.visual_scale;
        if (wheel.visual && (!visual_allowed(wheel.visual, record.state.entity, record.state.handle, index) ||
            !physics.scene().set_world_transform(wheel.visual, state.visual_transform))) {
            ++statistics.rejected_visuals;
            record.state.last_error = VehicleError::InvalidVisual;
        }
    }
};

VehicleSystem::VehicleSystem(PhysicsWorld& physics, VehicleLimits limits)
    : impl_(std::make_unique<Impl>(physics, limits)) {}
VehicleSystem::~VehicleSystem() = default;
VehicleError VehicleSystem::last_error() const noexcept { return impl_->error; }
const VehicleLimits& VehicleSystem::limits() const noexcept { return impl_->limits; }
const VehicleStatistics& VehicleSystem::statistics() const noexcept { return impl_->statistics; }
VehicleHandle VehicleSystem::player() const noexcept {
    const auto* record = impl_->raw(impl_->human);
    return record && impl_->owned(*record) ? impl_->human : VehicleHandle{};
}

VehicleHandle VehicleSystem::attach(ecs::Entity entity, const VehicleDescription& description,
                                     VehicleAuthority authority) {
    auto& service = *impl_;
    const auto fail = [&](VehicleError error) { (void)service.reject(error); return VehicleHandle{}; };
    if (!service.editable()) { return {}; }
    if (!valid_vehicle(description)) { return fail(VehicleError::InvalidDescription); }
    if (!valid_authority(authority) || authority == VehicleAuthority::Human) { return fail(VehicleError::WrongAuthority); }
    if (!service.registry.alive(entity) || !service.physics.scene().contains(entity)) { return fail(VehicleError::InvalidEntity); }
    if (service.registry.has<VehicleComponent>(entity) || service.registry.has<CharacterComponent>(entity)) {
        return fail(VehicleError::AlreadyAttached);
    }
    const auto* body = service.physics.get(entity);
    if (!body || body->mode != BodyMode::Dynamic || !math::almost_equal(body->pose.scale, {1.0, 1.0, 1.0})) {
        return fail(VehicleError::InvalidBody);
    }
    Impl::Record record;
    record.description = description;
    record.wheels.resize(description.wheels.size());
    record.torques.resize(description.wheels.size());
    record.original_visuals.resize(description.wheels.size());
    for (std::size_t index = 0; index < description.wheels.size(); ++index) {
        const auto visual = description.wheels[index].visual;
        if (!service.visual_allowed(visual, entity)) { return fail(VehicleError::InvalidVisual); }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto other = description.wheels[previous].visual;
            if (visual && other && (visual == other || service.physics.scene().is_ancestor(visual, other) ||
                service.physics.scene().is_ancestor(other, visual))) { return fail(VehicleError::InvalidVisual); }
        }
        if (visual) { record.original_visuals[index] = service.physics.scene().local_transform(visual); }
        service.neutral_contact(record.wheels[index], description.wheels[index]);
    }
    const auto allocated = service.handles.acquire();
    if (!allocated) { return fail(VehicleError::Capacity); }
    const VehicleHandle handle{allocated->index, allocated->generation};
    record.state.handle = handle;
    record.state.entity = entity;
    record.state.chassis = body->handle;
    record.state.authority = authority;
    record.state.pose = body->pose;
    record.state.mass = body->mass.mass;
    record.state.linear_velocity = body->linear_velocity;
    record.state.angular_velocity = body->angular_velocity;
    try {
        if (service.slots.size() < handle.index) { service.slots.resize(handle.index); }
        service.slots[static_cast<std::size_t>(handle.index - 1)] = std::move(record);
        (void)service.registry.emplace<VehicleComponent>(entity, VehicleComponent{handle, body->handle});
    } catch (...) {
        if (service.slots.size() >= handle.index) { service.slots[static_cast<std::size_t>(handle.index - 1)].reset(); }
        (void)service.handles.release(*allocated);
        throw;
    }
    service.statistics.vehicles = service.handles.size();
    return handle;
}

bool VehicleSystem::detach(VehicleHandle vehicle) {
    if (!impl_->editable()) { return false; }
    if (!impl_->raw(vehicle)) { return impl_->reject(VehicleError::InvalidHandle); }
    impl_->erase(vehicle);
    impl_->statistics.vehicles = impl_->handles.size();
    return true;
}
bool VehicleSystem::detach(ecs::Entity entity) { return detach(find(entity)); }

bool VehicleSystem::clear() {
    if (!impl_->editable()) { return false; }
    for (auto& slot : impl_->slots) { if (slot) { impl_->erase(slot->state.handle); } }
    impl_->statistics = {};
    impl_->prepared = false;
    return true;
}

bool VehicleSystem::shutdown() {
    if (impl_->closed) { return true; }
    if (!clear()) { return false; }
    if (impl_->input && impl_->context != 0) { (void)impl_->input->remove_context(impl_->context); }
    impl_->input = nullptr;
    impl_->context = 0;
    impl_->closed = true;
    return true;
}

VehicleHandle VehicleSystem::find(ecs::Entity entity) const noexcept {
    if (impl_->closed) { return {}; }
    const auto* component = impl_->registry.get<VehicleComponent>(entity);
    const auto* record = component ? impl_->raw(component->vehicle) : nullptr;
    return record && record->state.entity == entity && impl_->owned(*record)
        ? record->state.handle : VehicleHandle{};
}

std::vector<VehicleHandle> VehicleSystem::vehicles() const {
    std::vector<VehicleHandle> result;
    result.reserve(impl_->handles.size());
    for (const auto& slot : impl_->slots) { if (slot && impl_->owned(*slot)) { result.push_back(slot->state.handle); } }
    return result;
}

std::optional<VehicleState> VehicleSystem::state(VehicleHandle vehicle) const noexcept {
    const auto* record = impl_->raw(vehicle);
    return record && impl_->owned(*record) ? std::optional<VehicleState>{record->state} : std::nullopt;
}

std::optional<VehicleDescription> VehicleSystem::description(VehicleHandle vehicle) const {
    const auto* record = impl_->raw(vehicle);
    return record && impl_->owned(*record) ? std::optional<VehicleDescription>{record->description} : std::nullopt;
}

std::span<const WheelState> VehicleSystem::wheels(VehicleHandle vehicle) const noexcept {
    const auto* record = impl_->raw(vehicle);
    return record && impl_->owned(*record) ? std::span<const WheelState>{record->wheels} : std::span<const WheelState>{};
}

bool VehicleSystem::set_input(VehicleHandle vehicle, DriveInput input) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    const auto bounded = bounded_drive_input(input);
    if (!bounded) { return impl_->reject(VehicleError::InvalidInput); }
    if (record->state.authority == VehicleAuthority::Human) { return impl_->reject(VehicleError::WrongAuthority); }
    record->state.input = *bounded;
    record->state.input_age = 0.0;
    record->state.input_expired = false;
    return true;
}
bool VehicleSystem::set_input(ecs::Entity entity, DriveInput input) { return set_input(find(entity), input); }

bool VehicleSystem::set_authority(VehicleHandle vehicle, VehicleAuthority authority) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (!valid_authority(authority) || authority == VehicleAuthority::Human) { return impl_->reject(VehicleError::WrongAuthority); }
    if (impl_->human == vehicle) { impl_->release_human(); }
    record->state.authority = authority;
    record->state.input = {};
    record->state.input_age = 0.0;
    std::fill(record->torques.begin(), record->torques.end(), WheelTorque{});
    return true;
}

bool VehicleSystem::set_enabled(VehicleHandle vehicle, bool enabled) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (!enabled && impl_->human == vehicle) { impl_->release_human(); }
    record->state.enabled = enabled;
    record->state.input = {};
    std::fill(record->torques.begin(), record->torques.end(), WheelTorque{});
    return true;
}

bool VehicleSystem::set_tuning(VehicleHandle vehicle, const VehicleTuning& tuning) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (!valid_tuning(tuning)) { return impl_->reject(VehicleError::InvalidDescription); }
    record->description.tuning = tuning;
    for (auto& wheel : record->wheels) {
        wheel.angular_speed = std::clamp(wheel.angular_speed, -tuning.maximum_wheel_speed, tuning.maximum_wheel_speed);
    }
    std::fill(record->torques.begin(), record->torques.end(), WheelTorque{});
    return true;
}

bool VehicleSystem::set_wheel(VehicleHandle vehicle, std::size_t index, const WheelDescription& wheel) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (index >= record->wheels.size()) { return impl_->reject(VehicleError::InvalidWheel); }
    if (!valid_wheel(wheel)) { return impl_->reject(VehicleError::InvalidDescription); }
    if (!impl_->visual_allowed(wheel.visual, record->state.entity, vehicle, index)) { return impl_->reject(VehicleError::InvalidVisual); }
    if (wheel.visual != record->description.wheels[index].visual) {
        const auto original = wheel.visual ? impl_->physics.scene().local_transform(wheel.visual) : std::nullopt;
        impl_->restore_visual(*record, index);
        record->original_visuals[index] = original;
    }
    record->description.wheels[index] = wheel;
    impl_->neutral_contact(record->wheels[index], wheel);
    record->torques[index] = {};
    return true;
}

bool VehicleSystem::set_chassis_mass(VehicleHandle vehicle, const MassProperties& mass) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (!impl_->physics.set_mass(record->state.chassis, mass)) { return impl_->reject(VehicleError::InvalidBody); }
    record->state.mass = mass.mass;
    return true;
}

bool VehicleSystem::set_drive_mode(VehicleHandle vehicle, VehicleDriveMode mode) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (mode != VehicleDriveMode::SimpleEngine && mode != VehicleDriveMode::WheelTorques) { return impl_->reject(VehicleError::InvalidInput); }
    record->state.drive_mode = mode;
    std::fill(record->torques.begin(), record->torques.end(), WheelTorque{});
    return true;
}

bool VehicleSystem::set_wheel_torque(VehicleHandle vehicle, std::size_t index, WheelTorque torque) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (index >= record->wheels.size()) { return impl_->reject(VehicleError::InvalidWheel); }
    const double maximum = record->description.tuning.maximum_wheel_torque;
    if (record->state.drive_mode != VehicleDriveMode::WheelTorques ||
        !range(torque.drive, -maximum, maximum) || !range(torque.brake, 0.0, maximum) ||
        (!record->description.wheels[index].driven && torque.drive != 0.0)) { return impl_->reject(VehicleError::InvalidInput); }
    record->torques[index] = torque;
    return true;
}

bool VehicleSystem::set_wheel_torques(VehicleHandle vehicle, std::span<const WheelTorque> torques) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (torques.size() != record->wheels.size()) { return impl_->reject(VehicleError::InvalidWheel); }
    if (record->state.drive_mode != VehicleDriveMode::WheelTorques) { return impl_->reject(VehicleError::InvalidInput); }
    const double maximum = record->description.tuning.maximum_wheel_torque;
    for (std::size_t index = 0; index < torques.size(); ++index) {
        if (!range(torques[index].drive, -maximum, maximum) || !range(torques[index].brake, 0.0, maximum) ||
            (!record->description.wheels[index].driven && torques[index].drive != 0.0)) { return impl_->reject(VehicleError::InvalidInput); }
    }
    std::copy(torques.begin(), torques.end(), record->torques.begin());
    return true;
}

bool VehicleSystem::clear_wheel_torques(VehicleHandle vehicle) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    std::fill(record->torques.begin(), record->torques.end(), WheelTorque{});
    return true;
}

bool VehicleSystem::select_player(VehicleHandle vehicle, input::InputSystem& input) {
    auto* record = impl_->edit(vehicle);
    if (!record) { return false; }
    if (!record->state.enabled || !impl_->create_input(input)) { return impl_->reject(VehicleError::InputUnavailable); }
    if (impl_->human == vehicle) {
        return input.set_context_enabled(impl_->context, true) || impl_->reject(VehicleError::InputUnavailable);
    }
    impl_->release_human();
    if (!input.set_context_enabled(impl_->context, true)) { return impl_->reject(VehicleError::InputUnavailable); }
    record->previous_authority = record->state.authority;
    record->state.authority = VehicleAuthority::Human;
    record->state.input = {};
    record->state.input_age = 0.0;
    std::fill(record->torques.begin(), record->torques.end(), WheelTorque{});
    impl_->human = vehicle;
    return true;
}

bool VehicleSystem::release_player() {
    if (!impl_->editable()) { return false; }
    impl_->release_human();
    return true;
}

void VehicleSystem::pre_step(double delta_seconds, std::uint64_t tick) {
    auto& service = *impl_;
    if (!service.editable()) { return; }
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) {
        service.statistics.invalid_delta = true;
        (void)service.reject(VehicleError::InvalidInput);
        return;
    }
    if (service.have_tick && service.tick == tick) {
        service.statistics.duplicate_tick = true;
        (void)service.reject(VehicleError::Busy);
        return;
    }
    BusyScope guard(service.busy);
    service.statistics = {};
    service.have_tick = true;
    service.prepared = true;
    service.tick = tick;
    service.physics_start = service.physics.statistics().steps;
    const auto& configuration = service.physics.config();
    service.delta = std::min({delta_seconds, configuration.max_frame_seconds,
        configuration.max_substep_seconds * static_cast<double>(configuration.max_substeps)});
    const auto synchronized = service.physics.collision().sync(service.physics.scene());
    if (synchronized.scene_busy) {
        service.prepared = false;
        (void)service.reject(VehicleError::Busy);
        return;
    }
    for (auto& slot : service.slots) {
        if (!slot) { continue; }
        if (!service.owned(*slot)) {
            service.erase(slot->state.handle);
            ++service.statistics.removed_stale;
            continue;
        }
        auto& record = *slot;
        const auto& tuning = record.description.tuning;
        const auto* body = service.physics.get(record.state.chassis);
        record.integrated = false;
        record.state.last_error = VehicleError::None;
        record.state.tick = tick;
        record.state.aerodynamic_force = {};
        record.initial_velocity = body->linear_velocity;
        service.statistics.wheels += record.wheels.size();
        const auto* matrix = service.physics.scene().cached_world_matrix(record.state.entity);
        const bool body_valid = body->mode == BodyMode::Dynamic &&
            math::almost_equal(body->pose.scale, {1.0, 1.0, 1.0}) && matrix &&
            math::almost_equal(*matrix, math::matrix(body->pose), 1.0e-8);
        if (!body_valid || !record.state.enabled || !body->enabled ||
            !service.physics.scene().effective_enabled(record.state.entity)) {
            for (std::size_t index = 0; index < record.wheels.size(); ++index) {
                service.neutral_contact(record.wheels[index], record.description.wheels[index]);
                record.wheels[index].drive_torque = 0.0;
                record.wheels[index].brake_torque = 0.0;
            }
            if (!body_valid) { record.state.last_error = VehicleError::InvalidBody; }
            std::fill(record.torques.begin(), record.torques.end(), WheelTorque{});
            continue;
        }
        if (record.state.authority == VehicleAuthority::Human) {
            DriveInput controls{0.0, 0.0, 1.0, 0.0};
            if (service.human == record.state.handle && service.input && service.input->snapshot().focused &&
                service.input->context(service.context) && service.input->context(service.context)->enabled) {
                controls = {service.input->axis("vehicle.throttle"), service.input->axis("vehicle.steering"),
                    service.input->axis("vehicle.brake"), service.input->axis("vehicle.handbrake")};
            } else {
                std::fill(record.torques.begin(), record.torques.end(), WheelTorque{});
            }
            record.state.input = bounded_drive_input(controls).value_or(DriveInput{0.0, 0.0, 1.0, 0.0});
            record.state.input_age = 0.0;
            record.state.input_expired = false;
        } else {
            record.state.input_age = std::min(1.0e9, record.state.input_age + service.delta);
            record.state.input_expired = record.state.authority == VehicleAuthority::AI &&
                tuning.ai_input_timeout > 0.0 && record.state.input_age > tuning.ai_input_timeout;
            if (record.state.input_expired) {
                record.state.input = {0.0, 0.0, 1.0, 0.0};
                std::fill(record.torques.begin(), record.torques.end(), WheelTorque{});
            }
        }
        if (service.delta <= 1.0e-8) { continue; }
        record.integrated = true;
        record.state.steering_angle = math::move_towards(record.state.steering_angle,
            record.state.input.steering * tuning.maximum_steering_angle, tuning.steering_rate * service.delta);
        const auto driven = static_cast<std::size_t>(std::count_if(record.description.wheels.begin(),
            record.description.wheels.end(), [](const WheelDescription& wheel) { return wheel.driven; }));
        for (std::size_t index = 0; index < record.wheels.size(); ++index) {
            record.previous_omega[index] = record.wheels[index].angular_speed;
            record.wheels[index].steering_angle = wheel_steering(record.description.wheels[index],
                tuning, record.state.steering_angle);
            service.wheel_force(record, index, *body, driven);
        }
        service.aerodynamic_force(record, *body);
        std::fill(record.torques.begin(), record.torques.end(), WheelTorque{});
    }
    service.statistics.vehicles = service.handles.size();
}

void VehicleSystem::post_step(std::uint64_t tick) {
    auto& service = *impl_;
    if (!service.editable() || !service.prepared || service.tick != tick) { return; }
    BusyScope guard(service.busy);
    service.prepared = false;
    const auto& physics_stats = service.physics.statistics();
    const double actual_delta = physics_stats.steps != service.physics_start ?
        std::min(service.delta, physics_stats.simulated_seconds) : 0.0;
    service.statistics.simulated_delta = actual_delta;
    service.statistics.grounded_wheels = 0;
    for (auto& slot : service.slots) {
        if (!slot) { continue; }
        if (!service.owned(*slot)) {
            service.erase(slot->state.handle);
            ++service.statistics.removed_stale;
            continue;
        }
        auto& record = *slot;
        const auto* body = service.physics.get(record.state.chassis);
        auto& state = record.state;
        state.pose = body->pose;
        state.mass = body->mass.mass;
        state.linear_velocity = body->linear_velocity;
        state.angular_velocity = body->angular_velocity;
        state.acceleration = actual_delta > 1.0e-8 ?
            (body->linear_velocity - record.initial_velocity) / actual_delta : math::Vec3{};
        state.forward_speed = math::dot(body->linear_velocity, math::rotate(body->pose.rotation, {0.0, 0.0, -1.0}));
        state.sideways_speed = math::dot(body->linear_velocity, math::rotate(body->pose.rotation, {1.0, 0.0, 0.0}));
        state.distance_travelled = std::min(1.0e15, state.distance_travelled + actual_delta *
            0.5 * (math::length(record.initial_velocity) + math::length(body->linear_velocity)));
        state.grounded_wheels = 0;
        for (std::size_t index = 0; index < record.wheels.size(); ++index) {
            auto& wheel = record.wheels[index];
            if (record.integrated) {
                const double ratio = service.delta > 0.0 ? actual_delta / service.delta : 0.0;
                wheel.angular_speed = math::lerp(record.previous_omega[index], wheel.angular_speed, ratio);
                wheel.rotation_angle = math::wrap_angle(wheel.rotation_angle +
                    actual_delta * 0.5 * (record.previous_omega[index] + wheel.angular_speed));
                (void)service.probe(record, index, *body);
            }
            state.grounded_wheels += wheel.grounded ? 1U : 0U;
            service.publish_visual(record, index, *body);
        }
        state.airborne_seconds = state.grounded_wheels != 0 ? 0.0 : std::min(1.0e9, state.airborne_seconds + actual_delta);
        service.statistics.grounded_wheels += state.grounded_wheels;
    }
    service.statistics.vehicles = service.handles.size();
}

namespace {

core::CommandResult command_result(bool success, const VehicleSystem& service) {
    return {success, success ? "vehicle updated" : std::string(describe(service.last_error()))};
}

core::CommandResult tune_vehicle(VehicleSystem& service, PhysicsWorld& physics, core::CommandArgs args) {
    if (args.size() != 3 && args.size() != 5) {
        return {false, "usage: vehicle.tune entity property value | entity wheel index property value"};
    }
    const auto handle = service.find(resolve_entity(physics, args[0]));
    auto description = service.description(handle);
    if (!description) { return {false, "entity has no owned vehicle"}; }
    if (args.size() == 3 && args[1] == "drive") {
        if (args[2] != "simple" && args[2] != "external") { return {false, "drive mode is simple or external"}; }
        return command_result(service.set_drive_mode(handle, args[2] == "simple" ?
            VehicleDriveMode::SimpleEngine : VehicleDriveMode::WheelTorques), service);
    }
    const auto value = number(args.back());
    if (!value) { return {false, "tuning value must be finite"}; }
    if (args.size() == 3 && args[1] == "mass") {
        auto mass = physics.get(resolve_entity(physics, args[0]))->mass;
        mass.inertia *= *value / mass.mass;
        mass.mass = *value;
        return command_result(service.set_chassis_mass(handle, mass), service);
    }
    if (args.size() == 3) {
        const std::array<std::pair<std::string_view, double VehicleTuning::*>, 9> fields{{
            {"engine", &VehicleTuning::engine_torque}, {"steering", &VehicleTuning::maximum_steering_angle},
            {"steering_rate", &VehicleTuning::steering_rate}, {"ackermann", &VehicleTuning::ackermann_blend},
            {"wheelbase", &VehicleTuning::wheelbase}, {"drag", &VehicleTuning::drag_coefficient},
            {"downforce", &VehicleTuning::downforce_coefficient}, {"area", &VehicleTuning::frontal_area},
            {"timeout", &VehicleTuning::ai_input_timeout}}};
        for (const auto& [name, member] : fields) {
            if (args[1] == name) {
                description->tuning.*member = *value;
                return command_result(service.set_tuning(handle, description->tuning), service);
            }
        }
    } else if (args[1] == "wheel") {
        const auto index = wheel_index(args[2]);
        if (!index || *index >= description->wheels.size()) { return {false, "wheel index is out of range"}; }
        auto& wheel = description->wheels[*index];
        const std::array<std::pair<std::string_view, double WheelDescription::*>, 11> fields{{
            {"radius", &WheelDescription::radius}, {"spring", &WheelDescription::spring_rate},
            {"compression_damping", &WheelDescription::compression_damping}, {"rebound_damping", &WheelDescription::rebound_damping},
            {"rest", &WheelDescription::rest_length}, {"compression", &WheelDescription::compression_travel},
            {"droop", &WheelDescription::droop_travel}, {"friction", &WheelDescription::friction},
            {"rolling", &WheelDescription::rolling_resistance}, {"brake", &WheelDescription::maximum_brake_torque},
            {"steering", &WheelDescription::steering_ratio}}};
        for (const auto& [name, member] : fields) {
            if (args[3] == name) {
                wheel.*member = *value;
                return command_result(service.set_wheel(handle, *index, wheel), service);
            }
        }
        if ((args[3] == "driven" || args[3] == "handbrake") && (*value == 0.0 || *value == 1.0)) {
            if (args[3] == "driven") { wheel.driven = *value == 1.0; } else { wheel.handbrake = *value == 1.0; }
            return command_result(service.set_wheel(handle, *index, wheel), service);
        }
    }
    return {false, "unknown tuning property"};
}

} // namespace

void register_vehicle(core::Kernel& kernel) {
    if (kernel.services().find<VehicleSystem>()) { return; }
    auto* physics = kernel.services().find<PhysicsWorld>();
    if (!physics) { throw std::logic_error("vehicle registration requires PhysicsWorld"); }
    VehicleLimits limits;
    limits.vehicles = static_cast<std::uint32_t>(std::clamp<std::int64_t>(kernel.config().integer("vehicle.capacity", 1024), 0, 16384));
    limits.query_candidates = static_cast<std::size_t>(std::clamp<std::int64_t>(kernel.config().integer("vehicle.query_candidates", 256), 1, 4096));
    limits.queries_per_frame = static_cast<std::size_t>(std::clamp<std::int64_t>(kernel.config().integer("vehicle.query_budget", 32768), 0, 1048576));
    auto& service = kernel.services().emplace<VehicleSystem>(*physics, limits);
    (void)kernel.register_module({"physics.vehicle", "Ray suspension, bounded tire forces and explicit driving authority", 1});
    (void)kernel.add_shutdown("physics.vehicle", [pointer = &service] { (void)pointer->shutdown(); });
    (void)kernel.add_system({"physics.vehicle.forces", core::Phase::PreSimulation, -6000, {"physics.collision"}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& vehicles = host.services().get<VehicleSystem>();
            const auto* characters = host.services().find<CharacterSystem>();
            if (vehicles.player() && characters && characters->player()) { (void)vehicles.release_player(); }
            if (host.config().boolean("physics.enabled", true)) { vehicles.pre_step(frame.delta_seconds, frame.tick); }
        });
    (void)kernel.add_system({"physics.vehicle.sample", core::Phase::PostSimulation, 1100, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& vehicles = host.services().get<VehicleSystem>();
            vehicles.post_step(frame.tick);
            const auto& stats = vehicles.statistics();
            host.metric("vehicle.count", static_cast<double>(stats.vehicles));
            host.metric("vehicle.grounded_wheels", static_cast<double>(stats.grounded_wheels));
            host.metric("vehicle.queries", static_cast<double>(stats.queries));
            host.metric("vehicle.incomplete_queries", static_cast<double>(stats.incomplete_queries));
            host.metric("vehicle.rejected_forces", static_cast<double>(stats.rejected_forces));
            host.metric("vehicle.rejected_visuals", static_cast<double>(stats.rejected_visuals));
        });
    (void)kernel.add_command("vehicle.attach", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2) { return {false, "usage: vehicle.attach entity [rear|front|all|remove]"}; }
        auto& vehicles = host.services().get<VehicleSystem>();
        auto& world = host.services().get<PhysicsWorld>();
        const auto entity = resolve_entity(world, args[0]);
        if (args.size() == 2 && args[1] == "remove") { return command_result(vehicles.detach(entity), vehicles); }
        auto description = four_wheel_vehicle();
        if (args.size() == 2) {
            if (args[1] != "rear" && args[1] != "front" && args[1] != "all") { return {false, "drive layout is rear, front, or all"}; }
            for (std::size_t index = 0; index < description.wheels.size(); ++index) {
                description.wheels[index].driven = args[1] == "all" || (args[1] == "front" ? index < 2 : index >= 2);
            }
        }
        return command_result(static_cast<bool>(vehicles.attach(entity, description)), vehicles);
    }, "Attach four configurable wheels to an existing dynamic chassis; no body or mesh is created");
    (void)kernel.add_command("vehicle.control", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2 && args.size() != 5) { return {false, "usage: vehicle.control entity human|manual|ai|on|off | entity throttle steering brake handbrake"}; }
        auto& vehicles = host.services().get<VehicleSystem>();
        const auto handle = vehicles.find(resolve_entity(host.services().get<PhysicsWorld>(), args[0]));
        if (args.size() == 2) {
            if (args[1] == "human") {
                const auto* character = host.services().find<CharacterSystem>();
                if (character && character->player()) { return {false, "release character player authority before driving"}; }
                auto* input = host.services().find<input::InputSystem>();
                return input ? command_result(vehicles.select_player(handle, *input), vehicles) :
                    core::CommandResult{false, "InputSystem is unavailable"};
            }
            if (args[1] == "manual" || args[1] == "ai") {
                return command_result(vehicles.set_authority(handle, args[1] == "ai" ? VehicleAuthority::AI : VehicleAuthority::Manual), vehicles);
            }
            if (args[1] == "on" || args[1] == "off") { return command_result(vehicles.set_enabled(handle, args[1] == "on"), vehicles); }
            return {false, "unknown authority or enable state"};
        }
        const auto throttle = number(args[1]);
        const auto steering = number(args[2]);
        const auto brake = number(args[3]);
        const auto handbrake = number(args[4]);
        if (!throttle || !steering || !brake || !handbrake) { return {false, "all controls must be finite numbers"}; }
        return command_result(vehicles.set_input(handle, {*throttle, *steering, *brake, *handbrake}), vehicles);
    }, "Select explicit human/manual/AI authority; human keys are W/S, A/D, Space brake, Shift handbrake");
    (void)kernel.add_command("vehicle.tune", [](core::Kernel& host, core::CommandArgs args) {
        return tune_vehicle(host.services().get<VehicleSystem>(), host.services().get<PhysicsWorld>(), args);
    }, "Tune mass, engine, steering, aero, AI timeout, drive mode, or an indexed wheel; angles are radians");
    (void)kernel.add_command("vehicle.state", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& vehicles = host.services().get<VehicleSystem>();
        if (args.empty()) {
            const auto& stats = vehicles.statistics();
            return {true, "vehicles=" + std::to_string(stats.vehicles) + " wheels=" + std::to_string(stats.wheels) +
                " grounded=" + std::to_string(stats.grounded_wheels) + " incomplete_queries=" + std::to_string(stats.incomplete_queries)};
        }
        if (args.size() > 2) { return {false, "usage: vehicle.state [entity [wheel_index]]"}; }
        const auto handle = vehicles.find(resolve_entity(host.services().get<PhysicsWorld>(), args[0]));
        const auto state = vehicles.state(handle);
        if (!state) { return {false, "entity has no owned vehicle"}; }
        if (args.size() == 2) {
            const auto index = wheel_index(args[1]);
            const auto wheels = vehicles.wheels(handle);
            if (!index || *index >= wheels.size()) { return {false, "wheel index is out of range"}; }
            const auto& wheel = wheels[*index];
            return {true, "grounded=" + std::to_string(wheel.grounded) + " travel=" + std::to_string(wheel.suspension_length) +
                " omega=" + std::to_string(wheel.angular_speed) + " load=" + std::to_string(wheel.normal_load) +
                " slip=" + std::to_string(wheel.longitudinal_slip) + " lateral=" + std::to_string(wheel.lateral_speed) +
                " longitudinal_force=" + std::to_string(wheel.longitudinal_force) + " lateral_force=" + std::to_string(wheel.lateral_force)};
        }
        return {true, "vehicle=" + std::to_string(handle.index) + ":" + std::to_string(handle.generation) +
            " speed=" + std::to_string(state->forward_speed) + " grounded=" + std::to_string(state->grounded_wheels) +
            " mass=" + std::to_string(state->mass) + " distance=" + std::to_string(state->distance_travelled) +
            " expired=" + std::to_string(state->input_expired) + " error=" + std::string(describe(state->last_error))};
    }, "Inspect vehicle motion, suspension travel, tire slip, loads, and bounded query diagnostics");
}

} // namespace ge::physics
