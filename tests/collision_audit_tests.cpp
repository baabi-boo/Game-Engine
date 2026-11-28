#include <ge/physics/collision.hpp>

#include "test_support.hpp"

#include <cmath>

namespace {

void short_capsules_keep_their_centerlines() {
    for (const double scale : {1.0, 1.0e-7}) {
        const ge::math::Capsule first{{}, {5.0 * scale, 0.0, 0.0}, 0.1 * scale};
        const ge::math::Capsule second{first.b, {5.0 * scale, 5.0 * scale, 0.0}, 0.1 * scale};
        CHECK(ge::physics::valid_shape(first));
        CHECK(ge::physics::valid_shape(second));
        const auto closest = ge::physics::closest_segments({first.a, first.b}, {second.a, second.b});
        CHECK(ge::math::distance(closest.first, first.b) < scale * 1.0e-8);
        CHECK(ge::math::distance(closest.first, closest.second) < scale * 1.0e-8);
        const auto contact = ge::physics::narrowphase(first, second, 0.0);
        CHECK(contact.touching());
        CHECK(std::abs(contact.points[0].penetration - 0.2 * scale) < scale * 1.0e-8);
        CHECK(ge::math::finite(contact.normal));
    }
}

void short_capsules_find_closest_box_edge_points() {
    for (const double scale : {1.0, 1.0e-7}) {
        const ge::math::Obb box{{}, {scale, scale, scale}, {}};
        const ge::math::Capsule capsule{{0.0, 3.0 * scale, 0.0},
            {3.0 * scale, 0.0, 0.0}, 0.8 * scale};
        const auto contact = ge::physics::narrowphase(capsule, box, 0.0);
        CHECK(contact.touching());
        const double expected = (0.8 - std::sqrt(0.5)) * scale;
        CHECK(std::abs(contact.points[0].penetration - expected) < scale * 1.0e-8);
        CHECK(ge::math::finite(contact.normal));
    }
}

void collapsed_centerlines_remain_valid_spheres() {
    const ge::math::Capsule point{{}, {}, 0.25};
    const ge::math::Capsule segment{{-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, 0.25};
    const auto closest = ge::physics::closest_segments({point.a, point.b}, {segment.a, segment.b});
    CHECK(ge::math::almost_equal(closest.first, ge::math::Vec3{}));
    CHECK(ge::math::almost_equal(closest.second, ge::math::Vec3{}));
    CHECK(ge::physics::narrowphase(point, segment, 0.0).touching());
    CHECK(ge::physics::narrowphase(point, point, 0.0).touching());
}

} // namespace

int main() {
    return ge::test::run("collision_audit", {
        {"short_capsule_centerlines", short_capsules_keep_their_centerlines},
        {"short_capsule_box_edge", short_capsules_find_closest_box_edge_points},
        {"collapsed_centerlines", collapsed_centerlines_remain_valid_spheres},
    });
}
