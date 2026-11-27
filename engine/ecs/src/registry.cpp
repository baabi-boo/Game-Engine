#include <ge/ecs/registry.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>

#include <charconv>
#include <cmath>
#include <sstream>

namespace ge::ecs {
namespace {

[[nodiscard]] std::size_t bounded_history(std::size_t value) {
    if (value > 1000000) {
        throw std::length_error("registry history limit exceeds supported bounds");
    }
    return value;
}

[[nodiscard]] bool valid_name(std::string_view text) noexcept {
    return text.size() <= 1024 && core::valid_utf8(text) && text.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool valid_tag(std::string_view text) noexcept {
    if (text.empty() || text.size() > 128 || !core::valid_utf8(text)) {
        return false;
    }
    return std::none_of(text.begin(), text.end(), [](char value) {
        return static_cast<unsigned char>(value) < 0x20U;
    });
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<double> parse_real(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    double result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] Entity resolve_entity(const Registry& registry, std::string_view text) {
    if (text.starts_with('@')) {
        const auto persistent = parse_unsigned(text.substr(1));
        return persistent ? registry.find(PersistentId{*persistent}) : null_entity;
    }
    if (const auto parsed = parse_entity(text)) {
        return registry.alive(*parsed) ? *parsed : null_entity;
    }
    return registry.find_name(text);
}

[[nodiscard]] std::size_t saturated_add(std::size_t a, std::size_t b) noexcept {
    return b > std::numeric_limits<std::size_t>::max() - a
        ? std::numeric_limits<std::size_t>::max() : a + b;
}

} // namespace

Registry::Registry(std::uint32_t entity_capacity, std::size_t component_type_limit,
                   std::size_t deferred_limit, std::size_t history_limit)
    : handles_(entity_capacity), component_type_limit_(component_type_limit),
      deferred_limit_(deferred_limit), history_(bounded_history(history_limit)) {
    if (entity_capacity > 10000000U || component_type_limit > 65535 ||
        deferred_limit > 1000000 || history_limit > 1000000) {
        throw std::length_error("registry limits exceed supported bounds");
    }
    live_.reserve(std::min<std::size_t>(entity_capacity, 1024));
    records_.reserve(std::min<std::size_t>(entity_capacity, 1024));
}

Registry::~Registry() = default;

void Registry::require_alive(Entity entity) const {
    if (!alive(entity)) {
        throw std::out_of_range("entity handle is invalid or stale");
    }
}

void Registry::require_structural_access() const {
    if (iteration_depth_ != 0) {
        throw std::logic_error("structural mutation during registry iteration must be deferred");
    }
}

Registry::Record* Registry::record_for(Entity entity) noexcept {
    return alive(entity) ? &records_[static_cast<std::size_t>(entity.index - 1)] : nullptr;
}

const Registry::Record* Registry::record_for(Entity entity) const noexcept {
    return alive(entity) ? &records_[static_cast<std::size_t>(entity.index - 1)] : nullptr;
}

void Registry::record(EntityEvent::Kind kind, Entity entity, ComponentType type) noexcept {
    if (revision_ != std::numeric_limits<std::uint64_t>::max()) {
        ++revision_;
    }
    auto* current = record_for(entity);
    const auto persistent = current ? current->persistent : PersistentId{};
    if (current) {
        current->revision = revision_;
    }
    (void)history_.push_overwrite(EntityEvent{kind, entity, persistent, type, revision_});
}

Entity Registry::create(PersistentId persistent, std::string_view entity_name) {
    require_structural_access();
    if (!valid_name(entity_name)) {
        throw std::invalid_argument("entity name is not bounded UTF-8 text");
    }
    if (persistent && persistent_.contains(persistent.value)) {
        return null_entity;
    }
    Pool<NameComponent>* names = entity_name.empty() ? nullptr : &ensure_pool<NameComponent>("Name");
    const auto acquired = handles_.acquire();
    if (!acquired) {
        return null_entity;
    }
    const auto entity = *acquired;
    bool appended{};
    bool indexed{};
    try {
        if (records_.size() < entity.index) {
            records_.resize(entity.index);
        }
        auto& destination = records_[static_cast<std::size_t>(entity.index - 1)];
        destination = Record{};
        destination.persistent = persistent;
        destination.dense_index = live_.size();
        live_.push_back(entity);
        appended = true;
        if (persistent) {
            const auto inserted = persistent_.emplace(persistent.value, entity);
            if (!inserted.second) {
                throw std::logic_error("persistent entity identity became duplicated");
            }
            indexed = true;
        }
        if (names) {
            (void)names->insert(entity, NameComponent{std::string(entity_name)});
        }
    } catch (...) {
        if (names) {
            names->erase(entity);
        }
        if (indexed) {
            persistent_.erase(persistent.value);
        }
        if (appended) {
            live_.pop_back();
        }
        (void)handles_.release(entity);
        throw;
    }
    record(EntityEvent::Kind::Created, entity);
    if (names) {
        record(EntityEvent::Kind::ComponentAdded, entity, names->id);
    }
    return entity;
}

bool Registry::destroy(Entity entity) {
    if (!alive(entity)) {
        return false;
    }
    require_structural_access();
    auto& current = records_[static_cast<std::size_t>(entity.index - 1)];
    record(EntityEvent::Kind::Destroyed, entity);
    for (auto& [type, storage] : pools_) {
        (void)type;
        storage->erase(entity);
    }
    if (current.persistent) {
        persistent_.erase(current.persistent.value);
    }
    const auto dense_index = current.dense_index;
    const auto moved = live_.back();
    live_[dense_index] = moved;
    records_[static_cast<std::size_t>(moved.index - 1)].dense_index = dense_index;
    live_.pop_back();
    current = Record{};
    return handles_.release(entity);
}

void Registry::clear() {
    require_structural_access();
    while (!live_.empty()) {
        (void)destroy(live_.back());
    }
}

bool Registry::alive(Entity entity) const noexcept {
    return handles_.alive(entity);
}

std::size_t Registry::size() const noexcept {
    return live_.size();
}

std::uint32_t Registry::capacity() const noexcept {
    return handles_.capacity();
}

std::vector<Entity> Registry::entities(bool include_disabled) const {
    std::vector<Entity> result;
    result.reserve(live_.size());
    for (const auto entity : live_) {
        if (include_disabled || enabled(entity)) {
            result.push_back(entity);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::uint64_t Registry::revision() const noexcept {
    return revision_;
}

std::uint64_t Registry::entity_revision(Entity entity) const noexcept {
    const auto* current = record_for(entity);
    return current ? current->revision : 0;
}

PersistentId Registry::persistent_id(Entity entity) const noexcept {
    const auto* current = record_for(entity);
    return current ? current->persistent : PersistentId{};
}

bool Registry::set_persistent_id(Entity entity, PersistentId persistent) {
    auto* current = record_for(entity);
    if (!current) {
        return false;
    }
    if (current->persistent == persistent) {
        return true;
    }
    if (persistent) {
        if (persistent_.contains(persistent.value)) {
            return false;
        }
        persistent_.emplace(persistent.value, entity);
    }
    if (current->persistent) {
        persistent_.erase(current->persistent.value);
    }
    current->persistent = persistent;
    record(EntityEvent::Kind::IdentityChanged, entity);
    return true;
}

Entity Registry::find(PersistentId persistent) const noexcept {
    const auto it = persistent_.find(persistent.value);
    return it == persistent_.end() ? null_entity : it->second;
}

Entity Registry::find_name(std::string_view entity_name) const {
    Entity result{};
    for (const auto entity : live_) {
        if (name(entity) == entity_name && (!result || entity < result)) {
            result = entity;
        }
    }
    return result;
}

std::vector<Entity> Registry::find_all_named(std::string_view entity_name) const {
    std::vector<Entity> result;
    for (const auto entity : live_) {
        if (name(entity) == entity_name) {
            result.push_back(entity);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string_view Registry::name(Entity entity) const noexcept {
    const auto* component = get<NameComponent>(entity);
    return component ? std::string_view(component->value) : std::string_view{};
}

bool Registry::set_name(Entity entity, std::string_view entity_name) {
    if (!alive(entity) || !valid_name(entity_name)) {
        return false;
    }
    if (auto* component = get<NameComponent>(entity)) {
        std::string replacement(entity_name);
        component->value.swap(replacement);
        record(EntityEvent::Kind::ComponentChanged, entity, component_type<NameComponent>());
    } else {
        (void)emplace<NameComponent>(entity, NameComponent{std::string(entity_name)});
    }
    return true;
}

bool Registry::enabled(Entity entity) const noexcept {
    const auto* current = record_for(entity);
    return current && current->enabled;
}

bool Registry::set_enabled(Entity entity, bool value) {
    auto* current = record_for(entity);
    if (!current) {
        return false;
    }
    if (current->enabled != value) {
        current->enabled = value;
        record(EntityEvent::Kind::EnabledChanged, entity);
    }
    return true;
}

std::uint64_t Registry::layers(Entity entity) const noexcept {
    const auto* current = record_for(entity);
    return current ? current->layers : 0;
}

bool Registry::set_layers(Entity entity, std::uint64_t value) {
    auto* current = record_for(entity);
    if (!current) {
        return false;
    }
    if (current->layers != value) {
        current->layers = value;
        record(EntityEvent::Kind::LayerChanged, entity);
    }
    return true;
}

bool Registry::add_tag(Entity entity, std::string_view tag) {
    auto* current = record_for(entity);
    if (!current || !valid_tag(tag)) {
        return false;
    }
    auto& values = current->tags;
    const auto position = std::lower_bound(values.begin(), values.end(), tag);
    if (position != values.end() && *position == tag) {
        return true;
    }
    if (values.size() >= 64) {
        return false;
    }
    values.insert(position, std::string(tag));
    record(EntityEvent::Kind::TagsChanged, entity);
    return true;
}

bool Registry::remove_tag(Entity entity, std::string_view tag) {
    auto* current = record_for(entity);
    if (!current) {
        return false;
    }
    auto& values = current->tags;
    const auto position = std::lower_bound(values.begin(), values.end(), tag);
    if (position == values.end() || *position != tag) {
        return false;
    }
    values.erase(position);
    record(EntityEvent::Kind::TagsChanged, entity);
    return true;
}

bool Registry::has_tag(Entity entity, std::string_view tag) const {
    const auto* current = record_for(entity);
    return current && std::binary_search(current->tags.begin(), current->tags.end(), tag);
}

std::vector<std::string> Registry::tags(Entity entity) const {
    const auto* current = record_for(entity);
    return current ? current->tags : std::vector<std::string>{};
}

Registry::PoolBase* Registry::pool_by_id(ComponentType type) noexcept {
    for (auto& [index, storage] : pools_) {
        (void)index;
        if (storage->id == type) {
            return storage.get();
        }
    }
    return nullptr;
}

const Registry::PoolBase* Registry::pool_by_id(ComponentType type) const noexcept {
    for (const auto& [index, storage] : pools_) {
        (void)index;
        if (storage->id == type) {
            return storage.get();
        }
    }
    return nullptr;
}

bool Registry::has(Entity entity, ComponentType type) const noexcept {
    if (!alive(entity)) {
        return false;
    }
    const auto* storage = pool_by_id(type);
    return storage && storage->contains(entity);
}

bool Registry::remove(Entity entity, ComponentType type) {
    if (!alive(entity)) {
        return false;
    }
    auto* storage = pool_by_id(type);
    if (!storage || !storage->contains(entity)) {
        return false;
    }
    require_structural_access();
    storage->erase(entity);
    record(EntityEvent::Kind::ComponentRemoved, entity, type);
    return true;
}

std::vector<ComponentInfo> Registry::component_info() const {
    std::vector<ComponentInfo> result;
    result.reserve(pools_.size());
    for (const auto& [index, storage] : pools_) {
        (void)index;
        result.push_back({storage->id, storage->name, storage->size(),
                          storage->element_bytes, storage->cloneable()});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    return result;
}

std::optional<EntityInfo> Registry::inspect(Entity entity) const {
    const auto* current = record_for(entity);
    if (!current) {
        return std::nullopt;
    }
    EntityInfo result;
    result.entity = entity;
    result.persistent = current->persistent;
    result.name = name(entity);
    result.enabled = current->enabled;
    result.layers = current->layers;
    result.tags = current->tags;
    result.revision = current->revision;
    for (const auto& [index, storage] : pools_) {
        (void)index;
        if (storage->contains(entity)) {
            result.components.push_back(storage->id);
        }
    }
    std::sort(result.components.begin(), result.components.end());
    return result;
}

bool Registry::matches(Entity entity, const Query& specification) const {
    if ((!specification.include_disabled && !enabled(entity)) ||
        (layers(entity) & specification.layers) == 0) {
        return false;
    }
    for (const auto type : specification.all) {
        if (!has(entity, type)) {
            return false;
        }
    }
    for (const auto type : specification.none) {
        if (has(entity, type)) {
            return false;
        }
    }
    if (!specification.any.empty() && std::none_of(specification.any.begin(), specification.any.end(),
        [&](ComponentType type) { return has(entity, type); })) {
        return false;
    }
    for (const auto& tag : specification.tags) {
        if (!has_tag(entity, tag)) {
            return false;
        }
    }
    return specification.name_contains.empty() ||
        name(entity).find(specification.name_contains) != std::string_view::npos;
}

std::vector<Entity> Registry::query(const Query& specification) const {
    if (specification.all.size() > component_type_limit_ ||
        specification.any.size() > component_type_limit_ ||
        specification.none.size() > component_type_limit_ || specification.tags.size() > 64 ||
        specification.name_contains.size() > 1024) {
        throw std::length_error("query exceeds registry bounds");
    }
    std::vector<Entity> result;
    const auto selected = entities(specification.include_disabled);
    result.reserve(std::min(specification.limit, selected.size()));
    for (const auto entity : selected) {
        if (result.size() >= specification.limit) {
            break;
        }
        if (matches(entity, specification)) {
            result.push_back(entity);
        }
    }
    return result;
}

Entity Registry::clone(Entity source, PersistentId persistent, std::string_view entity_name) {
    if (!alive(source)) {
        return null_entity;
    }
    require_structural_access();
    if (!valid_name(entity_name)) {
        throw std::invalid_argument("clone name is not bounded UTF-8 text");
    }
    for (const auto& [index, storage] : pools_) {
        (void)index;
        if (storage->contains(source) && !storage->cloneable()) {
            return null_entity;
        }
    }
    const auto source_tags = tags(source);
    const auto source_enabled = enabled(source);
    const auto source_layers = layers(source);
    const auto destination = create(persistent);
    if (!destination) {
        return null_entity;
    }
    try {
        for (auto& [index, storage] : pools_) {
            (void)index;
            if (storage->contains(source)) {
                storage->clone(source, destination);
                record(EntityEvent::Kind::ComponentAdded, destination, storage->id);
            }
        }
        auto& current = records_[static_cast<std::size_t>(destination.index - 1)];
        current.tags = source_tags;
        current.enabled = source_enabled;
        current.layers = source_layers;
        if (!entity_name.empty()) {
            (void)set_name(destination, entity_name);
        }
    } catch (...) {
        (void)destroy(destination);
        throw;
    }
    return destination;
}

ChangeBatch Registry::changes_since(std::uint64_t since, std::size_t limit) const {
    ChangeBatch result;
    result.newest_revision = since;
    if (since > revision_) {
        result.history_lost = true;
        result.newest_revision = revision_;
        return result;
    }
    if (since == revision_) {
        return result;
    }
    if (history_.empty()) {
        result.history_lost = true;
        result.newest_revision = revision_;
        return result;
    }
    const auto oldest = history_.front()->revision;
    result.history_lost = since < oldest && oldest - since > 1;
    result.events.reserve(std::min(limit, history_.size()));
    history_.for_each([&](const EntityEvent& event) {
        if (event.revision > since && result.events.size() < limit) {
            result.events.push_back(event);
            result.newest_revision = event.revision;
        }
    });
    return result;
}

RegistryStatistics Registry::statistics() const {
    RegistryStatistics result;
    result.entities = live_.size();
    result.component_types = pools_.size();
    result.queued_commands = deferred_.size();
    result.revision = revision_;
    result.deferred_failures = deferred_failures_;
    result.retired_slots = handles_.retired_slots();
    for (const auto entity : live_) {
        if (enabled(entity)) {
            ++result.enabled_entities;
        }
    }
    for (const auto& [index, storage] : pools_) {
        (void)index;
        result.component_instances = saturated_add(result.component_instances, storage->size());
        const auto bytes = storage->size() > std::numeric_limits<std::size_t>::max() / storage->element_bytes
            ? std::numeric_limits<std::size_t>::max() : storage->size() * storage->element_bytes;
        result.component_payload_bytes = saturated_add(result.component_payload_bytes, bytes);
    }
    return result;
}

std::optional<std::uint64_t> Registry::defer(std::function<void(Registry&)> function,
                                             std::string label) {
    if (!function || label.size() > 1024 || deferred_.size() >= deferred_limit_ ||
        next_ticket_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    const auto ticket = next_ticket_;
    deferred_.push_back({ticket, std::move(label), std::move(function)});
    ++next_ticket_;
    return ticket;
}

std::optional<std::uint64_t> Registry::defer_destroy(Entity entity) {
    if (!alive(entity)) {
        return std::nullopt;
    }
    return defer([entity](Registry& registry) {
        (void)registry.destroy(entity);
    }, "destroy entity");
}

std::optional<std::uint64_t> Registry::defer_create(
    PersistentId persistent, std::string entity_name,
    std::function<void(Registry&, Entity)> initialize) {
    if (!valid_name(entity_name)) {
        return std::nullopt;
    }
    return defer([persistent, entity_name = std::move(entity_name), initialize = std::move(initialize)]
        (Registry& registry) {
        const auto entity = registry.create(persistent, entity_name);
        if (!entity) {
            throw std::runtime_error("deferred entity creation was rejected");
        }
        try {
            if (initialize) {
                initialize(registry, entity);
            }
        } catch (...) {
            (void)registry.destroy(entity);
            throw;
        }
    }, "create entity");
}

std::vector<DeferredResult> Registry::flush_deferred(std::size_t limit) {
    require_structural_access();
    if (flushing_) {
        throw std::logic_error("recursive deferred command flush is not permitted");
    }
    const auto count = std::min(limit, deferred_.size());
    std::vector<DeferredResult> results;
    results.reserve(count);
    struct FlushScope {
        bool& active;
        explicit FlushScope(bool& value) : active(value) { active = true; }
        ~FlushScope() { active = false; }
    } scope(flushing_);
    for (std::size_t index = 0; index < count && !deferred_.empty(); ++index) {
        auto pending = std::move(deferred_.front());
        deferred_.pop_front();
        DeferredResult result{pending.ticket, true, pending.label};
        try {
            pending.function(*this);
        } catch (const std::exception& error) {
            result.success = false;
            result.message = pending.label + ": " + error.what();
        } catch (...) {
            result.success = false;
            result.message = pending.label + ": non-standard exception";
        }
        if (!result.success && deferred_failures_ != std::numeric_limits<std::uint64_t>::max()) {
            ++deferred_failures_;
        }
        results.push_back(std::move(result));
    }
    return results;
}

std::size_t Registry::deferred_count() const noexcept {
    return deferred_.size();
}

std::size_t Registry::discard_deferred() noexcept {
    const auto count = deferred_.size();
    deferred_.clear();
    return count;
}

bool Registry::iterating() const noexcept {
    return iteration_depth_ != 0;
}

std::string format_entity(Entity entity) {
    return std::to_string(entity.index) + ":" + std::to_string(entity.generation);
}

std::optional<Entity> parse_entity(std::string_view text) noexcept {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto index = parse_unsigned(text.substr(0, colon));
    const auto generation = parse_unsigned(text.substr(colon + 1));
    if (!index || !generation || *index == 0 || *generation == 0 ||
        *index > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return Entity{static_cast<std::uint32_t>(*index), *generation};
}

void register_registry(core::Kernel& kernel) {
    const auto configured = kernel.config().integer("ecs.capacity", 65536);
    const auto bounded = std::clamp<std::int64_t>(configured, 1, 10000000);
    auto& registry = kernel.services().emplace<Registry>(static_cast<std::uint32_t>(bounded));
    registry.register_component<TransformComponent>("Transform");
    registry.register_component<VelocityComponent>("Velocity");
    registry.register_component<NameComponent>("Name");
    registry.register_component<BoundsComponent>("Bounds");
    registry.register_component<LifetimeComponent>("Lifetime");
    (void)kernel.register_module({"ecs.registry", "Generation checked entities and stable component ownership", 1});
    (void)kernel.add_system({"ecs.deferred", core::Phase::PreSimulation, -10000, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& entities = host.services().get<Registry>();
            for (const auto& result : entities.flush_deferred()) {
                if (!result.success) {
                    host.diagnose(core::Diagnostic::Severity::Error, "ecs.deferred", result.message);
                }
            }
        });
    (void)kernel.add_system({"ecs.lifetime", core::Phase::PostSimulation, 10000, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& entities = host.services().get<Registry>();
            entities.each<LifetimeComponent>([&](Entity entity, LifetimeComponent& lifetime) {
                if (!lifetime.expire || !std::isfinite(lifetime.remaining_seconds)) {
                    return;
                }
                lifetime.remaining_seconds = std::max(0.0, lifetime.remaining_seconds - frame.delta_seconds);
                if (lifetime.remaining_seconds == 0.0 && !entities.defer_destroy(entity)) {
                    host.diagnose(core::Diagnostic::Severity::Warning, "ecs.lifetime", "deferred destruction queue is full");
                }
            });
        });
    (void)kernel.add_system({"ecs.changes", core::Phase::EndFrame, -10000, {}},
        [cursor = std::uint64_t{}](core::Kernel& host, const core::Frame&) mutable {
            const auto& entities = host.services().get<Registry>();
            const auto changes = entities.changes_since(cursor);
            for (const auto& event : changes.events) {
                (void)host.events().publish(event);
            }
            cursor = changes.newest_revision;
            const auto counters = entities.statistics();
            host.metric("ecs.entities", static_cast<double>(counters.entities));
            host.metric("ecs.components", static_cast<double>(counters.component_instances));
            host.metric("ecs.pending", static_cast<double>(counters.queued_commands));
            host.metric("ecs.payload_bytes", static_cast<double>(counters.component_payload_bytes));
            if (changes.history_lost) {
                host.diagnose(core::Diagnostic::Severity::Warning, "ecs.changes", "entity change history overflowed");
            }
        });
    (void)kernel.add_command("ecs.spawn", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() > 2) {
            return core::CommandResult{false, "usage: ecs.spawn [name] [persistent-id]"};
        }
        PersistentId persistent{};
        if (args.size() == 2) {
            const auto parsed = parse_unsigned(args[1]);
            if (!parsed || *parsed == 0) {
                return core::CommandResult{false, "persistent id must be a nonzero integer"};
            }
            persistent.value = *parsed;
        }
        auto& entities = host.services().get<Registry>();
        const auto entity = entities.create(persistent, args.empty() ? std::string_view{} : args[0]);
        if (!entity) {
            return core::CommandResult{false, "entity capacity exhausted or persistent id already exists"};
        }
        try {
            entities.emplace<TransformComponent>(entity);
        } catch (...) {
            (void)entities.destroy(entity);
            throw;
        }
        return core::CommandResult{true, format_entity(entity)};
    }, "Create an entity with a transform");
    (void)kernel.add_command("ecs.despawn", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: ecs.despawn entity"};
        }
        auto& entities = host.services().get<Registry>();
        const auto entity = resolve_entity(entities, args[0]);
        const bool removed = entities.destroy(entity);
        return core::CommandResult{removed, removed ? "destroyed" : "unknown or stale entity"};
    }, "Destroy an entity by index:generation, @persistent-id or name");
    (void)kernel.add_command("ecs.rename", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: ecs.rename entity name"};
        }
        auto& entities = host.services().get<Registry>();
        const bool accepted = entities.set_name(resolve_entity(entities, args[0]), args[1]);
        return core::CommandResult{accepted, accepted ? "renamed" : "invalid entity or name"};
    }, "Change the entity display name");
    (void)kernel.add_command("ecs.enable", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2 || (args[1] != "on" && args[1] != "off")) {
            return core::CommandResult{false, "usage: ecs.enable entity on|off"};
        }
        auto& entities = host.services().get<Registry>();
        const bool accepted = entities.set_enabled(resolve_entity(entities, args[0]), args[1] == "on");
        return core::CommandResult{accepted, accepted ? "updated" : "unknown entity"};
    }, "Enable or disable default ECS queries for an entity");
    (void)kernel.add_command("ecs.tag", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 3 || (args[1] != "add" && args[1] != "remove")) {
            return core::CommandResult{false, "usage: ecs.tag entity add|remove tag"};
        }
        auto& entities = host.services().get<Registry>();
        const auto entity = resolve_entity(entities, args[0]);
        const bool accepted = args[1] == "add" ? entities.add_tag(entity, args[2]) : entities.remove_tag(entity, args[2]);
        return core::CommandResult{accepted, accepted ? "updated" : "invalid entity/tag or absent tag"};
    }, "Edit bounded entity tags");
    (void)kernel.add_command("ecs.move", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 4) {
            return core::CommandResult{false, "usage: ecs.move entity x y z"};
        }
        const auto x = parse_real(args[1]);
        const auto y = parse_real(args[2]);
        const auto z = parse_real(args[3]);
        if (!x || !y || !z) {
            return core::CommandResult{false, "position components must be finite numbers"};
        }
        auto& entities = host.services().get<Registry>();
        const auto entity = resolve_entity(entities, args[0]);
        if (!entity) {
            return core::CommandResult{false, "unknown entity"};
        }
        auto& transform = entities.emplace<TransformComponent>(entity);
        transform.local.position = {*x, *y, *z};
        transform.dirty = true;
        (void)entities.mark_changed<TransformComponent>(entity);
        return core::CommandResult{true, "local position updated"};
    }, "Set a local position in metres");
    (void)kernel.add_command("ecs.velocity", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 4) {
            return core::CommandResult{false, "usage: ecs.velocity entity x y z"};
        }
        const auto x = parse_real(args[1]);
        const auto y = parse_real(args[2]);
        const auto z = parse_real(args[3]);
        if (!x || !y || !z) {
            return core::CommandResult{false, "velocity components must be finite numbers"};
        }
        auto& entities = host.services().get<Registry>();
        const auto entity = resolve_entity(entities, args[0]);
        if (!entity) {
            return core::CommandResult{false, "unknown entity"};
        }
        auto& velocity = entities.emplace<VelocityComponent>(entity);
        velocity.linear = {*x, *y, *z};
        (void)entities.mark_changed<VelocityComponent>(entity);
        return core::CommandResult{true, "linear velocity updated"};
    }, "Set a linear velocity in metres per second");
    (void)kernel.add_command("ecs.clone", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) {
            return core::CommandResult{false, "usage: ecs.clone entity [name]"};
        }
        auto& entities = host.services().get<Registry>();
        const auto source = resolve_entity(entities, args[0]);
        const auto copied = entities.clone(source, {}, args.size() == 2 ? args[1] : std::string_view{});
        return core::CommandResult{static_cast<bool>(copied), copied ? format_entity(copied) : "clone rejected"};
    }, "Copy cloneable components into a new transient entity");
    (void)kernel.add_command("ecs.list", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() > 1) {
            return core::CommandResult{false, "usage: ecs.list [tag]"};
        }
        const auto& entities = host.services().get<Registry>();
        Query specification;
        specification.include_disabled = true;
        specification.limit = 1000;
        if (!args.empty()) {
            specification.tags.push_back(args[0]);
        }
        std::ostringstream output;
        for (const auto entity : entities.query(specification)) {
            output << format_entity(entity) << " @" << entities.persistent_id(entity).value
                   << ' ' << (entities.enabled(entity) ? "on " : "off ") << entities.name(entity) << '\n';
        }
        return core::CommandResult{true, output.str()};
    }, "List up to 1000 entities, optionally requiring a tag");
    (void)kernel.add_command("ecs.inspect", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: ecs.inspect entity"};
        }
        const auto& entities = host.services().get<Registry>();
        const auto entity = resolve_entity(entities, args[0]);
        const auto information = entities.inspect(entity);
        if (!information) {
            return core::CommandResult{false, "unknown or stale entity"};
        }
        std::ostringstream output;
        output << format_entity(entity) << " name=" << information->name
               << " persistent=" << information->persistent.value << " layers=" << information->layers
               << " revision=" << information->revision << " components=";
        for (const auto type : information->components) {
            output << type << ',';
        }
        output << " tags=";
        for (const auto& tag : information->tags) {
            output << tag << ',';
        }
        if (const auto* transform = entities.get<TransformComponent>(entity)) {
            output << " local=" << transform->local.position.x << ','
                   << transform->local.position.y << ',' << transform->local.position.z;
        }
        return core::CommandResult{true, output.str()};
    }, "Inspect entity identity, ownership and transform");
}

} // namespace ge::ecs
