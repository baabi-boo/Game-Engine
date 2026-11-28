#include <ge/physics/constraints.hpp>

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
    ge::physics::JointService joints{physics, {.capacity = 8, .event_capacity = 16}};

    ~Fixture() { (void)joints.shutdown(); }
};

void lost_endpoint_stops_prepared_velocity_rows() {
    for (const bool lose_first : {false, true}) {
        for (const bool destroy_entity : {false, true}) {
            Fixture fixture;
            const auto first_entity = fixture.scene.create("first");
            ge::math::Transform pose;
            pose.position.x = 2.0;
            const auto second_entity = fixture.scene.create("second", {}, pose);
            CHECK(first_entity);
            CHECK(second_entity);
            const auto first = fixture.physics.add(first_entity);
            const auto second = fixture.physics.add(second_entity);
            CHECK(first);
            CHECK(second);

            ge::physics::JointDescription description;
            description.kind = ge::physics::JointKind::Distance;
            description.first = first;
            description.second = second;
            description.rest_length = 1.0;
            const auto joint = fixture.joints.create(description);
            CHECK(joint);
            fixture.joints.clear_events();

            bool invalidated = false;
            CHECK(fixture.physics.add_constraint_solver("invalidate-endpoint",
                [&](ge::physics::PhysicsWorld&, const ge::physics::ConstraintContext& context) {
                    if (context.stage != ge::physics::ConstraintStage::Prepare) {
                        return;
                    }
                    const auto entity = lose_first ? first_entity : second_entity;
                    invalidated = destroy_entity ? fixture.registry.destroy(entity) :
                        fixture.registry.remove<ge::physics::BodyComponent>(entity);
                }));

            const auto result = fixture.physics.step(0.125);
            CHECK(invalidated);
            CHECK(result.solver_failures == 0);
            const auto survivor = fixture.physics.state(lose_first ? second : first);
            CHECK(survivor);
            CHECK(survivor->linear_velocity == ge::math::Vec3{});
            CHECK(survivor->angular_velocity == ge::math::Vec3{});
            const auto pending = fixture.joints.inspect(joint);
            CHECK(pending);
            CHECK(pending->status == ge::physics::JointStatus::PendingRemoval);
            CHECK(pending->solver_rows == 0);
            CHECK(fixture.joints.statistics().lost_bodies == 1);
            CHECK(fixture.joints.maintenance() == 1);
            CHECK(!fixture.joints.inspect(joint));
            CHECK(fixture.joints.events().size() == 1);
            CHECK(fixture.joints.events().front().kind == ge::physics::JointEventKind::BodyLost);
        }
    }
}

} // namespace

int main() {
    return ge::test::run("constraints audit", {
        {"lost endpoints stop prepared velocity rows", lost_endpoint_stops_prepared_velocity_rows},
    });
}
