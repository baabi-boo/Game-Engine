#include "test_support.hpp"

#include <ge/animation/curves.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace {

// Fail the marker payload copy without depending on unrelated allocation order.
std::size_t rejected_allocation_size{};

struct RejectAllocation {
    explicit RejectAllocation(std::size_t size) { rejected_allocation_size = size; }
    ~RejectAllocation() { rejected_allocation_size = 0; }
};

} // namespace

void* operator new(std::size_t size) {
    if (rejected_allocation_size != 0 && size == rejected_allocation_size) {
        rejected_allocation_size = 0;
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) { return memory; }
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace ge::animation;

void near(double actual, double expected, double tolerance = 1.0e-12) {
    CHECK(std::isfinite(actual));
    CHECK(std::abs(actual - expected) <= tolerance);
}

void interpolation_and_failed_edits() {
    ScalarCurve curve;
    CHECK(curve.assign({
        {0.0, 0.0, 0.0, 0.0, CurveInterpolation::Hermite, CurveTangentPolicy::Flat},
        {2.0, 1.0, 0.0, 0.0, CurveInterpolation::Linear, CurveTangentPolicy::Flat}}));
    const auto middle = curve.sample(1.0);
    CHECK(middle);
    near(middle->value, 0.5);
    near(middle->velocity, 0.75);
    near(middle->acceleration, 0.0);
    CHECK(curve.set_extrapolation(CurveExtrapolation::PingPong, CurveExtrapolation::PingPong));
    const auto reverse = curve.sample(3.0);
    CHECK(reverse);
    near(reverse->value, 0.5);
    near(reverse->velocity, -0.75);
    const auto revision = curve.revision();
    CHECK(!curve.assign({{0.0, 2.0}, {0.0, 3.0}}));
    CHECK(curve.revision() == revision);
    near(curve.sample(1.0)->value, 0.5);
    std::vector<CurveKey<double>> output{{0.0, 7.0}};
    CHECK(!curve.resample(0.0, 1.0, 1, output));
    CHECK(output.size() == 1);
    near(output[0].value, 7.0);
}

void small_quaternion_rotations_remain_continuous() {
    constexpr double duration = 1.0e-6;
    constexpr double angle = 1.0e-11;
    QuaternionCurve curve;
    CHECK(curve.assign({{0.0, {}}, {duration, ge::math::from_axis_angle({0.0, 0.0, 1.0}, angle)}}));
    const auto middle = curve.sample(duration * 0.5);
    CHECK(middle);
    near(middle->value.z, std::sin(angle * 0.25), 1.0e-20);
    near(middle->angular_velocity.z, angle / duration, 1.0e-15);
    CHECK(curve.set_extrapolation(CurveExtrapolation::Linear, CurveExtrapolation::Linear));
    const auto extended = curve.sample(duration * 2.0);
    CHECK(extended);
    near(extended->value.z, std::sin(angle), 1.0e-20);
    CHECK(curve.set_extrapolation(CurveExtrapolation::PingPong, CurveExtrapolation::PingPong));
    const auto reflected = curve.sample(duration * 1.5);
    CHECK(reflected);
    near(reflected->value.z, std::sin(angle * 0.25), 1.0e-20);
    near(reflected->angular_velocity.z, -angle / duration, 1.0e-15);
    const ge::math::Quat constant{0.2, 0.3, 0.4, std::sqrt(0.71)};
    CHECK(curve.assign({{0.0, constant}, {duration, constant}}));
    CHECK(curve.sample(duration * 0.5)->angular_velocity == ge::math::Vec3{});
}

void marker_order_seams_turns_and_budget() {
    const std::vector<ClipEvent> markers{{0.0, "zero", {}}, {0.25, "first", {}},
        {0.25, "second", {}}, {1.0, "end", {}}};
    const auto backward = traverse_curve_markers(markers, 1.0, 0.5, -0.5, WrapMode::Loop);
    CHECK(backward.result);
    CHECK(!backward.truncated);
    CHECK(backward.events.size() == 4);
    CHECK(backward.events[0].event_index == 1);
    CHECK(backward.events[1].event_index == 2);
    CHECK(backward.events[2].event_index == 0);
    CHECK(backward.events[3].event_index == 3);
    CHECK(backward.events[2].cycle == 0);
    CHECK(backward.events[3].cycle == -1);
    for (const auto& event : backward.events) { CHECK(event.reverse); }

    const auto turns = traverse_curve_markers(markers, 1.0, 2.5, 0.5, WrapMode::PingPong);
    CHECK(turns.result);
    CHECK(turns.events.size() == 6);
    const std::size_t expected_indices[]{1, 2, 0, 1, 2, 3};
    const bool expected_reverse[]{true, true, true, false, false, false};
    for (std::size_t i = 0; i < turns.events.size(); ++i) {
        CHECK(turns.events[i].event_index == expected_indices[i]);
        CHECK(turns.events[i].reverse == expected_reverse[i]);
    }

    const auto bounded = traverse_curve_markers(markers, 1.0, 0.0, 1.0e12, WrapMode::Loop, 3);
    CHECK(bounded.result);
    CHECK(bounded.events.size() == 3);
    CHECK(bounded.truncated);
    const auto empty_budget = traverse_curve_markers(markers, 1.0, 0.0, 1.0, WrapMode::Loop, 0);
    CHECK(empty_budget.result);
    CHECK(empty_budget.events.empty());
    CHECK(empty_budget.truncated);
    const auto invalid = traverse_curve_markers(markers, 1.0, 0.0,
        std::numeric_limits<double>::infinity(), WrapMode::Loop);
    CHECK(invalid.result.error == CurveError::InvalidTime);
    CHECK(invalid.events.empty());
}

struct PlayerFixture {
    ge::ecs::Registry registry{8};
    ge::scene::Scene scene{registry};
    CurveService curves{4, 4};
    ge::ecs::Entity entity{};
    CurvePlayerHandle player{};

    PlayerFixture() {
        entity = scene.create("curve-target");
        CHECK(entity);
        VectorCurve position;
        CHECK(position.assign({{0.0, {}}, {1.0, {10.0, 0.0, 0.0}}}));
        CurveTrackSet set;
        CHECK(set.assign("timeline", 1.0, {{"position", CurveProperty::Position, position, {}}},
            {{0.5, "arrived", std::string(4093, 'x')}}));
        const auto resource = curves.create(std::move(set));
        CHECK(resource);
        player = curves.play(scene, nullptr, entity, resource, CurveBindingRoute::SceneLocal, WrapMode::Clamp);
        CHECK(player);
        CHECK(curves.update(scene, nullptr, 0.0).applied == 1);
    }
};

void marker_allocation_precedes_transform_publication() {
    PlayerFixture fixture;
    const auto revision = fixture.curves.state(fixture.player)->applied_revision;
    {
        const RejectAllocation reject_payload(4094);
        CHECK_THROWS_AS(fixture.curves.update(fixture.scene, nullptr, 0.5), std::bad_alloc);
    }
    near(fixture.scene.local_transform(fixture.entity)->position.x, 0.0);
    near(fixture.curves.state(fixture.player)->time, 0.0);
    CHECK(fixture.curves.state(fixture.player)->applied_revision == revision);
    const auto retried = fixture.curves.update(fixture.scene, nullptr, 0.5);
    CHECK(retried.applied == 1);
    CHECK(retried.events.size() == 1);
    near(fixture.scene.local_transform(fixture.entity)->position.x, 5.0);
    near(fixture.curves.state(fixture.player)->time, 0.5);
}

void physics_rejection_preserves_time_and_discards_markers() {
    ge::ecs::Registry registry{8};
    ge::scene::Scene scene{registry};
    ge::physics::CollisionWorld collision{registry};
    ge::physics::PhysicsWorld physics{scene, collision};
    CurveService curves{4, 4};
    const auto entity = scene.create("kinematic-target");
    const auto body = physics.add(entity, {.mode = ge::physics::BodyMode::Kinematic});
    CHECK(body);
    ScalarCurve scale;
    CHECK(scale.assign({{0.0, 1.0}, {1.0, 2.0}}));
    CurveTrackSet set;
    CHECK(set.assign("scale", 1.0, {{"scale", CurveProperty::UniformScale, scale, {}}},
        {{0.5, "arrived", {}}}));
    const auto resource = curves.create(std::move(set));
    const auto player = curves.play(scene, &physics, entity, resource,
        CurveBindingRoute::PhysicsKinematic, WrapMode::Clamp);
    CHECK(player);
    CHECK(curves.update(scene, &physics, 0.0).applied == 1);
    const auto revision = curves.state(player)->applied_revision;
    const auto rejected = curves.update(scene, &physics, 0.5);
    CHECK(rejected.rejected == 1);
    CHECK(rejected.applied == 0);
    CHECK(rejected.events.empty());
    CHECK(curves.state(player)->last_error == CurveError::PhysicsRejected);
    CHECK(curves.state(player)->applied_revision == revision);
    near(curves.state(player)->time, 0.0);
    near(physics.get(body)->pose.scale.x, 1.0);
    CHECK(scale.assign({{0.0, 1.0}}));
    CurveTrackSet replacement;
    CHECK(replacement.assign("scale", 1.0, {{"scale", CurveProperty::UniformScale, scale, {}}},
        {{0.5, "arrived", {}}}));
    CHECK(curves.replace(resource, std::move(replacement)));
    CHECK(curves.pause(player, false));
    const auto retried = curves.update(scene, &physics, 0.5);
    CHECK(retried.applied == 1);
    CHECK(retried.events.size() == 1);
}

} // namespace

int main() {
    return ge::test::run("animation_curves_audit", {
        {"interpolation_and_failed_edits", interpolation_and_failed_edits},
        {"small_quaternion_rotations", small_quaternion_rotations_remain_continuous},
        {"marker_order_and_budget", marker_order_seams_turns_and_budget},
        {"marker_allocation_before_publication", marker_allocation_precedes_transform_publication},
        {"physics_rejection", physics_rejection_preserves_time_and_discards_markers}});
}
