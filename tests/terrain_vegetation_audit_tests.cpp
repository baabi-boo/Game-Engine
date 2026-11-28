#include "test_support.hpp"

#include <ge/scene/scene.hpp>
#include <ge/terrain/vegetation.hpp>

#include <optional>
#include <vector>

namespace {

struct Fixture {
    ge::ecs::Registry registry{64};
    ge::scene::Scene scene{registry};
    std::optional<ge::geometry::MeshLibrary> meshes{std::in_place};
    ge::terrain::TerrainService terrain;
    ge::terrain::VegetationService vegetation;
    ge::geometry::MeshHandle plant;
    ge::terrain::TerrainHandle field;
    ge::terrain::VegetationSpeciesHandle species;
    ge::terrain::VegetationPatchHandle patch;

    explicit Fixture(bool build_patch = true) {
        plant = meshes->create(ge::geometry::make_box(), "plant");
        ge::terrain::HeightfieldDesc field_description;
        field_description.columns = 5;
        field_description.rows = 5;
        field = terrain.create("field", ge::terrain::Heightfield{field_description});
        CHECK(field);
        CHECK(terrain.rebuild(field, *meshes, scene));
        ge::terrain::VegetationSpeciesDesc species_description;
        species_description.name = "plants";
        species_description.lods[0].mesh = plant;
        species_description.density = 1.0;
        species_description.minimum_spacing = 0.0;
        species = vegetation.add_species(species_description);
        CHECK(species);
        ge::terrain::VegetationPatchDesc patch_description;
        patch_description.name = "patch";
        patch_description.terrain = field;
        patch_description.species = species;
        patch_description.maximum = {4.0, 4.0};
        patch_description.maximum_instances = 8;
        patch = vegetation.create_patch(patch_description);
        CHECK(patch);
        if (!build_patch) { return; }
        CHECK(vegetation.rebuild(patch, terrain, *meshes, nullptr, scene));
        CHECK(vegetation.instances(patch).size() == 8);
        vegetation.update(scene, *meshes, nullptr, 0.0);
        CHECK(vegetation.statistics().visible == 8);
    }

    ~Fixture() { vegetation.clear(scene); }
};

void initial_binding_rejects_foreign_terrain_owners() {
    Fixture fixture{false};
    ge::ecs::Registry foreign_registry{64};
    ge::scene::Scene foreign_scene{foreign_registry};
    const auto unrelated = foreign_scene.create("unrelated terrain");
    CHECK(unrelated == fixture.terrain.inspect(fixture.field)->entity);
    CHECK(fixture.vegetation.rebuild(fixture.patch, fixture.terrain, *fixture.meshes,
        nullptr, foreign_scene).error == ge::terrain::VegetationError::MissingBinding);
    CHECK(!fixture.vegetation.inspect_patch(fixture.patch)->root);
    CHECK(foreign_scene.children(unrelated).empty());

    CHECK(fixture.vegetation.rebuild(fixture.patch, fixture.terrain, *fixture.meshes,
        nullptr, fixture.scene));
    CHECK(fixture.vegetation.instances(fixture.patch).size() == 8);
}

void colliding_entities_in_another_scene_are_untouched() {
    Fixture fixture;
    const auto before = *fixture.vegetation.inspect_patch(fixture.patch);
    ge::ecs::Registry foreign_registry{64};
    ge::scene::Scene foreign_scene{foreign_registry};
    const auto foreign_terrain = foreign_scene.create("foreign terrain");
    const auto foreign_root = foreign_scene.create("foreign root", foreign_terrain);
    CHECK(foreign_terrain == before.terrain_entity);
    CHECK(foreign_root == before.root);
    std::vector<ge::RuntimeHandle> foreign_plants;
    for (const auto& instance : fixture.vegetation.instances(fixture.patch)) {
        const auto entity = foreign_scene.create("foreign plant", foreign_root);
        CHECK(entity == instance.entity);
        CHECK(fixture.meshes->assign(foreign_registry, entity, fixture.plant));
        foreign_registry.require<ge::geometry::MeshComponent>(entity).visible = false;
        foreign_registry.emplace<ge::render::MaterialComponent>(entity);
        foreign_registry.emplace<ge::terrain::VegetationInstanceComponent>(entity);
        foreign_plants.push_back(entity);
    }

    fixture.vegetation.update(foreign_scene, *fixture.meshes, nullptr, 1.0);
    for (const auto entity : foreign_plants) {
        CHECK(!foreign_registry.require<ge::geometry::MeshComponent>(entity).visible);
    }
    CHECK(fixture.vegetation.statistics().visible == 8);
    CHECK(fixture.vegetation.mark_dirty(fixture.patch));
    CHECK(fixture.vegetation.refresh(fixture.terrain, *fixture.meshes, nullptr, foreign_scene) == 0);
    CHECK(fixture.vegetation.rebuild(fixture.patch, fixture.terrain, *fixture.meshes, nullptr,
        foreign_scene).error == ge::terrain::VegetationError::SceneRejected);
    CHECK(!fixture.vegetation.destroy_patch(fixture.patch, foreign_scene));
    fixture.vegetation.clear(foreign_scene);
    const auto after = fixture.vegetation.inspect_patch(fixture.patch);
    CHECK(after);
    CHECK(after->root == before.root);
    CHECK(after->revision == before.revision);
    CHECK(fixture.vegetation.statistics().instances == 8);
    CHECK(foreign_scene.contains(foreign_root));
    for (const auto entity : foreign_plants) {
        CHECK(foreign_scene.contains(entity));
    }
    CHECK(fixture.vegetation.destroy_patch(fixture.patch, fixture.scene));
    CHECK(foreign_scene.contains(foreign_root));
    CHECK(fixture.meshes->contains(fixture.plant));
}

void reused_library_address_does_not_resurrect_borrowed_meshes() {
    Fixture fixture;
    const auto before = *fixture.vegetation.inspect_patch(fixture.patch);
    const auto* original_address = &*fixture.meshes;
    const auto original_identity = fixture.meshes->lifetime_identity();
    fixture.meshes.reset();
    fixture.meshes.emplace();
    CHECK(&*fixture.meshes == original_address);
    CHECK(fixture.meshes->lifetime_identity() != original_identity);
    const auto unrelated_mesh = fixture.meshes->create(ge::geometry::make_box(), "unrelated");
    CHECK(unrelated_mesh == fixture.plant);

    fixture.vegetation.update(fixture.scene, *fixture.meshes, nullptr, 1.0);
    CHECK(fixture.vegetation.statistics().visible == 0);
    for (const auto& instance : fixture.vegetation.instances(fixture.patch)) {
        CHECK(!instance.visible);
        CHECK(!fixture.registry.require<ge::geometry::MeshComponent>(instance.entity).visible);
    }
    CHECK(fixture.vegetation.mark_dirty(fixture.patch));
    CHECK(fixture.vegetation.rebuild(fixture.patch, fixture.terrain, *fixture.meshes, nullptr,
        fixture.scene).error == ge::terrain::VegetationError::MissingResource);
    const auto after = fixture.vegetation.inspect_patch(fixture.patch);
    CHECK(after);
    CHECK(after->root == before.root);
    CHECK(after->revision == before.revision);
    CHECK(fixture.vegetation.statistics().instances == 8);
    CHECK(fixture.vegetation.destroy_patch(fixture.patch, fixture.scene));
    CHECK(fixture.meshes->contains(unrelated_mesh));
}

} // namespace

int main() {
    return ge::test::run("terrain vegetation audit", {
        {"initial terrain binding ownership", initial_binding_rejects_foreign_terrain_owners},
        {"foreign scene ownership", colliding_entities_in_another_scene_are_untouched},
        {"borrowed mesh library lifetime", reused_library_address_does_not_resurrect_borrowed_meshes},
    });
}
