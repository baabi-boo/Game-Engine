#include <ge/physics/character.hpp>

#include <ge/core/runtime.hpp>
#include <ge/input/actions.hpp>
#include <ge/navigation/pathfinding.hpp>
#include <ge/physics/vehicle.hpp>
#include <ge/render/camera.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace ge::physics {
namespace {

constexpr math::Vec3 up{0.0, 1.0, 0.0};
constexpr math::Vec3 forward{0.0, 0.0, -1.0};
constexpr std::string_view upright_hook = "physics.character.upright";

bool range(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool valid_authority(CharacterAuthority authority) noexcept {
    return authority >= CharacterAuthority::Manual && authority <= CharacterAuthority::Navigation;
}

math::Capsule capsule(const CharacterController& settings, double height) noexcept {
    return {{0.0, settings.radius, 0.0}, {0.0, height - settings.radius, 0.0}, settings.radius};
}

std::optional<MassProperties> character_mass(const CharacterController& settings, double height) noexcept {
    auto result = box_mass(settings.mass, {settings.radius, height * 0.5, settings.radius});
    if (result) {
        result->center_of_mass = {0.0, height * 0.5, 0.0};
    }
    return result;
}

bool unit_scale(const math::Transform& pose) noexcept {
    return math::finite(pose) && math::almost_equal(pose.scale, {1.0, 1.0, 1.0}, 1.0e-8);
}

std::optional<double> number(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) {
        return std::nullopt;
    }
    double result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

ecs::Entity resolve(PhysicsWorld& physics, std::string_view value) {
    if (const auto parsed = ecs::parse_entity(value); parsed && physics.registry().alive(*parsed)) {
        return *parsed;
    }
    const auto path = physics.scene().find_path(value);
    if (path) {
        return path.entity;
    }
    const auto named = physics.registry().find_all_named(value);
    return named.size() == 1 ? named.front() : ecs::Entity{};
}

std::optional<CharacterAuthority> authority(std::string_view value) noexcept {
    if (value == "manual") { return CharacterAuthority::Manual; }
    if (value == "player") { return CharacterAuthority::Player; }
    if (value == "navigation") { return CharacterAuthority::Navigation; }
    return std::nullopt;
}

bool vehicle_selected(core::Kernel& kernel) noexcept {
    const auto* vehicles = kernel.services().find<VehicleSystem>();
    return vehicles && static_cast<bool>(vehicles->player());
}

} // namespace

std::string_view describe(CharacterError error) noexcept {
    switch (error) {
    case CharacterError::None: return "none";
    case CharacterError::InvalidEntity: return "invalid or unscaled Scene entity";
    case CharacterError::InvalidSettings: return "invalid character settings";
    case CharacterError::InvalidInput: return "invalid movement input";
    case CharacterError::AlreadyOwned: return "entity already owns physical components";
    case CharacterError::Capacity: return "character capacity exhausted";
    case CharacterError::MissingCharacter: return "missing character";
    case CharacterError::StaleOwnership: return "physical component ownership changed";
    case CharacterError::Busy: return "physics or Registry is busy";
    case CharacterError::BodyRejected: return "physics body write rejected";
    case CharacterError::ColliderRejected: return "capsule collider write rejected";
    case CharacterError::QueryBudget: return "character query budget exhausted";
    case CharacterError::QueryOverflow: return "collision query incomplete";
    case CharacterError::UnsupportedGeometry: return "capsule query encountered unsupported geometry";
    case CharacterError::ClearanceBlocked: return "standing clearance blocked";
    case CharacterError::CameraRejected: return "player camera rejected";
    case CharacterError::WrongAuthority: return "movement requires manual authority";
    }
    return "unknown character error";
}

std::string_view describe(CharacterMotion motion) noexcept {
    switch (motion) {
    case CharacterMotion::Disabled: return "disabled";
    case CharacterMotion::Grounded: return "grounded";
    case CharacterMotion::Rising: return "rising";
    case CharacterMotion::Falling: return "falling";
    case CharacterMotion::Sliding: return "sliding";
    }
    return "unknown";
}

bool valid_character(const CharacterController& value) noexcept {
    const std::array<double, 5> speeds{value.walk_speed, value.sprint_speed, value.crouch_speed,
                                      value.jump_speed, value.ground_stick_speed};
    const std::array<double, 4> accelerations{value.ground_acceleration, value.ground_deceleration,
                                             value.air_acceleration, value.air_deceleration};
    return range(value.radius, 0.01, 50.0) && range(value.standing_height, value.radius * 2.0, 100.0) &&
        range(value.crouching_height, value.radius * 2.0, value.standing_height) &&
        range(value.mass, 0.001, 1.0e6) &&
        std::all_of(speeds.begin(), speeds.end(), [](double item) { return range(item, 0.0, 1000.0); }) &&
        std::all_of(accelerations.begin(), accelerations.end(), [](double item) { return range(item, 0.0, 1.0e5); }) &&
        range(value.released_jump_multiplier, 0.0, 1.0) && range(value.jump_buffer_seconds, 0.0, 2.0) &&
        range(value.coyote_seconds, 0.0, 2.0) && range(value.jump_support_delay, 0.0, 2.0) &&
        range(value.gravity_scale, 0.0, 100.0) && range(value.falling_gravity_multiplier, 0.0, 10.0) &&
        range(value.terminal_speed, 0.01, 1000.0) && range(value.maximum_slope, 0.0, math::radians(85.0)) &&
        range(value.skin, 1.0e-6, value.radius * 0.2) && range(value.ground_probe, value.skin, value.radius) &&
        range(value.ground_snap, value.ground_probe, value.standing_height * 0.5) &&
        range(value.step_height, 0.0, value.standing_height * 0.5) &&
        range(value.maximum_assist_distance, 0.0, value.radius) &&
        range(value.maximum_depenetration, 0.0, value.radius) && range(value.turn_speed, 0.0, 100.0);
}

math::Vec3 character_direction(math::Vec2 movement, math::Vec3 camera_forward) noexcept {
    if (!math::finite(movement) || !math::finite(camera_forward)) {
        return {};
    }
    const double magnitude = std::hypot(movement.x, movement.y);
    if (magnitude > 1.0) {
        movement /= magnitude;
    }
    const auto facing = math::normalized(math::Vec3{camera_forward.x, 0.0, camera_forward.z}, forward);
    const auto right = math::cross(facing, up);
    return right * movement.x + facing * movement.y;
}

struct CharacterSystem::Impl {
    struct Record {
        ecs::Entity entity{};
        BodyHandle body{};
        ColliderHandle collider{};
        CharacterController settings{};
        CharacterInput input{};
        CharacterState state{};
        math::Quat upright{};
        math::Vec3 previous_position{};
        math::Vec3 steep_normal{};
        double support_delay{};
        double launch_support_y{};
        bool jump_requested{};
        bool jump_held{};
        bool jump_active{};
        bool jump_cut{};
        bool crouch_requested{};
        bool stand_reported{};
    };
    struct Probe {
        CharacterSupport support{};
        math::Vec3 correction_normal{};
        double penetration{};
        bool blocked{};
        bool complete{true};
    };

    PhysicsWorld& physics;
    CollisionWorld& collision;
    ecs::Registry& registry;
    CharacterLimits limits;
    CharacterStatistics statistics{};
    std::map<ecs::Entity, Record> records;
    std::vector<ColliderHandle> candidates;
    std::vector<ContactManifold> contacts;
    std::vector<CharacterEvent> events;
    ecs::Entity selected{};
    ecs::Entity camera_entity{};
    render::CameraService* camera_owner{};
    std::optional<render::CameraController> previous_camera;
    std::uint64_t tick{};
    bool stopped{};

    Impl(PhysicsWorld& world, CharacterLimits configuration)
        : physics(world), collision(world.collision()), registry(world.registry()), limits(configuration) {
        if (&collision.registry() != &registry || limits.characters > 65536 || limits.events > 1048576 ||
            limits.query_candidates == 0 || limits.query_candidates > 4096 || limits.contacts > 1048576 ||
            limits.queries_per_frame == 0 || limits.queries_per_frame > 1048576 ||
            limits.assist_samples == 0 || limits.assist_samples > 64 || limits.depenetration_iterations > 16) {
            throw std::invalid_argument("invalid character limits or shared Registry");
        }
        candidates.resize(limits.query_candidates);
        contacts.reserve(limits.contacts);
        events.reserve(limits.events);
        (void)registry.register_component<CharacterComponent>("physics.character");
    }

    bool busy() const noexcept {
        return stopped || physics.stepping() || registry.iterating();
    }

    bool owned(const Record& record) const noexcept {
        const auto* component = registry.get<CharacterComponent>(record.entity);
        const auto* body = physics.get(record.body);
        const auto* collider = collision.get(record.collider);
        return component && component->body == record.body && component->collider == record.collider &&
            body && body->entity == record.entity && body->mode == BodyMode::Dynamic && unit_scale(body->pose) &&
            collider && collider->entity == record.entity && !collider->description.trigger &&
            std::holds_alternative<math::Capsule>(collider->description.shape);
    }

    Record* find(ecs::Entity entity) noexcept {
        const auto found = records.find(entity);
        return found != records.end() && owned(found->second) ? &found->second : nullptr;
    }

    void publish(Record& record) {
        auto* component = registry.get<CharacterComponent>(record.entity);
        if (component && component->body == record.body && component->collider == record.collider) {
            component->controller = record.settings;
            component->state = record.state;
            (void)registry.mark_changed<CharacterComponent>(record.entity);
        }
    }

    void emit(const Record& record, CharacterEventKind kind, double value = 0.0) {
        if (events.size() >= limits.events) {
            ++statistics.dropped_events;
            return;
        }
        events.push_back({kind, record.entity, record.state.support.entity, record.state.position,
                          record.state.support.normal, value, tick});
    }

    bool release_player() {
        if (camera_owner && previous_camera) {
            const auto* component = camera_owner->component(camera_entity);
            if (component && component->controller.mode == render::CameraMode::Follow &&
                component->controller.target == selected) {
                (void)camera_owner->set_controller(camera_entity, *previous_camera);
            }
        }
        if (auto* record = find(selected)) {
            record->state.authority = CharacterAuthority::Manual;
            record->input = {};
            publish(*record);
        }
        const bool had_player = static_cast<bool>(selected);
        selected = {};
        camera_entity = {};
        camera_owner = nullptr;
        previous_camera.reset();
        return had_player;
    }

    void erase(std::map<ecs::Entity, Record>::iterator found, CharacterEventKind event) {
        auto& record = found->second;
        if (selected == record.entity) {
            (void)release_player();
        }
        emit(record, event);
        const auto* component = registry.get<CharacterComponent>(record.entity);
        if (component && component->body == record.body && component->collider == record.collider) {
            (void)registry.remove<CharacterComponent>(record.entity);
        }
        (void)collision.remove(record.collider);
        (void)physics.remove(record.body);
        records.erase(found);
        statistics.characters = records.size();
    }

    void copy_contacts() {
        const auto borrowed = collision.contacts();
        const auto count = std::min(borrowed.size(), limits.contacts);
        contacts.assign(borrowed.begin(), borrowed.begin() + static_cast<std::ptrdiff_t>(count));
        statistics.contacts_truncated = count != borrowed.size() || collision.statistics().contacts_truncated ||
            collision.statistics().pairs_truncated;
    }

    CharacterSupport support(ecs::Entity entity, ColliderHandle collider, math::Vec3 normal,
                             math::Vec3 point, double penetration) const noexcept {
        CharacterSupport result;
        result.entity = entity;
        result.collider = collider;
        result.normal = normal;
        result.point = point;
        result.penetration = penetration;
        result.valid = true;
        if (const auto* body = physics.get(entity); body && body->enabled && body->scene_enabled) {
            result.velocity = math::clamp_length(point_velocity(*body, point), physics.config().max_linear_speed);
        }
        return result;
    }

    void consider_support(Record& record, CharacterSupport& best, ecs::Entity entity,
                          ColliderHandle collider, math::Vec3 normal, math::Vec3 point,
                          double penetration, double foot_y) const noexcept {
        if (normal.y < std::cos(record.settings.maximum_slope) ||
            point.y > foot_y + record.settings.radius + record.settings.skin ||
            !math::finite(normal) || !math::finite(point) || !std::isfinite(penetration)) {
            return;
        }
        if (!best.valid || normal.y > best.normal.y + 1.0e-6 ||
            (math::almost_equal(normal.y, best.normal.y) && penetration > best.penetration)) {
            best = support(entity, collider, normal, point, penetration);
        }
    }

    Probe probe(Record& record, const math::Transform& pose, double height) {
        Probe result;
        auto fail = [&](CharacterError error) {
            result.complete = false;
            result.blocked = true;
            record.state.query_incomplete = true;
            record.state.last_error = error;
            ++statistics.incomplete_queries;
        };
        if (statistics.queries >= limits.queries_per_frame) {
            fail(CharacterError::QueryBudget);
            return result;
        }
        ++statistics.queries;
        const auto shape = transform_shape(capsule(record.settings, height), pose);
        const auto bounds = shape ? shape_bounds(*shape) : std::nullopt;
        if (!bounds) {
            fail(CharacterError::InvalidInput);
            return result;
        }
        const auto query = collision.query(math::expanded(*bounds, record.settings.skin), candidates,
                                            record.settings.filter, record.entity);
        if (query.invalid_input || query.truncated) {
            fail(CharacterError::QueryOverflow);
            return result;
        }
        for (std::size_t index = 0; index < query.written; ++index) {
            const auto* other = collision.get(candidates[index]);
            if (!other || !other->active || other->description.trigger) {
                continue;
            }
            if (std::holds_alternative<math::Triangle>(other->world_shape)) {
                if (other->bounds && math::overlaps(*bounds, *other->bounds)) {
                    fail(CharacterError::UnsupportedGeometry);
                }
                continue;
            }
            const auto contact = narrowphase(*shape, other->world_shape, collision.config().contact_tolerance);
            if (!contact.touching()) {
                continue;
            }
            const auto normal = -contact.normal;
            for (std::size_t point_index = 0; point_index < std::min(contact.point_count, contact.points.size()); ++point_index) {
                const auto& point = contact.points[point_index];
                consider_support(record, result.support, other->entity, other->handle, normal,
                                 point.point_b, point.penetration, pose.position.y);
                if (point.penetration > result.penetration) {
                    result.penetration = point.penetration;
                    result.correction_normal = normal;
                }
                result.blocked = result.blocked || point.penetration > record.settings.skin;
            }
        }
        return result;
    }

    CharacterSupport contact_support(Record& record, const BodyState& body) {
        CharacterSupport result;
        for (const auto& contact : contacts) {
            const bool first = contact.first == record.collider;
            if ((!first && contact.second != record.collider) || contact.trigger) {
                continue;
            }
            const auto other_handle = first ? contact.second : contact.first;
            const auto* other = collision.get(other_handle);
            if (!other || !other->active || other->description.trigger) {
                continue;
            }
            const auto normal = first ? -contact.normal : contact.normal;
            for (std::size_t index = 0; index < std::min(contact.point_count, contact.points.size()); ++index) {
                const auto& point = contact.points[index];
                const auto position = first ? point.point_b : point.point_a;
                consider_support(record, result, other->entity, other_handle, normal, position,
                                 point.penetration, body.pose.position.y);
                if (normal.y < -0.5 && position.y > body.pose.position.y + record.state.height * 0.5) {
                    record.state.ceiling_blocked = true;
                }
                if (normal.y > 0.0 && normal.y < std::cos(record.settings.maximum_slope)) {
                    record.steep_normal = normal;
                    record.state.motion = CharacterMotion::Sliding;
                }
            }
        }
        return result;
    }

    CharacterSupport detect_support(Record& record, const BodyState& body, bool use_contacts) {
        auto result = use_contacts ? contact_support(record, body) : CharacterSupport{};
        if (record.support_delay > 0.0) {
            return {};
        }
        if (result.valid && math::dot(body.linear_velocity - result.velocity, result.normal) <= 1.0) {
            return result;
        }
        const double distance = record.state.grounded ? record.settings.ground_snap : record.settings.ground_probe;
        auto pose = body.pose;
        pose.position.y -= distance;
        const auto tested = probe(record, pose, record.state.height);
        result = tested.support;
        if (!tested.complete || !result.valid || math::dot(body.linear_velocity - result.velocity, result.normal) > 1.0) {
            return {};
        }
        result.distance = std::max(0.0, distance - result.penetration / result.normal.y);
        if (result.penetration > distance + record.settings.maximum_depenetration + record.settings.skin) {
            return {};
        }
        return result;
    }

    void grounding(Record& record, CharacterSupport next) {
        const bool was_grounded = record.state.grounded;
        if (next.valid) {
            record.state.support = next;
            record.state.grounded = true;
            record.state.motion = CharacterMotion::Grounded;
            record.state.coyote_remaining = record.settings.coyote_seconds;
            record.state.airborne_seconds = 0.0;
            record.jump_active = false;
            if (!was_grounded) {
                record.state.landing_speed = std::max(0.0, -record.state.vertical_speed + next.velocity.y);
                emit(record, CharacterEventKind::Landed, record.state.landing_speed);
            }
        } else {
            if (was_grounded) { emit(record, CharacterEventKind::LeftGround); }
            record.state.support = {};
            record.state.grounded = false;
            if (record.state.vertical_speed > 0.0 || record.state.motion != CharacterMotion::Sliding) {
                record.state.motion = record.state.vertical_speed > 0.0 ? CharacterMotion::Rising : CharacterMotion::Falling;
            }
        }
    }

    bool write_pose(Record& record, const math::Transform& pose) {
        if (!physics.set_pose(record.body, pose)) {
            record.state.last_error = CharacterError::BodyRejected;
            ++statistics.rejected_writes;
            return false;
        }
        record.state.position = pose.position;
        return true;
    }

    void stance(Record& record, bool crouched) {
        if (crouched == record.state.crouched) {
            if (crouched) { record.stand_reported = false; }
            return;
        }
        const double height = crouched ? record.settings.crouching_height : record.settings.standing_height;
        const auto body = physics.state(record.body);
        if (!body) { return; }
        if (!crouched) {
            const auto clearance = probe(record, body->pose, height);
            if (!clearance.complete || clearance.blocked) {
                record.state.ceiling_blocked = true;
                if (clearance.complete) { record.state.last_error = CharacterError::ClearanceBlocked; }
                if (!record.stand_reported) { emit(record, CharacterEventKind::StandBlocked); }
                record.stand_reported = true;
                return;
            }
        }
        const auto mass = character_mass(record.settings, height);
        if (!mass || !collision.set_shape(record.collider, capsule(record.settings, height))) {
            record.state.last_error = CharacterError::ColliderRejected;
            ++statistics.rejected_writes;
            return;
        }
        if (!physics.set_mass(record.body, *mass)) {
            (void)collision.set_shape(record.collider, capsule(record.settings, record.state.height));
            record.state.last_error = CharacterError::BodyRejected;
            ++statistics.rejected_writes;
            return;
        }
        record.state.height = height;
        record.state.crouched = crouched;
        record.stand_reported = false;
        emit(record, crouched ? CharacterEventKind::Crouched : CharacterEventKind::Stood);
    }

    bool clear_path(Record& record, const math::Transform& start, math::Vec3 displacement) {
        const double length = math::length(displacement);
        const double stride = record.settings.radius * 0.5;
        const double required = std::max(1.0, std::ceil(length / stride));
        if (!std::isfinite(required) || required > static_cast<double>(limits.assist_samples)) {
            record.state.query_incomplete = true;
            record.state.last_error = CharacterError::QueryBudget;
            ++statistics.incomplete_queries;
            return false;
        }
        const auto count = static_cast<std::uint32_t>(required);
        for (std::uint32_t index = 1; index <= count; ++index) {
            auto pose = start;
            pose.position += displacement * (static_cast<double>(index) / static_cast<double>(count));
            const auto tested = probe(record, pose, record.state.height);
            if (!tested.complete || tested.blocked) { return false; }
        }
        return true;
    }

    bool depenetrate(Record& record) {
        double remaining = record.settings.maximum_depenetration;
        bool changed = false;
        for (std::uint32_t iteration = 0; iteration < limits.depenetration_iterations && remaining > 0.0; ++iteration) {
            const auto body = physics.state(record.body);
            if (!body) { break; }
            const auto current = probe(record, body->pose, record.state.height);
            if (!current.complete || !current.blocked || current.penetration <= record.settings.skin) { break; }
            const double distance = std::min(remaining, current.penetration - record.settings.skin);
            auto candidate = body->pose;
            candidate.position += current.correction_normal * distance;
            const auto destination = probe(record, candidate, record.state.height);
            if (!destination.complete || destination.penetration >= current.penetration - 1.0e-8) { break; }
            if (!write_pose(record, candidate)) { break; }
            changed = true;
            remaining -= distance;
        }
        if (changed) { emit(record, CharacterEventKind::Depenetrated, record.settings.maximum_depenetration - remaining); }
        return changed;
    }

    void step_assist(Record& record, math::Vec3 desired, double delta) {
        if (!record.state.grounded || record.settings.step_height == 0.0 || record.support_delay > 0.0) { return; }
        desired.y = 0.0;
        const auto travel = math::clamp_length(desired * delta, record.settings.maximum_assist_distance);
        if (math::length_squared(travel) <= record.settings.skin * record.settings.skin) { return; }
        const auto body = physics.state(record.body);
        if (!body) { return; }
        auto destination = body->pose;
        destination.position += travel;
        const auto obstacle = probe(record, destination, record.state.height);
        if (!obstacle.complete || !obstacle.blocked) { return; }
        record.state.movement_blocked = true;
        auto raised = body->pose;
        raised.position.y += record.settings.step_height + record.settings.skin;
        if (!clear_path(record, body->pose, raised.position - body->pose.position) || !clear_path(record, raised, travel)) { return; }
        raised.position += travel;
        const double drop = record.settings.step_height + record.settings.ground_snap;
        const double required = std::ceil(drop / (record.settings.radius * 0.5));
        if (required > static_cast<double>(limits.assist_samples)) { return; }
        const auto count = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(required));
        for (std::uint32_t index = 1; index <= count; ++index) {
            auto landing = raised;
            landing.position.y -= drop * (static_cast<double>(index) / static_cast<double>(count));
            const auto tested = probe(record, landing, record.state.height);
            if (!tested.complete) { return; }
            if (!tested.support.valid) {
                if (tested.blocked) { return; }
                continue;
            }
            landing.position.y += tested.support.penetration / tested.support.normal.y + record.settings.skin;
            const double lift = landing.position.y - body->pose.position.y;
            if (lift <= record.settings.skin || lift > record.settings.step_height + record.settings.skin * 2.0) { return; }
            auto lifted = body->pose;
            lifted.position.y += lift;
            if (!clear_path(record, lifted, travel) || !write_pose(record, lifted)) { return; }
            record.state.movement_blocked = false;
            emit(record, CharacterEventKind::Stepped, lift);
            return;
        }
    }

    math::Vec3 desired(Record& record, const CharacterInput& controls) const noexcept {
        if (record.state.authority == CharacterAuthority::Navigation) {
            const auto* follower = registry.get<navigation::PathFollower>(record.entity);
            if (!follower || follower->pause || !math::finite(follower->desired_velocity) ||
                (follower->state != navigation::FollowerState::Following &&
                 follower->state != navigation::FollowerState::ControllerRequired)) { return {}; }
            auto velocity = follower->desired_velocity;
            velocity.y = 0.0;
            return math::clamp_length(velocity, record.state.crouched ? record.settings.crouch_speed : record.settings.sprint_speed);
        }
        const double speed = record.state.crouched ? record.settings.crouch_speed :
            (controls.sprint ? record.settings.sprint_speed : record.settings.walk_speed);
        return character_direction(controls.movement, controls.forward) * speed;
    }

    void move(Record& record, const CharacterInput& controls, double delta) {
        auto body = physics.state(record.body);
        if (!body) { return; }
        auto wanted = desired(record, controls);
        if (!record.state.grounded && record.steep_normal.y > 0.0) {
            const auto outward = math::normalized(math::Vec3{record.steep_normal.x, 0.0, record.steep_normal.z});
            wanted -= outward * std::min(0.0, math::dot(wanted, outward));
        }
        record.state.desired_velocity = wanted;
        auto velocity = body->linear_velocity;
        const auto platform = record.state.grounded && record.settings.inherit_support_velocity ? record.state.support.velocity : math::Vec3{};
        const bool moving = math::length_squared(wanted) > 1.0e-12;
        if (record.state.grounded) {
            const auto normal = record.state.support.normal;
            wanted = math::normalized(math::reject(wanted, normal)) * math::length(wanted);
            const auto tangent = math::reject(velocity - platform, normal);
            const double acceleration = moving ? record.settings.ground_acceleration : record.settings.ground_deceleration;
            velocity = math::move_towards(tangent, wanted, acceleration * delta) + platform - normal * record.settings.ground_stick_speed;
        } else {
            const double acceleration = moving ? record.settings.air_acceleration : record.settings.air_deceleration;
            const auto horizontal = math::move_towards({velocity.x, 0.0, velocity.z}, wanted, acceleration * delta);
            velocity.x = horizontal.x;
            velocity.z = horizontal.z;
        }
        const bool pressed = controls.jump_pressed || record.jump_requested;
        if (pressed) { record.state.jump_buffer_remaining = record.settings.jump_buffer_seconds; }
        const bool can_jump = record.state.grounded || record.state.coyote_remaining > 0.0;
        if ((pressed || record.state.jump_buffer_remaining > 0.0) && can_jump && record.settings.jump_speed > 0.0) {
            record.launch_support_y = platform.y;
            velocity.y = platform.y + record.settings.jump_speed;
            grounding(record, {});
            record.state.coyote_remaining = 0.0;
            record.state.jump_buffer_remaining = 0.0;
            record.support_delay = record.settings.jump_support_delay;
            record.jump_active = true;
            record.jump_cut = false;
            emit(record, CharacterEventKind::Jumped, record.settings.jump_speed);
        }
        const bool held = controls.jump_held || record.jump_held;
        if (record.jump_active && !held && !record.jump_cut && velocity.y > record.launch_support_y) {
            velocity.y = record.launch_support_y + (velocity.y - record.launch_support_y) * record.settings.released_jump_multiplier;
            record.jump_cut = true;
        }
        if (record.state.ceiling_blocked && velocity.y > 0.0) {
            velocity.y = 0.0;
            record.jump_active = false;
        }
        velocity.y = std::max(velocity.y, -record.settings.terminal_speed);
        const double gravity = record.settings.gravity_scale *
            (velocity.y < 0.0 && !record.state.grounded ? record.settings.falling_gravity_multiplier : 1.0);
        if (!physics.set_gravity_scale(record.body, gravity) || !physics.set_velocity(record.body, velocity, {})) {
            record.state.last_error = CharacterError::BodyRejected;
            ++statistics.rejected_writes;
        }
        if (record.settings.face_movement && moving) {
            record.upright = math::rotate_towards(record.upright, math::look_rotation(record.state.desired_velocity), record.settings.turn_speed * delta);
        }
        record.state.facing = math::rotate(record.upright, forward);
        body = physics.state(record.body);
        if (body && math::angular_distance(body->pose.rotation, record.upright) > 1.0e-8) {
            auto pose = body->pose;
            pose.rotation = record.upright;
            (void)write_pose(record, pose);
        }
        step_assist(record, record.state.desired_velocity, delta);
        record.jump_requested = false;
        record.input.jump_pressed = false;
        record.state.velocity = velocity;
        record.state.vertical_speed = velocity.y;
        if (!record.state.grounded && velocity.y > 0.0) { record.state.motion = CharacterMotion::Rising; }
    }

    void upright(const ConstraintContext& context) {
        if (stopped || context.stage != ConstraintStage::Finish) { return; }
        for (auto& [entity, record] : records) {
            (void)entity;
            if (!owned(record) || !record.state.enabled) { continue; }
            const auto body = physics.state(record.body);
            if (!body || !body->enabled || !body->scene_enabled) { continue; }
            auto pose = body->pose;
            pose.rotation = record.upright;
            auto velocity = body->linear_velocity;
            velocity.y = std::max(velocity.y, -record.settings.terminal_speed);
            if (!physics.set_pose(record.body, pose) || !physics.set_velocity(record.body, velocity, {})) {
                ++statistics.rejected_writes;
            }
        }
    }
};

CharacterSystem::CharacterSystem(PhysicsWorld& physics, CharacterLimits limits)
    : impl_(std::make_shared<Impl>(physics, limits)) {
    const std::weak_ptr<Impl> weak = impl_;
    if (!physics.add_constraint_solver(std::string(upright_hook), [weak](PhysicsWorld&, const ConstraintContext& context) {
        if (const auto state = weak.lock()) { state->upright(context); }
    })) {
        throw std::logic_error("character upright constraint registration rejected");
    }
}

CharacterSystem::~CharacterSystem() = default;

CharacterResult CharacterSystem::attach(ecs::Entity entity, const CharacterController& settings, CharacterAuthority owner) {
    auto& self = *impl_;
    if (self.busy()) { return {CharacterError::Busy}; }
    if (!valid_character(settings) || !valid_authority(owner)) { return {CharacterError::InvalidSettings}; }
    if (!self.registry.alive(entity) || !self.physics.scene().contains(entity)) { return {CharacterError::InvalidEntity}; }
    if (self.records.size() >= self.limits.characters) { return {CharacterError::Capacity}; }
    if (self.records.contains(entity) || self.registry.has<CharacterComponent>(entity) ||
        self.registry.has<BodyComponent>(entity) || !self.collision.colliders(entity).empty()) { return {CharacterError::AlreadyOwned}; }
    const auto pose = self.physics.scene().world_transform(entity);
    const auto mass = character_mass(settings, settings.standing_height);
    if (!pose || !unit_scale(*pose) || !mass || !self.physics.scene().world_transform_exact(entity)) { return {CharacterError::InvalidEntity}; }
    BodyDescription body;
    body.mass = *mass;
    body.linear_drag = 0.0;
    body.angular_drag = 0.0;
    body.gravity_scale = settings.gravity_scale;
    body.allow_sleep = false;
    const auto body_handle = self.physics.add(entity, body);
    if (!body_handle) { return {CharacterError::BodyRejected}; }
    ColliderHandle collider_handle;
    try {
        ColliderDescription collider;
        collider.shape = capsule(settings, settings.standing_height);
        collider.filter = settings.filter;
        collider.material.friction = 0.0;
        collider_handle = self.collision.add(entity, collider);
        if (!collider_handle || !self.collision.set_transform(collider_handle, *pose)) {
            if (collider_handle) { (void)self.collision.remove(collider_handle); }
            (void)self.physics.remove(body_handle);
            return {CharacterError::ColliderRejected};
        }
        Impl::Record record;
        record.entity = entity;
        record.body = body_handle;
        record.collider = collider_handle;
        record.settings = settings;
        record.state.entity = entity;
        record.state.height = settings.standing_height;
        record.state.position = pose->position;
        record.previous_position = pose->position;
        record.state.authority = owner == CharacterAuthority::Player ? CharacterAuthority::Manual : owner;
        record.state.facing = math::normalized(math::Vec3{math::rotate(pose->rotation, forward).x, 0.0,
                                                       math::rotate(pose->rotation, forward).z}, forward);
        record.upright = math::look_rotation(record.state.facing);
        (void)self.registry.emplace<CharacterComponent>(entity, CharacterComponent{body_handle, collider_handle, settings, record.state});
        const auto inserted = self.records.emplace(entity, std::move(record));
        auto upright_pose = *pose;
        upright_pose.rotation = inserted.first->second.upright;
        if (!self.physics.set_pose(body_handle, upright_pose)) {
            (void)self.registry.remove<CharacterComponent>(entity);
            (void)self.collision.remove(collider_handle);
            (void)self.physics.remove(body_handle);
            self.records.erase(inserted.first);
            return {CharacterError::BodyRejected};
        }
        self.emit(inserted.first->second, CharacterEventKind::Attached);
        self.statistics.characters = self.records.size();
    } catch (...) {
        try { (void)self.physics.set_pose(body_handle, *pose); } catch (...) {}
        (void)self.registry.remove<CharacterComponent>(entity);
        if (collider_handle) { (void)self.collision.remove(collider_handle); }
        (void)self.physics.remove(body_handle);
        self.records.erase(entity);
        throw;
    }
    if (owner == CharacterAuthority::Player) { (void)select_player(entity); }
    return {};
}

bool CharacterSystem::detach(ecs::Entity entity) {
    if (impl_->busy()) { return false; }
    const auto found = impl_->records.find(entity);
    if (found == impl_->records.end()) { return false; }
    impl_->erase(found, CharacterEventKind::Detached);
    return true;
}

bool CharacterSystem::clear() {
    if (impl_->busy()) { return false; }
    while (!impl_->records.empty()) { impl_->erase(impl_->records.begin(), CharacterEventKind::Detached); }
    return true;
}

bool CharacterSystem::shutdown() {
    if (impl_->stopped) { return true; }
    if (!clear()) { return false; }
    (void)impl_->release_player();
    (void)impl_->physics.remove_constraint_solver(upright_hook);
    impl_->stopped = true;
    return true;
}

const CharacterComponent* CharacterSystem::get(ecs::Entity entity) const noexcept {
    return impl_->find(entity) ? impl_->registry.get<CharacterComponent>(entity) : nullptr;
}

std::optional<CharacterState> CharacterSystem::state(ecs::Entity entity) const noexcept {
    const auto* record = impl_->find(entity);
    return record ? std::optional<CharacterState>{record->state} : std::nullopt;
}

std::vector<ecs::Entity> CharacterSystem::entities() const {
    std::vector<ecs::Entity> result;
    result.reserve(impl_->records.size());
    for (const auto& [entity, record] : impl_->records) {
        if (impl_->owned(record)) { result.push_back(entity); }
    }
    return result;
}

std::size_t CharacterSystem::size() const noexcept { return impl_->records.size(); }
const CharacterLimits& CharacterSystem::limits() const noexcept { return impl_->limits; }
const CharacterStatistics& CharacterSystem::statistics() const noexcept { return impl_->statistics; }
ecs::Entity CharacterSystem::player() const noexcept { return impl_->find(impl_->selected) ? impl_->selected : ecs::Entity{}; }
std::span<const CharacterEvent> CharacterSystem::events() const noexcept { return impl_->events; }
void CharacterSystem::clear_events() noexcept { impl_->events.clear(); }

bool CharacterSystem::set_input(ecs::Entity entity, const CharacterInput& input) {
    auto* record = impl_->find(entity);
    if (!record || impl_->busy()) { return false; }
    if (!math::finite(input.movement) || !math::finite(input.forward) ||
        std::abs(input.movement.x) > 1.0 || std::abs(input.movement.y) > 1.0) {
        record->state.last_error = CharacterError::InvalidInput;
        return false;
    }
    if (record->state.authority != CharacterAuthority::Manual) {
        record->state.last_error = CharacterError::WrongAuthority;
        return false;
    }
    record->input = input;
    return true;
}

bool CharacterSystem::request_jump(ecs::Entity entity, bool held) {
    auto* record = impl_->find(entity);
    if (!record || impl_->busy()) { return false; }
    if (held && !record->jump_held) { record->jump_requested = true; }
    record->jump_held = held;
    return true;
}

bool CharacterSystem::request_crouch(ecs::Entity entity, bool crouched) {
    auto* record = impl_->find(entity);
    if (!record || impl_->busy()) { return false; }
    record->crouch_requested = crouched;
    return true;
}

bool CharacterSystem::set_enabled(ecs::Entity entity, bool enabled) {
    auto* record = impl_->find(entity);
    if (!record || impl_->busy()) { return false; }
    if (record->state.enabled == enabled) { return true; }
    const auto* collider = impl_->collision.get(record->collider);
    if (!collider) { return false; }
    const bool was_enabled = collider->description.enabled;
    if (!impl_->collision.set_enabled(record->collider, enabled)) { return false; }
    if (!impl_->physics.set_enabled(record->body, enabled)) {
        (void)impl_->collision.set_enabled(record->collider, was_enabled);
        return false;
    }
    record->state.enabled = enabled;
    record->state.grounded = false;
    record->state.support = {};
    record->state.motion = enabled ? CharacterMotion::Falling : CharacterMotion::Disabled;
    record->state.coyote_remaining = 0.0;
    record->state.jump_buffer_remaining = 0.0;
    record->input = {};
    record->jump_requested = false;
    record->jump_held = false;
    impl_->emit(*record, enabled ? CharacterEventKind::Enabled : CharacterEventKind::Disabled);
    impl_->publish(*record);
    return true;
}

bool CharacterSystem::set_authority(ecs::Entity entity, CharacterAuthority owner) {
    auto* record = impl_->find(entity);
    if (!record || impl_->busy() || !valid_authority(owner)) { return false; }
    if (owner == CharacterAuthority::Player) { return select_player(entity); }
    if (impl_->selected == entity) { (void)impl_->release_player(); }
    record->state.authority = owner;
    record->input = {};
    impl_->publish(*record);
    return true;
}

bool CharacterSystem::select_player(ecs::Entity entity, render::CameraService* cameras, ecs::Entity camera) {
    auto* record = impl_->find(entity);
    if (!record || impl_->busy()) { return false; }
    std::optional<render::CameraController> previous;
    if (cameras) {
        if (!camera) { camera = cameras->active_entity(); }
        const auto* component = cameras->component(camera);
        if (!component || camera == entity || impl_->registry.has<BodyComponent>(camera) ||
            impl_->physics.scene().is_ancestor(camera, entity)) {
            record->state.last_error = CharacterError::CameraRejected;
            return false;
        }
        previous = impl_->camera_owner == cameras && impl_->camera_entity == camera && impl_->previous_camera ?
            impl_->previous_camera : std::optional<render::CameraController>{component->controller};
        auto follow = component->controller;
        follow.mode = render::CameraMode::Follow;
        follow.target = entity;
        follow.focus_point = {0.0, record->state.height * 0.75, 0.0};
        follow.follow_target_rotation = false;
        if (!cameras->set_controller(camera, follow)) {
            record->state.last_error = CharacterError::CameraRejected;
            return false;
        }
    }
    const bool same_camera = impl_->selected == entity && impl_->camera_owner == cameras && impl_->camera_entity == camera;
    if (!same_camera) { (void)impl_->release_player(); }
    impl_->selected = entity;
    impl_->camera_owner = cameras;
    impl_->camera_entity = camera;
    impl_->previous_camera = previous;
    record->state.authority = CharacterAuthority::Player;
    record->input = {};
    impl_->publish(*record);
    return true;
}

bool CharacterSystem::release_player() {
    return !impl_->busy() && impl_->release_player();
}

void CharacterSystem::control(double delta_seconds, std::uint64_t tick, input::InputSystem* input,
                              const render::CameraService* cameras) {
    auto& self = *impl_;
    if (self.busy()) { return; }
    self.statistics.queries = 0;
    self.statistics.incomplete_queries = 0;
    self.statistics.invalid_delta = !std::isfinite(delta_seconds) || delta_seconds < 0.0;
    self.statistics.simulated_delta = 0.0;
    if (self.statistics.invalid_delta) { return; }
    self.tick = tick;
    const auto& physics_limits = self.physics.config();
    const double delta = std::min({delta_seconds, physics_limits.max_frame_seconds,
        physics_limits.max_substep_seconds * static_cast<double>(physics_limits.max_substeps)});
    self.statistics.simulated_delta = delta;
    self.copy_contacts();
    for (auto iterator = self.records.begin(); iterator != self.records.end();) {
        if (!self.owned(iterator->second)) {
            const auto stale = iterator++;
            self.erase(stale, CharacterEventKind::OwnershipLost);
            ++self.statistics.removed_stale;
        } else { ++iterator; }
    }
    CharacterInput player_input;
    if (input && self.find(self.selected)) {
        player_input.movement = input->vector2("move");
        player_input.jump_pressed = input->pressed("jump");
        player_input.jump_held = input->down("jump");
        player_input.crouch = input->down("crouch");
        player_input.sprint = input->down("sprint");
        if (cameras) {
            const auto* view = self.camera_entity ? cameras->snapshot(self.camera_entity) : cameras->current_view();
            if (view && !view->stale) { player_input.forward = view->forward; }
            else if (const auto pose = self.physics.scene().world_transform(self.camera_entity ? self.camera_entity : cameras->active_entity())) {
                player_input.forward = math::rotate(pose->rotation, forward);
            }
        }
        (void)input->consume("move");
        (void)input->consume("jump");
        (void)input->consume("crouch");
        (void)input->consume("sprint");
    }
    for (auto& [entity, record] : self.records) {
        (void)entity;
        record.state.tick = tick;
        record.state.last_error = CharacterError::None;
        record.state.query_incomplete = false;
        record.state.ceiling_blocked = false;
        record.state.movement_blocked = false;
        record.steep_normal = {};
        const auto body = self.physics.state(record.body);
        if (!body) { continue; }
        record.previous_position = body->pose.position;
        record.state.position = body->pose.position;
        record.state.velocity = body->linear_velocity;
        record.state.vertical_speed = body->linear_velocity.y;
        if (!record.state.enabled || !body->enabled || !body->scene_enabled || !self.registry.enabled(record.entity)) {
            record.state.motion = CharacterMotion::Disabled;
            record.state.grounded = false;
            record.state.support = {};
            self.publish(record);
            continue;
        }
        if (delta == 0.0) { continue; }
        record.support_delay = std::max(0.0, record.support_delay - delta);
        record.state.coyote_remaining = std::max(0.0, record.state.coyote_remaining - delta);
        record.state.jump_buffer_remaining = std::max(0.0, record.state.jump_buffer_remaining - delta);
        record.state.motion = body->linear_velocity.y > 0.0 ? CharacterMotion::Rising : CharacterMotion::Falling;
        self.grounding(record, self.detect_support(record, *body, true));
        const double gap = record.state.support.distance;
        if (record.state.grounded && gap > record.settings.skin) {
            auto snapped = body->pose;
            snapped.position.y -= gap;
            if (self.clear_path(record, body->pose, {0.0, -gap, 0.0}) && self.write_pose(record, snapped)) {
                record.state.support.distance = 0.0;
            }
        }
        const auto controls = record.state.authority == CharacterAuthority::Player ? player_input : record.input;
        self.stance(record, controls.crouch || record.crouch_requested);
        const bool corrected = self.depenetrate(record);
        if (corrected) {
            if (const auto current = self.physics.state(record.body)) {
                self.grounding(record, self.detect_support(record, *current, false));
            }
        }
        self.move(record, controls, delta);
        if (!record.state.grounded) { record.state.airborne_seconds += delta; }
        self.publish(record);
    }
}

void CharacterSystem::sample(std::uint64_t tick) {
    auto& self = *impl_;
    if (self.busy()) { return; }
    self.tick = tick;
    self.copy_contacts();
    self.statistics.grounded = 0;
    self.statistics.crouched = 0;
    self.statistics.disabled = 0;
    for (auto& [entity, record] : self.records) {
        (void)entity;
        if (!self.owned(record)) { continue; }
        const auto body = self.physics.state(record.body);
        if (!body) { continue; }
        record.state.tick = tick;
        record.state.position = body->pose.position;
        record.state.velocity = body->linear_velocity;
        record.state.horizontal_speed = std::hypot(body->linear_velocity.x, body->linear_velocity.z);
        record.state.motion = body->linear_velocity.y > 0.0 ? CharacterMotion::Rising : CharacterMotion::Falling;
        const bool active = record.state.enabled && body->enabled && body->scene_enabled && self.registry.enabled(record.entity);
        if (active) { self.grounding(record, self.detect_support(record, *body, true)); }
        else { record.state.motion = CharacterMotion::Disabled; record.state.grounded = false; record.state.support = {}; }
        record.state.vertical_speed = body->linear_velocity.y;
        const double distance = math::distance(record.previous_position, body->pose.position);
        if (std::isfinite(distance)) {
            record.state.distance_travelled = std::min(1.0e15, record.state.distance_travelled + distance);
        }
        record.previous_position = body->pose.position;
        self.statistics.grounded += record.state.grounded ? 1U : 0U;
        self.statistics.crouched += record.state.crouched ? 1U : 0U;
        self.statistics.disabled += active ? 0U : 1U;
        self.publish(record);
    }
    self.statistics.characters = self.records.size();
}

void register_character(core::Kernel& kernel) {
    if (kernel.services().find<CharacterSystem>()) { return; }
    auto* physics = kernel.services().find<PhysicsWorld>();
    if (!physics) { throw std::logic_error("character registration requires PhysicsWorld"); }
    CharacterLimits limits;
    limits.characters = static_cast<std::size_t>(std::clamp<std::int64_t>(kernel.config().integer("character.capacity", 1024), 0, 65536));
    limits.queries_per_frame = static_cast<std::size_t>(std::clamp<std::int64_t>(kernel.config().integer("character.query_budget", 16384), 1, 1048576));
    auto& characters = kernel.services().emplace<CharacterSystem>(*physics, limits);
    if (auto* actions = kernel.services().find<input::InputSystem>()) {
        const auto gameplay = actions->context_id("gameplay");
        const auto* context = actions->context(gameplay);
        if (context && std::none_of(context->actions.begin(), context->actions.end(),
                                   [](const auto& action) { return action.name == "crouch"; })) {
            if (actions->define_action(gameplay, {"crouch", input::ActionKind::Button})) {
                input::Binding binding;
                binding.action = "crouch";
                binding.controls[0] = input::key(17);
                binding.response.deadzone = 0.0;
                (void)actions->bind(gameplay, std::move(binding));
            }
        }
    }
    (void)kernel.register_module({"physics.character", "Owned capsule characters, support, player and navigation control", 1});
    (void)kernel.add_shutdown("physics.character.shutdown", [service = &characters] { (void)service->shutdown(); });
    (void)kernel.add_system({"physics.character.control", core::Phase::PreSimulation, -7900,
                           {"navigation.update", "physics.collision"}},
        [](core::Kernel& host, const core::Frame& frame) {
            if (host.config().boolean("physics.enabled", true)) {
                host.services().get<CharacterSystem>().control(frame.delta_seconds, frame.tick,
                    host.services().find<input::InputSystem>(), host.services().find<render::CameraService>());
            }
        });
    (void)kernel.add_system({"physics.character.sample", core::Phase::PostSimulation, -9000, {"physics.dynamics.step"}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& service = host.services().get<CharacterSystem>();
            if (host.config().boolean("physics.enabled", true)) { service.sample(frame.tick); }
            for (const auto& event : service.events()) { (void)host.events().publish(event); }
            service.clear_events();
            const auto& statistics = service.statistics();
            host.metric("character.count", static_cast<double>(statistics.characters));
            host.metric("character.grounded", static_cast<double>(statistics.grounded));
            host.metric("character.queries", static_cast<double>(statistics.queries));
            host.metric("character.query_failures", static_cast<double>(statistics.incomplete_queries));
            host.metric("character.write_failures", static_cast<double>(statistics.rejected_writes));
        });
    (void)kernel.add_command("character.attach", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2) { return {false, "usage: character.attach entity [manual|player|navigation]"}; }
        auto& service = host.services().get<CharacterSystem>();
        auto& world = host.services().get<PhysicsWorld>();
        const auto entity = resolve(world, args[0]);
        const auto owner = args.size() == 2 ? authority(args[1]) : std::optional{CharacterAuthority::Manual};
        if (!owner) { return {false, "unknown character authority"}; }
        if (*owner == CharacterAuthority::Player && vehicle_selected(host)) { return {false, "release the vehicle player before selecting a character"}; }
        const auto result = service.attach(entity, {}, *owner == CharacterAuthority::Player ? CharacterAuthority::Manual : *owner);
        if (!result) { return {false, std::string(describe(result.error))}; }
        if (*owner == CharacterAuthority::Player && !service.select_player(entity, host.services().find<render::CameraService>())) {
            (void)service.detach(entity);
            return {false, "player camera rejected; attachment rolled back"};
        }
        return {true, "character attached to " + ecs::format_entity(entity)};
    }, "Attach an owned capsule/body to an unowned Scene entity");
    (void)kernel.add_command("character.move", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 3 && args.size() != 5) { return {false, "usage: character.move entity right forward [camera_forward_x camera_forward_z]"}; }
        const auto x = number(args[1]);
        const auto y = number(args[2]);
        const auto facing_x = args.size() == 5 ? number(args[3]) : std::optional{0.0};
        const auto facing_z = args.size() == 5 ? number(args[4]) : std::optional{-1.0};
        if (!x || !y || !facing_x || !facing_z) { return {false, "movement values must be finite numbers"}; }
        CharacterInput controls;
        controls.movement = {*x, *y};
        controls.forward = {*facing_x, 0.0, *facing_z};
        const auto entity = resolve(host.services().get<PhysicsWorld>(), args[0]);
        const bool accepted = host.services().get<CharacterSystem>().set_input(entity, controls);
        return {accepted, accepted ? "manual movement updated" : "movement rejected; use manual authority and axes in [-1,1]"};
    }, "Set persistent manual camera-relative movement; zero axes stop");
    (void)kernel.add_command("character.jump", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "hold" && args[1] != "release")) {
            return {false, "usage: character.jump entity [hold|release]"};
        }
        const auto entity = resolve(host.services().get<PhysicsWorld>(), args[0]);
        const bool held = args.size() == 1 || args[1] == "hold";
        const bool accepted = host.services().get<CharacterSystem>().request_jump(entity, held);
        return {accepted, accepted ? "jump request updated" : "character not available"};
    }, "Buffer a jump or release its variable-height hold");
    (void)kernel.add_command("character.crouch", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2 || (args[1] != "on" && args[1] != "off")) { return {false, "usage: character.crouch entity on|off"}; }
        const auto entity = resolve(host.services().get<PhysicsWorld>(), args[0]);
        const bool accepted = host.services().get<CharacterSystem>().request_crouch(entity, args[1] == "on");
        return {accepted, accepted ? "stance requested; standing requires clearance" : "character not available"};
    }, "Request crouching or checked standing clearance");
    (void)kernel.add_command("character.state", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: character.state entity"}; }
        const auto entity = resolve(host.services().get<PhysicsWorld>(), args[0]);
        const auto state = host.services().get<CharacterSystem>().state(entity);
        if (!state) { return {false, "character not available"}; }
        return {true, "motion=" + std::string(describe(state->motion)) + " speed=" + std::to_string(state->horizontal_speed) +
            " vertical=" + std::to_string(state->vertical_speed) + " crouched=" + (state->crouched ? "true" : "false") +
            " support=" + ecs::format_entity(state->support.entity) + " error=" + std::string(describe(state->last_error))};
    }, "Inspect the shared gameplay and animation character state");
    (void)kernel.add_command("character.control", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: character.control entity manual|player|navigation|remove|enable|disable"}; }
        auto& service = host.services().get<CharacterSystem>();
        const auto entity = resolve(host.services().get<PhysicsWorld>(), args[0]);
        if (args[1] == "remove") { return {service.detach(entity), "character removal requested"}; }
        if (args[1] == "enable" || args[1] == "disable") { return {service.set_enabled(entity, args[1] == "enable"), "character availability requested"}; }
        const auto owner = authority(args[1]);
        if (!owner) { return {false, "unknown character control mode"}; }
        if (*owner == CharacterAuthority::Player && vehicle_selected(host)) { return {false, "release the vehicle player before selecting a character"}; }
        const bool accepted = *owner == CharacterAuthority::Player ?
            service.select_player(entity, host.services().find<render::CameraService>()) : service.set_authority(entity, *owner);
        return {accepted, accepted ? "character authority updated" : "character authority rejected"};
    }, "Select player/manual/navigation authority or release an owned character");
}

} // namespace ge::physics
