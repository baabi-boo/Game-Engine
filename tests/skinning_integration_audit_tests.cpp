#include "test_support.hpp"

#include <ge/animation/skinning.hpp>

#include <optional>

namespace {

using namespace ge;

struct Fixture {
    ecs::Registry registry{8};
    animation::AnimationService animation;
    animation::SkinningService skinning;
    std::optional<geometry::MeshLibrary> meshes{std::in_place};
    ecs::Entity entity = registry.create();
    geometry::MeshHandle source{};
    geometry::MeshHandle output{};

    Fixture() {
        animation::Bone root;
        root.name = "root";
        animation::Skeleton skeleton;
        CHECK(skeleton.load({root}));
        const auto handle = animation.skeletons.create(std::move(skeleton), "rig");
        CHECK(animation.attach(registry, entity, handle));
        source = meshes->create(geometry::make_plane());
        CHECK(meshes->assign(registry, entity, source));
        CHECK(skinning.bind_rigid(registry, animation, *meshes, entity, source, 0));
        output = skinning.inspect(entity)->output;
    }

    void reconstruct_mesh_library() {
        const auto* address = &*meshes;
        meshes.reset();
        meshes.emplace();
        CHECK(&*meshes == address);
        CHECK(meshes->create(geometry::make_plane()) == source);
        CHECK(meshes->create(geometry::make_box()) == output);
    }
};

void a_reconstructed_library_cannot_receive_stale_deformation() {
    Fixture fixture;
    fixture.reconstruct_mesh_library();
    const auto unrelated = *fixture.meshes->get(fixture.output);
    const auto revision = fixture.meshes->inspect(fixture.output)->revision;
    const auto update = fixture.skinning.update(fixture.registry, fixture.animation, *fixture.meshes);
    CHECK(update.errors == 1);
    CHECK(update.skinned == 0);
    CHECK(!fixture.skinning.bind_rigid(fixture.registry, fixture.animation, *fixture.meshes,
                                      fixture.entity, fixture.source, 0));
    CHECK(fixture.meshes->get(fixture.output)->vertices == unrelated.vertices);
    CHECK(fixture.meshes->get(fixture.output)->indices == unrelated.indices);
    CHECK(fixture.meshes->inspect(fixture.output)->revision == revision);
}

void a_reconstructed_library_cannot_destroy_unrelated_meshes() {
    Fixture fixture;
    fixture.reconstruct_mesh_library();
    CHECK(!fixture.skinning.detach(fixture.registry, *fixture.meshes, fixture.entity));
    CHECK(!fixture.skinning.clear(fixture.registry, *fixture.meshes));
    CHECK(fixture.meshes->contains(fixture.output));
    CHECK(fixture.meshes->statistics().meshes == 2);
    CHECK(fixture.registry.require<geometry::MeshComponent>(fixture.entity).mesh == fixture.output);
    CHECK(fixture.registry.has<animation::SkinningComponent>(fixture.entity));
}

void explicit_cleanup_allows_binding_to_another_library_lifetime() {
    Fixture fixture;
    CHECK(fixture.skinning.clear(fixture.registry, *fixture.meshes));
    fixture.reconstruct_mesh_library();
    CHECK(fixture.skinning.bind_rigid(fixture.registry, fixture.animation, *fixture.meshes,
                                     fixture.entity, fixture.source, 0));
    const auto owned = fixture.skinning.inspect(fixture.entity)->output;
    CHECK(owned != fixture.output);
    CHECK(fixture.skinning.detach(fixture.registry, *fixture.meshes, fixture.entity));
    CHECK(!fixture.meshes->contains(owned));
    CHECK(fixture.meshes->contains(fixture.output));
    CHECK(fixture.meshes->statistics().meshes == 2);
}

} // namespace

int main() {
    return test::run("skinning_integration_audit", {
        {"library lifetime publication", a_reconstructed_library_cannot_receive_stale_deformation},
        {"library lifetime cleanup", a_reconstructed_library_cannot_destroy_unrelated_meshes},
        {"explicit owner cleanup", explicit_cleanup_allows_binding_to_another_library_lifetime},
    });
}
