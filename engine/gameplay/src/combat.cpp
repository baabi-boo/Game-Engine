#include <ge/gameplay/combat.hpp>

#include <ge/ai/perception.hpp>
#include <ge/core/runtime.hpp>
#include <ge/gameplay/equipment.hpp>
#include <ge/gameplay/mission.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ge::gameplay {
namespace {

constexpr double stat_limit = 1.0e9;
constexpr double coordinate_limit = 1.0e9;
constexpr auto counter_limit = std::numeric_limits<std::uint64_t>::max();

void increment(std::uint64_t& value) noexcept {
    if (value != counter_limit) { ++value; }
}

bool interval(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool point_valid(math::Vec3 point) noexcept {
    return math::finite(point) && std::abs(point.x) <= coordinate_limit &&
        std::abs(point.y) <= coordinate_limit && std::abs(point.z) <= coordinate_limit;
}

bool config_valid(const CombatantConfig& config) noexcept {
    if (!interval(config.maximum_health, 1.0, stat_limit) ||
        !interval(config.maximum_shield, 0.0, stat_limit) ||
        !interval(config.armor, 0.0, stat_limit) || config.faction >= 64 ||
        config.defeat_key.empty() || config.defeat_key.size() > 64 ||
        config.immune_damage >= (std::uint64_t{1} << damage_type_count)) { return false; }
    for (const double resistance : config.resistance) {
        if (!interval(resistance, -1.0, 0.95)) { return false; }
    }
    return std::all_of(config.defeat_key.begin(), config.defeat_key.end(), [](char value) {
        return static_cast<unsigned int>(static_cast<unsigned char>(value)) >= 32U && value != '\x7f';
    });
}

bool component_valid(const CombatantComponent& component) noexcept {
    return config_valid(component.configuration) && component.life != 0 &&
        interval(component.health, 0.0, component.configuration.maximum_health) &&
        interval(component.shield, 0.0, component.configuration.maximum_shield) &&
        component.defeated == (component.health == 0.0);
}

bool damage_valid(const DamageProfile& profile) noexcept {
    return profile.type < DamageType::Count && interval(profile.amount, 0.0, stat_limit) &&
        interval(profile.hit_chance, 0.0, 1.0) && interval(profile.critical_chance, 0.0, 1.0) &&
        interval(profile.critical_multiplier, 1.0, 8.0) &&
        interval(profile.armor_penetration, 0.0, stat_limit);
}

bool status_valid(const StatusSpec& status) noexcept {
    return status.id != 0 && status.kind <= StatusKind::Stun &&
        status.stacking <= StatusStacking::Stack && status.damage_type < DamageType::Count &&
        interval(status.magnitude, 0.0, 1.0e6) && interval(status.duration_seconds, 0.05, 3600.0) &&
        interval(status.interval_seconds, 0.05, 3600.0) &&
        status.maximum_stacks > 0 && status.maximum_stacks <= 16;
}

std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double sample(std::uint64_t seed, ecs::Entity target, std::uint64_t domain) noexcept {
    const auto bits = mix(seed ^ mix(target.generation) ^
        (static_cast<std::uint64_t>(target.index) << 32U) ^ domain);
    return static_cast<double>(bits >> 11U) * (1.0 / 9007199254740992.0);
}

CombatLimits checked(CombatLimits limits) {
    const auto bounded = [](std::size_t value, std::size_t maximum) {
        return value != 0 && value <= maximum;
    };
    if (!bounded(limits.actors, 65536) || !bounded(limits.statuses_per_actor, 64) ||
        !bounded(limits.cooldowns_per_actor, 64) || !bounded(limits.history, 65536) ||
        !bounded(limits.query_candidates, 16384) || !bounded(limits.targets_per_attack, 128) ||
        !bounded(limits.periodic_ticks_per_frame, 16384) || !bounded(limits.mission_outbox, 65536) ||
        !bounded(limits.mission_attempts_per_frame, 4096) ||
        !interval(limits.maximum_delta_seconds, 0.001, 1.0)) {
        throw std::invalid_argument("invalid combat limits");
    }
    return limits;
}

struct BusyGuard {
    bool& flag;
    explicit BusyGuard(bool& value) noexcept : flag(value) { flag = true; }
    ~BusyGuard() { flag = false; }
};

bool parse_number(std::string_view text, double& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

bool parse_integer(std::string_view text, std::uint64_t& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

ecs::Entity resolve(ecs::Registry& registry, std::string_view text) {
    const auto separator = text.find(':');
    if (separator == std::string_view::npos) { return registry.find_name(text); }
    std::uint64_t index{}, generation{};
    if (!parse_integer(text.substr(0, separator), index) ||
        !parse_integer(text.substr(separator + 1), generation) ||
        index > std::numeric_limits<std::uint32_t>::max()) { return {}; }
    const ecs::Entity entity{static_cast<std::uint32_t>(index), generation};
    return registry.alive(entity) ? entity : ecs::Entity{};
}

std::optional<DamageType> parse_damage(std::string_view name) noexcept {
    for (std::size_t index = 0; index < damage_type_count; ++index) {
        const auto type = static_cast<DamageType>(index);
        if (damage_type_name(type) == name) { return type; }
    }
    return std::nullopt;
}

} // namespace

std::string_view describe(CombatError value) noexcept {
    switch (value) {
    case CombatError::None: return "ok";
    case CombatError::InvalidArgument: return "invalid argument";
    case CombatError::InvalidActor: return "invalid or stale actor";
    case CombatError::NotAttached: return "actor has no combat state";
    case CombatError::AlreadyAttached: return "combat state already exists";
    case CombatError::Disabled: return "actor disabled";
    case CombatError::Defeated: return "actor defeated";
    case CombatError::FriendlyFire: return "friendly or self damage blocked";
    case CombatError::Immune: return "target immune";
    case CombatError::Cooldown: return "attack cooling down";
    case CombatError::Stunned: return "actor stunned";
    case CombatError::Capacity: return "combat capacity reached";
    case CombatError::IncompleteQuery: return "primitive query incomplete";
    case CombatError::MissingService: return "required service unavailable";
    case CombatError::InventoryRejected: return "inventory cost rejected";
    case CombatError::Exhausted: return "sequence exhausted";
    case CombatError::AllocationFailure: return "allocation failed";
    case CombatError::Busy: return "combat service busy";
    }
    return "unknown combat error";
}

std::string_view damage_type_name(DamageType value) noexcept {
    constexpr std::array<std::string_view, damage_type_count> names{
        "physical", "fire", "cold", "shock", "poison", "pure"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "invalid";
}

struct CombatService::Impl {
    struct Actor {
        std::vector<ActiveStatus> statuses;
        std::vector<CombatCooldown> cooldowns;
    };
    struct PendingDefeat {
        MissionInput input;
        PersistentId persistent_source{};
    };
    struct PreparedDamage {
        DamageResult result;
        CombatEvent event;
        DamageRequest request;
        std::optional<PendingDefeat> mission;
        double health{};
        double shield{};
    };
    core::Kernel& kernel;
    ecs::Registry& registry;
    scene::Scene& scene;
    CombatLimits limits;
    CombatMissionCursor mission;
    std::map<ecs::Entity, Actor> actors;
    std::vector<CombatEvent> history;
    std::vector<PendingDefeat> outbox;
    CombatStatistics counters;
    std::size_t history_begin{};
    std::size_t outbox_cursor{};
    std::uint64_t sequence{};
    std::uint64_t tick{};
    ecs::Entity periodic_cursor{};
    bool busy{};

    Impl(core::Kernel& host, ecs::Registry& entities, scene::Scene& world,
         CombatLimits configuration, CombatMissionCursor cursor)
        : kernel(host), registry(entities), scene(world), limits(checked(configuration)), mission(cursor) {
        if (&scene.registry() != &registry || mission.next_sequence == 0) {
            throw std::invalid_argument("combat registry or mission cursor mismatch");
        }
        history.reserve(limits.history);
        outbox.reserve(limits.mission_outbox);
    }

    CombatantComponent* component(ecs::Entity entity) noexcept {
        return actors.contains(entity) ? registry.get<CombatantComponent>(entity) : nullptr;
    }
    const CombatantComponent* component(ecs::Entity entity) const noexcept {
        return actors.contains(entity) ? registry.get<CombatantComponent>(entity) : nullptr;
    }
    CombatError actor_error(ecs::Entity entity, bool living = true) const noexcept {
        if (!registry.alive(entity)) { return CombatError::InvalidActor; }
        const auto* value = component(entity);
        if (!value) { return CombatError::NotAttached; }
        if (!component_valid(*value)) { return CombatError::InvalidArgument; }
        if (!registry.enabled(entity)) { return CombatError::Disabled; }
        if (living && value->defeated) { return CombatError::Defeated; }
        return CombatError::None;
    }
    bool room(std::size_t events) const noexcept {
        return events <= counter_limit - sequence;
    }
    std::uint64_t record(CombatEvent event) noexcept {
        event.sequence = ++sequence;
        event.tick = tick;
        if (history.size() < limits.history) { history.push_back(event); }
        else {
            history[history_begin] = event;
            history_begin = (history_begin + 1) % history.size();
        }
        return event.sequence;
    }
    void mark(ecs::Entity entity) noexcept {
        try { static_cast<void>(registry.mark_changed<CombatantComponent>(entity)); }
        catch (...) { increment(counters.notification_failures); }
    }
    double status_total(ecs::Entity entity, StatusKind kind) const noexcept {
        const auto found = actors.find(entity);
        if (found == actors.end()) { return 0.0; }
        double total{};
        for (const auto& status : found->second.statuses) {
            if (status.specification.kind == kind && status.remaining_seconds > 0.0) {
                total = std::min(stat_limit, total + status.specification.magnitude * static_cast<double>(status.stacks));
            }
        }
        return total;
    }
    bool stunned(ecs::Entity entity) const noexcept {
        const auto found = actors.find(entity);
        if (found == actors.end()) { return false; }
        return std::any_of(found->second.statuses.begin(), found->second.statuses.end(), [](const auto& value) {
            return value.specification.kind == StatusKind::Stun && value.remaining_seconds > 0.0;
        });
    }
    EquipmentModifierSnapshot modifiers(ecs::Entity entity) const {
        const auto* equipment = kernel.services().find<EquipmentService>();
        return equipment && entity ? equipment->modifiers(entity) : EquipmentModifierSnapshot{};
    }
    PreparedDamage prepare(const DamageRequest& request, std::size_t reserved_defeats = 0) {
        PreparedDamage prepared;
        prepared.request = request;
        prepared.result.target = request.target;
        auto reject = [&](CombatError error) {
            prepared.result = {error, request.target};
            return prepared;
        };
        if (!damage_valid(request.damage) || !point_valid(request.point) ||
            !point_valid(request.impulse)) { return reject(CombatError::InvalidArgument); }
        if (request.source != ecs::Entity{} && (!registry.alive(request.source) || !registry.enabled(request.source))) {
            return reject(CombatError::InvalidActor);
        }
        const auto failure = actor_error(request.target);
        if (failure != CombatError::None) { return reject(failure); }
        if (!room(1)) { return reject(CombatError::Exhausted); }
        const auto& target = *component(request.target);
        const auto* source = component(request.source);
        if (source && (!component_valid(*source) || source->defeated)) { return reject(CombatError::Defeated); }
        if (request.source == request.target && !target.configuration.self_damage) {
            return reject(CombatError::FriendlyFire);
        }
        if (source && request.source != request.target && !source->configuration.friendly_fire &&
            source->configuration.faction != 0 &&
            source->configuration.faction == target.configuration.faction) {
            return reject(CombatError::FriendlyFire);
        }
        const auto type_index = static_cast<std::size_t>(request.damage.type);
        if (target.configuration.invulnerable ||
            (target.configuration.immune_damage & (std::uint64_t{1} << type_index)) != 0) {
            return reject(CombatError::Immune);
        }
        prepared.health = target.health;
        prepared.shield = target.shield;
        prepared.event.source = request.source;
        prepared.event.target = request.target;
        prepared.event.persistent_target = registry.persistent_id(request.target);
        prepared.event.damage_type = request.damage.type;
        prepared.event.life = target.life;
        prepared.result.hit = sample(request.seed, request.target, 0x484954ULL) < request.damage.hit_chance;
        prepared.event.kind = prepared.result.hit ? CombatEventKind::Damaged : CombatEventKind::Missed;
        if (!prepared.result.hit) { return prepared; }
        const auto outgoing = modifiers(request.source);
        const auto incoming = modifiers(request.target);
        const double critical_chance = std::clamp(request.damage.critical_chance + outgoing.additive("crit_chance"), 0.0, 1.0);
        prepared.result.critical = sample(request.seed, request.target, 0x43524954ULL) < critical_chance;
        const double critical_multiplier = std::clamp(request.damage.critical_multiplier + outgoing.additive("crit_multiplier"), 1.0, 8.0);
        double amount = std::clamp(request.damage.amount + outgoing.additive("attack_damage"), 0.0, stat_limit);
        amount *= std::clamp(1.0 + outgoing.additive("damage_dealt"), 0.0, 16.0);
        if (prepared.result.critical) { amount *= critical_multiplier; }
        amount *= std::clamp(1.0 + incoming.additive("damage_taken"), 0.0, 16.0);
        amount *= std::clamp(1.0 + status_total(request.target, StatusKind::Vulnerability), 1.0, 16.0);
        if (request.damage.type == DamageType::Physical) {
            const double armor = std::clamp(target.configuration.armor + incoming.additive("armor") -
                status_total(request.target, StatusKind::ArmorBreak) - request.damage.armor_penetration, 0.0, stat_limit);
            amount *= 100.0 / (100.0 + armor);
        }
        if (request.damage.type != DamageType::Pure) { amount *= 1.0 - target.configuration.resistance[type_index]; }
        amount = std::clamp(amount, 0.0, stat_limit);
        prepared.result.shield_damage = request.damage.bypass_shield ? 0.0 : std::min(target.shield, amount);
        const double floor = request.damage.nonlethal ? std::min(1.0, target.health) : 0.0;
        prepared.result.health_damage = std::min(target.health - floor, amount - prepared.result.shield_damage);
        prepared.health -= prepared.result.health_damage;
        prepared.shield -= prepared.result.shield_damage;
        prepared.result.defeated = prepared.health == 0.0;
        prepared.event.health_change = -prepared.result.health_damage;
        prepared.event.shield_change = -prepared.result.shield_damage;
        prepared.event.critical = prepared.result.critical;
        if (prepared.result.defeated) {
            prepared.event.kind = CombatEventKind::Defeated;
            if (request.source && mission.producer != 0) {
                if (outbox.size() + reserved_defeats >= limits.mission_outbox) { return reject(CombatError::Capacity); }
                if (reserved_defeats >= counter_limit - mission.next_sequence) { return reject(CombatError::Exhausted); }
                PendingDefeat pending;
                pending.input.id = {mission.producer, mission.next_sequence + static_cast<std::uint64_t>(reserved_defeats)};
                pending.input.actor = request.source;
                pending.input.kind = MissionObjectiveKind::Defeat;
                pending.input.key = target.configuration.defeat_key;
                pending.input.target = registry.persistent_id(request.target);
                pending.persistent_source = registry.persistent_id(request.source);
                prepared.mission = std::move(pending);
            }
        }
        return prepared;
    }
    void notify_damage(const PreparedDamage& prepared) noexcept {
        if (prepared.result.health_damage + prepared.result.shield_damage == 0.0) { return; }
        auto* perception = kernel.services().find<ai::PerceptionService>();
        if (!perception) { increment(counters.perception_failures); return; }
        try {
            ai::Stimulus stimulus;
            stimulus.kind = ai::StimulusKind::Damage;
            stimulus.source = prepared.request.source;
            stimulus.recipient = prepared.request.target;
            stimulus.absolute_position = prepared.request.point;
            if (prepared.request.source) {
                if (const auto position = scene.local_to_world(prepared.request.source, {})) {
                    stimulus.absolute_position = *position;
                }
            }
            if (auto* partition = kernel.services().find<scene::WorldPartition>()) {
                const auto absolute = partition->to_absolute(stimulus.absolute_position);
                if (!absolute) { increment(counters.perception_failures); return; }
                stimulus.absolute_position = *absolute;
            }
            stimulus.strength = std::clamp((prepared.result.health_damage + prepared.result.shield_damage) / 100.0, 0.000001, 1.0);
            stimulus.threat = stimulus.strength;
            if (const auto* source = component(prepared.request.source)) { stimulus.faction = source->configuration.faction; }
            if (perception->emit(stimulus) == 0) { increment(counters.perception_failures); }
        } catch (...) { increment(counters.perception_failures); }
    }
    DamageResult commit(PreparedDamage prepared) noexcept {
        auto& target = *component(prepared.request.target);
        target.health = prepared.health;
        target.shield = prepared.shield;
        target.defeated = prepared.result.defeated;
        prepared.result.sequence = record(prepared.event);
        increment(counters.damage_resolutions);
        if (prepared.result.defeated) {
            increment(counters.defeated);
            if (prepared.mission) {
                outbox.push_back(std::move(*prepared.mission));
                ++mission.next_sequence;
            } else if (prepared.request.source) { increment(counters.mission_disabled_credits); }
        }
        mark(prepared.request.target);
        if (prepared.result.hit && prepared.result.health_damage + prepared.result.shield_damage > 0.0) {
            if (auto* world = kernel.services().find<physics::PhysicsWorld>()) {
                if (&world->registry() == &registry && math::length_squared(prepared.request.impulse) > 0.0) {
                    try {
                        prepared.result.impulse_applied = world->apply_impulse(
                            world->find(prepared.request.target), prepared.request.impulse, prepared.request.point);
                    } catch (...) { increment(counters.notification_failures); }
                }
            }
        }
        notify_damage(prepared);
        return prepared.result;
    }
    CombatResult heal(const HealRequest& request) {
        if (!interval(request.amount, 0.0, stat_limit) || request.pool > HealPool::Shield) { return {CombatError::InvalidArgument}; }
        if (request.source != ecs::Entity{} && !registry.alive(request.source)) { return {CombatError::InvalidActor}; }
        const auto failure = actor_error(request.target);
        if (failure != CombatError::None) { return {failure}; }
        if (!room(1)) { return {CombatError::Exhausted}; }
        auto& target = *component(request.target);
        const bool shield = request.pool == HealPool::Shield;
        double& current = shield ? target.shield : target.health;
        const double maximum = shield ? target.configuration.maximum_shield : target.configuration.maximum_health;
        const double gain = std::min(request.amount, maximum - current);
        current += gain;
        CombatEvent event;
        event.kind = CombatEventKind::Healed;
        event.source = request.source;
        event.target = request.target;
        event.life = target.life;
        event.health_change = shield ? 0.0 : gain;
        event.shield_change = shield ? gain : 0.0;
        mark(request.target);
        return {CombatError::None, record(event)};
    }
    void deliver() {
        auto* missions = kernel.services().find<MissionService>();
        const auto attempts = std::min(limits.mission_attempts_per_frame, outbox.size());
        for (std::size_t index = 0; index < attempts && !outbox.empty(); ++index) {
            outbox_cursor %= outbox.size();
            const auto& pending = outbox[outbox_cursor];
            bool delivered{};
            if (missions && registry.alive(pending.input.actor) &&
                registry.persistent_id(pending.input.actor) == pending.persistent_source) {
                try {
                    const auto result = missions->submit(pending.input);
                    delivered = result.error == MissionError::None || result.error == MissionError::DuplicateEvent;
                } catch (...) { delivered = false; }
            }
            if (delivered) { outbox.erase(outbox.begin() + static_cast<std::ptrdiff_t>(outbox_cursor)); }
            else { increment(counters.mission_failures); ++outbox_cursor; }
        }
    }
    CombatQuery query(const math::Aabb& bounds, const math::Ray* ray, double range,
                      const math::Sphere* sphere, physics::CollisionFilter filter,
                      ecs::Entity exclude, bool triggers) {
        CombatQuery result;
        if (const auto* world = kernel.services().find<physics::PhysicsWorld>(); world && world->stepping()) {
            result.error = CombatError::Busy;
            return result;
        }
        auto* collision = kernel.services().find<physics::CollisionWorld>();
        if (!collision || &collision->registry() != &registry) {
            result.error = CombatError::MissingService;
            return result;
        }
        const auto synchronized = collision->sync(scene);
        if (synchronized.scene_busy || synchronized.rejected != 0) {
            result.error = CombatError::IncompleteQuery;
            increment(counters.incomplete_queries);
            return result;
        }
        std::vector<physics::ColliderHandle> candidates(limits.query_candidates);
        const auto found = collision->query(bounds, candidates, filter, exclude);
        result.candidates = found.matched;
        if (found.truncated || found.invalid_input) {
            result.error = CombatError::IncompleteQuery;
            increment(counters.incomplete_queries);
            return result;
        }
        result.hits.reserve(found.written);
        for (std::size_t index = 0; index < found.written; ++index) {
            const auto* collider = collision->get(candidates[index]);
            if (!collider || !registry.alive(collider->entity) || !collider->active) {
                result.error = CombatError::IncompleteQuery;
                increment(counters.incomplete_queries);
                result.hits.clear();
                return result;
            }
            if (!triggers && collider->description.trigger) { continue; }
            CombatHit hit;
            hit.entity = collider->entity;
            hit.collider = collider->handle;
            if (ray) {
                const auto intersection = std::visit([&](const auto& shape) -> std::optional<math::RayHit> {
                    using Shape = std::decay_t<decltype(shape)>;
                    bool inside{};
                    if constexpr (std::is_same_v<Shape, math::Sphere>) { inside = math::contains(shape, ray->origin); }
                    else if constexpr (std::is_same_v<Shape, math::Capsule>) {
                        inside = math::distance_squared(math::Segment{shape.a, shape.b}, ray->origin) <= shape.radius * shape.radius;
                    } else if constexpr (std::is_same_v<Shape, math::Obb>) {
                        inside = math::distance_squared(math::closest_point(shape, ray->origin), ray->origin) <= 1.0e-16;
                    } else if constexpr (std::is_same_v<Shape, math::Plane>) {
                        inside = math::signed_distance(shape, ray->origin) <= 0.0;
                    }
                    if (inside) { return math::RayHit{0.0, ray->origin, -ray->direction, {}, true}; }
                    return math::intersect(*ray, shape, 0.0, range);
                }, collider->world_shape);
                if (!intersection) { continue; }
                hit.point = intersection->point;
                hit.normal = intersection->normal;
                hit.distance = intersection->distance;
            } else {
                const auto contact = physics::narrowphase(*sphere, collider->world_shape);
                if (!contact.touching()) { continue; }
                hit.point = contact.points[0].point_b;
                hit.normal = -contact.normal;
                hit.distance = math::distance(sphere->center, hit.point);
            }
            result.hits.push_back(hit);
        }
        std::sort(result.hits.begin(), result.hits.end(), [](const CombatHit& first, const CombatHit& second) {
            if (first.distance != second.distance) { return first.distance < second.distance; }
            return first.collider < second.collider;
        });
        std::vector<CombatHit> unique;
        unique.reserve(result.hits.size());
        std::set<ecs::Entity> seen;
        for (const auto& hit : result.hits) {
            if (seen.insert(hit.entity).second) { unique.push_back(hit); }
        }
        result.hits = std::move(unique);
        result.complete = true;
        return result;
    }
};

CombatService::CombatService(core::Kernel& kernel, ecs::Registry& registry, scene::Scene& scene,
                             CombatLimits limits, CombatMissionCursor mission)
    : impl_(std::make_unique<Impl>(kernel, registry, scene, limits, mission)) {}

CombatService::~CombatService() = default;

CombatResult CombatService::attach(ecs::Entity entity, CombatantConfig config) {
    auto& state = *impl_;
    if (state.busy) { return {CombatError::Busy}; }
    if (!state.registry.alive(entity)) { return {CombatError::InvalidActor}; }
    if (!config_valid(config)) { return {CombatError::InvalidArgument}; }
    if (state.actors.contains(entity) || state.registry.has<CombatantComponent>(entity)) { return {CombatError::AlreadyAttached}; }
    if (state.actors.size() >= state.limits.actors) { return {CombatError::Capacity}; }
    if (!state.room(1)) { return {CombatError::Exhausted}; }
    try {
        Impl::Actor actor;
        actor.statuses.reserve(state.limits.statuses_per_actor);
        actor.cooldowns.reserve(state.limits.cooldowns_per_actor);
        CombatantComponent component;
        component.health = config.maximum_health;
        component.shield = config.maximum_shield;
        component.configuration = std::move(config);
        state.actors.emplace(entity, std::move(actor));
        try { state.registry.emplace<CombatantComponent>(entity, std::move(component)); }
        catch (...) {
            state.actors.erase(entity);
            if (state.registry.has<CombatantComponent>(entity)) { static_cast<void>(state.registry.remove<CombatantComponent>(entity)); }
            throw;
        }
        CombatEvent event;
        event.kind = CombatEventKind::Attached;
        event.target = entity;
        event.life = 1;
        return {CombatError::None, state.record(event)};
    } catch (const std::bad_alloc&) { return {CombatError::AllocationFailure}; }
    catch (const std::logic_error&) { return {CombatError::Busy}; }
}

CombatResult CombatService::detach(ecs::Entity entity) {
    auto& state = *impl_;
    if (state.busy) { return {CombatError::Busy}; }
    if (!state.actors.contains(entity)) { return {CombatError::NotAttached}; }
    if (!state.room(1)) { return {CombatError::Exhausted}; }
    try { static_cast<void>(state.registry.remove<CombatantComponent>(entity)); }
    catch (const std::logic_error&) { return {CombatError::Busy}; }
    state.actors.erase(entity);
    CombatEvent event;
    event.kind = CombatEventKind::Detached;
    event.target = entity;
    return {CombatError::None, state.record(event)};
}

CombatResult CombatService::configure(ecs::Entity entity, const CombatantConfig& config) {
    auto& state = *impl_;
    if (state.busy) { return {CombatError::Busy}; }
    const auto failure = state.actor_error(entity, false);
    if (failure != CombatError::None) { return {failure}; }
    if (!config_valid(config)) { return {CombatError::InvalidArgument}; }
    try {
        auto copy = config;
        auto& target = *state.component(entity);
        target.configuration = std::move(copy);
        target.health = std::min(target.health, config.maximum_health);
        target.shield = std::min(target.shield, config.maximum_shield);
        state.mark(entity);
        return {};
    } catch (const std::bad_alloc&) { return {CombatError::AllocationFailure}; }
}

CombatResult CombatService::revive(ecs::Entity entity, double health_fraction) {
    auto& state = *impl_;
    if (state.busy) { return {CombatError::Busy}; }
    const auto failure = state.actor_error(entity, false);
    if (failure != CombatError::None) { return {failure}; }
    auto& target = *state.component(entity);
    if (!target.defeated || !interval(health_fraction, 0.000001, 1.0)) { return {CombatError::InvalidArgument}; }
    if (target.life == counter_limit || !state.room(1)) { return {CombatError::Exhausted}; }
    target.health = target.configuration.maximum_health * health_fraction;
    target.shield = 0.0;
    target.defeated = false;
    ++target.life;
    state.actors.at(entity).statuses.clear();
    state.actors.at(entity).cooldowns.clear();
    state.mark(entity);
    CombatEvent event;
    event.kind = CombatEventKind::Revived;
    event.target = entity;
    event.health_change = target.health;
    event.life = target.life;
    return {CombatError::None, state.record(event)};
}

std::optional<CombatSnapshot> CombatService::inspect(ecs::Entity entity) const {
    const auto* component = impl_->component(entity);
    if (!component || !component_valid(*component)) { return std::nullopt; }
    const auto& actor = impl_->actors.at(entity);
    return CombatSnapshot{entity, *component, actor.statuses, actor.cooldowns, can_act(entity)};
}

bool CombatService::can_act(ecs::Entity entity) const noexcept {
    return impl_->actor_error(entity) == CombatError::None && !impl_->stunned(entity);
}

double CombatService::cooldown(ecs::Entity entity, std::uint64_t attack) const noexcept {
    const auto found = impl_->actors.find(entity);
    if (found == impl_->actors.end() || !impl_->registry.alive(entity)) { return 0.0; }
    for (const auto& value : found->second.cooldowns) {
        if (value.attack == attack) { return value.remaining_seconds; }
    }
    return 0.0;
}

DamageResult CombatService::damage(const DamageRequest& request) {
    if (impl_->busy) { return {CombatError::Busy, request.target}; }
    BusyGuard guard(impl_->busy);
    try {
        auto prepared = impl_->prepare(request);
        return prepared.result ? impl_->commit(std::move(prepared)) : prepared.result;
    } catch (const std::bad_alloc&) { return {CombatError::AllocationFailure, request.target}; }
}

CombatResult CombatService::heal(const HealRequest& request) {
    if (impl_->busy) { return {CombatError::Busy}; }
    BusyGuard guard(impl_->busy);
    return impl_->heal(request);
}

CombatResult CombatService::apply_status(ecs::Entity target, ecs::Entity source, const StatusSpec& status) {
    auto& state = *impl_;
    if (state.busy) { return {CombatError::Busy}; }
    if (!status_valid(status)) { return {CombatError::InvalidArgument}; }
    const auto failure = state.actor_error(target);
    if (failure != CombatError::None) { return {failure}; }
    if (source != ecs::Entity{} && (!state.registry.alive(source) || !state.registry.enabled(source))) { return {CombatError::InvalidActor}; }
    const bool harmful = status.kind != StatusKind::HealOverTime && status.kind != StatusKind::ShieldOverTime;
    const auto& victim = *state.component(target);
    const auto* origin = state.component(source);
    if (harmful && victim.configuration.invulnerable) { return {CombatError::Immune}; }
    if (harmful && ((source == target && !victim.configuration.self_damage) ||
        (origin && source != target && origin->configuration.faction != 0 &&
         origin->configuration.faction == victim.configuration.faction && !origin->configuration.friendly_fire))) {
        return {CombatError::FriendlyFire};
    }
    auto& statuses = state.actors.at(target).statuses;
    const auto found = std::find_if(statuses.begin(), statuses.end(), [&](const ActiveStatus& current) {
        return current.specification.id == status.id && current.source == source;
    });
    if (found == statuses.end() && statuses.size() >= state.limits.statuses_per_actor) { return {CombatError::Capacity}; }
    if (!state.room(1)) { return {CombatError::Exhausted}; }
    ActiveStatus next{status, source, status.duration_seconds, status.interval_seconds, 1, 0};
    if (found != statuses.end() && status.stacking != StatusStacking::Replace) {
        if (found->specification.kind != status.kind || found->specification.damage_type != status.damage_type) {
            return {CombatError::InvalidArgument};
        }
        next.stacks = std::min(status.maximum_stacks, found->stacks + (status.stacking == StatusStacking::Stack ? 1U : 0U));
        next.until_tick_seconds = std::min(found->until_tick_seconds, status.interval_seconds);
        next.tick_sequence = found->tick_sequence;
    }
    if (found == statuses.end()) { statuses.push_back(next); }
    else { *found = next; }
    CombatEvent event;
    event.kind = CombatEventKind::StatusApplied;
    event.source = source;
    event.target = target;
    event.status = status.id;
    event.life = victim.life;
    return {CombatError::None, state.record(event)};
}

CombatResult CombatService::remove_status(ecs::Entity target, std::uint64_t id) {
    auto& state = *impl_;
    if (state.busy) { return {CombatError::Busy}; }
    const auto found = state.actors.find(target);
    if (found == state.actors.end() || !state.component(target)) { return {CombatError::NotAttached}; }
    auto& statuses = found->second.statuses;
    const auto count = std::count_if(statuses.begin(), statuses.end(), [&](const auto& value) { return value.specification.id == id; });
    if (count == 0) { return {CombatError::InvalidArgument}; }
    if (!state.room(1)) { return {CombatError::Exhausted}; }
    std::erase_if(statuses, [&](const auto& value) { return value.specification.id == id; });
    CombatEvent event;
    event.kind = CombatEventKind::StatusRemoved;
    event.target = target;
    event.status = id;
    return {CombatError::None, state.record(event)};
}

CombatQuery CombatService::raycast(const math::Ray& ray, double range,
                                  physics::CollisionFilter filter, ecs::Entity exclude, bool triggers) {
    if (!point_valid(ray.origin) || !point_valid(ray.direction) ||
        !interval(range, 0.000001, 1.0e6) || math::length_squared(ray.direction) < 1.0e-20) {
        return {CombatError::InvalidArgument, {}, 0, false};
    }
    const math::Ray unit{ray.origin, math::normalized(ray.direction)};
    const auto bounds = math::expanded(math::include(math::include(math::Aabb{}, unit.origin), unit.point(range)), 1.0e-6);
    try { return impl_->query(bounds, &unit, range, nullptr, filter, exclude, triggers); }
    catch (const std::bad_alloc&) { return {CombatError::AllocationFailure, {}, 0, false}; }
}

CombatQuery CombatService::melee(const math::Sphere& sphere, physics::CollisionFilter filter,
                                ecs::Entity exclude, bool triggers) {
    if (!point_valid(sphere.center) || !interval(sphere.radius, 0.000001, 1.0e6)) {
        return {CombatError::InvalidArgument, {}, 0, false};
    }
    try { return impl_->query(math::bounds(sphere), nullptr, 0.0, &sphere, filter, exclude, triggers); }
    catch (const std::bad_alloc&) { return {CombatError::AllocationFailure, {}, 0, false}; }
}

AttackResult CombatService::attack(const AttackRequest& request) {
    auto& state = *impl_;
    AttackResult result;
    const auto reject = [&](CombatError error) {
        result.error = error;
        return result;
    };
    if (state.busy) { return reject(CombatError::Busy); }
    BusyGuard guard(state.busy);
    const auto failure = state.actor_error(request.source);
    if (failure != CombatError::None) { return reject(failure); }
    if (state.stunned(request.source)) { return reject(CombatError::Stunned); }
    if (request.attack == 0 || request.kind > AttackKind::MeleeSphere || !damage_valid(request.damage) ||
        !interval(request.cooldown_seconds, 0.0, 3600.0) || request.cost_count > request.costs.size() ||
        request.maximum_targets == 0 || request.maximum_targets > state.limits.targets_per_attack) {
        return reject(CombatError::InvalidArgument);
    }
    if (cooldown(request.source, request.attack) > 0.0) { return reject(CombatError::Cooldown); }
    auto& cooldowns = state.actors.at(request.source).cooldowns;
    if (request.cooldown_seconds > 0.0 && cooldowns.size() >= state.limits.cooldowns_per_actor) {
        return reject(CombatError::Capacity);
    }
    try {
        const auto query = request.kind == AttackKind::Ray
            ? raycast(request.ray, request.range, request.filter, request.source, request.include_triggers)
            : melee({request.ray.origin, request.radius}, request.filter, request.source, request.include_triggers);
        if (!query.complete) { return reject(query.error); }
        const auto maximum = request.kind == AttackKind::Ray ? std::size_t{1} : request.maximum_targets;
        const auto count = std::min(maximum, query.hits.size());
        if (!state.room(count + 1)) { return reject(CombatError::Exhausted); }
        std::vector<Impl::PreparedDamage> plans;
        plans.reserve(count);
        result.targets.reserve(count);
        std::size_t defeat_reservations{};
        for (std::size_t index = 0; index < count; ++index) {
            DamageRequest damage_request;
            damage_request.source = request.source;
            damage_request.target = query.hits[index].entity;
            damage_request.damage = request.damage;
            damage_request.seed = request.seed;
            damage_request.point = query.hits[index].point;
            auto plan = state.prepare(damage_request, defeat_reservations);
            const auto error = plan.result.error;
            if (error != CombatError::None && error != CombatError::NotAttached && error != CombatError::Defeated &&
                error != CombatError::FriendlyFire && error != CombatError::Immune && error != CombatError::Disabled) {
                return reject(error);
            }
            if (plan.mission) { ++defeat_reservations; }
            plans.push_back(std::move(plan));
        }
        if (request.cost_count != 0) {
            auto* inventory = state.kernel.services().find<InventoryService>();
            if (!inventory) { return reject(CombatError::MissingService); }
            InventoryTransaction transaction;
            transaction.operations.reserve(request.cost_count);
            for (std::size_t index = 0; index < request.cost_count; ++index) {
                const auto& cost = request.costs[index];
                if (!cost.item || cost.quantity == 0 || cost.quantity > 1000000U) { return reject(CombatError::InvalidArgument); }
                const auto container = cost.container ? cost.container : inventory->primary(request.source);
                const auto contents = inventory->inspect(container);
                if (!contents || contents->specification.owner != request.source) { return reject(CombatError::InvalidActor); }
                InventoryOperation operation;
                operation.kind = InventoryOperationKind::Remove;
                operation.source = container;
                operation.definition = cost.item;
                operation.quantity = cost.quantity;
                operation.expected_source_revision = contents->revision;
                transaction.operations.push_back(operation);
            }
            const auto paid = inventory->transact(transaction);
            if (!paid) {
                result.inventory_error = paid.error;
                return reject(CombatError::InventoryRejected);
            }
            result.costs_committed = true;
        }
        if (request.cooldown_seconds > 0.0) { cooldowns.push_back({request.attack, request.cooldown_seconds}); }
        CombatEvent event;
        event.kind = CombatEventKind::AttackFired;
        event.source = request.source;
        event.status = request.attack;
        result.sequence = state.record(event);
        result.fired = true;
        increment(state.counters.attacks);
        for (auto& plan : plans) {
            result.targets.push_back(plan.result ? state.commit(std::move(plan)) : plan.result);
        }
        return result;
    } catch (const std::bad_alloc&) { return reject(CombatError::AllocationFailure); }
}

void CombatService::update(const core::Frame& frame) {
    auto& state = *impl_;
    if (state.busy) { return; }
    BusyGuard guard(state.busy);
    state.tick = frame.tick;
    const double delta = std::isfinite(frame.delta_seconds) && frame.delta_seconds >= 0.0
        ? std::min(frame.delta_seconds, state.limits.maximum_delta_seconds) : 0.0;
    if (std::isfinite(frame.delta_seconds) && frame.delta_seconds > delta) {
        state.counters.dropped_seconds = std::min(stat_limit, state.counters.dropped_seconds + frame.delta_seconds - delta);
    }
    for (auto actor = state.actors.begin(); actor != state.actors.end();) {
        const auto* component = state.component(actor->first);
        if (!component) { actor = state.actors.erase(actor); continue; }
        if (!component_valid(*component) || !state.registry.enabled(actor->first)) { ++actor; continue; }
        if (component->defeated) { actor->second.statuses.clear(); }
        for (auto& value : actor->second.cooldowns) { value.remaining_seconds = std::max(0.0, value.remaining_seconds - delta); }
        std::erase_if(actor->second.cooldowns, [](const auto& value) { return value.remaining_seconds == 0.0; });
        for (auto& status : actor->second.statuses) {
            const double active = std::min(delta, status.remaining_seconds);
            status.remaining_seconds -= active;
            if (status.specification.kind <= StatusKind::ShieldOverTime) { status.until_tick_seconds -= active; }
        }
        ++actor;
    }
    std::size_t budget = state.limits.periodic_ticks_per_frame;
    auto actor = state.actors.upper_bound(state.periodic_cursor);
    for (std::size_t visited = 0; visited < state.actors.size() && budget != 0; ++visited) {
        if (actor == state.actors.end()) { actor = state.actors.begin(); }
        state.periodic_cursor = actor->first;
        if (state.actor_error(actor->first) != CombatError::None) { ++actor; continue; }
        auto& statuses = actor->second.statuses;
        for (auto status = statuses.begin(); status != statuses.end();) {
            const bool stale = status->source && !state.registry.alive(status->source);
            const bool periodic = status->specification.kind <= StatusKind::ShieldOverTime;
            while (!stale && periodic && status->until_tick_seconds <= 0.0 && budget != 0) {
                --budget;
                if (status->tick_sequence == counter_limit) { status->remaining_seconds = 0.0; status->until_tick_seconds = 1.0; break; }
                CombatError error{CombatError::None};
                try {
                    const double magnitude = status->specification.magnitude * static_cast<double>(status->stacks);
                    if (status->specification.kind == StatusKind::DamageOverTime) {
                        DamageRequest request;
                        request.source = status->source;
                        request.target = actor->first;
                        request.damage.type = status->specification.damage_type;
                        request.damage.amount = magnitude;
                        request.seed = mix(status->specification.id ^ status->tick_sequence ^ status->source.generation);
                        if (const auto position = state.scene.local_to_world(actor->first, {})) { request.point = *position; }
                        auto plan = state.prepare(request);
                        error = plan.result.error;
                        if (plan.result) { static_cast<void>(state.commit(std::move(plan))); }
                    } else {
                        const auto pool = status->specification.kind == StatusKind::ShieldOverTime ? HealPool::Shield : HealPool::Health;
                        error = state.heal({status->source, actor->first, magnitude, pool}).error;
                    }
                } catch (const std::bad_alloc&) { error = CombatError::AllocationFailure; }
                if (error == CombatError::Capacity || error == CombatError::Exhausted || error == CombatError::AllocationFailure) { break; }
                ++status->tick_sequence;
                status->until_tick_seconds += status->specification.interval_seconds;
                if (state.component(actor->first)->defeated) { break; }
            }
            if (stale || (status->remaining_seconds == 0.0 && (!periodic || status->until_tick_seconds > 0.0))) {
                if (!state.room(1)) { ++status; continue; }
                CombatEvent event;
                event.kind = stale ? CombatEventKind::StatusRemoved : CombatEventKind::StatusExpired;
                event.source = status->source;
                event.target = actor->first;
                event.status = status->specification.id;
                static_cast<void>(state.record(event));
                status = statuses.erase(status);
            } else {
                if (periodic && status->until_tick_seconds <= 0.0) { increment(state.counters.deferred_periodic_ticks); }
                ++status;
            }
            if (state.component(actor->first)->defeated) { statuses.clear(); break; }
        }
        ++actor;
    }
    state.deliver();
}

CombatHistory CombatService::events_since(std::uint64_t cursor, std::size_t limit) const {
    const auto& state = *impl_;
    CombatHistory result;
    result.next_cursor = cursor;
    result.newest_sequence = state.sequence;
    const auto oldest = state.history.empty() ? state.sequence : state.history[state.history_begin].sequence - 1;
    result.history_lost = cursor < oldest || cursor > state.sequence;
    if (cursor > state.sequence) { result.next_cursor = state.sequence; return result; }
    result.events.reserve(std::min(limit, state.history.size()));
    for (std::size_t index = 0; index < state.history.size(); ++index) {
        const auto& event = state.history[(state.history_begin + index) % state.history.size()];
        if (event.sequence <= cursor) { continue; }
        if (result.events.size() == limit) { result.truncated = true; break; }
        result.events.push_back(event);
        result.next_cursor = event.sequence;
    }
    return result;
}

CombatStatistics CombatService::statistics() const noexcept {
    auto result = impl_->counters;
    result.actors = impl_->actors.size();
    result.pending_defeats = impl_->outbox.size();
    result.mission_delivery_enabled = impl_->mission.producer != 0;
    return result;
}

CombatMissionCursor CombatService::mission_cursor() const noexcept { return impl_->mission; }

CombatResult CombatService::set_mission_cursor(CombatMissionCursor cursor) {
    if (impl_->busy || !impl_->outbox.empty()) { return {CombatError::Busy}; }
    if (cursor.next_sequence == 0 ||
        (cursor.producer == impl_->mission.producer && cursor.next_sequence < impl_->mission.next_sequence)) {
        return {CombatError::InvalidArgument};
    }
    impl_->mission = cursor;
    return {};
}

const CombatLimits& CombatService::limits() const noexcept { return impl_->limits; }

void register_combat(core::Kernel& kernel) {
    static_cast<void>(kernel.register_module({"gameplay.combat", "Bounded actor damage, primitive attacks and timed effects", 1}));
    auto* registry = kernel.services().find<ecs::Registry>();
    auto* scene = kernel.services().find<scene::Scene>();
    if (!registry || !scene) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "gameplay.combat", "Registry and Scene are required");
        return;
    }
    CombatMissionCursor cursor;
    cursor.producer = static_cast<std::uint64_t>(std::max(std::int64_t{0}, kernel.config().integer("combat.mission_producer", 0)));
    cursor.next_sequence = static_cast<std::uint64_t>(std::max(std::int64_t{1}, kernel.config().integer("combat.mission_next_sequence", 1)));
    auto* service = &kernel.services().emplace<CombatService>(kernel, *registry, *scene, CombatLimits{}, cursor);
    static_cast<void>(kernel.add_system({"gameplay.combat.update", core::Phase::PostSimulation, 2000, {}},
        [service](core::Kernel& host, const core::Frame& frame) {
            service->update(frame);
            const auto statistics = service->statistics();
            host.metric("combat.actors", static_cast<double>(statistics.actors));
            host.metric("combat.pending_defeats", static_cast<double>(statistics.pending_defeats));
            host.metric("combat.mission_delivery_enabled", statistics.mission_delivery_enabled ? 1.0 : 0.0);
            host.metric("combat.mission_failures", static_cast<double>(statistics.mission_failures));
            host.metric("combat.mission_disabled_credits", static_cast<double>(statistics.mission_disabled_credits));
        }));
    const auto report = [](CombatResult result) {
        return core::CommandResult{static_cast<bool>(result), std::string(describe(result.error))};
    };
    static_cast<void>(kernel.add_command("combat.attach", [service, registry, report](core::Kernel&, core::CommandArgs args) {
        if (args.empty() || args.size() > 4) { return core::CommandResult{false, "usage: combat.attach actor [health] [shield] [faction]"}; }
        CombatantConfig config;
        std::uint64_t faction{};
        if ((args.size() > 1 && !parse_number(args[1], config.maximum_health)) ||
            (args.size() > 2 && !parse_number(args[2], config.maximum_shield)) ||
            (args.size() > 3 && (!parse_integer(args[3], faction) || faction >= 64))) {
            return core::CommandResult{false, "invalid health, shield or faction"};
        }
        config.faction = static_cast<std::uint32_t>(faction);
        return report(service->attach(resolve(*registry, args[0]), std::move(config)));
    }, "Attach combat state to an existing Registry actor"));
    static_cast<void>(kernel.add_command("combat.damage", [service, registry, scene](core::Kernel&, core::CommandArgs args) {
        if (args.size() < 3 || args.size() > 5) { return core::CommandResult{false, "usage: combat.damage source|- target amount [type] [seed]"}; }
        DamageRequest request;
        request.source = args[0] == "-" ? ecs::Entity{} : resolve(*registry, args[0]);
        request.target = resolve(*registry, args[1]);
        const auto type = args.size() > 3 ? parse_damage(args[3]) : std::optional{DamageType::Physical};
        if ((args[0] != "-" && !request.source) || !parse_number(args[2], request.damage.amount) || !type ||
            (args.size() > 4 && !parse_integer(args[4], request.seed))) { return core::CommandResult{false, "invalid source, amount, type or seed"}; }
        request.damage.type = *type;
        if (const auto point = scene->local_to_world(request.target, {})) { request.point = *point; }
        const auto result = service->damage(request);
        std::ostringstream text;
        text << describe(result.error) << " health_damage=" << result.health_damage << " shield_damage=" << result.shield_damage;
        return core::CommandResult{static_cast<bool>(result), text.str()};
    }, "Resolve seeded typed damage with explicit actor attribution"));
    static_cast<void>(kernel.add_command("combat.heal", [service, registry, report](core::Kernel&, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 3) { return core::CommandResult{false, "usage: combat.heal target amount [shield]"}; }
        HealRequest request;
        request.target = resolve(*registry, args[0]);
        if (!parse_number(args[1], request.amount) || (args.size() == 3 && args[2] != "shield")) { return core::CommandResult{false, "invalid healing arguments"}; }
        request.pool = args.size() == 3 ? HealPool::Shield : HealPool::Health;
        return report(service->heal(request));
    }, "Restore bounded health or shield without reviving"));
    static_cast<void>(kernel.add_command("combat.revive", [service, registry, report](core::Kernel&, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) { return core::CommandResult{false, "usage: combat.revive actor [health_fraction]"}; }
        double fraction{1.0};
        if (args.size() == 2 && !parse_number(args[1], fraction)) { return core::CommandResult{false, "invalid health fraction"}; }
        return report(service->revive(resolve(*registry, args[0]), fraction));
    }, "Start an explicit new actor life and clear effects/cooldowns"));
    static_cast<void>(kernel.add_command("combat.status", [service, registry, report](core::Kernel&, core::CommandArgs args) {
        if (args.size() < 6 || args.size() > 7) { return core::CommandResult{false, "usage: combat.status target source|- id kind[0..5] magnitude duration [interval]"}; }
        StatusSpec status;
        std::uint64_t kind{};
        const auto source = args[1] == "-" ? ecs::Entity{} : resolve(*registry, args[1]);
        if ((args[1] != "-" && !source) || !parse_integer(args[2], status.id) || !parse_integer(args[3], kind) || kind > 5 ||
            !parse_number(args[4], status.magnitude) || !parse_number(args[5], status.duration_seconds) ||
            (args.size() == 7 && !parse_number(args[6], status.interval_seconds))) { return core::CommandResult{false, "invalid status fields"}; }
        status.kind = static_cast<StatusKind>(kind);
        return report(service->apply_status(resolve(*registry, args[0]), source, status));
    }, "Apply damage/heal/shield periodic, vulnerability, armor-break or stun status"));
    for (const auto kind : {AttackKind::Ray, AttackKind::MeleeSphere}) {
        const std::string name = kind == AttackKind::Ray ? "combat.fire" : "combat.melee";
        static_cast<void>(kernel.add_command(name, [service, registry, scene, kind](core::Kernel& host, core::CommandArgs args) {
            if (args.size() < 4 || args.size() > 5) { return core::CommandResult{false, "usage: combat.fire|melee actor damage range|radius seed [ammo_definition]"}; }
            AttackRequest request;
            request.source = resolve(*registry, args[0]);
            request.kind = kind;
            if (!parse_number(args[1], request.damage.amount) || !parse_number(args[2], request.range) || !parse_integer(args[3], request.seed)) {
                return core::CommandResult{false, "invalid attack fields"};
            }
            const auto pose = scene->world_transform(request.source);
            if (!pose || !scene->world_transform_exact(request.source)) { return core::CommandResult{false, "actor needs an exact Scene transform"}; }
            request.ray = {pose->position, math::rotate(pose->rotation, {0.0, 0.0, -1.0})};
            request.radius = request.range;
            if (args.size() == 5) {
                const auto* inventory = host.services().find<InventoryService>();
                const auto* item = inventory ? inventory->definition(args[4]) : nullptr;
                if (!item) { return core::CommandResult{false, "unknown ammunition definition"}; }
                request.costs[0].item = item->id;
                request.cost_count = 1;
            }
            const auto result = service->attack(request);
            return core::CommandResult{result.fired, std::string(describe(result.error)) + " targets=" + std::to_string(result.targets.size())};
        }, "Fire from an actor Scene pose; optional ammo spends one unreserved owned item"));
    }
    static_cast<void>(kernel.add_command("combat.inspect", [service, registry](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "usage: combat.inspect actor"}; }
        const auto snapshot = service->inspect(resolve(*registry, args[0]));
        if (!snapshot) { return core::CommandResult{false, "combat actor unavailable"}; }
        std::ostringstream text;
        text << "health=" << snapshot->combatant.health << " shield=" << snapshot->combatant.shield
             << " defeated=" << snapshot->combatant.defeated << " can_act=" << snapshot->can_act
             << " effects=" << snapshot->statuses.size() << " cooldowns=" << snapshot->cooldowns.size();
        return core::CommandResult{true, text.str()};
    }, "Inspect real combat component and timed state"));
    static_cast<void>(kernel.add_command("combat.mission_cursor", [service, report](core::Kernel&, core::CommandArgs args) {
        if (args.empty()) {
            const auto current = service->mission_cursor();
            return core::CommandResult{true, "producer=" + std::to_string(current.producer) + " next_sequence=" + std::to_string(current.next_sequence)};
        }
        CombatMissionCursor next;
        if (args.size() != 2 || !parse_integer(args[0], next.producer) || !parse_integer(args[1], next.next_sequence)) {
            return core::CommandResult{false, "usage: combat.mission_cursor [producer next_sequence]"};
        }
        return report(service->set_mission_cursor(next));
    }, "Read/configure mission lineage cursor; producer zero disables credit delivery"));
}

} // namespace ge::gameplay
