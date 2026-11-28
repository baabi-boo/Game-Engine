#include "test_support.hpp"

#include <ge/animation/blend.hpp>

#include <array>
#include <utility>

namespace {

using namespace ge;

animation::Skeleton make_skeleton() {
    animation::Bone bone;
    bone.name = "root";
    animation::Skeleton skeleton;
    CHECK(skeleton.load({bone}));
    return skeleton;
}

void same_rotation(math::Quat actual, math::Quat expected) {
    for (const auto axis : {math::Vec3{1.0, 0.0, 0.0}, math::Vec3{0.0, 1.0, 0.0}}) {
        CHECK(math::length(math::rotate(actual, axis) - math::rotate(expected, axis)) < 1.0e-9);
    }
}

void additive_extrapolation_preserves_the_reference_relative_angle() {
    const auto skeleton = make_skeleton();
    animation::Pose base, additive, reference, output;
    CHECK(base.reset(skeleton));
    CHECK(additive.reset(skeleton));
    CHECK(reference.reset(skeleton));
    base.local[0].rotation = math::from_axis_angle({1.0, 0.0, 0.0}, 0.4);
    reference.local[0].rotation = math::from_axis_angle({0.0, 1.0, 0.0}, 0.3);

    struct Sample { double angle; double weight; double mask; };
    for (const auto sample : {Sample{0.001, 1000.0, 1.0}, Sample{0.001, 2000.0, 0.5},
                              Sample{1.0e-10, 1.0e6, 1.0}, Sample{0.08, 250.0, 1.0}}) {
        additive.local[0].rotation = reference.local[0].rotation *
            math::from_axis_angle({0.0, 0.0, 1.0}, sample.angle);
        const std::array<double, 1> mask{sample.mask};
        const auto expected = base.local[0].rotation *
            math::from_axis_angle({0.0, 0.0, 1.0}, sample.angle * sample.weight * sample.mask);
        CHECK(animation::additive_pose(skeleton, base, additive, reference, sample.weight, mask, output));
        same_rotation(output.local[0].rotation, expected);
        CHECK(output.global.size() == 1);
        CHECK(output.skin.size() == 1);

        // Opposite quaternion signs represent the same shortest local delta.
        additive.local[0].rotation = -additive.local[0].rotation;
        CHECK(animation::additive_pose(skeleton, base, additive, reference, sample.weight, mask, output));
        same_rotation(output.local[0].rotation, expected);
    }
}

void additive_graph_extrapolates_and_skips_inactive_dependencies() {
    const auto skeleton = make_skeleton();
    animation::ClipLibrary clips;
    animation::BoneChannel channel;
    channel.bone = 0;
    channel.rotations = {{0.0, math::from_axis_angle({0.0, 0.0, 1.0}, 0.001)}};
    animation::AnimationClip clip;
    CHECK(clip.load("delta", 1.0, skeleton, {channel}));
    const auto handle = clips.create(std::move(clip), "delta");
    CHECK(handle);

    animation::BlendNode base;
    base.name = "base";
    animation::BlendNode delta;
    delta.name = "delta";
    delta.kind = animation::BlendNodeKind::Clip;
    delta.clip = handle;
    delta.missing_clip = animation::MissingClipPolicy::Fail;
    animation::BlendNode mixed;
    mixed.name = "additive";
    mixed.kind = animation::BlendNodeKind::Additive;
    mixed.inputs = {{0, 1.0, animation::no_blend_parameter, {}, {}}, {1, 1.0, 0, {}, {0.5}}};
    animation::BlendLimits limits;
    limits.max_cached_transforms = 3;
    animation::BlendTree tree;
    CHECK(tree.load(skeleton, {{"weight", animation::BlendParameterType::Number, 1000.0, 0.0, 1.0e6}},
                    {base, delta, mixed}, 2, limits));
    auto parameters = tree.defaults();
    animation::Pose output;
    auto evaluation = tree.evaluate(skeleton, clips, parameters, 0.0, output);
    CHECK(evaluation.result);
    CHECK(evaluation.evaluated_nodes == 3);
    CHECK(evaluation.clip_samples == 1);
    same_rotation(output.local[0].rotation, math::from_axis_angle({0.0, 0.0, 1.0}, 0.5));
    CHECK(parameters.set_number(0, 2000.0));
    evaluation = tree.evaluate(skeleton, clips, parameters, 0.0, output);
    CHECK(evaluation.result);
    same_rotation(output.local[0].rotation, math::from_axis_angle({0.0, 0.0, 1.0}, 1.0));

    const auto published = output.local[0].rotation;
    CHECK(clips.destroy(handle));
    evaluation = tree.evaluate(skeleton, clips, parameters, 0.0, output);
    CHECK(evaluation.result.error == animation::BlendError::MissingClip);
    CHECK(output.local[0].rotation == published);
    CHECK(parameters.set_number(0, 0.0));
    evaluation = tree.evaluate(skeleton, clips, parameters, 0.0, output);
    CHECK(evaluation.result);
    CHECK(evaluation.evaluated_nodes == 2);
    CHECK(evaluation.clip_samples == 0);
    CHECK(evaluation.missing_clips == 0);
    same_rotation(output.local[0].rotation, {});
}

} // namespace

int main() {
    return test::run("animation_blend_audit", {
        {"additive_extrapolation_preserves_the_reference_relative_angle", additive_extrapolation_preserves_the_reference_relative_angle},
        {"additive_graph_extrapolates_and_skips_inactive_dependencies", additive_graph_extrapolates_and_skips_inactive_dependencies},
    });
}
