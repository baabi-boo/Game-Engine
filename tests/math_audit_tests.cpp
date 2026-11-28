#include <ge/math/math.hpp>
#include <ge/physics/collision.hpp>

#include "test_support.hpp"

#include <limits>

namespace {

void affine_inverse_is_independent_of_translation_size() {
    using namespace ge::math;
    const Vec3 position{1.0e12, -2.0e11, 3.0e10};
    const auto translated = translation_matrix(position);
    const auto translated_inverse = inverse(translated);
    CHECK(translated_inverse);
    CHECK(almost_equal(*translated_inverse, translation_matrix(-position)));
    CHECK(almost_equal(*translated_inverse * translated, Mat4{}));

    // Scene world matrices can retain shear from a scaled parent and a
    // rotated child. Invert the full linear part rather than decompose TRS.
    auto affine = scaling_matrix({2.0, -4.0, 0.5});
    affine(0, 1) = 1.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        affine(axis, 3) = position[axis];
    }
    const auto affine_inverse = inverse(affine);
    CHECK(affine_inverse);
    CHECK(almost_equal(*affine_inverse * affine, Mat4{}));
    CHECK(almost_equal(affine * *affine_inverse, Mat4{}));
    const Vec3 local{3.0, -7.0, 12.0};
    CHECK(almost_equal(transform_point(*affine_inverse, transform_point(affine, local)), local));
}

void affine_inverse_keeps_validation_and_singularity_checks() {
    using namespace ge::math;
    auto singular = translation_matrix({1.0e12, 0.0, 0.0});
    singular(1, 1) = 0.0;
    CHECK(!inverse(singular));

    auto nonfinite = Mat4{};
    nonfinite(0, 3) = infinity;
    CHECK(!inverse(nonfinite));
    nonfinite(0, 3) = std::numeric_limits<double>::quiet_NaN();
    CHECK(!inverse(nonfinite));
    CHECK(!inverse(Mat4{}, -1.0));
    CHECK(!inverse(Mat4{}, std::numeric_limits<double>::quiet_NaN()));

    auto overflow = scaling_matrix({1.0e-200, 1.0, 1.0});
    overflow(0, 3) = 1.0e200;
    CHECK(!inverse(overflow));
}

void projective_inverse_retains_the_general_path() {
    using namespace ge::math;
    const auto projection = perspective(radians(60.0), 16.0 / 9.0, 0.1, 1000.0);
    const auto projection_inverse = inverse(projection);
    CHECK(projection_inverse);
    CHECK(almost_equal(*projection_inverse * projection, Mat4{}));
    CHECK(almost_equal(projection * *projection_inverse, Mat4{}));

    // A nonzero projective row must never be silently treated as affine.
    auto nearly_affine = Mat4{};
    nearly_affine(3, 0) = 1.0e-12;
    const auto nearly_affine_inverse = inverse(nearly_affine);
    CHECK(nearly_affine_inverse);
    CHECK((*nearly_affine_inverse)(3, 0) == -1.0e-12);
}

void triangle_closest_points_preserve_small_faces() {
    using namespace ge::math;
    for (const double scale : {1.0, 1.0e-6}) {
        const Triangle triangle{{}, {2.0 * scale, 0.0, 0.0}, {0.0, 2.0 * scale, 0.0}};
        const Vec3 face{0.5 * scale, 0.5 * scale, 0.0};
        const double tolerance = scale * 1.0e-12;
        CHECK(distance(closest_point(triangle, face), face) < tolerance);
        CHECK(distance(closest_point(triangle, face + Vec3{0.0, 0.0, scale}), face) < tolerance);
        CHECK(distance(closest_point(Triangle{triangle.a, triangle.c, triangle.b}, face), face) < tolerance);
        CHECK(distance(closest_point(triangle, {2.0 * scale, 2.0 * scale, scale}),
                       Vec3{scale, scale, 0.0}) < tolerance);
        CHECK(distance(closest_point(triangle, {-scale, -scale, scale}), triangle.a) < tolerance);
    }
}

void small_triangle_faces_produce_sphere_contacts() {
    using namespace ge::math;
    for (const double scale : {1.0, 1.0e-6}) {
        const Triangle triangle{{}, {2.0 * scale, 0.0, 0.0}, {0.0, 2.0 * scale, 0.0}};
        const Sphere sphere{{0.5 * scale, 0.5 * scale, 0.0}, 0.1 * scale};
        CHECK(ge::physics::valid_shape(triangle));
        CHECK(ge::physics::valid_shape(sphere));
        const auto contact = ge::physics::narrowphase(sphere, triangle, 0.0);
        CHECK(contact.touching());
        CHECK(distance(contact.points[0].point_b, sphere.center) < scale * 1.0e-12);
        CHECK(almost_equal(contact.normal, Vec3{0.0, 0.0, -1.0}));
    }
}

void degenerate_triangles_retain_edge_closest_points() {
    using namespace ge::math;
    for (const double scale : {1.0, 1.0e-6}) {
        const Triangle line{{}, {2.0 * scale, 0.0, 0.0}, {4.0 * scale, 0.0, 0.0}};
        const Vec3 query{scale, scale, scale};
        const Vec3 expected{scale, 0.0, 0.0};
        CHECK(distance(closest_point(line, query), expected) < scale * 1.0e-12);
        CHECK(distance(closest_point(Triangle{line.a, line.b, line.b}, query), expected) < scale * 1.0e-12);
        CHECK(closest_point(Triangle{line.b, line.b, line.b}, query) == line.b);
        CHECK(!barycentric(line, query));
        CHECK(!intersect(Ray{query, {0.0, 0.0, -1.0}}, line));
    }
}

void triangle_rays_and_coordinates_preserve_scale() {
    using namespace ge::math;
    for (const double scale : {1.0, 1.0e-6}) {
        const Triangle triangle{{}, {2.0 * scale, 0.0, 0.0}, {0.0, 2.0 * scale, 0.0}};
        const Vec3 face{0.5 * scale, 0.5 * scale, 0.0};
        const auto coordinates = barycentric(triangle, face);
        CHECK(coordinates);
        CHECK(almost_equal(*coordinates, Vec3{0.5, 0.25, 0.25}));
        for (const double direction_scale : {1.0, scale}) {
            const Ray ray{face + Vec3{0.0, 0.0, scale}, {0.0, 0.0, -2.0 * direction_scale}};
            const auto hit = intersect(ray, triangle, 0.0, infinity, true);
            CHECK(hit);
            CHECK(distance(hit->point, face) < scale * 1.0e-12);
            CHECK(almost_equal(hit->distance, scale / (2.0 * direction_scale)));
            CHECK(almost_equal(hit->normal, Vec3{0.0, 0.0, 1.0}));
            CHECK(almost_equal(hit->barycentric, *coordinates));
            CHECK(!intersect(ray, Triangle{triangle.a, triangle.c, triangle.b}, 0.0, infinity, true));
        }
        CHECK(!intersect(Ray{face, {scale, 0.0, 0.0}}, triangle));
    }
}

} // namespace

int main() {
    return ge::test::run("math_audit", {
        {"affine_translation", affine_inverse_is_independent_of_translation_size},
        {"affine_validation", affine_inverse_keeps_validation_and_singularity_checks},
        {"projective_inverse", projective_inverse_retains_the_general_path},
        {"small_triangle_faces", triangle_closest_points_preserve_small_faces},
        {"small_triangle_contacts", small_triangle_faces_produce_sphere_contacts},
        {"degenerate_triangles", degenerate_triangles_retain_edge_closest_points},
        {"triangle_query_scale", triangle_rays_and_coordinates_preserve_scale},
    });
}
