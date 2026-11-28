#include <ge/gameplay/interaction.hpp>

#include <ge/core/runtime.hpp>
#include <ge/input/actions.hpp>
#include <ge/physics/character.hpp>
#include <ge/physics/vehicle.hpp>
#include <ge/render/camera.hpp>
#include <ge/scene/scene.hpp>
#include <ge/script/vm.hpp>
#include <ge/spatial/index.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::gameplay {
namespace {

constexpr double epsilon = 1.0e-6;
constexpr std::size_t string_limit = 1024;

bool bounded(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool point_valid(math::Vec3 point) noexcept {
    return math::finite(point) && math::length_squared(point) <= 1.0e24;
}

bool actor_valid(const InteractionActorComponent& actor) noexcept {
    return point_valid(actor.eye_offset) && point_valid(actor.local_forward) &&
        math::length_squared(actor.local_forward) > epsilon * epsilon &&
        bounded(actor.range, epsilon, 1.0e6) && bounded(actor.cone_cosine, -1.0, 1.0) &&
        bounded(actor.focus_bias, 0.0, 1.0);
}

bool target_valid(const InteractableComponent& target) noexcept {
    return target.prompt.size() <= 256 && point_valid(target.local_point) &&
        bounded(target.range, epsilon, 1.0e6) &&
        bounded(target.hold_seconds, epsilon, 3600.0) &&
        bounded(target.repeat_seconds, epsilon, 3600.0) &&
        bounded(target.cooldown_seconds, 0.0, 3600.0) &&
        bounded(target.trigger_radius, 0.0, 1.0e6) &&
        bounded(target.trigger_hysteresis, 0.0, 1000.0) &&
        target.mode <= InteractionMode::Repeat;
}

bool name_valid(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) { return false; }
    return std::all_of(name.begin(), name.end(), [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
    });
}

bool action_valid(const InteractionAction& action) noexcept {
    constexpr auto allowed = interaction_event_mask(InteractionEventKind::FocusEnter) |
        interaction_event_mask(InteractionEventKind::FocusLeave) |
        interaction_event_mask(InteractionEventKind::HoldStarted) |
        interaction_event_mask(InteractionEventKind::Cancelled) |
        interaction_event_mask(InteractionEventKind::Activated) |
        interaction_event_mask(InteractionEventKind::TriggerEnter) |
        interaction_event_mask(InteractionEventKind::TriggerLeave);
    if (action.kind > InteractionActionKind::SelectVehicle ||
        action.subject > InteractionSubject::Explicit || !point_valid(action.vector) ||
        action.name.size() > string_limit || action.arguments.size() > 16 ||
        action.events == 0 || (action.events & ~allowed) != 0) { return false; }
    if ((action.kind == InteractionActionKind::Native ||
         action.kind == InteractionActionKind::Command ||
         action.kind == InteractionActionKind::Script) && !name_valid(action.name)) { return false; }
    if ((action.kind == InteractionActionKind::AddTag ||
         action.kind == InteractionActionKind::RemoveTag) &&
        (action.name.empty() || action.name.size() > 256)) { return false; }
    for (const auto& argument : action.arguments) {
        if (argument.size() > string_limit || argument.find('\0') != std::string::npos) { return false; }
    }
    return true;
}

bool better(const InteractionCandidate& first, const InteractionCandidate& second) noexcept {
    if (first.priority != second.priority) { return first.priority > second.priority; }
    if (first.score != second.score) { return first.score > second.score; }
    return first.entity < second.entity;
}

template<class T>
bool parse_number(std::string_view text, T& output) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

struct BusyScope {
    bool& busy;
    explicit BusyScope(bool& flag) : busy(flag) { busy = true; }
    ~BusyScope() { busy = false; }
};

} // namespace

std::string_view describe(InteractionError value) noexcept {
    switch (value) {
    case InteractionError::None: return "ok";
    case InteractionError::InvalidEntity: return "invalid entity";
    case InteractionError::InvalidSettings: return "invalid interaction settings";
    case InteractionError::Capacity: return "interaction capacity exhausted";
    case InteractionError::Busy: return "interaction dispatch is busy";
    case InteractionError::MissingActor: return "actor is not attached";
    case InteractionError::MissingTarget: return "target is not attached";
    case InteractionError::Disabled: return "interaction is disabled";
    case InteractionError::Context: return "interaction context or flags do not match";
    case InteractionError::Layers: return "interaction layers do not match";
    case InteractionError::OutOfRange: return "target is out of range";
    case InteractionError::OutsideCone: return "target is outside the aim cone";
    case InteractionError::Obstructed: return "line of sight is obstructed";
    case InteractionError::IncompleteQuery: return "line of sight query is incomplete";
    case InteractionError::Cooldown: return "target is cooling down";
    case InteractionError::Locked: return "target is reserved or executing";
    case InteractionError::AlreadyUsed: return "target has already been used";
    case InteractionError::NoActions: return "target has no activation actions";
    case InteractionError::Released: return "interaction button released";
    case InteractionError::FocusLost: return "interaction focus lost";
    case InteractionError::ActionFailed: return "interaction action failed";
    case InteractionError::MissingService: return "required service is unavailable";
    case InteractionError::Budget: return "interaction frame budget exhausted";
    case InteractionError::Stale: return "interaction ownership is stale";
    }
    return "unknown interaction error";
}

std::string_view describe(InteractionEventKind value) noexcept {
    switch (value) {
    case InteractionEventKind::FocusEnter: return "focus.enter";
    case InteractionEventKind::FocusLeave: return "focus.leave";
    case InteractionEventKind::HoldStarted: return "hold.started";
    case InteractionEventKind::Cancelled: return "cancelled";
    case InteractionEventKind::ActivationStarted: return "activation.started";
    case InteractionEventKind::Activated: return "activated";
    case InteractionEventKind::Rejected: return "rejected";
    case InteractionEventKind::TriggerEnter: return "trigger.enter";
    case InteractionEventKind::TriggerLeave: return "trigger.leave";
    case InteractionEventKind::ActionSucceeded: return "action.succeeded";
    case InteractionEventKind::ActionFailed: return "action.failed";
    }
    return "unknown";
}

std::string_view describe(InteractionActionKind value) noexcept {
    switch (value) {
    case InteractionActionKind::Notify: return "notify";
    case InteractionActionKind::Native: return "native";
    case InteractionActionKind::Command: return "command";
    case InteractionActionKind::Script: return "script";
    case InteractionActionKind::SetEnabled: return "enable";
    case InteractionActionKind::ToggleEnabled: return "toggle";
    case InteractionActionKind::AddTag: return "tag.add";
    case InteractionActionKind::RemoveTag: return "tag.remove";
    case InteractionActionKind::Translate: return "translate";
    case InteractionActionKind::Impulse: return "impulse";
    case InteractionActionKind::SelectCharacter: return "character";
    case InteractionActionKind::SelectVehicle: return "vehicle";
    }
    return "unknown";
}

struct InteractionService::Impl {
    struct Actor {
        InteractionActorState state;
        std::uint64_t instance{};
        std::set<ecs::Entity> inside;
        bool desired_down{};
        bool manual{};
        bool blocked_until_release{};
    };
    struct Target {
        InteractionTargetState state;
        std::uint64_t instance{};
        std::uint64_t actions_instance{};
        bool inflight{};
    };
    struct Batch {
        ecs::Entity actor{};
        ecs::Entity target{};
        InteractionEventKind event{InteractionEventKind::Activated};
        std::uint64_t activation{};
        std::vector<InteractionAction> actions;
    };
    core::Kernel& kernel;
    scene::Scene& scene;
    ecs::Registry& registry;
    InteractionLimits limits;
    InteractionStatistics stats;
    std::map<ecs::Entity, Actor> actors;
    std::map<ecs::Entity, Target> targets;
    std::map<std::string, InteractionCallback, std::less<>> callbacks;
    std::deque<Batch> pending;
    std::deque<InteractionEvent> history;
    std::vector<physics::ColliderHandle> colliders;
    std::vector<spatial::RayHit> ray_hits;
    ecs::Entity selected{};
    ecs::Entity input_actor{};
    InteractionError error{InteractionError::None};
    std::uint64_t sequence{};
    std::uint64_t activation{};
    std::uint64_t tick{};
    bool has_tick{};
    bool busy{};

    Impl(core::Kernel& host, scene::Scene& world, InteractionLimits configuration)
        : kernel(host), scene(world), registry(world.registry()), limits(configuration) {
        if (limits.actors == 0 || limits.actors > 1024 || limits.targets == 0 ||
            limits.targets > 65536 || limits.actions_per_target == 0 ||
            limits.actions_per_target > 64 || limits.callbacks > 4096 ||
            limits.pending_batches == 0 || limits.pending_batches > 8192 ||
            limits.history == 0 || limits.history > 65536 || limits.trigger_pairs > 65536 ||
            limits.query_candidates == 0 || limits.query_candidates > 16384 ||
            limits.evaluations_per_frame == 0 || limits.evaluations_per_frame > 1048576 ||
            limits.actions_per_frame < limits.actions_per_target ||
            limits.actions_per_frame > 65536 ||
            !bounded(limits.maximum_delta_seconds, epsilon, 1.0)) {
            throw std::invalid_argument("invalid interaction limits");
        }
        colliders.resize(limits.query_candidates);
        ray_hits.resize(limits.query_candidates);
    }

    bool fail(InteractionError reason) { error = reason; return false; }
    bool editable() {
        error = InteractionError::None;
        return !busy && !registry.iterating() ? true : fail(InteractionError::Busy);
    }
    bool enrolled(ecs::Entity entity) const {
        return registry.alive(entity) && scene.contains(entity);
    }
    bool owns_actor(ecs::Entity entity) const {
        const auto found = actors.find(entity);
        return found != actors.end() && enrolled(entity) && found->second.instance != 0 &&
            registry.component_instance<InteractionActorComponent>(entity) == found->second.instance;
    }
    bool owns_target(ecs::Entity entity) const {
        const auto found = targets.find(entity);
        return found != targets.end() && enrolled(entity) && found->second.instance != 0 &&
            registry.component_instance<InteractableComponent>(entity) == found->second.instance &&
            registry.component_instance<InteractionActionsComponent>(entity) == found->second.actions_instance;
    }
    void remove_components(ecs::Entity entity, const Actor& actor) {
        if (registry.component_instance<InteractionActorComponent>(entity) == actor.instance) {
            (void)registry.remove<InteractionActorComponent>(entity);
        }
    }
    void remove_components(ecs::Entity entity, const Target& target) {
        if (registry.component_instance<InteractableComponent>(entity) == target.instance) {
            (void)registry.remove<InteractableComponent>(entity);
        }
        if (registry.component_instance<InteractionActionsComponent>(entity) == target.actions_instance) {
            (void)registry.remove<InteractionActionsComponent>(entity);
        }
    }
    math::Vec3 point(ecs::Entity entity) {
        const auto* settings = registry.get<InteractableComponent>(entity);
        const auto position = scene.local_to_world(entity, settings ? settings->local_point : math::Vec3{});
        return position.value_or(math::Vec3{});
    }
    void emit(InteractionEventKind kind, ecs::Entity actor, ecs::Entity target,
              InteractionError reason = InteractionError::None, std::uint64_t serial = 0,
              std::size_t index = 0, std::string name = {}, std::string message = {}) {
        if (sequence == std::numeric_limits<std::uint64_t>::max()) { return; }
        if (history.size() == limits.history) {
            history.pop_front();
            ++stats.overwritten_events;
        }
        name.resize(std::min(name.size(), string_limit));
        message.resize(std::min(message.size(), string_limit));
        history.push_back({++sequence, tick, serial, kind, reason, actor, target,
                           point(target), index, std::move(name), std::move(message)});
    }
    InteractionError filter(ecs::Entity actor, ecs::Entity target,
                            const InteractionActorComponent& a, const InteractableComponent& t) {
        if (!enrolled(actor) || !enrolled(target) || actor == target) { return InteractionError::InvalidEntity; }
        if (!actor_valid(a) || !target_valid(t)) { return InteractionError::InvalidSettings; }
        if (!a.enabled || !t.enabled || !scene.effective_enabled(actor) ||
            !scene.effective_enabled(target)) { return InteractionError::Disabled; }
        if ((a.contexts & t.contexts) == 0 || (a.flags & t.required_flags) != t.required_flags ||
            (a.flags & t.forbidden_flags) != 0) { return InteractionError::Context; }
        if ((registry.layers(target) & a.target_layers) == 0 ||
            (registry.layers(actor) & t.actor_layers) == 0) { return InteractionError::Layers; }
        return InteractionError::None;
    }
    bool ignored(ecs::Entity entity, ecs::Entity actor, ecs::Entity target) const {
        return entity == actor || entity == target || scene.is_ancestor(actor, entity) ||
            scene.is_ancestor(target, entity);
    }
    InteractionError visible(ecs::Entity actor, ecs::Entity target, math::Vec3 start,
                             math::Vec3 finish, std::uint64_t layers) {
        const auto distance = math::length(finish - start);
        if (distance <= epsilon || layers == 0) { return InteractionError::None; }
        const math::Ray ray{start, (finish - start) / distance};
        if (auto* collision = kernel.services().find<physics::CollisionWorld>()) {
            const math::Aabb box{math::min(start, finish) - math::Vec3{epsilon, epsilon, epsilon},
                                 math::max(start, finish) + math::Vec3{epsilon, epsilon, epsilon}};
            const auto report = collision->query(box, colliders, {~std::uint64_t{0}, layers, 0}, actor);
            if (report.truncated || report.invalid_input) {
                ++stats.incomplete_queries;
                return InteractionError::IncompleteQuery;
            }
            for (std::size_t i = 0; i < report.written; ++i) {
                const auto* collider = collision->get(colliders[i]);
                if (!collider || !collider->active || collider->description.trigger ||
                    ignored(collider->entity, actor, target)) { continue; }
                const bool hit = std::visit([&](const auto& shape) {
                    return math::intersect(ray, shape, 0.0, distance - epsilon).has_value();
                }, collider->world_shape);
                if (hit) { return InteractionError::Obstructed; }
            }
            return InteractionError::None;
        }
        if (auto* index = kernel.services().find<spatial::SpatialIndex>()) {
            spatial::QueryFilter query;
            query.layers = layers;
            query.exclude = actor;
            const auto report = index->raycast(ray, distance - epsilon, ray_hits, query);
            if (report.truncated || report.invalid_input || report.stale || report.traversal_exhausted) {
                ++stats.incomplete_queries;
                return InteractionError::IncompleteQuery;
            }
            for (std::size_t i = 0; i < report.written; ++i) {
                if (!ignored(ray_hits[i].entity, actor, target)) { return InteractionError::Obstructed; }
            }
            return InteractionError::None;
        }
        return InteractionError::MissingService;
    }
    InteractionCandidate evaluate(ecs::Entity actor, ecs::Entity target) {
        InteractionCandidate result;
        result.entity = target;
        if (stats.evaluations >= limits.evaluations_per_frame) {
            stats.budget_exhausted = true;
            result.error = InteractionError::Budget;
            return result;
        }
        ++stats.evaluations;
        const auto* a = registry.get<InteractionActorComponent>(actor);
        const auto* t = registry.get<InteractableComponent>(target);
        if (!a || !t || !owns_actor(actor) || !owns_target(target)) {
            result.error = InteractionError::Stale;
            return result;
        }
        result.error = filter(actor, target, *a, *t);
        if (result.error != InteractionError::None) { return result; }
        const auto eye = scene.local_to_world(actor, a->eye_offset);
        const auto destination = scene.local_to_world(target, t->local_point);
        auto forward = scene.transform_direction(actor, a->local_forward);
        if (!eye || !destination || !forward) { result.error = InteractionError::Stale; return result; }
        auto aim_origin = *eye;
        if (a->camera_aim && actor == input_actor) {
            const auto* cameras = kernel.services().find<render::CameraService>();
            const auto* view = cameras ? cameras->current_view() : nullptr;
            if (view && !view->stale) {
                const auto camera_origin = scene.local_to_world(view->entity, {});
                const auto camera_forward = scene.transform_direction(view->entity, {0.0, 0.0, -1.0});
                if (camera_origin && camera_forward) { aim_origin = *camera_origin; forward = camera_forward; }
            }
        }
        result.point = *destination;
        result.priority = t->priority;
        result.distance = math::length(*destination - *eye);
        const double range = std::min(a->range, t->range);
        if (result.distance > range) { result.error = InteractionError::OutOfRange; return result; }
        const auto aim = math::normalized(*destination - aim_origin, math::normalized(*forward));
        result.alignment = math::dot(math::normalized(*forward), aim);
        if (result.alignment < a->cone_cosine) { result.error = InteractionError::OutsideCone; return result; }
        if (t->require_line_of_sight) {
            result.error = visible(actor, target, *eye, *destination, a->obstruction_layers);
        }
        result.score = result.alignment * 0.8 + (1.0 - result.distance / range) * 0.2;
        if (actors.at(actor).state.focus == target) { result.score += a->focus_bias; }
        return result;
    }
    InteractionError ready(ecs::Entity actor, ecs::Entity target) const {
        const auto found = targets.find(target);
        const auto* config = registry.get<InteractableComponent>(target);
        if (!owns_actor(actor) || !owns_target(target) || !config) { return InteractionError::Stale; }
        const auto& value = found->second;
        if (value.inflight || (config->exclusive && value.state.reserved_by.valid() &&
            value.state.reserved_by != actor)) { return InteractionError::Locked; }
        if (config->once && value.state.used) { return InteractionError::AlreadyUsed; }
        if (value.state.cooldown_remaining > 0.0) { return InteractionError::Cooldown; }
        return InteractionError::None;
    }
    bool enqueue(ecs::Entity actor, ecs::Entity target, InteractionEventKind event,
                 std::uint64_t serial = 0) {
        if (!owns_actor(actor) || !owns_target(target)) {
            return event != InteractionEventKind::Activated || fail(InteractionError::Stale);
        }
        const auto* component = registry.get<InteractionActionsComponent>(target);
        if (!component || component->actions.size() > limits.actions_per_target) {
            return event != InteractionEventKind::Activated || fail(InteractionError::NoActions);
        }
        Batch batch{actor, target, event, serial, {}};
        for (const auto& action : component->actions) {
            if (action.enabled && (action.events & interaction_event_mask(event)) != 0) {
                if (!action_valid(action)) { return fail(InteractionError::InvalidSettings); }
                batch.actions.push_back(action);
            }
        }
        if (batch.actions.empty()) {
            return event != InteractionEventKind::Activated || fail(InteractionError::NoActions);
        }
        if (pending.size() >= limits.pending_batches) { return fail(InteractionError::Capacity); }
        pending.push_back(std::move(batch));
        return true;
    }
    void transition(InteractionEventKind event, ecs::Entity actor, ecs::Entity target,
                    InteractionError reason = InteractionError::None) {
        emit(event, actor, target, reason);
        if (!enqueue(actor, target, event)) {
            emit(InteractionEventKind::ActionFailed, actor, target, error);
        }
    }
    void release(Actor& actor, InteractionError reason) {
        const auto target = actor.state.holding;
        if (target.valid()) {
            const auto found = targets.find(target);
            if (found != targets.end() && found->second.state.reserved_by == actor.state.entity) {
                found->second.state.reserved_by = {};
            }
            transition(InteractionEventKind::Cancelled, actor.state.entity, target, reason);
            abandon(actor.state.entity, {}, reason);
        }
        actor.state.holding = {};
        actor.state.held_seconds = 0.0;
        actor.state.progress = 0.0;
        actor.state.repeat_remaining = 0.0;
    }
    void change_focus(Actor& actor, ecs::Entity target) {
        if (actor.state.focus == target) { return; }
        release(actor, InteractionError::FocusLost);
        abandon(actor.state.entity, {}, InteractionError::FocusLost);
        if (actor.state.focus.valid()) {
            transition(InteractionEventKind::FocusLeave, actor.state.entity, actor.state.focus);
        }
        actor.state.focus = target;
        if (target.valid()) { transition(InteractionEventKind::FocusEnter, actor.state.entity, target); }
        if (actor.state.button_down) { actor.blocked_until_release = true; }
    }
    void choose(Actor& actor) {
        std::optional<InteractionCandidate> best;
        for (const auto& [entity, runtime] : targets) {
            (void)runtime;
            auto candidate = evaluate(actor.state.entity, entity);
            if (candidate.error == InteractionError::Budget) { return; }
            if (candidate.error == InteractionError::None && (!best || better(candidate, *best))) {
                best = candidate;
            }
        }
        change_focus(actor, best ? best->entity : ecs::Entity{});
    }
    void reject(Actor& actor, InteractionError reason) {
        actor.state.last_error = reason;
        actor.state.activated_this_press = true;
        actor.blocked_until_release = true;
        ++stats.rejected;
        emit(InteractionEventKind::Rejected, actor.state.entity, actor.state.focus, reason);
        release(actor, reason);
    }
    void activate(Actor& actor) {
        const auto entity = actor.state.focus;
        const auto candidate = evaluate(actor.state.entity, entity);
        auto reason = candidate.error;
        if (reason == InteractionError::None) { reason = ready(actor.state.entity, entity); }
        if (reason != InteractionError::None) { reject(actor, reason); return; }
        if (activation == std::numeric_limits<std::uint64_t>::max()) {
            reject(actor, InteractionError::Capacity);
            return;
        }
        if (!enqueue(actor.state.entity, entity, InteractionEventKind::Activated, activation + 1)) {
            reject(actor, error);
            return;
        }
        auto& target = targets.at(entity);
        const auto config = *registry.get<InteractableComponent>(entity);
        target.inflight = true;
        target.state.cooldown_remaining = config.cooldown_seconds;
        actor.state.activated_this_press = true;
        actor.state.last_error = InteractionError::None;
        actor.state.repeat_remaining = config.repeat_seconds;
        emit(InteractionEventKind::ActivationStarted, actor.state.entity, entity,
             InteractionError::None, ++activation);
    }
    void buttons(Actor& actor, double delta) {
        const bool pressed = actor.desired_down && !actor.state.button_down;
        const bool released = !actor.desired_down && actor.state.button_down;
        actor.state.button_down = actor.desired_down;
        if (released) {
            release(actor, InteractionError::Released);
            actor.state.activated_this_press = false;
            actor.blocked_until_release = false;
        }
        if (!actor.desired_down) { actor.blocked_until_release = false; }
        if (actor.blocked_until_release && actor.desired_down) { return; }
        if (!actor.desired_down || !actor.state.focus.valid()) { return; }
        const auto* config = registry.get<InteractableComponent>(actor.state.focus);
        if (!config) { reject(actor, InteractionError::Stale); return; }
        const auto mode = config->mode;
        if (pressed) {
            actor.state.activated_this_press = false;
            const auto reason = ready(actor.state.entity, actor.state.focus);
            if (reason != InteractionError::None) { reject(actor, reason); return; }
            if (mode == InteractionMode::Press || mode == InteractionMode::Repeat) { activate(actor); }
            else {
                actor.state.holding = actor.state.focus;
                if (config->exclusive) { targets.at(actor.state.focus).state.reserved_by = actor.state.entity; }
                transition(InteractionEventKind::HoldStarted, actor.state.entity, actor.state.focus);
            }
        }
        if (mode == InteractionMode::Hold && actor.state.holding.valid() &&
            !actor.state.activated_this_press) {
            const auto candidate = evaluate(actor.state.entity, actor.state.holding);
            if (candidate.error != InteractionError::None) { reject(actor, candidate.error); return; }
            actor.state.held_seconds = std::min(config->hold_seconds, actor.state.held_seconds + delta);
            actor.state.progress = actor.state.held_seconds / config->hold_seconds;
            if (actor.state.progress >= 1.0) { activate(actor); }
        }
        if (mode == InteractionMode::Repeat && !pressed && actor.state.activated_this_press) {
            actor.state.repeat_remaining = std::max(0.0, actor.state.repeat_remaining - delta);
            if (actor.state.repeat_remaining <= 0.0) {
                const auto reason = ready(actor.state.entity, actor.state.focus);
                if (reason == InteractionError::None) { activate(actor); }
                else if (reason != InteractionError::Cooldown && reason != InteractionError::Locked) { reject(actor, reason); }
            }
        }
    }
    void triggers(Actor& actor) {
        const auto* config = registry.get<InteractionActorComponent>(actor.state.entity);
        if (!config) { return; }
        const auto origin = scene.local_to_world(actor.state.entity, {});
        for (const auto& [entity, target] : targets) {
            (void)target;
            if (stats.evaluations >= limits.evaluations_per_frame) { stats.budget_exhausted = true; return; }
            ++stats.evaluations;
            const auto* settings = registry.get<InteractableComponent>(entity);
            const bool previous = actor.inside.contains(entity);
            const auto destination = settings ? scene.local_to_world(entity, settings->local_point) : std::nullopt;
            bool inside = settings && origin && destination && settings->trigger_radius > 0.0 &&
                filter(actor.state.entity, entity, *config, *settings) == InteractionError::None;
            if (inside) {
                const auto radius = settings->trigger_radius + (previous ? settings->trigger_hysteresis : 0.0);
                inside = math::length_squared(*destination - *origin) <= radius * radius;
            }
            if (inside == previous) { continue; }
            if (inside) {
                if (stats.trigger_pairs >= limits.trigger_pairs) { stats.budget_exhausted = true; continue; }
                actor.inside.insert(entity);
                ++stats.trigger_pairs;
                transition(InteractionEventKind::TriggerEnter, actor.state.entity, entity);
            } else {
                actor.inside.erase(entity);
                --stats.trigger_pairs;
                transition(InteractionEventKind::TriggerLeave, actor.state.entity, entity);
            }
        }
    }

    InteractionActionResult execute(const Batch& batch, const InteractionAction& action);
    void dispatch();
    void prune();
    void abandon(ecs::Entity actor, ecs::Entity target, InteractionError reason);
};

InteractionActionResult InteractionService::Impl::execute(const Batch& batch,
                                                         const InteractionAction& action) {
    const auto subject = action.subject == InteractionSubject::Actor ? batch.actor :
        action.subject == InteractionSubject::Explicit ? action.entity : batch.target;
    if (!registry.alive(subject)) { return {false, "action subject is no longer alive"}; }
    auto* dynamics = kernel.services().find<physics::PhysicsWorld>();
    switch (action.kind) {
    case InteractionActionKind::Notify:
        return {true, action.name};
    case InteractionActionKind::Native: {
        const auto found = callbacks.find(action.name);
        if (found == callbacks.end()) { return {false, "native callback is not registered"}; }
        auto callback = found->second;
        InteractionActionContext context{kernel, batch.actor, batch.target, subject,
                                         batch.event, point(batch.target), batch.activation, action};
        return callback(context);
    }
    case InteractionActionKind::Command: {
        std::string command = action.name;
        for (const auto& argument : action.arguments) {
            auto value = argument;
            if (value == "$actor") { value = ecs::format_entity(batch.actor); }
            else if (value == "$target") { value = ecs::format_entity(batch.target); }
            else if (value == "$subject") { value = ecs::format_entity(subject); }
            command += " \"";
            for (const auto character : value) {
                if (character == '"' || character == '\\') { command += '\\'; }
                command += character;
            }
            command += '"';
        }
        return {kernel.enqueue_command(std::move(command)), "deferred command admission"};
    }
    case InteractionActionKind::Script: {
        auto* scripts = kernel.services().find<script::ScriptService>();
        if (!scripts) { return {false, "script service is unavailable"}; }
        const std::array<script::Value, 3> arguments{
            script::Value(batch.actor), script::Value(batch.target), script::Value(action.vector)};
        std::string message;
        const auto instance = scripts->start(action.name, subject, arguments, &message);
        return {static_cast<bool>(instance), instance ? "script instance admitted" : std::move(message)};
    }
    case InteractionActionKind::SetEnabled:
        return {registry.set_enabled(subject, action.value), "entity enable state"};
    case InteractionActionKind::ToggleEnabled:
        return {registry.set_enabled(subject, !registry.enabled(subject)), "entity enable state toggled"};
    case InteractionActionKind::AddTag:
        return {registry.has_tag(subject, action.name) || registry.add_tag(subject, action.name), "tag added"};
    case InteractionActionKind::RemoveTag:
        return {!registry.has_tag(subject, action.name) || registry.remove_tag(subject, action.name), "tag removed"};
    case InteractionActionKind::Translate: {
        if (!scene.contains(subject)) { return {false, "subject is not in the scene"}; }
        if (dynamics) {
            if (const auto* body = dynamics->get(subject)) {
                auto pose = body->pose;
                pose.position = pose.position + action.vector;
                return {dynamics->set_pose(body->handle, pose), "physics pose translation"};
            }
        }
        if (registry.has<physics::BodyComponent>(subject)) { return {false, "physics ownership is stale"}; }
        for (const auto descendant : scene.descendants(subject)) {
            if (registry.has<physics::BodyComponent>(descendant)) {
                return {false, "translation would move a physics-owned descendant"};
            }
        }
        return {static_cast<bool>(scene.translate(subject, action.vector, scene::TransformSpace::World)),
                "scene world translation"};
    }
    case InteractionActionKind::Impulse:
        return {dynamics && dynamics->apply_impulse(dynamics->find(subject), action.vector), "physics impulse"};
    case InteractionActionKind::SelectCharacter: {
        auto* characters = kernel.services().find<physics::CharacterSystem>();
        const bool success = characters && characters->select_player(subject,
            kernel.services().find<render::CameraService>());
        if (success) {
            if (auto* vehicles = kernel.services().find<physics::VehicleSystem>()) { (void)vehicles->release_player(); }
        }
        return {success, "character input authority"};
    }
    case InteractionActionKind::SelectVehicle: {
        auto* vehicles = kernel.services().find<physics::VehicleSystem>();
        auto* input_system = kernel.services().find<input::InputSystem>();
        const bool success = vehicles && input_system && vehicles->select_player(vehicles->find(subject), *input_system);
        if (success) {
            if (auto* characters = kernel.services().find<physics::CharacterSystem>()) { (void)characters->release_player(); }
        }
        return {success, "vehicle input authority"};
    }
    }
    return {false, "unsupported action"};
}

void InteractionService::Impl::dispatch() {
    while (!pending.empty()) {
        if (pending.front().actions.size() > limits.actions_per_frame - stats.actions ||
            (pending.front().event == InteractionEventKind::Activated &&
             stats.evaluations >= limits.evaluations_per_frame)) {
            stats.budget_exhausted = true;
            break;
        }
        auto batch = std::move(pending.front());
        pending.pop_front();
        bool success = owns_actor(batch.actor) && owns_target(batch.target);
        auto failure = success ? InteractionError::None : InteractionError::Stale;
        if (success && batch.event == InteractionEventKind::Activated) {
            if (auto* collision = kernel.services().find<physics::CollisionWorld>()) { (void)collision->sync(scene); }
            failure = evaluate(batch.actor, batch.target).error;
            success = failure == InteractionError::None;
        }
        const bool admitted = success;
        for (std::size_t index = 0; admitted && index < batch.actions.size(); ++index) {
            const auto& action = batch.actions[index];
            InteractionActionResult result;
            try {
                result = owns_actor(batch.actor) && owns_target(batch.target)
                    ? execute(batch, action) : InteractionActionResult{false, "interaction ownership is stale"};
            } catch (const std::exception& exception) {
                result = {false, exception.what()};
            } catch (...) {
                result = {false, "native action threw an unknown exception"};
            }
            const bool current = owns_actor(batch.actor) && owns_target(batch.target);
            if (!current) { result = {false, "interaction ownership was removed or replaced"}; }
            ++stats.actions;
            emit(result.success ? InteractionEventKind::ActionSucceeded : InteractionEventKind::ActionFailed,
                 batch.actor, batch.target, result.success ? InteractionError::None :
                    current ? InteractionError::ActionFailed : InteractionError::Stale,
                 batch.activation, index, action.name, std::move(result.message));
            if (!result.success) {
                success = false;
                failure = current ? InteractionError::ActionFailed : InteractionError::Stale;
                if (action.stop_on_failure) { break; }
            }
        }
        if (batch.event != InteractionEventKind::Activated) { continue; }
        if (auto target = targets.find(batch.target); target != targets.end()) {
            target->second.inflight = false;
            target->second.state.reserved_by = {};
            if (success) { target->second.state.used = true; ++target->second.state.activations; }
        }
        if (auto actor = actors.find(batch.actor); actor != actors.end()) {
            actor->second.state.holding = {};
            actor->second.state.last_error = failure;
            if (!success) { actor->second.blocked_until_release = true; }
        }
        if (success) { ++stats.activations; }
        else { ++stats.rejected; }
        emit(success ? InteractionEventKind::Activated : InteractionEventKind::Rejected,
             batch.actor, batch.target, failure, batch.activation);
    }
}

void InteractionService::Impl::abandon(ecs::Entity actor, ecs::Entity target, InteractionError reason) {
    for (auto current = pending.begin(); current != pending.end();) {
        const bool matches = (actor.valid() && current->actor == actor) ||
            (target.valid() && current->target == target);
        if (!matches || (reason != InteractionError::Stale && current->event != InteractionEventKind::Activated)) {
            ++current;
            continue;
        }
        if (current->event == InteractionEventKind::Activated) {
            if (auto found = targets.find(current->target); found != targets.end()) {
                found->second.inflight = false;
                found->second.state.reserved_by = {};
            }
            ++stats.rejected;
            emit(InteractionEventKind::Rejected, current->actor, current->target, reason, current->activation);
        }
        current = pending.erase(current);
    }
}

void InteractionService::Impl::prune() {
    for (auto current = actors.begin(); current != actors.end();) {
        if (owns_actor(current->first)) { ++current; continue; }
        change_focus(current->second, {});
        for (const auto entity : current->second.inside) {
            transition(InteractionEventKind::TriggerLeave, current->first, entity, InteractionError::Stale);
            --stats.trigger_pairs;
        }
        abandon(current->first, {}, InteractionError::Stale);
        remove_components(current->first, current->second);
        current = actors.erase(current);
    }
    for (auto current = targets.begin(); current != targets.end();) {
        if (owns_target(current->first)) { ++current; continue; }
        for (auto& [entity, actor] : actors) {
            if (actor.state.focus == current->first) { change_focus(actor, {}); }
            if (actor.inside.erase(current->first) != 0) {
                --stats.trigger_pairs;
                transition(InteractionEventKind::TriggerLeave, entity, current->first, InteractionError::Stale);
            }
        }
        abandon({}, current->first, InteractionError::Stale);
        remove_components(current->first, current->second);
        current = targets.erase(current);
    }
    if (!actors.contains(selected)) { selected = {}; }
    if (!actors.contains(input_actor)) { input_actor = {}; }
}

InteractionService::InteractionService(core::Kernel& kernel, scene::Scene& scene, InteractionLimits limits)
    : impl_(std::make_unique<Impl>(kernel, scene, limits)) {}
InteractionService::~InteractionService() = default;

bool InteractionService::attach_actor(ecs::Entity entity, InteractionActorComponent settings) {
    auto& state = *impl_;
    if (!state.editable()) { return false; }
    if (!state.enrolled(entity)) { return state.fail(InteractionError::InvalidEntity); }
    if (!actor_valid(settings)) { return state.fail(InteractionError::InvalidSettings); }
    if (state.actors.contains(entity) || state.registry.has<InteractionActorComponent>(entity)) {
        return state.fail(InteractionError::Locked);
    }
    if (state.actors.size() >= state.limits.actors) { return state.fail(InteractionError::Capacity); }
    auto [entry, inserted] = state.actors.emplace(entity, Impl::Actor{});
    (void)inserted;
    entry->second.state.entity = entity;
    try { state.registry.emplace<InteractionActorComponent>(entity, std::move(settings)); }
    catch (...) { state.actors.erase(entry); return state.fail(InteractionError::Capacity); }
    entry->second.instance = state.registry.component_instance<InteractionActorComponent>(entity);
    return true;
}

bool InteractionService::attach_target(ecs::Entity entity, InteractableComponent settings) {
    auto& state = *impl_;
    if (!state.editable()) { return false; }
    if (!state.enrolled(entity)) { return state.fail(InteractionError::InvalidEntity); }
    if (!target_valid(settings)) { return state.fail(InteractionError::InvalidSettings); }
    if (state.targets.contains(entity) || state.registry.has<InteractableComponent>(entity) ||
        state.registry.has<InteractionActionsComponent>(entity)) { return state.fail(InteractionError::Locked); }
    if (state.targets.size() >= state.limits.targets) { return state.fail(InteractionError::Capacity); }
    auto [entry, inserted] = state.targets.emplace(entity, Impl::Target{});
    (void)inserted;
    entry->second.state.entity = entity;
    try {
        state.registry.emplace<InteractableComponent>(entity, std::move(settings));
        state.registry.emplace<InteractionActionsComponent>(entity);
    } catch (...) {
        (void)state.registry.remove<InteractableComponent>(entity);
        state.targets.erase(entry);
        return state.fail(InteractionError::Capacity);
    }
    entry->second.instance = state.registry.component_instance<InteractableComponent>(entity);
    entry->second.actions_instance = state.registry.component_instance<InteractionActionsComponent>(entity);
    return true;
}

bool InteractionService::detach_actor(ecs::Entity entity) {
    auto& state = *impl_;
    if (!state.editable()) { return false; }
    const auto found = state.actors.find(entity);
    if (found == state.actors.end()) { return state.fail(InteractionError::MissingActor); }
    state.change_focus(found->second, {});
    for (const auto target : found->second.inside) {
        --state.stats.trigger_pairs;
        state.transition(InteractionEventKind::TriggerLeave, entity, target);
    }
    for (auto& [target, runtime] : state.targets) {
        (void)target;
        if (runtime.state.reserved_by == entity) { runtime.state.reserved_by = {}; }
    }
    state.abandon(entity, {}, InteractionError::Stale);
    state.remove_components(entity, found->second);
    state.actors.erase(found);
    if (state.selected == entity) { state.selected = {}; }
    if (state.input_actor == entity) { state.input_actor = {}; }
    return true;
}

bool InteractionService::detach_target(ecs::Entity entity) {
    auto& state = *impl_;
    if (!state.editable()) { return false; }
    const auto found = state.targets.find(entity);
    if (found == state.targets.end()) { return state.fail(InteractionError::MissingTarget); }
    for (auto& [actor, runtime] : state.actors) {
        if (runtime.state.focus == entity) { state.change_focus(runtime, {}); }
        if (runtime.inside.erase(entity) != 0) {
            --state.stats.trigger_pairs;
            state.transition(InteractionEventKind::TriggerLeave, actor, entity);
        }
    }
    state.abandon({}, entity, InteractionError::Stale);
    state.remove_components(entity, found->second);
    state.targets.erase(found);
    return true;
}

bool InteractionService::set_actions(ecs::Entity entity, std::span<const InteractionAction> actions) {
    auto& state = *impl_;
    if (!state.editable()) { return false; }
    const auto found = state.targets.find(entity);
    if (!state.owns_target(entity)) { return state.fail(InteractionError::MissingTarget); }
    if (found->second.inflight) { return state.fail(InteractionError::Locked); }
    if (actions.size() > state.limits.actions_per_target) { return state.fail(InteractionError::Capacity); }
    for (const auto& action : actions) {
        if (!action_valid(action)) { return state.fail(InteractionError::InvalidSettings); }
    }
    InteractionActionsComponent replacement;
    replacement.actions.assign(actions.begin(), actions.end());
    state.registry.emplace_or_replace<InteractionActionsComponent>(entity, std::move(replacement));
    found->second.actions_instance = state.registry.component_instance<InteractionActionsComponent>(entity);
    return true;
}

bool InteractionService::add_action(ecs::Entity entity, InteractionAction action) {
    if (!impl_->editable()) { return false; }
    const auto* component = impl_->registry.get<InteractionActionsComponent>(entity);
    if (!component) { return impl_->fail(InteractionError::MissingTarget); }
    if (component->actions.size() >= impl_->limits.actions_per_target) { return impl_->fail(InteractionError::Capacity); }
    auto actions = component->actions;
    actions.push_back(std::move(action));
    return set_actions(entity, actions);
}

bool InteractionService::register_native(std::string name, InteractionCallback callback) {
    if (!impl_->editable()) { return false; }
    if (!name_valid(name) || !callback) { return impl_->fail(InteractionError::InvalidSettings); }
    if (!impl_->callbacks.contains(name) && impl_->callbacks.size() >= impl_->limits.callbacks) {
        return impl_->fail(InteractionError::Capacity);
    }
    impl_->callbacks.insert_or_assign(std::move(name), std::move(callback));
    return true;
}

bool InteractionService::unregister_native(std::string_view name) {
    if (!impl_->editable()) { return false; }
    const auto found = impl_->callbacks.find(name);
    if (found == impl_->callbacks.end()) { return impl_->fail(InteractionError::InvalidSettings); }
    impl_->callbacks.erase(found);
    return true;
}

bool InteractionService::select_player(ecs::Entity actor) {
    if (!impl_->editable()) { return false; }
    if (actor.valid() && !impl_->owns_actor(actor)) { return impl_->fail(InteractionError::MissingActor); }
    if (impl_->input_actor != actor) {
        const auto previous = impl_->actors.find(impl_->input_actor);
        if (previous != impl_->actors.end() && !previous->second.manual) { previous->second.desired_down = false; }
    }
    impl_->selected = actor;
    impl_->input_actor = actor;
    return true;
}

ecs::Entity InteractionService::player() const noexcept {
    return impl_->owns_actor(impl_->input_actor) ? impl_->input_actor : ecs::Entity{};
}

bool InteractionService::focus(ecs::Entity actor, ecs::Entity target) {
    auto& state = *impl_;
    if (!state.editable()) { return false; }
    const auto found = state.actors.find(actor);
    if (!state.owns_actor(actor)) { return state.fail(InteractionError::MissingActor); }
    if (target.valid()) {
        if (auto* collision = state.kernel.services().find<physics::CollisionWorld>()) { (void)collision->sync(state.scene); }
        state.stats.evaluations = 0;
        const auto candidate = state.evaluate(actor, target);
        if (candidate.error != InteractionError::None) { return state.fail(candidate.error); }
    }
    state.change_focus(found->second, target);
    return true;
}

bool InteractionService::refresh(ecs::Entity actor) {
    if (!impl_->editable()) { return false; }
    const auto found = impl_->actors.find(actor);
    if (!impl_->owns_actor(actor)) { return impl_->fail(InteractionError::MissingActor); }
    if (auto* collision = impl_->kernel.services().find<physics::CollisionWorld>()) { (void)collision->sync(impl_->scene); }
    impl_->stats.evaluations = 0;
    impl_->choose(found->second);
    return true;
}

bool InteractionService::use(ecs::Entity actor, bool down) {
    if (!impl_->editable()) { return false; }
    const auto found = impl_->actors.find(actor);
    if (!impl_->owns_actor(actor)) { return impl_->fail(InteractionError::MissingActor); }
    found->second.desired_down = down;
    found->second.manual = true;
    return true;
}

bool InteractionService::cancel(ecs::Entity actor) {
    if (!impl_->editable()) { return false; }
    const auto found = impl_->actors.find(actor);
    if (!impl_->owns_actor(actor)) { return impl_->fail(InteractionError::MissingActor); }
    impl_->release(found->second, InteractionError::Released);
    impl_->abandon(actor, {}, InteractionError::Released);
    found->second.state.activated_this_press = true;
    found->second.desired_down = false;
    found->second.manual = true;
    return true;
}

void InteractionService::update(const core::Frame& frame) {
    auto& state = *impl_;
    if (!state.editable() || (state.has_tick && frame.tick <= state.tick)) { return; }
    BusyScope scope(state.busy);
    state.tick = frame.tick;
    state.has_tick = true;
    state.stats.invalid_delta = !bounded(frame.delta_seconds, 0.0, state.limits.maximum_delta_seconds);
    const double delta = std::isfinite(frame.delta_seconds)
        ? std::clamp(frame.delta_seconds, 0.0, state.limits.maximum_delta_seconds) : 0.0;
    state.stats.evaluations = 0;
    state.stats.actions = 0;
    state.stats.budget_exhausted = false;
    state.prune();
    const auto previous_input_actor = state.input_actor;
    state.input_actor = state.selected;
    if (!state.input_actor.valid()) {
        if (const auto* vehicles = state.kernel.services().find<physics::VehicleSystem>()) {
            const auto vehicle = vehicles->state(vehicles->player());
            if (vehicle && state.actors.contains(vehicle->entity)) { state.input_actor = vehicle->entity; }
        }
        if (!state.input_actor.valid()) {
            if (const auto* characters = state.kernel.services().find<physics::CharacterSystem>()) {
                if (state.actors.contains(characters->player())) { state.input_actor = characters->player(); }
            }
        }
    }
    if (previous_input_actor != state.input_actor) {
        const auto previous = state.actors.find(previous_input_actor);
        if (previous != state.actors.end() && !previous->second.manual) { previous->second.desired_down = false; }
    }
    if (auto* collision = state.kernel.services().find<physics::CollisionWorld>()) { (void)collision->sync(state.scene); }
    for (auto& [entity, target] : state.targets) {
        (void)entity;
        target.state.cooldown_remaining = std::max(0.0, target.state.cooldown_remaining - delta);
    }
    state.dispatch();
    state.prune();
    if (auto* collision = state.kernel.services().find<physics::CollisionWorld>()) { (void)collision->sync(state.scene); }
    auto* input_system = state.kernel.services().find<input::InputSystem>();
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (auto& [entity, actor] : state.actors) {
            if ((pass == 0) != (entity == state.input_actor)) { continue; }
            const auto* settings = state.registry.get<InteractionActorComponent>(entity);
            if (!settings || !settings->enabled || !state.scene.effective_enabled(entity)) {
                actor.desired_down = false;
                state.change_focus(actor, {});
            } else {
                state.choose(actor);
                if (entity == state.input_actor && !actor.manual) {
                    const auto* action = input_system ? input_system->action("interact") : nullptr;
                    actor.desired_down = action && !action->consumed && input_system->snapshot().focused && action->down;
                    if (actor.desired_down && settings->consume_input && actor.state.focus.valid()) { (void)input_system->consume("interact"); }
                }
            }
            state.buttons(actor, delta);
            state.triggers(actor);
            if (!actor.desired_down) { actor.manual = false; }
        }
    }
    state.dispatch();
    state.prune();
}

bool InteractionService::clear() {
    if (!impl_->editable()) { return false; }
    while (!impl_->actors.empty()) { (void)detach_actor(impl_->actors.begin()->first); }
    while (!impl_->targets.empty()) { (void)detach_target(impl_->targets.begin()->first); }
    impl_->pending.clear();
    impl_->input_actor = {};
    return true;
}

std::optional<InteractionActorState> InteractionService::actor(ecs::Entity entity) const {
    const auto found = impl_->actors.find(entity);
    return !impl_->owns_actor(entity) ? std::nullopt : std::optional{found->second.state};
}

std::optional<InteractionTargetState> InteractionService::target(ecs::Entity entity) const {
    const auto found = impl_->targets.find(entity);
    return !impl_->owns_target(entity) ? std::nullopt : std::optional{found->second.state};
}

std::vector<InteractionCandidate> InteractionService::candidates(ecs::Entity actor, std::size_t limit) {
    std::vector<InteractionCandidate> result;
    if (!impl_->editable() || !impl_->owns_actor(actor)) { return result; }
    if (auto* collision = impl_->kernel.services().find<physics::CollisionWorld>()) { (void)collision->sync(impl_->scene); }
    impl_->stats.evaluations = 0;
    limit = std::min(limit, impl_->limits.targets);
    for (const auto& [entity, target] : impl_->targets) {
        (void)target;
        auto candidate = impl_->evaluate(actor, entity);
        if (candidate.error != InteractionError::None) { continue; }
        const auto position = std::lower_bound(result.begin(), result.end(), candidate, better);
        if (result.size() < limit) { result.insert(position, candidate); }
        else if (position != result.end()) { result.insert(position, candidate); result.pop_back(); }
    }
    return result;
}

InteractionHistory InteractionService::events_since(std::uint64_t cursor, std::size_t limit) const {
    InteractionHistory result;
    result.next_cursor = cursor;
    result.newest_sequence = impl_->sequence;
    result.history_lost = cursor > impl_->sequence || (!impl_->history.empty() &&
        cursor < impl_->history.front().sequence - 1);
    if (cursor > impl_->sequence) { result.next_cursor = 0; cursor = 0; }
    limit = std::min(limit, impl_->limits.history);
    for (const auto& event : impl_->history) {
        if (event.sequence <= cursor) { continue; }
        if (result.events.size() >= limit) { break; }
        result.events.push_back(event);
        result.next_cursor = event.sequence;
    }
    return result;
}

InteractionError InteractionService::last_error() const noexcept { return impl_->error; }
const InteractionLimits& InteractionService::limits() const noexcept { return impl_->limits; }
InteractionStatistics InteractionService::statistics() const noexcept {
    auto result = impl_->stats;
    result.actors = impl_->actors.size();
    result.targets = impl_->targets.size();
    result.pending_batches = impl_->pending.size();
    result.focused = 0;
    result.holding = 0;
    for (const auto& [entity, actor] : impl_->actors) {
        (void)entity;
        if (actor.state.focus.valid()) { ++result.focused; }
        if (actor.state.holding.valid()) { ++result.holding; }
    }
    return result;
}

namespace {

ecs::Entity command_entity(core::Kernel& kernel, std::string_view name) {
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) { return {}; }
    if (const auto parsed = ecs::parse_entity(name); parsed && registry->alive(*parsed)) { return *parsed; }
    const auto matches = registry->find_all_named(name);
    if (matches.size() == 1) { return matches.front(); }
    if (auto* world = kernel.services().find<scene::Scene>()) {
        const auto path = world->find_path(name);
        if (path) { return path.entity; }
    }
    return {};
}

core::CommandResult operation(InteractionService& service, bool success) {
    return {success, std::string(success ? "ok" : describe(service.last_error()))};
}

} // namespace

void register_interaction(core::Kernel& kernel) {
    auto* world = kernel.services().find<scene::Scene>();
    if (!world) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "gameplay.interaction", "Scene service is required");
        return;
    }
    if (!kernel.register_module({"gameplay.interaction", "Bounded targeting, triggers and authored gameplay actions", 1})) { return; }
    auto* service = &kernel.services().emplace<InteractionService>(kernel, *world);
    (void)world->registry().register_component<InteractionActorComponent>("interaction.actor");
    (void)world->registry().register_component<InteractableComponent>("interaction.target");
    (void)world->registry().register_component<InteractionActionsComponent>("interaction.actions");
    (void)service->register_native("physics.wake", [](InteractionActionContext& context) {
        auto* dynamics = context.kernel.services().find<physics::PhysicsWorld>();
        return InteractionActionResult{dynamics && dynamics->wake(dynamics->find(context.subject)), "body wake requested"};
    });
    std::vector<std::string> after;
    if (kernel.services().find<spatial::SpatialIndex>()) { after.emplace_back("spatial.sync"); }
    if (kernel.services().find<render::CameraService>()) { after.emplace_back("camera.update"); }
    (void)kernel.add_system({"gameplay.interaction.update", core::Phase::Visibility, -7000, std::move(after)},
        [service](core::Kernel& host, const core::Frame& frame) {
            service->update(frame);
            const auto stats = service->statistics();
            host.metric("interaction.focused", static_cast<double>(stats.focused));
            host.metric("interaction.actions", static_cast<double>(stats.actions));
            host.metric("interaction.pending", static_cast<double>(stats.pending_batches));
            host.metric("interaction.budget_exhausted", stats.budget_exhausted ? 1.0 : 0.0);
        });
    (void)kernel.add_command("interaction.attach", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 9) {
            return core::CommandResult{false, "interaction.attach actor ENTITY [range cone_degrees] | target ENTITY [press|hold|repeat range hold repeat cooldown trigger prompt]"};
        }
        const auto entity = command_entity(host, args[1]);
        if (args[0] == "actor") {
            if (args.size() > 4) { return core::CommandResult{false, "actor accepts range and cone degrees"}; }
            InteractionActorComponent settings;
            double angle = 60.0;
            if ((args.size() > 2 && !parse_number(args[2], settings.range)) ||
                (args.size() > 3 && (!parse_number(args[3], angle) || !bounded(angle, 0.0, 180.0)))) {
                return core::CommandResult{false, "invalid actor range or cone"};
            }
            settings.cone_cosine = std::cos(math::radians(angle));
            return operation(*service, service->attach_actor(entity, settings));
        }
        if (args[0] != "target") { return core::CommandResult{false, "expected actor or target"}; }
        InteractableComponent settings;
        if (args.size() > 2) {
            if (args[2] == "hold") { settings.mode = InteractionMode::Hold; }
            else if (args[2] == "repeat") { settings.mode = InteractionMode::Repeat; }
            else if (args[2] != "press") { return core::CommandResult{false, "unknown interaction mode"}; }
        }
        const std::array<double*, 5> values{&settings.range, &settings.hold_seconds,
            &settings.repeat_seconds, &settings.cooldown_seconds, &settings.trigger_radius};
        for (std::size_t i = 3; i < std::min<std::size_t>(args.size(), 8); ++i) {
            if (!parse_number(args[i], *values[i - 3])) { return core::CommandResult{false, "invalid target number"}; }
        }
        if (args.size() > 8) { settings.prompt = args[8]; }
        return operation(*service, service->attach_target(entity, std::move(settings)));
    }, "Attach a real scene entity as an interaction actor or target");
    (void)kernel.add_command("interaction.detach", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) { return core::CommandResult{false, "interaction.detach actor|target ENTITY"}; }
        const auto entity = command_entity(host, args[1]);
        if (args[0] == "actor") { return operation(*service, service->detach_actor(entity)); }
        if (args[0] == "target") { return operation(*service, service->detach_target(entity)); }
        return core::CommandResult{false, "expected actor or target"};
    }, "Detach interaction data and cancel outstanding ownership");
    (void)kernel.add_command("interaction.player", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "interaction.player ENTITY|auto"}; }
        const auto entity = args[0] == "auto" ? ecs::Entity{} : command_entity(host, args[0]);
        if (args[0] != "auto" && !entity.valid()) { return core::CommandResult{false, "unknown player entity"}; }
        return operation(*service, service->select_player(entity));
    }, "Route the existing interact action to an actor, or use character/vehicle player selection");
    (void)kernel.add_command("interaction.focus", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) { return core::CommandResult{false, "interaction.focus ACTOR TARGET|auto|none"}; }
        const auto actor = command_entity(host, args[0]);
        if (args[1] == "auto") { return operation(*service, service->refresh(actor)); }
        const auto target = args[1] == "none" ? ecs::Entity{} : command_entity(host, args[1]);
        if (args[1] != "none" && !target.valid()) { return core::CommandResult{false, "unknown target"}; }
        return operation(*service, service->focus(actor, target));
    }, "Choose an eligible focus using range, cone and obstruction checks");
    (void)kernel.add_command("interaction.use", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) { return core::CommandResult{false, "interaction.use ACTOR press|release|cancel"}; }
        const auto actor = command_entity(host, args[0]);
        if (args[1] == "cancel") { return operation(*service, service->cancel(actor)); }
        if (args[1] != "press" && args[1] != "release") { return core::CommandResult{false, "expected press, release or cancel"}; }
        return operation(*service, service->use(actor, args[1] == "press"));
    }, "Submit a held button level; activation is processed by the frame system");
    (void)kernel.add_command("interaction.action", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 3 || args.size() > 20) {
            return core::CommandResult{false, "interaction.action TARGET EVENT KIND [name|x y z|value] [command arguments]"};
        }
        InteractionAction action;
        bool event_found = false;
        for (unsigned value = 0; value <= static_cast<unsigned>(InteractionEventKind::ActionFailed); ++value) {
            const auto event = static_cast<InteractionEventKind>(value);
            if (args[1] == describe(event)) { action.events = interaction_event_mask(event); event_found = true; break; }
        }
        bool kind_found = false;
        for (unsigned value = 0; value <= static_cast<unsigned>(InteractionActionKind::SelectVehicle); ++value) {
            const auto kind = static_cast<InteractionActionKind>(value);
            if (args[2] == describe(kind)) { action.kind = kind; kind_found = true; break; }
        }
        if (!event_found || !kind_found) { return core::CommandResult{false, "unknown event or action kind"}; }
        if (action.kind == InteractionActionKind::Translate || action.kind == InteractionActionKind::Impulse) {
            if (args.size() != 6 || !parse_number(args[3], action.vector.x) ||
                !parse_number(args[4], action.vector.y) || !parse_number(args[5], action.vector.z)) {
                return core::CommandResult{false, "vector action needs three finite numbers"};
            }
        } else if (action.kind == InteractionActionKind::SetEnabled) {
            if (args.size() != 4 || (args[3] != "true" && args[3] != "false")) { return core::CommandResult{false, "enable needs true or false"}; }
            action.value = args[3] == "true";
        } else if (action.kind == InteractionActionKind::ToggleEnabled ||
                   action.kind == InteractionActionKind::SelectCharacter || action.kind == InteractionActionKind::SelectVehicle) {
            if (args.size() != 3) { return core::CommandResult{false, "this action takes no arguments"}; }
        } else {
            if (args.size() < 4) { return core::CommandResult{false, "action needs a name"}; }
            action.name = args[3];
            if (action.kind == InteractionActionKind::Command) { action.arguments.assign(args.begin() + 4, args.end()); }
            else if (args.size() != 4) { return core::CommandResult{false, "unexpected action arguments"}; }
        }
        return operation(*service, service->add_action(command_entity(host, args[0]), std::move(action)));
    }, "Append native, script, deferred command, tag, transform, impulse or player actions");
    (void)kernel.add_command("interaction.inspect", [service](core::Kernel& host, core::CommandArgs args) {
        std::ostringstream output;
        if (args.empty()) {
            const auto stats = service->statistics();
            output << "actors=" << stats.actors << " targets=" << stats.targets << " focused=" << stats.focused
                   << " holding=" << stats.holding << " trigger_pairs=" << stats.trigger_pairs
                   << " pending=" << stats.pending_batches << " activated=" << stats.activations
                   << " rejected=" << stats.rejected << " budget=" << stats.budget_exhausted;
        } else if (args.size() == 1) {
            const auto entity = command_entity(host, args[0]);
            const auto actor = service->actor(entity);
            const auto target = service->target(entity);
            if (!actor && !target) { return core::CommandResult{false, "entity has no interaction ownership"}; }
            if (actor) {
                output << "focus=" << ecs::format_entity(actor->focus) << " holding=" << ecs::format_entity(actor->holding)
                       << " progress=" << actor->progress << " down=" << actor->button_down
                       << " error=" << describe(actor->last_error) << '\n';
            }
            if (target) {
                output << "cooldown=" << target->cooldown_remaining << " used=" << target->used
                       << " activations=" << target->activations << " reserved=" << ecs::format_entity(target->reserved_by);
            }
        } else { return core::CommandResult{false, "interaction.inspect [ENTITY]"}; }
        return core::CommandResult{true, output.str()};
    }, "Inspect live focus, activation progress, cooldowns and bounded-work statistics");
    (void)kernel.add_command("interaction.history", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t cursor{};
        if (args.size() > 1 || (!args.empty() && !parse_number(args[0], cursor))) {
            return core::CommandResult{false, "interaction.history [cursor]"};
        }
        const auto batch = service->events_since(cursor, 64);
        std::ostringstream output;
        output << "cursor=" << batch.next_cursor << " newest=" << batch.newest_sequence << " lost=" << batch.history_lost;
        for (const auto& event : batch.events) {
            output << '\n' << event.sequence << ' ' << describe(event.kind) << " actor=" << ecs::format_entity(event.actor)
                   << " target=" << ecs::format_entity(event.target) << " activation=" << event.activation
                   << " error=" << describe(event.error) << " name=" << event.name << " message=" << event.message;
        }
        return core::CommandResult{true, output.str()};
    }, "Read an independent cursor without consuming another gameplay subscriber's outcomes");
}

} // namespace ge::gameplay
