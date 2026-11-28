#include "test_support.hpp"

#include <ge/animation/skeleton.hpp>
#include <ge/scene/scene.hpp>

#include <cmath>

namespace {

using namespace ge;

void near(double actual, double expected) { CHECK(std::abs(actual - expected) < 1.0e-9); }

animation::Skeleton make_skeleton() {
    animation::Bone root;
    root.name = "root";
    animation::Bone child;
    child.name = "child";
    child.parent = 0;
    child.bind_local.position.y = 1.0;
    animation::Skeleton skeleton;
    CHECK(skeleton.load({root, child}));
    return skeleton;
}

struct Fixture {
    ecs::Registry registry{32};
    scene::Scene graph{registry};
    animation::AnimationService animation;
    animation::SkeletonHandle skeleton = animation.skeletons.create(make_skeleton(), "rig");
    ecs::Entity actor = registry.create({}, "actor");
    ecs::Entity root = graph.create("root");
    ecs::Entity child = graph.create("child", root);
    animation::ClipHandle clip;

    explicit Fixture(double fade = 0.0) {
        CHECK(skeleton && actor && root && child);
        CHECK(animation.attach(registry, actor, skeleton));
        component().bone_entities = {root, child};
        animation::BoneChannel channel;
        channel.bone = 0;
        channel.translations = {{0.0, {}}, {1.0, {2.0, 0.0, 0.0}}};
        animation::AnimationClip resource;
        CHECK(resource.load("move", 1.0, *animation.skeletons.get(skeleton), {channel},
                            {{0.25, "quarter", "payload"}, {1.0, "end", {}}}));
        clip = animation.clips.create(std::move(resource), "move");
        CHECK(animation.play(registry, actor, clip, fade, animation::WrapMode::Clamp));
    }

    animation::AnimatedComponent& component() {
        return registry.require<animation::AnimatedComponent>(actor);
    }

    void rejected(const animation::AnimationUpdate& update) {
        CHECK(update.errors == 1);
        CHECK(update.animated == 0);
        CHECK(update.events.empty());
        CHECK(update.truncated_entities == 0);
        near(component().animator.state().time, 0.0);
        near(component().animator.pose().local[0].position.x, 0.0);
        near(graph.local_transform(root)->position.x, 0.0);
    }

    void committed(const animation::AnimationUpdate& update, double position = 1.0) {
        CHECK(update.errors == 0);
        CHECK(update.animated == 1);
        CHECK(update.events.size() == 1);
        CHECK(update.events[0].name == "quarter");
        CHECK(update.events[0].payload == "payload");
        CHECK(update.events[0].entity == actor);
        CHECK(update.events[0].clip == clip);
        near(component().animator.state().time, 0.5);
        near(component().animator.pose().local[0].position.x, position);
        near(graph.local_transform(root)->position.x, position);
        near(graph.local_transform(child)->position.y, 1.0);
    }
};

void invalid_hierarchy_does_not_publish_playback_or_events() {
    Fixture fixture;
    CHECK(fixture.graph.reparent(fixture.child, {}, scene::ReparentMode::KeepLocal));
    fixture.rejected(fixture.animation.update(fixture.registry, &fixture.graph, 0.5));
    CHECK(fixture.graph.reparent(fixture.child, fixture.root, scene::ReparentMode::KeepLocal));
    fixture.committed(fixture.animation.update(fixture.registry, &fixture.graph, 0.5));
}

void stale_bone_entities_do_not_consume_events_or_alias_replacements() {
    Fixture fixture;
    const auto old_child = fixture.child;
    CHECK(fixture.graph.destroy(old_child));
    fixture.child = fixture.graph.create("replacement", fixture.root);
    CHECK(fixture.child.index == old_child.index);
    CHECK(fixture.child.generation != old_child.generation);
    fixture.rejected(fixture.animation.update(fixture.registry, &fixture.graph, 0.5, 0));
    near(fixture.graph.local_transform(fixture.child)->position.y, 0.0);
    fixture.component().bone_entities[1] = fixture.child;
    fixture.committed(fixture.animation.update(fixture.registry, &fixture.graph, 0.5));
}

void rejected_scene_batch_preserves_crossfade_progress() {
    Fixture fixture{1.0};
    CHECK(fixture.component().animator.fading());
    fixture.registry.each<animation::AnimatedComponent>(
        [&](ecs::Entity, animation::AnimatedComponent&) {
            fixture.rejected(fixture.animation.update(fixture.registry, &fixture.graph, 0.5));
        });
    CHECK(fixture.component().animator.fading());
    fixture.committed(fixture.animation.update(fixture.registry, &fixture.graph, 0.5), 0.5);
    CHECK(fixture.component().animator.fading());
}

} // namespace

int main() {
    return test::run("animation_skeleton_audit", {
        {"invalid hierarchy publication", invalid_hierarchy_does_not_publish_playback_or_events},
        {"stale bone entity publication", stale_bone_entities_do_not_consume_events_or_alias_replacements},
        {"scene rejection preserves crossfade", rejected_scene_batch_preserves_crossfade_progress},
    });
}
