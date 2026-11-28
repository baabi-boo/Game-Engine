#include <ge/ai/perception.hpp>

#include <ge/ai/behavior.hpp>
#include <ge/core/runtime.hpp>
#include <ge/physics/collision.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>
#include <ge/spatial/index.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ge::ai {
namespace {

constexpr double coordinate_limit = 1.0e12;
constexpr double ray_margin = 1.0e-5;
constexpr double ln_two = 0.6931471805599453;

bool bounded_point(math::Vec3 point) noexcept {
    return math::finite(point) && std::abs(point.x) <= coordinate_limit &&
        std::abs(point.y) <= coordinate_limit && std::abs(point.z) <= coordinate_limit;
}

bool interval(double value, double lower, double upper) noexcept {
    return std::isfinite(value) && value >= lower && value <= upper;
}

bool valid_target(const PerceptionTarget& target) noexcept {
    return bounded_point(target.aim_offset) && target.faction < 64 &&
        interval(target.visibility, 0.0, 1.0) && interval(target.threat, 0.0, 1.0) &&
        interval(target.salience, 0.0, 1.0);
}

bool valid_stimulus(const Stimulus& stimulus) noexcept {
    const bool known_kind = stimulus.kind == StimulusKind::Sound ||
        stimulus.kind == StimulusKind::Damage || stimulus.kind == StimulusKind::Alert;
    return known_kind && bounded_point(stimulus.absolute_position) && stimulus.faction < 64 &&
        interval(stimulus.strength, 0.000001, 1.0) &&
        interval(stimulus.radius, 0.000001, 1.0e6) &&
        interval(stimulus.lifetime_seconds, 0.000001, 3600.0) &&
        interval(stimulus.threat, 0.0, 1.0);
}

PerceptionLimits checked_limits(PerceptionLimits limits) {
    const auto bounded = [](std::size_t value, std::size_t maximum) {
        return value != 0 && value <= maximum;
    };
    if (!bounded(limits.max_sensors, 4096) ||
        !bounded(limits.memories_per_sensor, 256) ||
        !bounded(limits.candidates_per_sensor, 4096) ||
        !bounded(limits.colliders_per_query, 4096) ||
        !bounded(limits.rules_per_sensor, 32) ||
        !bounded(limits.stimulus_history, 65536) ||
        !bounded(limits.change_history, 65536) ||
        !bounded(limits.sensors_per_frame, 4096) ||
        !bounded(limits.candidate_visits_per_frame, 1048576) ||
        !bounded(limits.line_queries_per_frame, 65536) ||
        !bounded(limits.shape_tests_per_frame, 1048576) ||
        !bounded(limits.stimulus_visits_per_frame, 1048576) ||
        !bounded(limits.utility_evaluations_per_frame, 1048576) ||
        !bounded(limits.spatial_visits_per_query, 4194304)) {
        throw std::invalid_argument("invalid perception limits");
    }
    return limits;
}

bool hostile_to(const SensorConfig& config, std::uint32_t faction) noexcept {
    return faction < 64 && faction != config.faction &&
        (config.hostile_factions & (std::uint64_t{1} << faction)) != 0;
}

bool same_choice(const PerceptionDecision& first, const PerceptionDecision& second) noexcept {
    return first.action == second.action && first.target == second.target &&
        first.anonymous_stimulus == second.anonymous_stimulus;
}

template<class T>
PerceptionHistory<T> read_history(const std::deque<T>& history, std::uint64_t newest,
                                  std::uint64_t cursor, std::size_t limit) {
    PerceptionHistory<T> result;
    result.next_cursor = cursor;
    result.latest_cursor = newest;
    result.history_lost = cursor > newest ||
        (!history.empty() && cursor < history.front().sequence - 1);
    if (cursor > newest) {
        return result;
    }
    result.records.reserve(std::min(limit, history.size()));
    for (const auto& record : history) {
        if (record.sequence <= cursor) { continue; }
        if (result.records.size() == limit) {
            result.truncated = true;
            break;
        }
        result.records.push_back(record);
        result.next_cursor = record.sequence;
    }
    return result;
}

bool shape_blocks(const physics::CollisionShape& shape, const math::Ray& ray,
                  double maximum) noexcept {
    return std::visit([&](const auto& primitive) {
        using Shape = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<Shape, math::Sphere>) {
            if (math::contains(primitive, ray.origin)) { return true; }
        } else if constexpr (std::is_same_v<Shape, math::Capsule>) {
            if (math::distance_squared(math::Segment{primitive.a, primitive.b}, ray.origin) <=
                primitive.radius * primitive.radius) { return true; }
        } else if constexpr (std::is_same_v<Shape, math::Obb>) {
            if (math::distance_squared(math::closest_point(primitive, ray.origin), ray.origin) <=
                ray_margin * ray_margin) { return true; }
        } else if constexpr (std::is_same_v<Shape, math::Plane>) {
            if (math::signed_distance(primitive, ray.origin) <= 0.0) { return true; }
        }
        return math::intersect(ray, primitive, 0.0, maximum).has_value();
    }, shape);
}

template<class T>
std::optional<T> number(std::string_view text) noexcept {
    if (text.empty() || text.size() > 96) { return std::nullopt; }
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) { return std::nullopt; }
    }
    return value;
}

ecs::Entity parse_entity(core::Kernel& kernel, std::string_view text) {
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) { return {}; }
    const auto delimiter = text.find(':');
    if (delimiter == std::string_view::npos) {
        const auto matches = registry->find_all_named(text);
        return matches.size() == 1 ? matches.front() : ecs::Entity{};
    }
    const auto index = number<std::uint32_t>(text.substr(0, delimiter));
    const auto generation = number<std::uint64_t>(text.substr(delimiter + 1));
    const ecs::Entity entity{index.value_or(0), generation.value_or(0)};
    return registry->alive(entity) ? entity : ecs::Entity{};
}

} // namespace

bool valid_sensor_config(const SensorConfig& config) noexcept {
    return bounded_point(config.eye_offset) && math::finite(config.forward_local) &&
        interval(math::length(config.forward_local), 0.000001, 1.0e6) &&
        interval(config.sight_range, 0.000001, 1.0e6) &&
        interval(config.hearing_range, 0.000001, 1.0e6) &&
        interval(config.field_of_view_radians, 0.000001, 6.283185307179586) &&
        interval(config.interval_seconds, 0.0, 60.0) &&
        interval(config.acquisition_rate, 0.000001, 1000.0) &&
        interval(config.confidence_half_life, 0.000001, 86400.0) &&
        interval(config.threat_half_life, 0.000001, 86400.0) &&
        interval(config.memory_seconds, 0.000001, 86400.0) &&
        interval(config.minimum_confidence, 0.0, 1.0) &&
        interval(config.hearing_threshold, 0.0, 1.0) &&
        interval(config.switch_margin, 0.0, 1.0) &&
        interval(config.minimum_hold_seconds, 0.0, 3600.0) &&
        interval(config.idle_score, 0.0, 1.0) &&
        interval(config.maximum_tracked_speed, 0.0, 1.0e6) && config.faction < 64 &&
        config.sight_samples >= 1 && config.sight_samples <= 3 &&
        (config.occlusion == OcclusionMode::ColliderPrimitives ||
         config.occlusion == OcclusionMode::ConservativeBounds);
}

bool valid_utility_rule(const UtilityRule& rule) noexcept {
    const bool action = rule.action >= UtilityAction::Observe && rule.action <= UtilityAction::Flee;
    return action && interval(rule.bias, -1.0, 1.0) &&
        interval(rule.confidence_weight, 0.0, 100.0) &&
        interval(rule.proximity_weight, 0.0, 100.0) &&
        interval(rule.threat_weight, 0.0, 100.0) &&
        interval(rule.freshness_weight, 0.0, 100.0) &&
        interval(rule.salience_weight, 0.0, 100.0) &&
        interval(rule.minimum_confidence, 0.0, 1.0) &&
        interval(rule.maximum_age_seconds, 0.000001, 86400.0) &&
        interval(rule.preferred_distance, 0.0, 1.0e6) &&
        interval(rule.distance_width, 0.000001, 1.0e6);
}

std::array<UtilityRule, 4> default_utility_rules() noexcept {
    std::array<UtilityRule, 4> rules{};
    rules[0].action = UtilityAction::Observe;
    rules[0].bias = -0.15;
    rules[0].require_visible = true;
    rules[1].action = UtilityAction::Investigate;
    rules[1].bias = -0.1;
    rules[1].minimum_confidence = 0.15;
    rules[1].freshness_weight = 2.0;
    rules[2].action = UtilityAction::Engage;
    rules[2].require_visible = true;
    rules[2].require_hostile = true;
    rules[2].minimum_confidence = 0.5;
    rules[2].threat_weight = 1.0;
    rules[2].bias = 0.1;
    rules[3].action = UtilityAction::Flee;
    rules[3].require_hostile = true;
    rules[3].minimum_confidence = 0.4;
    rules[3].threat_weight = 5.0;
    rules[3].proximity_weight = 2.0;
    rules[3].distance_width = 12.0;
    rules[3].bias = -0.15;
    return rules;
}

std::string_view utility_action_name(UtilityAction action) noexcept {
    switch (action) {
    case UtilityAction::None: return "none";
    case UtilityAction::Observe: return "observe";
    case UtilityAction::Investigate: return "investigate";
    case UtilityAction::Engage: return "engage";
    case UtilityAction::Flee: return "flee";
    }
    return "invalid";
}

std::string_view sight_state_name(SightState state) noexcept {
    switch (state) {
    case SightState::Unknown: return "unknown";
    case SightState::Clear: return "clear";
    case SightState::Blocked: return "blocked";
    case SightState::OutsideRange: return "outside_range";
    case SightState::OutsideField: return "outside_field";
    }
    return "invalid";
}

struct PerceptionService::Impl {
    struct Sensor {
        SensorHandle handle{};
        ecs::Entity owner{};
        SensorConfig config;
        std::vector<PerceptionMemory> memories;
        std::vector<UtilityRule> rules;
        PerceptionDecision decision;
        std::uint64_t stimulus_cursor{};
        double last_sensed{-1.0};
        double last_decayed{};
        math::Vec3 eye{};
        math::Vec3 forward{};
        bool pose_valid{};
        bool deferred{};
    };

    ecs::Registry& registry;
    scene::Scene& scene_graph;
    PerceptionLimits configuration;
    ge::GenerationPool handles;
    std::vector<std::optional<Sensor>> slots;
    std::vector<spatial::QueryHit> candidates;
    std::vector<spatial::RayHit> bound_hits;
    std::vector<physics::ColliderHandle> collider_hits;
    std::deque<StimulusRecord> stimuli;
    std::deque<PerceptionChange> changes;
    PerceptionStatistics stats;
    BehaviorService* behavior{};
    math::Vec3 origin{};
    double now{};
    std::uint64_t stimulus_sequence{};
    std::uint64_t change_sequence{};
    std::size_t round_robin{1};

    Impl(ecs::Registry& shared_registry, scene::Scene& shared_scene, PerceptionLimits limits)
        : registry(shared_registry), scene_graph(shared_scene), configuration(checked_limits(limits)),
          handles(configuration.max_sensors),
          slots(static_cast<std::size_t>(configuration.max_sensors) + 1),
          candidates(configuration.candidates_per_sensor),
          bound_hits(configuration.colliders_per_query),
          collider_hits(configuration.colliders_per_query) {
        if (&scene_graph.registry() != &registry) {
            throw std::invalid_argument("perception requires the scene's registry");
        }
    }

    Sensor* get(SensorHandle handle) noexcept {
        if (!handles.alive({handle.index, handle.generation}) || handle.index >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[handle.index];
        if (!slot || !registry.alive(slot->owner)) { return nullptr; }
        const auto* component = registry.get<PerceptionSensor>(slot->owner);
        return component && component->owner == slot->owner && component->handle == handle
            ? &*slot : nullptr;
    }

    const Sensor* get(SensorHandle handle) const noexcept {
        return const_cast<Impl*>(this)->get(handle);
    }

    void record(const Sensor& sensor, MemoryChangeKind kind,
                const PerceptionMemory* memory = nullptr) {
        if (change_sequence == std::numeric_limits<std::uint64_t>::max()) { return; }
        PerceptionChange change;
        change.sequence = change_sequence + 1;
        change.tick = stats.tick;
        change.sensor = sensor.handle;
        change.owner = sensor.owner;
        change.kind = kind;
        change.target = memory ? memory->target : sensor.decision.target;
        change.anonymous_stimulus = memory ? memory->anonymous_stimulus : sensor.decision.anonymous_stimulus;
        change.confidence = memory ? memory->confidence : sensor.decision.confidence;
        change.action = sensor.decision.action;
        changes.push_back(change);
        change_sequence = change.sequence;
        if (changes.size() > configuration.change_history) { changes.pop_front(); }
    }

    void clear_output(ecs::Entity entity) {
        auto* board = behavior ? behavior->blackboard(entity) : nullptr;
        if (!board) { return; }
        constexpr std::array<std::string_view, 11> keys{
            "perception.target", "perception.position", "perception.confidence", "perception.visible",
            "perception.threat", "perception.action", "perception.score", "perception.tick",
            "perception.deferred", "perception.valid", "perception.sight_tick"};
        for (const auto key : keys) { static_cast<void>(board->erase(key)); }
    }

    bool publish(const Sensor& sensor) {
        auto* board = behavior ? behavior->blackboard(sensor.owner) : nullptr;
        if (!board) { return true; }
        auto staged = *board;
        const auto& choice = sensor.decision;
        bool accepted = true;
        const auto put = [&](std::string_view key, auto value) {
            accepted = staged.set(key, std::move(value)) && accepted;
        };
        put("perception.valid", choice.has_position);
        put("perception.confidence", choice.confidence);
        put("perception.visible", choice.visible);
        put("perception.threat", choice.threat);
        put("perception.action", std::string(utility_action_name(choice.action)));
        put("perception.score", choice.score);
        put("perception.deferred", choice.deferred);
        const auto tick = std::min(choice.tick, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        put("perception.tick", static_cast<std::int64_t>(tick));
        const auto sight_tick = std::min(choice.sight_tick, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        put("perception.sight_tick", static_cast<std::int64_t>(sight_tick));
        if (choice.target) { put("perception.target", choice.target); }
        else { static_cast<void>(staged.erase("perception.target")); }
        if (choice.has_position) { put("perception.position", AbsolutePosition{choice.absolute_position}); }
        else { static_cast<void>(staged.erase("perception.position")); }
        if (accepted) { *board = std::move(staged); }
        return accepted;
    }

    PerceptionMemory* find_memory(Sensor& sensor, ecs::Entity target, std::uint64_t anonymous) {
        for (auto& memory : sensor.memories) {
            if (target ? memory.target == target : !memory.target && memory.anonymous_stimulus == anonymous) {
                return &memory;
            }
        }
        return nullptr;
    }

    PerceptionMemory* admit_memory(Sensor& sensor, ecs::Entity target, std::uint64_t anonymous,
                                   double confidence, double threat) {
        if (auto* existing = find_memory(sensor, target, anonymous)) { return existing; }
        if (sensor.memories.size() == configuration.memories_per_sensor) {
            const auto retained_value = [&](const PerceptionMemory& memory) {
                const double age = std::max(0.0, now - memory.last_observed_seconds);
                return memory.confidence * (1.0 + memory.threat) / (1.0 + age);
            };
            const auto weakest = std::min_element(sensor.memories.begin(), sensor.memories.end(),
                [&](const auto& first, const auto& second) { return retained_value(first) < retained_value(second); });
            if (retained_value(*weakest) >= confidence * (1.0 + threat)) { return nullptr; }
            record(sensor, MemoryChangeKind::Forgotten, &*weakest);
            sensor.memories.erase(weakest);
        }
        PerceptionMemory memory;
        memory.target = target;
        memory.anonymous_stimulus = anonymous;
        memory.first_observed_seconds = now;
        memory.last_observed_seconds = now;
        sensor.memories.push_back(memory);
        return &sensor.memories.back();
    }

    void observe(Sensor& sensor, ecs::Entity target, std::uint64_t anonymous,
                 math::Vec3 position, double strength, double threat, double salience,
                 std::uint32_t faction, bool sight, StimulusKind kind) {
        auto* memory = admit_memory(sensor, target, anonymous, strength, threat);
        if (!memory) { sensor.deferred = true; return; }
        const bool acquired = memory->observations == 0;
        const double duration = now - memory->last_observed_seconds;
        if (!sight && memory->sight_tick != stats.tick) { memory->sight = SightState::Unknown; }
        if (sight || memory->sight != SightState::Clear) {
            if (!acquired && duration > 0.000001) {
                const auto measured = math::clamp_length((position - memory->absolute_position) / duration,
                                                         sensor.config.maximum_tracked_speed);
                memory->velocity = memory->velocity * 0.5 + measured * 0.5;
            }
            memory->absolute_position = position;
        }
        memory->last_observed_seconds = now;
        memory->confidence = math::saturate(1.0 - (1.0 - memory->confidence) * (1.0 - strength));
        memory->threat = std::max(memory->threat, threat * strength);
        memory->salience = salience;
        memory->faction = faction;
        memory->hostile = hostile_to(sensor.config, faction);
        if (memory->observations != std::numeric_limits<std::uint64_t>::max()) { ++memory->observations; }
        if (sight) {
            memory->last_seen_seconds = now;
            memory->sight_tick = stats.tick;
            memory->sight = SightState::Clear;
        } else {
            if (kind == StimulusKind::Sound) {
                memory->last_heard_seconds = now;
                memory->heard = true;
            }
            memory->damaged = memory->damaged || kind == StimulusKind::Damage;
        }
        record(sensor, acquired ? MemoryChangeKind::Acquired : MemoryChangeKind::Updated, memory);
    }

    void decay(Sensor& sensor) {
        const double elapsed = std::max(0.0, now - sensor.last_decayed);
        const double confidence_factor = std::exp(-ln_two * elapsed / sensor.config.confidence_half_life);
        const double threat_factor = std::exp(-ln_two * elapsed / sensor.config.threat_half_life);
        for (auto cursor = sensor.memories.begin(); cursor != sensor.memories.end();) {
            cursor->confidence *= confidence_factor;
            cursor->threat *= threat_factor;
            cursor->hostile = hostile_to(sensor.config, cursor->faction);
            if ((cursor->target && !registry.alive(cursor->target)) ||
                now - cursor->last_observed_seconds > sensor.config.memory_seconds ||
                cursor->confidence < sensor.config.minimum_confidence) {
                record(sensor, MemoryChangeKind::Forgotten, &*cursor);
                cursor = sensor.memories.erase(cursor);
            } else { ++cursor; }
        }
        sensor.last_decayed = now;
    }

    SightState line_of_sight(const Sensor& sensor, ecs::Entity target, math::Vec3 point,
                             spatial::SpatialIndex* index, physics::CollisionWorld* collision) {
        if (stats.line_queries == configuration.line_queries_per_frame) { return SightState::Unknown; }
        ++stats.line_queries;
        const auto displacement = point - sensor.eye;
        const double distance = math::length(displacement);
        if (!bounded_point(point) || !std::isfinite(distance)) { return SightState::Unknown; }
        if (distance <= ray_margin) { return SightState::Unknown; }
        const math::Ray ray{sensor.eye, displacement / distance};
        const double maximum = distance - ray_margin;
        if (sensor.config.occlusion == OcclusionMode::ConservativeBounds) {
            if (!index || !stats.spatial_complete) { return SightState::Unknown; }
            spatial::QueryFilter filter;
            filter.layers = sensor.config.occluder_layers;
            filter.exclude = sensor.owner;
            filter.max_visits = configuration.spatial_visits_per_query;
            const auto report = index->tree().raycast(ray, maximum, bound_hits, filter);
            for (std::size_t slot = 0; slot < report.written; ++slot) {
                if (bound_hits[slot].entity != target) { return SightState::Blocked; }
            }
            return report.truncated || report.traversal_exhausted || report.invalid_input || report.stale
                ? SightState::Unknown : SightState::Clear;
        }
        if (!collision || !stats.collision_complete) { return SightState::Unknown; }
        const math::Aabb segment_bounds = math::expanded(math::include(math::Aabb{}, ray.origin), ray_margin);
        const auto bounds = math::expanded(math::include(segment_bounds, point), ray_margin);
        physics::CollisionFilter filter;
        filter.layers = std::numeric_limits<std::uint64_t>::max();
        filter.mask = sensor.config.occluder_layers;
        const auto report = collision->query(bounds, collider_hits, filter, sensor.owner);
        for (std::size_t slot = 0; slot < report.written; ++slot) {
            if (stats.shape_tests == configuration.shape_tests_per_frame) { return SightState::Unknown; }
            ++stats.shape_tests;
            const auto* collider = collision->get(collider_hits[slot]);
            if (!collider || !collider->active) { return SightState::Unknown; }
            if (collider->entity == target || collider->description.trigger) { continue; }
            if (shape_blocks(collider->world_shape, ray, maximum)) { return SightState::Blocked; }
        }
        return report.truncated || report.invalid_input ? SightState::Unknown : SightState::Clear;
    }

    void see(Sensor& sensor, spatial::SpatialIndex* index, physics::CollisionWorld* collision,
             double sensing_elapsed) {
        if (!index || !stats.spatial_complete) { sensor.deferred = true; return; }
        const double range = sensor.config.sight_range;
        const math::Vec3 extent{range, range, range};
        const math::Aabb bounds{sensor.eye - extent, sensor.eye + extent};
        spatial::QueryFilter filter;
        filter.layers = sensor.config.target_layers;
        filter.exclude = sensor.owner;
        filter.max_visits = configuration.spatial_visits_per_query;
        const auto report = index->tree().query(bounds, candidates, filter);
        sensor.deferred = sensor.deferred || report.truncated || report.traversal_exhausted ||
            report.invalid_input || report.stale;
        std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(report.written),
            [&](const auto& first, const auto& second) {
                const double first_distance = math::distance_squared(first.bounds, sensor.eye);
                const double second_distance = math::distance_squared(second.bounds, sensor.eye);
                return first_distance == second_distance ? first.entity < second.entity : first_distance < second_distance;
            });
        const double cosine = std::cos(sensor.config.field_of_view_radians * 0.5);
        for (std::size_t slot = 0; slot < report.written; ++slot) {
            if (stats.candidates == configuration.candidate_visits_per_frame) {
                sensor.deferred = true;
                break;
            }
            ++stats.candidates;
            const auto entity = candidates[slot].entity;
            const auto* target = registry.get<PerceptionTarget>(entity);
            const auto* matrix = scene_graph.cached_world_matrix(entity);
            if (!target || !target->enabled || !valid_target(*target) || !matrix ||
                target->visibility == 0.0 || !scene_graph.effective_enabled(entity)) { continue; }
            const auto aim = math::transform_point(*matrix, target->aim_offset);
            const auto center = math::center(candidates[slot].bounds);
            const auto upper = center + math::Vec3{0.0, math::half_extent(candidates[slot].bounds).y * 0.8, 0.0};
            const std::array<math::Vec3, 3> samples{aim, center, upper};
            SightState outcome = SightState::OutsideRange;
            bool unknown = false;
            math::Vec3 seen_position = aim;
            for (std::uint32_t sample = 0; sample < sensor.config.sight_samples; ++sample) {
                const auto displacement = samples[sample] - sensor.eye;
                const double distance = math::length(displacement);
                if (!std::isfinite(distance) || distance > sensor.config.sight_range) { continue; }
                if (distance > ray_margin && math::dot(sensor.forward, displacement / distance) < cosine) {
                    if (outcome == SightState::OutsideRange) { outcome = SightState::OutsideField; }
                    continue;
                }
                const auto visibility = line_of_sight(sensor, entity, samples[sample], index, collision);
                unknown = unknown || visibility == SightState::Unknown;
                outcome = visibility;
                if (visibility == SightState::Clear) { seen_position = samples[sample]; break; }
            }
            if (outcome != SightState::Clear && unknown) { outcome = SightState::Unknown; }
            if (auto* memory = find_memory(sensor, entity, 0)) { memory->sight = outcome; }
            sensor.deferred = sensor.deferred || outcome == SightState::Unknown;
            if (outcome != SightState::Clear) { continue; }
            const auto absolute = seen_position + origin;
            if (!bounded_point(absolute)) {
                if (auto* memory = find_memory(sensor, entity, 0)) { memory->sight = SightState::Unknown; }
                sensor.deferred = true;
                continue;
            }
            const double gain = 1.0 - std::exp(-sensor.config.acquisition_rate * sensing_elapsed * target->visibility);
            observe(sensor, entity, 0, absolute, gain, target->threat, target->salience,
                    target->faction, true, StimulusKind::Alert);
        }
    }

    void hear(Sensor& sensor) {
        if (stimuli.empty()) { return; }
        if (sensor.stimulus_cursor < stimuli.front().sequence - 1) {
            sensor.stimulus_cursor = stimuli.front().sequence - 1;
            if (stats.lost_sensor_histories != std::numeric_limits<std::uint64_t>::max()) { ++stats.lost_sensor_histories; }
            sensor.deferred = true;
        }
        const auto eye_absolute = sensor.eye + origin;
        const auto first_record = std::upper_bound(stimuli.begin(), stimuli.end(), sensor.stimulus_cursor,
            [](std::uint64_t cursor, const StimulusRecord& record) { return cursor < record.sequence; });
        for (auto event = first_record; event != stimuli.end(); ++event) {
            const auto& record = *event;
            if (stats.stimulus_visits == configuration.stimulus_visits_per_frame) {
                sensor.deferred = true;
                break;
            }
            ++stats.stimulus_visits;
            sensor.stimulus_cursor = record.sequence;
            const auto& stimulus = record.stimulus;
            if (now - record.admitted_seconds > stimulus.lifetime_seconds || stimulus.source == sensor.owner ||
                (stimulus.recipient && stimulus.recipient != sensor.owner) ||
                (stimulus.source && !registry.alive(stimulus.source))) { continue; }
            const bool direct_damage = stimulus.kind == StimulusKind::Damage && stimulus.recipient == sensor.owner;
            const double radius = std::min(stimulus.radius, sensor.config.hearing_range);
            const double distance = math::distance(stimulus.absolute_position, eye_absolute);
            if (!direct_damage && distance > radius) { continue; }
            const double attenuation = direct_damage ? 1.0 : std::pow(math::saturate(1.0 - distance / radius), 2.0);
            const double strength = stimulus.strength * attenuation;
            if (strength < sensor.config.hearing_threshold) { continue; }
            observe(sensor, stimulus.source, stimulus.source ? 0 : record.sequence,
                    stimulus.absolute_position, strength, stimulus.threat, stimulus.strength,
                    stimulus.faction, false, stimulus.kind);
        }
    }

    double score(const Sensor& sensor, const PerceptionMemory& memory, const UtilityRule& rule) const {
        const double age = std::max(0.0, now - memory.last_observed_seconds);
        const bool visible = memory.sight == SightState::Clear;
        if (!rule.enabled || memory.confidence < rule.minimum_confidence || age > rule.maximum_age_seconds ||
            (rule.require_visible && !visible) || (rule.require_hostile && !memory.hostile)) { return -1.0; }
        const double distance = math::distance(memory.absolute_position, sensor.eye + origin);
        const double proximity = math::saturate(1.0 - std::abs(distance - rule.preferred_distance) / rule.distance_width);
        const double freshness = math::saturate(1.0 - age / rule.maximum_age_seconds);
        const double threat = rule.invert_threat ? 1.0 - memory.threat : memory.threat;
        const double total_weight = rule.confidence_weight + rule.proximity_weight + rule.threat_weight +
            rule.freshness_weight + rule.salience_weight;
        const double weighted = memory.confidence * rule.confidence_weight + proximity * rule.proximity_weight +
            threat * rule.threat_weight + freshness * rule.freshness_weight + memory.salience * rule.salience_weight;
        return math::saturate(rule.bias + (total_weight > 0.0 ? weighted / total_weight : 0.0));
    }

    void choose(Sensor& sensor) {
        PerceptionDecision best;
        best.score = sensor.config.idle_score;
        best.tick = stats.tick;
        best.selected_seconds = now;
        std::optional<PerceptionDecision> incumbent;
        bool exhausted = false;
        if (sensor.pose_valid && sensor.config.enabled) {
            for (const auto& memory : sensor.memories) {
                for (const auto& rule : sensor.rules) {
                    if (stats.utility_evaluations == configuration.utility_evaluations_per_frame) {
                        exhausted = true;
                        break;
                    }
                    ++stats.utility_evaluations;
                    const double value = score(sensor, memory, rule);
                    if (value < sensor.config.idle_score) { continue; }
                    PerceptionDecision candidate;
                    candidate.action = rule.action;
                    candidate.target = memory.target;
                    candidate.anonymous_stimulus = memory.anonymous_stimulus;
                    candidate.absolute_position = memory.absolute_position;
                    candidate.score = value;
                    candidate.confidence = memory.confidence;
                    candidate.threat = memory.threat;
                    candidate.selected_seconds = now;
                    candidate.tick = stats.tick;
                    candidate.sight_tick = memory.sight_tick;
                    candidate.has_position = true;
                    candidate.visible = memory.sight == SightState::Clear;
                    if (same_choice(candidate, sensor.decision)) { incumbent = candidate; }
                    if (candidate.score > best.score) { best = candidate; }
                }
                if (exhausted) { break; }
            }
        }
        if (incumbent && !same_choice(best, *incumbent) &&
            (now - sensor.decision.selected_seconds < sensor.config.minimum_hold_seconds ||
             best.score < incumbent->score + sensor.config.switch_margin)) { best = *incumbent; }
        best.deferred = sensor.deferred || exhausted;
        if (same_choice(best, sensor.decision)) { best.selected_seconds = sensor.decision.selected_seconds; }
        const bool changed = !same_choice(best, sensor.decision);
        sensor.decision = best;
        if (changed) { record(sensor, MemoryChangeKind::DecisionChanged); }
        if (best.deferred) { ++stats.deferred_sensors; }
        if (!publish(sensor)) { ++stats.blackboard_failures; }
    }
};

PerceptionService::PerceptionService(ecs::Registry& registry, scene::Scene& scene,
                                     PerceptionLimits limits)
    : impl_(std::make_unique<Impl>(registry, scene, limits)) {}

PerceptionService::~PerceptionService() = default;

const PerceptionLimits& PerceptionService::limits() const noexcept { return impl_->configuration; }

SensorHandle PerceptionService::attach(ecs::Entity owner, SensorConfig config) {
    auto& state = *impl_;
    if (!valid_sensor_config(config) || !state.registry.alive(owner) ||
        !state.scene_graph.contains(owner) || state.registry.iterating()) { return {}; }
    if (const auto existing = sensor(owner)) { return existing; }
    const auto token = state.handles.acquire();
    if (!token) { return {}; }
    const SensorHandle handle{token->index, token->generation};
    try {
        Impl::Sensor attachment;
        attachment.handle = handle;
        attachment.owner = owner;
        attachment.config = config;
        attachment.memories.reserve(state.configuration.memories_per_sensor);
        const auto rules = default_utility_rules();
        const auto count = std::min(rules.size(), state.configuration.rules_per_sensor);
        attachment.rules.assign(rules.begin(), rules.begin() + static_cast<std::ptrdiff_t>(count));
        attachment.stimulus_cursor = state.stimulus_sequence;
        attachment.last_decayed = state.now;
        state.slots[handle.index] = std::move(attachment);
        state.registry.emplace_or_replace<PerceptionSensor>(owner, PerceptionSensor{handle, owner});
    } catch (...) {
        state.slots[handle.index].reset();
        static_cast<void>(state.handles.release(*token));
        throw;
    }
    return handle;
}

bool PerceptionService::detach(SensorHandle handle) {
    auto& state = *impl_;
    if (!state.handles.alive({handle.index, handle.generation}) || state.registry.iterating()) { return false; }
    auto& slot = state.slots[handle.index];
    if (!slot) { return false; }
    state.record(*slot, MemoryChangeKind::SensorRemoved);
    const auto replacement = sensor(slot->owner);
    if (!replacement || replacement == handle) { state.clear_output(slot->owner); }
    const auto* component = state.registry.get<PerceptionSensor>(slot->owner);
    if (component && component->handle == handle && component->owner == slot->owner) {
        static_cast<void>(state.registry.remove<PerceptionSensor>(slot->owner));
    }
    slot.reset();
    return state.handles.release({handle.index, handle.generation});
}

SensorHandle PerceptionService::sensor(ecs::Entity owner) const noexcept {
    const auto* component = impl_->registry.get<PerceptionSensor>(owner);
    return component && component->owner == owner && impl_->get(component->handle) ? component->handle : SensorHandle{};
}

bool PerceptionService::configure(SensorHandle handle, const SensorConfig& config) {
    auto* sensor = impl_->get(handle);
    if (!sensor || !valid_sensor_config(config)) { return false; }
    sensor->config = config;
    sensor->last_sensed = -1.0;
    return true;
}

bool PerceptionService::set_rules(SensorHandle handle, std::span<const UtilityRule> rules) {
    auto* sensor = impl_->get(handle);
    if (!sensor || rules.size() > impl_->configuration.rules_per_sensor ||
        !std::all_of(rules.begin(), rules.end(), valid_utility_rule)) { return false; }
    for (std::size_t first = 0; first < rules.size(); ++first) {
        for (std::size_t second = first + 1; second < rules.size(); ++second) {
            if (rules[first].action == rules[second].action) { return false; }
        }
    }
    sensor->rules.assign(rules.begin(), rules.end());
    return true;
}

bool PerceptionService::set_target(ecs::Entity entity, const PerceptionTarget& target) {
    if (!valid_target(target) || !impl_->registry.alive(entity) ||
        !impl_->scene_graph.contains(entity) || impl_->registry.iterating()) { return false; }
    impl_->registry.emplace_or_replace<PerceptionTarget>(entity, target);
    return true;
}

bool PerceptionService::remove_target(ecs::Entity entity) {
    return !impl_->registry.iterating() && impl_->registry.remove<PerceptionTarget>(entity);
}

bool PerceptionService::forget(SensorHandle handle, ecs::Entity target) {
    auto* sensor = impl_->get(handle);
    if (!sensor) { return false; }
    bool removed = false;
    for (auto cursor = sensor->memories.begin(); cursor != sensor->memories.end();) {
        if (!target || cursor->target == target) {
            impl_->record(*sensor, MemoryChangeKind::Forgotten, &*cursor);
            cursor = sensor->memories.erase(cursor);
            removed = true;
        } else { ++cursor; }
    }
    if (!target || sensor->decision.target == target) {
        sensor->decision = {};
        sensor->decision.tick = impl_->stats.tick;
        if (!impl_->publish(*sensor)) { ++impl_->stats.blackboard_failures; }
    }
    return removed;
}

std::optional<SensorSnapshot> PerceptionService::inspect(SensorHandle handle) const {
    const auto* sensor = impl_->get(handle);
    if (!sensor) { return std::nullopt; }
    return SensorSnapshot{sensor->handle, sensor->owner, sensor->config, sensor->memories,
                          sensor->rules, sensor->decision, sensor->stimulus_cursor, sensor->last_sensed};
}

std::optional<PerceptionDecision> PerceptionService::decision(SensorHandle handle) const {
    const auto* sensor = impl_->get(handle);
    return sensor ? std::optional{sensor->decision} : std::nullopt;
}

std::uint64_t PerceptionService::emit(const Stimulus& stimulus) {
    auto& state = *impl_;
    if (!valid_stimulus(stimulus) || (stimulus.source && !state.registry.alive(stimulus.source)) ||
        (stimulus.recipient && !state.registry.alive(stimulus.recipient)) ||
        state.stimulus_sequence == std::numeric_limits<std::uint64_t>::max()) {
        if (state.stats.rejected_stimuli != std::numeric_limits<std::uint64_t>::max()) { ++state.stats.rejected_stimuli; }
        return 0;
    }
    const auto sequence = state.stimulus_sequence + 1;
    state.stimuli.push_back({sequence, state.now, stimulus});
    state.stimulus_sequence = sequence;
    if (state.stimuli.size() > state.configuration.stimulus_history) { state.stimuli.pop_front(); }
    if (state.stats.admitted_stimuli != std::numeric_limits<std::uint64_t>::max()) { ++state.stats.admitted_stimuli; }
    return sequence;
}

PerceptionHistory<StimulusRecord> PerceptionService::stimuli_since(std::uint64_t cursor, std::size_t limit) const {
    return read_history(impl_->stimuli, impl_->stimulus_sequence, cursor,
                        std::min(limit, impl_->configuration.stimulus_history));
}

PerceptionHistory<PerceptionChange> PerceptionService::changes_since(std::uint64_t cursor, std::size_t limit) const {
    return read_history(impl_->changes, impl_->change_sequence, cursor,
                        std::min(limit, impl_->configuration.change_history));
}

const PerceptionStatistics& PerceptionService::statistics() const noexcept { return impl_->stats; }

void PerceptionService::update(const core::Frame& frame, spatial::SpatialIndex* index,
                               physics::CollisionWorld* collision, scene::WorldPartition* partition,
                               BehaviorService* behavior) {
    auto& state = *impl_;
    if (!interval(frame.delta_seconds, 0.0, 86400.0) ||
        !std::isfinite(state.now + frame.delta_seconds) || state.registry.iterating()) { return; }
    const auto admitted = state.stats.admitted_stimuli;
    const auto rejected = state.stats.rejected_stimuli;
    const auto history_lost = state.stats.lost_sensor_histories;
    state.stats = {};
    state.stats.admitted_stimuli = admitted;
    state.stats.rejected_stimuli = rejected;
    state.stats.lost_sensor_histories = history_lost;
    state.stats.tick = frame.tick;
    state.now += frame.delta_seconds;
    state.behavior = behavior;
    const auto coordinates = partition ? partition->origin() : scene::OriginState{};
    state.origin = coordinates.absolute_origin;
    state.stats.coordinates_consistent = coordinates.consistent && bounded_point(state.origin);
    const auto scene_result = state.scene_graph.update();
    if (index && scene_result.error == scene::SceneError::None) {
        const auto synced = index->sync();
        state.stats.spatial_complete = !synced.scene_busy && synced.rejected == 0 && synced.capacity_failures == 0;
    }
    if (collision && scene_result.error == scene::SceneError::None) {
        const auto synced = collision->sync(state.scene_graph);
        state.stats.collision_complete = !synced.scene_busy && synced.rejected == 0;
    }
    const std::size_t slot_count = state.slots.size() - 1;
    const std::size_t first_slot = state.round_robin;
    std::size_t next_slot = first_slot;
    std::optional<std::size_t> first_deferred_slot;
    for (std::size_t offset = 0; offset < slot_count; ++offset) {
        const std::size_t slot_index = 1 + ((first_slot - 1 + offset) % slot_count);
        auto& slot = state.slots[slot_index];
        if (!slot) { continue; }
        if (!state.get(slot->handle)) {
            static_cast<void>(detach(slot->handle));
            continue;
        }
        auto& sensor = *slot;
        ++state.stats.sensors;
        // Shared work budgets must rotate even when every sensor fits the
        // sampling budget, or when utility runs between scheduled samples.
        if (state.stats.sensors == 1) { next_slot = 1 + (slot_index % slot_count); }
        sensor.deferred = false;
        state.decay(sensor);
        const auto* matrix = state.scene_graph.cached_world_matrix(sensor.owner);
        sensor.pose_valid = matrix && state.stats.coordinates_consistent &&
            scene_result.error == scene::SceneError::None && state.scene_graph.effective_enabled(sensor.owner);
        if (sensor.pose_valid) {
            sensor.eye = math::transform_point(*matrix, sensor.config.eye_offset);
            sensor.forward = math::normalized(math::transform_vector(*matrix, sensor.config.forward_local));
            sensor.pose_valid = bounded_point(sensor.eye) && bounded_point(sensor.eye + state.origin) &&
                math::length_squared(sensor.forward) > 0.5;
        }
        const bool due = sensor.last_sensed < 0.0 || state.now - sensor.last_sensed >= sensor.config.interval_seconds;
        if (due || !sensor.pose_valid || !sensor.config.enabled) {
            for (auto& memory : sensor.memories) { memory.sight = SightState::Unknown; }
        }
        if (sensor.config.enabled && due && sensor.pose_valid) {
            if (state.stats.sensed < state.configuration.sensors_per_frame) {
                const double elapsed = sensor.last_sensed < 0.0 ? std::max(frame.delta_seconds, 0.025)
                    : std::max(0.0, state.now - sensor.last_sensed);
                ++state.stats.sensed;
                state.see(sensor, index, collision, elapsed);
                state.hear(sensor);
                sensor.last_sensed = state.now;
            } else {
                sensor.deferred = true;
                if (!first_deferred_slot) { first_deferred_slot = slot_index; }
            }
        } else if (sensor.config.enabled && !sensor.pose_valid) { sensor.deferred = true; }
        state.choose(sensor);
        state.stats.memories += sensor.memories.size();
    }
    state.round_robin = first_deferred_slot.value_or(next_slot);
}

void register_perception(core::Kernel& kernel) {
    if (!kernel.register_module({"ai.perception", "Bounded sight, stimulus memory and utility intent", 1})) { return; }
    auto* registry = kernel.services().find<ecs::Registry>();
    auto* scene_graph = kernel.services().find<scene::Scene>();
    if (!registry || !scene_graph) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "ai.perception", "Registry and Scene are required");
        return;
    }
    static_cast<void>(registry->register_component<PerceptionSensor>("ai.perception.sensor"));
    static_cast<void>(registry->register_component<PerceptionTarget>("ai.perception.target"));
    kernel.services().emplace<PerceptionService>(*registry, *scene_graph);
    static_cast<void>(kernel.add_system({"ai.perception.sense", core::Phase::Visibility, -7100,
        {"spatial.sync", "camera.update"}}, [](core::Kernel& host, const core::Frame& frame) {
        auto& service = host.services().get<PerceptionService>();
        service.update(frame, host.services().find<spatial::SpatialIndex>(),
                       host.services().find<physics::CollisionWorld>(),
                       host.services().find<scene::WorldPartition>(), host.services().find<BehaviorService>());
        const auto& stats = service.statistics();
        host.metric("ai.perception.memories", static_cast<double>(stats.memories));
        host.metric("ai.perception.deferred", static_cast<double>(stats.deferred_sensors));
        host.metric("ai.perception.line_queries", static_cast<double>(stats.line_queries));
        host.metric("ai.perception.blackboard_failures", static_cast<double>(stats.blackboard_failures));
    }));
    static_cast<void>(kernel.order_after("gameplay.interaction.update", "ai.perception.sense"));
    static_cast<void>(kernel.add_command("perception.attach", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1 && args.size() != 3) { return core::CommandResult{false, "usage: perception.attach entity [range faction]"}; }
        SensorConfig config;
        if (args.size() == 3) {
            const auto range = number<double>(args[1]);
            const auto faction = number<std::uint32_t>(args[2]);
            if (!range || !faction) { return core::CommandResult{false, "invalid range or faction"}; }
            config.sight_range = *range;
            config.faction = *faction;
        }
        const auto handle = host.services().get<PerceptionService>().attach(parse_entity(host, args[0]), config);
        return core::CommandResult{static_cast<bool>(handle), handle ? "sensor attached" : "sensor rejected"};
    }, "Attach an existing scene entity; handles may be index:generation or an unambiguous name"));
    static_cast<void>(kernel.add_command("perception.target", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 3) { return core::CommandResult{false, "usage: perception.target entity faction threat"}; }
        const auto faction = number<std::uint32_t>(args[1]);
        const auto threat = number<double>(args[2]);
        if (!faction || !threat) { return core::CommandResult{false, "invalid target values"}; }
        PerceptionTarget target;
        target.faction = *faction;
        target.threat = *threat;
        const bool accepted = host.services().get<PerceptionService>().set_target(parse_entity(host, args[0]), target);
        return core::CommandResult{accepted, accepted ? "target assigned; sight discovery requires scene bounds" : "target rejected"};
    }, "Assign faction and threat to an existing bounded scene target"));
    static_cast<void>(kernel.add_command("perception.sound", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 5) { return core::CommandResult{false, "usage: perception.sound x y z radius strength"}; }
        std::array<double, 5> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto value = number<double>(args[index]);
            if (!value) { return core::CommandResult{false, "invalid sound value"}; }
            values[index] = *value;
        }
        Stimulus stimulus;
        stimulus.absolute_position = {values[0], values[1], values[2]};
        if (const auto* partition = host.services().find<scene::WorldPartition>()) {
            const auto absolute = partition->to_absolute(stimulus.absolute_position);
            if (!absolute) { return core::CommandResult{false, "origin conversion unavailable"}; }
            stimulus.absolute_position = *absolute;
        }
        stimulus.radius = values[3];
        stimulus.strength = values[4];
        const auto sequence = host.services().get<PerceptionService>().emit(stimulus);
        return core::CommandResult{sequence != 0, sequence ? "sound admitted " + std::to_string(sequence) : "sound rejected"};
    }, "Emit an anonymous distance-attenuated sound at runtime coordinates"));
    static_cast<void>(kernel.add_command("perception.inspect", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "usage: perception.inspect entity"}; }
        auto& service = host.services().get<PerceptionService>();
        const auto snapshot = service.inspect(service.sensor(parse_entity(host, args[0])));
        if (!snapshot) { return core::CommandResult{false, "sensor unavailable"}; }
        std::ostringstream output;
        output << utility_action_name(snapshot->decision.action) << " score=" << snapshot->decision.score
               << " deferred=" << snapshot->decision.deferred << " memories=" << snapshot->memories.size();
        for (const auto& memory : snapshot->memories) {
            output << '\n' << memory.target.index << ':' << memory.target.generation
                   << " stimulus=" << memory.anonymous_stimulus << " confidence=" << memory.confidence
                   << " threat=" << memory.threat << " sight=" << sight_state_name(memory.sight)
                   << " absolute=" << memory.absolute_position.x << ',' << memory.absolute_position.y << ',' << memory.absolute_position.z;
        }
        return core::CommandResult{true, output.str()};
    }, "Inspect retained evidence and the next-frame behavior decision"));
    static_cast<void>(kernel.add_command("perception.forget", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1 && args.size() != 2) { return core::CommandResult{false, "usage: perception.forget sensor_entity [target]"}; }
        auto& service = host.services().get<PerceptionService>();
        const auto handle = service.sensor(parse_entity(host, args[0]));
        const auto target = args.size() == 2 ? parse_entity(host, args[1]) : ecs::Entity{};
        if (!handle || (args.size() == 2 && !target)) { return core::CommandResult{false, "invalid sensor or target"}; }
        const bool changed = service.forget(handle, target);
        return core::CommandResult{changed, changed ? "memory forgotten" : "no matching memory"};
    }, "Forget one target or every memory and clear matching intent"));
    static_cast<void>(kernel.add_command("perception.detach", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "usage: perception.detach entity"}; }
        auto& service = host.services().get<PerceptionService>();
        const bool removed = service.detach(service.sensor(parse_entity(host, args[0])));
        return core::CommandResult{removed, removed ? "sensor detached" : "sensor unavailable"};
    }, "Release a sensor generation and clear its behavior outputs"));
}

} // namespace ge::ai
