#include <ge/physics/dynamics.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ge::physics {
namespace {

constexpr double minimum_mass = 1.0e-12;
constexpr double maximum_mass = 1.0e12;

bool bounded_vector(math::Vec3 value, double bound = 1.0e12) noexcept {
    return math::finite(value) && std::abs(value.x) <= bound &&
           std::abs(value.y) <= bound && std::abs(value.z) <= bound;
}

bool valid_pose(const math::Transform& pose) noexcept {
    const auto scale = math::abs(pose.scale);
    const double rotation_length = math::dot(pose.rotation, pose.rotation);
    return math::finite(pose) && bounded_vector(pose.position) &&
           bounded_vector(scale, 1.0e6) && scale.x > math::epsilon &&
           scale.y > math::epsilon && scale.z > math::epsilon &&
           std::isfinite(rotation_length) && rotation_length > math::epsilon;
}

bool valid_mode(BodyMode mode) noexcept {
    return mode == BodyMode::Static || mode == BodyMode::Kinematic || mode == BodyMode::Dynamic;
}

bool same_pose(const math::Transform& first, const math::Transform& second) noexcept {
    return math::almost_equal(first.position, second.position, 1.0e-8) &&
           math::almost_equal(first.scale, second.scale, 1.0e-8) &&
           std::abs(math::dot(math::normalized(first.rotation),
                             math::normalized(second.rotation))) >= 1.0 - 1.0e-12;
}

bool responsive(const BodyState* body) noexcept {
    return body && body->mode == BodyMode::Dynamic && body->awake &&
           body->enabled && body->scene_enabled;
}

void set_inverse_mass(BodyState& body) noexcept {
    const bool dynamic = body.mode == BodyMode::Dynamic;
    body.inverse_mass = dynamic ? 1.0 / body.mass.mass : 0.0;
    body.inverse_inertia = dynamic ? math::Vec3{1.0 / body.mass.inertia.x,
        1.0 / body.mass.inertia.y, 1.0 / body.mass.inertia.z} : math::Vec3{};
}

double inverse_effective_mass(const BodyState* body, math::Vec3 offset,
                              math::Vec3 direction) noexcept {
    if (!responsive(body)) {
        return 0.0;
    }
    const auto rotational = inverse_inertia_world(*body) * math::cross(offset, direction);
    return body->inverse_mass + math::dot(math::cross(rotational, offset), direction);
}

math::Vec3 velocity_at(const BodyState* body, math::Vec3 offset) noexcept {
    if (!body || body->mode == BodyMode::Static || !body->enabled || !body->scene_enabled) {
        return {};
    }
    return body->linear_velocity + math::cross(body->angular_velocity, offset);
}

PhysicsConfig validated(PhysicsConfig value) {
    const std::array<double, 11> nonnegative{{value.penetration_slop, value.correction_fraction,
        value.max_position_correction, value.restitution_threshold, value.sleep_linear_speed,
        value.sleep_angular_speed, value.sleep_after_seconds, value.max_linear_speed,
        value.max_angular_speed, value.max_substep_seconds, value.max_frame_seconds}};
    for (const double number : nonnegative) {
        if (!std::isfinite(number) || number < 0.0) {
            throw std::invalid_argument("physics configuration requires finite nonnegative values");
        }
    }
    if (value.capacity > 1000000 || value.max_substeps == 0 || value.max_substeps > 256 ||
        value.velocity_iterations == 0 || value.velocity_iterations > 128 ||
        value.position_iterations > 64 || value.max_solver_points > 1048576 ||
        value.max_events > 1048576 || value.max_constraint_solvers > 128 ||
        value.max_substep_seconds < 1.0e-6 || value.max_substep_seconds > 1.0 ||
        value.max_frame_seconds > 10.0 || value.max_frame_seconds == 0.0 ||
        value.max_linear_speed == 0.0 || value.max_linear_speed > 1.0e6 ||
        value.max_angular_speed == 0.0 || value.max_angular_speed > 1.0e6 ||
        value.correction_fraction > 1.0 || !bounded_vector(value.gravity, 1.0e6)) {
        throw std::invalid_argument("physics configuration exceeds bounded solver limits");
    }
    return value;
}

std::optional<double> parse_number(std::string_view text) noexcept {
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<math::Vec3> parse_vector(core::CommandArgs args, std::size_t start) noexcept {
    if (start > args.size() || args.size() - start < 3) {
        return std::nullopt;
    }
    const auto x = parse_number(args[start]);
    const auto y = parse_number(args[start + 1]);
    const auto z = parse_number(args[start + 2]);
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return math::Vec3{*x, *y, *z};
}

ecs::Entity resolve_entity(PhysicsWorld& world, std::string_view text) {
    if (const auto parsed = ecs::parse_entity(text); parsed && world.registry().alive(*parsed)) {
        return *parsed;
    }
    const auto named = world.registry().find_all_named(text);
    return named.size() == 1 ? named.front() : ecs::Entity{};
}

void publish_runtime(core::Kernel& kernel, PhysicsWorld& world) {
    const auto& stats = world.statistics();
    kernel.metric("physics.bodies", static_cast<double>(stats.bodies));
    kernel.metric("physics.sleeping", static_cast<double>(stats.sleeping_bodies));
    kernel.metric("physics.contacts", static_cast<double>(stats.contacts));
    kernel.metric("physics.solver_points", static_cast<double>(stats.solver_points));
    kernel.metric("physics.substeps", static_cast<double>(stats.substeps));
    kernel.metric("physics.dropped_seconds", stats.dropped_seconds);
    kernel.metric("physics.energy", stats.kinetic_energy);
    kernel.metric("physics.scene_rejections", static_cast<double>(stats.scene_rejections));
    kernel.metric("physics.collider_rejections", static_cast<double>(stats.collider_rejections));
    kernel.metric("physics.dropped_events", static_cast<double>(stats.dropped_events));
    for (const auto& event : world.events()) {
        (void)kernel.events().publish(event);
    }
    world.clear_events();
}

} // namespace

bool valid_mass(const MassProperties& properties) noexcept {
    return std::isfinite(properties.mass) && properties.mass >= minimum_mass &&
           properties.mass <= maximum_mass && bounded_vector(properties.inertia, 1.0e24) &&
           properties.inertia.x >= minimum_mass && properties.inertia.y >= minimum_mass &&
           properties.inertia.z >= minimum_mass && bounded_vector(properties.center_of_mass, 1.0e6);
}

std::optional<MassProperties> sphere_mass(double mass, double radius) noexcept {
    if (!std::isfinite(radius) || radius <= 0.0 || radius > 1.0e6) {
        return std::nullopt;
    }
    const double inertia = 0.4 * mass * radius * radius;
    MassProperties result{mass, {inertia, inertia, inertia}, {}};
    return valid_mass(result) ? std::optional<MassProperties>{result} : std::nullopt;
}

std::optional<MassProperties> box_mass(double mass, math::Vec3 half_extents) noexcept {
    if (!bounded_vector(half_extents, 1.0e6) || half_extents.x <= 0.0 ||
        half_extents.y <= 0.0 || half_extents.z <= 0.0) {
        return std::nullopt;
    }
    const auto squared = half_extents * half_extents;
    const math::Vec3 inertia{mass * (squared.y + squared.z) / 3.0,
        mass * (squared.x + squared.z) / 3.0, mass * (squared.x + squared.y) / 3.0};
    MassProperties result{mass, inertia, {}};
    return valid_mass(result) ? std::optional<MassProperties>{result} : std::nullopt;
}

math::Vec3 center_of_mass(const BodyState& body) noexcept {
    return math::transform_point(body.pose, body.mass.center_of_mass);
}

math::Mat3 inverse_inertia_world(const BodyState& body) noexcept {
    if (body.mode != BodyMode::Dynamic) {
        return math::Mat3::zero();
    }
    auto diagonal = math::Mat3::zero();
    diagonal(0, 0) = body.inverse_inertia.x;
    diagonal(1, 1) = body.inverse_inertia.y;
    diagonal(2, 2) = body.inverse_inertia.z;
    const auto rotation = math::rotation_matrix(body.pose.rotation);
    return rotation * diagonal * math::transpose(rotation);
}

math::Vec3 point_velocity(const BodyState& body, math::Vec3 world_point) noexcept {
    return velocity_at(&body, world_point - center_of_mass(body));
}

double kinetic_energy(const BodyState& body) noexcept {
    if (body.mode != BodyMode::Dynamic || !body.enabled || !body.scene_enabled) {
        return 0.0;
    }
    const auto angular = math::rotate(math::conjugate(body.pose.rotation), body.angular_velocity);
    return 0.5 * (body.mass.mass * math::length_squared(body.linear_velocity) +
                  math::dot(angular * body.mass.inertia, angular));
}

PhysicsWorld::PhysicsWorld(ge::scene::Scene& shared_scene, CollisionWorld& shared_collision, PhysicsConfig configuration)
    : scene_(shared_scene), collision_(shared_collision), registry_(shared_scene.registry()),
      config_(validated(configuration)), handles_(config_.capacity) {
    if (&registry_ != &collision_.registry()) {
        throw std::invalid_argument("physics scene and collision must share one registry");
    }
    (void)registry_.register_component<BodyComponent>("physics.body");
    (void)registry_.register_component<ecs::VelocityComponent>("velocity");
    slots_.resize(config_.capacity);
    edits_.reserve(config_.capacity);
    solver_points_.reserve(config_.max_solver_points);
    events_.reserve(config_.max_events);
    hooks_.reserve(config_.max_constraint_solvers);
}

ecs::Registry& PhysicsWorld::registry() noexcept { return registry_; }
ge::scene::Scene& PhysicsWorld::scene() noexcept { return scene_; }
CollisionWorld& PhysicsWorld::collision() noexcept { return collision_; }
const PhysicsConfig& PhysicsWorld::config() const noexcept { return config_; }
const PhysicsStatistics& PhysicsWorld::statistics() const noexcept { return statistics_; }
std::size_t PhysicsWorld::size() const noexcept { return handles_.size(); }
bool PhysicsWorld::stepping() const noexcept { return stepping_; }

const PhysicsWorld::Record* PhysicsWorld::record(BodyHandle handle) const noexcept {
    if (!handles_.alive({handle.index, handle.generation})) {
        return nullptr;
    }
    const auto& slot = slots_[static_cast<std::size_t>(handle.index - 1)];
    if (!slot || !registry_.alive(slot->body.entity)) {
        return nullptr;
    }
    const auto* component = registry_.get<BodyComponent>(slot->body.entity);
    return component && component->body == handle ? &*slot : nullptr;
}

PhysicsWorld::Record* PhysicsWorld::record(BodyHandle handle) noexcept {
    return const_cast<Record*>(std::as_const(*this).record(handle));
}

const BodyState* PhysicsWorld::get(BodyHandle handle) const noexcept {
    const auto* entry = record(handle);
    return entry ? &entry->body : nullptr;
}

BodyHandle PhysicsWorld::find(ecs::Entity entity) const noexcept {
    const auto* component = registry_.get<BodyComponent>(entity);
    const auto* body = component ? get(component->body) : nullptr;
    return body && body->entity == entity ? body->handle : BodyHandle{};
}

const BodyState* PhysicsWorld::get(ecs::Entity entity) const noexcept { return get(find(entity)); }

std::optional<BodyState> PhysicsWorld::state(BodyHandle handle) const noexcept {
    const auto* body = get(handle);
    return body ? std::optional<BodyState>{*body} : std::nullopt;
}

std::vector<BodyHandle> PhysicsWorld::bodies() const {
    std::vector<BodyHandle> result;
    result.reserve(size());
    for (const auto& entry : slots_) {
        if (entry && get(entry->body.handle)) {
            result.push_back(entry->body.handle);
        }
    }
    return result;
}

BodyHandle PhysicsWorld::add(ecs::Entity entity, const BodyDescription& description) {
    if (stepping_ || registry_.iterating() || !registry_.alive(entity) || find(entity) ||
        !valid_mode(description.mode) || !valid_mass(description.mass) ||
        !bounded_vector(description.linear_velocity) || !bounded_vector(description.angular_velocity) ||
        !std::isfinite(description.linear_drag) || description.linear_drag < 0.0 ||
        !std::isfinite(description.angular_drag) || description.angular_drag < 0.0 ||
        !std::isfinite(description.gravity_scale) || std::abs(description.gravity_scale) > 1.0e6) {
        return {};
    }
    (void)scene_.update();
    const auto pose = scene_.world_transform(entity);
    if (!pose || !valid_pose(*pose) || !scene_.world_transform_exact(entity)) {
        return {};
    }
    const auto allocated = handles_.acquire();
    if (!allocated) {
        return {};
    }
    const BodyHandle handle{allocated->index, allocated->generation};
    const bool had_velocity = registry_.has<ecs::VelocityComponent>(entity);
    try {
        (void)registry_.emplace<ecs::VelocityComponent>(entity);
        (void)registry_.emplace_or_replace<BodyComponent>(entity, BodyComponent{handle});
    } catch (...) {
        if (!had_velocity) {
            (void)registry_.remove<ecs::VelocityComponent>(entity);
        }
        (void)handles_.release(*allocated);
        throw;
    }
    Record entry;
    auto& body = entry.body;
    body.handle = handle;
    body.entity = entity;
    body.mode = description.mode;
    body.pose = *pose;
    body.pose.rotation = math::normalized(body.pose.rotation);
    body.previous_pose = body.pose;
    body.mass = description.mass;
    body.linear_velocity = description.linear_velocity;
    body.angular_velocity = description.angular_velocity;
    body.linear_drag = description.linear_drag;
    body.angular_drag = description.angular_drag;
    body.gravity_scale = description.gravity_scale;
    body.allow_sleep = description.allow_sleep;
    body.awake = description.awake || !description.allow_sleep;
    body.enabled = description.enabled;
    body.scene_enabled = scene_.effective_enabled(entity);
    if (body.mode == BodyMode::Static || !body.awake) {
        body.linear_velocity = {};
        body.angular_velocity = {};
    }
    set_inverse_mass(body);
    limit_velocity(body);
    entry.published_pose = body.pose;
    entry.checkpoint = body.pose;
    slots_[static_cast<std::size_t>(handle.index - 1)] = entry;
    publish_velocity(slots_[static_cast<std::size_t>(handle.index - 1)]->body);
    refresh_statistics();
    return handle;
}

bool PhysicsWorld::erase(BodyHandle handle) {
    if (!handles_.alive({handle.index, handle.generation})) {
        return false;
    }
    auto& slot = slots_[static_cast<std::size_t>(handle.index - 1)];
    if (slot) {
        const auto entity = slot->body.entity;
        const auto* component = registry_.get<BodyComponent>(entity);
        if (component && component->body == handle) {
            (void)registry_.remove<BodyComponent>(entity);
            if (auto* velocity = registry_.get<ecs::VelocityComponent>(entity)) {
                *velocity = {};
                (void)registry_.mark_changed<ecs::VelocityComponent>(entity);
            }
        }
    }
    slot.reset();
    return handles_.release({handle.index, handle.generation});
}

bool PhysicsWorld::remove(BodyHandle handle) {
    if (stepping_ || registry_.iterating()) {
        return false;
    }
    const bool removed = erase(handle);
    refresh_statistics();
    return removed;
}

bool PhysicsWorld::remove(ecs::Entity entity) { return remove(find(entity)); }

void PhysicsWorld::clear() {
    if (stepping_ || registry_.iterating()) {
        return;
    }
    for (auto& entry : slots_) {
        if (entry) {
            (void)erase(entry->body.handle);
        }
    }
    solver_points_.clear();
    tracked_contacts_.clear();
    events_.clear();
    refresh_statistics();
}

bool PhysicsWorld::set_mode(BodyHandle handle, BodyMode mode) {
    auto* entry = record(handle);
    if (!entry || stepping_ || !valid_mode(mode)) {
        return false;
    }
    auto& body = entry->body;
    body.mode = mode;
    body.force = {};
    body.torque = {};
    body.quiet_seconds = 0.0;
    body.awake = true;
    entry->target.reset();
    if (mode == BodyMode::Static) {
        body.linear_velocity = {};
        body.angular_velocity = {};
    }
    set_inverse_mass(body);
    publish_velocity(body);
    return true;
}

bool PhysicsWorld::set_mass(BodyHandle handle, const MassProperties& properties) {
    auto* entry = record(handle);
    if (!entry || stepping_ || !valid_mass(properties)) {
        return false;
    }
    entry->body.mass = properties;
    set_inverse_mass(entry->body);
    return wake(handle);
}

bool PhysicsWorld::set_enabled(BodyHandle handle, bool enabled) {
    auto* entry = record(handle);
    if (!entry || stepping_) {
        return false;
    }
    entry->body.enabled = enabled;
    entry->body.force = {};
    entry->body.torque = {};
    if (enabled) {
        (void)wake(handle);
    }
    return true;
}

bool PhysicsWorld::set_sleep_allowed(BodyHandle handle, bool allowed) {
    auto* entry = record(handle);
    if (!entry) {
        return false;
    }
    entry->body.allow_sleep = allowed;
    return allowed || wake(handle);
}

bool PhysicsWorld::wake(BodyHandle handle) {
    auto* entry = record(handle);
    if (!entry) {
        return false;
    }
    entry->body.awake = true;
    entry->body.quiet_seconds = 0.0;
    return true;
}

bool PhysicsWorld::sleep(BodyHandle handle) {
    auto* entry = record(handle);
    if (!entry || entry->body.mode != BodyMode::Dynamic || !entry->body.allow_sleep) {
        return false;
    }
    entry->body.awake = false;
    entry->body.linear_velocity = {};
    entry->body.angular_velocity = {};
    entry->body.force = {};
    entry->body.torque = {};
    publish_velocity(entry->body);
    return true;
}

void PhysicsWorld::limit_velocity(BodyState& body) const noexcept {
    body.linear_velocity = math::finite(body.linear_velocity)
        ? math::clamp_length(body.linear_velocity, config_.max_linear_speed) : math::Vec3{};
    body.angular_velocity = math::finite(body.angular_velocity)
        ? math::clamp_length(body.angular_velocity, config_.max_angular_speed) : math::Vec3{};
}

void PhysicsWorld::publish_velocity(BodyState& body) {
    if (auto* velocity = registry_.get<ecs::VelocityComponent>(body.entity)) {
        velocity->linear = body.linear_velocity;
        velocity->angular = body.angular_velocity;
        (void)registry_.mark_changed<ecs::VelocityComponent>(body.entity);
    }
}

bool PhysicsWorld::set_velocity(BodyHandle handle, math::Vec3 linear, math::Vec3 angular) {
    auto* entry = record(handle);
    if (!entry || entry->body.mode == BodyMode::Static || !bounded_vector(linear) || !bounded_vector(angular)) {
        return false;
    }
    entry->body.linear_velocity = linear;
    entry->body.angular_velocity = angular;
    entry->target.reset();
    limit_velocity(entry->body);
    (void)wake(handle);
    publish_velocity(entry->body);
    return true;
}

bool PhysicsWorld::set_pose(BodyHandle handle, const math::Transform& pose, bool clear_velocity) {
    auto* entry = record(handle);
    if (!entry || !valid_pose(pose)) {
        return false;
    }
    auto canonical = pose;
    canonical.rotation = math::normalized(pose.rotation);
    if (!stepping_ && !scene_.set_world_transform(entry->body.entity, canonical)) {
        return false;
    }
    entry->body.pose = canonical;
    entry->target.reset();
    if (!stepping_) {
        entry->body.previous_pose = canonical;
        entry->published_pose = canonical;
        for (const auto collider : collision_.colliders(entry->body.entity)) {
            (void)collision_.set_transform(collider, canonical);
        }
    }
    if (clear_velocity) {
        entry->body.linear_velocity = {};
        entry->body.angular_velocity = {};
        entry->body.force = {};
        entry->body.torque = {};
    }
    (void)wake(handle);
    publish_velocity(entry->body);
    return true;
}

bool PhysicsWorld::set_kinematic_target(BodyHandle handle, const math::Transform& target) {
    auto* entry = record(handle);
    if (!entry || stepping_ || entry->body.mode != BodyMode::Kinematic || !valid_pose(target) ||
        !math::almost_equal(target.scale, entry->body.pose.scale)) {
        return false;
    }
    entry->target = target;
    entry->target->rotation = math::normalized(target.rotation);
    return true;
}

bool PhysicsWorld::set_drag(BodyHandle handle, double linear, double angular) {
    auto* entry = record(handle);
    if (!entry || !std::isfinite(linear) || !std::isfinite(angular) || linear < 0.0 || angular < 0.0) {
        return false;
    }
    entry->body.linear_drag = linear;
    entry->body.angular_drag = angular;
    return true;
}

bool PhysicsWorld::set_gravity(math::Vec3 gravity) noexcept {
    if (!bounded_vector(gravity, 1.0e6)) {
        return false;
    }
    config_.gravity = gravity;
    for (auto& entry : slots_) {
        if (entry && entry->body.mode == BodyMode::Dynamic) {
            entry->body.awake = true;
            entry->body.quiet_seconds = 0.0;
        }
    }
    return true;
}

bool PhysicsWorld::set_gravity_scale(BodyHandle handle, double scale) {
    auto* entry = record(handle);
    if (!entry || !std::isfinite(scale) || std::abs(scale) > 1.0e6) {
        return false;
    }
    entry->body.gravity_scale = scale;
    return wake(handle);
}

bool PhysicsWorld::apply_force(BodyHandle handle, math::Vec3 force) {
    const auto* body = get(handle);
    return body && apply_force(handle, force, center_of_mass(*body));
}

bool PhysicsWorld::apply_force(BodyHandle handle, math::Vec3 force, math::Vec3 world_point) {
    auto* entry = record(handle);
    if (!entry || entry->body.mode != BodyMode::Dynamic || !bounded_vector(force) || !bounded_vector(world_point)) {
        return false;
    }
    auto& body = entry->body;
    const auto next_force = body.force + force;
    const auto next_torque = body.torque + math::cross(world_point - center_of_mass(body), force);
    if (!bounded_vector(next_force) || !bounded_vector(next_torque, 1.0e24)) {
        return false;
    }
    body.force = next_force;
    body.torque = next_torque;
    return math::length_squared(force) == 0.0 || wake(handle);
}

bool PhysicsWorld::apply_torque(BodyHandle handle, math::Vec3 torque) {
    auto* entry = record(handle);
    if (!entry || entry->body.mode != BodyMode::Dynamic || !bounded_vector(torque, 1.0e24) ||
        !bounded_vector(entry->body.torque + torque, 1.0e24)) {
        return false;
    }
    entry->body.torque += torque;
    return math::length_squared(torque) == 0.0 || wake(handle);
}

bool PhysicsWorld::apply_impulse(BodyHandle handle, math::Vec3 impulse) {
    const auto* body = get(handle);
    return body && apply_impulse(handle, impulse, center_of_mass(*body));
}

bool PhysicsWorld::apply_impulse(BodyHandle handle, math::Vec3 impulse, math::Vec3 world_point) {
    auto* entry = record(handle);
    if (!entry || entry->body.mode != BodyMode::Dynamic || !bounded_vector(impulse) || !bounded_vector(world_point)) {
        return false;
    }
    auto& body = entry->body;
    body.linear_velocity += impulse * body.inverse_mass;
    body.angular_velocity += inverse_inertia_world(body) * math::cross(world_point - center_of_mass(body), impulse);
    limit_velocity(body);
    publish_velocity(body);
    return math::length_squared(impulse) == 0.0 || wake(handle);
}

bool PhysicsWorld::apply_angular_impulse(BodyHandle handle, math::Vec3 impulse) {
    auto* entry = record(handle);
    if (!entry || entry->body.mode != BodyMode::Dynamic || !bounded_vector(impulse, 1.0e24)) {
        return false;
    }
    entry->body.angular_velocity += inverse_inertia_world(entry->body) * impulse;
    limit_velocity(entry->body);
    publish_velocity(entry->body);
    return math::length_squared(impulse) == 0.0 || wake(handle);
}

bool PhysicsWorld::clear_forces(BodyHandle handle) {
    auto* entry = record(handle);
    if (!entry) {
        return false;
    }
    entry->body.force = {};
    entry->body.torque = {};
    return true;
}

bool PhysicsWorld::apply_position_delta(BodyHandle handle, math::Vec3 translation, math::Vec3 rotation_vector) {
    auto* entry = record(handle);
    if (!entry || entry->body.mode != BodyMode::Dynamic || !bounded_vector(translation) ||
        !bounded_vector(rotation_vector, 1.0e6)) {
        return false;
    }
    auto pose = entry->body.pose;
    const auto next_center = center_of_mass(entry->body) + translation;
    pose.rotation = math::integrate_angular_velocity(pose.rotation, rotation_vector, 1.0);
    pose.position = next_center - math::rotate(pose.rotation, entry->body.mass.center_of_mass * pose.scale);
    if (!valid_pose(pose)) {
        return false;
    }
    if (!stepping_) {
        return set_pose(handle, pose);
    }
    entry->body.pose = pose;
    return true;
}

bool PhysicsWorld::shift_origin(math::Vec3 offset) {
    if (stepping_ || registry_.iterating() || !bounded_vector(offset)) {
        return false;
    }
    edits_.clear();
    for (const auto& entry : slots_) {
        if (!entry || !get(entry->body.handle)) {
            continue;
        }
        auto shifted = entry->body.pose;
        shifted.position -= offset;
        if (!valid_pose(shifted)) {
            return false;
        }
        edits_.push_back({entry->body.entity, math::matrix(shifted)});
    }
    if (!scene_.apply_world_matrices(edits_)) {
        return false;
    }
    for (auto& entry : slots_) {
        if (entry && get(entry->body.handle)) {
            entry->body.pose.position -= offset;
            entry->body.previous_pose.position -= offset;
            entry->published_pose.position -= offset;
            entry->checkpoint.position -= offset;
            if (entry->target) {
                entry->target->position -= offset;
            }
        }
    }
    tracked_contacts_.clear();
    events_.clear();
    solver_points_.clear();
    const auto synchronized = collision_.sync(scene_);
    statistics_.collider_rejections += synchronized.rejected;
    return true;
}

void PhysicsWorld::synchronize_scene() {
    (void)scene_.update();
    const auto synchronized = collision_.sync(scene_);
    statistics_.collider_rejections += synchronized.rejected;
    for (auto& entry : slots_) {
        if (!entry) {
            continue;
        }
        const auto handle = entry->body.handle;
        if (!get(handle) || !scene_.contains(entry->body.entity)) {
            (void)erase(handle);
            ++statistics_.removed_stale;
            continue;
        }
        auto& body = entry->body;
        const auto transform = scene_.world_transform(body.entity);
        body.scene_enabled = transform && valid_pose(*transform) &&
            scene_.world_transform_exact(body.entity) && scene_.effective_enabled(body.entity);
        if (body.scene_enabled && !same_pose(*transform, entry->published_pose)) {
            body.pose = *transform;
            body.pose.rotation = math::normalized(body.pose.rotation);
            entry->published_pose = body.pose;
            (void)wake(handle);
        }
        body.previous_pose = body.pose;
        entry->checkpoint = body.pose;
    }
}

void PhysicsWorld::synchronize_colliders() {
    for (auto& entry : slots_) {
        if (!entry || !entry->body.scene_enabled || !get(entry->body.handle)) {
            continue;
        }
        const auto colliders = collision_.colliders(entry->body.entity);
        bool accepted = true;
        for (const auto collider : colliders) {
            if (!collision_.set_transform(collider, entry->body.pose)) {
                ++statistics_.collider_rejections;
                accepted = false;
            }
        }
        if (!accepted) {
            entry->body.pose = entry->checkpoint;
            entry->body.linear_velocity = {};
            entry->body.angular_velocity = {};
            for (const auto collider : colliders) {
                (void)collision_.set_transform(collider, entry->body.pose);
            }
        }
    }
}

bool PhysicsWorld::publish_poses() {
    edits_.clear();
    for (const auto& entry : slots_) {
        if (entry && entry->body.scene_enabled && get(entry->body.handle)) {
            edits_.push_back({entry->body.entity, math::matrix(entry->body.pose)});
        }
    }
    const bool accepted = static_cast<bool>(scene_.apply_world_matrices(edits_));
    if (!accepted) {
        statistics_.scene_rejections += edits_.size();
    }
    for (auto& entry : slots_) {
        if (!entry || !entry->body.scene_enabled) {
            continue;
        }
        if (accepted) {
            entry->published_pose = entry->body.pose;
        } else {
            entry->body.pose = entry->checkpoint;
            entry->body.linear_velocity = {};
            entry->body.angular_velocity = {};
        }
        publish_velocity(entry->body);
    }
    return accepted;
}

void PhysicsWorld::integrate(double delta_seconds, double remaining_seconds) {
    for (auto& entry : slots_) {
        if (!entry) {
            continue;
        }
        auto& body = entry->body;
        entry->checkpoint = body.pose;
        if (!body.enabled || !body.scene_enabled || body.mode == BodyMode::Static ||
            (body.mode == BodyMode::Dynamic && !body.awake)) {
            continue;
        }
        if (body.mode == BodyMode::Kinematic && entry->target) {
            const auto target_center = math::transform_point(*entry->target, body.mass.center_of_mass);
            body.linear_velocity = (target_center - center_of_mass(body)) / remaining_seconds;
            auto difference = entry->target->rotation * math::conjugate(body.pose.rotation);
            if (difference.w < 0.0) {
                difference = -difference;
            }
            const auto [axis, angle] = math::to_axis_angle(difference);
            body.angular_velocity = axis * (angle / remaining_seconds);
            if (same_pose(body.pose, *entry->target)) {
                body.linear_velocity = {};
                body.angular_velocity = {};
                entry->target.reset();
            }
        }
        if (body.mode == BodyMode::Dynamic) {
            body.linear_velocity += (config_.gravity * body.gravity_scale + body.force * body.inverse_mass) * delta_seconds;
            body.angular_velocity += (inverse_inertia_world(body) * body.torque) * delta_seconds;
            body.linear_velocity *= std::exp(-body.linear_drag * delta_seconds);
            body.angular_velocity *= std::exp(-body.angular_drag * delta_seconds);
        }
        limit_velocity(body);
        const auto next_center = center_of_mass(body) + body.linear_velocity * delta_seconds;
        body.pose.rotation = math::integrate_angular_velocity(body.pose.rotation, body.angular_velocity, delta_seconds);
        body.pose.position = next_center - math::rotate(body.pose.rotation, body.mass.center_of_mass * body.pose.scale);
        if (!valid_pose(body.pose)) {
            body.pose = entry->checkpoint;
            body.linear_velocity = {};
            body.angular_velocity = {};
            ++statistics_.scene_rejections;
        }
    }
}

void PhysicsWorld::prepare_contacts(std::span<const ContactManifold> contacts) {
    solver_points_.clear();
    for (const auto& contact : contacts) {
        if (contact.trigger || !contact.touching() || !math::finite(contact.normal)) {
            continue;
        }
        const auto first_handle = find(contact.first_entity);
        const auto second_handle = find(contact.second_entity);
        if (first_handle && first_handle == second_handle) {
            continue;
        }
        auto* first_record = record(first_handle);
        auto* second_record = record(second_handle);
        auto* first = first_record ? &first_record->body : nullptr;
        auto* second = second_record ? &second_record->body : nullptr;
        const auto moving = [this](const BodyState* body) {
            if (!body || !body->enabled || !body->scene_enabled || body->mode == BodyMode::Static) {
                return false;
            }
            const double threshold = body->mode == BodyMode::Kinematic ? math::epsilon :
                std::max(0.2, config_.sleep_linear_speed * 4.0);
            return math::length_squared(body->linear_velocity) > threshold * threshold ||
                   math::length_squared(body->angular_velocity) > threshold * threshold;
        };
        double penetration{};
        const auto count = std::min(contact.point_count, contact.points.size());
        for (std::size_t point = 0; point < count; ++point) {
            penetration = std::max(penetration, contact.points[point].penetration);
        }
        const bool deep = penetration > std::max(0.01, config_.penetration_slop * 4.0);
        if (first && !first->awake && (moving(second) || deep)) {
            (void)wake(first_handle);
        }
        if (second && !second->awake && (moving(first) || deep)) {
            (void)wake(second_handle);
        }
        if (!responsive(first) && !responsive(second)) {
            continue;
        }
        const auto normal = math::normalized(contact.normal);
        if (math::length_squared(normal) < 0.5) {
            continue;
        }
        const auto tangents = math::orthonormal_basis(normal);
        for (std::size_t index = 0; index < count; ++index) {
            if (solver_points_.size() >= config_.max_solver_points) {
                statistics_.contacts_truncated = true;
                return;
            }
            const auto& source = contact.points[index];
            if (!bounded_vector(source.point_a) || !bounded_vector(source.point_b) ||
                !std::isfinite(source.penetration)) {
                continue;
            }
            SolverPoint point;
            point.first = first_handle;
            point.second = second_handle;
            point.first_collider = contact.first;
            point.second_collider = contact.second;
            point.normal = normal;
            point.tangent_one = tangents.first;
            point.tangent_two = tangents.second;
            point.first_offset = first ? source.point_a - center_of_mass(*first) : math::Vec3{};
            point.second_offset = second ? source.point_b - center_of_mass(*second) : math::Vec3{};
            point.first_anchor = first ? math::rotate(math::conjugate(first->pose.rotation), point.first_offset) : source.point_a;
            point.second_anchor = second ? math::rotate(math::conjugate(second->pose.rotation), point.second_offset) : source.point_b;
            point.penetration = source.penetration + math::dot(source.point_b - source.point_a, normal);
            const auto effective = [&](math::Vec3 direction) {
                const double denominator = inverse_effective_mass(first, point.first_offset, direction) +
                    inverse_effective_mass(second, point.second_offset, direction);
                return std::isfinite(denominator) && denominator > 0.0 ? 1.0 / denominator : 0.0;
            };
            point.normal_mass = effective(normal);
            point.tangent_mass_one = effective(point.tangent_one);
            point.tangent_mass_two = effective(point.tangent_two);
            const double approach = math::dot(velocity_at(second, point.second_offset) - velocity_at(first, point.first_offset), normal);
            const double restitution = std::isfinite(contact.restitution) ? std::clamp(contact.restitution, 0.0, 1.0) : 0.0;
            point.restitution_speed = approach < -config_.restitution_threshold ? -approach * restitution : 0.0;
            point.friction = std::isfinite(contact.friction) ? std::clamp(contact.friction, 0.0, 100.0) : 0.0;
            solver_points_.push_back(point);
        }
    }
}

void PhysicsWorld::impulse_pair(const SolverPoint& point, math::Vec3 impulse) {
    if (auto* entry = record(point.first); entry && responsive(&entry->body)) {
        auto& body = entry->body;
        body.linear_velocity -= impulse * body.inverse_mass;
        body.angular_velocity -= inverse_inertia_world(body) * math::cross(point.first_offset, impulse);
    }
    if (auto* entry = record(point.second); entry && responsive(&entry->body)) {
        auto& body = entry->body;
        body.linear_velocity += impulse * body.inverse_mass;
        body.angular_velocity += inverse_inertia_world(body) * math::cross(point.second_offset, impulse);
    }
}

void PhysicsWorld::solve_velocities() {
    for (auto& point : solver_points_) {
        auto relative = velocity_at(get(point.second), point.second_offset) - velocity_at(get(point.first), point.first_offset);
        const double normal_speed = math::dot(relative, point.normal);
        const double previous_normal = point.normal_impulse;
        point.normal_impulse = std::max(0.0, previous_normal + point.normal_mass * (point.restitution_speed - normal_speed));
        impulse_pair(point, point.normal * (point.normal_impulse - previous_normal));
        relative = velocity_at(get(point.second), point.second_offset) - velocity_at(get(point.first), point.first_offset);
        const double previous_one = point.tangent_impulse_one;
        const double previous_two = point.tangent_impulse_two;
        point.tangent_impulse_one -= math::dot(relative, point.tangent_one) * point.tangent_mass_one;
        point.tangent_impulse_two -= math::dot(relative, point.tangent_two) * point.tangent_mass_two;
        const double magnitude = std::hypot(point.tangent_impulse_one, point.tangent_impulse_two);
        const double maximum = point.friction * point.normal_impulse;
        if (magnitude > maximum && magnitude > math::epsilon) {
            point.tangent_impulse_one *= maximum / magnitude;
            point.tangent_impulse_two *= maximum / magnitude;
        }
        impulse_pair(point, point.tangent_one * (point.tangent_impulse_one - previous_one) +
                            point.tangent_two * (point.tangent_impulse_two - previous_two));
    }
}

void PhysicsWorld::solve_positions() {
    for (const auto& point : solver_points_) {
        const auto* first = get(point.first);
        const auto* second = get(point.second);
        const auto first_offset = first ? math::rotate(first->pose.rotation, point.first_anchor) : math::Vec3{};
        const auto second_offset = second ? math::rotate(second->pose.rotation, point.second_anchor) : math::Vec3{};
        const auto first_point = first ? center_of_mass(*first) + first_offset : point.first_anchor;
        const auto second_point = second ? center_of_mass(*second) + second_offset : point.second_anchor;
        const double depth = point.penetration - math::dot(second_point - first_point, point.normal) - config_.penetration_slop;
        const double denominator = inverse_effective_mass(first, first_offset, point.normal) +
            inverse_effective_mass(second, second_offset, point.normal);
        if (depth <= 0.0 || denominator <= 0.0 || !std::isfinite(depth) || !std::isfinite(denominator)) {
            continue;
        }
        const double magnitude = std::min(config_.max_position_correction, depth * config_.correction_fraction) / denominator;
        const auto impulse = point.normal * magnitude;
        if (responsive(first)) {
            const auto rotation = math::clamp_length(inverse_inertia_world(*first) * math::cross(first_offset, -impulse), 0.2);
            (void)apply_position_delta(point.first, -impulse * first->inverse_mass, rotation);
        }
        if (responsive(second)) {
            const auto rotation = math::clamp_length(inverse_inertia_world(*second) * math::cross(second_offset, impulse), 0.2);
            (void)apply_position_delta(point.second, impulse * second->inverse_mass, rotation);
        }
    }
}

void PhysicsWorld::append_event(const PhysicsContactEvent& event) {
    if (events_.size() < config_.max_events) {
        events_.push_back(event);
    } else {
        ++statistics_.dropped_events;
    }
}

void PhysicsWorld::update_contacts(std::span<const ContactManifold> contacts) {
    std::map<ContactKey, PhysicsContactEvent> current;
    statistics_.contacts = contacts.size();
    statistics_.triggers = 0;
    for (const auto& contact : contacts) {
        if (contact.trigger) {
            ++statistics_.triggers;
        }
        if (!contact.touching()) {
            continue;
        }
        if (current.size() >= config_.max_events) {
            ++statistics_.dropped_events;
            continue;
        }
        const ContactKey key{contact.first, contact.second};
        PhysicsContactEvent event;
        event.kind = tracked_contacts_.contains(key) ? ContactEventKind::Persist : ContactEventKind::Begin;
        event.first = contact.first;
        event.second = contact.second;
        event.first_entity = contact.first_entity;
        event.second_entity = contact.second_entity;
        event.normal = contact.normal;
        event.point = contact.points.front().position();
        event.penetration = contact.points.front().penetration;
        event.trigger = contact.trigger;
        event.step = steps_;
        current.emplace(key, event);
    }
    for (const auto& point : solver_points_) {
        const auto found = current.find({point.first_collider, point.second_collider});
        if (found != current.end()) {
            found->second.normal_impulse += point.normal_impulse;
        }
    }
    for (const auto& [key, previous] : tracked_contacts_) {
        if (!current.contains(key)) {
            auto ended = previous;
            ended.kind = ContactEventKind::End;
            ended.normal_impulse = 0.0;
            ended.penetration = 0.0;
            ended.step = steps_;
            append_event(ended);
        }
    }
    for (const auto& [key, event] : current) {
        (void)key;
        append_event(event);
    }
    tracked_contacts_.swap(current);
}

void PhysicsWorld::update_sleep(double delta_seconds) {
    for (auto& entry : slots_) {
        if (!entry || !responsive(&entry->body)) {
            continue;
        }
        auto& body = entry->body;
        limit_velocity(body);
        const bool quiet = math::length_squared(body.linear_velocity) <= config_.sleep_linear_speed * config_.sleep_linear_speed &&
            math::length_squared(body.angular_velocity) <= config_.sleep_angular_speed * config_.sleep_angular_speed &&
            math::length_squared(body.force) == 0.0 && math::length_squared(body.torque) == 0.0;
        body.quiet_seconds = quiet && body.allow_sleep ? body.quiet_seconds + delta_seconds : 0.0;
        if (quiet && body.allow_sleep && body.quiet_seconds >= config_.sleep_after_seconds) {
            (void)sleep(body.handle);
        } else {
            publish_velocity(body);
        }
    }
}

void PhysicsWorld::invoke_solvers(ConstraintStage stage, double delta_seconds, std::uint32_t iteration) {
    const ConstraintContext context{stage, delta_seconds, iteration, steps_};
    for (auto& hook : hooks_) {
        if (hook.enabled) {
            try {
                hook.solver(*this, context);
            } catch (...) {
                hook.enabled = false;
                ++statistics_.solver_failures;
            }
        }
    }
}

bool PhysicsWorld::add_constraint_solver(std::string name, ConstraintSolver solver) {
    if (stepping_ || !solver || name.empty() || name.size() > 128 || hooks_.size() >= config_.max_constraint_solvers) {
        return false;
    }
    for (const auto& hook : hooks_) {
        if (hook.name == name) {
            return false;
        }
    }
    hooks_.push_back({std::move(name), std::move(solver), true});
    return true;
}

bool PhysicsWorld::remove_constraint_solver(std::string_view name) {
    if (stepping_) {
        return false;
    }
    const auto found = std::find_if(hooks_.begin(), hooks_.end(), [name](const Hook& hook) { return hook.name == name; });
    if (found == hooks_.end()) {
        return false;
    }
    hooks_.erase(found);
    return true;
}

std::size_t PhysicsWorld::constraint_solver_count() const noexcept { return hooks_.size(); }
std::span<const PhysicsContactEvent> PhysicsWorld::events() const noexcept { return events_; }
void PhysicsWorld::clear_events() noexcept { events_.clear(); }

void PhysicsWorld::refresh_statistics() {
    statistics_.bodies = size();
    statistics_.dynamic_bodies = 0;
    statistics_.kinematic_bodies = 0;
    statistics_.sleeping_bodies = 0;
    statistics_.disabled_bodies = 0;
    statistics_.kinetic_energy = 0.0;
    for (const auto& entry : slots_) {
        if (!entry) {
            continue;
        }
        const auto& body = entry->body;
        statistics_.dynamic_bodies += body.mode == BodyMode::Dynamic ? 1U : 0U;
        statistics_.kinematic_bodies += body.mode == BodyMode::Kinematic ? 1U : 0U;
        statistics_.sleeping_bodies += body.mode == BodyMode::Dynamic && !body.awake ? 1U : 0U;
        statistics_.disabled_bodies += !body.enabled || !body.scene_enabled ? 1U : 0U;
        statistics_.kinetic_energy += kinetic_energy(body);
    }
    statistics_.steps = steps_;
}

PhysicsStatistics PhysicsWorld::step(double delta_seconds) {
    if (stepping_ || registry_.iterating()) {
        auto rejected = statistics_;
        rejected.busy = true;
        return rejected;
    }
    statistics_ = {};
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) {
        statistics_.invalid_delta = true;
        refresh_statistics();
        return statistics_;
    }
    struct StepScope {
        bool& flag;
        explicit StepScope(bool& value) : flag(value) { flag = true; }
        ~StepScope() { flag = false; }
    } scope(stepping_);
    events_.clear();
    synchronize_scene();
    const double capacity_seconds = config_.max_substep_seconds * static_cast<double>(config_.max_substeps);
    const double accepted = std::min({delta_seconds, config_.max_frame_seconds, capacity_seconds});
    statistics_.dropped_seconds = delta_seconds - accepted;
    if (accepted == 0.0) {
        refresh_statistics();
        return statistics_;
    }
    const double needed = std::ceil(accepted / config_.max_substep_seconds);
    const auto count = std::min(config_.max_substeps, static_cast<std::uint32_t>(needed));
    const double delta = accepted / static_cast<double>(count);
    for (std::uint32_t substep = 0; substep < count; ++substep) {
        if (steps_ != std::numeric_limits<std::uint64_t>::max()) {
            ++steps_;
        }
        const double remaining = delta * static_cast<double>(count - substep);
        invoke_solvers(ConstraintStage::BeforeIntegrate, delta, 0);
        integrate(delta, remaining);
        invoke_solvers(ConstraintStage::AfterIntegrate, delta, 0);
        synchronize_colliders();
        const auto detected = collision_.detect();
        statistics_.contacts_truncated = statistics_.contacts_truncated || detected.contacts_truncated || detected.pairs_truncated;
        prepare_contacts(collision_.contacts());
        invoke_solvers(ConstraintStage::Prepare, delta, 0);
        for (std::uint32_t iteration = 0; iteration < config_.velocity_iterations; ++iteration) {
            invoke_solvers(ConstraintStage::Velocity, delta, iteration);
            solve_velocities();
        }
        for (std::uint32_t iteration = 0; iteration < config_.position_iterations; ++iteration) {
            solve_positions();
            invoke_solvers(ConstraintStage::Position, delta, iteration);
        }
        invoke_solvers(ConstraintStage::Finish, delta, 0);
        const bool published = publish_poses();
        if (published) {
            update_sleep(delta);
        } else {
            solver_points_.clear();
        }
        const auto synchronized = collision_.sync(scene_);
        statistics_.collider_rejections += synchronized.rejected;
        const auto final_detection = collision_.detect();
        statistics_.contacts_truncated = statistics_.contacts_truncated || final_detection.contacts_truncated;
        update_contacts(collision_.contacts());
        statistics_.solver_points = solver_points_.size();
        if (!published) {
            statistics_.dropped_seconds += delta * static_cast<double>(count - substep);
            break;
        }
        ++statistics_.substeps;
        statistics_.simulated_seconds += delta;
    }
    for (auto& entry : slots_) {
        if (entry) {
            entry->body.force = {};
            entry->body.torque = {};
        }
    }
    refresh_statistics();
    return statistics_;
}

void register_dynamics(core::Kernel& kernel) {
    if (kernel.services().find<PhysicsWorld>()) {
        return;
    }
    auto* scene_service = kernel.services().find<scene::Scene>();
    auto* collision_service = kernel.services().find<CollisionWorld>();
    if (!scene_service || !collision_service) {
        throw std::logic_error("dynamics registration requires Scene and CollisionWorld services");
    }
    PhysicsConfig configured;
    configured.capacity = static_cast<std::uint32_t>(std::clamp<std::int64_t>(kernel.config().integer("physics.body_capacity", 16384), 0, 1000000));
    configured.max_substeps = static_cast<std::uint32_t>(std::clamp<std::int64_t>(kernel.config().integer("physics.max_substeps", 16), 1, 256));
    configured.velocity_iterations = static_cast<std::uint32_t>(std::clamp<std::int64_t>(kernel.config().integer("physics.velocity_iterations", 8), 1, 128));
    configured.position_iterations = static_cast<std::uint32_t>(std::clamp<std::int64_t>(kernel.config().integer("physics.position_iterations", 3), 0, 64));
    configured.gravity.y = kernel.config().number("physics.gravity_y", -9.81);
    configured.max_substep_seconds = kernel.config().number("physics.max_substep", 1.0 / 120.0);
    (void)kernel.services().emplace<PhysicsWorld>(*scene_service, *collision_service, configured);
    (void)kernel.register_module({"physics.dynamics", "Generation-checked rigid bodies and bounded contact impulses", 1});
    (void)kernel.add_system({"physics.dynamics.step", core::Phase::Simulation, 1000, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& physics = host.services().get<PhysicsWorld>();
            if (host.config().boolean("physics.enabled", true)) {
                (void)physics.step(frame.delta_seconds);
                publish_runtime(host, physics);
            }
        });
    (void)kernel.add_command("physics.body", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 3) {
            return core::CommandResult{false, "usage: physics.body entity [static|kinematic|dynamic|remove] [mass]"};
        }
        auto& physics = host.services().get<PhysicsWorld>();
        const auto entity = resolve_entity(physics, args[0]);
        if (!entity) {
            return core::CommandResult{false, "entity name or index:generation did not resolve uniquely"};
        }
        if (args.size() >= 2 && args[1] == "remove") {
            return core::CommandResult{args.size() == 2 && physics.remove(entity), "body removal requested"};
        }
        if (args.size() == 1) {
            const auto* body = physics.get(entity);
            return core::CommandResult{body != nullptr, body ? "body " + std::to_string(body->handle.index) + ":" +
                std::to_string(body->handle.generation) + " mass=" + std::to_string(body->mass.mass) +
                " awake=" + (body->awake ? "true" : "false") : "entity has no body"};
        }
        BodyDescription description;
        if (args[1] == "static") {
            description.mode = BodyMode::Static;
        } else if (args[1] == "kinematic") {
            description.mode = BodyMode::Kinematic;
        } else if (args[1] != "dynamic") {
            return core::CommandResult{false, "unknown body mode"};
        }
        if (args.size() == 3) {
            const auto mass = parse_number(args[2]);
            const auto properties = mass ? box_mass(*mass, {0.5, 0.5, 0.5}) : std::nullopt;
            if (!properties) {
                return core::CommandResult{false, "mass is outside supported bounds"};
            }
            description.mass = *properties;
        }
        if (const auto existing = physics.find(entity)) {
            const bool changed = physics.set_mass(existing, description.mass) && physics.set_mode(existing, description.mode);
            return core::CommandResult{changed, changed ? "body updated" : "body update rejected"};
        }
        const auto added = physics.add(entity, description);
        return core::CommandResult{static_cast<bool>(added), added ? "body created" : "body creation rejected"};
    }, "Inspect, attach, configure, or remove an entity rigid body");
    (void)kernel.add_command("physics.force", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 4 && args.size() != 7) {
            return core::CommandResult{false, "usage: physics.force entity fx fy fz [px py pz]"};
        }
        auto& physics = host.services().get<PhysicsWorld>();
        const auto body = physics.find(resolve_entity(physics, args[0]));
        const auto force = parse_vector(args, 1);
        const auto point = args.size() == 7 ? parse_vector(args, 4) : std::optional<math::Vec3>{};
        const bool applied = force && (args.size() == 4 ? physics.apply_force(body, *force) :
            point && physics.apply_force(body, *force, *point));
        return core::CommandResult{applied, applied ? "force queued for next step" : "force rejected"};
    }, "Accumulate a world-space force and optional point torque");
    (void)kernel.add_command("physics.velocity", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 4 && args.size() != 7) {
            return core::CommandResult{false, "usage: physics.velocity entity vx vy vz [wx wy wz]"};
        }
        auto& physics = host.services().get<PhysicsWorld>();
        const auto body = physics.find(resolve_entity(physics, args[0]));
        const auto linear = parse_vector(args, 1);
        const auto angular = args.size() == 7 ? parse_vector(args, 4) : std::optional<math::Vec3>{math::Vec3{}};
        const bool changed = linear && angular && physics.set_velocity(body, *linear, *angular);
        return core::CommandResult{changed, changed ? "velocity updated" : "velocity rejected"};
    }, "Set world linear velocity and angular velocity in radians per second");
    (void)kernel.add_command("physics.step", [](core::Kernel& host, core::CommandArgs args) {
        const auto delta = args.size() == 1 ? parse_number(args.front()) : std::nullopt;
        if (!delta || *delta < 0.0) {
            return core::CommandResult{false, "usage: physics.step nonnegative_seconds"};
        }
        auto& physics = host.services().get<PhysicsWorld>();
        const auto stats = physics.step(*delta);
        publish_runtime(host, physics);
        return core::CommandResult{!stats.busy && !stats.invalid_delta, "substeps=" + std::to_string(stats.substeps) +
            " simulated=" + std::to_string(stats.simulated_seconds) + " dropped=" + std::to_string(stats.dropped_seconds)};
    }, "Advance dynamics manually using bounded substeps");
    (void)kernel.add_command("physics.stats", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: physics.stats"};
        }
        const auto& stats = host.services().get<PhysicsWorld>().statistics();
        return core::CommandResult{true, "bodies=" + std::to_string(stats.bodies) + " dynamic=" +
            std::to_string(stats.dynamic_bodies) + " sleeping=" + std::to_string(stats.sleeping_bodies) +
            " contacts=" + std::to_string(stats.contacts) + " energy=" + std::to_string(stats.kinetic_energy) +
            " stale_removed=" + std::to_string(stats.removed_stale) + " scene_rejections=" + std::to_string(stats.scene_rejections)};
    }, "Report body, contact, energy, and integration counters");
}

} // namespace ge::physics
