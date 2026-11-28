#include "test_support.hpp"

#include <ge/animation/skeleton.hpp>
#include <ge/scene/scene.hpp>

#include <cmath>
#include <cstdlib>
#include <new>
#include <string>

namespace {

std::size_t rejected_allocation_size{};
std::size_t observed_allocation_size{};
bool observe_allocations{};

struct RejectAllocation {
    explicit RejectAllocation(std::size_t size) { rejected_allocation_size = size; }
    ~RejectAllocation() { rejected_allocation_size = 0; }
};

struct ObserveAllocation {
    ObserveAllocation() { observed_allocation_size = 0; observe_allocations = true; }
    ~ObserveAllocation() { observe_allocations = false; }
};

} // namespace

// Keep the payload-copy fault observable with optimized and different standard
// libraries, without relying on a string's capacity or alignment policy.
#if defined(_MSC_VER)
#define GE_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define GE_TEST_NOINLINE __attribute__((noinline))
#else
#define GE_TEST_NOINLINE
#endif

GE_TEST_NOINLINE void* operator new(std::size_t size) {
    if (observe_allocations && size > observed_allocation_size) { observed_allocation_size = size; }
    if (rejected_allocation_size != 0 && size == rejected_allocation_size) {
        rejected_allocation_size = 0;
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) { return memory; }
    throw std::bad_alloc{};
}

GE_TEST_NOINLINE void operator delete(void* memory) noexcept { std::free(memory); }
GE_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

// Algorithms such as stable_sort use nothrow allocation for scratch storage.
// Keep that path paired with the same replacement delete under sanitizers.
GE_TEST_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); }
    catch (...) { return nullptr; }
}

GE_TEST_NOINLINE void operator delete(void* memory, const std::nothrow_t&) noexcept {
    ::operator delete(memory);
}

#undef GE_TEST_NOINLINE

namespace {

using namespace ge;

void near(double actual, double expected) { CHECK(std::abs(actual - expected) < 1.0e-9); }

std::size_t payload_copy_allocation(const std::string& payload) {
    std::size_t request{};
    {
        const ObserveAllocation observe;
        const auto copy = payload;
        CHECK(copy == payload);
        request = observed_allocation_size;
    }
    CHECK(request >= payload.size() + 1);
    return request;
}

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
    std::string payload;

    explicit Fixture(double fade = 0.0, std::string event_payload = "payload")
        : payload(std::move(event_payload)) {
        CHECK(skeleton && actor && root && child);
        CHECK(animation.attach(registry, actor, skeleton));
        component().bone_entities = {root, child};
        animation::BoneChannel channel;
        channel.bone = 0;
        channel.translations = {{0.0, {}}, {1.0, {2.0, 0.0, 0.0}}};
        animation::AnimationClip resource;
        CHECK(resource.load("move", 1.0, *animation.skeletons.get(skeleton), {channel},
                            {{0.25, "quarter", payload}, {1.0, "end", {}}}));
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
        CHECK(update.events[0].payload == payload);
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

void check_event_allocation_failure(bool bound) {
    Fixture fixture{0.0, std::string(997, 'x')};
    if (!bound) { fixture.component().bone_entities.clear(); }
    const auto publication = fixture.component().animator.pose_publication();
    const auto allocation = payload_copy_allocation(fixture.payload);
    {
        const RejectAllocation reject(allocation);
        CHECK_THROWS_AS(fixture.animation.update(fixture.registry, &fixture.graph, 0.5), std::bad_alloc);
    }
    near(fixture.component().animator.state().time, 0.0);
    near(fixture.component().animator.pose().local[0].position.x, 0.0);
    near(fixture.graph.local_transform(fixture.root)->position.x, 0.0);
    CHECK(fixture.component().animator.pose_publication() == publication);
    const auto retry = fixture.animation.update(fixture.registry, &fixture.graph, 0.5);
    CHECK(retry.animated == 1 && retry.errors == 0);
    CHECK(retry.events.size() == 1);
    CHECK(retry.events[0].payload == fixture.payload);
    near(fixture.component().animator.state().time, 0.5);
    near(fixture.component().animator.pose().local[0].position.x, 1.0);
    near(fixture.graph.local_transform(fixture.root)->position.x, bound ? 1.0 : 0.0);
}

void bound_event_allocation_failure_preserves_publication() { check_event_allocation_failure(true); }
void unbound_event_allocation_failure_preserves_publication() { check_event_allocation_failure(false); }

void later_event_allocation_failure_retains_prior_entity_commit() {
    Fixture fixture;
    const auto later = fixture.registry.create({}, "later");
    CHECK(fixture.animation.attach(fixture.registry, later, fixture.skeleton));
    animation::BoneChannel channel;
    channel.bone = 0;
    channel.translations = {{0.0, {}}, {1.0, {2.0, 0.0, 0.0}}};
    const std::string payload(997, 'x');
    animation::AnimationClip resource;
    CHECK(resource.load("later", 1.0, *fixture.animation.skeletons.get(fixture.skeleton), {channel},
                        {{0.25, "later", payload}}));
    const auto clip = fixture.animation.clips.create(std::move(resource), "later");
    CHECK(fixture.animation.play(fixture.registry, later, clip, 0.0, animation::WrapMode::Clamp));
    CHECK(fixture.registry.view<animation::AnimatedComponent>() ==
          std::vector<ecs::Entity>({fixture.actor, later}));
    auto& component = fixture.registry.require<animation::AnimatedComponent>(later);
    const auto publication = component.animator.pose_publication();
    const auto allocation = payload_copy_allocation(payload);
    {
        const RejectAllocation reject(allocation);
        CHECK_THROWS_AS(fixture.animation.update(fixture.registry, &fixture.graph, 0.5), std::bad_alloc);
    }
    near(fixture.component().animator.state().time, 0.5);
    near(fixture.graph.local_transform(fixture.root)->position.x, 1.0);
    near(component.animator.state().time, 0.0);
    near(component.animator.pose().local[0].position.x, 0.0);
    CHECK(component.animator.pose_publication() == publication);
    const auto retry = fixture.animation.update(fixture.registry, &fixture.graph, 0.5);
    CHECK(retry.animated == 2 && retry.errors == 0);
    CHECK(retry.events.size() == 2);
    CHECK(retry.events[0].name == "end");
    CHECK(retry.events[1].entity == later && retry.events[1].payload == payload);
    near(fixture.component().animator.state().time, 1.0);
    near(component.animator.state().time, 0.5);
}

} // namespace

int main() {
    return test::run("animation_skeleton_audit", {
        {"invalid hierarchy publication", invalid_hierarchy_does_not_publish_playback_or_events},
        {"stale bone entity publication", stale_bone_entities_do_not_consume_events_or_alias_replacements},
        {"scene rejection preserves crossfade", rejected_scene_batch_preserves_crossfade_progress},
        {"bound event allocation preserves publication", bound_event_allocation_failure_preserves_publication},
        {"unbound event allocation preserves publication", unbound_event_allocation_failure_preserves_publication},
        {"later allocation retains prior entity commit", later_event_allocation_failure_retains_prior_entity_commit},
    });
}
