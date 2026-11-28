#include "test_support.hpp"

#include <ge/animation/retarget.hpp>
#include <ge/animation/skinning.hpp>
#include <ge/scene/scene.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace {

using namespace ge;

void near(double actual, double expected) {
    CHECK(std::isfinite(actual));
    CHECK_MSG(std::abs(actual - expected) < 1.0e-9,
              "actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}

struct Fixture {
    ecs::Registry registry{8};
    scene::Scene graph{registry};
    animation::AnimationService animation;
    animation::RetargetService retarget;
    animation::SkinningService skinning;
    geometry::MeshLibrary meshes;
    ecs::Entity actor = graph.create("actor");
    ecs::Entity bone = graph.create("root", actor);
    animation::SkeletonHandle skeleton;
    animation::RetargetHandle root;
    geometry::MeshHandle source;
    geometry::MeshHandle output;
    std::uint64_t tick{};

    explicit Fixture(double duration = 1.0) {
        animation::Bone definition;
        definition.name = "root";
        animation::Skeleton resource;
        CHECK(resource.load({definition}));
        skeleton = animation.skeletons.create(std::move(resource), "rig");
        CHECK(animation.attach(registry, actor, skeleton));
        component().bone_entities = {bone};
        animation::BoneChannel channel;
        channel.bone = 0;
        channel.translations = {{0.0, {2.0, 0.0, 0.0}}};
        if (duration > 0.0) { channel.translations.push_back({duration, {6.0, 0.0, 0.0}}); }
        animation::AnimationClip clip;
        CHECK(clip.load("move", duration, *animation.skeletons.get(skeleton), {channel}));
        const auto handle = animation.clips.create(std::move(clip), "move");
        CHECK(animation.play(registry, actor, handle, 0.0, animation::WrapMode::Clamp));
        source = meshes.create(geometry::make_box(), "source");
        CHECK(meshes.assign(registry, actor, source));
        CHECK(skinning.bind_rigid(registry, animation, meshes, actor, source, 0));
        output = skinning.inspect(actor)->output;
        animation::RootMotionBindingOptions options;
        options.model_entity = actor;
        options.application = animation::RootMotionApplication::Scene;
        const auto attached = retarget.attach_root(registry, animation, &graph, nullptr, actor, options);
        CHECK(attached);
        root = attached.handle;
        CHECK(step(0.0).captured == 1);
        check_display(0.0);
    }

    animation::AnimatedComponent& component() {
        return registry.require<animation::AnimatedComponent>(actor);
    }

    animation::RetargetUpdate step(double seconds) {
        const auto animated = animation.update(registry, &graph, seconds);
        CHECK(animated.errors == 0);
        const auto captured = retarget.update(registry, animation, &graph, seconds, ++tick);
        CHECK(captured.errors == 0);
        const auto skinned = skinning.update(registry, animation, meshes);
        CHECK(skinned.errors == 0);
        CHECK(skinned.skinned == 1);
        return captured;
    }

    void check_display(double actor_x) {
        near(graph.world_transform(actor)->position.x, actor_x);
        near(graph.local_transform(bone)->position.x, 0.0);
        near(component().animator.pose().local[0].position.x, 0.0);
        const auto* original = meshes.get(source);
        const auto* deformed = meshes.get(output);
        CHECK(original && deformed);
        CHECK(original->vertices.size() == deformed->vertices.size());
        for (std::size_t index = 0; index < original->vertices.size(); ++index) {
            near(deformed->vertices[index].position.x, original->vertices[index].position.x);
        }
    }
};

void same_time_paused_seek_strips_the_fresh_pose_before_skinning() {
    Fixture fixture;
    CHECK(fixture.step(0.25).captured == 1);
    fixture.check_display(1.0);
    auto& animator = fixture.component().animator;
    animator.pause();
    CHECK(animator.seek(*fixture.animation.skeletons.get(fixture.skeleton), fixture.animation.clips,
                        animator.state().time));
    near(animator.pose().local[0].position.x, 3.0);
    const auto captured = fixture.step(0.1);
    fixture.check_display(1.0);
    CHECK(captured.captured == 1);
    CHECK(fixture.step(0.1).skipped == 1);
    fixture.check_display(1.0);
}

void resumed_clamped_endpoint_strips_the_fresh_pose_before_skinning() {
    Fixture fixture;
    for (int step = 0; step < 4; ++step) {
        CHECK(fixture.step(0.25).captured == 1);
    }
    fixture.check_display(4.0);
    auto& animator = fixture.component().animator;
    CHECK(!animator.state().playing);
    animator.resume();
    const auto captured = fixture.step(0.1);
    CHECK(!animator.state().playing);
    fixture.check_display(4.0);
    CHECK(captured.captured == 1);
    CHECK(fixture.step(0.1).skipped == 1);
    fixture.check_display(4.0);
}

void resumed_zero_duration_clip_strips_each_successful_publication() {
    Fixture fixture{0.0};
    auto& animator = fixture.component().animator;
    CHECK(!animator.state().playing);
    animator.resume();
    const auto captured = fixture.step(0.1);
    fixture.check_display(0.0);
    CHECK(captured.captured == 1);
    CHECK(!animator.state().playing);
    CHECK(fixture.step(0.1).skipped == 1);
    fixture.check_display(0.0);
}

void rejected_and_staged_samples_preserve_the_extraction_baseline() {
    Fixture fixture;
    CHECK(fixture.step(0.25).captured == 1);
    fixture.check_display(1.0);
    auto& animator = fixture.component().animator;
    const auto publication = animator.pose_publication();
    const auto& skeleton = *fixture.animation.skeletons.get(fixture.skeleton);
    auto staged = animator;
    CHECK(staged.pose_publication() == publication);
    CHECK(staged.advance(skeleton, fixture.animation.clips, 0.1).result);
    CHECK(staged.pose_publication() != publication);
    CHECK(animator.pose_publication() == publication);
    near(animator.state().time, 0.25);
    const auto invalid_time = std::numeric_limits<double>::quiet_NaN();
    CHECK(!animator.seek(skeleton, fixture.animation.clips, invalid_time));
    CHECK(!animator.advance(skeleton, fixture.animation.clips, invalid_time).result);
    CHECK(animator.pose_publication() == publication);

    // Sampling succeeds in AnimationService's temporary Animator, but the
    // missing required Scene rejects that publication. Root capture must retain
    // its raw baseline instead of treating the previous stripped pose as raw.
    const auto rejected = fixture.animation.update(fixture.registry, nullptr, 0.1);
    CHECK(rejected.errors == 1 && rejected.animated == 0);
    CHECK(animator.pose_publication() == publication);
    const auto retained = fixture.retarget.update(fixture.registry, fixture.animation,
                                                 &fixture.graph, 0.1, ++fixture.tick);
    CHECK(retained.errors == 0 && retained.skipped == 1);
    fixture.check_display(1.0);
    CHECK(fixture.step(0.1).captured == 1);
    fixture.check_display(1.4);
}

} // namespace

int main() {
    return test::run("animation_pipeline_audit", {
        {"paused same-time seek strips fresh pose", same_time_paused_seek_strips_the_fresh_pose_before_skinning},
        {"resumed endpoint strips fresh pose", resumed_clamped_endpoint_strips_the_fresh_pose_before_skinning},
        {"zero-duration resampling strips fresh pose", resumed_zero_duration_clip_strips_each_successful_publication},
        {"rejected sampling preserves root baseline", rejected_and_staged_samples_preserve_the_extraction_baseline},
    });
}
