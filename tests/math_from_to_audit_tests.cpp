#include "test_support.hpp"

#include <ge/math/math.hpp>

#include <cmath>
#include <limits>

namespace {

using namespace ge;

void maps_direction(math::Vec3 from, math::Vec3 to) {
    const auto rotation = math::from_to(from, to);
    CHECK(math::finite(rotation));
    CHECK(std::abs(math::length(rotation) - 1.0) < 1.0e-12);
    CHECK(math::distance(rotation * math::normalized(from), math::normalized(to)) < 1.0e-12);
}

void small_corrections_are_preserved() {
    for (const double offset : {1.0e-4, 1.0e-6, 1.0e-8}) {
        maps_direction({1.0, 0.0, 0.0}, {1.0, offset, 0.0});
        maps_direction({0.0, 0.0, -1.0}, {offset, 0.0, -1.0});
    }
}

void almost_opposite_directions_keep_their_turning_plane() {
    for (const double offset : {1.0e-4, 1.0e-6, 1.0e-8}) {
        maps_direction({1.0, 0.0, 0.0}, {-1.0, 0.0, offset});
        maps_direction({0.0, 1.0, 0.0}, {offset, -1.0, 0.0});
    }
}

void ordinary_degenerate_and_invalid_inputs_remain_supported() {
    maps_direction({2.0, 0.0, 0.0}, {0.0, 3.0, 0.0});
    maps_direction({1.0, 2.0, 3.0}, {-2.0, 1.0, 4.0});
    maps_direction({1.0, 2.0, 3.0}, {1.0, 2.0, 3.0});
    maps_direction({1.0, 2.0, 3.0}, {-1.0, -2.0, -3.0});
    maps_direction({1.0e300, 0.0, 0.0}, {1.0e-300, 1.0e-306, 0.0});
    for (const auto invalid : {math::Vec3{},
                              math::Vec3{std::numeric_limits<double>::infinity(), 0.0, 0.0},
                              math::Vec3{0.0, std::numeric_limits<double>::quiet_NaN(), 0.0}}) {
        CHECK(math::from_to(invalid, {1.0, 0.0, 0.0}) == math::Quat{});
        CHECK(math::from_to({1.0, 0.0, 0.0}, invalid) == math::Quat{});
    }
}

} // namespace

int main() {
    return test::run("math_from_to_audit", {
        {"small direction corrections", small_corrections_are_preserved},
        {"almost opposite directions", almost_opposite_directions_keep_their_turning_plane},
        {"ordinary and invalid directions", ordinary_degenerate_and_invalid_inputs_remain_supported},
    });
}
