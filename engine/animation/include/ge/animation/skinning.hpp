#pragma once

#include <ge/animation/skeleton.hpp>

#include <map>
#include <memory>

namespace ge::animation {

inline constexpr int skinning_pose_order = 300;
inline constexpr std::string_view skinning_system_name = "animation.skinning";

struct SkinningLimits {
    std::size_t max_bindings{512};
    // Total influenced vertices across bindings, also bounding work per update.
    std::size_t max_vertices{1'048'576};
};

// This ownership marker is deliberately not cloneable. Bind a cloned entity
// explicitly so it receives its own output mesh.
struct SkinningComponent {
    explicit SkinningComponent(std::uint64_t value = 0) : binding(value) {}
    SkinningComponent(const SkinningComponent&) = delete;
    SkinningComponent& operator=(const SkinningComponent&) = delete;
    SkinningComponent(SkinningComponent&&) noexcept = default;
    SkinningComponent& operator=(SkinningComponent&&) noexcept = default;
    std::uint64_t binding{};
};

struct SkinningBindingInfo {
    geometry::MeshHandle source{};
    std::uint64_t source_revision{};
    geometry::MeshHandle output{};
    AnimationResult last_result{};
};

struct SkinningUpdate {
    std::size_t skinned{};
    std::size_t skipped{};
    std::size_t errors{};
    std::size_t retired{};
};

// Single-writer adapter for an existing AnimatedComponent and MeshComponent.
// Final blend-owned poses remain eligible while blend authority disables the
// base player. Disabled entities and disabled unblended players retain output.
// Source handles/revisions are immutable until an explicit successful rebind.
// A changed source revision or failed deformation retains the last valid output.
// A missing source/entity/component or replaced AnimatedComponent retires its
// owned output on the next update, even if component storage reuses an address.
// Invalid bindings never replace the current binding or rendered mesh. Mesh
// allocations obey MeshLibrary budgets; allocation failures otherwise propagate.
// One service uses one registry, animation service and mesh library while bound.
// Call clear before destroying those owners; kernel registration does so at shutdown.
// A replacement mesh library at a reused address is rejected without modifying it.
class SkinningService final {
public:
    explicit SkinningService(SkinningLimits limits = {});
    SkinningService(const SkinningService&) = delete;
    SkinningService& operator=(const SkinningService&) = delete;

    [[nodiscard]] AnimationResult bind(ecs::Registry& registry, const AnimationService& animation,
                                       geometry::MeshLibrary& meshes, ecs::Entity entity,
                                       geometry::MeshHandle source,
                                       std::span<const VertexInfluences> influences);
    [[nodiscard]] AnimationResult bind_rigid(ecs::Registry& registry, const AnimationService& animation,
                                             geometry::MeshLibrary& meshes, ecs::Entity entity,
                                             geometry::MeshHandle source, BoneIndex bone);
    bool detach(ecs::Registry& registry, geometry::MeshLibrary& meshes, ecs::Entity entity);
    bool clear(ecs::Registry& registry, geometry::MeshLibrary& meshes);
    [[nodiscard]] SkinningUpdate update(ecs::Registry& registry, const AnimationService& animation,
                                        geometry::MeshLibrary& meshes);
    [[nodiscard]] std::optional<SkinningBindingInfo> inspect(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Binding {
        SkinningBindingInfo info;
        std::uint64_t token{};
        SkeletonHandle skeleton{};
        std::uint64_t skeleton_signature{};
        std::uint64_t owner{};
        std::vector<VertexInfluences> influences;
    };
    SkinningLimits limits_;
    std::map<ecs::Entity, Binding> bindings_;
    std::size_t vertices_{};
    std::uint64_t next_token_{1};
    const ecs::Registry* registry_{};
    const AnimationService* animation_{};
    std::shared_ptr<const geometry::MeshLibrary::LifetimeIdentity> mesh_lifetime_;
};

void register_skinning(core::Kernel& kernel);

} // namespace ge::animation
