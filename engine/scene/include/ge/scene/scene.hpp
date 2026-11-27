#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::scene {

using Entity = ecs::Entity;
inline constexpr std::size_t append_child = std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t no_parent = std::numeric_limits<std::size_t>::max();

enum class SceneError : std::uint8_t {
    None,
    InvalidEntity,
    NotInScene,
    InvalidParent,
    Cycle,
    DepthLimit,
    Capacity,
    InvalidTransform,
    SingularParent,
    UnrepresentableTransform,
    InvalidBounds,
    InvalidName,
    InvalidPath,
    AmbiguousPath,
    ChildrenPresent,
    DuplicateEntity,
    InvalidSnapshot,
    IdentityConflict,
    RegistryBusy,
    CloneRejected
};

[[nodiscard]] std::string_view describe(SceneError error) noexcept;

struct SceneResult {
    SceneError error{SceneError::None};
    std::size_t affected{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == SceneError::None;
    }
};

struct SceneLimits {
    std::size_t max_nodes{65536};
    std::size_t max_depth{4096};
    std::size_t max_path_bytes{65536};
    bool adopt_registry_transforms{true};
};

enum class ReparentMode : std::uint8_t { KeepLocal, KeepWorld };
enum class TransformSpace : std::uint8_t { Local, Parent, World };
enum class DestroyPolicy : std::uint8_t {
    Subtree,
    DetachChildren,
    PromoteChildren,
    RejectChildren
};

struct LocalTransformEdit {
    Entity entity{};
    math::Transform transform{};
};

struct WorldMatrixEdit {
    Entity entity{};
    math::Mat4 matrix{};
};

struct SceneUpdate {
    SceneError error{SceneError::None};
    std::size_t adopted{};
    std::size_t removed{};
    std::size_t transformed{};
    std::size_t bounds_updated{};
    std::size_t non_trs{};
    std::size_t invalid{};
    std::size_t skipped{};
    std::uint64_t revision{};
};

struct NodeInfo {
    Entity entity{};
    Entity parent{};
    std::vector<Entity> children;
    std::size_t depth{};
    std::size_t sibling_index{};
    math::Mat4 world_matrix{};
    std::optional<math::Aabb> world_bounds;
    std::optional<math::Aabb> subtree_bounds;
    bool effective_enabled{};
    bool world_transform_exact{};
    bool world_valid{};
    std::uint64_t revision{};
};

struct SceneStatistics {
    std::size_t nodes{};
    std::size_t roots{};
    std::size_t deepest_level{};
    std::size_t dirty_nodes{};
    std::size_t non_trs_nodes{};
    std::size_t invalid_nodes{};
    std::size_t bounded_nodes{};
    std::uint64_t revision{};
};

struct EntityRemap {
    Entity source{};
    Entity destination{};
};

struct CloneResult {
    SceneError error{SceneError::None};
    Entity root{};
    std::vector<EntityRemap> entities;
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == SceneError::None;
    }
};

struct SnapshotNode {
    Entity source{};
    PersistentId persistent{};
    std::size_t parent_index{no_parent};
    std::string name;
    math::Transform local{};
    std::optional<math::Aabb> bounds;
    bool enabled{true};
    std::uint64_t layers{1};
    std::vector<std::string> tags;
};

struct SceneSnapshot {
    std::uint32_t version{1};
    std::uint64_t revision{};
    std::vector<SnapshotNode> nodes;
};

struct PathResult {
    SceneError error{SceneError::None};
    Entity entity{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == SceneError::None;
    }
};

struct BoundsHit {
    Entity entity{};
    math::RayHit hit{};
};

// Single-writer hierarchy borrowing the registry for its entire lifetime. All
// handles, identities, component data and clone ownership remain in Registry.
// TransformComponent.local is input; world is synchronized TRS output. When
// affine composition introduces shear, world is a documented TRS approximation:
// use world_matrix() and world_transform_exact() for lossless spatial work.
// Local components edited directly through Registry are discovered by update().
class Scene final {
public:
    explicit Scene(ecs::Registry& shared_registry, SceneLimits configuration = {});
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    [[nodiscard]] ecs::Registry& registry() noexcept;
    [[nodiscard]] const ecs::Registry& registry() const noexcept;
    [[nodiscard]] const SceneLimits& limits() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool contains(Entity entity) const noexcept;

    // Invalid input/capacity returns null. Allocation failures propagate after
    // rolling back the new entity; no independent scene ID is ever allocated.
    [[nodiscard]] Entity create(std::string_view name = {}, Entity parent = {},
                                const math::Transform& local = {},
                                PersistentId persistent = {});
    SceneResult adopt(Entity entity, Entity parent = {},
                      std::size_t index = append_child);
    SceneResult reparent(Entity entity, Entity parent,
                         ReparentMode mode = ReparentMode::KeepWorld,
                         std::size_t index = append_child);
    SceneResult detach(Entity entity, ReparentMode mode = ReparentMode::KeepWorld,
                       std::size_t index = append_child);
    SceneResult reorder(Entity entity, std::size_t index);
    SceneResult destroy(Entity entity, DestroyPolicy policy = DestroyPolicy::Subtree,
                        ReparentMode child_mode = ReparentMode::KeepWorld);
    SceneResult destroy_all();

    [[nodiscard]] Entity parent(Entity entity) const noexcept;
    [[nodiscard]] std::vector<Entity> children(Entity entity) const;
    [[nodiscard]] std::vector<Entity> roots() const;
    [[nodiscard]] std::vector<Entity> ancestors(Entity entity) const;
    [[nodiscard]] std::vector<Entity> descendants(Entity entity, bool include_self = false) const;
    [[nodiscard]] std::vector<Entity> ordered_entities() const;
    [[nodiscard]] std::optional<std::size_t> depth(Entity entity) const noexcept;
    [[nodiscard]] std::optional<std::size_t> sibling_index(Entity entity) const noexcept;
    [[nodiscard]] bool is_ancestor(Entity ancestor, Entity descendant) const noexcept;
    [[nodiscard]] Entity common_ancestor(Entity first, Entity second) const noexcept;
    [[nodiscard]] std::optional<NodeInfo> inspect(Entity entity);
    [[nodiscard]] SceneStatistics statistics() const;

    SceneResult rename(Entity entity, std::string_view name);
    [[nodiscard]] std::vector<Entity> find_all_named(std::string_view name,
                                                    Entity within = {},
                                                    std::size_t limit = 65536) const;
    // Names use percent escaping. Anonymous or duplicate siblings use @index:generation.
    // Unqualified duplicate name segments are rejected. '/' denotes the virtual root.
    [[nodiscard]] std::optional<std::string> path(Entity entity) const;
    [[nodiscard]] PathResult find_path(std::string_view path, Entity relative_to = {}) const;

    SceneResult set_local_transform(Entity entity, const math::Transform& value);
    SceneResult set_world_transform(Entity entity, const math::Transform& value);
    SceneResult set_world_matrix(Entity entity, const math::Mat4& value);
    SceneResult set_local_position(Entity entity, math::Vec3 value);
    SceneResult set_world_position(Entity entity, math::Vec3 value);
    SceneResult set_local_rotation(Entity entity, math::Quat value);
    SceneResult set_local_scale(Entity entity, math::Vec3 value);
    SceneResult translate(Entity entity, math::Vec3 offset,
                          TransformSpace space = TransformSpace::Parent);
    SceneResult rotate(Entity entity, math::Quat rotation,
                       TransformSpace space = TransformSpace::Local);
    SceneResult scale(Entity entity, math::Vec3 factor);
    SceneResult look_at(Entity entity, math::Vec3 target,
                        math::Vec3 up = {0.0, 1.0, 0.0});
    // Both batches validate the entire selection before publishing any local edit.
    // World edits account for other edited ancestors in the same batch.
    SceneResult apply_local_transforms(std::span<const LocalTransformEdit> edits);
    SceneResult apply_world_matrices(std::span<const WorldMatrixEdit> edits);
    SceneResult set_enabled_recursive(Entity entity, bool enabled);
    SceneResult mark_dirty(Entity entity);
    void mark_all_dirty() noexcept;

    [[nodiscard]] std::optional<math::Transform> local_transform(Entity entity) const;
    [[nodiscard]] std::optional<math::Transform> world_transform(Entity entity);
    [[nodiscard]] std::optional<math::Mat4> world_matrix(Entity entity);
    [[nodiscard]] bool world_transform_exact(Entity entity);
    [[nodiscard]] bool effective_enabled(Entity entity);
    // Cached access does not synchronize. Dirty/invalid entries return nullptr.
    [[nodiscard]] const math::Mat4* cached_world_matrix(Entity entity) const noexcept;
    [[nodiscard]] std::optional<math::Vec3> local_to_world(Entity entity, math::Vec3 point);
    [[nodiscard]] std::optional<math::Vec3> world_to_local(Entity entity, math::Vec3 point);
    [[nodiscard]] std::optional<math::Vec3> transform_direction(Entity entity, math::Vec3 direction);

    SceneResult set_local_bounds(Entity entity, const math::Aabb& bounds);
    SceneResult clear_bounds(Entity entity);
    [[nodiscard]] std::optional<math::Aabb> world_bounds(Entity entity);
    [[nodiscard]] std::optional<math::Aabb> subtree_bounds(Entity entity);
    [[nodiscard]] std::optional<math::Aabb> aggregate_bounds();
    [[nodiscard]] std::vector<Entity> query_bounds(const math::Aabb& bounds,
                                                  bool include_disabled = false,
                                                  std::size_t limit = 65536);
    [[nodiscard]] std::vector<Entity> query_frustum(const math::Frustum& frustum,
                                                   bool include_disabled = false,
                                                   std::size_t limit = 65536);
    [[nodiscard]] std::vector<BoundsHit> raycast_bounds(const math::Ray& ray,
                                                      double maximum = math::infinity,
                                                      bool include_disabled = false,
                                                      std::size_t limit = 65536);

    // Registry copies arbitrary components by value. References inside those
    // components are not rewritten; entities supplies the remap for their owner.
    [[nodiscard]] CloneResult clone_subtree(Entity source, Entity parent = {},
                                            ReparentMode mode = ReparentMode::KeepLocal,
                                            std::string_view root_name = {});
    // Snapshots include scene transforms/bounds plus registry names, identities,
    // enablement, layers and tags. Arbitrary component payloads are not serialized.
    [[nodiscard]] std::optional<SceneSnapshot> snapshot(Entity subtree = {});
    [[nodiscard]] CloneResult import_snapshot(const SceneSnapshot& snapshot,
                                              Entity parent = {},
                                              bool preserve_persistent = false);

    // External registry destruction/component removal promotes surviving children
    // to roots with their local transforms unchanged. Scene destruction policies
    // should be used when preservation of world transforms is required.
    [[nodiscard]] SceneUpdate update();

private:
    struct Node {
        Entity parent{};
        std::vector<Entity> children;
        math::Transform observed_local{};
        math::Mat4 world{};
        std::optional<math::Aabb> own_bounds;
        std::optional<math::Aabb> subtree_bounds;
        bool dirty{true};
        bool world_exact{true};
        bool world_valid{};
        bool effective_enabled{true};
        std::uint64_t revision{};
    };

    [[nodiscard]] Node* node(Entity entity) noexcept;
    [[nodiscard]] const Node* node(Entity entity) const noexcept;
    [[nodiscard]] std::vector<Entity>& siblings(Entity parent);
    [[nodiscard]] const std::vector<Entity>& siblings(Entity parent) const;
    [[nodiscard]] SceneError editable(Entity entity) const noexcept;
    [[nodiscard]] SceneError parent_error(Entity parent) const noexcept;
    [[nodiscard]] std::size_t subtree_height(Entity entity) const;
    [[nodiscard]] SceneError local_from_world(Entity parent, const math::Mat4& world,
                                             math::Transform& output) const noexcept;
    void dirty_subtree(Entity entity);
    void changed(Node* changed_node = nullptr) noexcept;
    void erase_node(Entity entity) noexcept;
    void rollback_entities(std::span<const Entity> entities) noexcept;
    std::size_t prune_external_changes();

    ecs::Registry& registry_;
    SceneLimits limits_;
    std::unordered_map<Entity, Node, ecs::EntityHash> nodes_;
    std::vector<Entity> roots_;
    std::uint64_t revision_{};
};

void register_scene(core::Kernel& kernel);

} // namespace ge::scene
