#include <ge/traffic/simulation.hpp>

#include "test_support.hpp"

namespace {

ge::physics::PhysicsConfig configuration() {
    ge::physics::PhysicsConfig result;
    result.capacity = 8;
    result.max_solver_points = 16;
    result.max_events = 16;
    result.gravity = {};
    return result;
}

struct Fixture {
    ge::ecs::Registry registry{8};
    ge::scene::Scene scene{registry};
    ge::physics::CollisionWorld collision{registry, {.capacity = 8}};
    ge::physics::PhysicsWorld physics{scene, collision, configuration()};
    ge::physics::VehicleSystem vehicles{physics, {4, 8, 64}};
    ge::traffic::RoadNetwork roads;
    ge::traffic::TrafficSimulation traffic{roads, vehicles, physics};
    ge::traffic::LaneHandle lane{};

    Fixture() {
        ge::traffic::RoadDescriptor road;
        road.name = "straight";
        road.lanes.resize(1);
        CHECK(road.centerline.assign({{0.0, {0.0, 0.0, 0.0}}, {1.0, {0.0, 0.0, -100.0}}}));
        const auto handle = roads.create(std::move(road));
        CHECK(handle);
        lane = roads.lanes(handle).front();
    }

    ~Fixture() {
        traffic.shutdown();
        (void)vehicles.shutdown();
    }

    ge::physics::VehicleHandle attach(double begin = 0.0, double end = 100.0,
                                      ge::math::Vec3 position = {}, ge::math::Vec3 velocity = {}) {
        const auto entity = scene.create("chassis");
        CHECK(entity);
        ge::math::Transform pose;
        pose.position = position;
        CHECK(scene.set_world_transform(entity, pose));
        ge::physics::BodyDescription body;
        body.linear_velocity = velocity;
        CHECK(physics.add(entity, body));
        const auto vehicle = vehicles.attach(entity, ge::physics::four_wheel_vehicle(),
                                             ge::physics::VehicleAuthority::AI);
        CHECK(vehicle);
        const auto route = roads.route(lane, begin, lane, end);
        CHECK(route);
        CHECK(traffic.attach(vehicle, route) == ge::traffic::TrafficError::None);
        return vehicle;
    }
};

void busy_update_preserves_ownership_and_tick() {
    Fixture fixture;
    const auto vehicle = fixture.attach();
    fixture.registry.each<const ge::traffic::TrafficAgentComponent>(
        [&](ge::ecs::Entity, const ge::traffic::TrafficAgentComponent&) {
            fixture.traffic.update(0.1, 1);
        });
    CHECK(fixture.traffic.state(vehicle)->control_owned);
    CHECK(fixture.traffic.state(vehicle)->status != ge::traffic::TrafficStatus::Relinquished);

    // The same tick remains eligible after the surrounding iteration ends.
    fixture.traffic.update(0.1, 1);
    CHECK(!fixture.traffic.statistics().duplicate_tick);
    CHECK(fixture.traffic.state(vehicle)->tick == 1);
    CHECK(fixture.traffic.state(vehicle)->status == ge::traffic::TrafficStatus::Following);
    CHECK(fixture.vehicles.state(vehicle)->input.throttle > 0.0);
}

void busy_shutdown_preserves_cleanup_for_retry() {
    Fixture fixture;
    const auto vehicle = fixture.attach();
    const auto entity = fixture.vehicles.state(vehicle)->entity;
    fixture.registry.each<const ge::traffic::TrafficAgentComponent>(
        [&](ge::ecs::Entity, const ge::traffic::TrafficAgentComponent&) {
            fixture.traffic.shutdown();
        });
    CHECK(fixture.traffic.state(vehicle));
    CHECK(fixture.traffic.state(vehicle)->control_owned);

    fixture.traffic.shutdown();
    CHECK(!fixture.traffic.state(vehicle));
    CHECK(!fixture.registry.has<ge::traffic::TrafficAgentComponent>(entity));
    CHECK(fixture.vehicles.state(vehicle)->input.brake == 1.0);
}

void moving_chassis_does_not_arrive() {
    for (const auto velocity : {ge::math::Vec3{0.0, 0.0, 5.0}, ge::math::Vec3{5.0, 0.0, 0.0}}) {
        Fixture fixture;
        const auto vehicle = fixture.attach(90.0, 100.0, {0.0, 0.0, -100.0}, velocity);
        fixture.traffic.update(0.1, 1);
        const auto moving = fixture.traffic.state(vehicle);
        CHECK(moving);
        CHECK(moving->status != ge::traffic::TrafficStatus::Arrived);
        CHECK(moving->speed == 5.0);
        CHECK(fixture.traffic.occupancy().size() == 1);
        CHECK(fixture.traffic.occupancy().front().speed == 0.0);

        CHECK(fixture.physics.set_velocity(fixture.vehicles.state(vehicle)->chassis, {}));
        fixture.traffic.update(0.1, 2);
        CHECK(fixture.traffic.state(vehicle)->status == ge::traffic::TrafficStatus::Arrived);
    }
}

} // namespace

int main() {
    return ge::test::run("traffic simulation audit", {
        {"busy update preserves ownership and tick", busy_update_preserves_ownership_and_tick},
        {"busy shutdown preserves cleanup for retry", busy_shutdown_preserves_cleanup_for_retry},
        {"moving chassis does not arrive", moving_chassis_does_not_arrive},
    });
}
