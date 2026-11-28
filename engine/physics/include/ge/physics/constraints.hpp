#pragma once

#include <ge/physics/dynamics.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::scene { class WorldPartition; }

namespace ge::physics {

struct JointHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const JointHandle&) const = default;
};

enum class JointKind : std::uint8_t {
    Distance,
    Spring,
    BallSocket,
    Hinge,
    Fixed,
    Slider
};

enum class JointMotorMode : std::uint8_t { Disabled, Velocity, Position };

// Anchor positions use the body's local TRS, including scale. Orientation is a
// local unit quaternion independent of scale; its +X axis is the hinge/slide
// axis and +Y is the hinge twist reference. An empty BodyHandle means a fixed
// world frame expressed in current runtime coordinates, never absolute ones.
struct JointFrame {
    math::Vec3 anchor{};
    math::Quat orientation{};
};

struct JointLimit {
    bool enabled{};
    double lower{};
    double upper{};
};

struct JointMotor {
    JointMotorMode mode{JointMotorMode::Disabled};
    double velocity{};
    double target{};
    double gain{10.0};
    double maximum_speed{20.0};
    // Newtons for slider; newton-metres for hinge.
    double maximum_force{100.0};
};

struct JointTuning {
    double velocity_bias{0.15};
    double position_fraction{0.25};
    double linear_slop{0.001};
    double angular_slop{0.001};
    double maximum_bias_speed{10.0};
    double maximum_position_delta{0.1};
    double maximum_angle_delta{0.1};
    // Row-space inverse stiffness, metres/newton or radians/newton-metre.
    // Positive compliance uses a soft velocity constraint without hard pose
    // projection. Spring joints instead use their own stiffness and damping.
    double linear_compliance{};
    double angular_compliance{};
    // Each scalar solver row has this accumulated-impulse cap.
    double maximum_impulse{1.0e6};
    // Zero disables breaking. Bounds include the moment of anchor impulses.
    double break_force{};
    double break_torque{};
};

struct JointDescription {
    JointKind kind{JointKind::BallSocket};
    BodyHandle first{};
    BodyHandle second{};
    JointFrame first_frame{};
    JointFrame second_frame{};
    double rest_length{1.0};
    double spring_stiffness{100.0};
    double spring_damping{10.0};
    // Distance/slider use linear limits. Hinge uses signed twist limits in
    // [-pi, pi]; it intentionally does not count revolutions across the seam.
    JointLimit linear_limit{};
    JointLimit angular_limit{};
    JointMotor motor{};
    JointTuning tuning{};
    bool enabled{true};
    bool warm_start{true};
};

struct JointLimits {
    std::uint32_t capacity{4096};
    std::size_t event_capacity{8192};
    double coordinate_bound{1.0e10};
    double minimum_delta{1.0e-7};
};

enum class JointStatus : std::uint8_t {
    Ready,
    Disabled,
    Sleeping,
    InactiveBody,
    Broken,
    PendingRemoval,
    InvalidGeometry,
    OriginInconsistent
};

enum class JointEventKind : std::uint8_t {
    Created,
    Reconfigured,
    Removed,
    Broken,
    BodyLost,
    Repaired,
    OriginShifted
};

struct JointEvent {
    JointEventKind kind{JointEventKind::Created};
    JointHandle joint{};
    BodyHandle first{};
    BodyHandle second{};
    std::uint64_t substep{};
    double force{};
    double torque{};
};

struct JointState {
    JointHandle handle{};
    JointDescription description{};
    JointStatus status{JointStatus::Ready};
    math::Vec3 first_anchor{};
    math::Vec3 second_anchor{};
    math::Vec3 linear_impulse{};
    math::Vec3 first_angular_impulse{};
    math::Vec3 second_angular_impulse{};
    double coordinate{};
    double linear_error{};
    double angular_error{};
    // Maximum of the metre and radian magnitudes; use the separate errors
    // above whenever a comparison requires a single physical unit.
    double error{};
    double force{};
    double torque{};
    std::size_t solver_rows{};
    std::uint64_t solved_substep{};
};

struct JointStatistics {
    std::size_t joints{};
    std::size_t active{};
    std::size_t sleeping{};
    std::size_t broken{};
    std::size_t pending_removals{};
    std::size_t rows{};
    std::uint64_t substeps{};
    std::uint64_t rejected_operations{};
    std::uint64_t lost_bodies{};
    std::uint64_t breaks{};
    std::uint64_t dropped_events{};
    std::uint64_t invalid_rows{};
    std::uint64_t origin_shifts{};
    double largest_error{};
    double largest_force{};
    double largest_torque{};
    bool connected{};
    bool origin_consistent{true};
};

[[nodiscard]] std::string_view joint_kind_name(JointKind kind) noexcept;
[[nodiscard]] std::string_view joint_status_name(JointStatus status) noexcept;
[[nodiscard]] bool valid_joint_description(const JointDescription& description,
                                           double coordinate_bound = 1.0e10) noexcept;

// Single writer, borrowing PhysicsWorld. All response goes through that world's
// shared velocities and inertia. No collider filters or body ownership change.
// Removal during a step only marks records; maintenance erases outside a step.
// Losing either body stops subsequent solve work and queues joint removal.
// Explicit shutdown unregisters the named callback while PhysicsWorld is alive.
// Destruction only drops shared state; the retained callback is weak and inert.
class JointService final {
public:
    explicit JointService(PhysicsWorld& world, JointLimits configured = {});
    ~JointService();
    JointService(const JointService&) = delete;
    JointService& operator=(const JointService&) = delete;
    JointService(JointService&&) = delete;
    JointService& operator=(JointService&&) = delete;

    [[nodiscard]] const JointLimits& limits() const noexcept;
    [[nodiscard]] JointHandle create(const JointDescription& description);
    // Converts runtime anchors/frames to local coordinates and preserves the
    // current relative orientation. Distance/spring capture the current length.
    [[nodiscard]] JointHandle attach(JointKind kind, BodyHandle first, BodyHandle second,
                                     math::Vec3 first_anchor, math::Vec3 second_anchor,
                                     math::Vec3 axis = {1.0, 0.0, 0.0});
    bool configure(JointHandle handle, const JointDescription& description);
    bool set_enabled(JointHandle handle, bool enabled);
    bool set_motor(JointHandle handle, const JointMotor& motor);
    bool set_limit(JointHandle handle, const JointLimit& limit);
    bool repair(JointHandle handle);
    bool remove(JointHandle handle);
    void clear();
    std::size_t maintenance();
    [[nodiscard]] std::optional<JointState> inspect(JointHandle handle) const;
    [[nodiscard]] std::vector<JointHandle> joints() const;
    [[nodiscard]] JointStatistics statistics() const noexcept;
    [[nodiscard]] std::span<const JointEvent> events() const noexcept;
    void clear_events() noexcept;

    // Call after PhysicsWorld and Scene subtract the same offset. Only world
    // endpoints move; body-local anchors and orientations remain unchanged.
    // If a partition baseline exists, advance its absolute offset as well so
    // subsequently reading that committed partition shift cannot apply it twice.
    bool shift_origin(math::Vec3 offset);
    // First call captures the baseline and must precede external world-joint
    // creation. Later calls consume this reader's origin history; an absolute
    // origin difference recovers an exhausted history without losing anchors.
    bool synchronize_origin(const scene::WorldPartition& partition);
    bool shutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

void register_constraints(core::Kernel& kernel);

} // namespace ge::physics
