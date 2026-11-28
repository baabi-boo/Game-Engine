#include "test_support.hpp"

#include <ge/navigation/navmesh.hpp>

#include <cmath>

namespace {

using namespace ge::navigation;

NavMeshInput four_squares() {
    NavMeshInput input;
    input.vertices = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0},
                      {0.0, 0.0, 1.0}, {1.0, 0.0, 1.0}, {2.0, 0.0, 1.0},
                      {0.0, 0.0, 2.0}, {1.0, 0.0, 2.0}, {2.0, 0.0, 2.0}};
    input.polygons = {{{0, 1, 4, 3}}, {{1, 2, 5, 4}},
                      {{3, 4, 7, 6}}, {{4, 5, 8, 7}}};
    return input;
}

void movement_crosses_shared_vertices() {
    NavMesh mesh;
    CHECK(mesh.rebuild(four_squares()));
    for (const auto start : {ge::math::Vec3{0.5, 0.0, 0.5}, ge::math::Vec3{1.5, 0.0, 1.5}}) {
        const ge::math::Vec3 target{2.0 - start.x, 0.0, 2.0 - start.z};
        const auto location = mesh.containing(start);
        CHECK(location);
        const auto result = mesh.move_along_surface(location, target);
        CHECK(result);
        CHECK(!result.blocked);
        CHECK(result.fraction == 1.0);
        CHECK(result.location.position == target);
        CHECK(result.visited.size() == 3);
    }
}

void shared_vertex_movement_respects_filters_and_budgets() {
    auto input = four_squares();
    input.polygons.back().flags = 2;
    NavMesh mesh;
    CHECK(mesh.rebuild(input));
    const auto start = mesh.containing({0.5, 0.0, 0.5});
    CHECK(start);
    NavQueryOptions options;
    options.filter.include_flags = 1;
    const auto blocked = mesh.move_along_surface(start, {1.5, 0.0, 1.5}, options);
    CHECK(blocked);
    CHECK(blocked.blocked);
    CHECK(blocked.fraction == 0.5);
    CHECK(blocked.location.position == ge::math::Vec3{1.0, 0.0, 1.0});
    options.filter.include_flags = 3;
    options.max_polygons = 2;
    CHECK(mesh.move_along_surface(start, {1.5, 0.0, 1.5}, options).status == NavStatus::BudgetExceeded);
}

void winding_preserves_the_authored_height_fan() {
    NavMeshInput input;
    input.vertices = {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0},
                      {1.0, 1.0, 1.0}, {1.0, 0.0, 0.0}};
    input.polygons = {{{0, 1, 2, 3}}};
    NavMesh clockwise;
    CHECK(clockwise.rebuild(input));
    input.polygons.front().vertices = {0, 3, 2, 1};
    NavMesh counterclockwise;
    CHECK(counterclockwise.rebuild(input));
    for (const auto point : {ge::math::Vec3{0.5, 0.5, 0.5}, ge::math::Vec3{0.25, 0.25, 0.75}}) {
        const auto first = clockwise.containing(point);
        const auto second = counterclockwise.containing(point);
        CHECK(first);
        CHECK(second);
        CHECK(std::abs(first.position.y - point.y) < 1.0e-12);
        CHECK(std::abs(second.position.y - point.y) < 1.0e-12);
    }
}

} // namespace

int main() {
    return ge::test::run("navigation_navmesh_audit", {
        {"movement_crosses_shared_vertices", movement_crosses_shared_vertices},
        {"shared_vertex_filters_and_budgets", shared_vertex_movement_respects_filters_and_budgets},
        {"winding_preserves_height_fan", winding_preserves_the_authored_height_fan},
    });
}
