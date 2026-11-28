#include "test_support.hpp"

#include <ge/effects/particles.hpp>
#include <ge/render/camera.hpp>
#include <ge/scene/scene.hpp>

namespace {

using namespace ge;

struct Fixture {
    ecs::Registry registry{32};
    scene::Scene scene{registry};
    geometry::MeshLibrary meshes;
    render::MaterialService materials;
    effects::ParticleService particles{scene, meshes, materials};

    ~Fixture() { particles.shutdown(); }
};

void retirement_preserves_a_removed_or_replaced_render_binding() {
    for (const bool replace : {false, true}) {
        Fixture fixture;
        const auto source = fixture.scene.create("borrowed source");
        const auto emitter = fixture.particles.create("particles", {}, source);
        CHECK(emitter);
        const auto state = *fixture.particles.inspect(emitter);
        if (replace) {
            fixture.registry.emplace_or_replace<effects::ParticleBindingComponent>(
                state.render_entity, effects::ParticleBindingComponent{emitter, false});
        } else {
            CHECK(fixture.registry.remove<effects::ParticleBindingComponent>(state.render_entity));
        }
        const auto replacement_mesh = fixture.meshes.create(geometry::make_box());
        CHECK(fixture.meshes.assign(fixture.registry, state.render_entity, replacement_mesh));
        const math::Aabb bounds{{-3.0, -2.0, -1.0}, {3.0, 2.0, 1.0}};
        CHECK(fixture.scene.set_local_bounds(state.render_entity, bounds));
        const auto replacement_material = fixture.materials.materials().create("replacement");
        CHECK(fixture.materials.materials().assign(fixture.registry, state.render_entity, replacement_material));

        const bool reassigned = fixture.particles.set_material(emitter, {});
        CHECK(fixture.particles.destroy(emitter));
        CHECK(fixture.scene.contains(source));
        CHECK(fixture.scene.contains(state.render_entity));
        CHECK(fixture.registry.require<geometry::MeshComponent>(state.render_entity).visible);
        CHECK(fixture.registry.require<geometry::MeshComponent>(state.render_entity).mesh == replacement_mesh);
        const auto actual_bounds = fixture.registry.require<ecs::BoundsComponent>(state.render_entity).local;
        CHECK(actual_bounds.min == bounds.min && actual_bounds.max == bounds.max);
        CHECK(!reassigned);
        CHECK(fixture.materials.materials().assigned(fixture.registry, state.render_entity) == replacement_material);
        CHECK(fixture.meshes.contains(replacement_mesh));
        CHECK(!fixture.meshes.contains(state.mesh));
    }
}

void replaced_owned_source_is_not_destroyed() {
    Fixture fixture;
    const auto emitter = fixture.particles.create("owned source");
    CHECK(emitter);
    const auto state = *fixture.particles.inspect(emitter);
    fixture.registry.emplace_or_replace<effects::ParticleBindingComponent>(
        state.source, effects::ParticleBindingComponent{emitter, true});
    fixture.particles.update(0.0);
    CHECK(!fixture.particles.contains(emitter));
    CHECK(fixture.scene.contains(state.source));
    CHECK(!fixture.scene.contains(state.render_entity));
    CHECK(!fixture.meshes.contains(state.mesh));
}

void owned_particles_keep_emission_and_mesh_lifetimes() {
    Fixture fixture;
    effects::EmitterDescription description;
    description.rate = 0.0;
    description.lifetime = {0.05, 0.05};
    description.speed = {0.0, 0.0};
    description.forces.acceleration = {};
    const auto emitter = fixture.particles.create("lifetime", description);
    CHECK(emitter);
    const auto state = *fixture.particles.inspect(emitter);
    CHECK(fixture.particles.burst(emitter, 1) == 1);
    fixture.particles.update(0.0);
    CHECK(fixture.particles.inspect(emitter)->live_particles == 1);
    const render::CameraView camera;
    CHECK(fixture.particles.rebuild_meshes(&camera) == 1);
    CHECK(fixture.registry.require<geometry::MeshComponent>(state.render_entity).visible);
    CHECK(fixture.scene.world_bounds(state.render_entity));
    fixture.particles.update(0.1);
    CHECK(fixture.particles.inspect(emitter)->live_particles == 0);
    CHECK(fixture.particles.statistics().expired == 1);
    CHECK(!fixture.registry.require<geometry::MeshComponent>(state.render_entity).visible);
    CHECK(!fixture.scene.world_bounds(state.render_entity));
    CHECK(fixture.particles.destroy(emitter));
    CHECK(!fixture.scene.contains(state.source));
    CHECK(!fixture.scene.contains(state.render_entity));
    CHECK(!fixture.meshes.contains(state.mesh));
}

} // namespace

int main() {
    return ge::test::run("effects_particles_audit", {
        {"render_binding_ownership", retirement_preserves_a_removed_or_replaced_render_binding},
        {"source_binding_ownership", replaced_owned_source_is_not_destroyed},
        {"owned_emission_and_mesh_lifetime", owned_particles_keep_emission_and_mesh_lifetimes},
    });
}
