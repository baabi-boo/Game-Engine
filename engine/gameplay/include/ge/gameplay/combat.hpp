#pragma once

#include <ge/gameplay/inventory.hpp>
#include <ge/physics/collision.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }
namespace ge::scene { class Scene; }

namespace ge::gameplay {

enum class DamageType : std::uint8_t { Physical, Fire, Cold, Shock, Poison, Pure, Count };
inline constexpr std::size_t damage_type_count = static_cast<std::size_t>(DamageType::Count);
enum class CombatError : std::uint8_t {
    None, InvalidArgument, InvalidActor, NotAttached, AlreadyAttached, Disabled,
    Defeated, FriendlyFire, Immune, Cooldown, Stunned, Capacity, IncompleteQuery,
    MissingService, InventoryRejected, Exhausted, AllocationFailure, Busy
};
enum class CombatEventKind : std::uint8_t {
    Attached, Detached, Damaged, Missed, Defeated, Healed, Revived,
    StatusApplied, StatusRemoved, StatusExpired, AttackFired
};
enum class StatusKind : std::uint8_t {
    DamageOverTime, HealOverTime, ShieldOverTime, Vulnerability, ArmorBreak, Stun
};
enum class StatusStacking : std::uint8_t { Refresh, Replace, Stack };
enum class AttackKind : std::uint8_t { Ray, MeleeSphere };
enum class HealPool : std::uint8_t { Health, Shield };

[[nodiscard]] std::string_view describe(CombatError value) noexcept;
[[nodiscard]] std::string_view damage_type_name(DamageType value) noexcept;

struct CombatantConfig {
    double maximum_health{100.0};
    double maximum_shield{};
    double armor{};
    std::array<double, damage_type_count> resistance{};
    std::uint64_t immune_damage{};
    // Faction zero is neutral. Equal nonzero factions are allies.
    std::uint32_t faction{};
    bool friendly_fire{};
    bool self_damage{};
    bool invulnerable{};
    std::string defeat_key{"defeat"};
};

struct CombatantComponent {
    CombatantConfig configuration;
    double health{100.0};
    double shield{};
    std::uint64_t life{1};
    bool defeated{};
};

struct DamageProfile {
    DamageType type{DamageType::Physical};
    double amount{10.0};
    double hit_chance{1.0};
    double critical_chance{};
    double critical_multiplier{1.5};
    double armor_penetration{};
    bool bypass_shield{};
    bool nonlethal{};
};

struct DamageRequest {
    // Source is the credited actor, never inferred from a collider or parent.
    // Empty source means environmental damage and carries no mission credit.
    ecs::Entity source{};
    ecs::Entity target{};
    DamageProfile damage;
    std::uint64_t seed{};
    math::Vec3 point{};
    math::Vec3 impulse{};
};

struct CombatResult {
    CombatError error{CombatError::None};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == CombatError::None; }
};

struct DamageResult {
    CombatError error{CombatError::None};
    ecs::Entity target{};
    std::uint64_t sequence{};
    double health_damage{};
    double shield_damage{};
    bool hit{};
    bool critical{};
    bool defeated{};
    bool impulse_applied{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == CombatError::None; }
};

struct HealRequest {
    ecs::Entity source{};
    ecs::Entity target{};
    double amount{10.0};
    HealPool pool{HealPool::Health};
};

struct StatusSpec {
    std::uint64_t id{};
    StatusKind kind{StatusKind::DamageOverTime};
    StatusStacking stacking{StatusStacking::Refresh};
    DamageType damage_type{DamageType::Poison};
    double magnitude{1.0};
    double duration_seconds{5.0};
    double interval_seconds{1.0};
    std::uint32_t maximum_stacks{4};
};

struct ActiveStatus {
    StatusSpec specification;
    ecs::Entity source{};
    double remaining_seconds{};
    double until_tick_seconds{};
    std::uint32_t stacks{1};
    std::uint64_t tick_sequence{};
};

struct CombatCooldown {
    std::uint64_t attack{};
    double remaining_seconds{};
};

struct CombatSnapshot {
    ecs::Entity entity{};
    CombatantComponent combatant;
    std::vector<ActiveStatus> statuses;
    std::vector<CombatCooldown> cooldowns;
    bool can_act{};
};

struct CombatEvent {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    CombatEventKind kind{CombatEventKind::Damaged};
    ecs::Entity source{};
    ecs::Entity target{};
    PersistentId persistent_target{};
    DamageType damage_type{DamageType::Physical};
    double health_change{};
    double shield_change{};
    std::uint64_t status{};
    std::uint64_t life{};
    bool critical{};
};

struct CombatHistory {
    std::vector<CombatEvent> events;
    std::uint64_t next_cursor{};
    std::uint64_t newest_sequence{};
    bool history_lost{};
    bool truncated{};
};

struct CombatHit {
    ecs::Entity entity{};
    physics::ColliderHandle collider{};
    math::Vec3 point{};
    math::Vec3 normal{};
    double distance{};
};

struct CombatQuery {
    CombatError error{CombatError::None};
    std::vector<CombatHit> hits;
    std::size_t candidates{};
    bool complete{};
};

struct CombatCost {
    InventoryContainerId container{};
    PersistentId item{};
    std::uint32_t quantity{1};
};

struct AttackRequest {
    ecs::Entity source{};
    std::uint64_t attack{1};
    std::uint64_t seed{};
    AttackKind kind{AttackKind::Ray};
    // Runtime coordinates. Melee uses ray.origin and ignores ray.direction.
    math::Ray ray{};
    double range{100.0};
    double radius{1.0};
    double cooldown_seconds{0.25};
    DamageProfile damage;
    physics::CollisionFilter filter{};
    std::array<CombatCost, 4> costs{};
    std::size_t cost_count{};
    std::size_t maximum_targets{1};
    bool include_triggers{};
};

struct AttackResult {
    CombatError error{CombatError::None};
    InventoryError inventory_error{InventoryError::None};
    std::uint64_t sequence{};
    std::vector<DamageResult> targets;
    bool fired{};
    bool costs_committed{};
};

struct CombatMissionCursor {
    // Producer zero disables automatic delivery, with explicit lost-credit counts.
    std::uint64_t producer{};
    std::uint64_t next_sequence{1};
};

struct CombatLimits {
    std::size_t actors{4096};
    std::size_t statuses_per_actor{16};
    std::size_t cooldowns_per_actor{16};
    std::size_t history{4096};
    std::size_t query_candidates{512};
    std::size_t targets_per_attack{32};
    std::size_t periodic_ticks_per_frame{1024};
    std::size_t mission_outbox{1024};
    std::size_t mission_attempts_per_frame{64};
    double maximum_delta_seconds{0.25};
};

struct CombatStatistics {
    std::size_t actors{};
    std::size_t pending_defeats{};
    std::uint64_t damage_resolutions{};
    std::uint64_t defeated{};
    std::uint64_t attacks{};
    std::uint64_t incomplete_queries{};
    std::uint64_t deferred_periodic_ticks{};
    std::uint64_t mission_failures{};
    std::uint64_t mission_disabled_credits{};
    std::uint64_t perception_failures{};
    std::uint64_t notification_failures{};
    double dropped_seconds{};
    bool mission_delivery_enabled{};
};

// Single kernel-thread writer. Registry owns actors and CombatantComponent;
// this service owns bounded status/cooldown state and independent event history.
// Native source handles are trusted attribution, not network authorization.
// Damage uses explicit seeds; health never revives implicitly. Pure damage skips
// armor/resistance but still obeys invulnerability, immunity, shield and factions.
// Ray queries use shared Sphere/Obb/Capsule/Plane/Triangle intersections. Melee
// uses sphere narrowphase against those same shapes. Queries cover registered,
// active primitive colliders only, never arbitrary meshes/terrain. Any sync
// rejection or truncated candidate set makes the whole query incomplete.
// Queries synchronize CollisionWorld and invalidate borrowed contact spans;
// calls during PhysicsWorld stepping are rejected. Collider entities are exact
// targets: ownership is never inferred from Scene ancestry or attached weapons.
// Ray attacks stop at the first primitive, including scenery and friendly actors.
// Melee is an overlap volume without line-of-sight or swept collision guarantees.
// Accepted fired misses/blocked shots spend costs and start cooldowns. Rejected
// queries/preflight spend nothing; costs commit once via InventoryService.
// Preserve mission_cursor with the mission save lineage, or configure a fresh
// producer per new combat lifetime. Combat snapshots/persistence are not provided.
// The retry outbox is memory only; drain it before saving/recreating the service.
class CombatService final {
public:
    CombatService(core::Kernel& kernel, ecs::Registry& registry, scene::Scene& scene,
                  CombatLimits limits = {}, CombatMissionCursor mission = {});
    ~CombatService();
    CombatService(const CombatService&) = delete;
    CombatService& operator=(const CombatService&) = delete;
    CombatResult attach(ecs::Entity entity, CombatantConfig config = {});
    CombatResult detach(ecs::Entity entity);
    CombatResult configure(ecs::Entity entity, const CombatantConfig& config);
    CombatResult revive(ecs::Entity entity, double health_fraction = 1.0);
    [[nodiscard]] std::optional<CombatSnapshot> inspect(ecs::Entity entity) const;
    [[nodiscard]] bool can_act(ecs::Entity entity) const noexcept;
    [[nodiscard]] double cooldown(ecs::Entity entity, std::uint64_t attack) const noexcept;
    DamageResult damage(const DamageRequest& request);
    CombatResult heal(const HealRequest& request);
    CombatResult apply_status(ecs::Entity target, ecs::Entity source, const StatusSpec& status);
    CombatResult remove_status(ecs::Entity target, std::uint64_t id);
    [[nodiscard]] CombatQuery raycast(const math::Ray& ray, double range,
        physics::CollisionFilter filter = {}, ecs::Entity exclude = {}, bool triggers = false);
    [[nodiscard]] CombatQuery melee(const math::Sphere& sphere,
        physics::CollisionFilter filter = {}, ecs::Entity exclude = {}, bool triggers = false);
    AttackResult attack(const AttackRequest& request);
    void update(const core::Frame& frame);
    [[nodiscard]] CombatHistory events_since(std::uint64_t cursor, std::size_t limit = 128) const;
    [[nodiscard]] CombatStatistics statistics() const noexcept;
    [[nodiscard]] CombatMissionCursor mission_cursor() const noexcept;
    // Explicit administrative action; rejected while any defeat awaits delivery.
    CombatResult set_mission_cursor(CombatMissionCursor cursor);
    [[nodiscard]] const CombatLimits& limits() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_combat(core::Kernel& kernel);

} // namespace ge::gameplay
