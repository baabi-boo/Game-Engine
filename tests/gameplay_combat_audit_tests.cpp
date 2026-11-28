#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/gameplay/combat.hpp>
#include <ge/scene/scene.hpp>

#include <limits>

namespace {

using namespace ge;

struct Fixture {
    ecs::Registry registry{8};
    scene::Scene scene{registry};
    core::Kernel kernel;
    gameplay::CombatService combat;

    explicit Fixture(std::uint64_t next_sequence = 1)
        : combat(kernel, registry, scene, {.mission_outbox = 1}, {73, next_sequence}) {}

    ecs::Entity actor(std::uint64_t persistent) {
        const auto entity = scene.create({}, {}, {}, PersistentId{persistent});
        CHECK(entity);
        CHECK(combat.attach(entity, {.maximum_shield = 25.0}));
        return entity;
    }
};

gameplay::DamageRequest lethal(ecs::Entity source, ecs::Entity target) {
    gameplay::DamageRequest request;
    request.source = source;
    request.target = target;
    request.damage.amount = 100.0;
    request.damage.critical_chance = 1.0;
    request.damage.critical_multiplier = 2.0;
    return request;
}

void check_rejected_outcome(const gameplay::DamageResult& result, ecs::Entity target) {
    CHECK(!result);
    CHECK(result.target == target);
    CHECK(result.sequence == 0);
    CHECK(result.health_damage == 0.0);
    CHECK(result.shield_damage == 0.0);
    CHECK(!result.hit);
    CHECK(!result.critical);
    CHECK(!result.defeated);
    CHECK(!result.impulse_applied);
}

void rejected_lethal_damage_has_no_outcome(bool exhausted) {
    Fixture fixture(exhausted ? std::numeric_limits<std::uint64_t>::max() : 1);
    const auto source = fixture.actor(1);
    const auto target = fixture.actor(2);
    if (!exhausted) {
        const auto previous_target = fixture.actor(3);
        const auto applied = fixture.combat.damage(lethal(source, previous_target));
        CHECK(applied && applied.defeated);
    }
    const auto cursor = fixture.combat.events_since(0).newest_sequence;
    const auto before = fixture.combat.statistics();
    const auto result = fixture.combat.damage(lethal(source, target));
    CHECK(result.error == (exhausted ? gameplay::CombatError::Exhausted : gameplay::CombatError::Capacity));
    check_rejected_outcome(result, target);
    const auto snapshot = fixture.combat.inspect(target);
    CHECK(snapshot);
    CHECK(snapshot->combatant.health == 100.0);
    CHECK(snapshot->combatant.shield == 25.0);
    CHECK(!snapshot->combatant.defeated);
    CHECK(fixture.combat.events_since(cursor).events.empty());
    CHECK(fixture.combat.statistics().damage_resolutions == before.damage_resolutions);
    CHECK(fixture.combat.statistics().defeated == before.defeated);
    CHECK(fixture.combat.statistics().pending_defeats == before.pending_defeats);
}

void rejected_attack_preserves_costs_and_cooldown() {
    Fixture fixture;
    const auto source = fixture.actor(1);
    const auto previous_target = fixture.actor(2);
    const auto target = fixture.actor(3);
    CHECK(fixture.combat.damage(lethal(source, previous_target)).defeated);
    auto& collision = fixture.kernel.services().emplace<physics::CollisionWorld>(fixture.registry);
    CHECK(collision.add(target));
    auto& inventory = fixture.kernel.services().emplace<gameplay::InventoryService>(fixture.registry);
    const PersistentId ammunition{11};
    gameplay::ItemDefinition definition;
    definition.id = ammunition;
    definition.name = "ammunition";
    CHECK(inventory.define(definition));
    const auto container = inventory.create({.id = PersistentId{12}, .owner = source});
    CHECK(container);
    gameplay::InventoryTransaction fill;
    fill.operations.push_back({.kind = gameplay::InventoryOperationKind::Add,
        .source = container, .definition = ammunition, .quantity = 2});
    CHECK(inventory.transact(fill));
    const auto revision = inventory.inspect(container)->revision;
    const auto cursor = fixture.combat.events_since(0).newest_sequence;

    gameplay::AttackRequest request;
    request.source = source;
    request.kind = gameplay::AttackKind::MeleeSphere;
    request.damage = lethal(source, target).damage;
    request.costs[0] = {container, ammunition, 1};
    request.cost_count = 1;
    const auto result = fixture.combat.attack(request);
    CHECK(result.error == gameplay::CombatError::Capacity);
    CHECK(!result.fired);
    CHECK(!result.costs_committed);
    CHECK(result.sequence == 0);
    CHECK(result.targets.empty());
    CHECK(inventory.quantity(container, ammunition) == 2);
    CHECK(inventory.inspect(container)->revision == revision);
    CHECK(fixture.combat.cooldown(source, request.attack) == 0.0);
    CHECK(fixture.combat.events_since(cursor).events.empty());
    CHECK(fixture.combat.inspect(target)->combatant.health == 100.0);
    CHECK(fixture.combat.inspect(target)->combatant.shield == 25.0);
}

} // namespace

int main() {
    return ge::test::run("gameplay_combat_audit", {
        {"full mission queue leaves no damage outcome", [] { rejected_lethal_damage_has_no_outcome(false); }},
        {"exhausted mission cursor leaves no damage outcome", [] { rejected_lethal_damage_has_no_outcome(true); }},
        {"rejected attack preserves costs and cooldown", rejected_attack_preserves_costs_and_cooldown},
    });
}
