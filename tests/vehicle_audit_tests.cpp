#include <ge/input/actions.hpp>
#include <ge/physics/vehicle.hpp>

#include "test_support.hpp"

namespace {

ge::physics::PhysicsConfig configuration() {
    ge::physics::PhysicsConfig result;
    result.capacity = 8;
    result.max_substep_seconds = 0.125;
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
    ge::input::InputSystem input;
    ge::physics::VehicleSystem vehicles{physics, {4, 8, 64}};

    ~Fixture() { (void)vehicles.shutdown(); }

    ge::physics::VehicleHandle attach(ge::physics::VehicleAuthority authority) {
        const auto entity = scene.create("chassis");
        CHECK(entity);
        CHECK(physics.add(entity));
        ge::physics::VehicleDescription description;
        description.wheels.resize(1);
        description.wheels.front().bearing_drag = 0.0;
        description.tuning.ai_input_timeout = 0.01;
        const auto handle = vehicles.attach(entity, description, authority);
        CHECK(handle);
        return handle;
    }

    void step(double delta, std::uint64_t tick) {
        vehicles.pre_step(delta, tick);
        (void)physics.step(delta);
        vehicles.post_step(tick);
    }
};

void skipped_integration_consumes_wheel_torques() {
    for (const double delta : {0.0, 1.0e-9}) {
        Fixture fixture;
        const auto vehicle = fixture.attach(ge::physics::VehicleAuthority::Manual);
        CHECK(fixture.vehicles.set_drive_mode(vehicle, ge::physics::VehicleDriveMode::WheelTorques));
        CHECK(fixture.vehicles.set_wheel_torque(vehicle, 0, {50.0, 25.0}));
        fixture.step(delta, 1);
        fixture.step(0.05, 2);
        CHECK(fixture.vehicles.wheels(vehicle).front().drive_torque == 0.0);
        CHECK(fixture.vehicles.wheels(vehicle).front().brake_torque == 0.0);
        CHECK(fixture.vehicles.wheels(vehicle).front().angular_speed == 0.0);

        // A fresh submission still drives exactly the next preparation.
        CHECK(fixture.vehicles.set_wheel_torque(vehicle, 0, {50.0, 0.0}));
        fixture.step(0.05, 3);
        CHECK(fixture.vehicles.wheels(vehicle).front().drive_torque == 50.0);
        CHECK(fixture.vehicles.wheels(vehicle).front().angular_speed > 0.0);
        fixture.step(delta, 4);
        CHECK(fixture.vehicles.wheels(vehicle).front().drive_torque == 0.0);
        CHECK(fixture.vehicles.wheels(vehicle).front().brake_torque == 0.0);
        fixture.step(0.05, 5);
        CHECK(fixture.vehicles.wheels(vehicle).front().drive_torque == 0.0);
    }
}

void authority_changes_clear_expired_ai_input() {
    for (const bool human : {false, true}) {
        Fixture fixture;
        const auto vehicle = fixture.attach(ge::physics::VehicleAuthority::AI);
        CHECK(fixture.vehicles.set_input(vehicle, {1.0, 0.0, 0.0, 0.0}));
        fixture.step(0.05, 1);
        CHECK(fixture.vehicles.state(vehicle)->input_expired);
        CHECK(fixture.vehicles.state(vehicle)->input.brake == 1.0);

        if (human) {
            CHECK(fixture.vehicles.select_player(vehicle, fixture.input));
        } else {
            CHECK(fixture.vehicles.set_authority(vehicle, ge::physics::VehicleAuthority::Manual));
        }
        const auto state = fixture.vehicles.state(vehicle);
        CHECK(state);
        CHECK(state->authority == (human ? ge::physics::VehicleAuthority::Human :
            ge::physics::VehicleAuthority::Manual));
        CHECK(!state->input_expired);
        CHECK(state->input_age == 0.0);
        CHECK(state->input.throttle == 0.0);
        CHECK(state->input.brake == 0.0);
        if (human) {
            CHECK(fixture.vehicles.release_player());
            CHECK(!fixture.vehicles.player());
            CHECK(fixture.vehicles.state(vehicle)->authority == ge::physics::VehicleAuthority::AI);
            const auto context = fixture.input.context_id("vehicle-driving");
            CHECK(context != 0);
            CHECK(!fixture.input.context(context)->enabled);
        }
    }
}

} // namespace

int main() {
    return ge::test::run("vehicle audit", {
        {"skipped integration consumes wheel torques", skipped_integration_consumes_wheel_torques},
        {"authority changes clear expired AI input", authority_changes_clear_expired_ai_input},
    });
}
