#pragma once

#include <ge/core/identity.hpp>
#include <ge/core/signals.hpp>
#include <ge/ecs/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }
namespace ge::persistence { class SaveJournal; }

namespace ge::gameplay {

struct InventoryContainerId {
    std::uint64_t scope{};
    RuntimeHandle handle{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return scope != 0 && handle.valid();
    }
    auto operator<=>(const InventoryContainerId&) const = default;
};

struct InventoryStackId {
    std::uint64_t scope{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return scope != 0 && sequence != 0;
    }
    auto operator<=>(const InventoryStackId&) const = default;
};

struct InventoryReceiptId {
    PersistentId issuer{};
    std::uint64_t scope{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return issuer.valid() && scope != 0 && sequence != 0;
    }
    auto operator<=>(const InventoryReceiptId&) const = default;
};

enum class InventoryError : std::uint8_t {
    None, AlreadyApplied, InvalidArgument, InvalidOwner, UnknownDefinition,
    Duplicate, MissingContainer, MissingStack, StaleRevision, Capacity,
    WeightLimit, Filtered, Insufficient, Reserved, Locked, ReceiptConflict,
    ReceiptCapacity, Exhausted, AllocationFailure, Corrupt, UnsupportedVersion,
    PersistenceFailure, Busy, NotEmpty
};

[[nodiscard]] std::string_view describe(InventoryError error) noexcept;

struct ItemDefinition {
    PersistentId id{};
    std::string name;
    std::string display_name;
    std::uint32_t maximum_stack{99};
    // Integer grams; zero denotes an item with no carried weight.
    std::uint64_t unit_weight{};
    std::uint64_t tags{};
    std::uint64_t equipment_slots{};
    std::uint32_t maximum_durability{};
    bool transferable{true};
    std::map<std::string, double, std::less<>> properties;
};

struct InventoryItemState {
    std::uint64_t variant{};
    // This sentinel requests the definition's maximum when adding an item.
    std::uint32_t durability{std::numeric_limits<std::uint32_t>::max()};
    auto operator<=>(const InventoryItemState&) const = default;
};

struct InventoryReservation {
    PersistentId token{};
    std::uint32_t quantity{};
    auto operator<=>(const InventoryReservation&) const = default;
};

struct InventoryStack {
    InventoryStackId id{};
    PersistentId definition{};
    InventoryItemState state{};
    std::uint32_t quantity{};
    std::vector<InventoryReservation> reservations;
    [[nodiscard]] std::uint32_t reserved() const noexcept;
    [[nodiscard]] std::uint32_t available() const noexcept;
};

struct InventoryContainerSpec {
    PersistentId id{};
    ecs::Entity owner{};
    std::string name{"inventory"};
    std::uint32_t slots{32};
    std::uint64_t maximum_weight{100000};
    std::uint64_t accepted_tags{~std::uint64_t{0}};
    std::uint64_t rejected_tags{};
    bool primary{true};
};

struct InventoryContainerSnapshot {
    InventoryContainerId id{};
    InventoryContainerSpec specification;
    PersistentId persistent_owner{};
    PersistentId lock{};
    std::uint64_t revision{1};
    std::uint64_t weight{};
    std::vector<InventoryStack> stacks;
};

enum class InventoryOperationKind : std::uint8_t {
    Add, Remove, Transfer, Reserve, Release, Consume, Lock, Unlock, Split
};

struct InventoryOperation {
    InventoryOperationKind kind{InventoryOperationKind::Add};
    InventoryContainerId source{};
    InventoryContainerId destination{};
    // Empty stack selects matching definition in stable stack order.
    InventoryStackId stack{};
    PersistentId definition{};
    std::uint64_t quantity{};
    InventoryItemState state{};
    PersistentId reservation{};
    // Zero opts out; every nonzero expectation refers to the pre-batch revision.
    std::uint64_t expected_source_revision{};
    std::uint64_t expected_destination_revision{};
};

struct InventoryTransaction {
    std::vector<InventoryOperation> operations;
    InventoryReceiptId receipt{};
    PersistentId access{};
    std::uint64_t expected_catalog_revision{};
};

struct InventoryResult {
    InventoryError error{InventoryError::None};
    std::uint64_t revision{};
    std::size_t operation{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == InventoryError::None || error == InventoryError::AlreadyApplied;
    }
};

enum class InventoryChangeKind : std::uint8_t {
    Created, Removed, Committed, Restored, DefinitionAdded, DefinitionRemoved
};

struct InventoryChange {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    std::uint64_t transaction_revision{};
    InventoryChangeKind kind{InventoryChangeKind::Committed};
    InventoryContainerId container{};
    PersistentId persistent_container{};
    ecs::Entity owner{};
    std::uint64_t container_revision{};
    InventoryReceiptId receipt{};
};

struct InventoryHistory {
    std::vector<InventoryChange> changes;
    std::uint64_t next_cursor{};
    std::uint64_t newest_sequence{};
    bool history_lost{};
};

struct InventoryLimits {
    std::uint32_t containers{1024};
    std::uint32_t slots_per_container{256};
    std::size_t definitions{4096};
    std::size_t properties_per_definition{32};
    std::size_t operations_per_transaction{128};
    std::size_t reservations_per_stack{16};
    std::size_t receipts{8192};
    std::size_t receipt_bytes{16384};
    std::size_t history{4096};
    std::size_t name_bytes{128};
    std::size_t snapshot_bytes{16U * 1024U * 1024U};
};

struct InventoryStatistics {
    std::size_t definitions{};
    std::size_t containers{};
    std::size_t stacks{};
    std::size_t reservations{};
    std::size_t receipts{};
    std::uint64_t revision{};
    std::uint64_t committed{};
    std::uint64_t rejected{};
    std::uint64_t duplicate_receipts{};
    std::uint64_t lost_signals{};
};

// Single kernel-thread writer; Registry exclusively owns all associated entities.
// Container generation handles and never-reused stack sequences belong to this
// service scope. Copies returned by inspect are safe across later mutations.
// A transaction prepares every affected container and its retained receipt before
// an allocation-free commit. Reservations reserve quantity, including for an
// equipped item; only matching Consume/Release operations may use that quantity.
// A lock requires transaction.access to match. A reservation token is a local
// coordination capability, not authentication. Revision checks use batch entry
// state. Committed receipts are retained permanently and capacity backpressures.
// Add may merge matching variants. Transfer preserves a whole stack's identity;
// partial transfers and Split receive fresh sequences and require another slot.
// General transaction receipts compare stable container IDs/stack sequences and
// semantic operations, excluding revision expectations. A retry after restore
// must resolve current container handles before resubmitting that transaction.
class InventoryService final {
public:
    explicit InventoryService(ecs::Registry& registry, InventoryLimits limits = {});
    ~InventoryService();
    InventoryService(const InventoryService&) = delete;
    InventoryService& operator=(const InventoryService&) = delete;
    InventoryResult define(ItemDefinition definition);
    InventoryResult undefine(PersistentId definition);
    [[nodiscard]] const ItemDefinition* definition(PersistentId id) const noexcept;
    [[nodiscard]] const ItemDefinition* definition(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<ItemDefinition> definitions() const;
    [[nodiscard]] InventoryContainerId create(InventoryContainerSpec specification);
    InventoryResult destroy(InventoryContainerId container, bool discard_contents = false);
    [[nodiscard]] InventoryContainerId find(PersistentId container) const noexcept;
    [[nodiscard]] InventoryContainerId primary(ecs::Entity owner) const noexcept;
    [[nodiscard]] std::vector<InventoryContainerId> owned_by(ecs::Entity owner) const;
    [[nodiscard]] std::optional<InventoryContainerSnapshot> inspect(InventoryContainerId id) const;
    [[nodiscard]] std::optional<InventoryStack> stack(InventoryContainerId container,
                                                   InventoryStackId stack) const;
    // Persistence key: container PersistentId + stack sequence. Restore retains
    // sequences; resolve returns the new service-scoped ID or invalid if absent.
    [[nodiscard]] InventoryStackId resolve_stack(PersistentId container, std::uint64_t sequence) const noexcept;
    [[nodiscard]] std::uint64_t quantity(InventoryContainerId container,
                                       PersistentId definition, bool include_reserved = true) const noexcept;
    InventoryResult transact(const InventoryTransaction& transaction);
    // Stable grant identity includes recipient, definition, quantity and exact
    // bounded application_context. Existing receipts are checked before owner
    // and container lookup, including after restore.
    InventoryResult grant(ecs::Entity owner, PersistentId persistent_owner,
                          PersistentId definition, std::uint64_t quantity,
                          InventoryReceiptId receipt, std::span<const std::byte> application_context = {});
    [[nodiscard]] bool applied(InventoryReceiptId receipt) const noexcept;
    void bind_signals(core::SignalHub& hub);
    [[nodiscard]] core::Signal<InventoryChange> change_signal() const noexcept;
    [[nodiscard]] InventoryHistory changes_since(std::uint64_t cursor,
                                                std::size_t limit = 4096) const;
    void update(const core::Frame& frame);
    // Snapshots include definitions, contents, reservations, locks and receipts.
    // Restore requires no live containers/receipts and resolves persistent owners
    // through Registry. No entities are allocated, and stale runtime IDs expire.
    // Consumers restore references with resolve_stack and their saved reservation
    // tokens. Saving other services separately provides no cross-service crash
    // atomicity; coordinate their snapshots in one journal transaction externally.
    InventoryResult snapshot(std::vector<std::byte>& destination) const;
    InventoryResult restore(std::span<const std::byte> bytes);
    InventoryResult save(persistence::SaveJournal& journal, std::string_view key) const;
    InventoryResult load(const persistence::SaveJournal& journal, std::string_view key);
    [[nodiscard]] InventoryError last_error() const noexcept;
    [[nodiscard]] std::uint64_t catalog_revision() const noexcept;
    [[nodiscard]] InventoryStatistics statistics() const noexcept;
    [[nodiscard]] const InventoryLimits& limits() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_inventory(core::Kernel& kernel);

} // namespace ge::gameplay
