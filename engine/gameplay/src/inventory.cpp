#include <ge/gameplay/inventory.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/gameplay/mission.hpp>
#include <ge/persistence/journal.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace ge::gameplay {
namespace {

constexpr auto maximum_counter = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t inventory_value_type = 0x494e5654U;
constexpr PersistentId mission_issuer{0x4d495353494f4e49ULL};
std::atomic<std::uint64_t> next_scope{1};

std::uint64_t allocate_scope() {
    auto current = next_scope.load(std::memory_order_relaxed);
    while (current != 0 && current != maximum_counter) {
        if (next_scope.compare_exchange_weak(current, current + 1,
                                            std::memory_order_relaxed)) {
            return current;
        }
    }
    throw std::overflow_error("inventory service identity exhausted");
}

void increment(std::uint64_t& value) noexcept {
    if (value != maximum_counter) {
        ++value;
    }
}

bool number(std::string_view text, std::uint64_t& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool valid_text(std::string_view text, std::size_t limit) noexcept {
    return !text.empty() && text.size() <= limit && text.find('\0') == std::string_view::npos &&
           core::valid_utf8(text);
}

bool empty_receipt(InventoryReceiptId id) noexcept {
    return !id.issuer && id.scope == 0 && id.sequence == 0;
}

InventoryLimits checked_limits(InventoryLimits limits) {
    if (limits.containers == 0 || limits.containers > 65536 ||
        limits.slots_per_container == 0 || limits.slots_per_container > 4096 ||
        limits.definitions == 0 || limits.definitions > 65536 ||
        limits.properties_per_definition > 256 || limits.operations_per_transaction == 0 ||
        limits.operations_per_transaction > 4096 || limits.reservations_per_stack > 256 ||
        limits.receipts > 65536 || limits.history > 262144 || limits.name_bytes == 0 ||
        limits.name_bytes > 4096 || limits.snapshot_bytes < 128 ||
        limits.snapshot_bytes > 256U * 1024U * 1024U || limits.receipt_bytes < 32 ||
        limits.receipt_bytes > 1024U * 1024U || limits.receipt_bytes > limits.snapshot_bytes) {
        throw std::invalid_argument("invalid inventory limits");
    }
    return limits;
}

void write_receipt(core::ByteWriter& writer, InventoryReceiptId id) {
    writer.write_u64(id.issuer.value);
    writer.write_u64(id.scope);
    writer.write_u64(id.sequence);
}

InventoryReceiptId read_receipt(core::ByteReader& reader) {
    InventoryReceiptId result;
    result.issuer.value = reader.read_u64();
    result.scope = reader.read_u64();
    result.sequence = reader.read_u64();
    return result;
}

bool matches(const InventoryStack& stack, const InventoryOperation& operation) noexcept {
    return (!operation.stack || stack.id == operation.stack) &&
           (!operation.definition || stack.definition == operation.definition);
}

std::size_t reservation_index(const InventoryStack& stack, PersistentId token) noexcept {
    for (std::size_t index = 0; index < stack.reservations.size(); ++index) {
        if (stack.reservations[index].token == token) {
            return index;
        }
    }
    return stack.reservations.size();
}

void erase_empty(InventoryContainerSnapshot& container) {
    std::erase_if(container.stacks, [](const InventoryStack& stack) {
        return stack.quantity == 0;
    });
}

} // namespace

std::string_view describe(InventoryError error) noexcept {
    switch (error) {
    case InventoryError::None: return "applied";
    case InventoryError::AlreadyApplied: return "already applied";
    case InventoryError::InvalidArgument: return "invalid argument";
    case InventoryError::InvalidOwner: return "invalid inventory owner";
    case InventoryError::UnknownDefinition: return "unknown item definition";
    case InventoryError::Duplicate: return "duplicate identity or name";
    case InventoryError::MissingContainer: return "missing or stale container";
    case InventoryError::MissingStack: return "missing or stale stack";
    case InventoryError::StaleRevision: return "stale revision";
    case InventoryError::Capacity: return "inventory capacity exhausted";
    case InventoryError::WeightLimit: return "container weight limit exceeded";
    case InventoryError::Filtered: return "item rejected by container filter";
    case InventoryError::Insufficient: return "insufficient item quantity";
    case InventoryError::Reserved: return "item quantity is reserved";
    case InventoryError::Locked: return "container is locked";
    case InventoryError::ReceiptConflict: return "receipt reused with different request";
    case InventoryError::ReceiptCapacity: return "retained receipt capacity exhausted";
    case InventoryError::Exhausted: return "inventory identity or revision exhausted";
    case InventoryError::AllocationFailure: return "inventory allocation failed";
    case InventoryError::Corrupt: return "invalid inventory snapshot";
    case InventoryError::UnsupportedVersion: return "unsupported inventory snapshot";
    case InventoryError::PersistenceFailure: return "inventory journal operation failed";
    case InventoryError::Busy: return "inventory is busy";
    case InventoryError::NotEmpty: return "inventory must be empty";
    }
    return "unknown inventory error";
}

std::uint32_t InventoryStack::reserved() const noexcept {
    std::uint64_t total{};
    for (const auto& reservation : reservations) {
        total += reservation.quantity;
        if (total >= quantity) {
            return quantity;
        }
    }
    return static_cast<std::uint32_t>(total);
}

std::uint32_t InventoryStack::available() const noexcept {
    return quantity - reserved();
}

struct InventoryService::Impl {
    struct Receipt {
        InventoryReceiptId id;
        std::vector<std::byte> request;
        std::uint64_t revision{};
    };
    ecs::Registry& registry;
    InventoryLimits limits;
    GenerationPool handles;
    std::uint64_t scope{allocate_scope()};
    std::map<PersistentId, ItemDefinition> definitions;
    std::vector<std::unique_ptr<InventoryContainerSnapshot>> containers;
    std::vector<Receipt> receipts;
    std::vector<InventoryChange> history;
    std::size_t history_begin{};
    std::uint64_t sequence{};
    std::uint64_t next_stack{1};
    std::uint64_t catalog_revision{};
    std::uint64_t revision{};
    std::uint64_t tick{};
    std::uint64_t committed{};
    std::uint64_t rejected{};
    std::uint64_t duplicate_receipts{};
    std::uint64_t lost_signals{};
    InventoryError error{InventoryError::None};
    core::SignalHub* signals{};
    core::Signal<InventoryChange> signal{};
    bool busy{};

    Impl(ecs::Registry& owner, InventoryLimits bounds)
        : registry(owner), limits(checked_limits(bounds)), handles(limits.containers), containers(limits.containers) {
        receipts.reserve(limits.receipts);
        history.reserve(limits.history);
    }

    InventoryResult result(InventoryError code, std::size_t operation = 0) noexcept {
        error = code;
        if (code != InventoryError::None && code != InventoryError::AlreadyApplied) {
            increment(rejected);
        }
        return {code, revision, operation};
    }

    InventoryContainerSnapshot* get(InventoryContainerId id) noexcept {
        if (id.scope != scope || !handles.alive(id.handle)) {
            return nullptr;
        }
        return containers[static_cast<std::size_t>(id.handle.index - 1)].get();
    }

    const InventoryContainerSnapshot* get(InventoryContainerId id) const noexcept {
        if (id.scope != scope || !handles.alive(id.handle)) {
            return nullptr;
        }
        return containers[static_cast<std::size_t>(id.handle.index - 1)].get();
    }

    const ItemDefinition* item(PersistentId id) const noexcept {
        const auto found = definitions.find(id);
        return found == definitions.end() ? nullptr : &found->second;
    }

    const Receipt* receipt(InventoryReceiptId id) const noexcept {
        const auto found = std::find_if(receipts.begin(), receipts.end(), [id](const Receipt& value) {
            return value.id == id;
        });
        return found == receipts.end() ? nullptr : &*found;
    }

    bool owner_valid(const InventoryContainerSnapshot& container) const noexcept {
        return !container.specification.owner ||
               (registry.alive(container.specification.owner) &&
                registry.persistent_id(container.specification.owner) == container.persistent_owner);
    }

    bool room_for_events(std::size_t count) const noexcept {
        return revision != maximum_counter && count <= maximum_counter - sequence;
    }

    void record(InventoryChangeKind kind, const InventoryContainerSnapshot* container = nullptr,
                InventoryReceiptId receipt_id = {}) noexcept {
        InventoryChange change;
        change.sequence = ++sequence;
        change.tick = tick;
        change.transaction_revision = revision;
        change.kind = kind;
        change.receipt = receipt_id;
        if (container) {
            change.container = container->id;
            change.persistent_container = container->specification.id;
            change.owner = container->specification.owner;
            change.container_revision = container->revision;
        }
        if (history.size() < limits.history) {
            history.push_back(change);
        } else if (!history.empty()) {
            history[history_begin] = change;
            history_begin = (history_begin + 1) % history.size();
        }
        if (signals && signal) {
            try {
                if (!signals->enqueue(signal, change)) {
                    increment(lost_signals);
                }
            } catch (...) {
                increment(lost_signals);
            }
        }
    }

    InventoryError definition_valid(const ItemDefinition& value) const noexcept {
        if (!value.id || !valid_text(value.name, limits.name_bytes) ||
            (!value.display_name.empty() && !valid_text(value.display_name, limits.name_bytes)) ||
            value.maximum_stack == 0 || value.properties.size() > limits.properties_per_definition) {
            return InventoryError::InvalidArgument;
        }
        for (const auto& [name, amount] : value.properties) {
            if (!valid_text(name, limits.name_bytes) || !std::isfinite(amount)) {
                return InventoryError::InvalidArgument;
            }
        }
        return InventoryError::None;
    }

    InventoryError validate(InventoryContainerSnapshot& container) const noexcept {
        if (container.stacks.size() > container.specification.slots) {
            return InventoryError::Capacity;
        }
        std::uint64_t weight{};
        for (const auto& stack : container.stacks) {
            const auto* definition = item(stack.definition);
            if (!definition) {
                return InventoryError::UnknownDefinition;
            }
            if (!stack.id || stack.id.scope != scope || stack.quantity == 0 ||
                stack.quantity > definition->maximum_stack ||
                stack.state.durability > definition->maximum_durability) {
                return InventoryError::InvalidArgument;
            }
            if ((definition->tags & container.specification.rejected_tags) != 0 ||
                (definition->tags != 0 && (definition->tags & container.specification.accepted_tags) == 0)) {
                return InventoryError::Filtered;
            }
            std::uint64_t reserved{};
            for (std::size_t index = 0; index < stack.reservations.size(); ++index) {
                const auto& reservation = stack.reservations[index];
                if (!reservation.token || reservation.quantity == 0 ||
                    reservation_index(stack, reservation.token) != index) {
                    return InventoryError::InvalidArgument;
                }
                reserved += reservation.quantity;
            }
            if (reserved > stack.quantity || stack.reservations.size() > limits.reservations_per_stack) {
                return InventoryError::Reserved;
            }
            if (definition->unit_weight != 0 &&
                stack.quantity > (container.specification.maximum_weight - weight) / definition->unit_weight) {
                return InventoryError::WeightLimit;
            }
            weight += definition->unit_weight * stack.quantity;
        }
        container.weight = weight;
        return InventoryError::None;
    }

    InventoryError add(InventoryContainerSnapshot& container, PersistentId definition_id,
                       InventoryItemState state, std::uint64_t quantity, std::uint64_t& serial) {
        const auto* definition = item(definition_id);
        if (!definition) {
            return InventoryError::UnknownDefinition;
        }
        if (state.durability == std::numeric_limits<std::uint32_t>::max()) {
            state.durability = definition->maximum_durability;
        }
        if (state.durability > definition->maximum_durability) {
            return InventoryError::InvalidArgument;
        }
        for (auto& stack : container.stacks) {
            if (stack.definition != definition_id || stack.state != state) {
                continue;
            }
            const auto space = definition->maximum_stack - stack.quantity;
            const auto amount = static_cast<std::uint32_t>(std::min<std::uint64_t>(space, quantity));
            stack.quantity += amount;
            quantity -= amount;
        }
        const auto needed = quantity / definition->maximum_stack +
                            static_cast<std::uint64_t>(quantity % definition->maximum_stack != 0);
        if (needed > container.specification.slots - container.stacks.size()) {
            return InventoryError::Capacity;
        }
        while (quantity != 0) {
            if (serial == 0 || serial == maximum_counter) {
                return InventoryError::Exhausted;
            }
            const auto amount = static_cast<std::uint32_t>(std::min<std::uint64_t>(quantity, definition->maximum_stack));
            container.stacks.push_back({{scope, serial++}, definition_id, state, amount, {}});
            quantity -= amount;
        }
        return InventoryError::None;
    }

    InventoryError manipulate(InventoryContainerSnapshot& container,
                              const InventoryOperation& operation,
                              InventoryContainerSnapshot* destination,
                              std::uint64_t& serial) {
        if (operation.quantity == 0 || (!operation.stack && !operation.definition)) {
            return InventoryError::InvalidArgument;
        }
        if (operation.stack && operation.stack.scope != scope) {
            return InventoryError::MissingStack;
        }
        if (operation.definition && !item(operation.definition)) {
            return InventoryError::UnknownDefinition;
        }
        const bool token_operation = operation.kind == InventoryOperationKind::Reserve ||
            operation.kind == InventoryOperationKind::Release || operation.kind == InventoryOperationKind::Consume;
        if (token_operation && !operation.reservation) {
            return InventoryError::InvalidArgument;
        }
        std::uint64_t remaining = operation.quantity;
        bool found{};
        bool blocked{};
        for (auto& stack : container.stacks) {
            if (remaining == 0 || !matches(stack, operation)) {
                continue;
            }
            found = true;
            blocked = blocked || stack.reserved() != 0;
            const auto reserved_index = reservation_index(stack, operation.reservation);
            const bool consuming = operation.kind == InventoryOperationKind::Consume ||
                                   operation.kind == InventoryOperationKind::Release;
            const auto available = consuming
                ? (reserved_index < stack.reservations.size() ? stack.reservations[reserved_index].quantity : 0U)
                : stack.available();
            const auto amount = static_cast<std::uint32_t>(std::min<std::uint64_t>(available, remaining));
            if (amount == 0) {
                blocked = blocked || !stack.reservations.empty();
                continue;
            }
            if (operation.kind == InventoryOperationKind::Reserve) {
                if (reserved_index == stack.reservations.size()) {
                    if (stack.reservations.size() >= limits.reservations_per_stack) {
                        return InventoryError::Capacity;
                    }
                    stack.reservations.push_back({operation.reservation, amount});
                } else {
                    stack.reservations[reserved_index].quantity += amount;
                }
            } else if (consuming) {
                auto& reservation = stack.reservations[reserved_index];
                reservation.quantity -= amount;
                if (reservation.quantity == 0) {
                    stack.reservations.erase(stack.reservations.begin() + static_cast<std::ptrdiff_t>(reserved_index));
                }
                if (operation.kind == InventoryOperationKind::Consume) {
                    stack.quantity -= amount;
                }
            } else {
                if (destination) {
                    const auto* definition = item(stack.definition);
                    if (!definition || !definition->transferable) {
                        return InventoryError::Filtered;
                    }
                    if (destination->stacks.size() >= destination->specification.slots) {
                        return InventoryError::Capacity;
                    }
                    auto identity = stack.id;
                    if (amount != stack.quantity) {
                        if (serial == 0 || serial == maximum_counter) {
                            return InventoryError::Exhausted;
                        }
                        identity = {scope, serial++};
                    }
                    destination->stacks.push_back({identity, stack.definition, stack.state, amount, {}});
                }
                stack.quantity -= amount;
            }
            remaining -= amount;
        }
        if (remaining != 0) {
            return !found && operation.stack ? InventoryError::MissingStack
                 : blocked ? InventoryError::Reserved : InventoryError::Insufficient;
        }
        erase_empty(container);
        return InventoryError::None;
    }

    std::vector<std::byte> request_bytes(const InventoryTransaction& transaction) const {
        core::ByteWriter writer(limits.receipt_bytes);
        writer.write_u8(0);
        writer.write_u64(transaction.access.value);
        writer.write_varuint(transaction.operations.size());
        for (const auto& operation : transaction.operations) {
            const auto* source = get(operation.source);
            const auto* destination = get(operation.destination);
            writer.write_u8(static_cast<std::uint8_t>(operation.kind));
            writer.write_u64(source ? source->specification.id.value : 0);
            writer.write_u64(destination ? destination->specification.id.value : 0);
            writer.write_u64(operation.stack.sequence);
            writer.write_u64(operation.definition.value);
            writer.write_u64(operation.quantity);
            writer.write_u64(operation.state.variant);
            writer.write_u32(operation.state.durability);
            writer.write_u64(operation.reservation.value);
        }
        return writer.take();
    }

    InventoryResult execute(const InventoryTransaction& transaction, std::vector<std::byte> request) {
        if (busy) {
            return result(InventoryError::Busy);
        }
        if (transaction.operations.empty() || transaction.operations.size() > limits.operations_per_transaction ||
            (!empty_receipt(transaction.receipt) && !transaction.receipt)) {
            return result(InventoryError::InvalidArgument);
        }
        if (transaction.receipt) {
            if (const auto* prior = receipt(transaction.receipt)) {
                if (prior->request != request) {
                    return result(InventoryError::ReceiptConflict);
                }
                increment(duplicate_receipts);
                error = InventoryError::AlreadyApplied;
                return {error, prior->revision, 0};
            }
            if (receipts.size() >= limits.receipts) {
                return result(InventoryError::ReceiptCapacity);
            }
        }
        if (transaction.expected_catalog_revision != 0 && transaction.expected_catalog_revision != catalog_revision) {
            return result(InventoryError::StaleRevision);
        }
        std::map<std::uint32_t, std::unique_ptr<InventoryContainerSnapshot>> draft;
        std::uint64_t serial = next_stack;
        for (std::size_t index = 0; index < transaction.operations.size(); ++index) {
            const auto& operation = transaction.operations[index];
            auto prepare = [&](InventoryContainerId id, std::uint64_t expected) -> InventoryError {
                const auto* original = get(id);
                if (!original) {
                    return InventoryError::MissingContainer;
                }
                if (!owner_valid(*original)) {
                    return InventoryError::InvalidOwner;
                }
                if (expected != 0 && original->revision != expected) {
                    return InventoryError::StaleRevision;
                }
                if (original->revision == maximum_counter) {
                    return InventoryError::Exhausted;
                }
                if (!draft.contains(id.handle.index)) {
                    draft.emplace(id.handle.index, std::make_unique<InventoryContainerSnapshot>(*original));
                }
                return InventoryError::None;
            };
            auto status = prepare(operation.source, operation.expected_source_revision);
            if (status != InventoryError::None) {
                return result(status, index);
            }
            auto& source = *draft.at(operation.source.handle.index);
            if (source.lock && source.lock != transaction.access) {
                return result(InventoryError::Locked, index);
            }
            InventoryContainerSnapshot* destination{};
            if (operation.kind == InventoryOperationKind::Transfer) {
                if (operation.source == operation.destination) {
                    return result(InventoryError::InvalidArgument, index);
                }
                status = prepare(operation.destination, operation.expected_destination_revision);
                if (status != InventoryError::None) {
                    return result(status, index);
                }
                destination = draft.at(operation.destination.handle.index).get();
                if (destination->lock && destination->lock != transaction.access) {
                    return result(InventoryError::Locked, index);
                }
            }
            switch (operation.kind) {
            case InventoryOperationKind::Add:
                status = operation.quantity == 0 || operation.stack ? InventoryError::InvalidArgument
                       : add(source, operation.definition, operation.state, operation.quantity, serial);
                break;
            case InventoryOperationKind::Lock:
                status = !operation.reservation || transaction.access != operation.reservation
                    ? InventoryError::InvalidArgument : InventoryError::None;
                if (status == InventoryError::None) {
                    source.lock = operation.reservation;
                }
                break;
            case InventoryOperationKind::Unlock:
                source.lock = {};
                break;
            case InventoryOperationKind::Split: {
                auto found = std::find_if(source.stacks.begin(), source.stacks.end(), [&](const InventoryStack& value) {
                    return operation.stack && matches(value, operation);
                });
                if (found == source.stacks.end()) {
                    status = InventoryError::MissingStack;
                } else if (operation.quantity == 0 || operation.quantity >= found->quantity ||
                           operation.quantity > found->available()) {
                    status = InventoryError::Insufficient;
                } else if (source.stacks.size() >= source.specification.slots) {
                    status = InventoryError::Capacity;
                } else if (serial == 0 || serial == maximum_counter) {
                    status = InventoryError::Exhausted;
                } else {
                    const auto amount = static_cast<std::uint32_t>(operation.quantity);
                    InventoryStack split{{scope, serial++}, found->definition, found->state, amount, {}};
                    found->quantity -= amount;
                    source.stacks.push_back(std::move(split));
                }
                break;
            }
            case InventoryOperationKind::Remove:
            case InventoryOperationKind::Transfer:
            case InventoryOperationKind::Reserve:
            case InventoryOperationKind::Release:
            case InventoryOperationKind::Consume:
                status = manipulate(source, operation, destination, serial);
                break;
            default:
                status = InventoryError::InvalidArgument;
                break;
            }
            if (status != InventoryError::None) {
                return result(status, index);
            }
        }
        if (!room_for_events(draft.size())) {
            return result(InventoryError::Exhausted);
        }
        for (auto& [index, container] : draft) {
            static_cast<void>(index);
            const auto status = validate(*container);
            if (status != InventoryError::None) {
                return result(status, transaction.operations.size() - 1);
            }
            ++container->revision;
        }
        Receipt applied_receipt{transaction.receipt, std::move(request), revision + 1};
        busy = true;
        for (auto& [index, container] : draft) {
            containers[static_cast<std::size_t>(index - 1)].swap(container);
        }
        next_stack = serial;
        ++revision;
        if (transaction.receipt) {
            receipts.push_back(std::move(applied_receipt));
        }
        increment(committed);
        for (const auto& [index, old_container] : draft) {
            static_cast<void>(old_container);
            record(InventoryChangeKind::Committed, containers[static_cast<std::size_t>(index - 1)].get(), transaction.receipt);
        }
        busy = false;
        return result(InventoryError::None);
    }
};

InventoryService::InventoryService(ecs::Registry& registry, InventoryLimits limits)
    : impl_(std::make_unique<Impl>(registry, limits)) {}

InventoryService::~InventoryService() = default;

InventoryResult InventoryService::define(ItemDefinition value) {
    auto& self = *impl_;
    if (self.busy) {
        return self.result(InventoryError::Busy);
    }
    const auto status = self.definition_valid(value);
    if (status != InventoryError::None) {
        return self.result(status);
    }
    if (definition(value.id) || definition(value.name)) {
        return self.result(InventoryError::Duplicate);
    }
    if (self.definitions.size() >= self.limits.definitions) {
        return self.result(InventoryError::Capacity);
    }
    if (!self.room_for_events(1) || self.catalog_revision == maximum_counter) {
        return self.result(InventoryError::Exhausted);
    }
    try {
        const auto id = value.id;
        self.definitions.emplace(id, std::move(value));
    } catch (const std::bad_alloc&) {
        return self.result(InventoryError::AllocationFailure);
    }
    ++self.catalog_revision;
    ++self.revision;
    self.record(InventoryChangeKind::DefinitionAdded);
    return self.result(InventoryError::None);
}

InventoryResult InventoryService::undefine(PersistentId id) {
    auto& self = *impl_;
    if (self.busy) {
        return self.result(InventoryError::Busy);
    }
    if (!definition(id)) {
        return self.result(InventoryError::UnknownDefinition);
    }
    for (const auto& container : self.containers) {
        if (container && quantity(container->id, id) != 0) {
            return self.result(InventoryError::NotEmpty);
        }
    }
    // Definitions referenced by retained grants must not be rebound to new content.
    if (!self.receipts.empty()) {
        return self.result(InventoryError::Busy);
    }
    if (!self.room_for_events(1) || self.catalog_revision == maximum_counter) {
        return self.result(InventoryError::Exhausted);
    }
    self.definitions.erase(id);
    ++self.catalog_revision;
    ++self.revision;
    self.record(InventoryChangeKind::DefinitionRemoved);
    return self.result(InventoryError::None);
}

const ItemDefinition* InventoryService::definition(PersistentId id) const noexcept {
    return impl_->item(id);
}

const ItemDefinition* InventoryService::definition(std::string_view name) const noexcept {
    for (const auto& [id, value] : impl_->definitions) {
        static_cast<void>(id);
        if (value.name == name) {
            return &value;
        }
    }
    return nullptr;
}

std::vector<ItemDefinition> InventoryService::definitions() const {
    std::vector<ItemDefinition> result;
    result.reserve(impl_->definitions.size());
    for (const auto& [id, value] : impl_->definitions) {
        static_cast<void>(id);
        result.push_back(value);
    }
    return result;
}

InventoryContainerId InventoryService::create(InventoryContainerSpec specification) {
    auto& self = *impl_;
    auto fail = [&](InventoryError status) {
        static_cast<void>(self.result(status));
        return InventoryContainerId{};
    };
    if (self.busy) {
        return fail(InventoryError::Busy);
    }
    if (!specification.id || !valid_text(specification.name, self.limits.name_bytes) ||
        specification.slots == 0 || specification.slots > self.limits.slots_per_container) {
        return fail(InventoryError::InvalidArgument);
    }
    if (find(specification.id) || (specification.primary && specification.owner && primary(specification.owner))) {
        return fail(InventoryError::Duplicate);
    }
    if (specification.owner && (!self.registry.alive(specification.owner) ||
                                !self.registry.persistent_id(specification.owner))) {
        return fail(InventoryError::InvalidOwner);
    }
    if (!self.room_for_events(1)) {
        return fail(InventoryError::Exhausted);
    }
    try {
        auto container = std::make_unique<InventoryContainerSnapshot>();
        container->persistent_owner = self.registry.persistent_id(specification.owner);
        container->specification = std::move(specification);
        const auto handle = self.handles.acquire();
        if (!handle) {
            return fail(InventoryError::Capacity);
        }
        container->id = {self.scope, *handle};
        const auto id = container->id;
        auto& destination = self.containers[static_cast<std::size_t>(handle->index - 1)];
        destination.swap(container);
        ++self.revision;
        self.record(InventoryChangeKind::Created, destination.get());
        self.error = InventoryError::None;
        return id;
    } catch (const std::bad_alloc&) {
        return fail(InventoryError::AllocationFailure);
    }
}

InventoryResult InventoryService::destroy(InventoryContainerId id, bool discard_contents) {
    auto& self = *impl_;
    if (self.busy) {
        return self.result(InventoryError::Busy);
    }
    const auto* container = self.get(id);
    if (!container) {
        return self.result(InventoryError::MissingContainer);
    }
    if (container->lock) {
        return self.result(InventoryError::Locked);
    }
    if (!discard_contents && !container->stacks.empty()) {
        return self.result(InventoryError::NotEmpty);
    }
    for (const auto& stack : container->stacks) {
        if (!stack.reservations.empty()) {
            return self.result(InventoryError::Reserved);
        }
    }
    if (!self.room_for_events(1)) {
        return self.result(InventoryError::Exhausted);
    }
    ++self.revision;
    self.record(InventoryChangeKind::Removed, container);
    self.containers[static_cast<std::size_t>(id.handle.index - 1)].reset();
    static_cast<void>(self.handles.release(id.handle));
    return self.result(InventoryError::None);
}

InventoryContainerId InventoryService::find(PersistentId id) const noexcept {
    for (const auto& container : impl_->containers) {
        if (container && container->specification.id == id) {
            return container->id;
        }
    }
    return {};
}

InventoryContainerId InventoryService::primary(ecs::Entity owner) const noexcept {
    for (const auto& container : impl_->containers) {
        if (container && container->specification.owner == owner && container->specification.primary &&
            impl_->owner_valid(*container)) {
            return container->id;
        }
    }
    return {};
}

std::vector<InventoryContainerId> InventoryService::owned_by(ecs::Entity owner) const {
    std::vector<InventoryContainerId> result;
    for (const auto& container : impl_->containers) {
        if (container && container->specification.owner == owner && impl_->owner_valid(*container)) {
            result.push_back(container->id);
        }
    }
    return result;
}

std::optional<InventoryContainerSnapshot> InventoryService::inspect(InventoryContainerId id) const {
    const auto* container = impl_->get(id);
    return container ? std::optional<InventoryContainerSnapshot>(*container) : std::nullopt;
}

std::optional<InventoryStack> InventoryService::stack(InventoryContainerId id, InventoryStackId stack_id) const {
    const auto* container = impl_->get(id);
    if (container) {
        for (const auto& value : container->stacks) {
            if (value.id == stack_id) {
                return value;
            }
        }
    }
    return std::nullopt;
}

InventoryStackId InventoryService::resolve_stack(PersistentId container_id, std::uint64_t sequence) const noexcept {
    const auto* container = impl_->get(find(container_id));
    if (container) {
        for (const auto& value : container->stacks) {
            if (value.id.sequence == sequence) { return value.id; }
        }
    }
    return {};
}

std::uint64_t InventoryService::quantity(InventoryContainerId id, PersistentId definition_id,
                                        bool include_reserved) const noexcept {
    const auto* container = impl_->get(id);
    std::uint64_t result{};
    if (container) {
        for (const auto& stack : container->stacks) {
            if (stack.definition == definition_id) {
                result += include_reserved ? stack.quantity : stack.available();
            }
        }
    }
    return result;
}

InventoryResult InventoryService::transact(const InventoryTransaction& transaction) {
    try {
        return impl_->execute(transaction, transaction.receipt ? impl_->request_bytes(transaction) : std::vector<std::byte>{});
    } catch (const std::bad_alloc&) {
        return impl_->result(InventoryError::AllocationFailure);
    } catch (const core::BinaryException&) {
        return impl_->result(InventoryError::Capacity);
    } catch (const std::length_error&) {
        return impl_->result(InventoryError::Capacity);
    }
}

InventoryResult InventoryService::grant(ecs::Entity owner, PersistentId persistent_owner,
                                        PersistentId definition_id, std::uint64_t amount,
                                        InventoryReceiptId receipt_id, std::span<const std::byte> application_context) {
    auto& self = *impl_;
    if (!persistent_owner || !definition_id || amount == 0 || !receipt_id) {
        return self.result(InventoryError::InvalidArgument);
    }
    try {
        core::ByteWriter writer(self.limits.receipt_bytes);
        writer.write_u8(1);
        writer.write_u64(persistent_owner.value);
        writer.write_u64(definition_id.value);
        writer.write_u64(amount);
        writer.write_sized_bytes(application_context);
        auto request = writer.take();
        if (const auto* previous = self.receipt(receipt_id)) {
            if (previous->request != request) {
                return self.result(InventoryError::ReceiptConflict);
            }
            increment(self.duplicate_receipts);
            self.error = InventoryError::AlreadyApplied;
            return {self.error, previous->revision, 0};
        }
        if (!self.registry.alive(owner) || self.registry.persistent_id(owner) != persistent_owner) {
            return self.result(InventoryError::InvalidOwner);
        }
        InventoryTransaction transaction;
        transaction.receipt = receipt_id;
        InventoryOperation operation;
        operation.source = primary(owner);
        operation.definition = definition_id;
        operation.quantity = amount;
        transaction.operations.push_back(operation);
        return self.execute(transaction, std::move(request));
    } catch (const std::bad_alloc&) {
        return self.result(InventoryError::AllocationFailure);
    } catch (const core::BinaryException&) {
        return self.result(InventoryError::Capacity);
    } catch (const std::length_error&) {
        return self.result(InventoryError::Capacity);
    }
}

bool InventoryService::applied(InventoryReceiptId receipt) const noexcept {
    return impl_->receipt(receipt) != nullptr;
}

void InventoryService::bind_signals(core::SignalHub& hub) {
    const auto channel = hub.channel<InventoryChange>("gameplay.inventory.changed");
    impl_->signals = &hub;
    impl_->signal = channel;
}

core::Signal<InventoryChange> InventoryService::change_signal() const noexcept {
    return impl_->signal;
}

InventoryHistory InventoryService::changes_since(std::uint64_t cursor, std::size_t limit) const {
    const auto& self = *impl_;
    InventoryHistory result;
    result.next_cursor = cursor;
    result.newest_sequence = self.sequence;
    const auto oldest = self.history.empty() ? self.sequence : self.history[self.history_begin].sequence - 1;
    result.history_lost = cursor < oldest || cursor > self.sequence;
    if (cursor > self.sequence) {
        result.next_cursor = self.sequence;
        return result;
    }
    result.changes.reserve(std::min(limit, self.history.size()));
    for (std::size_t index = 0; index < self.history.size() && result.changes.size() < limit; ++index) {
        const auto& change = self.history[(self.history_begin + index) % self.history.size()];
        if (change.sequence > cursor) {
            result.changes.push_back(change);
            result.next_cursor = change.sequence;
        }
    }
    if (self.history.empty()) {
        result.next_cursor = self.sequence;
    }
    return result;
}

void InventoryService::update(const core::Frame& frame) {
    auto& self = *impl_;
    self.tick = frame.tick;
    for (auto& container : self.containers) {
        if (!container || self.owner_valid(*container)) {
            continue;
        }
        if (!self.room_for_events(1)) {
            self.error = InventoryError::Exhausted;
            break;
        }
        ++self.revision;
        self.record(InventoryChangeKind::Removed, container.get());
        static_cast<void>(self.handles.release(container->id.handle));
        container.reset();
    }
}

InventoryError InventoryService::last_error() const noexcept {
    return impl_->error;
}

std::uint64_t InventoryService::catalog_revision() const noexcept {
    return impl_->catalog_revision;
}

InventoryStatistics InventoryService::statistics() const noexcept {
    InventoryStatistics result;
    result.definitions = impl_->definitions.size();
    result.containers = impl_->handles.size();
    result.receipts = impl_->receipts.size();
    result.revision = impl_->revision;
    result.committed = impl_->committed;
    result.rejected = impl_->rejected;
    result.duplicate_receipts = impl_->duplicate_receipts;
    result.lost_signals = impl_->lost_signals;
    for (const auto& container : impl_->containers) {
        if (container) {
            result.stacks += container->stacks.size();
            for (const auto& stack : container->stacks) {
                result.reservations += stack.reservations.size();
            }
        }
    }
    return result;
}

const InventoryLimits& InventoryService::limits() const noexcept {
    return impl_->limits;
}

InventoryResult InventoryService::snapshot(std::vector<std::byte>& destination) const {
    auto& self = *impl_;
    try {
        core::ByteWriter writer(self.limits.snapshot_bytes);
        writer.write_bytes("GEINV001");
        writer.write_u16(1);
        writer.write_u64(self.revision);
        writer.write_u64(self.catalog_revision);
        writer.write_u64(self.next_stack);
        writer.write_varuint(self.definitions.size());
        for (const auto& [id, value] : self.definitions) {
            writer.write_u64(id.value);
            writer.write_string(value.name, self.limits.name_bytes);
            writer.write_string(value.display_name, self.limits.name_bytes);
            writer.write_u32(value.maximum_stack);
            writer.write_u64(value.unit_weight);
            writer.write_u64(value.tags);
            writer.write_u64(value.equipment_slots);
            writer.write_u32(value.maximum_durability);
            writer.write_bool(value.transferable);
            writer.write_varuint(value.properties.size());
            for (const auto& [name, amount] : value.properties) {
                writer.write_string(name, self.limits.name_bytes);
                writer.write_f64(amount);
            }
        }
        writer.write_varuint(self.handles.size());
        for (const auto& container : self.containers) {
            if (!container) { continue; }
            if (!self.owner_valid(*container)) { return self.result(InventoryError::InvalidOwner); }
            const auto& spec = container->specification;
            writer.write_u64(spec.id.value);
            writer.write_u64(container->persistent_owner.value);
            writer.write_string(spec.name, self.limits.name_bytes);
            writer.write_u32(spec.slots);
            writer.write_u64(spec.maximum_weight);
            writer.write_u64(spec.accepted_tags);
            writer.write_u64(spec.rejected_tags);
            writer.write_bool(spec.primary);
            writer.write_u64(container->lock.value);
            writer.write_u64(container->revision);
            writer.write_varuint(container->stacks.size());
            for (const auto& stack : container->stacks) {
                writer.write_u64(stack.id.sequence);
                writer.write_u64(stack.definition.value);
                writer.write_u64(stack.state.variant);
                writer.write_u32(stack.state.durability);
                writer.write_u32(stack.quantity);
                writer.write_varuint(stack.reservations.size());
                for (const auto& reservation : stack.reservations) {
                    writer.write_u64(reservation.token.value);
                    writer.write_u32(reservation.quantity);
                }
            }
        }
        writer.write_varuint(self.receipts.size());
        for (const auto& receipt : self.receipts) {
            write_receipt(writer, receipt.id);
            writer.write_u64(receipt.revision);
            writer.write_sized_bytes(receipt.request);
        }
        const auto checksum = core::crc32(writer.bytes());
        writer.write_u32(checksum);
        auto encoded = writer.take();
        destination.swap(encoded);
        return self.result(InventoryError::None);
    } catch (const std::bad_alloc&) {
        return self.result(InventoryError::AllocationFailure);
    } catch (const core::BinaryException&) {
        return self.result(InventoryError::Capacity);
    } catch (const std::length_error&) {
        return self.result(InventoryError::Capacity);
    }
}

InventoryResult InventoryService::restore(std::span<const std::byte> bytes) {
    auto& self = *impl_;
    if (self.busy) { return self.result(InventoryError::Busy); }
    if (self.handles.size() != 0 || !self.receipts.empty()) { return self.result(InventoryError::NotEmpty); }
    if (bytes.size() > self.limits.snapshot_bytes || bytes.size() < 41) { return self.result(InventoryError::Corrupt); }
    try {
        core::BinaryLimits bounds;
        bounds.max_total_bytes = self.limits.snapshot_bytes;
        bounds.max_string_bytes = self.limits.name_bytes;
        core::ByteReader checksum_reader(bytes.last(4));
        if (checksum_reader.read_u32() != core::crc32(bytes.first(bytes.size() - 4))) {
            return self.result(InventoryError::Corrupt);
        }
        core::ByteReader reader(bytes.first(bytes.size() - 4), core::ByteOrder::little, bounds);
        reader.expect("GEINV001");
        if (reader.read_u16() != 1) { return self.result(InventoryError::UnsupportedVersion); }
        auto candidate = std::make_unique<Impl>(self.registry, self.limits);
        candidate->revision = reader.read_u64();
        candidate->catalog_revision = reader.read_u64();
        candidate->next_stack = reader.read_u64();
        if (candidate->revision == maximum_counter || candidate->next_stack == 0 ||
            candidate->catalog_revision > candidate->revision || !self.room_for_events(1)) {
            return self.result(InventoryError::Corrupt);
        }
        auto count = [&](std::size_t maximum) {
            const auto value = reader.read_varuint();
            if (value > maximum) {
                throw core::BinaryException(core::BinaryError::limit_exceeded, reader.position(), "inventory count exceeds limit");
            }
            return static_cast<std::size_t>(value);
        };
        const auto definition_count = count(self.limits.definitions);
        std::set<std::string, std::less<>> names;
        for (std::size_t index = 0; index < definition_count; ++index) {
            ItemDefinition value;
            value.id.value = reader.read_u64();
            value.name = reader.read_string();
            value.display_name = reader.read_string();
            value.maximum_stack = reader.read_u32();
            value.unit_weight = reader.read_u64();
            value.tags = reader.read_u64();
            value.equipment_slots = reader.read_u64();
            value.maximum_durability = reader.read_u32();
            value.transferable = reader.read_bool();
            const auto properties = count(self.limits.properties_per_definition);
            for (std::size_t property = 0; property < properties; ++property) {
                auto name = reader.read_string();
                const auto amount = reader.read_f64();
                if (!value.properties.emplace(std::move(name), amount).second) {
                    return self.result(InventoryError::Corrupt);
                }
            }
            if (candidate->definition_valid(value) != InventoryError::None || !names.insert(value.name).second) {
                return self.result(InventoryError::Corrupt);
            }
            const auto id = value.id;
            if (!candidate->definitions.emplace(id, std::move(value)).second) { return self.result(InventoryError::Corrupt); }
        }
        std::set<PersistentId> container_ids;
        std::set<PersistentId> primary_owners;
        std::set<std::uint64_t> stack_sequences;
        const auto container_count = count(self.limits.containers);
        for (std::size_t index = 0; index < container_count; ++index) {
            auto container = std::make_unique<InventoryContainerSnapshot>();
            auto& spec = container->specification;
            spec.id.value = reader.read_u64();
            container->persistent_owner.value = reader.read_u64();
            spec.owner = self.registry.find(container->persistent_owner);
            spec.name = reader.read_string();
            spec.slots = reader.read_u32();
            spec.maximum_weight = reader.read_u64();
            spec.accepted_tags = reader.read_u64();
            spec.rejected_tags = reader.read_u64();
            spec.primary = reader.read_bool();
            container->lock.value = reader.read_u64();
            container->revision = reader.read_u64();
            if (!spec.id || !container_ids.insert(spec.id).second || spec.slots == 0 ||
                spec.slots > self.limits.slots_per_container || !valid_text(spec.name, self.limits.name_bytes) ||
                container->revision == 0 || container->revision > candidate->revision ||
                (container->persistent_owner && !spec.owner) ||
                (spec.owner && spec.primary && !primary_owners.insert(container->persistent_owner).second)) {
                return self.result(InventoryError::Corrupt);
            }
            const auto handle = candidate->handles.acquire();
            if (!handle) { return self.result(InventoryError::Capacity); }
            container->id = {candidate->scope, *handle};
            const auto stacks = count(spec.slots);
            container->stacks.reserve(stacks);
            for (std::size_t stack_index = 0; stack_index < stacks; ++stack_index) {
                InventoryStack stack;
                stack.id = {candidate->scope, reader.read_u64()};
                stack.definition.value = reader.read_u64();
                stack.state.variant = reader.read_u64();
                stack.state.durability = reader.read_u32();
                stack.quantity = reader.read_u32();
                const auto reservations = count(self.limits.reservations_per_stack);
                stack.reservations.reserve(reservations);
                for (std::size_t reservation = 0; reservation < reservations; ++reservation) {
                    const PersistentId token{reader.read_u64()};
                    stack.reservations.push_back({token, reader.read_u32()});
                }
                if (stack.id.sequence >= candidate->next_stack || !stack_sequences.insert(stack.id.sequence).second) {
                    return self.result(InventoryError::Corrupt);
                }
                container->stacks.push_back(std::move(stack));
            }
            if (candidate->validate(*container) != InventoryError::None) { return self.result(InventoryError::Corrupt); }
            candidate->containers[static_cast<std::size_t>(handle->index - 1)] = std::move(container);
        }
        const auto receipts = count(self.limits.receipts);
        std::set<InventoryReceiptId> receipt_ids;
        for (std::size_t index = 0; index < receipts; ++index) {
            Impl::Receipt receipt;
            receipt.id = read_receipt(reader);
            receipt.revision = reader.read_u64();
            auto request = reader.read_sized_subreader();
            if (!receipt.id || !receipt_ids.insert(receipt.id).second || receipt.revision == 0 ||
                receipt.revision > candidate->revision || request.empty() || request.size() > self.limits.receipt_bytes) {
                return self.result(InventoryError::Corrupt);
            }
            const auto data = request.source();
            receipt.request.assign(data.begin(), data.end());
            candidate->receipts.push_back(std::move(receipt));
        }
        reader.require_end();
        candidate->revision = std::max(candidate->revision, self.revision) + 1;
        candidate->tick = self.tick;
        candidate->sequence = self.sequence;
        candidate->signals = self.signals;
        candidate->signal = self.signal;
        candidate->history = self.history;
        candidate->history_begin = self.history_begin;
        impl_.swap(candidate);
        impl_->record(InventoryChangeKind::Restored);
        return impl_->result(InventoryError::None);
    } catch (const std::bad_alloc&) {
        return self.result(InventoryError::AllocationFailure);
    } catch (const core::BinaryException&) {
        return self.result(InventoryError::Corrupt);
    } catch (const std::length_error&) {
        return self.result(InventoryError::Capacity);
    } catch (const std::overflow_error&) {
        return self.result(InventoryError::Exhausted);
    }
}

InventoryResult InventoryService::save(persistence::SaveJournal& journal, std::string_view key) const {
    std::vector<std::byte> bytes;
    const auto encoded = snapshot(bytes);
    if (!encoded) { return encoded; }
    persistence::TransactionId transaction{};
    if (!journal.begin(transaction, "inventory snapshot")) { return impl_->result(InventoryError::PersistenceFailure); }
    const auto staged = journal.put(transaction, key, bytes, inventory_value_type, 1);
    if (!staged) {
        static_cast<void>(journal.abort(transaction));
        return impl_->result(InventoryError::PersistenceFailure);
    }
    if (!journal.commit(transaction)) {
        static_cast<void>(journal.abort(transaction));
        return impl_->result(InventoryError::PersistenceFailure);
    }
    return impl_->result(InventoryError::None);
}

InventoryResult InventoryService::load(const persistence::SaveJournal& journal, std::string_view key) {
    const auto* value = journal.find(key);
    if (!value) { return impl_->result(InventoryError::PersistenceFailure); }
    if (value->type != inventory_value_type || value->version != 1) {
        return impl_->result(InventoryError::UnsupportedVersion);
    }
    return restore(value->bytes);
}

namespace {

core::CommandResult command_result(InventoryResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error)) +
            "; revision=" + std::to_string(result.revision)};
}

bool install_mission_adapter(core::Kernel& kernel, InventoryService& service) {
    auto* missions = kernel.services().find<MissionService>();
    if (!missions) { return false; }
    const auto registered = missions->register_reward_adapter("inventory", [&service](const MissionRewardRequest& request) {
        const auto* item = service.definition(request.reward.item);
        std::uint64_t item_id{};
        if (!item && number(request.reward.item, item_id)) { item = service.definition(PersistentId{item_id}); }
        if (!item) { return MissionRewardResult{MissionRewardOutcome::Retry, "inventory item definition unavailable"}; }
        try {
            core::ByteWriter context(service.limits().receipt_bytes);
            context.write_string(request.mission, service.limits().receipt_bytes);
            context.write_u64(request.run);
            context.write_u32(request.reward_index);
            context.write_string(request.reward.adapter, service.limits().receipt_bytes);
            context.write_string(request.reward.item, service.limits().receipt_bytes);
            const auto result = service.grant(request.actor, request.persistent_actor, item->id,
                request.reward.quantity, {mission_issuer, request.receipt.scope, request.receipt.sequence}, context.bytes());
            auto outcome = MissionRewardOutcome::Retry;
            if (result.error == InventoryError::None) { outcome = MissionRewardOutcome::Applied; }
            if (result.error == InventoryError::AlreadyApplied) { outcome = MissionRewardOutcome::AlreadyApplied; }
            if (result.error == InventoryError::InvalidArgument || result.error == InventoryError::ReceiptConflict) {
                outcome = MissionRewardOutcome::Rejected;
            }
            return MissionRewardResult{outcome, std::string(describe(result.error))};
        } catch (const core::BinaryException&) {
            return MissionRewardResult{MissionRewardOutcome::Rejected, "inventory reward context exceeds bound"};
        } catch (const std::bad_alloc&) {
            return MissionRewardResult{MissionRewardOutcome::Retry, "inventory reward allocation failed"};
        }
    });
    return static_cast<bool>(registered);
}

} // namespace

void register_inventory(core::Kernel& kernel) {
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "gameplay.inventory", "Registry service is required");
        return;
    }
    if (!kernel.register_module({"gameplay.inventory", "Bounded inventory and atomic item transactions", 1})) { return; }
    auto* service = &kernel.services().emplace<InventoryService>(*registry);
    auto adapter_installed = std::make_shared<bool>(install_mission_adapter(kernel, *service));
    if (auto* hub = kernel.services().find<core::SignalHub>()) { service->bind_signals(*hub); }
    static_cast<void>(kernel.add_system({"gameplay.inventory.update", core::Phase::PreSimulation, -4000, {}},
        [service, adapter_installed](core::Kernel& host, const core::Frame& frame) {
            if (!*adapter_installed) { *adapter_installed = install_mission_adapter(host, *service); }
            if (!service->change_signal()) {
                if (auto* hub = host.services().find<core::SignalHub>()) { service->bind_signals(*hub); }
            }
            service->update(frame);
            const auto statistics = service->statistics();
            host.metric("inventory.containers", static_cast<double>(statistics.containers));
            host.metric("inventory.stacks", static_cast<double>(statistics.stacks));
            host.metric("inventory.receipts", static_cast<double>(statistics.receipts));
            host.metric("inventory.rejected", static_cast<double>(statistics.rejected));
        }));
    static_cast<void>(kernel.add_shutdown("gameplay.inventory.shutdown", [host = &kernel, adapter_installed] {
        if (*adapter_installed) {
            if (auto* missions = host->services().find<MissionService>()) {
                static_cast<void>(missions->unregister_reward_adapter("inventory"));
            }
            *adapter_installed = false;
        }
    }));
    static_cast<void>(kernel.add_command("inventory.define", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t id{}, maximum{}, weight{}, tags{};
        if (args.size() < 4 || args.size() > 5 || !number(args[0], id) || !number(args[2], maximum) ||
            !number(args[3], weight) || maximum > std::numeric_limits<std::uint32_t>::max() ||
            (args.size() == 5 && !number(args[4], tags))) {
            return core::CommandResult{false, "inventory.define ID NAME MAX_STACK UNIT_WEIGHT_GRAMS [TAGS]"};
        }
        ItemDefinition definition;
        definition.id = {id}; definition.name = args[1]; definition.display_name = args[1];
        definition.maximum_stack = static_cast<std::uint32_t>(maximum);
        definition.unit_weight = weight; definition.tags = tags;
        return command_result(service->define(std::move(definition)));
    }, "Register an immutable stable item definition"));
    static_cast<void>(kernel.add_command("inventory.create", [service, registry](core::Kernel&, core::CommandArgs args) {
        std::uint64_t id{}, slots{}, weight{};
        if (args.size() < 4 || args.size() > 5 || !number(args[0], id) || !number(args[2], slots) ||
            !number(args[3], weight) || slots > std::numeric_limits<std::uint32_t>::max()) {
            return core::CommandResult{false, "inventory.create ID OWNER_NAME_OR_HANDLE|- SLOTS MAX_WEIGHT_GRAMS [NAME]"};
        }
        InventoryContainerSpec specification;
        specification.id = {id}; specification.slots = static_cast<std::uint32_t>(slots);
        specification.maximum_weight = weight;
        if (args[1] != "-") {
            specification.owner = ecs::parse_entity(args[1]).value_or(registry->find_name(args[1]));
            if (!specification.owner) { return core::CommandResult{false, "owner entity unavailable"}; }
        }
        if (args.size() == 5) { specification.name = args[4]; }
        const auto container = service->create(std::move(specification));
        return core::CommandResult{static_cast<bool>(container), std::string(describe(service->last_error()))};
    }, "Create a primary owner inventory or an unowned container"));
    static_cast<void>(kernel.add_command("inventory.change", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t container{}, definition{}, quantity{}, token{};
        if (args.size() < 4 || args.size() > 5 || !number(args[1], container) ||
            !number(args[2], definition) || !number(args[3], quantity) || (args.size() == 5 && !number(args[4], token))) {
            return core::CommandResult{false, "inventory.change add|remove|reserve|release|consume CONTAINER_ID ITEM_ID QUANTITY [TOKEN]"};
        }
        InventoryOperation operation;
        if (args[0] == "add") { operation.kind = InventoryOperationKind::Add; }
        else if (args[0] == "remove") { operation.kind = InventoryOperationKind::Remove; }
        else if (args[0] == "reserve") { operation.kind = InventoryOperationKind::Reserve; }
        else if (args[0] == "release") { operation.kind = InventoryOperationKind::Release; }
        else if (args[0] == "consume") { operation.kind = InventoryOperationKind::Consume; }
        else { return core::CommandResult{false, "unknown inventory operation"}; }
        operation.source = service->find({container}); operation.definition = {definition};
        operation.quantity = quantity; operation.reservation = {token};
        InventoryTransaction transaction;
        transaction.operations.push_back(operation);
        return command_result(service->transact(transaction));
    }, "Commit a quantity edit or reservation operation"));
    static_cast<void>(kernel.add_command("inventory.transfer", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t source{}, destination{}, definition{}, quantity{};
        if (args.size() != 4 || !number(args[0], source) || !number(args[1], destination) ||
            !number(args[2], definition) || !number(args[3], quantity)) {
            return core::CommandResult{false, "inventory.transfer SOURCE_ID DESTINATION_ID ITEM_ID QUANTITY"};
        }
        InventoryOperation operation;
        operation.kind = InventoryOperationKind::Transfer;
        operation.source = service->find({source}); operation.destination = service->find({destination});
        operation.definition = {definition}; operation.quantity = quantity;
        InventoryTransaction transaction;
        transaction.operations.push_back(operation);
        return command_result(service->transact(transaction));
    }, "Atomically move items between containers"));
    static_cast<void>(kernel.add_command("inventory.inspect", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t id{};
        if (args.size() != 1 || !number(args[0], id)) { return core::CommandResult{false, "inventory.inspect CONTAINER_ID"}; }
        const auto container = service->inspect(service->find({id}));
        if (!container) { return core::CommandResult{false, "container unavailable"}; }
        std::string output = container->specification.name + " revision=" + std::to_string(container->revision) +
            " weight=" + std::to_string(container->weight) + " stacks=" + std::to_string(container->stacks.size());
        for (const auto& stack : container->stacks) {
            output += "\nstack=" + std::to_string(stack.id.sequence) + " item=" + std::to_string(stack.definition.value) +
                      " quantity=" + std::to_string(stack.quantity) + " reserved=" + std::to_string(stack.reserved());
        }
        return core::CommandResult{true, std::move(output)};
    }, "Inspect container totals and unique stacks"));
    for (const bool loading : {false, true}) {
        static_cast<void>(kernel.add_command(loading ? "inventory.load" : "inventory.save",
            [service, loading](core::Kernel& host, core::CommandArgs args) {
                auto* journal = host.services().find<persistence::SaveJournal>();
                if (!journal || args.size() != 1) { return core::CommandResult{false, "open journal and one snapshot key required"}; }
                return command_result(loading ? service->load(*journal, args[0]) : service->save(*journal, args[0]));
            }, loading ? "Restore into an empty inventory service" : "Save contents and retained receipts together"));
    }
}

} // namespace ge::gameplay
