#include "test_support.hpp"

#include <ge/animation/ik.hpp>

#include <array>

namespace {

using namespace ge;

animation::Skeleton make_chain() {
    animation::Bone root;
    root.name = "root";
    animation::Bone middle;
    middle.name = "middle";
    middle.parent = 0;
    middle.bind_local.position.x = 1.0;
    animation::Bone tip;
    tip.name = "tip";
    tip.parent = 1;
    tip.bind_local.position.x = 1.0;
    animation::Skeleton skeleton;
    CHECK(skeleton.load({root, middle, tip}));
    return skeleton;
}

void look_at_reaches_tight_direction_tolerances() {
    const auto skeleton = make_chain();
    animation::IKConstraint constraint;
    constraint.name = "aim";
    constraint.kind = animation::IKConstraintKind::LookAt;
    constraint.chain = {0};
    constraint.local_axis = {1.0, 0.0, 0.0};
    constraint.tolerance = 1.0e-8;
    animation::IKRig rig;
    CHECK(rig.load(skeleton, {constraint}));
    for (const auto direction : {math::Vec3{1.0, 1.0e-6, 0.0}, math::Vec3{-1.0, 0.0, 1.0e-6}}) {
        animation::Pose pose;
        CHECK(pose.reset(skeleton));
        const std::array targets{animation::IKResolvedTarget{direction, {}, true, false}};
        const auto result = animation::solve_ik(skeleton, rig, targets, pose);
        CHECK(result.result);
        CHECK(result.constraints.size() == 1);
        CHECK(result.constraints[0].reached);
        CHECK(math::distance(pose.local[0].rotation * constraint.local_axis,
                             math::normalized(direction)) < 1.0e-10);
    }
}

void chains_reach_small_offsets_without_stretching() {
    const auto skeleton = make_chain();
    const auto target = math::normalized(math::Vec3{1.0, 1.0e-6, 0.0}) * 2.0;
    for (const auto kind : {animation::IKConstraintKind::CCD, animation::IKConstraintKind::TwoBone}) {
        animation::IKConstraint constraint;
        constraint.name = "reach";
        constraint.kind = kind;
        constraint.chain = {0, 1, 2};
        constraint.tolerance = 1.0e-8;
        animation::IKRig rig;
        CHECK(rig.load(skeleton, {constraint}));
        animation::Pose pose;
        CHECK(pose.reset(skeleton));
        const auto before = pose.local;
        const std::array targets{animation::IKResolvedTarget{target}};
        const auto result = animation::solve_ik(skeleton, rig, targets, pose);
        CHECK(result.result);
        CHECK(result.constraints.size() == 1);
        CHECK(result.constraints[0].reached);
        CHECK(math::distance(math::transform_point(pose.global[2], {}), target) < constraint.tolerance);
        for (std::size_t bone = 0; bone < before.size(); ++bone) {
            CHECK(pose.local[bone].position == before[bone].position);
            CHECK(pose.local[bone].scale == before[bone].scale);
        }
    }
}

} // namespace

int main() {
    return test::run("animation_ik_audit", {
        {"look-at direction precision", look_at_reaches_tight_direction_tolerances},
        {"chain precision without stretching", chains_reach_small_offsets_without_stretching},
    });
}
