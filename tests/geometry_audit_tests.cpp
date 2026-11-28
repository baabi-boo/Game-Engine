#include "test_support.hpp"

#include <ge/geometry/mesh.hpp>

#include <stdexcept>

namespace {

using namespace ge::geometry;

MeshData implicit_plane() {
    auto mesh = make_plane();
    mesh.submeshes.clear();
    return mesh;
}

void implicit_submeshes_obey_the_range_limit() {
    const auto implicit = implicit_plane();
    const auto explicit_range = make_plane();
    MeshLimits limits;
    limits.max_submeshes = 0;

    CHECK(validate_mesh(implicit, limits).error == MeshError::CapacityExceeded);
    CHECK(validate_mesh(explicit_range, limits).error == MeshError::CapacityExceeded);
    CHECK(validate_mesh({}, limits));

    limits.max_submeshes = 1;
    CHECK(validate_mesh(implicit, limits));
    CHECK(validate_mesh(explicit_range, limits));
}

void rejected_implicit_ranges_preserve_library_publication() {
    MeshLibraryLimits limits;
    limits.capacity = 1;
    limits.mesh.max_submeshes = 0;
    MeshLibrary library{limits};

    CHECK_THROWS_AS(library.create(implicit_plane(), "rejected"), std::length_error);
    CHECK(library.statistics().meshes == 0);
    CHECK(library.statistics().resident_bytes == 0);
    CHECK(library.statistics().revision == 0);
    CHECK(!library.find("rejected"));

    const auto handle = library.create({}, "empty");
    CHECK(handle.index == 1);
    CHECK(handle.generation == 1);
    const auto* published = library.get(handle);
    const auto before = library.statistics();
    const auto info = library.inspect(handle);
    CHECK(info);
    CHECK_THROWS_AS(library.replace(handle, implicit_plane()), std::length_error);
    CHECK(library.contains(handle));
    CHECK(library.get(handle) == published);
    CHECK(library.find("empty") == handle);
    CHECK(library.statistics().meshes == before.meshes);
    CHECK(library.statistics().resident_bytes == before.resident_bytes);
    CHECK(library.statistics().revision == before.revision);
    const auto after = library.inspect(handle);
    CHECK(after);
    CHECK(after->revision == info->revision);
    CHECK(after->submeshes == 0);
}

} // namespace

int main() {
    return ge::test::run("geometry_audit", {
        {"implicit_submesh_limit", implicit_submeshes_obey_the_range_limit},
        {"rejected_implicit_range_publication", rejected_implicit_ranges_preserve_library_publication},
    });
}
