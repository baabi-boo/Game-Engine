#include "test_support.hpp"

#include <ge/scene/scene.hpp>
#include <ge/terrain/vegetation.hpp>

#include <array>
#include <cmath>
#include <limits>

namespace {

using namespace ge;

void transformed_fractional_boundaries_remain_inclusive() {
    ecs::Registry registry{16};
    scene::Scene scene{registry};
    geometry::MeshLibrary meshes;
    terrain::TerrainService terrain;
    const auto field = terrain.create("fractional", terrain::Heightfield{
        {3, 3, {0.1, 7.0, 0.1}, {0.1, 0.1}, 2.0}});
    CHECK(field);
    CHECK(terrain.rebuild(field, meshes, scene));
    const auto entity = terrain.inspect(field)->entity;
    const auto authored = terrain.get(field)->bounds();
    const std::array<math::Transform, 3> poses{{
        {{-10.0, -3.0, -10.0}, {}, {1.0, 1.0, 1.0}},
        {{10.0, 2.0, -100.0}, {}, {3.0, 2.0, 0.1}},
        {{-100.0, -7.0, 10.0}, {}, {0.1, 0.5, 3.0}},
    }};
    for (const auto& pose : poses) {
        CHECK(scene.set_local_transform(entity, pose));
        const auto bounds = terrain.world_bounds(scene, field);
        CHECK(bounds);
        const auto matrix = math::matrix(pose);
        const auto minimum = math::transform_point(matrix, authored.min);
        const auto maximum = math::transform_point(matrix, authored.max);
        for (const double x : {minimum.x, maximum.x}) {
            for (const double z : {minimum.z, maximum.z}) {
                const auto sample = terrain.sample_world(scene, field, x, z);
                CHECK(sample);
                CHECK(math::almost_equal(sample->position, {x, minimum.y, z}));
                CHECK(math::almost_equal(sample->normal, {0.0, 1.0, 0.0}));
            }
        }
        CHECK(bounds->min == minimum);
        CHECK(bounds->max == maximum);
        const double infinity = std::numeric_limits<double>::infinity();
        CHECK(!terrain.sample_world(scene, field, std::nextafter(minimum.x, -infinity), minimum.z));
        CHECK(!terrain.sample_world(scene, field, std::nextafter(maximum.x, infinity), maximum.z));
        CHECK(!terrain.sample_world(scene, field, minimum.x, std::nextafter(minimum.z, -infinity)));
        CHECK(!terrain.sample_world(scene, field, maximum.x, std::nextafter(maximum.z, infinity)));
    }
    const std::array<math::Transform, 3> unsupported{{
        {{}, math::from_axis_angle({0.0, 1.0, 0.0}, 0.25), {1.0, 1.0, 1.0}},
        {{}, {}, {-1.0, 1.0, 1.0}},
        {{}, {}, {0.0, 1.0, 1.0}},
    }};
    for (const auto& pose : unsupported) {
        CHECK(scene.set_local_transform(entity, pose));
        CHECK(!terrain.world_bounds(scene, field));
        CHECK(!terrain.sample_world(scene, field, 0.2, 0.2));
    }
    terrain.release_render_resources(meshes, scene);
    CHECK(scene.size() == 0);
    CHECK(meshes.statistics().meshes == 0);
}

void populated_vegetation_follows_rebases_and_cleans_up_stale_resources() {
    ecs::Registry registry{64};
    scene::Scene scene{registry};
    geometry::MeshLibrary meshes;
    render::MaterialLibrary materials;
    terrain::TerrainService terrain;
    terrain::VegetationService vegetation;
    const auto plant = meshes.create(geometry::make_box(), "plant");
    const auto material = materials.create("leaves");
    const auto field = terrain.create("ground", terrain::Heightfield{
        {5, 5, {100.0, 5.0, 200.0}, {1.0, 1.0}, 2.0}});
    CHECK(plant && material && field);
    CHECK(terrain.rebuild(field, meshes, scene, {100.0, 0.0, 200.0}));

    terrain::VegetationSpeciesDesc species_description;
    species_description.name = "plants";
    species_description.lods[0].mesh = plant;
    species_description.materials = {material};
    species_description.density = 1.0;
    species_description.minimum_spacing = 0.0;
    const auto species = vegetation.add_species(species_description);
    CHECK(species);
    terrain::VegetationPatchDesc patch_description;
    patch_description.name = "plants";
    patch_description.terrain = field;
    patch_description.species = species;
    patch_description.minimum = {100.0, 200.0};
    patch_description.maximum = {104.0, 204.0};
    patch_description.maximum_instances = 4;
    const auto patch = vegetation.create_patch(patch_description);
    CHECK(patch);
    CHECK(vegetation.rebuild(patch, terrain, meshes, &materials, scene));
    vegetation.update(scene, meshes, &materials, 0.0);
    CHECK(vegetation.statistics().visible == 4);
    const auto before = *vegetation.inspect_patch(patch);
    CHECK(math::valid(before.authored_bounds));
    const auto root_bounds = scene.subtree_bounds(before.root);
    CHECK(root_bounds);
    const math::Vec3 shift{10.0, 3.0, -20.0};
    CHECK(scene.translate(before.terrain_entity, -shift));
    vegetation.update(scene, meshes, &materials, 0.0);
    const auto rebased_bounds = scene.subtree_bounds(before.root);
    CHECK(rebased_bounds);
    CHECK(math::almost_equal(rebased_bounds->min, root_bounds->min - shift));
    CHECK(math::almost_equal(rebased_bounds->max, root_bounds->max - shift));
    for (const auto& instance : vegetation.instances(patch)) {
        const auto world = scene.world_transform(instance.entity);
        CHECK(world);
        CHECK(math::almost_equal(world->position,
            instance.authored_pose.position - math::Vec3{100.0, 0.0, 200.0} - shift));
        const auto surface = terrain.sample_world(scene, field, world->position.x, world->position.z);
        CHECK(surface);
        CHECK(math::almost_equal(surface->position, world->position));
        CHECK(scene.world_bounds(instance.entity));
    }
    CHECK(vegetation.statistics().visible == 4);
    CHECK(vegetation.inspect_patch(patch)->revision == before.revision);

    CHECK(materials.erase(material));
    const auto unrelated_material = materials.create("unrelated");
    CHECK(unrelated_material != material);
    vegetation.update(scene, meshes, &materials, 0.0);
    CHECK(vegetation.statistics().visible == 0);
    CHECK(vegetation.rebuild(patch, terrain, meshes, &materials, scene).error ==
        terrain::VegetationError::MissingResource);
    CHECK(vegetation.inspect_patch(patch)->root == before.root);
    for (const auto& instance : vegetation.instances(patch)) {
        CHECK(!registry.require<geometry::MeshComponent>(instance.entity).visible);
    }
    terrain.release_render_resources(meshes, scene);
    CHECK(!terrain.matches_render_scene(field, scene));
    vegetation.update(scene, meshes, &materials, 0.0);
    CHECK(vegetation.statistics().visible == 0);
    vegetation.clear(scene);
    CHECK(vegetation.statistics().patches == 0);
    CHECK(vegetation.statistics().instances == 0);
    CHECK(scene.size() == 0);
    CHECK(meshes.contains(plant));
    CHECK(meshes.statistics().meshes == 1);
    CHECK(materials.get(unrelated_material));
}

} // namespace

int main() {
    return test::run("terrain_integration_audit", {
        {"transformed fractional endpoints", transformed_fractional_boundaries_remain_inclusive},
        {"populated rebase and cleanup", populated_vegetation_follows_rebases_and_cleans_up_stale_resources},
    });
}
