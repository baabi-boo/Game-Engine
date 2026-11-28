#include <ge/animation/skinning.hpp>

#include <ge/animation/blend.hpp>
#include <ge/animation/ik.hpp>
#include <ge/animation/retarget.hpp>
#include <ge/core/runtime.hpp>

#include <charconv>
#include <stdexcept>
#include <utility>

namespace ge::animation {
namespace {

bool valid_selection(const geometry::MeshComponent& component, const geometry::MeshInfo& info) {
    return component.first_submesh < info.submeshes &&
        component.submesh_count <= info.submeshes - component.first_submesh;
}

AnimationResult publish_mesh(geometry::MeshLibrary& meshes, geometry::MeshData mesh,
                             geometry::MeshHandle& output) {
    try {
        if (output) {
            if (!meshes.replace(output, std::move(mesh))) { return {AnimationError::InvalidHandle}; }
        } else {
            output = meshes.create(std::move(mesh));
        }
    } catch (const std::invalid_argument&) {
        return {AnimationError::InvalidMesh};
    } catch (const std::length_error&) {
        return {AnimationError::Capacity};
    } catch (const std::overflow_error&) {
        return {AnimationError::Capacity};
    }
    return {};
}

} // namespace

SkinningService::SkinningService(SkinningLimits limits) : limits_(limits) {
    if (limits.max_bindings == 0 || limits.max_bindings > 65'536 || limits.max_vertices == 0 ||
        limits.max_vertices > geometry::MeshLimits{}.max_vertices) {
        throw std::invalid_argument("invalid skinning limits");
    }
}

AnimationResult SkinningService::bind(ecs::Registry& registry, const AnimationService& animation,
                                       geometry::MeshLibrary& meshes, ecs::Entity entity,
                                       geometry::MeshHandle source,
                                       std::span<const VertexInfluences> influences) {
    if (registry.iterating() || (registry_ && registry_ != &registry) ||
        (animation_ && animation_ != &animation) ||
        (mesh_lifetime_ && mesh_lifetime_ != meshes.lifetime_identity())) {
        return {AnimationError::InvalidEntity};
    }
    auto* animated = registry.get<AnimatedComponent>(entity);
    auto* component = registry.get<geometry::MeshComponent>(entity);
    if (!animated || !component) { return {AnimationError::InvalidEntity}; }
    const auto* skeleton = animation.skeletons.get(animated->skeleton);
    const auto& pose = animated->animator.pose();
    if (!skeleton || !pose.compatible(*skeleton) || pose.skin.size() != skeleton->size()) {
        return {AnimationError::IncompatibleSkeleton};
    }
    const auto source_info = meshes.inspect(source);
    if (!source_info || !valid_selection(*component, *source_info)) { return {AnimationError::InvalidMesh}; }
    if (source_info->vertices != influences.size()) { return {AnimationError::InvalidInfluence}; }
    for (const auto& [bound_entity, binding] : bindings_) {
        (void)bound_entity;
        if (binding.info.output == source) { return {AnimationError::InvalidMesh}; }
    }
    const auto existing = bindings_.find(entity);
    const bool replacing = existing != bindings_.end();
    const auto* marker = registry.get<SkinningComponent>(entity);
    if ((marker && (!replacing || marker->binding != existing->second.token)) ||
        (replacing && (!marker || component->mesh != existing->second.info.output))) {
        return {AnimationError::InvalidEntity};
    }
    const auto old_vertices = replacing ? existing->second.influences.size() : 0;
    if ((!replacing && (bindings_.size() >= limits_.max_bindings || next_token_ == 0)) ||
        influences.size() > limits_.max_vertices - (vertices_ - old_vertices)) {
        return {AnimationError::Capacity};
    }

    Binding candidate;
    candidate.info.source = source;
    candidate.info.source_revision = source_info->revision;
    candidate.token = replacing ? existing->second.token : next_token_;
    candidate.skeleton = animated->skeleton;
    candidate.skeleton_signature = skeleton->signature();
    candidate.owner = registry.component_instance<AnimatedComponent>(entity);
    candidate.influences.assign(influences.begin(), influences.end());
    for (std::size_t index = 0; index < candidate.influences.size(); ++index) {
        const auto normalized = normalize_influences(candidate.influences[index], skeleton->size());
        if (!normalized) { return {normalized.error, index}; }
    }
    geometry::MeshData output;
    const auto skinned = skin_mesh(*meshes.get(source), candidate.influences, pose, output);
    if (!skinned) { return skinned; }

    if (replacing) {
        candidate.info.output = existing->second.info.output;
        const auto published = publish_mesh(meshes, std::move(output), candidate.info.output);
        if (!published) { return published; }
        existing->second = std::move(candidate);
    } else {
        const auto published = publish_mesh(meshes, std::move(output), candidate.info.output);
        if (!published) { return published; }
        const auto owned_output = candidate.info.output;
        try {
            bindings_.emplace(entity, std::move(candidate));
            registry.emplace<SkinningComponent>(entity, next_token_);
        } catch (...) {
            bindings_.erase(entity);
            (void)meshes.destroy(owned_output);
            throw;
        }
        component->mesh = owned_output;
        ++next_token_; // Zero after exhaustion prevents token reuse.
    }
    vertices_ = vertices_ - old_vertices + influences.size();
    registry_ = &registry;
    animation_ = &animation;
    mesh_lifetime_ = meshes.lifetime_identity();
    (void)registry.mark_changed<geometry::MeshComponent>(entity);
    return {};
}

AnimationResult SkinningService::bind_rigid(ecs::Registry& registry, const AnimationService& animation,
                                             geometry::MeshLibrary& meshes, ecs::Entity entity,
                                             geometry::MeshHandle source, BoneIndex bone) {
    const auto* animated = registry.get<AnimatedComponent>(entity);
    const auto* skeleton = animated ? animation.skeletons.get(animated->skeleton) : nullptr;
    if (!skeleton || bone >= skeleton->size()) { return {AnimationError::InvalidBone}; }
    const auto info = meshes.inspect(source);
    if (!info) { return {AnimationError::InvalidMesh}; }
    if (info->vertices > limits_.max_vertices) { return {AnimationError::Capacity}; }
    VertexInfluences rigid;
    rigid.count = 1;
    rigid.values[0] = {bone, 1.0};
    const std::vector<VertexInfluences> influences(info->vertices, rigid);
    return bind(registry, animation, meshes, entity, source, influences);
}

bool SkinningService::detach(ecs::Registry& registry, geometry::MeshLibrary& meshes, ecs::Entity entity) {
    if (registry.iterating() || registry_ != &registry ||
        mesh_lifetime_ != meshes.lifetime_identity()) { return false; }
    const auto found = bindings_.find(entity);
    if (found == bindings_.end()) { return false; }
    const auto& binding = found->second;
    if (auto* component = registry.get<geometry::MeshComponent>(entity);
        component && component->mesh == binding.info.output) {
        const auto source = meshes.inspect(binding.info.source);
        if (source && valid_selection(*component, *source)) {
            component->mesh = binding.info.source;
            (void)registry.mark_changed<geometry::MeshComponent>(entity);
        } else {
            (void)registry.remove<geometry::MeshComponent>(entity);
        }
    }
    if (const auto* marker = registry.get<SkinningComponent>(entity); marker && marker->binding == binding.token) {
        (void)registry.remove<SkinningComponent>(entity);
    }
    (void)meshes.destroy(binding.info.output);
    vertices_ -= binding.influences.size();
    bindings_.erase(found);
    if (bindings_.empty()) { registry_ = nullptr; animation_ = nullptr; mesh_lifetime_.reset(); }
    return true;
}

bool SkinningService::clear(ecs::Registry& registry, geometry::MeshLibrary& meshes) {
    if (registry.iterating() || (registry_ && registry_ != &registry) ||
        (mesh_lifetime_ && mesh_lifetime_ != meshes.lifetime_identity())) {
        return false;
    }
    while (!bindings_.empty()) { (void)detach(registry, meshes, bindings_.begin()->first); }
    return true;
}

SkinningUpdate SkinningService::update(ecs::Registry& registry, const AnimationService& animation,
                                        geometry::MeshLibrary& meshes) {
    SkinningUpdate result;
    if (registry.iterating() || (registry_ && registry_ != &registry) ||
        (animation_ && animation_ != &animation) ||
        (mesh_lifetime_ && mesh_lifetime_ != meshes.lifetime_identity())) {
        result.errors = 1;
        return result;
    }
    for (auto iterator = bindings_.begin(); iterator != bindings_.end();) {
        const auto entity = iterator->first;
        auto& binding = iterator->second;
        ++iterator;
        const auto* animated = registry.get<AnimatedComponent>(entity);
        const auto* marker = registry.get<SkinningComponent>(entity);
        const auto* component = registry.get<geometry::MeshComponent>(entity);
        const auto* skeleton = animated ? animation.skeletons.get(animated->skeleton) : nullptr;
        const auto source = meshes.inspect(binding.info.source);
        if (!animated || registry.component_instance<AnimatedComponent>(entity) != binding.owner ||
            !marker || marker->binding != binding.token ||
            !component || component->mesh != binding.info.output || !source || !skeleton ||
            animated->skeleton != binding.skeleton || skeleton->signature() != binding.skeleton_signature ||
            !meshes.contains(binding.info.output)) {
            (void)detach(registry, meshes, entity);
            ++result.retired;
            continue;
        }
        if (!registry.enabled(entity) ||
            (!animated->enabled && !blend_owns_pose(registry, entity))) {
            ++result.skipped;
            continue;
        }
        binding.info.last_result = {};
        if (source->revision != binding.info.source_revision) {
            binding.info.last_result = {AnimationError::InvalidMesh};
        } else {
            const auto& pose = animated->animator.pose();
            if (!pose.compatible(*skeleton) || pose.skin.size() != skeleton->size()) {
                binding.info.last_result = {AnimationError::IncompatibleSkeleton};
            } else {
                geometry::MeshData output;
                binding.info.last_result = skin_mesh(*meshes.get(binding.info.source), binding.influences, pose, output);
                if (binding.info.last_result) {
                    binding.info.last_result = publish_mesh(meshes, std::move(output), binding.info.output);
                }
            }
        }
        if (!binding.info.last_result) { ++result.errors; continue; }
        (void)registry.mark_changed<geometry::MeshComponent>(entity);
        ++result.skinned;
    }
    return result;
}

std::optional<SkinningBindingInfo> SkinningService::inspect(ecs::Entity entity) const noexcept {
    const auto found = bindings_.find(entity);
    return found == bindings_.end() ? std::nullopt : std::optional{found->second.info};
}

std::size_t SkinningService::size() const noexcept { return bindings_.size(); }

void register_skinning(core::Kernel& kernel) {
    if (kernel.services().find<SkinningService>()) { return; }
    auto& service = kernel.services().emplace<SkinningService>();
    kernel.register_module({"animation.skinning", "Per-instance meshes from the final animated pose", 1});
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        registry->register_component<SkinningComponent>("animation.skinning");
    }
    std::vector<std::string> after{"animation.skeleton"};
    for (const auto& system : kernel.system_statistics()) {
        if (system.name == ik_system_name || system.name == retarget_system_name || system.name == "animation.blend") {
            after.push_back(system.name);
        }
    }
    kernel.add_system({std::string(skinning_system_name), core::Phase::Animation, skinning_pose_order, std::move(after)},
        [owner = &service](core::Kernel& active, const core::Frame&) {
            auto* registry = active.services().find<ecs::Registry>();
            const auto* animation = active.services().find<AnimationService>();
            auto* meshes = active.services().find<geometry::MeshLibrary>();
            if (!registry || !animation || !meshes) { return; }
            const auto update = owner->update(*registry, *animation, *meshes);
            active.metric("animation.skinning.skinned", static_cast<double>(update.skinned));
            active.metric("animation.skinning.errors", static_cast<double>(update.errors));
            active.metric("animation.skinning.retired", static_cast<double>(update.retired));
        });
    kernel.add_shutdown("animation.skinning", [owner = &service, active = &kernel] {
        auto* registry = active->services().find<ecs::Registry>();
        auto* meshes = active->services().find<geometry::MeshLibrary>();
        if (registry && meshes) { (void)owner->clear(*registry, *meshes); }
    });
    kernel.add_command("animation.skin", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        const auto* animation = active.services().find<AnimationService>();
        auto* meshes = active.services().find<geometry::MeshLibrary>();
        if (!registry || !animation || !meshes ||
            (args.size() != 3 && (args.size() != 2 || args[1] != "off"))) {
            return core::CommandResult{false, "usage: animation.skin ENTITY MESH BONE | animation.skin ENTITY off"};
        }
        const auto entity = registry->find_name(args[0]);
        if (args.size() == 2) {
            const bool detached = owner->detach(*registry, *meshes, entity);
            return core::CommandResult{detached, detached ? "skinning detached" : "skinning binding unavailable"};
        }
        const auto* animated = registry->get<AnimatedComponent>(entity);
        const auto* skeleton = animated ? animation->skeletons.get(animated->skeleton) : nullptr;
        BoneIndex bone = skeleton ? skeleton->find(args[2]) : no_bone;
        if (bone == no_bone) {
            const auto parsed = std::from_chars(args[2].data(), args[2].data() + args[2].size(), bone);
            if (parsed.ec != std::errc{} || parsed.ptr != args[2].data() + args[2].size()) { bone = no_bone; }
        }
        const auto result = owner->bind_rigid(*registry, *animation, *meshes, entity, meshes->find(args[1]), bone);
        return core::CommandResult{static_cast<bool>(result), result ? "skinning bound" : std::string(describe(result.error))};
    }, "Bind rigid bone weights to a source mesh, or detach and release the instance output");
}

} // namespace ge::animation
