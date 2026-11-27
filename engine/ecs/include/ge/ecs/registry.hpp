#pragma once

#include <ge/core/identity.hpp>
#include <ge/core/memory.hpp>
#include <ge/math/math.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::ecs {

using Entity = ge::RuntimeHandle;
using ComponentType = std::uint32_t;
inline constexpr Entity null_entity{};

struct EntityHash {
    [[nodiscard]] std::size_t operator()(Entity entity) const noexcept {
        const auto combined = entity.generation ^ (static_cast<std::uint64_t>(entity.index) << 32U);
        return std::hash<std::uint64_t>{}(combined);
    }
};

struct TransformComponent {
    math::Transform local{};
    math::Transform world{};
    bool dirty{true};
};

struct VelocityComponent {
    math::Vec3 linear{};
    math::Vec3 angular{};
};

struct NameComponent {
    std::string value;
};

struct BoundsComponent {
    math::Aabb local{};
    math::Aabb world{};
};

struct LifetimeComponent {
    double remaining_seconds{};
    bool expire{true};
};

struct EntityEvent {
    enum class Kind : std::uint8_t {
        Created,
        Destroyed,
        ComponentAdded,
        ComponentRemoved,
        ComponentChanged,
        EnabledChanged,
        IdentityChanged,
        TagsChanged,
        LayerChanged
    };
    Kind kind{Kind::Created};
    Entity entity{};
    PersistentId persistent{};
    ComponentType component{};
    std::uint64_t revision{};
};

struct ChangeBatch {
    std::vector<EntityEvent> events;
    std::uint64_t newest_revision{};
    bool history_lost{};
};

struct ComponentInfo {
    ComponentType id{};
    std::string name;
    std::size_t instances{};
    std::size_t element_bytes{};
    bool cloneable{};
};

struct EntityInfo {
    Entity entity{};
    PersistentId persistent{};
    std::string name;
    bool enabled{};
    std::uint64_t layers{};
    std::vector<std::string> tags;
    std::vector<ComponentType> components;
    std::uint64_t revision{};
};

struct Query {
    std::vector<ComponentType> all;
    std::vector<ComponentType> any;
    std::vector<ComponentType> none;
    std::vector<std::string> tags;
    std::string name_contains;
    std::uint64_t layers{std::numeric_limits<std::uint64_t>::max()};
    bool include_disabled{};
    std::size_t limit{65536};
};

struct DeferredResult {
    std::uint64_t ticket{};
    bool success{};
    std::string message;
};

struct RegistryStatistics {
    std::size_t entities{};
    std::size_t enabled_entities{};
    std::size_t component_types{};
    std::size_t component_instances{};
    std::size_t component_payload_bytes{};
    std::size_t queued_commands{};
    std::uint64_t revision{};
    std::uint64_t deferred_failures{};
    std::uint32_t retired_slots{};
};

// A registry is a single-writer owner. Handles belong to this registry lifetime.
// Component addresses remain stable until that component is replaced or removed.
// Structural edits during each() are rejected; use deferred commands instead.
class Registry final {
public:
    explicit Registry(std::uint32_t capacity = 65536,
                      std::size_t component_type_limit = 256,
                      std::size_t deferred_limit = 4096,
                      std::size_t history_limit = 8192);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;

    [[nodiscard]] Entity create(PersistentId persistent = {}, std::string_view name = {});
    bool destroy(Entity entity);
    void clear();
    [[nodiscard]] bool alive(Entity entity) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint32_t capacity() const noexcept;
    [[nodiscard]] std::vector<Entity> entities(bool include_disabled = true) const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t entity_revision(Entity entity) const noexcept;

    [[nodiscard]] PersistentId persistent_id(Entity entity) const noexcept;
    bool set_persistent_id(Entity entity, PersistentId persistent);
    [[nodiscard]] Entity find(PersistentId persistent) const noexcept;
    [[nodiscard]] Entity find_name(std::string_view name) const;
    [[nodiscard]] std::vector<Entity> find_all_named(std::string_view name) const;
    [[nodiscard]] std::string_view name(Entity entity) const noexcept;
    bool set_name(Entity entity, std::string_view name);
    [[nodiscard]] bool enabled(Entity entity) const noexcept;
    bool set_enabled(Entity entity, bool enabled);
    [[nodiscard]] std::uint64_t layers(Entity entity) const noexcept;
    bool set_layers(Entity entity, std::uint64_t layers);
    bool add_tag(Entity entity, std::string_view tag);
    bool remove_tag(Entity entity, std::string_view tag);
    [[nodiscard]] bool has_tag(Entity entity, std::string_view tag) const;
    [[nodiscard]] std::vector<std::string> tags(Entity entity) const;

    template<class T>
    [[nodiscard]] ComponentType component_type() const noexcept {
        const auto* storage = pool<T>();
        return storage ? storage->id : 0;
    }

    template<class T>
    ComponentType register_component(std::string_view name = {}) {
        require_structural_access();
        return ensure_pool<T>(name).id;
    }

    template<class T, class... Args>
    T& emplace(Entity entity, Args&&... args) {
        require_alive(entity);
        auto* storage = pool<T>();
        if (storage) {
            if (auto* existing = storage->get(entity)) {
                return *existing;
            }
        }
        require_structural_access();
        auto& destination = ensure_pool<T>();
        auto& value = destination.insert(entity, std::forward<Args>(args)...);
        record(EntityEvent::Kind::ComponentAdded, entity, destination.id);
        return value;
    }

    template<class T, class... Args>
    T& emplace_or_replace(Entity entity, Args&&... args) {
        require_alive(entity);
        require_structural_access();
        auto& destination = ensure_pool<T>();
        const bool existed = destination.contains(entity);
        auto& value = destination.replace(entity, std::forward<Args>(args)...);
        record(existed ? EntityEvent::Kind::ComponentChanged : EntityEvent::Kind::ComponentAdded,
               entity, destination.id);
        return value;
    }

    template<class T>
    [[nodiscard]] T* get(Entity entity) noexcept {
        if (!alive(entity)) {
            return nullptr;
        }
        auto* storage = pool<T>();
        return storage ? storage->get(entity) : nullptr;
    }

    template<class T>
    [[nodiscard]] const T* get(Entity entity) const noexcept {
        if (!alive(entity)) {
            return nullptr;
        }
        const auto* storage = pool<T>();
        return storage ? storage->get(entity) : nullptr;
    }

    template<class T>
    [[nodiscard]] T& require(Entity entity) {
        if (auto* value = get<T>(entity)) {
            return *value;
        }
        throw std::out_of_range("entity does not own the requested component");
    }

    template<class T>
    [[nodiscard]] const T& require(Entity entity) const {
        if (const auto* value = get<T>(entity)) {
            return *value;
        }
        throw std::out_of_range("entity does not own the requested component");
    }

    template<class T>
    [[nodiscard]] bool has(Entity entity) const noexcept {
        return get<T>(entity) != nullptr;
    }

    template<class T>
    bool remove(Entity entity) {
        if (!alive(entity)) {
            return false;
        }
        auto* storage = pool<T>();
        if (!storage || !storage->contains(entity)) {
            return false;
        }
        require_structural_access();
        storage->erase(entity);
        record(EntityEvent::Kind::ComponentRemoved, entity, storage->id);
        return true;
    }

    template<class T, class Function>
    bool patch(Entity entity, Function&& function) {
        auto* value = get<T>(entity);
        if (!value) {
            return false;
        }
        std::invoke(std::forward<Function>(function), *value);
        record(EntityEvent::Kind::ComponentChanged, entity, component_type<T>());
        return true;
    }

    template<class T>
    bool mark_changed(Entity entity) {
        if (!has<T>(entity)) {
            return false;
        }
        record(EntityEvent::Kind::ComponentChanged, entity, component_type<T>());
        return true;
    }

    template<class... Components>
    [[nodiscard]] std::vector<Entity> view(bool include_disabled = false) const {
        std::vector<Entity> result;
        result.reserve(live_.size());
        for (const auto entity : live_) {
            if ((include_disabled || enabled(entity)) && (has<Components>(entity) && ...)) {
                result.push_back(entity);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    template<class... Components, class Function>
    void each(Function&& function, bool include_disabled = false) {
        const auto selected = view<Components...>(include_disabled);
        IterationScope scope(*this);
        for (const auto entity : selected) {
            std::invoke(function, entity, *get<Components>(entity)...);
        }
    }

    template<class... Components, class Function>
    void each(Function&& function, bool include_disabled = false) const {
        const auto selected = view<Components...>(include_disabled);
        IterationScope scope(*this);
        for (const auto entity : selected) {
            std::invoke(function, entity, *get<Components>(entity)...);
        }
    }

    [[nodiscard]] bool has(Entity entity, ComponentType type) const noexcept;
    bool remove(Entity entity, ComponentType type);
    [[nodiscard]] std::vector<ComponentInfo> component_info() const;
    [[nodiscard]] std::optional<EntityInfo> inspect(Entity entity) const;
    [[nodiscard]] std::vector<Entity> query(const Query& query) const;
    [[nodiscard]] Entity clone(Entity source, PersistentId persistent = {},
                               std::string_view name = {});
    [[nodiscard]] ChangeBatch changes_since(std::uint64_t revision,
                                            std::size_t limit = 8192) const;
    [[nodiscard]] RegistryStatistics statistics() const;

    [[nodiscard]] std::optional<std::uint64_t> defer(std::function<void(Registry&)> function,
                                                    std::string label = {});
    [[nodiscard]] std::optional<std::uint64_t> defer_destroy(Entity entity);
    [[nodiscard]] std::optional<std::uint64_t> defer_create(
        PersistentId persistent, std::string name,
        std::function<void(Registry&, Entity)> initialize = {});

    template<class T>
    [[nodiscard]] std::optional<std::uint64_t> defer_remove(Entity entity) {
        return defer([entity](Registry& registry) {
            (void)registry.remove<T>(entity);
        }, "remove component");
    }

    template<class T>
    [[nodiscard]] std::optional<std::uint64_t> defer_set(Entity entity, T value) {
        auto owned = std::make_shared<T>(std::move(value));
        return defer([entity, owned](Registry& registry) {
            if (registry.alive(entity)) {
                registry.emplace_or_replace<T>(entity, std::move(*owned));
            }
        }, "set component");
    }

    [[nodiscard]] std::vector<DeferredResult> flush_deferred(std::size_t limit = 4096);
    [[nodiscard]] std::size_t deferred_count() const noexcept;
    [[nodiscard]] std::size_t discard_deferred() noexcept;
    [[nodiscard]] bool iterating() const noexcept;

private:
    struct Record {
        PersistentId persistent{};
        std::size_t dense_index{};
        bool enabled{true};
        std::uint64_t layers{1};
        std::vector<std::string> tags;
        std::uint64_t revision{};
    };

    struct PoolBase {
        ComponentType id;
        std::string name;
        std::size_t element_bytes;
        PoolBase(ComponentType type, std::string label, std::size_t bytes)
            : id(type), name(std::move(label)), element_bytes(bytes) {}
        virtual ~PoolBase() = default;
        [[nodiscard]] virtual bool contains(Entity entity) const noexcept = 0;
        virtual void erase(Entity entity) noexcept = 0;
        [[nodiscard]] virtual std::size_t size() const noexcept = 0;
        [[nodiscard]] virtual bool cloneable() const noexcept = 0;
        virtual void clone(Entity source, Entity destination) = 0;
    };

    template<class T>
    struct Pool final : PoolBase {
        static_assert(std::is_nothrow_destructible_v<T>, "components must have nonthrowing destruction");
        struct Item {
            std::uint64_t generation;
            std::unique_ptr<T> value;
        };
        std::unordered_map<std::uint32_t, Item> values;
        std::uint32_t capacity;

        Pool(ComponentType type, std::string label, std::uint32_t entity_capacity)
            : PoolBase(type, std::move(label), sizeof(T)), capacity(entity_capacity) {}

        [[nodiscard]] T* get(Entity entity) noexcept {
            const auto it = values.find(entity.index);
            return it != values.end() && it->second.generation == entity.generation
                ? it->second.value.get() : nullptr;
        }

        [[nodiscard]] const T* get(Entity entity) const noexcept {
            const auto it = values.find(entity.index);
            return it != values.end() && it->second.generation == entity.generation
                ? it->second.value.get() : nullptr;
        }

        [[nodiscard]] bool contains(Entity entity) const noexcept override {
            return get(entity) != nullptr;
        }

        template<class... Args>
        T& insert(Entity entity, Args&&... args) {
            if (auto* existing = get(entity)) {
                return *existing;
            }
            if (values.size() >= capacity) {
                throw std::length_error("component storage capacity exhausted");
            }
            auto value = std::make_unique<T>(std::forward<Args>(args)...);
            auto* result = value.get();
            const auto inserted = values.emplace(entity.index, Item{entity.generation, std::move(value)});
            if (!inserted.second) {
                throw std::logic_error("component generation ownership mismatch");
            }
            return *result;
        }

        template<class... Args>
        T& replace(Entity entity, Args&&... args) {
            const auto it = values.find(entity.index);
            if (it == values.end()) {
                return insert(entity, std::forward<Args>(args)...);
            }
            if (it->second.generation != entity.generation) {
                throw std::logic_error("component generation ownership mismatch");
            }
            auto replacement = std::make_unique<T>(std::forward<Args>(args)...);
            it->second.value.swap(replacement);
            return *it->second.value;
        }

        void erase(Entity entity) noexcept override {
            const auto it = values.find(entity.index);
            if (it != values.end() && it->second.generation == entity.generation) {
                values.erase(it);
            }
        }

        [[nodiscard]] std::size_t size() const noexcept override {
            return values.size();
        }

        [[nodiscard]] bool cloneable() const noexcept override {
            return std::is_copy_constructible_v<T>;
        }

        void clone(Entity source, Entity destination) override {
            if constexpr (std::is_copy_constructible_v<T>) {
                if (const auto* value = get(source)) {
                    (void)insert(destination, *value);
                }
            } else {
                (void)source;
                (void)destination;
                throw std::logic_error("component is not copy constructible");
            }
        }
    };

    class IterationScope final {
    public:
        explicit IterationScope(const Registry& registry) : registry_(registry) {
            if (registry_.iteration_depth_ == std::numeric_limits<std::size_t>::max()) {
                throw std::overflow_error("registry iteration nesting exhausted");
            }
            ++registry_.iteration_depth_;
        }
        ~IterationScope() { --registry_.iteration_depth_; }
        IterationScope(const IterationScope&) = delete;
        IterationScope& operator=(const IterationScope&) = delete;
    private:
        const Registry& registry_;
    };

    struct DeferredCommand {
        std::uint64_t ticket;
        std::string label;
        std::function<void(Registry&)> function;
    };

    template<class T>
    [[nodiscard]] Pool<T>* pool() noexcept {
        const auto it = pools_.find(std::type_index(typeid(T)));
        return it == pools_.end() ? nullptr : static_cast<Pool<T>*>(it->second.get());
    }

    template<class T>
    [[nodiscard]] const Pool<T>* pool() const noexcept {
        const auto it = pools_.find(std::type_index(typeid(T)));
        return it == pools_.end() ? nullptr : static_cast<const Pool<T>*>(it->second.get());
    }

    template<class T>
    Pool<T>& ensure_pool(std::string_view label = {}) {
        if (auto* existing = pool<T>()) {
            return *existing;
        }
        if (pools_.size() >= component_type_limit_ || next_type_ == 0) {
            throw std::length_error("component type capacity exhausted");
        }
        const auto name = label.empty() ? std::string(typeid(T).name()) : std::string(label);
        if (name.size() > 1024) {
            throw std::length_error("component name exceeds 1024 bytes");
        }
        auto storage = std::make_unique<Pool<T>>(next_type_, name, handles_.capacity());
        auto* result = storage.get();
        pools_.emplace(std::type_index(typeid(T)), std::move(storage));
        ++next_type_;
        return *result;
    }

    void require_alive(Entity entity) const;
    void require_structural_access() const;
    void record(EntityEvent::Kind kind, Entity entity, ComponentType type = 0) noexcept;
    [[nodiscard]] Record* record_for(Entity entity) noexcept;
    [[nodiscard]] const Record* record_for(Entity entity) const noexcept;
    [[nodiscard]] PoolBase* pool_by_id(ComponentType type) noexcept;
    [[nodiscard]] const PoolBase* pool_by_id(ComponentType type) const noexcept;
    [[nodiscard]] bool matches(Entity entity, const Query& query) const;

    GenerationPool handles_;
    std::size_t component_type_limit_;
    std::size_t deferred_limit_;
    std::vector<Record> records_;
    std::vector<Entity> live_;
    std::unordered_map<std::uint64_t, Entity> persistent_;
    std::unordered_map<std::type_index, std::unique_ptr<PoolBase>> pools_;
    std::deque<DeferredCommand> deferred_;
    core::RingBuffer<EntityEvent> history_;
    ComponentType next_type_{1};
    std::uint64_t revision_{};
    std::uint64_t next_ticket_{1};
    std::uint64_t deferred_failures_{};
    mutable std::size_t iteration_depth_{};
    bool flushing_{};
};

[[nodiscard]] std::string format_entity(Entity entity);
[[nodiscard]] std::optional<Entity> parse_entity(std::string_view text) noexcept;
void register_registry(core::Kernel& kernel);

} // namespace ge::ecs
