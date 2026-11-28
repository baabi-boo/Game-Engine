#include "test_support.hpp"

#include <ge/animation/blend.hpp>
#include <ge/animation/ik.hpp>
#include <ge/animation/retarget.hpp>
#include <ge/animation/skinning.hpp>

#include <cmath>
#include <memory>

namespace {

using namespace ge;

struct Fixture {
    ecs::Registry registry{8};
    animation::AnimationService animation;
    animation::SkeletonHandle skeleton;

    Fixture() {
        animation::Bone bone;
        bone.name = "root";
        animation::Skeleton resource;
        CHECK(resource.load({bone}));
        skeleton = animation.skeletons.create(std::move(resource), "rig");
        CHECK(skeleton);
    }

    ecs::Entity create() {
        const auto entity = registry.create();
        CHECK(animation.attach(registry, entity, skeleton));
        return entity;
    }

    void edit_pose(ecs::Entity entity, double x) {
        const auto instance = registry.component_instance<animation::AnimatedComponent>(entity);
        auto& pose = registry.require<animation::AnimatedComponent>(entity).animator.pose();
        pose.local[0].position.x = x;
        CHECK(pose.update(*animation.skeletons.get(skeleton)));
        CHECK(registry.mark_changed<animation::AnimatedComponent>(entity));
        CHECK(registry.component_instance<animation::AnimatedComponent>(entity) == instance);
    }

    void replace_twice(ecs::Entity entity) {
        const auto original = registry.component_instance<animation::AnimatedComponent>(entity);
        CHECK(original != 0);
        CHECK(animation.attach(registry, entity, skeleton));
        const auto intermediate = registry.component_instance<animation::AnimatedComponent>(entity);
        CHECK(intermediate != 0 && intermediate != original);
        CHECK(animation.attach(registry, entity, skeleton));
        const auto replacement = registry.component_instance<animation::AnimatedComponent>(entity);
        CHECK(replacement != 0 && replacement != original && replacement != intermediate);
        // Two replacements reproduce the old address-reuse path. Identity checks
        // make the expectations independent of the allocator's chosen addresses.
        edit_pose(entity, 9.0);
    }

    double x(ecs::Entity entity) const {
        return registry.require<animation::AnimatedComponent>(entity).animator.pose().local[0].position.x;
    }
};

std::shared_ptr<const animation::BlendStateMachine> make_machine(const animation::Skeleton& skeleton) {
    animation::BlendNode node;
    node.name = "bind";
    auto tree = std::make_shared<animation::BlendTree>();
    CHECK(tree->load(skeleton, {}, {node}, 0));
    auto machine = std::make_shared<animation::BlendStateMachine>();
    CHECK(machine->load(tree, {{"idle", 0}}, {}, 0));
    return machine;
}

void skinning_retires_replaced_components_and_releases_outputs() {
    Fixture fixture;
    const auto entity = fixture.create();
    geometry::MeshLibrary meshes;
    animation::SkinningService skinning;
    const auto source = meshes.create(geometry::make_plane(), "source");
    CHECK(meshes.assign(fixture.registry, entity, source));
    CHECK(skinning.bind_rigid(fixture.registry, fixture.animation, meshes, entity, source, 0));
    const auto output = skinning.inspect(entity)->output;
    fixture.edit_pose(entity, 3.0);
    const auto edited = skinning.update(fixture.registry, fixture.animation, meshes);
    CHECK(edited.skinned == 1 && edited.retired == 0 && edited.errors == 0);
    CHECK(std::abs(meshes.get(output)->vertices[0].position.x -
                   meshes.get(source)->vertices[0].position.x - 3.0) < 1.0e-9);

    fixture.replace_twice(entity);
    const auto replaced = skinning.update(fixture.registry, fixture.animation, meshes);
    CHECK(replaced.retired == 1 && replaced.skinned == 0 && replaced.errors == 0);
    CHECK(!skinning.inspect(entity));
    CHECK(!meshes.contains(output));
    CHECK(!fixture.registry.has<animation::SkinningComponent>(entity));
    CHECK(fixture.registry.require<geometry::MeshComponent>(entity).mesh == source);
    CHECK(fixture.x(entity) == 9.0);
    CHECK(skinning.bind_rigid(fixture.registry, fixture.animation, meshes, entity, source, 0));
    CHECK(skinning.update(fixture.registry, fixture.animation, meshes).skinned == 1);
    CHECK(skinning.clear(fixture.registry, meshes));
}

void ik_retires_replaced_components_without_changing_the_new_pose() {
    Fixture fixture;
    const auto entity = fixture.create();
    animation::IKService ik;
    animation::IKConstraint constraint;
    constraint.name = "aim";
    constraint.kind = animation::IKConstraintKind::LookAt;
    constraint.chain = {0};
    constraint.target = {animation::IKTargetSpace::ModelDirection, {0.0, 0.0, -1.0}, {}};
    auto rig = std::make_shared<animation::IKRig>();
    CHECK(rig->load(*fixture.animation.skeletons.get(fixture.skeleton), {constraint}));
    CHECK(ik.publish("aim", rig));
    animation::IKBindingOptions options;
    options.write_scene = false;
    const auto binding = ik.attach(fixture.registry, fixture.animation, nullptr, entity, "aim", options);
    CHECK(binding);
    fixture.edit_pose(entity, 3.0);
    const auto edited = ik.update(fixture.registry, fixture.animation, nullptr);
    CHECK(edited.solved == 1 && edited.retired == 0 && edited.errors == 0);
    CHECK(fixture.x(entity) == 3.0);

    fixture.replace_twice(entity);
    const auto replaced = ik.update(fixture.registry, fixture.animation, nullptr);
    CHECK(replaced.retired == 1 && replaced.solved == 0 && replaced.errors == 0);
    CHECK(!ik.inspect(binding.handle));
    CHECK(!ik.binding(entity));
    CHECK(!fixture.registry.has<animation::IKComponent>(entity));
    CHECK(fixture.x(entity) == 9.0);
}

void blend_authority_does_not_survive_replacement_or_cross_registries() {
    Fixture fixture;
    const auto entity = fixture.create();
    animation::BlendService blend;
    CHECK(blend.publish("idle", make_machine(*fixture.animation.skeletons.get(fixture.skeleton))));
    CHECK(blend.attach(fixture.registry, entity, fixture.animation, "idle"));
    fixture.edit_pose(entity, 3.0);
    CHECK(animation::blend_owns_pose(fixture.registry, entity));
    const auto edited = blend.update(fixture.registry, fixture.animation, nullptr, 0.1);
    CHECK(edited.animated == 1 && edited.errors == 0);
    CHECK(fixture.x(entity) == 0.0);

    // Equal component IDs in another registry do not transfer authority.
    Fixture other;
    const auto other_entity = other.create();
    CHECK(fixture.registry.component_instance<animation::AnimatedComponent>(entity) ==
          other.registry.component_instance<animation::AnimatedComponent>(other_entity));
    other.registry.emplace<animation::BlendComponent>(other_entity,
        fixture.registry.require<animation::BlendComponent>(entity));
    CHECK(!animation::blend_owns_pose(other.registry, other_entity));

    fixture.replace_twice(entity);
    auto& replacement = fixture.registry.require<animation::AnimatedComponent>(entity);
    replacement.enabled = false;
    CHECK(!animation::blend_owns_pose(fixture.registry, entity));
    const auto replaced = blend.update(fixture.registry, fixture.animation, nullptr, 0.1);
    CHECK(replaced.animated == 0 && replaced.errors == 1 && replaced.ownership_reassertions == 0);
    CHECK(fixture.x(entity) == 9.0);
    CHECK(blend.detach(fixture.registry, entity));
    CHECK(!replacement.enabled);
    CHECK(!fixture.registry.has<animation::BlendComponent>(entity));
}

void retarget_retires_replaced_sources_targets_and_root_owners() {
    for (const bool replace_source : {false, true}) {
        Fixture fixture;
        const auto source = fixture.create();
        const auto target = fixture.create();
        animation::RetargetService retarget;
        const auto& skeleton = *fixture.animation.skeletons.get(fixture.skeleton);
        auto map = std::make_shared<animation::RetargetMap>();
        CHECK(map->match_names(skeleton, skeleton));
        CHECK(retarget.publish("copy", map));
        animation::RetargetBindingOptions options;
        options.write_scene = false;
        const auto binding = retarget.attach(fixture.registry, fixture.animation, nullptr, source, target, "copy", options);
        CHECK(binding);
        const auto replaced_entity = replace_source ? source : target;
        animation::RootMotionBindingOptions root_options;
        root_options.write_scene = false;
        root_options.strip = false;
        const auto root = retarget.attach_root(fixture.registry, fixture.animation, nullptr, nullptr,
            replaced_entity, root_options);
        CHECK(root);
        fixture.edit_pose(source, 3.0);
        const auto edited = retarget.update(fixture.registry, fixture.animation, nullptr, 0.1, 1);
        CHECK(edited.retargeted == 1 && edited.captured == 1 && edited.retired == 0 && edited.errors == 0);
        CHECK(fixture.x(target) == 3.0);

        fixture.replace_twice(replaced_entity);
        const auto replaced = retarget.update(fixture.registry, fixture.animation, nullptr, 0.1, 2);
        CHECK(replaced.retired == 2 && replaced.retargeted == 0 && replaced.captured == 0 && replaced.errors == 0);
        CHECK(!retarget.inspect(binding.handle));
        CHECK(!retarget.inspect_root(root.handle));
        CHECK(fixture.x(replaced_entity) == 9.0);
    }
}

} // namespace

int main() {
    return test::run("animation_owner_audit", {
        {"skinning component replacement", skinning_retires_replaced_components_and_releases_outputs},
        {"IK component replacement", ik_retires_replaced_components_without_changing_the_new_pose},
        {"blend component authority", blend_authority_does_not_survive_replacement_or_cross_registries},
        {"retarget component replacement", retarget_retires_replaced_sources_targets_and_root_owners},
    });
}
