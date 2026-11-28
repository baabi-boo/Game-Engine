#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/gameplay/inventory.hpp>

#include <vector>

namespace {

using namespace ge;
using namespace ge::gameplay;

void changed_owner_identity_retires_container() {
    for (const PersistentId replacement : {PersistentId{22}, PersistentId{}}) {
        ecs::Registry registry{2};
        core::SignalHub signals;
        InventoryService inventory{registry};
        const auto owner = registry.create({11});
        CHECK(owner);
        ItemDefinition item;
        item.id = {1};
        item.name = "item";
        CHECK(inventory.define(item));
        InventoryContainerSpec specification;
        specification.id = {33};
        specification.owner = owner;
        const auto container = inventory.create(specification);
        CHECK(container);

        InventoryOperation add;
        add.source = container;
        add.definition = item.id;
        add.quantity = 3;
        auto reserve = add;
        reserve.kind = InventoryOperationKind::Reserve;
        reserve.quantity = 1;
        reserve.reservation = {44};
        auto lock = reserve;
        lock.kind = InventoryOperationKind::Lock;
        InventoryTransaction transaction;
        transaction.operations = {add, reserve, lock};
        transaction.access = reserve.reservation;
        CHECK(inventory.transact(transaction));

        inventory.bind_signals(signals);
        std::vector<InventoryChange> delivered;
        const auto subscription = signals.subscribe(inventory.change_signal(),
            [&](const InventoryChange& change) { delivered.push_back(change); });
        CHECK(subscription);
        const auto cursor = inventory.changes_since(0).newest_sequence;
        CHECK(registry.set_persistent_id(owner, replacement));
        CHECK(registry.alive(owner));
        std::vector<std::byte> snapshot;
        CHECK(inventory.snapshot(snapshot).error == InventoryError::InvalidOwner);

        inventory.update(core::Frame{7});
        CHECK(!inventory.inspect(container));
        CHECK(!inventory.find(specification.id));
        CHECK(inventory.statistics().containers == 0);
        CHECK(inventory.statistics().reservations == 0);
        CHECK(inventory.snapshot(snapshot));
        const auto changes = inventory.changes_since(cursor);
        CHECK(changes.changes.size() == 1);
        CHECK(changes.changes[0].kind == InventoryChangeKind::Removed);
        CHECK(changes.changes[0].container == container);
        CHECK(changes.changes[0].owner == owner);
        CHECK(changes.changes[0].tick == 7);
        CHECK(signals.flush().callbacks == 1);
        CHECK(delivered.size() == 1);
        CHECK(delivered[0].sequence == changes.changes[0].sequence);

        inventory.update(core::Frame{8});
        CHECK(inventory.changes_since(changes.next_cursor).changes.empty());
        CHECK(registry.set_persistent_id(owner, {55}));
        const auto renewed = inventory.create(specification);
        CHECK(renewed);
        CHECK(renewed != container);
        CHECK(!inventory.inspect(container));
        CHECK(inventory.inspect(renewed)->persistent_owner == PersistentId{55});
    }
}

} // namespace

int main() {
    return ge::test::run("gameplay_inventory_audit", {
        {"changed owner identity retires container", changed_owner_identity_retires_container},
    });
}
