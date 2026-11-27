#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::spatial {

struct ProxyHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const ProxyHandle&) const = default;
};

struct TreeConfig {
    std::uint32_t capacity{65536};
    double fat_margin{0.1};
    double displacement_multiplier{2.0};
    std::size_t traversal_limit{4194304};
};

struct QueryFilter {
    std::uint64_t layers{std::numeric_limits<std::uint64_t>::max()};
    ecs::Entity exclude{};
    std::size_t max_visits{4194304};
    bool fat_bounds{};
};

struct QueryReport {
    std::size_t written{};
    std::size_t matched{};
    std::size_t visited{};
    bool truncated{};
    bool traversal_exhausted{};
    bool invalid_input{};
    bool stale{};
};

struct ProxyInfo {
    ProxyHandle proxy{};
    ecs::Entity entity{};
    math::Aabb bounds{};
    math::Aabb fat_bounds{};
    std::uint64_t layers{};
};

struct QueryHit {
    ProxyHandle proxy{};
    ecs::Entity entity{};
    math::Aabb bounds{};
};

struct RayHit {
    ProxyHandle proxy{};
    ecs::Entity entity{};
    math::RayHit hit{};
};

struct OverlapPair {
    ProxyHandle first{};
    ProxyHandle second{};
    ecs::Entity first_entity{};
    ecs::Entity second_entity{};
};

enum class ProxyUpdate : std::uint8_t {
    InvalidHandle,
    InvalidBounds,
    Unchanged,
    Refitted,
    Reinserted
};

struct TreeStatistics {
    std::size_t proxies{};
    std::size_t nodes{};
    std::size_t capacity{};
    std::size_t free_nodes{};
    std::size_t retired_nodes{};
    std::size_t memory_bytes{};
    std::int32_t height{};
    std::int32_t maximum_balance{};
    double surface_area_ratio{};
    std::uint64_t insertions{};
    std::uint64_t removals{};
    std::uint64_t reinsertions{};
    std::uint64_t rotations{};
    std::uint64_t rebuilds{};
};

// Single-writer tree. Proxy handles are scoped to this tree lifetime. Duplicate
// entity payloads are supported for compound colliders; all queries retain IDs.
// Query storage belongs to the caller. matched counts matches actually visited;
// truncated also covers traversal exhaustion. Ray outputs retain the nearest
// visited hits in ascending ray-parameter order. A ray starting inside a box
// reports the minimum parameter with a zero normal. Other order is unspecified.
class DynamicAabbTree final {
public:
    static constexpr std::uint32_t maximum_capacity = 1048576;
    explicit DynamicAabbTree(TreeConfig configuration = {});
    DynamicAabbTree(const DynamicAabbTree&) = delete;
    DynamicAabbTree& operator=(const DynamicAabbTree&) = delete;
    DynamicAabbTree(DynamicAabbTree&&) = delete;
    DynamicAabbTree& operator=(DynamicAabbTree&&) = delete;

    [[nodiscard]] ProxyHandle insert(ecs::Entity entity, const math::Aabb& bounds,
                                     std::uint64_t layers = ~std::uint64_t{},
                                     math::Vec3 displacement = {});
    bool remove(ProxyHandle proxy) noexcept;
    ProxyUpdate update(ProxyHandle proxy, const math::Aabb& bounds,
                       math::Vec3 displacement = {}, bool force_reinsert = false) noexcept;
    bool set_layers(ProxyHandle proxy, std::uint64_t layers) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool contains(ProxyHandle proxy) const noexcept;
    [[nodiscard]] std::optional<ProxyInfo> get(ProxyHandle proxy) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const TreeConfig& config() const noexcept;
    [[nodiscard]] std::optional<math::Aabb> bounds() const noexcept;
    [[nodiscard]] TreeStatistics statistics() const noexcept;

    [[nodiscard]] QueryReport query(const math::Aabb& bounds,
                                    std::span<QueryHit> output,
                                    QueryFilter filter = {}) const;
    [[nodiscard]] QueryReport query_frustum(const math::Frustum& frustum,
                                            std::span<QueryHit> output,
                                            QueryFilter filter = {}) const;
    [[nodiscard]] QueryReport raycast(const math::Ray& ray, double maximum,
                                      std::span<RayHit> output,
                                      QueryFilter filter = {}, double minimum = 0.0) const;
    // Each unordered proxy pair occurs once. Both proxies must pass the filter.
    // fat_bounds selects conservative fat overlap; false tests tight AABBs.
    [[nodiscard]] QueryReport collect_pairs(std::span<OverlapPair> output,
                                            QueryFilter filter = {},
                                            bool include_same_entity = true) const;
    // Allocations are bounded by node capacity and happen before restructuring.
    // Both operations preserve every live proxy handle and entity generation.
    void refit(bool tighten = false);
    void rebuild(bool tighten = true);

private:
    using Index = std::uint32_t;
    static constexpr Index none = std::numeric_limits<Index>::max();
    struct Node {
        math::Aabb box{};
        math::Aabb tight{};
        ecs::Entity entity{};
        std::uint64_t layers{};
        std::uint64_t generation{1};
        Index parent{none};
        Index left{none};
        Index right{none};
        Index next_free{none};
        std::int32_t height{-1};
    };
    [[nodiscard]] Index allocate_node() noexcept;
    void release_node(Index index) noexcept;
    [[nodiscard]] ProxyHandle handle(Index index) const noexcept;
    [[nodiscard]] bool fatten(const math::Aabb& bounds, math::Vec3 displacement,
                              math::Aabb& output) const noexcept;
    void insert_leaf(Index leaf, Index branch) noexcept;
    void detach_leaf(Index leaf, bool release_branch) noexcept;
    void refit_node(Index index) noexcept;
    void refit_upwards(Index index) noexcept;
    [[nodiscard]] Index rotate(Index index) noexcept;
    [[nodiscard]] Index next_after(Index index) const noexcept;
    [[nodiscard]] bool accepts(const Node& node, const QueryFilter& filter) const noexcept;
    [[nodiscard]] Index build_subtree(std::span<Index> leaves, std::span<Index> branches,
                                      std::size_t& branch_cursor, Index parent) noexcept;
    template<class Predicate>
    [[nodiscard]] QueryReport query_impl(Predicate&& predicate, std::span<QueryHit> output,
                                         const QueryFilter& filter) const;

    TreeConfig config_;
    std::vector<Node> nodes_;
    Index root_{none};
    Index free_head_{none};
    std::size_t live_nodes_{};
    std::size_t proxies_{};
    std::size_t retired_nodes_{};
    std::uint64_t insertions_{};
    std::uint64_t removals_{};
    std::uint64_t reinsertions_{};
    std::uint64_t rotations_{};
    std::uint64_t rebuilds_{};
};

struct IndexConfig {
    TreeConfig tree{};
    std::size_t change_limit{8192};
    bool include_disabled{};
};

struct SyncReport {
    std::size_t processed{};
    std::size_t added{};
    std::size_t updated{};
    std::size_t removed{};
    std::size_t unchanged{};
    std::size_t rejected{};
    std::size_t capacity_failures{};
    std::uint64_t registry_revision{};
    std::uint64_t scene_revision{};
    bool full_scan{};
    bool history_lost{};
    bool scene_busy{};
};

// Borrows the registry and scene for its lifetime. Indexes BoundsComponent.local
// through exact scene matrices, including affine shear. Queries synchronize first;
// tree() offers an explicit cached view for a consumer that already called sync().
// proxy() and world_bounds() also inspect the last synchronized snapshot.
class SpatialIndex final {
public:
    SpatialIndex(ecs::Registry& registry, scene::Scene& scene, IndexConfig configuration = {});
    [[nodiscard]] SyncReport sync(bool force_full_scan = false);
    // Last successful synchronization; a busy call preserves its revision cursor.
    [[nodiscard]] const SyncReport& last_sync() const noexcept;
    [[nodiscard]] const DynamicAabbTree& tree() const noexcept;
    [[nodiscard]] std::optional<ProxyHandle> proxy(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::optional<math::Aabb> world_bounds(ecs::Entity entity) const noexcept;
    [[nodiscard]] QueryReport query(const math::Aabb& bounds, std::span<QueryHit> output,
                                    QueryFilter filter = {});
    [[nodiscard]] QueryReport query_frustum(const math::Frustum& frustum,
                                            std::span<QueryHit> output, QueryFilter filter = {});
    [[nodiscard]] QueryReport raycast(const math::Ray& ray, double maximum,
                                      std::span<RayHit> output, QueryFilter filter = {},
                                      double minimum = 0.0);
    [[nodiscard]] QueryReport collect_pairs(std::span<OverlapPair> output,
                                            QueryFilter filter = {});
    void rebuild(bool tighten = true);
    void clear() noexcept;

private:
    struct Entry {
        ProxyHandle proxy{};
        std::uint64_t entity_revision{};
    };
    [[nodiscard]] bool eligible(ecs::Entity entity) const noexcept;
    void synchronize_entity(ecs::Entity entity, SyncReport& report);
    void erase(ecs::Entity entity, SyncReport& report) noexcept;

    ecs::Registry& registry_;
    scene::Scene& scene_;
    IndexConfig config_;
    DynamicAabbTree tree_;
    std::unordered_map<ecs::Entity, Entry, ecs::EntityHash> entries_;
    SyncReport last_{};
    bool initialized_{};
    bool retry_full_scan_{};
};

void register_index(core::Kernel& kernel);

} // namespace ge::spatial
