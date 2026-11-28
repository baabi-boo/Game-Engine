#include <ge/math/math.hpp>

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

} // namespace

int main() {
    return ge::test::run("math_audit", {
        {"affine_translation", affine_inverse_is_independent_of_translation_size},
        {"affine_validation", affine_inverse_keeps_validation_and_singularity_checks},
        {"projective_inverse", projective_inverse_retains_the_general_path},
    });
}
