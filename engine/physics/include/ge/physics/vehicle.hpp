#pragma once

#include <ge/physics/dynamics.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::input { class InputSystem; }

namespace ge::physics {

inline constexpr std::size_t maximum_vehicle_wheels = 16;

struct VehicleHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const VehicleHandle&) const = default;
};

enum class VehicleAuthority : std::uint8_t { Manual, Human, AI };
enum class VehicleDriveMode : std::uint8_t { SimpleEngine, WheelTorques };
enum class VehicleError : std::uint8_t {
    None, InvalidEntity, InvalidBody, InvalidDescription, AlreadyAttached,
    Capacity, Busy, InvalidHandle, InvalidInput, InvalidWheel, InvalidVisual,
    WrongAuthority, InputUnavailable, QueryIncomplete, ForceRejected, Closed
};

[[nodiscard]] std::string_view describe(VehicleError error) noexcept;

// Signed throttle selects forward (-Z) or reverse; positive steering turns right.
// Finite values are clamped to throttle/steering [-1,1], brake/handbrake [0,1].
struct DriveInput {
    double throttle{};
    double steering{};
    double brake{};
    double handbrake{};
};

[[nodiscard]] std::optional<DriveInput> bounded_drive_input(DriveInput input) noexcept;

struct WheelDescription {
    math::Vec3 mount{};
    double radius{0.34};
    double inertia{1.4};
    double rest_length{0.45};
    double compression_travel{0.25};
    double droop_travel{0.18};
    double spring_rate{35000.0};
    double compression_damping{3200.0};
    double rebound_damping{4200.0};
    double bump_stop_rate{120000.0};
    double maximum_suspension_force{60000.0};
    double longitudinal_stiffness{16000.0};
    double lateral_stiffness{22000.0};
    double friction{1.2};
    double rolling_resistance{0.015};
    double bearing_drag{0.08};
    double maximum_brake_torque{1800.0};
    double handbrake_torque{2400.0};
    double steering_ratio{};
    bool driven{true};
    bool handbrake{};
    // Optional decorative Scene entity: no rigid body, collider, or chassis
    // ancestor is allowed. Its original local transform is restored on detach.
    ecs::Entity visual{};
    math::Quat visual_rotation{};
    math::Vec3 visual_scale{1.0, 1.0, 1.0};
};

struct VehicleTuning {
    double engine_torque{1800.0};
    double reverse_torque_factor{0.65};
    double maximum_wheel_torque{100000.0};
    double maximum_wheel_speed{800.0};
    double maximum_steering_angle{0.6};
    double steering_rate{2.5};
    double ackermann_blend{0.85};
    double wheelbase{2.7};
    double minimum_ground_alignment{0.25};
    double air_density{1.225};
    double drag_coefficient{0.32};
    double frontal_area{2.2};
    double downforce_coefficient{0.15};
    double maximum_aero_force{30000.0};
    math::Vec3 aerodynamic_center{};
    math::Vec3 wind_velocity{};
    // AI input expires to a full service brake; zero permits persistent input.
    double ai_input_timeout{0.5};
    CollisionFilter suspension_filter{};
    bool react_on_support{true};
};

struct VehicleDescription {
    VehicleTuning tuning{};
    std::vector<WheelDescription> wheels;
};

[[nodiscard]] VehicleDescription four_wheel_vehicle();
[[nodiscard]] bool valid_wheel(const WheelDescription& wheel) noexcept;
[[nodiscard]] bool valid_vehicle(const VehicleDescription& description) noexcept;

struct VehicleComponent {
    VehicleHandle vehicle{};
    BodyHandle chassis{};
};

struct WheelTorque {
    double drive{};
    double brake{};
};

struct WheelState {
    ColliderHandle collider{};
    BodyHandle support_body{};
    ecs::Entity support{};
    math::Vec3 contact_point{};
    math::Vec3 contact_normal{0.0, 1.0, 0.0};
    math::Vec3 forward{0.0, 0.0, -1.0};
    math::Vec3 right{1.0, 0.0, 0.0};
    math::Transform visual_transform{};
    double suspension_length{};
    double compression{};
    double suspension_velocity{};
    double normal_load{};
    double steering_angle{};
    // Positive angular speed rolls forward; visual spin is about local -X.
    double angular_speed{};
    double rotation_angle{};
    double longitudinal_speed{};
    double lateral_speed{};
    double longitudinal_slip{};
    double slip_angle{};
    double longitudinal_force{};
    double lateral_force{};
    double drive_torque{};
    double brake_torque{};
    double friction_utilization{};
    bool grounded{};
    bool query_incomplete{};
};

struct VehicleState {
    VehicleHandle handle{};
    ecs::Entity entity{};
    BodyHandle chassis{};
    VehicleAuthority authority{VehicleAuthority::Manual};
    VehicleDriveMode drive_mode{VehicleDriveMode::SimpleEngine};
    VehicleError last_error{VehicleError::None};
    DriveInput input{};
    math::Transform pose{};
    math::Vec3 linear_velocity{};
    math::Vec3 angular_velocity{};
    math::Vec3 acceleration{};
    math::Vec3 aerodynamic_force{};
    double mass{};
    double forward_speed{};
    double sideways_speed{};
    double steering_angle{};
    double distance_travelled{};
    double airborne_seconds{};
    double input_age{};
    std::size_t grounded_wheels{};
    std::uint64_t tick{};
    bool enabled{true};
    bool input_expired{};
};

struct VehicleLimits {
    std::uint32_t vehicles{1024};
    std::size_t query_candidates{256};
    std::size_t queries_per_frame{32768};
};

struct VehicleStatistics {
    std::size_t vehicles{};
    std::size_t wheels{};
    std::size_t grounded_wheels{};
    std::size_t queries{};
    std::size_t incomplete_queries{};
    std::size_t removed_stale{};
    std::size_t rejected_forces{};
    std::size_t rejected_visuals{};
    double simulated_delta{};
    bool invalid_delta{};
    bool duplicate_tick{};
};

// Single writer, borrowing PhysicsWorld and optional human InputSystem. The
// chassis must already be a dynamic, unit-scale Scene body. Attach never creates
// bodies, colliders, meshes, or entities. Registry copies do not transfer ownership.
// Suspension rays sample existing collision primitives; forces are held for the
// bounded public dynamics step. No sweeps, gearing, or chassis pose writes occur.
// Call shutdown before borrowed services die; destruction does not access them.
class VehicleSystem final {
public:
    explicit VehicleSystem(PhysicsWorld& physics, VehicleLimits limits = {});
    ~VehicleSystem();
    VehicleSystem(const VehicleSystem&) = delete;
    VehicleSystem& operator=(const VehicleSystem&) = delete;
    VehicleSystem(VehicleSystem&&) = delete;
    VehicleSystem& operator=(VehicleSystem&&) = delete;

    [[nodiscard]] VehicleHandle attach(ecs::Entity entity,
        const VehicleDescription& description = four_wheel_vehicle(),
        VehicleAuthority authority = VehicleAuthority::Manual);
    bool detach(VehicleHandle vehicle);
    bool detach(ecs::Entity entity);
    bool clear();
    bool shutdown();
    [[nodiscard]] VehicleHandle find(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::vector<VehicleHandle> vehicles() const;
    [[nodiscard]] std::optional<VehicleState> state(VehicleHandle vehicle) const noexcept;
    [[nodiscard]] std::optional<VehicleDescription> description(VehicleHandle vehicle) const;
    [[nodiscard]] std::span<const WheelState> wheels(VehicleHandle vehicle) const noexcept;
    [[nodiscard]] VehicleError last_error() const noexcept;
    [[nodiscard]] const VehicleLimits& limits() const noexcept;
    [[nodiscard]] const VehicleStatistics& statistics() const noexcept;

    bool set_input(VehicleHandle vehicle, DriveInput input);
    bool set_input(ecs::Entity entity, DriveInput input);
    bool set_authority(VehicleHandle vehicle, VehicleAuthority authority);
    bool set_enabled(VehicleHandle vehicle, bool enabled);
    bool set_tuning(VehicleHandle vehicle, const VehicleTuning& tuning);
    bool set_wheel(VehicleHandle vehicle, std::size_t index, const WheelDescription& wheel);
    bool set_chassis_mass(VehicleHandle vehicle, const MassProperties& mass);
    bool set_drive_mode(VehicleHandle vehicle, VehicleDriveMode mode);
    // External torques require WheelTorques mode. Submissions are consumed once
    // by the next accepted pre_step, including zero-duration frames; absent
    // submissions are zero. Positive drive means forward.
    // External brake torque adds to the normal service and handbrake inputs.
    bool set_wheel_torque(VehicleHandle vehicle, std::size_t wheel, WheelTorque torque);
    bool set_wheel_torques(VehicleHandle vehicle, std::span<const WheelTorque> torques);
    bool clear_wheel_torques(VehicleHandle vehicle);
    bool select_player(VehicleHandle vehicle, input::InputSystem& input);
    bool release_player();
    [[nodiscard]] VehicleHandle player() const noexcept;

    // Exactly one pre_step, PhysicsWorld::step, then post_step per frame tick.
    // Returned wheel spans expire on attachment, detachment, or wheel tuning.
    void pre_step(double delta_seconds, std::uint64_t tick);
    void post_step(std::uint64_t tick);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_vehicle(core::Kernel& kernel);

} // namespace ge::physics
