#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/gameplay/equipment.hpp>

#include <vector>

namespace {

using namespace ge;
using namespace ge::gameplay;

constexpr PersistentId actor_id{1};
constexpr PersistentId container_id{2};
constexpr PersistentId item_id{3};
constexpr PersistentId recipe_id{4};
constexpr PersistentId attachment_token{5};
constexpr PersistentId craft_token{6};

struct Fixture {
    ecs::Registry registry{1};
    InventoryService inventory{registry};
    EquipmentService equipment{registry, inventory};
    ecs::Entity actor{};
    InventoryContainerId container{};
    InventoryStackId stack{};
    CraftJobId craft{};

    Fixture() {
        actor = registry.create(actor_id);
        CHECK(actor);
        ItemDefinition item;
        item.id = item_id;
        item.name = "equipment-audit-item";
        item.maximum_stack = 8;
        item.equipment_slots = 1;
        item.properties.emplace("equip.attack_damage", 4.0);
        CHECK(inventory.define(item));
        InventoryContainerSpec specification;
        specification.id = container_id;
        specification.owner = actor;
        container = inventory.create(specification);
        CHECK(container);
        InventoryOperation add;
        add.source = container;
        add.definition = item_id;
        add.quantity = 4;
        CHECK(inventory.transact({{add}}));
        stack = inventory.inspect(container)->stacks.front().id;
        CHECK(equipment.bind(actor, 1));
        CHECK(equipment.equip(actor, 0, container, stack, attachment_token));
        CraftRecipe recipe;
        recipe.id = recipe_id;
        recipe.name = "equipment-audit-recipe";
        recipe.inputs.push_back({item_id, 1});
        recipe.outputs.push_back({item_id, 1, {}});
        recipe.duration_seconds = 60.0;
        CHECK(equipment.define_recipe(recipe));
        const auto started = equipment.start({craft_token, actor, recipe_id, container, container});
        CHECK(started.admitted());
        craft = started.job;
        CHECK(inventory.stack(container, stack)->reserved() == 2);
        CHECK(equipment.modifiers(actor).additive("attack_damage") == 4.0);
    }
};

void invalid_owner_cleanup(bool destroy) {
    Fixture fixture;
    ecs::Entity replacement = fixture.actor;
    if (destroy) {
        CHECK(fixture.registry.destroy(fixture.actor));
        replacement = fixture.registry.create(actor_id);
        CHECK(replacement.index == fixture.actor.index);
        CHECK(replacement.generation != fixture.actor.generation);
    } else {
        CHECK(fixture.registry.set_persistent_id(fixture.actor, PersistentId{7}));
    }
    CHECK(fixture.equipment.modifiers(fixture.actor).error == EquipmentError::InvalidActor);

    // Match the runtime dependency: inventory retirement precedes equipment cleanup.
    const core::Frame frame{1, 0.0, 0.0};
    fixture.inventory.update(frame);
    fixture.equipment.update(frame);
    CHECK(!fixture.inventory.find(container_id));
    CHECK(fixture.inventory.statistics().reservations == 0);
    CHECK(fixture.equipment.statistics().attachments == 0);
    CHECK(fixture.equipment.statistics().actors == 0);
    CHECK(fixture.equipment.statistics().active_jobs == 0);
    const auto job = fixture.equipment.job(fixture.craft);
    CHECK(job);
    CHECK(job->state == CraftState::Invalidated);
    CHECK(job->inputs.empty());
    CHECK(!fixture.equipment.inspect(fixture.actor));
    CHECK(fixture.equipment.modifiers(replacement).error == EquipmentError::NotBound);
}

void destroyed_actor_does_not_transfer_equipment_to_reused_handle() {
    invalid_owner_cleanup(true);
}

void changed_actor_identity_releases_equipment_and_craft_ownership() {
    invalid_owner_cleanup(false);
}

void restore_remaps_handles_and_releases_only_owned_reservations() {
    Fixture fixture;
    std::vector<std::byte> inventory_bytes;
    std::vector<std::byte> equipment_bytes;
    CHECK(fixture.inventory.snapshot(inventory_bytes));
    CHECK(fixture.equipment.snapshot(equipment_bytes));

    ecs::Registry registry{1};
    const auto actor = registry.create(actor_id);
    InventoryService inventory{registry};
    EquipmentService equipment{registry, inventory};
    CHECK(inventory.restore(inventory_bytes));
    CHECK(equipment.restore(equipment_bytes));
    const auto container = inventory.find(container_id);
    const auto stack = inventory.resolve_stack(container_id, fixture.stack.sequence);
    CHECK(container && container != fixture.container);
    CHECK(stack && stack != fixture.stack);
    CHECK(!inventory.inspect(fixture.container));
    CHECK(!equipment.job(fixture.craft));
    const auto attachment = equipment.inspect(actor);
    CHECK(attachment && attachment->attachments.size() == 1);
    CHECK(attachment->attachments.front().container == container);
    CHECK(attachment->attachments.front().stack == stack);
    CHECK(!attachment->attachments.front().retiring);
    CHECK(equipment.modifiers(actor).additive("attack_damage") == 4.0);

    const auto craft = equipment.find_job(craft_token);
    CHECK(craft && craft != fixture.craft);
    CHECK(equipment.cancel(craft));
    CHECK(inventory.stack(container, stack)->reserved() == 1);
    CHECK(equipment.modifiers(actor).additive("attack_damage") == 4.0);
    CHECK(equipment.unequip(actor, 0));
    CHECK(inventory.stack(container, stack)->reserved() == 0);
    CHECK(inventory.quantity(container, item_id) == 4);
    CHECK(equipment.modifiers(actor).equipped == 0);
    CHECK(equipment.modifiers(actor).additive("attack_damage") == 0.0);
}

} // namespace

int main() {
    return ge::test::run("gameplay_equipment_audit", {
        {"destroyed owner cleanup", destroyed_actor_does_not_transfer_equipment_to_reused_handle},
        {"changed identity cleanup", changed_actor_identity_releases_equipment_and_craft_ownership},
        {"restore reservation ownership", restore_remaps_handles_and_releases_only_owned_reservations},
    });
}
