#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }
namespace ge::scene { class Scene; class WorldPartition; }
namespace ge::spatial { class SpatialIndex; }
namespace ge::physics { class CollisionWorld; }

namespace ge::ai {

class BehaviorService;

struct SensorHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return index != 0 && generation != 0;
    }
    auto operator<=>(const SensorHandle&) const = default;
};

struct PerceptionSensor {
    SensorHandle handle{};
    ecs::Entity owner{};
};

struct PerceptionTarget {
    math::Vec3 aim_offset{};
    std::uint32_t faction{};
    double visibility{1.0};
    double threat{0.5};
    double salience{0.5};
    bool enabled{true};
};

enum class SightState : std::uint8_t {
    Unknown, Clear, Blocked, OutsideRange, OutsideField
};

enum class OcclusionMode : std::uint8_t {
    ColliderPrimitives, ConservativeBounds
};

enum class StimulusKind : std::uint8_t { Sound, Damage, Alert };
enum class UtilityAction : std::uint8_t { None, Observe, Investigate, Engage, Flee };
enum class MemoryChangeKind : std::uint8_t {
    Acquired, Updated, Forgotten, DecisionChanged, SensorRemoved
};

struct SensorConfig {
    math::Vec3 eye_offset{0.0, 1.6, 0.0};
    math::Vec3 forward_local{0.0, 0.0, -1.0};
    double sight_range{50.0};
    double hearing_range{40.0};
    double field_of_view_radians{2.0943951023931953};
    double interval_seconds{0.1};
    double acquisition_rate{4.0};
    double confidence_half_life{4.0};
    double threat_half_life{8.0};
    double memory_seconds{20.0};
    double minimum_confidence{0.03};
    double hearing_threshold{0.05};
    double switch_margin{0.08};
    double minimum_hold_seconds{0.3};
    double idle_score{0.05};
    double maximum_tracked_speed{100.0};
    std::uint64_t target_layers{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t occluder_layers{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t hostile_factions{std::numeric_limits<std::uint64_t>::max()};
    std::uint32_t faction{};
    std::uint32_t sight_samples{3};
    OcclusionMode occlusion{OcclusionMode::ColliderPrimitives};
    bool enabled{true};
};

// All positions in retained records are absolute coordinates. The service never
// treats a raw blackboard Vec3 as a world position; it publishes AbsolutePosition.
struct Stimulus {
    StimulusKind kind{StimulusKind::Sound};
    ecs::Entity source{};
    ecs::Entity recipient{};
    math::Vec3 absolute_position{};
    double strength{1.0};
    double radius{30.0};
    double lifetime_seconds{2.0};
    double threat{0.25};
    std::uint32_t faction{};
};

struct StimulusRecord {
    std::uint64_t sequence{};
    double admitted_seconds{};
    Stimulus stimulus;
};

struct PerceptionMemory {
    ecs::Entity target{};
    std::uint64_t anonymous_stimulus{};
    math::Vec3 absolute_position{};
    math::Vec3 velocity{};
    double first_observed_seconds{};
    double last_observed_seconds{};
    double last_seen_seconds{-1.0};
    double last_heard_seconds{-1.0};
    double confidence{};
    double threat{};
    double salience{};
    std::uint32_t faction{};
    std::uint64_t observations{};
    std::uint64_t sight_tick{};
    SightState sight{SightState::Unknown};
    bool hostile{};
    bool heard{};
    bool damaged{};
};

// Scores are explicit weighted considerations, gated by confidence, age,
// hostility and current sight. They choose intent; they do not move entities.
struct UtilityRule {
    UtilityAction action{UtilityAction::Observe};
    double bias{};
    double confidence_weight{1.0};
    double proximity_weight{1.0};
    double threat_weight{};
    double freshness_weight{1.0};
    double salience_weight{1.0};
    double minimum_confidence{0.1};
    double maximum_age_seconds{20.0};
    double preferred_distance{};
    double distance_width{50.0};
    bool require_visible{};
    bool require_hostile{};
    bool invert_threat{};
    bool enabled{true};
};

struct PerceptionDecision {
    UtilityAction action{UtilityAction::None};
    ecs::Entity target{};
    std::uint64_t anonymous_stimulus{};
    math::Vec3 absolute_position{};
    double score{};
    double confidence{};
    double threat{};
    double selected_seconds{};
    std::uint64_t tick{};
    std::uint64_t sight_tick{};
    bool has_position{};
    // Cached only until the next scheduled sample; sight_tick identifies its
    // evidence frame. An incomplete due sample clears this flag.
    bool visible{};
    bool deferred{};
};

struct PerceptionChange {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    SensorHandle sensor{};
    ecs::Entity owner{};
    ecs::Entity target{};
    std::uint64_t anonymous_stimulus{};
    MemoryChangeKind kind{MemoryChangeKind::Updated};
    double confidence{};
    UtilityAction action{UtilityAction::None};
};

template<class T>
struct PerceptionHistory {
    std::vector<T> records;
    std::uint64_t next_cursor{};
    std::uint64_t latest_cursor{};
    bool history_lost{};
    bool truncated{};
};

struct PerceptionLimits {
    std::uint32_t max_sensors{256};
    std::size_t memories_per_sensor{64};
    std::size_t candidates_per_sensor{128};
    std::size_t colliders_per_query{128};
    std::size_t rules_per_sensor{8};
    std::size_t stimulus_history{2048};
    std::size_t change_history{2048};
    std::size_t sensors_per_frame{64};
    std::size_t candidate_visits_per_frame{4096};
    std::size_t line_queries_per_frame{256};
    std::size_t shape_tests_per_frame{8192};
    std::size_t stimulus_visits_per_frame{4096};
    std::size_t utility_evaluations_per_frame{32768};
    std::size_t spatial_visits_per_query{16384};
};

struct PerceptionStatistics {
    std::size_t sensors{};
    std::size_t memories{};
    std::size_t sensed{};
    std::size_t candidates{};
    std::size_t line_queries{};
    std::size_t shape_tests{};
    std::size_t stimulus_visits{};
    std::size_t utility_evaluations{};
    std::size_t deferred_sensors{};
    std::size_t blackboard_failures{};
    std::uint64_t admitted_stimuli{};
    std::uint64_t rejected_stimuli{};
    std::uint64_t lost_sensor_histories{};
    std::uint64_t tick{};
    bool coordinates_consistent{true};
    bool collision_complete{};
    bool spatial_complete{};
};

struct SensorSnapshot {
    SensorHandle handle{};
    ecs::Entity owner{};
    SensorConfig config;
    std::vector<PerceptionMemory> memories;
    std::vector<UtilityRule> rules;
    PerceptionDecision decision;
    std::uint64_t stimulus_cursor{};
    double last_sensed_seconds{};
};

[[nodiscard]] bool valid_sensor_config(const SensorConfig& config) noexcept;
[[nodiscard]] bool valid_utility_rule(const UtilityRule& rule) noexcept;
[[nodiscard]] std::array<UtilityRule, 4> default_utility_rules() noexcept;
[[nodiscard]] std::string_view utility_action_name(UtilityAction action) noexcept;
[[nodiscard]] std::string_view sight_state_name(SightState state) noexcept;

// Single writer borrowing Registry and Scene. Destruction touches neither.
// Discovery requires a target's BoundsComponent to appear in SpatialIndex.
// Collider LOS covers only active registered primitive colliders. Conservative
// bounds LOS covers indexed AABBs and can reject a geometrically clear path.
// Missing services, stale snapshots and truncated queries never establish clear
// LOS. Collision synchronization invalidates previously borrowed contact spans.
class PerceptionService final {
public:
    PerceptionService(ecs::Registry& registry, scene::Scene& scene,
                      PerceptionLimits limits = {});
    ~PerceptionService();
    PerceptionService(const PerceptionService&) = delete;
    PerceptionService& operator=(const PerceptionService&) = delete;
    [[nodiscard]] const PerceptionLimits& limits() const noexcept;
    [[nodiscard]] SensorHandle attach(ecs::Entity owner, SensorConfig config = {});
    bool detach(SensorHandle sensor);
    [[nodiscard]] SensorHandle sensor(ecs::Entity owner) const noexcept;
    bool configure(SensorHandle sensor, const SensorConfig& config);
    bool set_rules(SensorHandle sensor, std::span<const UtilityRule> rules);
    bool set_target(ecs::Entity entity, const PerceptionTarget& target);
    bool remove_target(ecs::Entity entity);
    bool forget(SensorHandle sensor, ecs::Entity target = {});
    [[nodiscard]] std::optional<SensorSnapshot> inspect(SensorHandle sensor) const;
    [[nodiscard]] std::optional<PerceptionDecision> decision(SensorHandle sensor) const;
    [[nodiscard]] std::uint64_t emit(const Stimulus& stimulus);
    [[nodiscard]] PerceptionHistory<StimulusRecord> stimuli_since(
        std::uint64_t cursor, std::size_t limit = 128) const;
    [[nodiscard]] PerceptionHistory<PerceptionChange> changes_since(
        std::uint64_t cursor, std::size_t limit = 128) const;
    [[nodiscard]] const PerceptionStatistics& statistics() const noexcept;
    void update(const core::Frame& frame, spatial::SpatialIndex* index,
                physics::CollisionWorld* collision, scene::WorldPartition* partition,
                BehaviorService* behavior);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Visibility after spatial.sync/camera.update, before gameplay.interaction.update.
// PreSimulation behavior consumes the preceding frame's decisions; absolute
// positions remain valid across rebasing. Hearing is distance attenuation only.
void register_perception(core::Kernel& kernel);

} // namespace ge::ai
