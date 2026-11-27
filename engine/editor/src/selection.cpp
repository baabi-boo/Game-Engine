#include <ge/editor/selection.hpp>

#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>
#include <ge/spatial/index.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ge::editor {
namespace {
using Entity = ecs::Entity;
using EntitySet = std::unordered_set<Entity, ecs::EntityHash>;

bool has(std::span<const Entity> entities, Entity entity) {
    return std::find(entities.begin(), entities.end(), entity) != entities.end();
}

bool valid_filter(const SelectionFilter& filter) {
    if (filter.required_tags.size() > 64 || filter.excluded_tags.size() > 64) { return false; }
    const auto valid_tags = [](const std::vector<std::string>& tags) {
        return std::all_of(tags.begin(), tags.end(), [](const auto& tag) {
            return !tag.empty() && tag.size() <= 256 && tag.find('\0') == std::string::npos;
        });
    };
    return valid_tags(filter.required_tags) && valid_tags(filter.excluded_tags);
}

bool valid_options(const SelectionOptions& options) {
    return valid_filter(options.filter) && std::isfinite(options.minimum_distance) &&
        std::isfinite(options.maximum_distance) && options.minimum_distance >= 0.0 &&
        options.maximum_distance >= options.minimum_distance &&
        options.maximum_distance <= 1.0e15 &&
        options.hierarchy <= SelectionHierarchy::MarkedRoot &&
        options.precision <= SelectionPrecision::Bounds;
}

bool valid_limits(const SelectionLimits& value) {
    return value.selected > 0 && value.selected <= 65536 &&
        value.scene_entities > 0 && value.scene_entities <= 1048576 &&
        value.candidates > 0 && value.candidates <= value.scene_entities &&
        value.triangles > 0 && value.triangles <= 16777216 &&
        value.hits > 0 && value.hits <= value.candidates &&
        value.query_visits > 0 && value.query_visits <= 4194304 &&
        value.hierarchy_depth > 0 && value.hierarchy_depth <= 4096 &&
        value.undo_entries <= 256 && value.change_entries > 0 && value.change_entries <= 1024 &&
        value.observers <= 256 && value.pointer_requests > 0 && value.pointer_requests <= 64;
}

void incomplete(SelectionQueryReport& report) {
    report.complete = false;
    if (report.error == SelectionError::None) { report.error = SelectionError::Incomplete; }
}

bool hit_less(const SelectionHit& first, const SelectionHit& second) {
    if (first.distance != second.distance) { return first.distance < second.distance; }
    if (first.target != second.target) { return first.target < second.target; }
    if (first.entity != second.entity) { return first.entity < second.entity; }
    return first.triangle < second.triangle;
}

std::array<math::Vec3, 8> box_corners(const math::Aabb& box) {
    std::array<math::Vec3, 8> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = {(index & 1U) != 0 ? box.max.x : box.min.x,
                         (index & 2U) != 0 ? box.max.y : box.min.y,
                         (index & 4U) != 0 ? box.max.z : box.min.z};
    }
    return result;
}

template<class Number>
std::optional<Number> parse_number(std::string_view text) {
    Number value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) { return std::nullopt; }
    return value;
}

std::optional<SelectionMode> parse_mode(std::string_view name) {
    if (name == "replace") { return SelectionMode::Replace; }
    if (name == "add") { return SelectionMode::Add; }
    if (name == "toggle") { return SelectionMode::Toggle; }
    if (name == "subtract") { return SelectionMode::Subtract; }
    return std::nullopt;
}

core::CommandResult command_result(const SelectionResult& result) {
    return {static_cast<bool>(result), std::string(describe(result.error)) +
        "; accepted=" + std::to_string(result.accepted) +
        "; rejected=" + std::to_string(result.rejected)};
}
} // namespace

std::string_view describe(SelectionError error) noexcept {
    switch (error) {
    case SelectionError::None: return "selection operation completed";
    case SelectionError::InvalidArgument: return "invalid selection argument";
    case SelectionError::InvalidEntity: return "entity is stale, absent or filtered";
    case SelectionError::Capacity: return "selection capacity exceeded";
    case SelectionError::InvalidCamera: return "camera or pixel is invalid or stale";
    case SelectionError::Incomplete: return "query is incomplete; selection was not committed";
    case SelectionError::Busy: return "selection observer is active";
    case SelectionError::HistoryEmpty: return "selection history is empty";
    case SelectionError::Inactive: return "selection interaction is inactive";
    case SelectionError::Captured: return "pointer is captured by another UI owner";
    }
    return "unknown selection error";
}

struct SelectionService::Impl {
    struct State {
        std::vector<Entity> entities;
        Entity primary{};
    };
    scene::Scene& scene;
    SelectionLimits limits;
    State state;
    Entity hover{};
    std::uint64_t revision{};
    std::deque<State> undo;
    std::deque<State> redo;
    std::deque<SelectionChange> changes;
    std::vector<std::pair<std::uint64_t, Observer>> observers;
    std::uint64_t next_observer{1};
    std::uint64_t observer_failures{};
    std::uint64_t incomplete_queries{};
    bool notifying{};
    bool interaction_enabled{};
    bool pointer_captured{true};
    SelectionOptions pointer_options{};
    std::vector<SelectionPointerRequest> pending;
    std::vector<Entity> cycle_targets;
    Entity cycle_target{};
    math::Ray cycle_ray{};
    Entity cycle_camera{};

    Impl(scene::Scene& owner, SelectionLimits configuration) : scene(owner), limits(configuration) {
        if (!valid_limits(limits)) { throw std::invalid_argument("invalid selection limits"); }
    }

    bool live(Entity entity) const {
        return scene.registry().alive(entity) && scene.contains(entity);
    }

    SelectionComponent policy(Entity entity) const {
        const auto* value = scene.registry().get<SelectionComponent>(entity);
        return value != nullptr ? *value : SelectionComponent{};
    }

    bool accepts(Entity entity, const SelectionFilter& filter, bool* exhausted = nullptr) const {
        const auto& registry = scene.registry();
        if (!live(entity) || !policy(entity).selectable || (registry.layers(entity) & filter.layers) == 0) {
            return false;
        }
        if (filter.subtree && (!live(filter.subtree) ||
            (entity != filter.subtree && !scene.is_ancestor(filter.subtree, entity)))) { return false; }
        for (const auto& tag : filter.required_tags) {
            if (!registry.has_tag(entity, tag)) { return false; }
        }
        for (const auto& tag : filter.excluded_tags) {
            if (registry.has_tag(entity, tag)) { return false; }
        }
        std::size_t depth{};
        for (Entity cursor = entity; cursor; cursor = scene.parent(cursor)) {
            if (++depth > limits.hierarchy_depth) {
                if (exhausted != nullptr) { *exhausted = true; }
                return false;
            }
            const auto metadata = policy(cursor);
            if ((!filter.include_locked && metadata.locked) ||
                (!filter.include_hidden && metadata.hidden) ||
                (!filter.include_disabled && !registry.enabled(cursor))) { return false; }
            if (!filter.inherit_restrictions) { break; }
        }
        return true;
    }

    Entity target(Entity entity, SelectionHierarchy hierarchy, const SelectionFilter& filter,
                  bool* exhausted = nullptr) const {
        if (!accepts(entity, filter, exhausted)) { return {}; }
        if (hierarchy == SelectionHierarchy::Entity) { return entity; }
        Entity result = entity;
        std::size_t depth{};
        for (Entity cursor = entity; cursor; cursor = scene.parent(cursor)) {
            if (++depth > limits.hierarchy_depth) {
                if (exhausted != nullptr) { *exhausted = true; }
                return {};
            }
            if ((hierarchy == SelectionHierarchy::Root || policy(cursor).group_root) &&
                accepts(cursor, filter, exhausted)) { result = cursor; }
            if (cursor == filter.subtree) { break; }
        }
        return result;
    }

    SelectionResult commit(State next, Entity next_hover, SelectionReason reason, bool record) {
        if (notifying) { return {SelectionError::Busy}; }
        const bool state_changed = next.entities != state.entities || next.primary != state.primary;
        if (!state_changed && next_hover == hover) { return {}; }
        if (revision == std::numeric_limits<std::uint64_t>::max()) { return {SelectionError::Capacity}; }
        SelectionChange change;
        change.revision = revision + 1;
        change.reason = reason;
        change.previous_primary = state.primary;
        change.primary = next.primary;
        change.previous_hover = hover;
        change.hover = next_hover;
        const EntitySet previous(state.entities.begin(), state.entities.end());
        const EntitySet following(next.entities.begin(), next.entities.end());
        for (const auto entity : next.entities) {
            if (!previous.contains(entity)) { change.added.push_back(entity); }
        }
        for (const auto entity : state.entities) {
            if (!following.contains(entity)) { change.removed.push_back(entity); }
        }
        changes.push_back(change);
        try {
            if (record && state_changed && limits.undo_entries != 0) { undo.push_back(state); }
        } catch (...) {
            changes.pop_back();
            throw;
        }
        if (record && state_changed) { redo.clear(); }
        while (undo.size() > limits.undo_entries) { undo.pop_front(); }
        while (changes.size() > limits.change_entries) { changes.pop_front(); }
        state = std::move(next);
        hover = next_hover;
        revision = change.revision;
        notifying = true;
        for (const auto& observer : observers) {
            try { observer.second(change); }
            catch (...) {
                if (observer_failures != std::numeric_limits<std::uint64_t>::max()) { ++observer_failures; }
            }
        }
        notifying = false;
        return {SelectionError::None, state.entities.size(), 0, true};
    }

    State sanitize(const State& input) const {
        State result;
        for (const auto entity : input.entities) {
            if (live(entity)) { result.entities.push_back(entity); }
        }
        result.primary = has(result.entities, input.primary) ? input.primary :
            (result.entities.empty() ? Entity{} : result.entities.back());
        return result;
    }

    SelectionResult travel(std::deque<State>& from, std::deque<State>& to, SelectionReason reason) {
        if (notifying) { return {SelectionError::Busy}; }
        if (from.empty()) { return {SelectionError::HistoryEmpty}; }
        auto next = sanitize(from.back());
        to.push_back(state);
        SelectionResult result;
        try { result = commit(std::move(next), live(hover) ? hover : Entity{}, reason, false); }
        catch (...) { to.pop_back(); throw; }
        if (!result) { to.pop_back(); return result; }
        from.pop_back();
        while (to.size() > limits.undo_entries) { to.pop_front(); }
        return result;
    }

    std::optional<math::Aabb> entity_bounds(Entity entity, const geometry::MeshLibrary& meshes) const {
        const auto* world = scene.cached_world_matrix(entity);
        if (world == nullptr) { return std::nullopt; }
        const auto* component = scene.registry().get<geometry::MeshComponent>(entity);
        if (component != nullptr) {
            if (const auto info = meshes.inspect(component->mesh)) {
                const auto bounds = math::transform_bounds(*world, info->bounds);
                if (math::valid(bounds)) { return bounds; }
            }
        }
        const auto* component_bounds = scene.registry().get<ecs::BoundsComponent>(entity);
        if (component_bounds == nullptr) { return std::nullopt; }
        const auto bounds = math::transform_bounds(*world, component_bounds->local);
        return math::valid(bounds) ? std::optional{bounds} : std::nullopt;
    }

    std::vector<Entity> candidates(const geometry::MeshLibrary& meshes, spatial::SpatialIndex* index,
                                   const SelectionFilter& filter, SelectionQueryReport& report,
                                   const math::Ray* ray, double minimum, double maximum,
                                   const math::Frustum* frustum, bool origins) {
        std::vector<Entity> result;
        if (scene.size() > limits.scene_entities) {
            report.candidates_truncated = true;
            incomplete(report);
            return result;
        }
        EntitySet indexed_hits;
        bool trust_index = index != nullptr && !filter.include_disabled && !origins;
        if (trust_index) {
            spatial::QueryFilter spatial_filter;
            spatial_filter.layers = filter.layers;
            spatial_filter.max_visits = limits.query_visits;
            spatial::QueryReport query;
            if (ray != nullptr) {
                std::vector<spatial::RayHit> buffer(limits.candidates);
                query = index->raycast(*ray, maximum, buffer, spatial_filter, minimum);
                for (std::size_t i = 0; i < query.written; ++i) { indexed_hits.insert(buffer[i].entity); }
            } else {
                std::vector<spatial::QueryHit> buffer(limits.candidates);
                query = index->query_frustum(*frustum, buffer, spatial_filter);
                for (std::size_t i = 0; i < query.written; ++i) { indexed_hits.insert(buffer[i].entity); }
            }
            report.used_index = true;
            report.index_incomplete = query.truncated || query.traversal_exhausted ||
                query.invalid_input || query.stale || index->last_sync().rejected != 0 ||
                index->last_sync().capacity_failures != 0;
            trust_index = !report.index_incomplete;
        }
        for (const auto entity : scene.ordered_entities()) {
            bool exhausted{};
            if (!accepts(entity, filter, &exhausted)) {
                if (exhausted) { incomplete(report); }
                continue;
            }
            const auto* mesh = scene.registry().get<geometry::MeshComponent>(entity);
            if (mesh != nullptr && !mesh->visible && !filter.include_hidden) { continue; }
            const auto* world = scene.cached_world_matrix(entity);
            if (world == nullptr) { ++report.invalid_geometry; incomplete(report); continue; }
            const auto box = entity_bounds(entity, meshes);
            bool intersects{};
            if (origins) {
                ++report.direct_tests;
                intersects = math::contains(*frustum, math::transform_point(*world, {}));
            } else if (box) {
                const auto indexed = trust_index ? index->world_bounds(entity) : std::nullopt;
                if (indexed && math::contains(*indexed, *box) && !indexed_hits.contains(entity)) { continue; }
                ++report.direct_tests;
                intersects = ray != nullptr ? math::ray_interval(*ray, *box, minimum, maximum).has_value() :
                    math::intersects(*frustum, *box);
            } else if (mesh != nullptr) {
                ++report.invalid_geometry;
                incomplete(report);
            }
            if (!intersects) { continue; }
            if (result.size() == limits.candidates) {
                report.candidates_truncated = true;
                incomplete(report);
                break;
            }
            result.push_back(entity);
        }
        return result;
    }

    void account(const SelectionQueryReport& report) {
        if (!report.complete && incomplete_queries != std::numeric_limits<std::uint64_t>::max()) {
            ++incomplete_queries;
        }
    }
};

SelectionService::SelectionService(scene::Scene& scene, SelectionLimits limits)
    : impl_(std::make_unique<Impl>(scene, limits)) {}
SelectionService::~SelectionService() = default;
SelectionSnapshot SelectionService::snapshot() const {
    return {impl_->state.entities, impl_->state.primary, impl_->hover, impl_->revision};
}
std::span<const Entity> SelectionService::selected() const noexcept { return impl_->state.entities; }
Entity SelectionService::primary() const noexcept { return impl_->state.primary; }
Entity SelectionService::hover() const noexcept { return impl_->hover; }
bool SelectionService::contains(Entity entity) const noexcept { return has(impl_->state.entities, entity); }
const SelectionLimits& SelectionService::limits() const noexcept { return impl_->limits; }
bool SelectionService::eligible(Entity entity, const SelectionFilter& filter) const {
    return valid_filter(filter) && impl_->accepts(entity, filter);
}
SelectionStatistics SelectionService::statistics() const noexcept {
    return {impl_->state.entities.size(), impl_->undo.size(), impl_->redo.size(), impl_->pending.size(),
        impl_->revision, impl_->observer_failures, impl_->incomplete_queries,
        impl_->interaction_enabled, impl_->pointer_captured};
}
SelectionComponent SelectionService::policy(Entity entity) const noexcept { return impl_->policy(entity); }
bool SelectionService::set_policy(Entity entity, SelectionComponent policy_value) {
    if (impl_->notifying || !impl_->live(entity)) { return false; }
    impl_->scene.registry().emplace_or_replace<SelectionComponent>(entity, policy_value);
    return true;
}

SelectionResult SelectionService::apply(std::span<const Entity> entities, SelectionMode mode,
                                        const SelectionFilter& filter, SelectionHierarchy hierarchy) {
    if (impl_->notifying) { return {SelectionError::Busy}; }
    if (!valid_filter(filter) || mode > SelectionMode::Subtract || hierarchy > SelectionHierarchy::MarkedRoot) {
        return {SelectionError::InvalidArgument};
    }
    if (entities.size() > impl_->limits.scene_entities) { return {SelectionError::Capacity}; }
    auto next = mode == SelectionMode::Replace ? Impl::State{} : impl_->sanitize(impl_->state);
    EntitySet unique;
    std::size_t accepted{};
    std::size_t rejected{};
    for (const auto entity : entities) {
        bool exhausted{};
        const auto target = impl_->target(entity, hierarchy, filter, &exhausted);
        if (exhausted) { return {SelectionError::Incomplete}; }
        if (!target) { ++rejected; continue; }
        if (!unique.insert(target).second) { continue; }
        ++accepted;
        const auto found = std::find(next.entities.begin(), next.entities.end(), target);
        if (mode == SelectionMode::Subtract || (mode == SelectionMode::Toggle && found != next.entities.end())) {
            if (found != next.entities.end()) { next.entities.erase(found); }
        } else {
            if (found == next.entities.end()) {
                if (next.entities.size() >= impl_->limits.selected) { return {SelectionError::Capacity}; }
                next.entities.push_back(target);
            }
            next.primary = target;
        }
    }
    if (!has(next.entities, next.primary)) {
        next.primary = next.entities.empty() ? Entity{} : next.entities.back();
    }
    auto result = impl_->commit(std::move(next), impl_->hover, SelectionReason::Edit, true);
    result.accepted = accepted;
    result.rejected = rejected;
    return result;
}

SelectionResult SelectionService::clear() {
    return impl_->commit({}, impl_->hover, SelectionReason::Edit, true);
}
SelectionResult SelectionService::set_primary(Entity entity) {
    if (!impl_->live(entity) || !contains(entity)) { return {SelectionError::InvalidEntity}; }
    auto next = impl_->state;
    next.primary = entity;
    return impl_->commit(std::move(next), impl_->hover, SelectionReason::Primary, true);
}
SelectionResult SelectionService::set_hover(Entity entity, const SelectionFilter& filter) {
    if (entity && !eligible(entity, filter)) { return {SelectionError::InvalidEntity}; }
    return impl_->commit(impl_->state, entity, SelectionReason::Hover, false);
}
SelectionResult SelectionService::restore(const SelectionSnapshot& saved, const SelectionFilter& filter) {
    if (impl_->notifying) { return {SelectionError::Busy}; }
    if (!valid_filter(filter)) { return {SelectionError::InvalidArgument}; }
    if (saved.entities.size() > impl_->limits.selected) { return {SelectionError::Capacity}; }
    Impl::State next;
    EntitySet seen;
    std::size_t rejected{};
    for (const auto entity : saved.entities) {
        if (!eligible(entity, filter)) { ++rejected; continue; }
        if (seen.insert(entity).second) { next.entities.push_back(entity); }
    }
    next.primary = has(next.entities, saved.primary) ? saved.primary :
        (next.entities.empty() ? Entity{} : next.entities.back());
    auto result = impl_->commit(std::move(next), eligible(saved.hover, filter) ? saved.hover : Entity{},
                                SelectionReason::Restore, true);
    result.rejected = rejected;
    return result;
}
SelectionResult SelectionService::prune() {
    auto next = impl_->sanitize(impl_->state);
    return impl_->commit(std::move(next), impl_->live(impl_->hover) ? impl_->hover : Entity{},
                         SelectionReason::Prune, false);
}
SelectionResult SelectionService::undo() { return impl_->travel(impl_->undo, impl_->redo, SelectionReason::Undo); }
SelectionResult SelectionService::redo() { return impl_->travel(impl_->redo, impl_->undo, SelectionReason::Redo); }
void SelectionService::clear_history() noexcept {
    if (!impl_->notifying) { impl_->undo.clear(); impl_->redo.clear(); }
}

SelectionChanges SelectionService::changes_since(std::uint64_t cursor, std::size_t limit) const {
    SelectionChanges result;
    result.next_cursor = cursor;
    result.newest_revision = impl_->revision;
    result.history_lost = cursor > impl_->revision ||
        (!impl_->changes.empty() && cursor < impl_->changes.front().revision - 1);
    if (cursor > impl_->revision) { result.next_cursor = impl_->revision; return result; }
    limit = std::min(limit, impl_->limits.change_entries);
    for (const auto& change : impl_->changes) {
        if (change.revision <= cursor) { continue; }
        if (result.entries.size() >= limit) { break; }
        result.entries.push_back(change);
        result.next_cursor = change.revision;
    }
    return result;
}
std::uint64_t SelectionService::subscribe(Observer observer) {
    if (impl_->notifying || !observer || impl_->observers.size() >= impl_->limits.observers ||
        impl_->next_observer == std::numeric_limits<std::uint64_t>::max()) { return 0; }
    const auto token = impl_->next_observer;
    impl_->observers.emplace_back(token, std::move(observer));
    ++impl_->next_observer;
    return token;
}
bool SelectionService::unsubscribe(std::uint64_t token) noexcept {
    if (impl_->notifying) { return false; }
    const auto found = std::find_if(impl_->observers.begin(), impl_->observers.end(),
        [token](const auto& observer) { return observer.first == token; });
    if (found == impl_->observers.end()) { return false; }
    impl_->observers.erase(found);
    return true;
}

std::vector<Entity> SelectionService::selection_roots() const {
    std::vector<Entity> result;
    const EntitySet selected(impl_->state.entities.begin(), impl_->state.entities.end());
    for (const auto entity : impl_->state.entities) {
        if (!impl_->live(entity)) { continue; }
        bool covered{};
        std::size_t depth{};
        for (auto ancestor = impl_->scene.parent(entity); ancestor; ancestor = impl_->scene.parent(ancestor)) {
            if ((selected.contains(ancestor) && impl_->live(ancestor)) || ++depth > impl_->limits.hierarchy_depth) {
                covered = true;
                break;
            }
        }
        if (!covered) { result.push_back(entity); }
    }
    return result;
}
std::optional<math::Aabb> SelectionService::bounds(bool include_descendants) {
    if (impl_->scene.update().error != scene::SceneError::None) { return std::nullopt; }
    std::optional<math::Aabb> result;
    for (const auto entity : impl_->state.entities) {
        if (!impl_->live(entity)) { continue; }
        const auto box = include_descendants ? impl_->scene.subtree_bounds(entity) : impl_->scene.world_bounds(entity);
        if (box) { result = result ? math::merge(*result, *box) : *box; }
    }
    return result;
}
SelectionResult SelectionService::select_all(const SelectionFilter& filter) {
    if (impl_->scene.size() > impl_->limits.scene_entities) { return {SelectionError::Capacity}; }
    const auto entities = impl_->scene.ordered_entities();
    return apply(entities, SelectionMode::Replace, filter);
}
SelectionResult SelectionService::invert(const SelectionFilter& filter) {
    if (impl_->scene.size() > impl_->limits.scene_entities) { return {SelectionError::Capacity}; }
    std::vector<Entity> entities;
    for (const auto entity : impl_->scene.ordered_entities()) {
        if (!contains(entity)) { entities.push_back(entity); }
    }
    return apply(entities, SelectionMode::Replace, filter);
}
SelectionResult SelectionService::select_parents(const SelectionFilter& filter) {
    std::vector<Entity> entities;
    for (const auto entity : impl_->state.entities) {
        const auto parent = impl_->scene.parent(entity);
        if (parent) { entities.push_back(parent); }
    }
    return apply(entities, SelectionMode::Replace, filter);
}
SelectionResult SelectionService::select_children(bool recursive, const SelectionFilter& filter) {
    if (impl_->scene.size() > impl_->limits.scene_entities) { return {SelectionError::Capacity}; }
    std::vector<Entity> entities;
    const EntitySet selected(impl_->state.entities.begin(), impl_->state.entities.end());
    for (const auto entity : impl_->scene.ordered_entities()) {
        std::size_t depth{};
        for (auto parent = impl_->scene.parent(entity); parent; parent = impl_->scene.parent(parent)) {
            if (++depth > impl_->limits.hierarchy_depth) { return {SelectionError::Incomplete}; }
            if (selected.contains(parent)) { entities.push_back(entity); break; }
            if (!recursive) { break; }
        }
    }
    return apply(entities, SelectionMode::Replace, filter);
}

SelectionPick SelectionService::raycast(const math::Ray& ray, const geometry::MeshLibrary& meshes,
                                         spatial::SpatialIndex* spatial, const SelectionOptions& options) {
    SelectionPick result;
    result.ray = ray;
    if (!valid_options(options) || !math::valid(ray) || !math::normalize(result.ray.direction)) {
        result.report.error = SelectionError::InvalidArgument;
        result.report.complete = false;
        return result;
    }
    result.filter = options.filter;
    if (impl_->scene.update().error != scene::SceneError::None) {
        result.report.error = SelectionError::Busy;
        result.report.complete = false;
        return result;
    }
    auto candidates = impl_->candidates(meshes, spatial, options.filter, result.report, &result.ray,
                                        options.minimum_distance, options.maximum_distance, nullptr, false);
    result.report.scene_revision = impl_->scene.revision();
    for (const auto entity : candidates) {
        ++result.report.candidates_tested;
        bool exhausted{};
        const auto target = impl_->target(entity, options.hierarchy, options.filter, &exhausted);
        if (exhausted) { incomplete(result.report); }
        if (!target) { ++result.report.rejected; continue; }
        const auto* world = impl_->scene.cached_world_matrix(entity);
        const auto* component = impl_->scene.registry().get<geometry::MeshComponent>(entity);
        if (world == nullptr) { ++result.report.invalid_geometry; incomplete(result.report); continue; }
        SelectionHit closest;
        closest.entity = entity;
        closest.target = target;
        closest.distance = options.maximum_distance;
        bool found{};
        if (options.precision == SelectionPrecision::Bounds || (component == nullptr && options.bounds_for_non_mesh)) {
            const auto box = impl_->entity_bounds(entity, meshes);
            const auto hit = box ? math::intersect(result.ray, *box, options.minimum_distance, options.maximum_distance) : std::nullopt;
            if (hit) {
                closest.distance = hit->distance;
                closest.point = hit->point;
                closest.normal = hit->normal;
                closest.geometric_normal = hit->normal;
                closest.bounds_only = true;
                found = true;
            }
        } else if (component != nullptr) {
            const auto* mesh = meshes.get(component->mesh);
            const auto info = meshes.inspect(component->mesh);
            const auto inverse = math::inverse(*world);
            if (mesh == nullptr || !info || !inverse) {
                ++result.report.invalid_geometry;
                incomplete(result.report);
                continue;
            }
            closest.mesh = component->mesh;
            closest.mesh_revision = info->revision;
            const math::Ray local{math::transform_point(*inverse, result.ray.origin),
                                  math::transform_vector(*inverse, result.ray.direction)};
            const std::size_t ranges = mesh->submeshes.empty() ? 1 : mesh->submeshes.size();
            const auto first = static_cast<std::size_t>(component->first_submesh);
            const auto count = static_cast<std::size_t>(component->submesh_count);
            if (!math::valid(local) || first >= ranges || (count != 0 && count > ranges - first)) {
                ++result.report.invalid_geometry;
                incomplete(result.report);
                continue;
            }
            const auto last = count == 0 ? ranges : first + count;
            for (std::size_t range = first; range < last; ++range) {
                const std::size_t begin = mesh->submeshes.empty() ? 0 : mesh->submeshes[range].first_index;
                const std::size_t size = mesh->submeshes.empty() ? mesh->indices.size() : mesh->submeshes[range].index_count;
                if (begin > mesh->indices.size() || size > mesh->indices.size() - begin || size % 3 != 0) {
                    ++result.report.invalid_geometry;
                    incomplete(result.report);
                    continue;
                }
                for (std::size_t offset = begin; offset < begin + size; offset += 3) {
                    if (result.report.triangles_tested == impl_->limits.triangles) {
                        result.report.triangles_truncated = true;
                        incomplete(result.report);
                        break;
                    }
                    ++result.report.triangles_tested;
                    const auto a = static_cast<std::size_t>(mesh->indices[offset]);
                    const auto b = static_cast<std::size_t>(mesh->indices[offset + 1]);
                    const auto c = static_cast<std::size_t>(mesh->indices[offset + 2]);
                    if (a >= mesh->vertices.size() || b >= mesh->vertices.size() || c >= mesh->vertices.size()) {
                        ++result.report.invalid_geometry;
                        incomplete(result.report);
                        continue;
                    }
                    const auto& va = mesh->vertices[a];
                    const auto& vb = mesh->vertices[b];
                    const auto& vc = mesh->vertices[c];
                    // Keeping the inverse-transformed direction unnormalized preserves
                    // t in world metres even under nonuniform scale and affine shear.
                    const auto hit = math::intersect(local, math::Triangle{va.position, vb.position, vc.position},
                                                      options.minimum_distance, closest.distance, false);
                    if (!hit || (found && hit->distance >= closest.distance)) { continue; }
                    const auto edge_a = math::transform_vector(*world, vb.position - va.position);
                    const auto edge_b = math::transform_vector(*world, vc.position - va.position);
                    auto geometric_normal = math::cross(edge_a, edge_b);
                    if (!math::normalize(geometric_normal)) {
                        ++result.report.invalid_geometry;
                        incomplete(result.report);
                        continue;
                    }
                    if (options.cull_backfaces && math::dot(geometric_normal, result.ray.direction) >= 0.0) { continue; }
                    const auto barycentric = hit->barycentric;
                    const auto normal = va.normal * barycentric.x + vb.normal * barycentric.y + vc.normal * barycentric.z;
                    closest.distance = hit->distance;
                    closest.point = result.ray.point(hit->distance);
                    closest.geometric_normal = geometric_normal;
                    closest.normal = math::transform_normal(*world, normal).value_or(geometric_normal);
                    closest.barycentric = barycentric;
                    closest.uv = va.uv * barycentric.x + vb.uv * barycentric.y + vc.uv * barycentric.z;
                    closest.triangle = offset / 3;
                    closest.submesh = range;
                    found = true;
                }
                if (result.report.triangles_truncated) { break; }
            }
        }
        if (found) { result.hits.push_back(closest); }
        if (result.report.triangles_truncated) { break; }
    }
    std::sort(result.hits.begin(), result.hits.end(), hit_less);
    EntitySet targets;
    std::erase_if(result.hits, [&targets](const SelectionHit& hit) { return !targets.insert(hit.target).second; });
    if (result.hits.size() > impl_->limits.hits) {
        result.hits.resize(impl_->limits.hits);
        result.report.hits_truncated = true;
        incomplete(result.report);
    }
    impl_->account(result.report);
    return result;
}

SelectionPick SelectionService::pick(const render::CameraView& camera, math::Vec2 pixel,
                                      const geometry::MeshLibrary& meshes, spatial::SpatialIndex* spatial,
                                      const SelectionOptions& options) {
    if (!valid_options(options)) {
        SelectionPick rejected;
        rejected.report.error = SelectionError::InvalidArgument;
        rejected.report.complete = false;
        return rejected;
    }
    const auto ray = render::camera_ray(camera, pixel);
    const auto far = render::camera_unproject(camera, {pixel.x, pixel.y, 1.0});
    if (camera.stale || !ray || !far || !math::finite(camera.view_projection)) {
        SelectionPick rejected;
        rejected.report.error = SelectionError::InvalidCamera;
        rejected.report.complete = false;
        return rejected;
    }
    auto clipped = options;
    clipped.filter.layers &= camera.visibility_mask;
    const auto far_distance = math::dot(*far - ray->origin, math::normalized(ray->direction));
    if (!std::isfinite(far_distance) || far_distance < clipped.minimum_distance) {
        SelectionPick rejected;
        rejected.report.error = SelectionError::InvalidCamera;
        rejected.report.complete = false;
        return rejected;
    }
    clipped.maximum_distance = std::min(clipped.maximum_distance, far_distance);
    auto result = raycast(*ray, meshes, spatial, clipped);
    result.camera = camera.entity;
    result.camera_revision = camera.revision;
    return result;
}

SelectionVolume SelectionService::query_frustum(const math::Frustum& frustum,
                                                const geometry::MeshLibrary& meshes, spatial::SpatialIndex* spatial,
                                                const SelectionFilter& filter, SelectionContainment containment) {
    SelectionVolume result;
    if (!valid_filter(filter) || containment > SelectionContainment::Origin ||
        !std::all_of(frustum.planes.begin(), frustum.planes.end(), [](const auto& plane) { return math::valid(plane); })) {
        result.report.error = SelectionError::InvalidArgument;
        result.report.complete = false;
        return result;
    }
    if (impl_->scene.update().error != scene::SceneError::None) {
        result.report.error = SelectionError::Busy;
        result.report.complete = false;
        return result;
    }
    const auto candidates = impl_->candidates(meshes, spatial, filter, result.report, nullptr, 0.0, 0.0,
                                              &frustum, containment == SelectionContainment::Origin);
    result.report.scene_revision = impl_->scene.revision();
    for (const auto entity : candidates) {
        ++result.report.candidates_tested;
        if (containment == SelectionContainment::ContainBounds) {
            const auto bounds = impl_->entity_bounds(entity, meshes);
            if (!bounds) { continue; }
            const auto corners = box_corners(*bounds);
            if (!std::all_of(corners.begin(), corners.end(), [&frustum](const auto& corner) {
                return math::contains(frustum, corner);
            })) { continue; }
        }
        if (result.entities.size() == impl_->limits.hits) {
            result.report.hits_truncated = true;
            incomplete(result.report);
            break;
        }
        result.entities.push_back(entity);
    }
    std::sort(result.entities.begin(), result.entities.end());
    impl_->account(result.report);
    return result;
}

SelectionVolume SelectionService::marquee(const render::CameraView& camera, math::Vec2 start, math::Vec2 end,
                                           const geometry::MeshLibrary& meshes, spatial::SpatialIndex* spatial,
                                           const SelectionFilter& filter, SelectionContainment containment) {
    SelectionVolume rejected;
    rejected.report.error = SelectionError::InvalidCamera;
    rejected.report.complete = false;
    if (camera.stale || camera.viewport.empty() || !math::finite(start) || !math::finite(end)) { return rejected; }
    const double x = static_cast<double>(camera.viewport.x);
    const double y = static_cast<double>(camera.viewport.y);
    const double width = static_cast<double>(camera.viewport.width);
    const double height = static_cast<double>(camera.viewport.height);
    const double left = std::clamp(std::min(start.x, end.x), x, x + width);
    const double right = std::clamp(std::max(start.x, end.x), x, x + width);
    const double top = std::clamp(std::min(start.y, end.y), y, y + height);
    const double bottom = std::clamp(std::max(start.y, end.y), y, y + height);
    if (right - left <= math::epsilon || bottom - top <= math::epsilon) { return rejected; }
    const double ndc_left = 2.0 * (left - x) / width - 1.0;
    const double ndc_right = 2.0 * (right - x) / width - 1.0;
    const double ndc_top = 1.0 - 2.0 * (top - y) / height;
    const double ndc_bottom = 1.0 - 2.0 * (bottom - y) / height;
    math::Mat4 crop;
    crop(0, 0) = 2.0 / (ndc_right - ndc_left);
    crop(1, 1) = 2.0 / (ndc_top - ndc_bottom);
    crop(0, 3) = -(ndc_right + ndc_left) / (ndc_right - ndc_left);
    crop(1, 3) = -(ndc_top + ndc_bottom) / (ndc_top - ndc_bottom);
    const auto frustum = math::extract_frustum(crop * camera.view_projection, camera.clip_depth);
    if (!frustum) { return rejected; }
    auto visible_filter = filter;
    visible_filter.layers &= camera.visibility_mask;
    return query_frustum(*frustum, meshes, spatial, visible_filter, containment);
}

SelectionResult SelectionService::choose(const SelectionPick& picked, int cycle_steps, SelectionMode mode) {
    if (impl_->notifying) { return {SelectionError::Busy}; }
    if (mode > SelectionMode::Subtract || !valid_filter(picked.filter)) { return {SelectionError::InvalidArgument}; }
    if (!picked.report.complete || picked.report.error != SelectionError::None ||
        picked.report.scene_revision != impl_->scene.revision()) { return {SelectionError::Incomplete}; }
    if (picked.hits.size() > impl_->limits.hits) { return {SelectionError::Capacity}; }
    if (picked.hits.empty()) {
        reset_cycle();
        return mode == SelectionMode::Replace ? clear() : SelectionResult{};
    }
    std::vector<Entity> targets;
    for (const auto& hit : picked.hits) {
        if (!eligible(hit.target, picked.filter) || !impl_->live(hit.entity)) { return {SelectionError::InvalidEntity}; }
        targets.push_back(hit.target);
    }
    auto canonical = targets;
    std::sort(canonical.begin(), canonical.end());
    canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());
    if (canonical.size() != targets.size()) { return {SelectionError::InvalidArgument}; }
    const bool same_cycle = canonical == impl_->cycle_targets && impl_->cycle_camera == picked.camera &&
        math::almost_equal(impl_->cycle_ray.origin, picked.ray.origin, 1.0e-6) &&
        math::almost_equal(impl_->cycle_ray.direction, picked.ray.direction, 1.0e-10);
    std::int64_t cursor = cycle_steps < 0 ? 0 : -1;
    if (same_cycle) {
        const auto found = std::find(targets.begin(), targets.end(), impl_->cycle_target);
        if (found != targets.end()) { cursor = static_cast<std::int64_t>(std::distance(targets.begin(), found)); }
    }
    const auto size = static_cast<std::int64_t>(targets.size());
    cursor = cycle_steps == 0 ? 0 : ((cursor + static_cast<std::int64_t>(cycle_steps)) % size + size) % size;
    const Entity selected = targets[static_cast<std::size_t>(cursor)];
    auto result = apply(std::span<const Entity>{&selected, 1}, mode, picked.filter);
    if (result) {
        impl_->cycle_targets = std::move(canonical);
        impl_->cycle_target = selected;
        impl_->cycle_ray = picked.ray;
        impl_->cycle_camera = picked.camera;
    }
    return result;
}
void SelectionService::reset_cycle() noexcept {
    if (!impl_->notifying) { impl_->cycle_targets.clear(); impl_->cycle_target = {}; }
}
void SelectionService::set_interaction_enabled(bool enabled) noexcept {
    if (impl_->notifying) { return; }
    impl_->interaction_enabled = enabled;
    if (!enabled) { impl_->pending.clear(); reset_cycle(); }
}
void SelectionService::set_pointer_captured(bool captured) noexcept {
    if (impl_->notifying) { return; }
    impl_->pointer_captured = captured;
    if (captured) { impl_->pending.clear(); reset_cycle(); }
}
bool SelectionService::set_pointer_options(const SelectionOptions& options) {
    if (impl_->notifying || !valid_options(options)) { return false; }
    impl_->pointer_options = options;
    reset_cycle();
    return true;
}
SelectionResult SelectionService::queue_pointer(SelectionPointerRequest request) {
    if (impl_->notifying) { return {SelectionError::Busy}; }
    if (!impl_->interaction_enabled) { return {SelectionError::Inactive}; }
    if (impl_->pointer_captured || request.ui_captured) { return {SelectionError::Captured}; }
    if (!math::finite(request.pixel) || request.mode > SelectionMode::Subtract) { return {SelectionError::InvalidArgument}; }
    if (request.hover_only) {
        std::erase_if(impl_->pending, [](const auto& entry) { return entry.hover_only; });
    }
    if (impl_->pending.size() >= impl_->limits.pointer_requests) { return {SelectionError::Capacity}; }
    impl_->pending.push_back(request);
    return {SelectionError::None, 1};
}
void SelectionService::frame(const render::CameraService* cameras, const geometry::MeshLibrary* meshes,
                              spatial::SpatialIndex* spatial) {
    if (impl_->notifying) { return; }
    (void)prune();
    auto requests = std::move(impl_->pending);
    impl_->pending.clear();
    if (!impl_->interaction_enabled || impl_->pointer_captured || cameras == nullptr || meshes == nullptr) {
        (void)set_hover({});
        return;
    }
    for (const auto& request : requests) {
        if (request.ui_captured) { continue; }
        const auto* camera = cameras->view(request.view_slot);
        if (camera == nullptr) { (void)set_hover({}); continue; }
        const auto picked = pick(*camera, request.pixel, *meshes, spatial, impl_->pointer_options);
        if (!picked.report.complete) { (void)set_hover({}); continue; }
        const auto hovered = picked.hits.empty() ? Entity{} : picked.hits.front().target;
        (void)set_hover(hovered, picked.filter);
        if (!request.hover_only) { (void)choose(picked, request.cycle_steps, request.mode); }
    }
}

void register_selection(core::Kernel& kernel) {
    if (!kernel.register_module({"editor.selection", "Generation-safe selection and current Scene geometry picking", 1})) { return; }
    auto* scene_service = kernel.services().find<scene::Scene>();
    if (scene_service == nullptr) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "editor.selection", "Scene service is required");
        return;
    }
    (void)scene_service->registry().register_component<SelectionComponent>("editor.selection");
    kernel.services().emplace<SelectionService>(*scene_service);
    (void)kernel.add_system({"editor.selection.prune", core::Phase::PreSimulation, -8500, {}},
        [](core::Kernel& host, const core::Frame&) { (void)host.services().get<SelectionService>().prune(); });
    std::vector<std::string> after;
    if (kernel.services().find<spatial::SpatialIndex>() != nullptr) { after.emplace_back("spatial.sync"); }
    if (kernel.services().find<render::CameraService>() != nullptr) { after.emplace_back("camera.update"); }
    (void)kernel.add_system({"editor.selection.frame", core::Phase::Visibility, 100, std::move(after)},
        [](core::Kernel& host, const core::Frame&) {
            auto& service = host.services().get<SelectionService>();
            service.frame(host.services().find<render::CameraService>(), host.services().find<geometry::MeshLibrary>(),
                           host.services().find<spatial::SpatialIndex>());
            const auto status = service.statistics();
            host.metric("selection.selected", static_cast<double>(status.selected));
            host.metric("selection.incomplete_queries", static_cast<double>(status.incomplete_queries));
            host.metric("selection.observer_failures", static_cast<double>(status.observer_failures));
        });
    (void)kernel.add_command("selection.set", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty()) { return {false, "usage: selection.set replace|add|toggle|subtract [index:generation ...]"}; }
        const auto mode = parse_mode(args[0]);
        if (!mode) { return {false, "invalid selection mode"}; }
        auto& service = host.services().get<SelectionService>();
        if (args.size() - 1 > service.limits().scene_entities) { return {false, "entity input limit exceeded"}; }
        std::vector<Entity> entities;
        for (std::size_t index = 1; index < args.size(); ++index) {
            const auto entity = ecs::parse_entity(args[index]);
            if (!entity) { return {false, "invalid generation-qualified entity"}; }
            entities.push_back(*entity);
        }
        return command_result(service.apply(entities, *mode));
    }, "Edit selection membership using generation-qualified registry handles");
    (void)kernel.add_command("selection.inspect", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: selection.inspect"}; }
        const auto saved = host.services().get<SelectionService>().snapshot();
        std::string message = "count=" + std::to_string(saved.entities.size()) + " primary=" + ecs::format_entity(saved.primary) +
            " hover=" + ecs::format_entity(saved.hover) + " revision=" + std::to_string(saved.revision);
        const auto count = std::min<std::size_t>(saved.entities.size(), 128);
        for (std::size_t index = 0; index < count; ++index) { message += "\n" + ecs::format_entity(saved.entities[index]); }
        if (count < saved.entities.size()) { message += "\nentity listing truncated at 128"; }
        return {true, std::move(message)};
    }, "Inspect primary, hover and an ordered selection snapshot");
    (void)kernel.add_command("selection.history", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: selection.history undo|redo|clear"}; }
        auto& service = host.services().get<SelectionService>();
        if (args[0] == "undo") { return command_result(service.undo()); }
        if (args[0] == "redo") { return command_result(service.redo()); }
        if (args[0] == "clear") { service.clear_history(); return {true, "selection undo and redo history cleared"}; }
        return {false, "unknown history operation"};
    }, "Use selection-only history independently from scene command history");
    (void)kernel.add_command("selection.expand", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: selection.expand all|invert|parents|children|descendants|roots|clear"}; }
        auto& service = host.services().get<SelectionService>();
        if (args[0] == "all") { return command_result(service.select_all()); }
        if (args[0] == "invert") { return command_result(service.invert()); }
        if (args[0] == "parents") { return command_result(service.select_parents()); }
        if (args[0] == "children") { return command_result(service.select_children(false)); }
        if (args[0] == "descendants") { return command_result(service.select_children(true)); }
        if (args[0] == "roots") { const auto roots = service.selection_roots(); return command_result(service.apply(roots)); }
        if (args[0] == "clear") { return command_result(service.clear()); }
        return {false, "unknown hierarchy selection operation"};
    }, "Select eligible hierarchy relations or invert the current selection");
    (void)kernel.add_command("selection.policy", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 5) { return {false, "usage: selection.policy entity selectable locked hidden group_root (flags 0|1)"}; }
        const auto entity = ecs::parse_entity(args[0]);
        std::array<bool, 4> flags{};
        for (std::size_t index = 0; index < flags.size(); ++index) {
            if (args[index + 1] != "0" && args[index + 1] != "1") { return {false, "flags must be 0 or 1"}; }
            flags[index] = args[index + 1] == "1";
        }
        const bool changed = entity && host.services().get<SelectionService>().set_policy(*entity,
            {flags[0], flags[1], flags[2], flags[3]});
        return {changed, changed ? "editor selection policy updated" : "entity unavailable or observer active"};
    }, "Set editor selection metadata without changing rendering or physics");
    (void)kernel.add_command("selection.pick", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 5) { return {false, "usage: selection.pick x y [mode] [cycle_steps] [view_slot]"}; }
        const auto x = parse_number<double>(args[0]);
        const auto y = parse_number<double>(args[1]);
        const auto mode = args.size() >= 3 ? parse_mode(args[2]) : std::optional{SelectionMode::Replace};
        const auto steps = args.size() >= 4 ? parse_number<int>(args[3]) : std::optional{0};
        const auto slot = args.size() >= 5 ? parse_number<std::size_t>(args[4]) : std::optional<std::size_t>{0};
        const auto* cameras = host.services().find<render::CameraService>();
        const auto* meshes = host.services().find<geometry::MeshLibrary>();
        const auto* camera = cameras != nullptr && slot ? cameras->view(*slot) : nullptr;
        if (!x || !y || !mode || !steps || camera == nullptr || meshes == nullptr) { return {false, "invalid input or missing camera/mesh service"}; }
        auto& service = host.services().get<SelectionService>();
        const auto picked = service.pick(*camera, {*x, *y}, *meshes, host.services().find<spatial::SpatialIndex>());
        return command_result(service.choose(picked, *steps, *mode));
    }, "Pick current base geometry using continuous top-left camera pixels; no shader/depth visibility test");
    (void)kernel.add_command("selection.marquee", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 4 || args.size() > 5) { return {false, "usage: selection.marquee x1 y1 x2 y2 [mode]"}; }
        std::array<double, 4> coordinates{};
        for (std::size_t index = 0; index < coordinates.size(); ++index) {
            const auto value = parse_number<double>(args[index]);
            if (!value) { return {false, "invalid marquee coordinate"}; }
            coordinates[index] = *value;
        }
        const auto mode = args.size() == 5 ? parse_mode(args[4]) : std::optional{SelectionMode::Replace};
        const auto* cameras = host.services().find<render::CameraService>();
        const auto* meshes = host.services().find<geometry::MeshLibrary>();
        const auto* camera = cameras != nullptr ? cameras->view(0) : nullptr;
        if (!mode || camera == nullptr || meshes == nullptr) { return {false, "invalid mode or missing camera/mesh service"}; }
        auto& service = host.services().get<SelectionService>();
        const auto query = service.marquee(*camera, {coordinates[0], coordinates[1]}, {coordinates[2], coordinates[3]},
                                           *meshes, host.services().find<spatial::SpatialIndex>());
        if (!query.report.complete) { return {false, std::string(describe(query.report.error))}; }
        return command_result(service.apply(query.entities, *mode));
    }, "Select current geometry bounds intersecting a camera-clipped marquee");
    (void)kernel.add_command("selection.interaction", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2 || (args[0] != "0" && args[0] != "1") || (args[1] != "0" && args[1] != "1")) {
            return {false, "usage: selection.interaction enabled pointer_captured (flags 0|1)"};
        }
        auto& service = host.services().get<SelectionService>();
        service.set_interaction_enabled(args[0] == "1");
        service.set_pointer_captured(args[1] == "1");
        return {true, "explicit pointer request gates updated; host must queue requests after UI arbitration"};
    }, "Gate explicitly queued editor pointer requests without binding gameplay input");
}

} // namespace ge::editor
