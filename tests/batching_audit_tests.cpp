#include "test_support.hpp"

#include <ge/render/batching.hpp>
#include <ge/scene/scene.hpp>

#include <optional>

namespace {

struct Fixture {
    ge::ecs::Registry registry;
    ge::scene::Scene scene{registry};
    ge::render::MaterialService materials;
    ge::render::BatchingService batching;
    ge::render::RasterizerSettings settings;
    ge::render::RasterizerLimits limits;
    ge::render::VisibilitySubmission output;
    ge::RuntimeHandle entity{scene.create("mesh")};

    void build(const ge::geometry::MeshLibrary& library, ge::geometry::MeshHandle handle) {
        const auto info = library.inspect(handle);
        CHECK(info);
        ge::render::VisibilityView view;
        ge::render::VisibilityEntry entry;
        entry.mesh = handle;
        entry.mesh_revision = info->revision;
        entry.world_bounds = info->bounds;
        entry.draw.entity = entity;
        entry.draw.mesh = library.get(handle);
        entry.draw.material = &materials.fallback();
        entry.draw.index_count = 3;
        view.entries.push_back(entry);

        CHECK(batching.build({scene, library, materials, view, settings, limits, 1}, output).success);
        CHECK(output.draws.size() == 1);
        CHECK(output.owned_meshes.size() == 1);
        CHECK(output.draws.front().mesh == output.owned_meshes.front().get());
        CHECK(batching.instances().front().geometry_prepared);
    }
};

double minimum_x(const ge::render::VisibilitySubmission& submission) {
    const auto bounds = ge::geometry::mesh_bounds(*submission.draws.front().mesh);
    CHECK(bounds);
    return bounds->min.x;
}

void recreated_library_never_reuses_an_earlier_lifetimes_mesh() {
    Fixture fixture;
    std::optional<ge::geometry::MeshLibrary> library{std::in_place};
    const auto* address = &*library;
    const auto original = library->create(ge::geometry::make_plane({2.0, 2.0, 1, 1}));
    const auto original_revision = library->inspect(original)->revision;

    fixture.build(*library, original);
    CHECK(minimum_x(fixture.output) == -1.0);
    CHECK(fixture.batching.statistics().mesh_cache_hits == 0);
    CHECK(fixture.batching.cache_statistics().meshes == 1);
    const auto retained = fixture.output;
    const auto* original_prepared = retained.draws.front().mesh;

    fixture.build(*library, original);
    CHECK(fixture.batching.statistics().mesh_cache_hits == 1);
    CHECK(fixture.batching.statistics().prepared_indices == 0);
    CHECK(fixture.output.draws.front().mesh == original_prepared);

    library.emplace();
    const auto replacement = library->create(ge::geometry::make_plane({20.0, 2.0, 1, 1}));
    CHECK(&*library == address);
    CHECK(replacement == original);
    CHECK(library->inspect(replacement)->revision == original_revision);

    fixture.build(*library, replacement);
    CHECK(minimum_x(fixture.output) == -10.0);
    CHECK(fixture.output.draws.front().mesh != original_prepared);
    CHECK(fixture.batching.statistics().mesh_cache_hits == 0);
    CHECK(fixture.batching.statistics().cache_evictions == 1);
    CHECK(fixture.batching.cache_statistics().meshes == 1);
    CHECK(minimum_x(retained) == -1.0);

    const auto* replacement_prepared = fixture.output.draws.front().mesh;
    fixture.build(*library, replacement);
    CHECK(fixture.batching.statistics().mesh_cache_hits == 1);
    CHECK(fixture.batching.statistics().prepared_indices == 0);
    CHECK(fixture.output.draws.front().mesh == replacement_prepared);
}

void resource_mutations_invalidate_prepared_meshes() {
    Fixture fixture;
    ge::geometry::MeshLibrary library;
    const auto original = library.create(ge::geometry::make_plane({2.0, 2.0, 1, 1}));
    fixture.build(library, original);

    CHECK(library.replace(original, ge::geometry::make_plane({8.0, 2.0, 1, 1})));
    fixture.build(library, original);
    CHECK(minimum_x(fixture.output) == -4.0);
    CHECK(fixture.batching.statistics().mesh_cache_hits == 0);
    CHECK(fixture.batching.statistics().cache_evictions == 1);

    fixture.build(library, original);
    CHECK(fixture.batching.statistics().mesh_cache_hits == 1);
    library.clear();
    const auto replacement = library.create(ge::geometry::make_plane({16.0, 2.0, 1, 1}));
    CHECK(replacement != original);
    fixture.build(library, replacement);
    CHECK(minimum_x(fixture.output) == -8.0);
    CHECK(fixture.batching.statistics().mesh_cache_hits == 0);
    CHECK(fixture.batching.statistics().cache_evictions == 1);
    CHECK(fixture.batching.cache_statistics().meshes == 1);
}

} // namespace

int main() {
    return ge::test::run("batching_audit", {
        {"library_lifetime_cache_identity", recreated_library_never_reuses_an_earlier_lifetimes_mesh},
        {"resource_mutation_invalidation", resource_mutations_invalidate_prepared_meshes},
    });
}
