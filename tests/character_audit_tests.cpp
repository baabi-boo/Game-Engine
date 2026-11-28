#include <ge/physics/character.hpp>

#include "test_support.hpp"

#include <cmath>

namespace {

struct Fixture {
    ge::ecs::Registry registry{8};
    ge::scene::Scene scene{registry};
    ge::physics::CollisionWorld collision{registry, {.capacity = 8}};
    ge::physics::PhysicsWorld physics{scene, collision, {.capacity = 8}};
    ge::physics::CharacterSystem characters{physics};

    ~Fixture() { (void)characters.shutdown(); }
};

void turning_preserves_scene_teleports() {
    for (const double origin : {0.0, 1.0e8, -1.0e8}) {
        Fixture fixture;
        ge::math::Transform pose;
        pose.position.x = origin;
        const auto entity = fixture.scene.create("character", {}, pose);
        CHECK(entity);
        CHECK(fixture.characters.attach(entity));

        pose.position += ge::math::Vec3{0.25, 2.0, 3.0};
        CHECK(fixture.scene.set_world_transform(entity, pose));
        ge::physics::CharacterInput input;
        input.movement = {1.0, 0.0};
        CHECK(fixture.characters.set_input(entity, input));
        fixture.characters.control(1.0 / 60.0, 1);

        const auto actual = fixture.scene.world_transform(entity);
        const auto state = fixture.characters.state(entity);
        CHECK(actual);
        CHECK(state);
        CHECK(ge::math::distance(actual->position, pose.position) < 1.0e-8);
        CHECK(ge::math::distance(state->position, pose.position) < 1.0e-8);
        CHECK(state->facing.x > 0.0);
        CHECK(state->last_error == ge::physics::CharacterError::None);
    }
}

void teleports_discard_contacts_from_the_old_pose() {
    Fixture fixture;
    const auto floor = fixture.scene.create("floor");
    CHECK(floor);
    ge::physics::ColliderDescription floor_shape;
    floor_shape.shape = ge::math::Plane{{0.0, 1.0, 0.0}, 0.0};
    CHECK(fixture.collision.add(floor, floor_shape));
    const auto entity = fixture.scene.create("character");
    CHECK(entity);
    CHECK(fixture.characters.attach(entity));
    (void)fixture.physics.step(1.0 / 60.0);
    CHECK(!fixture.collision.contacts().empty());
    fixture.characters.sample(1);
    CHECK(fixture.characters.state(entity)->grounded);

    ge::math::Transform pose;
    pose.position.y = 10.0;
    CHECK(fixture.scene.set_world_transform(entity, pose));
    fixture.characters.control(1.0 / 60.0, 2);
    const auto state = fixture.characters.state(entity);
    CHECK(state);
    CHECK(!state->grounded);
    CHECK(!state->support.valid);
    CHECK(std::abs(state->position.y - 10.0) < 1.0e-8);
}

} // namespace

int main() {
    return ge::test::run("character audit", {
        {"turning retains Scene teleports", turning_preserves_scene_teleports},
        {"teleports discard old contacts", teleports_discard_contacts_from_the_old_pose},
    });
}
