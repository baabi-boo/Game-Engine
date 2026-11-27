#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>
#include <ge/spatial/index.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::physics {

using CollisionShape = std::variant<math::Sphere, math::Obb, math::Capsule,
                                    math::Plane, math::Triangle>;

enum class ShapeKind : std::uint8_t { Sphere, Box, Capsule, Plane, Triangle };

struct ColliderHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const ColliderHandle&) const = default;
};

struct CollisionFilter {
    std::uint64_t layers{1};
    std::uint64_t mask{std::numeric_limits<std::uint64_t>::max()};
    // Equal positive groups force collision; equal negative groups suppress it.
    // Group rules override masks, but never entity ownership or enabled state.
    std::int32_t group{};
};

struct CollisionMaterial {
    double friction{0.5};
    double restitution{};
};

struct ColliderDescription {
    CollisionShape shape{math::Sphere{{}, 0.5}};
    math::Transform local_transform{};
    CollisionFilter filter{};
    CollisionMaterial material{};
    bool trigger{};
    bool enabled{true};
};

struct ColliderComponent {
    // Handles belong to the CollisionWorld, and may share an entity for compounds.
    // Copying this component does not clone shapes or transfer their ownership.
    std::vector<ColliderHandle> colliders;
};

struct ContactPoint {
    math::Vec3 point_a{};
    math::Vec3 point_b{};
    double penetration{};
    std::uint64_t feature{};
    [[nodiscard]] math::Vec3 position() const noexcept {
        return (point_a + point_b) * 0.5;
    }
};

struct ContactManifold {
    ColliderHandle first{};
    ColliderHandle second{};
    ecs::Entity first_entity{};
    ecs::Entity second_entity{};
    // Unit normal from A toward B: separating A moves it along -normal.
    math::Vec3 normal{};
    std::array<ContactPoint, 4> points{};
    std::size_t point_count{};
    bool trigger{};
    double friction{};
    double restitution{};
    [[nodiscard]] bool touching() const noexcept { return point_count != 0; }
};

struct SegmentClosestPoints {
    math::Vec3 first{};
    math::Vec3 second{};
    double first_parameter{};
    double second_parameter{};
};

[[nodiscard]] ShapeKind shape_kind(const CollisionShape& shape) noexcept;
// Finite positive dimensions and coordinates bounded to 1e12 metres. Degenerate
// triangles, zero plane normals, and zero quaternion orientations are rejected.
[[nodiscard]] bool valid_shape(const CollisionShape& shape) noexcept;
[[nodiscard]] bool should_collide(CollisionFilter first, CollisionFilter second) noexcept;
[[nodiscard]] SegmentClosestPoints closest_segments(math::Segment first,
                                                     math::Segment second) noexcept;
// Planes have no finite AABB. The plane convention is dot(normal, p) + distance = 0.
// Planes enclose their negative half-space and push finite shapes toward +normal.
[[nodiscard]] std::optional<math::Aabb> shape_bounds(const CollisionShape& shape) noexcept;
// Round shapes use the maximum absolute TRS scale as their world radius scale.
// Box-local orientations that would introduce shear are rejected, as are singular
// transforms. Scene synchronization additionally requires exact world TRS.
[[nodiscard]] std::optional<CollisionShape> transform_shape(
    const CollisionShape& shape, const math::Transform& transform) noexcept;
// Supports every sphere/box/capsule/plane pairing except plane/plane; triangles
// currently support sphere contacts only. Unsupported pairs return no contacts.
// Capsule/box centerline intersections use conservative face penetration rather
// than claiming the minimum translation among every possible edge direction.
[[nodiscard]] ContactManifold narrowphase(const CollisionShape& first_world,
                                          const CollisionShape& second_world,
                                          double tolerance = 1.0e-8) noexcept;

struct Collider {
    ColliderHandle handle{};
    ecs::Entity entity{};
    ColliderDescription description{};
    math::Transform transform{};
    CollisionShape world_shape{math::Sphere{{}, 0.5}};
    std::optional<math::Aabb> bounds;
    spatial::ProxyHandle proxy{};
    bool active{};
};

struct CollisionConfig {
    std::uint32_t capacity{16384};
    std::size_t max_pairs{65536};
    std::size_t max_manifolds{16384};
    std::size_t max_planes{128};
    std::size_t traversal_limit{4194304};
    double contact_tolerance{1.0e-8};
    double fat_margin{0.05};
    bool collide_same_entity{};
};

struct CollisionSyncResult {
    std::size_t updated{};
    std::size_t removed{};
    std::size_t disabled{};
    std::size_t rejected{};
    bool scene_busy{};
};

struct CollisionStatistics {
    std::size_t colliders{};
    std::size_t active{};
    std::size_t proxies{};
    std::size_t planes{};
    std::size_t candidate_pairs{};
    std::size_t filtered_pairs{};
    std::size_t narrowphase_pairs{};
    std::size_t manifolds{};
    std::size_t contact_points{};
    bool pairs_truncated{};
    bool contacts_truncated{};
    bool traversal_exhausted{};
};

struct CollisionQueryResult {
    std::size_t written{};
    std::size_t matched{};
    bool truncated{};
    bool invalid_input{};
};

// Single-writer service borrowing the sole entity registry. No entity is created
// by add(). All mutations invalidate contacts(), whose contents are cached until
// detect(). Handles remain generation-checked throughout remove/reuse/clear.
class CollisionWorld final {
public:
    explicit CollisionWorld(ecs::Registry& shared_registry, CollisionConfig configuration = {});
    CollisionWorld(const CollisionWorld&) = delete;
    CollisionWorld& operator=(const CollisionWorld&) = delete;
    CollisionWorld(CollisionWorld&&) = delete;
    CollisionWorld& operator=(CollisionWorld&&) = delete;
    [[nodiscard]] ecs::Registry& registry() noexcept;
    [[nodiscard]] const CollisionConfig& config() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    // New colliders are inactive until sync() or an explicit set_transform().
    [[nodiscard]] ColliderHandle add(ecs::Entity entity,
                                      const ColliderDescription& description = {});
    bool remove(ColliderHandle handle);
    std::size_t remove(ecs::Entity entity);
    void clear();
    [[nodiscard]] const Collider* get(ColliderHandle handle) const noexcept;
    [[nodiscard]] std::vector<ColliderHandle> colliders(ecs::Entity entity) const;
    bool set_shape(ColliderHandle handle, const CollisionShape& shape);
    bool set_filter(ColliderHandle handle, CollisionFilter filter);
    bool set_material(ColliderHandle handle, CollisionMaterial material);
    bool set_enabled(ColliderHandle handle, bool enabled);
    bool set_trigger(ColliderHandle handle, bool trigger);
    // Sets the entity world transform for this collider, preserving its local
    // offset. It updates collision only; a dynamics owner also writes Scene.
    bool set_transform(ColliderHandle handle, const math::Transform& world_transform);
    // Prunes dead/unlinked colliders, synchronizes exact Scene transforms, and
    // excludes disabled, missing, invalid, or sheared Scene nodes from detection.
    [[nodiscard]] CollisionSyncResult sync(scene::Scene& scene);
    [[nodiscard]] CollisionStatistics detect();
    [[nodiscard]] std::span<const ContactManifold> contacts() const noexcept;
    [[nodiscard]] const CollisionStatistics& statistics() const noexcept;
    // Conservative AABB candidates; plane half-spaces are included separately.
    // Call narrowphase() when an exact supported primitive overlap is required.
    [[nodiscard]] CollisionQueryResult query(const math::Aabb& bounds,
                                             std::span<ColliderHandle> output,
                                             CollisionFilter filter = {},
                                             ecs::Entity exclude = {}) const;

private:
    [[nodiscard]] Collider* mutable_collider(ColliderHandle handle) noexcept;
    [[nodiscard]] ColliderHandle from_proxy(spatial::ProxyHandle proxy) const noexcept;
    bool refresh(Collider& collider, const ColliderDescription& description,
                 const math::Transform& world_transform, bool enabled);
    void deactivate(Collider& collider) noexcept;
    void invalidate_contacts() noexcept;
    void evaluate_pair(ColliderHandle first, ColliderHandle second);
    ecs::Registry& registry_;
    CollisionConfig config_;
    ge::GenerationPool handles_;
    spatial::DynamicAabbTree tree_;
    std::vector<std::optional<Collider>> slots_;
    std::map<spatial::ProxyHandle, ColliderHandle> proxy_owners_;
    std::vector<spatial::OverlapPair> pairs_;
    std::vector<ContactManifold> contacts_;
    CollisionStatistics statistics_{};
};

void register_collision(core::Kernel& kernel);

} // namespace ge::physics
