#include "test_support.hpp"

#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/scene/scene.hpp>
#include <ge/terrain/heightfield.hpp>

#include <array>
#include <cmath>
#include <limits>

namespace {

using namespace ge;

void fractional_grid_includes_its_authored_boundary() {
    terrain::Heightfield field{{3, 3, {0.1, 7.0, 0.1}, {0.1, 0.1}, 0.0}};
    const std::array<double, 9> heights{0.0, 1.0, 2.0, 2.0, 3.0, 4.0, 4.0, 5.0, 6.0};
    CHECK(field.replace(heights));
    const auto bounds = field.bounds();
    const auto sample = field.sample(bounds.max.x, bounds.max.z);
    CHECK(sample);
    CHECK(sample->position == bounds.max);
    CHECK(sample->cell_x == 1 && sample->cell_z == 1);
    CHECK(math::almost_equal(sample->gradient, {10.0, 20.0}));
    const double outside_x = std::nextafter(bounds.max.x, std::numeric_limits<double>::infinity());
    const double outside_z = std::nextafter(bounds.max.z, std::numeric_limits<double>::infinity());
    CHECK(!field.sample(outside_x, bounds.max.z));
    CHECK(!field.sample(bounds.max.x, outside_z));
    CHECK(field.sample(outside_x, outside_z, terrain::SampleBoundary::Clamp)->position == bounds.max);
}

struct Fixture {
    ecs::Registry registry{16};
    scene::Scene scene{registry};
    geometry::MeshLibrary meshes;
    terrain::TerrainService terrain;
    terrain::TerrainHandle handle;

    Fixture() {
        handle = terrain.create("terrain", terrain::Heightfield{{2, 2, {}, {1.0, 1.0}, 2.0}});
        CHECK(handle);
        CHECK(terrain.rebuild(handle, meshes, scene));
    }
};

void scene_queries_reject_foreign_handle_collisions() {
    Fixture fixture;
    ecs::Registry registry{16};
    scene::Scene foreign{registry};
    const auto entity = foreign.create("foreign");
    CHECK(entity == fixture.terrain.inspect(fixture.handle)->entity);
    CHECK(!fixture.terrain.sample_world(foreign, fixture.handle, 0.5, 0.5));
    CHECK(!fixture.terrain.world_bounds(foreign, fixture.handle));
    CHECK(fixture.terrain.sample_world(fixture.scene, fixture.handle, 0.5, 0.5));
}

void rebuild_rejects_foreign_resource_owners() {
    Fixture fixture;
    ecs::Registry registry{16};
    scene::Scene foreign_scene{registry};
    const auto entity = foreign_scene.create("foreign");
    geometry::MeshLibrary foreign_meshes;
    const auto mesh = foreign_meshes.create(geometry::make_plane(), "foreign");
    const auto original = *fixture.terrain.inspect(fixture.handle);
    CHECK(entity == original.entity && mesh == original.mesh);
    const auto mesh_revision = foreign_meshes.inspect(mesh)->revision;
    CHECK(!fixture.terrain.rebuild(fixture.handle, fixture.meshes, foreign_scene));
    CHECK(!fixture.terrain.rebuild(fixture.handle, foreign_meshes, fixture.scene));
    CHECK(!registry.has<terrain::TerrainComponent>(entity));
    CHECK(foreign_meshes.inspect(mesh)->revision == mesh_revision);
    CHECK(fixture.terrain.rebuild(fixture.handle, fixture.meshes, fixture.scene));
}

void destroy_rejects_foreign_resource_owners() {
    Fixture fixture;
    ecs::Registry registry{16};
    scene::Scene foreign_scene{registry};
    const auto entity = foreign_scene.create("foreign");
    geometry::MeshLibrary foreign_meshes;
    const auto mesh = foreign_meshes.create(geometry::make_plane(), "foreign");
    CHECK(!fixture.terrain.destroy(fixture.handle, &fixture.meshes, &foreign_scene));
    CHECK(!fixture.terrain.destroy(fixture.handle, &foreign_meshes, &fixture.scene));
    CHECK(foreign_scene.contains(entity));
    CHECK(foreign_meshes.contains(mesh));
    CHECK(fixture.terrain.destroy(fixture.handle, &fixture.meshes, &fixture.scene));
    CHECK(fixture.scene.size() == 0);
    CHECK(fixture.meshes.statistics().meshes == 0);
}

void release_rejects_foreign_owners_and_allows_rebinding() {
    Fixture fixture;
    ecs::Registry registry{16};
    scene::Scene foreign_scene{registry};
    const auto entity = foreign_scene.create("foreign");
    geometry::MeshLibrary foreign_meshes;
    const auto mesh = foreign_meshes.create(geometry::make_plane(), "foreign");
    fixture.terrain.release_render_resources(foreign_meshes, foreign_scene);
    CHECK(foreign_scene.contains(entity));
    CHECK(foreign_meshes.contains(mesh));
    const auto original = *fixture.terrain.inspect(fixture.handle);
    CHECK(fixture.scene.contains(original.entity));
    CHECK(fixture.meshes.contains(original.mesh));
    fixture.terrain.release_render_resources(fixture.meshes, fixture.scene);
    CHECK(!fixture.terrain.inspect(fixture.handle)->mesh);
    CHECK(!fixture.terrain.inspect(fixture.handle)->entity);
    CHECK(fixture.terrain.rebuild(fixture.handle, foreign_meshes, foreign_scene));
    CHECK(foreign_scene.contains(entity));
    CHECK(foreign_meshes.contains(mesh));
    fixture.terrain.release_render_resources(foreign_meshes, foreign_scene);
}

} // namespace

int main() {
    return ge::test::run("terrain_heightfield_audit", {
        {"fractional_grid_boundary", fractional_grid_includes_its_authored_boundary},
        {"foreign_scene_queries", scene_queries_reject_foreign_handle_collisions},
        {"foreign_owner_rebuild", rebuild_rejects_foreign_resource_owners},
        {"foreign_owner_destroy", destroy_rejects_foreign_resource_owners},
        {"foreign_owner_release", release_rejects_foreign_owners_and_allows_rebinding},
    });
}
