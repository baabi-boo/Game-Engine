#include <ge/ecs/registry.hpp>

#include "test_support.hpp"

#include <stdexcept>
#include <type_traits>

namespace {

struct Payload {
    int value{};
};

void qualified_access_uses_the_same_storage() {
    ge::ecs::Registry registry(4);
    const auto entity = registry.create();
    auto& payload = registry.emplace<Payload>(entity, Payload{17});
    static_assert(std::is_same_v<decltype(registry.get<const Payload>(entity)), const Payload*>);
    CHECK(registry.get<const Payload>(entity) == &payload);
    CHECK(registry.get<const volatile Payload>(entity) == &payload);
    const auto& read_only = registry;
    CHECK(read_only.get<const Payload>(entity) == &payload);
    CHECK(registry.component_type<const Payload>() == registry.component_type<Payload>());
    CHECK(registry.register_component<const Payload>() == registry.component_type<Payload>());
    CHECK(registry.statistics().component_types == 1);

    const auto before = registry.revision();
    CHECK(registry.patch<Payload>(entity, [](Payload& value) { value.value = 23; }));
    CHECK(registry.require<const Payload>(entity).value == 23);
    const auto changes = registry.changes_since(before);
    CHECK(changes.events.size() == 1);
    CHECK(changes.events.front().kind == ge::ecs::EntityEvent::Kind::ComponentChanged);
    CHECK(changes.events.front().component == registry.component_type<const Payload>());
}

void qualified_registration_does_not_create_const_owned_objects() {
    ge::ecs::Registry registry(2);
    const auto type = registry.register_component<const Payload>();
    const auto entity = registry.create();
    registry.emplace<const Payload>(entity, Payload{31});
    registry.require<Payload>(entity).value = 37;
    CHECK(registry.require<const Payload>(entity).value == 37);
    CHECK(registry.component_type<Payload>() == type);
    registry.emplace_or_replace<const Payload>(entity, Payload{41});
    CHECK(registry.require<Payload>(entity).value == 41);
    CHECK(registry.statistics().component_instances == 1);
    const auto before = registry.revision();
    CHECK(registry.remove<const Payload>(entity));
    CHECK(!registry.has<Payload>(entity));
    CHECK(!registry.has<const Payload>(entity));
    CHECK(registry.statistics().component_instances == 0);
    const auto changes = registry.changes_since(before);
    CHECK(changes.events.size() == 1);
    CHECK(changes.events.front().kind == ge::ecs::EntityEvent::Kind::ComponentRemoved);
    CHECK(changes.events.front().component == type);
}

void qualified_iteration_preserves_deferred_generation_checks() {
    ge::ecs::Registry registry(1);
    const auto original = registry.create(ge::PersistentId{7});
    registry.emplace<Payload>(original, Payload{43});
    std::size_t visited{};
    registry.each<const Payload>([&](ge::ecs::Entity entity, const Payload& payload) {
        ++visited;
        CHECK(entity == original);
        CHECK(payload.value == 43);
        CHECK_THROWS_AS(registry.remove<const Payload>(entity), std::logic_error);
        CHECK(registry.defer_destroy(entity));
        CHECK(registry.defer_set<Payload>(entity, Payload{47}));
    });
    CHECK(visited == 1);
    CHECK(!registry.iterating());
    CHECK(registry.alive(original));
    CHECK(registry.flush_deferred(1).front().success);
    CHECK(!registry.alive(original));
    const auto replacement = registry.create(ge::PersistentId{7});
    CHECK(replacement.index == original.index);
    CHECK(replacement.generation != original.generation);
    registry.emplace<Payload>(replacement, Payload{53});
    CHECK(registry.flush_deferred().front().success);
    CHECK(registry.require<const Payload>(replacement).value == 53);
    CHECK(registry.get<const Payload>(original) == nullptr);
    CHECK(!registry.remove<const Payload>(original));
    CHECK(registry.find(ge::PersistentId{7}) == replacement);
}

} // namespace

int main() {
    return ge::test::run("ecs_audit", {
        {"qualified_access", qualified_access_uses_the_same_storage},
        {"qualified_registration", qualified_registration_does_not_create_const_owned_objects},
        {"qualified_iteration_and_deferred_generation", qualified_iteration_preserves_deferred_generation_checks},
    });
}
