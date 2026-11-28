#include <ge/physics/dynamics.hpp>

#include "test_support.hpp"

#include <cmath>

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
};

void scene_edits_preserve_small_displacements_far_from_origin() {
    for (const double origin : {0.0, 1.0e8, -1.0e8}) {
        Fixture fixture;
        ge::math::Transform pose;
        pose.position.x = origin;
        const auto entity = fixture.scene.create("body", {}, pose);
        CHECK(entity);
        ge::physics::BodyDescription description;
        description.mode = ge::physics::BodyMode::Static;
        const auto body = fixture.physics.add(entity, description);
        CHECK(body);

        pose.position.x += 0.25;
        CHECK(fixture.scene.set_world_transform(entity, pose));
        const auto result = fixture.physics.step(0.25);
        CHECK(result.substeps == 2);
        CHECK(result.scene_rejections == 0);
        const auto state = fixture.physics.state(body);
        CHECK(state);
        // Relative coordinate comparisons would conceal this regression.
        CHECK(std::abs(state->pose.position.x - pose.position.x) <= 1.0e-7);
        const auto published = fixture.scene.world_transform(entity);
        CHECK(published);
        CHECK(std::abs(published->position.x - pose.position.x) <= 1.0e-7);
    }
}

void kinematic_targets_preserve_small_displacements_far_from_origin() {
    for (const double origin : {0.0, 1.0e8, -1.0e8}) {
        Fixture fixture;
        ge::math::Transform pose;
        pose.position.x = origin;
        const auto entity = fixture.scene.create("body", {}, pose);
        CHECK(entity);
        ge::physics::BodyDescription description;
        description.mode = ge::physics::BodyMode::Kinematic;
        const auto body = fixture.physics.add(entity, description);
        CHECK(body);

        pose.position.x += 0.25;
        CHECK(fixture.physics.set_kinematic_target(body, pose));
        CHECK(fixture.physics.step(0.25).substeps == 2);
        const auto state = fixture.physics.state(body);
        CHECK(state);
        CHECK(std::abs(state->pose.position.x - pose.position.x) <= 1.0e-7);
        CHECK(std::abs(state->linear_velocity.x - 1.0) <= 1.0e-7);

        CHECK(fixture.physics.step(0.125).substeps == 1);
        const auto stopped = fixture.physics.state(body);
        CHECK(stopped);
        CHECK(std::abs(stopped->pose.position.x - pose.position.x) <= 1.0e-7);
        CHECK(stopped->linear_velocity == ge::math::Vec3{});
    }
}

} // namespace

int main() {
    return ge::test::run("dynamics audit", {
        {"scene edits retain small world displacements", scene_edits_preserve_small_displacements_far_from_origin},
        {"kinematic targets retain small world displacements", kinematic_targets_preserve_small_displacements_far_from_origin},
    });
}
