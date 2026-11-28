#include "test_support.hpp"

#include <ge/animation/retarget.hpp>

#include <cmath>

namespace {

using namespace ge;

void near(double actual, double expected) { CHECK(std::abs(actual - expected) < 1.0e-9); }

struct Fixture {
    ecs::Registry registry{8};
    animation::AnimationService animation;
    animation::RetargetService retarget;
    ecs::Entity entity = registry.create({}, "actor");
    animation::RetargetHandle root;

    Fixture() {
        animation::Bone bone;
        bone.name = "root";
        animation::Skeleton skeleton;
        CHECK(skeleton.load({bone}));
        animation::BoneChannel channel;
        channel.bone = 0;
        channel.translations = {{0.0, {2.0, 0.0, 0.0}}, {1.0, {6.0, 0.0, 0.0}}};
        animation::AnimationClip clip;
        CHECK(clip.load("move", 1.0, skeleton, {channel}));
        const auto skeleton_handle = animation.skeletons.create(std::move(skeleton), "rig");
        const auto clip_handle = animation.clips.create(std::move(clip), "move");
        CHECK(animation.attach(registry, entity, skeleton_handle));
        CHECK(animation.play(registry, entity, clip_handle, 0.0, animation::WrapMode::Clamp));
        animation::RootMotionBindingOptions options;
        options.write_scene = false;
        const auto attached = retarget.attach_root(registry, animation, nullptr, nullptr, entity, options);
        CHECK(attached);
        root = attached.handle;
        CHECK(retarget.update(registry, animation, nullptr, 0.0, 1).captured == 1);
        near(animator().pose().local[0].position.x, 0.0);
    }

    animation::Animator& animator() {
        return registry.require<animation::AnimatedComponent>(entity).animator;
    }

    void refresh_and_capture(double seconds, std::uint64_t tick) {
        CHECK(animation.update(registry, nullptr, seconds).errors == 0);
        near(animator().pose().local[0].position.x, 2.0);
        const auto update = retarget.update(registry, animation, nullptr, seconds, tick);
        CHECK(update.errors == 0);
        CHECK(update.captured == 1);
        near(animator().pose().local[0].position.x, 0.0);
        const auto info = retarget.inspect_root(root);
        CHECK(info);
        CHECK(!info->last_sample.primed);
        near(info->last_sample.delta.translation.x, 0.0);
    }
};

void frozen_playback_still_strips_refreshed_roots() {
    Fixture fixture;
    CHECK(fixture.animator().set_speed(0.0));
    fixture.refresh_and_capture(0.1, 2);
    fixture.refresh_and_capture(0.1, 3);
}

void zero_delta_still_strips_refreshed_roots() {
    Fixture fixture;
    fixture.refresh_and_capture(0.0, 2);
    fixture.refresh_and_capture(0.0, 3);
}

void paused_playback_preserves_the_extraction_baseline() {
    Fixture fixture;
    fixture.animator().pause();
    CHECK(fixture.animation.update(fixture.registry, nullptr, 0.1).errors == 0);
    const auto paused = fixture.retarget.update(fixture.registry, fixture.animation, nullptr, 0.1, 2);
    CHECK(paused.errors == 0);
    CHECK(paused.skipped == 1);
    near(fixture.animator().pose().local[0].position.x, 0.0);
    fixture.animator().resume();
    CHECK(fixture.animation.update(fixture.registry, nullptr, 0.1).errors == 0);
    CHECK(fixture.retarget.update(fixture.registry, fixture.animation, nullptr, 0.1, 3).captured == 1);
    const auto info = fixture.retarget.inspect_root(fixture.root);
    CHECK(info);
    CHECK(!info->last_sample.primed);
    near(info->last_sample.delta.translation.x, 0.4);
    near(fixture.animator().pose().local[0].position.x, 0.0);
}

} // namespace

int main() {
    return test::run("animation_retarget_audit", {
        {"frozen playback strips refreshed roots", frozen_playback_still_strips_refreshed_roots},
        {"zero delta strips refreshed roots", zero_delta_still_strips_refreshed_roots},
        {"paused playback preserves baseline", paused_playback_preserves_the_extraction_baseline},
    });
}
