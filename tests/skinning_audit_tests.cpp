#include "test_support.hpp"

#include <ge/animation/ik.hpp>
#include <ge/animation/retarget.hpp>
#include <ge/animation/skinning.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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
    animation::AnimationService animation;
    geometry::MeshLibrary meshes;
    animation::SkinningService skinning;
    animation::SkeletonHandle skeleton = animation.skeletons.create(make_skeleton(), "rig");
    geometry::MeshHandle source = meshes.create(geometry::make_plane(), "shared");

    ecs::Entity create(std::string_view name = "actor") {
        const auto entity = registry.create({}, name);
        CHECK(entity);
        CHECK(animation.attach(registry, entity, skeleton));
        CHECK(meshes.assign(registry, entity, source));
        return entity;
    }

    geometry::MeshHandle bind(ecs::Entity entity) {
        CHECK(skinning.bind_rigid(registry, animation, meshes, entity, source, 0));
        return skinning.inspect(entity)->output;
    }
};

void final_animation_deforms_only_its_instance() {
    Fixture fixture;
    const auto first = fixture.create("first");
    const auto second = fixture.create("second");
    const auto original = *fixture.meshes.get(fixture.source);
    const auto source_revision = fixture.meshes.inspect(fixture.source)->revision;
    animation::BoneChannel channel;
    channel.bone = 0;
    channel.translations = {{0.0, {}}, {2.0, {4.0, 0.0, 0.0}}};
    animation::AnimationClip clip;
    CHECK(clip.load("move", 2.0, *fixture.animation.skeletons.get(fixture.skeleton), {channel}));
    const auto clip_handle = fixture.animation.clips.create(std::move(clip), "move");
    CHECK(fixture.animation.play(fixture.registry, first, clip_handle, 0.0, animation::WrapMode::Clamp));
    const auto first_output = fixture.bind(first);
    const auto second_output = fixture.bind(second);
    CHECK(first_output != second_output);
    CHECK(first_output != fixture.source);
    CHECK(second_output != fixture.source);
    CHECK(fixture.animation.update(fixture.registry, nullptr, 0.5).errors == 0);
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).skinned == 2);
    near(fixture.meshes.get(first_output)->vertices[0].position.x, original.vertices[0].position.x + 1.0);
    CHECK(fixture.meshes.get(second_output)->vertices == original.vertices);
    CHECK(fixture.meshes.get(fixture.source)->vertices == original.vertices);
    CHECK(fixture.meshes.inspect(fixture.source)->revision == source_revision);
    CHECK(fixture.meshes.get(first_output)->indices == original.indices);
    CHECK(fixture.meshes.get(first_output)->submeshes == original.submeshes);
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).skinned == 2);
    near(fixture.meshes.get(first_output)->vertices[0].position.x, original.vertices[0].position.x + 1.0);
    CHECK(fixture.skinning.clear(fixture.registry, fixture.meshes));
    CHECK(fixture.meshes.statistics().meshes == 1);
}

void arbitrary_weights_and_rejected_replacement_are_transactional() {
    Fixture fixture;
    const auto entity = fixture.create();
    auto& pose = fixture.registry.require<animation::AnimatedComponent>(entity).animator.pose();
    pose.local[1].position.x = 2.0;
    CHECK(pose.update(*fixture.animation.skeletons.get(fixture.skeleton)));
    const auto& original = *fixture.meshes.get(fixture.source);
    std::vector<animation::VertexInfluences> weights(original.vertices.size());
    weights[0].count = 2;
    weights[0].values[0] = {0, 2.0};
    weights[0].values[1] = {1, 2.0};
    CHECK(fixture.skinning.bind(fixture.registry, fixture.animation, fixture.meshes, entity, fixture.source, weights));
    const auto output = fixture.skinning.inspect(entity)->output;
    const auto revision = fixture.meshes.inspect(output)->revision;
    const auto displayed = fixture.meshes.get(output)->vertices;
    near(displayed[0].position.x, original.vertices[0].position.x + 1.0);
    CHECK(displayed[1] == original.vertices[1]);

    weights[0].values[1].bone = 1000;
    CHECK(!fixture.skinning.bind(fixture.registry, fixture.animation, fixture.meshes, entity, fixture.source, weights));
    CHECK(fixture.skinning.inspect(entity)->output == output);
    CHECK(fixture.registry.require<geometry::MeshComponent>(entity).mesh == output);
    CHECK(fixture.meshes.inspect(output)->revision == revision);
    CHECK(fixture.meshes.get(output)->vertices == displayed);
    CHECK(!fixture.skinning.bind_rigid(fixture.registry, fixture.animation, fixture.meshes, entity, output, 0));
    CHECK(fixture.skinning.size() == 1);
    CHECK(fixture.meshes.statistics().meshes == 2);

    pose.skin[1](0, 0) = std::numeric_limits<double>::quiet_NaN();
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).errors == 1);
    CHECK(fixture.meshes.inspect(output)->revision == revision);
    CHECK(fixture.meshes.get(output)->vertices == displayed);
    CHECK(pose.update(*fixture.animation.skeletons.get(fixture.skeleton)));
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).skinned == 1);
    CHECK(fixture.skinning.clear(fixture.registry, fixture.meshes));
}

void source_revision_requires_an_explicit_rebind() {
    Fixture fixture;
    const auto entity = fixture.create();
    const auto output = fixture.bind(entity);
    const auto old_binding = *fixture.skinning.inspect(entity);
    const auto displayed = fixture.meshes.get(output)->vertices;
    const auto output_revision = fixture.meshes.inspect(output)->revision;
    CHECK(fixture.meshes.replace(fixture.source, geometry::make_box()));
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).errors == 1);
    CHECK(fixture.skinning.inspect(entity)->source_revision == old_binding.source_revision);
    CHECK(fixture.meshes.inspect(output)->revision == output_revision);
    CHECK(fixture.meshes.get(output)->vertices == displayed);
    CHECK(fixture.skinning.bind_rigid(fixture.registry, fixture.animation, fixture.meshes, entity, fixture.source, 0));
    CHECK(fixture.skinning.inspect(entity)->output == output);
    CHECK(fixture.skinning.inspect(entity)->source_revision == fixture.meshes.inspect(fixture.source)->revision);
    CHECK(fixture.meshes.get(output)->vertices == fixture.meshes.get(fixture.source)->vertices);
    CHECK(fixture.skinning.clear(fixture.registry, fixture.meshes));
}

void owned_meshes_retire_on_detach_and_lost_bindings() {
    Fixture fixture;
    const auto entity = fixture.create();
    auto output = fixture.bind(entity);
    CHECK(fixture.skinning.detach(fixture.registry, fixture.meshes, entity));
    CHECK(!fixture.meshes.contains(output));
    CHECK(fixture.registry.require<geometry::MeshComponent>(entity).mesh == fixture.source);
    CHECK(!fixture.registry.has<animation::SkinningComponent>(entity));

    output = fixture.bind(entity);
    CHECK(fixture.registry.remove<animation::SkinningComponent>(entity));
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).retired == 1);
    CHECK(!fixture.meshes.contains(output));
    CHECK(fixture.registry.require<geometry::MeshComponent>(entity).mesh == fixture.source);

    output = fixture.bind(entity);
    CHECK(fixture.registry.remove<animation::AnimatedComponent>(entity));
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).retired == 1);
    CHECK(!fixture.meshes.contains(output));
    CHECK(!fixture.registry.has<animation::SkinningComponent>(entity));
    CHECK(fixture.animation.attach(fixture.registry, entity, fixture.skeleton));

    output = fixture.bind(entity);
    const auto external = fixture.meshes.create(geometry::make_box(), "external");
    CHECK(fixture.meshes.assign(fixture.registry, entity, external));
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).retired == 1);
    CHECK(!fixture.meshes.contains(output));
    CHECK(fixture.registry.require<geometry::MeshComponent>(entity).mesh == external);

    CHECK(fixture.meshes.assign(fixture.registry, entity, fixture.source));
    output = fixture.bind(entity);
    CHECK(fixture.registry.destroy(entity));
    const auto replacement = fixture.create("replacement");
    CHECK(replacement != entity);
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).retired == 1);
    CHECK(!fixture.meshes.contains(output));
    CHECK(fixture.registry.require<geometry::MeshComponent>(replacement).mesh == fixture.source);

    output = fixture.bind(replacement);
    CHECK(fixture.meshes.destroy(fixture.source));
    CHECK(fixture.skinning.update(fixture.registry, fixture.animation, fixture.meshes).retired == 1);
    CHECK(!fixture.meshes.contains(output));
    CHECK(!fixture.registry.has<geometry::MeshComponent>(replacement));
    CHECK(!fixture.registry.has<animation::SkinningComponent>(replacement));
    CHECK(fixture.skinning.size() == 0);
}

void final_pose_reaches_bounds_and_render_in_the_same_tick() {
    core::Kernel kernel;
    auto& registry = kernel.services().emplace<ecs::Registry>();
    geometry::register_mesh(kernel);
    animation::register_skeleton(kernel);
    animation::register_ik(kernel);
    animation::register_retarget(kernel);
    auto& animation_service = kernel.services().get<animation::AnimationService>();
    auto& meshes = kernel.services().get<geometry::MeshLibrary>();
    const auto skeleton = animation_service.skeletons.create(make_skeleton(), "rig");
    const auto source = meshes.create(geometry::make_plane(), "shared");
    const auto entity = registry.create({}, "actor");
    CHECK(animation_service.attach(registry, entity, skeleton));
    CHECK(meshes.assign(registry, entity, source));
    CHECK(kernel.add_system({"audit.post_ik_pose", core::Phase::Animation, 200, {std::string(animation::ik_system_name)}},
        [&](core::Kernel&, const core::Frame&) {
            auto& pose = registry.require<animation::AnimatedComponent>(entity).animator.pose();
            pose.local[0].position.x = 5.0;
            CHECK(pose.update(*animation_service.skeletons.get(skeleton)));
        }));
    animation::register_skinning(kernel);
    const auto original_x = meshes.get(source)->vertices[0].position.x;
    const auto original_min = meshes.inspect(source)->bounds.min.x;
    bool render_observed = false;
    CHECK(kernel.add_system({"audit.render_consumer", core::Phase::Render, 0, {}},
        [&](core::Kernel&, const core::Frame&) {
            const auto handle = registry.require<geometry::MeshComponent>(entity).mesh;
            near(meshes.get(handle)->vertices[0].position.x, original_x + 5.0);
            near(registry.require<ecs::BoundsComponent>(entity).local.min.x, original_min + 5.0);
            render_observed = true;
        }));
    CHECK(kernel.execute("animation.skin actor shared root").success);
    const auto output = registry.require<geometry::MeshComponent>(entity).mesh;
    const auto order = kernel.execution_order();
    const auto position = [&](std::string_view name) { return std::find(order.begin(), order.end(), name); };
    CHECK(position("animation.skeleton") < position(animation::skinning_system_name));
    CHECK(position(animation::ik_system_name) < position(animation::skinning_system_name));
    CHECK(position(animation::retarget_system_name) < position(animation::skinning_system_name));
    CHECK(position("audit.post_ik_pose") < position(animation::skinning_system_name));
    CHECK(position(animation::skinning_system_name) < position("geometry.mesh.bounds"));
    CHECK(kernel.tick(0.1));
    CHECK(render_observed);
    CHECK(kernel.metric_value("animation.skinning.skinned") == 1.0);
    CHECK(kernel.metric_value("animation.skinning.errors") == 0.0);
    CHECK(kernel.execute("animation.skin actor off").success);
    CHECK(!meshes.contains(output));
    CHECK(registry.require<geometry::MeshComponent>(entity).mesh == source);
    CHECK(kernel.execute("animation.skin actor shared 0").success);
    const auto shutdown_output = registry.require<geometry::MeshComponent>(entity).mesh;
    kernel.shutdown();
    CHECK(!meshes.contains(shutdown_output));
    CHECK(registry.require<geometry::MeshComponent>(entity).mesh == source);
}

} // namespace

int main() {
    return test::run("skinning_audit", {
        {"instance deformation", final_animation_deforms_only_its_instance},
        {"transactional influences and deformation", arbitrary_weights_and_rejected_replacement_are_transactional},
        {"source revision", source_revision_requires_an_explicit_rebind},
        {"binding retirement", owned_meshes_retire_on_detach_and_lost_bindings},
        {"final pose scheduling and commands", final_pose_reaches_bounds_and_render_in_the_same_tick},
    });
}
