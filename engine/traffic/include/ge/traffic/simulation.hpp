#pragma once

#include <ge/physics/vehicle.hpp>
#include <ge/traffic/roads.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::scene { class WorldPartition; }

namespace ge::traffic {

enum class TrafficError : std::uint8_t {
    None, InvalidInput, InvalidVehicle, WrongAuthority, AlreadyAttached,
    Capacity, InvalidRoute, StaleRoute, RouteLost, MissingControl,
    SensorIncomplete, InconsistentOrigin, InputRejected, Closed, Busy
};

[[nodiscard]] std::string_view describe(TrafficError error) noexcept;

enum class TrafficStatus : std::uint8_t {
    Following, Waiting, Arrived, Recovering, EmergencyStop, Paused, Relinquished
};

enum class JunctionRule : std::uint8_t { Priority, Stop, Yield, Signals };
enum class SignalAspect : std::uint8_t { Red, Green, Amber, Yield };

struct TrafficMovement {
    LaneHandle from{};
    LaneHandle to{};
    auto operator<=>(const TrafficMovement&) const = default;
};

struct SignalPhase {
    std::vector<TrafficMovement> movements;
    double green_seconds{15.0};
    double amber_seconds{3.0};
    double all_red_seconds{1.0};
};

struct JunctionControl {
    JunctionHandle junction{};
    JunctionRule rule{JunctionRule::Stop};
    std::vector<SignalPhase> phases;
    double stop_offset{1.0};
    double stopped_seconds{1.0};
    double clearance_seconds{0.5};
    double admission_seconds{5.0};
    bool enabled{true};
};

struct SignalSnapshot {
    JunctionHandle junction{};
    JunctionRule rule{JunctionRule::Stop};
    std::size_t phase{};
    double phase_seconds{};
    double clearance_remaining{};
    bool enabled{};
    bool faulted{};
};

struct TrafficDestination {
    LaneHandle lane{};
    double distance{};
};

struct TrafficAgentDescription {
    // Explicit physical envelope supplied by the caller; never inferred from
    // mass or wheel mounts. Length is centered on the chassis reference point.
    double length{4.6};
    double width{1.9};
    double height{1.6};
    double sensor_ground_clearance{0.25};
    double maximum_speed{22.0};
    double comfortable_acceleration{2.0};
    double comfortable_braking{4.0};
    double emergency_braking{8.0};
    double lateral_acceleration{2.5};
    double time_headway{1.5};
    double minimum_gap{2.0};
    double side_margin{0.25};
    double lookahead_minimum{4.0};
    double lookahead_seconds{0.6};
    double lookahead_maximum{25.0};
    double planning_distance{100.0};
    double planning_spacing{4.0};
    double projection_radius{12.0};
    double projection_backtrack{3.0};
    double projection_forward{50.0};
    double arrival_tolerance{0.5};
    double stopped_speed{0.2};
    double stalled_seconds{10.0};
    double recovery_interval{2.0};
    std::uint64_t access_mask{~std::uint64_t{0}};
    physics::CollisionFilter obstacle_filter{};
    bool automatic_recovery{true};
};

[[nodiscard]] bool valid_traffic_agent(const TrafficAgentDescription& description) noexcept;

struct TrafficAgentComponent {
    physics::VehicleHandle vehicle{};
};

struct TrafficAgentState {
    physics::VehicleHandle vehicle{};
    ecs::Entity entity{};
    TrafficStatus status{TrafficStatus::Paused};
    TrafficError error{TrafficError::None};
    TrafficDestination destination{};
    LaneHandle lane{};
    LaneHandle connector_to{};
    JunctionHandle junction{};
    math::Vec3 absolute_position{};
    physics::DriveInput input{};
    std::uint64_t route_revision{};
    std::uint64_t tick{};
    double route_distance{};
    double remaining_distance{};
    // Chassis speed magnitude; reverse and lateral motion are not stopped.
    double speed{};
    double target_speed{};
    double lateral_error{};
    double waiting_seconds{};
    double stalled_seconds{};
    bool control_owned{};
    bool sensor_complete{};
    bool reservation{};
};

struct LaneOccupancy {
    physics::VehicleHandle vehicle{};
    ecs::Entity entity{};
    LaneHandle lane{};
    LaneHandle connector_to{};
    math::Vec3 absolute_position{};
    double distance{};
    // Nonnegative forward speed used for following; reverse motion is zero.
    double speed{};
    double length{};
    double width{};
};

struct IntersectionReservation {
    JunctionHandle junction{};
    physics::VehicleHandle vehicle{};
    ecs::Entity entity{};
    TrafficMovement movement{};
    math::Vec3 absolute_center{};
    double radius{};
    double age_seconds{};
    bool entered{};
    bool quarantined{};
};

struct TrafficLimits {
    std::size_t agents{1024};
    std::size_t junctions{512};
    std::size_t phases_per_junction{32};
    std::size_t movements_per_phase{128};
    std::size_t route_legs{512};
    std::size_t route_work{65536};
    std::size_t route_queries_per_frame{8};
    std::size_t projection_samples{256};
    std::size_t planning_samples{128};
    std::size_t sensor_queries_per_frame{8192};
    std::size_t sensor_candidates{128};
    double maximum_delta{0.25};
};

struct TrafficStatistics {
    std::size_t agents{};
    std::size_t controlled{};
    std::size_t waiting{};
    std::size_t arrived{};
    std::size_t relinquished{};
    std::size_t emergency_stops{};
    std::size_t route_queries{};
    std::size_t recovered{};
    std::size_t sensor_queries{};
    std::size_t incomplete_sensors{};
    std::size_t reservations{};
    std::size_t quarantined{};
    std::size_t input_rejections{};
    std::uint64_t tick{};
    bool invalid_delta{};
    bool duplicate_tick{};
    bool inconsistent_origin{};
};

// Single writer borrowing the existing registry/scene/physics/vehicle/road
// services. No entities, rigid bodies, vehicle forces or poses are created.
// Attach accepts only existing AI SimpleEngine vehicles. Authority/drive-mode
// takeover permanently relinquishes control; explicit detach/attach is needed
// to resume. This service never changes or restores driving authority.
// Observable replacement of its last input also relinquishes control. Identical
// writes and authority transitions completed entirely between frames are not
// observable through VehicleSystem; callers must use explicit ownership changes.
//
// Road coordinates and cached route points are absolute. update receives the
// current partition and converts fresh PhysicsWorld poses every frame. Invalid
// origin state or incomplete sensing brakes controlled vehicles conservatively.
// update synchronizes CollisionWorld from Scene and invalidates borrowed contact
// spans; a failed sync is treated as incomplete sensing. Occupancy spans expire
// on the next update or shutdown and describe only registered traffic agents.
//
// Reservations serialize all movements through each authored junction. They
// model traffic admission, not collision avoidance guarantees. Primitive AABB
// sensing and supplied vehicle dimensions cannot represent arbitrary moving
// geometry, absent colliders, or braking/contact limitations of the physics.
// Entered reservations remain quarantined until their vehicle is observed clear;
// lease expiry alone never releases a possibly occupied intersection.
// Destruction never touches borrowed services. Call shutdown while they live.
// update and shutdown do nothing while the registry is iterating or physics is
// stepping; retry when both are idle. A skipped update does not consume its tick.
class TrafficSimulation final {
public:
    TrafficSimulation(RoadNetwork& roads, physics::VehicleSystem& vehicles,
                      physics::PhysicsWorld& physics, TrafficLimits limits = {});
    ~TrafficSimulation();
    TrafficSimulation(const TrafficSimulation&) = delete;
    TrafficSimulation& operator=(const TrafficSimulation&) = delete;
    TrafficSimulation(TrafficSimulation&&) = delete;
    TrafficSimulation& operator=(TrafficSimulation&&) = delete;

    [[nodiscard]] TrafficError attach(physics::VehicleHandle vehicle, const RoadRoute& route,
                                      TrafficAgentDescription description = {});
    [[nodiscard]] TrafficError detach(physics::VehicleHandle vehicle);
    [[nodiscard]] TrafficError set_route(physics::VehicleHandle vehicle, const RoadRoute& route);
    [[nodiscard]] TrafficError set_destination(physics::VehicleHandle vehicle,
                                              TrafficDestination destination);
    [[nodiscard]] TrafficError set_paused(physics::VehicleHandle vehicle, bool paused);
    [[nodiscard]] TrafficError retry(physics::VehicleHandle vehicle);
    [[nodiscard]] std::optional<TrafficAgentState> state(physics::VehicleHandle vehicle) const noexcept;
    [[nodiscard]] std::optional<RoadRoute> route(physics::VehicleHandle vehicle) const;
    [[nodiscard]] std::vector<physics::VehicleHandle> agents() const;
    [[nodiscard]] std::span<const LaneOccupancy> occupancy() const noexcept;
    [[nodiscard]] std::vector<IntersectionReservation> reservations() const;
    [[nodiscard]] TrafficError configure_junction(JunctionControl control);
    [[nodiscard]] TrafficError remove_junction_control(JunctionHandle junction);
    [[nodiscard]] TrafficError set_junction_enabled(JunctionHandle junction, bool enabled);
    [[nodiscard]] std::optional<SignalSnapshot> signal(JunctionHandle junction) const noexcept;
    [[nodiscard]] SignalAspect aspect(JunctionHandle junction, TrafficMovement movement) const noexcept;
    [[nodiscard]] const TrafficLimits& limits() const noexcept;
    [[nodiscard]] const TrafficStatistics& statistics() const noexcept;
    void update(double delta_seconds, std::uint64_t tick, const scene::WorldPartition* partition = nullptr);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_simulation(core::Kernel& kernel);

} // namespace ge::traffic
