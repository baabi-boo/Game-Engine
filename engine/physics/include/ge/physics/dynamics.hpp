#pragma once

#include <ge/physics/collision.hpp>
#include <ge/scene/scene.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::physics {

struct BodyHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const BodyHandle&) const = default;
};

enum class BodyMode : std::uint8_t { Static, Kinematic, Dynamic };

struct BodyComponent {
    BodyHandle body{};
};

struct MassProperties {
    double mass{1.0};
    math::Vec3 inertia{1.0, 1.0, 1.0};
    math::Vec3 center_of_mass{};
};

[[nodiscard]] std::optional<MassProperties> sphere_mass(double mass, double radius) noexcept;
[[nodiscard]] std::optional<MassProperties> box_mass(double mass, math::Vec3 half_extents) noexcept;
[[nodiscard]] bool valid_mass(const MassProperties& properties) noexcept;

struct BodyDescription {
    BodyMode mode{BodyMode::Dynamic};
    MassProperties mass{};
    math::Vec3 linear_velocity{};
    math::Vec3 angular_velocity{};
    double linear_drag{0.02};
    double angular_drag{0.05};
    double gravity_scale{1.0};
    bool allow_sleep{true};
    bool awake{true};
    bool enabled{true};
};

struct BodyState {
    BodyHandle handle{};
    ecs::Entity entity{};
    BodyMode mode{BodyMode::Dynamic};
    math::Transform pose{};
    math::Transform previous_pose{};
    MassProperties mass{};
    double inverse_mass{};
    math::Vec3 inverse_inertia{};
    math::Vec3 linear_velocity{};
    math::Vec3 angular_velocity{};
    math::Vec3 force{};
    math::Vec3 torque{};
    double linear_drag{};
    double angular_drag{};
    double gravity_scale{1.0};
    double quiet_seconds{};
    bool allow_sleep{true};
    bool awake{true};
    bool enabled{true};
    bool scene_enabled{true};
};

[[nodiscard]] math::Vec3 center_of_mass(const BodyState& body) noexcept;
[[nodiscard]] math::Mat3 inverse_inertia_world(const BodyState& body) noexcept;
[[nodiscard]] math::Vec3 point_velocity(const BodyState& body, math::Vec3 world_point) noexcept;
[[nodiscard]] double kinetic_energy(const BodyState& body) noexcept;

struct PhysicsConfig {
    std::uint32_t capacity{16384};
    std::uint32_t max_substeps{16};
    std::uint32_t velocity_iterations{8};
    std::uint32_t position_iterations{3};
    std::size_t max_solver_points{65536};
    std::size_t max_events{16384};
    std::size_t max_constraint_solvers{16};
    double max_substep_seconds{1.0 / 120.0};
    double max_frame_seconds{0.25};
    double max_linear_speed{1000.0};
    double max_angular_speed{100.0};
    double penetration_slop{0.001};
    double correction_fraction{0.25};
    double max_position_correction{0.2};
    double restitution_threshold{1.0};
    double sleep_linear_speed{0.05};
    double sleep_angular_speed{0.05};
    double sleep_after_seconds{0.5};
    math::Vec3 gravity{0.0, -9.81, 0.0};
};

enum class ContactEventKind : std::uint8_t { Begin, Persist, End };

struct PhysicsContactEvent {
    ContactEventKind kind{ContactEventKind::Begin};
    ColliderHandle first{};
    ColliderHandle second{};
    ecs::Entity first_entity{};
    ecs::Entity second_entity{};
    math::Vec3 normal{};
    math::Vec3 point{};
    double penetration{};
    double normal_impulse{};
    bool trigger{};
    std::uint64_t step{};
};

struct PhysicsStatistics {
    std::size_t bodies{};
    std::size_t dynamic_bodies{};
    std::size_t kinematic_bodies{};
    std::size_t sleeping_bodies{};
    std::size_t disabled_bodies{};
    std::size_t removed_stale{};
    std::size_t contacts{};
    std::size_t solver_points{};
    std::size_t triggers{};
    std::size_t scene_rejections{};
    std::size_t collider_rejections{};
    std::size_t dropped_events{};
    std::size_t solver_failures{};
    std::uint32_t substeps{};
    std::uint64_t steps{};
    double simulated_seconds{};
    double dropped_seconds{};
    double kinetic_energy{};
    bool contacts_truncated{};
    bool invalid_delta{};
    bool busy{};
};

enum class ConstraintStage : std::uint8_t {
    Prepare, Velocity, Position, Finish, BeforeIntegrate, AfterIntegrate
};

struct ConstraintContext {
    ConstraintStage stage{ConstraintStage::Prepare};
    double delta_seconds{};
    std::uint32_t iteration{};
    std::uint64_t step{};
};

class PhysicsWorld;
using ConstraintSolver = std::function<void(PhysicsWorld&, const ConstraintContext&)>;

// Single writer; Scene and CollisionWorld must borrow the same Registry. Poses
// are world TRS, velocities are world-space COM velocities, and inertia is a
// positive local diagonal supplied in physical units. Scaling a pose does not
// automatically recompute inertia. Components refer to owned generation handles.
class PhysicsWorld final {
public:
    explicit PhysicsWorld(ge::scene::Scene& scene, CollisionWorld& collision,
                          PhysicsConfig configuration = {});
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    [[nodiscard]] ecs::Registry& registry() noexcept;
    [[nodiscard]] ge::scene::Scene& scene() noexcept;
    [[nodiscard]] CollisionWorld& collision() noexcept;
    [[nodiscard]] const PhysicsConfig& config() const noexcept;
    [[nodiscard]] const PhysicsStatistics& statistics() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool stepping() const noexcept;
    [[nodiscard]] BodyHandle add(ecs::Entity entity, const BodyDescription& description = {});
    bool remove(BodyHandle handle);
    bool remove(ecs::Entity entity);
    void clear();
    [[nodiscard]] BodyHandle find(ecs::Entity entity) const noexcept;
    [[nodiscard]] const BodyState* get(BodyHandle handle) const noexcept;
    [[nodiscard]] const BodyState* get(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::optional<BodyState> state(BodyHandle handle) const noexcept;
    [[nodiscard]] std::vector<BodyHandle> bodies() const;

    bool set_mode(BodyHandle handle, BodyMode mode);
    bool set_mass(BodyHandle handle, const MassProperties& properties);
    // Disabling a body pauses its response; independently owned colliders remain.
    bool set_enabled(BodyHandle handle, bool enabled);
    bool set_sleep_allowed(BodyHandle handle, bool allowed);
    bool wake(BodyHandle handle);
    bool sleep(BodyHandle handle);
    bool set_velocity(BodyHandle handle, math::Vec3 linear, math::Vec3 angular = {});
    bool set_pose(BodyHandle handle, const math::Transform& pose, bool clear_velocity = false);
    bool set_kinematic_target(BodyHandle handle, const math::Transform& target);
    bool set_drag(BodyHandle handle, double linear, double angular);
    bool set_gravity(math::Vec3 gravity) noexcept;
    bool set_gravity_scale(BodyHandle handle, double scale);
    bool apply_force(BodyHandle handle, math::Vec3 force);
    bool apply_force(BodyHandle handle, math::Vec3 force, math::Vec3 world_point);
    bool apply_torque(BodyHandle handle, math::Vec3 torque);
    bool apply_impulse(BodyHandle handle, math::Vec3 impulse);
    bool apply_impulse(BodyHandle handle, math::Vec3 impulse, math::Vec3 world_point);
    bool apply_angular_impulse(BodyHandle handle, math::Vec3 impulse);
    bool clear_forces(BodyHandle handle);
    // Translation and rotation-vector corrections do not change velocities.
    bool apply_position_delta(BodyHandle handle, math::Vec3 translation,
                              math::Vec3 rotation_vector = {});
    // Shifts owned bodies and clears contact history; the caller shifts other
    // scene content separately.
    bool shift_origin(math::Vec3 offset);

    // Forces last for one public step and are reused across its bounded substeps.
    // Excess frame time is explicitly reported as dropped_seconds.
    [[nodiscard]] PhysicsStatistics step(double delta_seconds);
    [[nodiscard]] std::span<const PhysicsContactEvent> events() const noexcept;
    void clear_events() noexcept;
    // Named callbacks execute in registration order. BeforeIntegrate sees the
    // substep's initial body poses; AfterIntegrate sees integrated body poses
    // before collider synchronization and discrete contact detection. These
    // stages support swept collision without adding a second body integrator.
    // CollisionWorld still holds initial poses during AfterIntegrate. Hooks may
    // correct owned bodies through this PhysicsWorld; do not write Scene there.
    // Structural mutation and hook replacement are rejected during stepping;
    // throwing hooks are disabled. All other stages retain their solver timing.
    bool add_constraint_solver(std::string name, ConstraintSolver solver);
    bool remove_constraint_solver(std::string_view name);
    [[nodiscard]] std::size_t constraint_solver_count() const noexcept;

private:
    struct Record {
        BodyState body{};
        math::Transform published_pose{};
        math::Transform checkpoint{};
        std::optional<math::Transform> target;
    };
    struct SolverPoint {
        BodyHandle first{};
        BodyHandle second{};
        ColliderHandle first_collider{};
        ColliderHandle second_collider{};
        math::Vec3 normal{};
        math::Vec3 tangent_one{};
        math::Vec3 tangent_two{};
        math::Vec3 first_offset{};
        math::Vec3 second_offset{};
        math::Vec3 first_anchor{};
        math::Vec3 second_anchor{};
        double penetration{};
        double normal_mass{};
        double tangent_mass_one{};
        double tangent_mass_two{};
        double restitution_speed{};
        double friction{};
        double normal_impulse{};
        double tangent_impulse_one{};
        double tangent_impulse_two{};
    };
    struct Hook {
        std::string name;
        ConstraintSolver solver;
        bool enabled{true};
    };
    struct ContactKey {
        ColliderHandle first{};
        ColliderHandle second{};
        auto operator<=>(const ContactKey&) const = default;
    };

    [[nodiscard]] Record* record(BodyHandle handle) noexcept;
    [[nodiscard]] const Record* record(BodyHandle handle) const noexcept;
    bool erase(BodyHandle handle);
    void synchronize_scene();
    void synchronize_colliders();
    bool publish_poses();
    void publish_velocity(BodyState& body);
    void integrate(double delta_seconds, double remaining_seconds);
    void prepare_contacts(std::span<const ContactManifold> contacts);
    void solve_velocities();
    void solve_positions();
    void update_contacts(std::span<const ContactManifold> contacts);
    void update_sleep(double delta_seconds);
    void invoke_solvers(ConstraintStage stage, double delta_seconds, std::uint32_t iteration);
    void refresh_statistics();
    void append_event(const PhysicsContactEvent& event);
    void limit_velocity(BodyState& body) const noexcept;
    void impulse_pair(const SolverPoint& point, math::Vec3 impulse);

    ge::scene::Scene& scene_;
    CollisionWorld& collision_;
    ecs::Registry& registry_;
    PhysicsConfig config_;
    ge::GenerationPool handles_;
    std::vector<std::optional<Record>> slots_;
    std::vector<SolverPoint> solver_points_;
    std::vector<ge::scene::WorldMatrixEdit> edits_;
    std::vector<PhysicsContactEvent> events_;
    std::map<ContactKey, PhysicsContactEvent> tracked_contacts_;
    std::vector<Hook> hooks_;
    PhysicsStatistics statistics_{};
    std::uint64_t steps_{};
    bool stepping_{};
};

using DynamicsWorld = PhysicsWorld;
void register_dynamics(core::Kernel& kernel);

} // namespace ge::physics
