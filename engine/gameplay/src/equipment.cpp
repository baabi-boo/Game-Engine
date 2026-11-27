#include <ge/gameplay/equipment.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/persistence/journal.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace ge::gameplay {
namespace {

constexpr auto maximum_counter = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t equipment_value_type = 0x45515550U;
std::atomic<std::uint64_t> equipment_scope{1};

std::uint64_t acquire_scope() {
    auto value = equipment_scope.load(std::memory_order_relaxed);
    for (;;) {
        if (value == maximum_counter) { throw std::overflow_error("equipment scopes exhausted"); }
        if (equipment_scope.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
}

bool terminal(CraftState state) noexcept {
    return state == CraftState::Completed || state == CraftState::Cancelled || state == CraftState::Invalidated;
}

std::uint32_t held(const InventoryStack& stack, PersistentId token) noexcept {
    for (const auto& reservation : stack.reservations) {
        if (reservation.token == token) { return reservation.quantity; }
    }
    return 0;
}

const InventoryStack* stack_in(const InventoryContainerSnapshot& container, InventoryStackId id) noexcept {
    for (const auto& stack : container.stacks) {
        if (stack.id == id) { return &stack; }
    }
    return nullptr;
}

bool contains_token(const InventoryContainerSnapshot& container, PersistentId token) noexcept {
    return std::any_of(container.stacks.begin(), container.stacks.end(), [token](const auto& stack) {
        return held(stack, token) != 0;
    });
}

EquipmentResult inventory_result(InventoryResult result) noexcept {
    return {result ? EquipmentError::None : EquipmentError::InventoryRejected, result.error};
}

bool number(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) { return false; }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool seconds(std::string_view text, double& value) noexcept {
    if (text.empty()) { return false; }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

void validate_limits(const EquipmentLimits& value) {
    if (value.actors == 0 || value.actors > 65536 || value.recipes == 0 || value.recipes > 65536 ||
        value.jobs == 0 || value.jobs > 65536 || value.ingredients == 0 || value.ingredients > 128 ||
        value.products == 0 || value.products > 128 || value.reserved_stacks == 0 || value.reserved_stacks > 1024 ||
        value.jobs_per_actor == 0 || value.jobs_per_actor > value.jobs || value.job_attempts_per_frame == 0 ||
        value.job_attempts_per_frame > value.jobs || value.history == 0 || value.history > 1048576 ||
        value.name_bytes == 0 || value.name_bytes > 4096 || value.snapshot_bytes < 128 ||
        value.snapshot_bytes > 256U * 1024U * 1024U || !std::isfinite(value.maximum_duration_seconds) ||
        value.maximum_duration_seconds < 0 || value.maximum_duration_seconds > 31536000.0) {
        throw std::invalid_argument("invalid equipment limits");
    }
}

InventoryOperation reservation_operation(InventoryOperationKind kind, InventoryContainerId container,
                                         InventoryStackId stack, PersistentId token, std::uint32_t quantity) {
    InventoryOperation operation;
    operation.kind = kind;
    operation.source = container;
    operation.stack = stack;
    operation.reservation = token;
    operation.quantity = quantity;
    return operation;
}

} // namespace

std::string_view describe(EquipmentError error) noexcept {
    switch (error) {
    case EquipmentError::None: return "ok";
    case EquipmentError::InvalidArgument: return "invalid argument";
    case EquipmentError::InvalidActor: return "actor identity unavailable";
    case EquipmentError::NotBound: return "actor equipment not bound";
    case EquipmentError::Duplicate: return "duplicate identity";
    case EquipmentError::Capacity: return "equipment capacity exceeded";
    case EquipmentError::UnknownRecipe: return "recipe unavailable";
    case EquipmentError::UnknownItem: return "item definition unavailable";
    case EquipmentError::MissingContainer: return "owned container unavailable";
    case EquipmentError::MissingStack: return "stack unavailable";
    case EquipmentError::IneligibleSlot: return "item is ineligible for slot";
    case EquipmentError::SlotOccupied: return "slot occupied";
    case EquipmentError::EmptySlot: return "slot empty";
    case EquipmentError::TokenConflict: return "reservation identity already used";
    case EquipmentError::InventoryRejected: return "inventory transaction rejected";
    case EquipmentError::NotReady: return "craft has not finished its timer";
    case EquipmentError::Invalidated: return "ownership or reservation invalidated";
    case EquipmentError::MissingJob: return "craft job unavailable";
    case EquipmentError::TerminalJob: return "craft job already terminal";
    case EquipmentError::Busy: return "references prevent removal";
    case EquipmentError::Exhausted: return "identity or revision exhausted";
    case EquipmentError::AllocationFailure: return "allocation failed";
    case EquipmentError::ModifierOverflow: return "modifier sum is not finite";
    case EquipmentError::Corrupt: return "invalid equipment snapshot";
    case EquipmentError::UnsupportedVersion: return "unsupported equipment snapshot";
    case EquipmentError::PersistenceFailure: return "journal operation failed";
    case EquipmentError::NotEmpty: return "restore requires empty equipment service";
    case EquipmentError::Stopped: return "equipment service stopped";
    }
    return "unknown equipment error";
}

std::string_view describe(CraftState state) noexcept {
    switch (state) {
    case CraftState::Working: return "working";
    case CraftState::Ready: return "ready";
    case CraftState::Blocked: return "blocked";
    case CraftState::Cancelling: return "cancelling";
    case CraftState::Completed: return "completed";
    case CraftState::Cancelled: return "cancelled";
    case CraftState::Invalidated: return "invalidated";
    }
    return "unknown";
}

double EquipmentModifierSnapshot::additive(std::string_view name) const noexcept {
    const auto found = values.find(name);
    return found == values.end() ? 0.0 : found->second;
}

struct EquipmentService::Impl {
    struct Actor {
        ecs::Entity entity{};
        PersistentId persistent{};
        std::uint64_t slots{};
        std::array<std::optional<EquipmentAttachment>, 64> attachments;
    };
    ecs::Registry& registry;
    InventoryService& inventory;
    EquipmentLimits limits;
    const std::uint64_t scope;
    std::uint64_t next_job{1};
    std::uint64_t revision{};
    std::uint64_t tick{};
    std::uint64_t completed{};
    bool tick_seen{};
    bool stopped{};
    std::size_t job_cursor{};
    std::size_t history_begin{};
    std::size_t history_count{};
    std::vector<std::unique_ptr<Actor>> actors;
    std::map<PersistentId, CraftRecipe> recipes;
    std::vector<std::unique_ptr<CraftJobSnapshot>> jobs;
    std::vector<EquipmentChange> history;

    Impl(ecs::Registry& entities, InventoryService& items, EquipmentLimits bounds)
        : registry(entities), inventory(items), limits(bounds), scope(acquire_scope()) {
        validate_limits(limits);
        actors.resize(limits.actors);
        jobs.resize(limits.jobs);
        history.resize(limits.history);
    }

    bool room() const noexcept { return revision != maximum_counter; }
    bool alive(ecs::Entity actor, PersistentId persistent) const noexcept {
        return persistent && registry.alive(actor) && registry.persistent_id(actor) == persistent;
    }
    bool owned(const InventoryContainerSnapshot& container, ecs::Entity actor, PersistentId persistent) const noexcept {
        return alive(actor, persistent) && container.specification.owner == actor && container.persistent_owner == persistent;
    }
    Actor* find_actor(ecs::Entity entity) const noexcept {
        for (const auto& actor : actors) {
            if (actor && actor->entity == entity) { return actor.get(); }
        }
        return nullptr;
    }
    CraftJobSnapshot* find_job(CraftJobId id) const noexcept {
        if (id.scope != scope || !id) { return nullptr; }
        for (const auto& job : jobs) {
            if (job && job->id == id) { return job.get(); }
        }
        return nullptr;
    }
    const CraftRecipe* find_recipe(PersistentId id) const noexcept {
        const auto found = recipes.find(id);
        return found == recipes.end() ? nullptr : &found->second;
    }
    bool token_used(PersistentId id) const noexcept {
        for (const auto& actor : actors) {
            if (!actor) { continue; }
            for (const auto& attachment : actor->attachments) {
                if (attachment && attachment->reservation == id) { return true; }
            }
        }
        for (const auto& job : jobs) {
            if (job && job->request.id == id) { return true; }
        }
        return false;
    }
    void record(EquipmentChangeKind kind, PersistentId actor = {}, PersistentId subject = {}, std::uint8_t slot = 0) noexcept {
        const EquipmentChange change{++revision, tick, kind, actor, subject, slot};
        if (history_count != history.size()) {
            history[(history_begin + history_count++) % history.size()] = change;
        } else {
            history[history_begin] = change;
            history_begin = (history_begin + 1) % history.size();
        }
    }
    EquipmentError validate_recipe(const CraftRecipe& value) const {
        if (!value.id || value.name.empty() || value.name.size() > limits.name_bytes || !core::valid_utf8(value.name) ||
            value.name.find('\0') != std::string::npos || value.inputs.empty() || value.outputs.empty() ||
            value.inputs.size() > limits.ingredients || value.outputs.size() > limits.products || value.maximum_batch == 0 ||
            !std::isfinite(value.duration_seconds) || value.duration_seconds < 0 ||
            value.duration_seconds > limits.maximum_duration_seconds ||
            value.inputs.size() + value.outputs.size() > inventory.limits().operations_per_transaction) {
            return EquipmentError::InvalidArgument;
        }
        std::set<PersistentId> inputs;
        for (const auto& input : value.inputs) {
            if (!input.definition || input.quantity == 0 || !inputs.insert(input.definition).second) {
                return EquipmentError::InvalidArgument;
            }
            if (!inventory.definition(input.definition)) { return EquipmentError::UnknownItem; }
        }
        for (const auto& output : value.outputs) {
            const auto* definition = inventory.definition(output.definition);
            if (!definition) { return EquipmentError::UnknownItem; }
            if (output.quantity == 0 || (output.state.durability != std::numeric_limits<std::uint32_t>::max() &&
                output.state.durability > definition->maximum_durability)) { return EquipmentError::InvalidArgument; }
        }
        return EquipmentError::None;
    }
    bool attachment_valid(const Actor& actor, const EquipmentAttachment& attachment) const {
        if (attachment.retiring || attachment.slot >= 64 || !alive(actor.entity, actor.persistent)) { return false; }
        const auto container = inventory.inspect(attachment.container);
        if (!container || container->specification.id != attachment.persistent_container ||
            !owned(*container, actor.entity, actor.persistent)) { return false; }
        const auto* stack = stack_in(*container, attachment.stack);
        const auto* definition = inventory.definition(attachment.definition);
        return stack && definition && stack->definition == attachment.definition && stack->state == attachment.item_state &&
            held(*stack, attachment.reservation) >= 1 &&
            ((actor.slots & definition->equipment_slots) & (std::uint64_t{1} << attachment.slot)) != 0;
    }
    EquipmentResult release_attachment(const EquipmentAttachment& attachment) {
        const auto container_id = inventory.find(attachment.persistent_container);
        const auto stack_id = inventory.resolve_stack(attachment.persistent_container, attachment.stack.sequence);
        const auto stack = inventory.stack(container_id, stack_id);
        if (!stack || held(*stack, attachment.reservation) == 0) { return {}; }
        InventoryTransaction transaction;
        transaction.access = attachment.access;
        transaction.operations.push_back(reservation_operation(InventoryOperationKind::Release,
            container_id, stack_id, attachment.reservation, 1));
        return inventory_result(inventory.transact(transaction));
    }
    bool inputs_valid(const CraftJobSnapshot& job, const InventoryContainerSnapshot& source) const noexcept {
        if (!owned(source, job.request.actor, job.persistent_actor) || source.specification.id != job.persistent_source) {
            return false;
        }
        for (const auto& input : job.inputs) {
            const auto* stack = stack_in(source, input.stack);
            if (!stack || stack->definition != input.definition || stack->state != input.state ||
                held(*stack, job.request.id) < input.quantity) { return false; }
        }
        return !job.inputs.empty();
    }
    EquipmentResult release_job(CraftJobSnapshot& job) {
        if (!room()) { return {EquipmentError::Exhausted}; }
        InventoryTransaction transaction;
        transaction.access = job.request.access;
        transaction.operations.reserve(job.inputs.size());
        const auto source_id = inventory.find(job.persistent_source);
        const auto source = inventory.inspect(source_id);
        if (source) {
            for (const auto& input : job.inputs) {
                const auto stack_id = inventory.resolve_stack(job.persistent_source, input.stack.sequence);
                const auto* stack = stack_in(*source, stack_id);
                const auto quantity = stack ? std::min(input.quantity, held(*stack, job.request.id)) : 0U;
                if (quantity != 0) {
                    auto operation = reservation_operation(InventoryOperationKind::Release, source_id,
                        stack_id, job.request.id, quantity);
                    operation.expected_source_revision = source->revision;
                    transaction.operations.push_back(operation);
                }
            }
        }
        const auto result = transaction.operations.empty() ? InventoryResult{} : inventory.transact(transaction);
        job.inventory_error = result.error;
        if (!result) {
            if (job.state != CraftState::Cancelling) {
                job.state = CraftState::Cancelling;
                record(EquipmentChangeKind::CraftCancelling, job.persistent_actor, job.request.id);
            }
            return inventory_result(result);
        }
        job.state = job.invalidating ? CraftState::Invalidated : CraftState::Cancelled;
        job.inputs.clear();
        record(job.invalidating ? EquipmentChangeKind::CraftInvalidated : EquipmentChangeKind::CraftCancelled,
            job.persistent_actor, job.request.id);
        return {};
    }
};

EquipmentService::EquipmentService(ecs::Registry& registry, InventoryService& inventory, EquipmentLimits limits)
    : impl_(std::make_unique<Impl>(registry, inventory, limits)) {}
EquipmentService::~EquipmentService() = default;

EquipmentResult EquipmentService::bind(ecs::Entity actor, std::uint64_t enabled_slots) {
    auto& self = *impl_;
    if (self.stopped) { return {EquipmentError::Stopped}; }
    if (!self.room()) { return {EquipmentError::Exhausted}; }
    const auto persistent = self.registry.persistent_id(actor);
    if (!self.alive(actor, persistent)) { return {EquipmentError::InvalidActor}; }
    if (enabled_slots == 0) { return {EquipmentError::InvalidArgument}; }
    if (self.find_actor(actor)) { return {EquipmentError::Duplicate}; }
    const auto free = std::find(self.actors.begin(), self.actors.end(), nullptr);
    if (free == self.actors.end()) { return {EquipmentError::Capacity}; }
    try {
        auto value = std::make_unique<Impl::Actor>();
        value->entity = actor;
        value->persistent = persistent;
        value->slots = enabled_slots;
        *free = std::move(value);
        self.record(EquipmentChangeKind::Bound, persistent);
        return {};
    } catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
}

EquipmentResult EquipmentService::equip(ecs::Entity entity, std::uint8_t slot, InventoryContainerId container_id,
                                        InventoryStackId stack_id, PersistentId token, PersistentId access) {
    auto& self = *impl_;
    if (self.stopped) { return {EquipmentError::Stopped}; }
    if (!self.room()) { return {EquipmentError::Exhausted}; }
    if (slot >= 64 || !token) { return {EquipmentError::InvalidArgument}; }
    auto* actor = self.find_actor(entity);
    if (!actor) { return {EquipmentError::NotBound}; }
    if (!self.alive(entity, actor->persistent)) { return {EquipmentError::InvalidActor}; }
    if (actor->attachments[slot]) { return {EquipmentError::SlotOccupied}; }
    if (self.token_used(token)) { return {EquipmentError::TokenConflict}; }
    try {
        const auto container = self.inventory.inspect(container_id);
        if (!container || !self.owned(*container, entity, actor->persistent)) { return {EquipmentError::MissingContainer}; }
        if (contains_token(*container, token)) { return {EquipmentError::TokenConflict}; }
        const auto* stack = stack_in(*container, stack_id);
        if (!stack) { return {EquipmentError::MissingStack}; }
        const auto* definition = self.inventory.definition(stack->definition);
        if (!definition) { return {EquipmentError::UnknownItem}; }
        if (((actor->slots & definition->equipment_slots) & (std::uint64_t{1} << slot)) == 0) {
            return {EquipmentError::IneligibleSlot};
        }
        EquipmentAttachment attachment{slot, container_id, container->specification.id, stack_id,
            stack->definition, stack->state, token, access, false};
        InventoryTransaction transaction;
        transaction.access = access;
        transaction.expected_catalog_revision = self.inventory.catalog_revision();
        auto operation = reservation_operation(InventoryOperationKind::Reserve, container_id, stack_id, token, 1);
        operation.expected_source_revision = container->revision;
        transaction.operations.push_back(operation);
        const auto result = self.inventory.transact(transaction);
        if (!result) { return inventory_result(result); }
        actor->attachments[slot] = attachment;
        self.record(EquipmentChangeKind::Equipped, actor->persistent, token, slot);
        return {};
    } catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
}

EquipmentResult EquipmentService::unequip(ecs::Entity entity, std::uint8_t slot) {
    auto& self = *impl_;
    if (slot >= 64) { return {EquipmentError::InvalidArgument}; }
    if (!self.room()) { return {EquipmentError::Exhausted}; }
    auto* actor = self.find_actor(entity);
    if (!actor) { return {EquipmentError::NotBound}; }
    auto& attachment = actor->attachments[slot];
    if (!attachment) { return {EquipmentError::EmptySlot}; }
    try {
        const auto result = self.release_attachment(*attachment);
        if (!result) { return result; }
        const auto token = attachment->reservation;
        attachment.reset();
        self.record(EquipmentChangeKind::Unequipped, actor->persistent, token, slot);
        return {};
    } catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
}

EquipmentResult EquipmentService::unbind(ecs::Entity entity) {
    auto& self = *impl_;
    auto* actor = self.find_actor(entity);
    if (!actor) { return {EquipmentError::NotBound}; }
    EquipmentResult outcome;
    for (std::size_t slot = 0; slot < actor->attachments.size(); ++slot) {
        if (!actor->attachments[slot]) { continue; }
        const auto released = unequip(entity, static_cast<std::uint8_t>(slot));
        if (!released) { outcome = released; }
    }
    if (!outcome) { return outcome; }
    if (!self.room()) { return {EquipmentError::Exhausted}; }
    const auto persistent = actor->persistent;
    for (auto& value : self.actors) {
        if (value.get() == actor) { value.reset(); break; }
    }
    self.record(EquipmentChangeKind::Unbound, persistent);
    return {};
}

std::optional<EquipmentActorSnapshot> EquipmentService::inspect(ecs::Entity entity) const {
    const auto* actor = impl_->find_actor(entity);
    if (!actor) { return std::nullopt; }
    EquipmentActorSnapshot result{entity, actor->persistent, actor->slots, {}};
    result.attachments.reserve(actor->attachments.size());
    for (const auto& attachment : actor->attachments) {
        if (attachment) {
            result.attachments.push_back(*attachment);
            result.attachments.back().retiring = !impl_->attachment_valid(*actor, *attachment);
        }
    }
    return result;
}

EquipmentModifierSnapshot EquipmentService::modifiers(ecs::Entity entity) const {
    EquipmentModifierSnapshot result;
    result.revision = impl_->revision;
    if (impl_->stopped) { result.error = EquipmentError::Stopped; return result; }
    const auto* actor = impl_->find_actor(entity);
    if (!actor) { result.error = EquipmentError::NotBound; return result; }
    if (!impl_->alive(entity, actor->persistent)) { result.error = EquipmentError::InvalidActor; return result; }
    try {
        for (const auto& attachment : actor->attachments) {
            if (!attachment || !impl_->attachment_valid(*actor, *attachment)) { continue; }
            const auto* definition = impl_->inventory.definition(attachment->definition);
            if (!definition || (definition->maximum_durability != 0 && attachment->item_state.durability == 0)) { continue; }
            ++result.equipped;
            for (const auto& [name, value] : definition->properties) {
                constexpr std::string_view prefix = "equip.";
                if (!name.starts_with(prefix) || name.size() == prefix.size()) { continue; }
                auto& total = result.values[name.substr(prefix.size())];
                const auto sum = total + value;
                if (!std::isfinite(value) || !std::isfinite(sum)) {
                    result.error = EquipmentError::ModifierOverflow;
                    result.values.clear();
                    result.equipped = 0;
                    return result;
                }
                total = sum;
            }
        }
    } catch (const std::bad_alloc&) {
        result.error = EquipmentError::AllocationFailure;
        result.values.clear();
        result.equipped = 0;
    }
    return result;
}

EquipmentResult EquipmentService::define_recipe(CraftRecipe value) {
    auto& self = *impl_;
    if (self.stopped) { return {EquipmentError::Stopped}; }
    if (!self.room()) { return {EquipmentError::Exhausted}; }
    if (self.recipes.size() >= self.limits.recipes) { return {EquipmentError::Capacity}; }
    try {
        const auto error = self.validate_recipe(value);
        if (error != EquipmentError::None) { return {error}; }
        for (const auto& [id, existing] : self.recipes) {
            if (id == value.id || existing.name == value.name) { return {EquipmentError::Duplicate}; }
        }
        const auto id = value.id;
        self.recipes.emplace(id, std::move(value));
        self.record(EquipmentChangeKind::RecipeDefined, {}, id);
        return {};
    } catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
}

EquipmentResult EquipmentService::remove_recipe(PersistentId id) {
    auto& self = *impl_;
    if (!self.room()) { return {EquipmentError::Exhausted}; }
    if (!self.find_recipe(id)) { return {EquipmentError::UnknownRecipe}; }
    for (const auto& job : self.jobs) {
        if (job && job->request.recipe == id) { return {EquipmentError::Busy}; }
    }
    self.recipes.erase(id);
    self.record(EquipmentChangeKind::RecipeRemoved, {}, id);
    return {};
}

const CraftRecipe* EquipmentService::recipe(PersistentId id) const noexcept { return impl_->find_recipe(id); }

std::vector<CraftRecipe> EquipmentService::recipes() const {
    std::vector<CraftRecipe> result;
    result.reserve(impl_->recipes.size());
    for (const auto& [id, value] : impl_->recipes) {
        static_cast<void>(id);
        result.push_back(value);
    }
    return result;
}

CraftAdmission EquipmentService::start(const CraftRequest& request) {
    auto& self = *impl_;
    if (self.stopped) { return {{EquipmentError::Stopped}, {}}; }
    if (!self.room() || self.next_job == maximum_counter) { return {{EquipmentError::Exhausted}, {}}; }
    const auto persistent = self.registry.persistent_id(request.actor);
    if (!self.alive(request.actor, persistent)) { return {{EquipmentError::InvalidActor}, {}}; }
    const auto* recipe = self.find_recipe(request.recipe);
    if (!recipe) { return {{EquipmentError::UnknownRecipe}, {}}; }
    if (!request.id || request.batches == 0 || request.batches > recipe->maximum_batch) {
        return {{EquipmentError::InvalidArgument}, {}};
    }
    if (self.token_used(request.id)) { return {{EquipmentError::TokenConflict}, {}}; }
    const auto free = std::find(self.jobs.begin(), self.jobs.end(), nullptr);
    std::size_t actor_jobs{};
    for (const auto& job : self.jobs) {
        if (job && job->request.actor == request.actor && !terminal(job->state)) { ++actor_jobs; }
    }
    if (free == self.jobs.end() || actor_jobs >= self.limits.jobs_per_actor) { return {{EquipmentError::Capacity}, {}}; }
    try {
        const auto recipe_error = self.validate_recipe(*recipe);
        if (recipe_error != EquipmentError::None) { return {{recipe_error}, {}}; }
        auto candidate = std::make_unique<CraftJobSnapshot>();
        candidate->id = {self.scope, self.next_job};
        candidate->request = request;
        if (!candidate->request.source) { candidate->request.source = self.inventory.primary(request.actor); }
        if (!candidate->request.destination) { candidate->request.destination = candidate->request.source; }
        const auto source = self.inventory.inspect(candidate->request.source);
        const auto destination = self.inventory.inspect(candidate->request.destination);
        if (!source || !destination || !self.owned(*source, request.actor, persistent) ||
            !self.owned(*destination, request.actor, persistent)) { return {{EquipmentError::MissingContainer}, {}}; }
        if (contains_token(*source, request.id) || contains_token(*destination, request.id)) {
            return {{EquipmentError::TokenConflict}, {}};
        }
        candidate->persistent_actor = persistent;
        candidate->persistent_source = source->specification.id;
        candidate->persistent_destination = destination->specification.id;
        candidate->remaining_seconds = recipe->duration_seconds;
        candidate->state = recipe->duration_seconds == 0 ? CraftState::Ready : CraftState::Working;
        candidate->inputs.reserve(self.limits.reserved_stacks);
        InventoryTransaction transaction;
        transaction.access = request.access;
        transaction.expected_catalog_revision = self.inventory.catalog_revision();
        transaction.operations.reserve(self.limits.reserved_stacks);
        for (const auto& input : recipe->inputs) {
            auto remaining = static_cast<std::uint64_t>(input.quantity) * request.batches;
            for (const auto& stack : source->stacks) {
                if (remaining == 0 || stack.definition != input.definition || stack.available() == 0) { continue; }
                if (candidate->inputs.size() >= self.limits.reserved_stacks ||
                    candidate->inputs.size() + 1 + recipe->outputs.size() > self.inventory.limits().operations_per_transaction) {
                    return {{EquipmentError::Capacity}, {}};
                }
                const auto amount = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, stack.available()));
                candidate->inputs.push_back({stack.id, stack.definition, stack.state, amount});
                auto operation = reservation_operation(InventoryOperationKind::Reserve, source->id, stack.id, request.id, amount);
                operation.expected_source_revision = source->revision;
                transaction.operations.push_back(operation);
                remaining -= amount;
            }
            if (remaining != 0) { return {{EquipmentError::InventoryRejected, InventoryError::Insufficient}, {}}; }
        }
        const auto committed = self.inventory.transact(transaction);
        if (!committed) { return {inventory_result(committed), {}}; }
        const auto id = candidate->id;
        *free = std::move(candidate);
        ++self.next_job;
        self.record(EquipmentChangeKind::CraftStarted, persistent, request.id);
        return {{}, id};
    } catch (const std::bad_alloc&) { return {{EquipmentError::AllocationFailure}, {}}; }
}

EquipmentResult EquipmentService::complete(CraftJobId id) {
    auto& self = *impl_;
    if (self.stopped) { return {EquipmentError::Stopped}; }
    auto* job = self.find_job(id);
    if (!job) { return {EquipmentError::MissingJob}; }
    if (job->state == CraftState::Completed) { return {}; }
    if (terminal(job->state) || job->state == CraftState::Cancelling) { return {EquipmentError::TerminalJob}; }
    if (job->remaining_seconds != 0) { return {EquipmentError::NotReady}; }
    if (!self.room()) { return {EquipmentError::Exhausted}; }
    try {
        if (job->invalidating) {
            const auto released = self.release_job(*job);
            return {EquipmentError::Invalidated, released.inventory_error};
        }
        const auto source = self.inventory.inspect(job->request.source);
        const auto destination = self.inventory.inspect(job->request.destination);
        const auto* recipe = self.find_recipe(job->request.recipe);
        if (!recipe || !source || !destination || !self.inputs_valid(*job, *source) ||
            !self.owned(*destination, job->request.actor, job->persistent_actor) ||
            destination->specification.id != job->persistent_destination) {
            job->invalidating = true;
            const auto released = self.release_job(*job);
            return {EquipmentError::Invalidated, released.inventory_error};
        }
        InventoryTransaction transaction;
        transaction.access = job->request.access;
        transaction.expected_catalog_revision = self.inventory.catalog_revision();
        transaction.operations.reserve(job->inputs.size() + recipe->outputs.size());
        for (const auto& input : job->inputs) {
            auto operation = reservation_operation(InventoryOperationKind::Consume, source->id,
                input.stack, job->request.id, input.quantity);
            operation.expected_source_revision = source->revision;
            transaction.operations.push_back(operation);
        }
        for (const auto& product : recipe->outputs) {
            InventoryOperation operation;
            operation.kind = InventoryOperationKind::Add;
            operation.source = destination->id;
            operation.definition = product.definition;
            operation.state = product.state;
            operation.quantity = static_cast<std::uint64_t>(product.quantity) * job->request.batches;
            operation.expected_source_revision = destination->revision;
            transaction.operations.push_back(operation);
        }
        const auto committed = self.inventory.transact(transaction);
        job->inventory_error = committed.error;
        if (!committed) {
            if (job->state != CraftState::Blocked) {
                job->state = CraftState::Blocked;
                self.record(EquipmentChangeKind::CraftBlocked, job->persistent_actor, job->request.id);
            }
            return inventory_result(committed);
        }
        job->state = CraftState::Completed;
        job->inputs.clear();
        if (self.completed != maximum_counter) { ++self.completed; }
        self.record(EquipmentChangeKind::CraftCompleted, job->persistent_actor, job->request.id);
        return {};
    } catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
}

EquipmentResult EquipmentService::cancel(CraftJobId id) {
    auto* job = impl_->find_job(id);
    if (!job) { return {EquipmentError::MissingJob}; }
    if (job->state == CraftState::Completed) { return {EquipmentError::TerminalJob}; }
    if (terminal(job->state)) { return {}; }
    try { return impl_->release_job(*job); }
    catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
}

EquipmentResult EquipmentService::forget(CraftJobId id) {
    if (!impl_->room()) { return {EquipmentError::Exhausted}; }
    for (auto& job : impl_->jobs) {
        if (!job || job->id != id) { continue; }
        if (!terminal(job->state)) { return {EquipmentError::Busy}; }
        impl_->record(EquipmentChangeKind::CraftForgotten, job->persistent_actor, job->request.id);
        job.reset();
        return {};
    }
    return {EquipmentError::MissingJob};
}

std::optional<CraftJobSnapshot> EquipmentService::job(CraftJobId id) const {
    const auto* value = impl_->find_job(id);
    return value ? std::optional<CraftJobSnapshot>(*value) : std::nullopt;
}

CraftJobId EquipmentService::find_job(PersistentId id) const noexcept {
    for (const auto& value : impl_->jobs) {
        if (value && value->request.id == id) { return value->id; }
    }
    return {};
}

std::vector<CraftJobId> EquipmentService::jobs(ecs::Entity actor) const {
    std::vector<CraftJobId> result;
    result.reserve(impl_->jobs.size());
    for (const auto& value : impl_->jobs) {
        if (value && (!actor || value->request.actor == actor)) { result.push_back(value->id); }
    }
    return result;
}

EquipmentHistory EquipmentService::changes_since(std::uint64_t cursor, std::size_t limit) const {
    const auto& self = *impl_;
    EquipmentHistory result;
    result.next_cursor = cursor;
    result.newest_sequence = self.revision;
    const auto oldest = self.history_count == 0 ? self.revision : self.history[self.history_begin].sequence - 1;
    result.history_lost = cursor < oldest || cursor > self.revision;
    if (cursor > self.revision) { result.next_cursor = self.revision; return result; }
    result.changes.reserve(std::min(limit, self.history_count));
    for (std::size_t index = 0; index < self.history_count && result.changes.size() < limit; ++index) {
        const auto& change = self.history[(self.history_begin + index) % self.history.size()];
        if (change.sequence > cursor) { result.changes.push_back(change); result.next_cursor = change.sequence; }
    }
    if (self.history_count == 0) { result.next_cursor = self.revision; }
    return result;
}

void EquipmentService::update(const core::Frame& frame) {
    auto& self = *impl_;
    if (self.stopped || !std::isfinite(frame.delta_seconds) || frame.delta_seconds < 0 ||
        (self.tick_seen && frame.tick <= self.tick)) { return; }
    self.tick_seen = true;
    self.tick = frame.tick;
    try {
        for (auto& actor : self.actors) {
            if (!actor) { continue; }
            for (auto& attachment : actor->attachments) {
                if (!attachment || !self.room()) { continue; }
                if (!self.attachment_valid(*actor, *attachment) && !attachment->retiring) {
                    attachment->retiring = true;
                    self.record(EquipmentChangeKind::AttachmentInvalidated, actor->persistent,
                        attachment->reservation, attachment->slot);
                }
                if (attachment->retiring && self.room()) { static_cast<void>(unequip(actor->entity, attachment->slot)); }
            }
            if (!self.alive(actor->entity, actor->persistent) && self.room() &&
                std::none_of(actor->attachments.begin(), actor->attachments.end(), [](const auto& value) { return value.has_value(); })) {
                self.record(EquipmentChangeKind::Unbound, actor->persistent);
                actor.reset();
            }
        }
        for (auto& job : self.jobs) {
            if (!job || terminal(job->state)) { continue; }
            if (job->state == CraftState::Working) {
                job->remaining_seconds -= std::min(job->remaining_seconds, frame.delta_seconds);
                if (job->remaining_seconds == 0 && self.room()) {
                    job->state = CraftState::Ready;
                    self.record(EquipmentChangeKind::CraftReady, job->persistent_actor, job->request.id);
                }
            }
        }
        std::size_t attempts{};
        for (std::size_t visited = 0; visited < self.jobs.size() && attempts < self.limits.job_attempts_per_frame; ++visited) {
            const auto index = self.job_cursor;
            self.job_cursor = (self.job_cursor + 1) % self.jobs.size();
            auto* job = self.jobs[index].get();
            if (!job || terminal(job->state)) { continue; }
            const auto source = self.inventory.inspect(job->request.source);
            const auto destination = self.inventory.inspect(job->request.destination);
            if (!source || !destination || !self.inputs_valid(*job, *source) ||
                !self.owned(*destination, job->request.actor, job->persistent_actor)) { job->invalidating = true; }
            if (job->invalidating || job->state == CraftState::Cancelling) {
                ++attempts;
                static_cast<void>(cancel(job->id));
            } else if (job->state == CraftState::Ready) {
                ++attempts;
                static_cast<void>(complete(job->id));
            }
        }
    } catch (const std::bad_alloc&) {
        // Retain every ownership record so a later frame can retry cleanup.
    }
}

void EquipmentService::shutdown() {
    auto& self = *impl_;
    self.stopped = true;
    for (const auto& actor : self.actors) {
        if (actor) { static_cast<void>(unbind(actor->entity)); }
    }
    for (const auto& job : self.jobs) {
        if (job && !terminal(job->state)) { static_cast<void>(cancel(job->id)); }
    }
}

EquipmentStatistics EquipmentService::statistics() const noexcept {
    EquipmentStatistics result;
    result.recipes = impl_->recipes.size();
    result.revision = impl_->revision;
    result.completed_jobs = impl_->completed;
    for (const auto& actor : impl_->actors) {
        if (!actor) { continue; }
        ++result.actors;
        for (const auto& attachment : actor->attachments) {
            if (attachment) { ++result.attachments; result.retiring_attachments += static_cast<std::size_t>(attachment->retiring); }
        }
    }
    for (const auto& job : impl_->jobs) {
        if (!job) { continue; }
        ++result.jobs;
        result.active_jobs += static_cast<std::size_t>(!terminal(job->state));
        result.blocked_jobs += static_cast<std::size_t>(job->state == CraftState::Blocked || job->state == CraftState::Cancelling);
    }
    return result;
}

const EquipmentLimits& EquipmentService::limits() const noexcept { return impl_->limits; }

EquipmentResult EquipmentService::snapshot(std::vector<std::byte>& destination) const {
    const auto& self = *impl_;
    try {
        core::ByteWriter writer(self.limits.snapshot_bytes);
        writer.write_bytes("GEQUP001");
        writer.write_u16(1);
        writer.write_u64(self.revision);
        writer.write_u64(self.completed);
        writer.write_varuint(self.recipes.size());
        for (const auto& [id, recipe] : self.recipes) {
            const auto error = self.validate_recipe(recipe);
            if (error != EquipmentError::None) { return {error}; }
            writer.write_u64(id.value);
            writer.write_string(recipe.name, self.limits.name_bytes);
            writer.write_f64(recipe.duration_seconds);
            writer.write_u32(recipe.maximum_batch);
            writer.write_varuint(recipe.inputs.size());
            for (const auto& input : recipe.inputs) {
                writer.write_u64(input.definition.value);
                writer.write_u32(input.quantity);
            }
            writer.write_varuint(recipe.outputs.size());
            for (const auto& output : recipe.outputs) {
                writer.write_u64(output.definition.value);
                writer.write_u32(output.quantity);
                writer.write_u64(output.state.variant);
                writer.write_u32(output.state.durability);
            }
        }
        const auto totals = statistics();
        writer.write_varuint(totals.actors);
        for (const auto& actor : self.actors) {
            if (!actor) { continue; }
            if (!self.alive(actor->entity, actor->persistent)) { return {EquipmentError::InvalidActor}; }
            writer.write_u64(actor->persistent.value);
            writer.write_u64(actor->slots);
            const auto count = std::count_if(actor->attachments.begin(), actor->attachments.end(),
                [](const auto& value) { return value.has_value(); });
            writer.write_varuint(static_cast<std::uint64_t>(count));
            for (const auto& attachment : actor->attachments) {
                if (!attachment) { continue; }
                writer.write_u8(attachment->slot);
                writer.write_u64(attachment->persistent_container.value);
                writer.write_u64(attachment->stack.sequence);
                writer.write_u64(attachment->definition.value);
                writer.write_u64(attachment->item_state.variant);
                writer.write_u32(attachment->item_state.durability);
                writer.write_u64(attachment->reservation.value);
                writer.write_u64(attachment->access.value);
                writer.write_bool(attachment->retiring || !self.attachment_valid(*actor, *attachment));
            }
        }
        writer.write_varuint(totals.jobs);
        for (const auto& job : self.jobs) {
            if (!job) { continue; }
            if (!terminal(job->state) && job->state != CraftState::Cancelling) {
                const auto source = self.inventory.inspect(job->request.source);
                const auto output = self.inventory.inspect(job->request.destination);
                if (!source || !output || !self.inputs_valid(*job, *source) ||
                    !self.owned(*output, job->request.actor, job->persistent_actor)) {
                    return {EquipmentError::Invalidated};
                }
            }
            writer.write_u64(job->request.id.value);
            writer.write_u64(job->persistent_actor.value);
            writer.write_u64(job->request.recipe.value);
            writer.write_u64(job->persistent_source.value);
            writer.write_u64(job->persistent_destination.value);
            writer.write_u32(job->request.batches);
            writer.write_u64(job->request.access.value);
            writer.write_u8(static_cast<std::uint8_t>(job->state));
            writer.write_f64(job->remaining_seconds);
            writer.write_u8(static_cast<std::uint8_t>(job->inventory_error));
            writer.write_bool(job->invalidating);
            writer.write_varuint(job->inputs.size());
            for (const auto& input : job->inputs) {
                writer.write_u64(input.stack.sequence);
                writer.write_u64(input.definition.value);
                writer.write_u64(input.state.variant);
                writer.write_u32(input.state.durability);
                writer.write_u32(input.quantity);
            }
        }
        writer.write_u32(core::crc32(writer.bytes()));
        auto encoded = writer.take();
        destination.swap(encoded);
        return {};
    } catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
    catch (const core::BinaryException&) { return {EquipmentError::Capacity}; }
    catch (const std::length_error&) { return {EquipmentError::Capacity}; }
}

EquipmentResult EquipmentService::restore(std::span<const std::byte> bytes) {
    auto& self = *impl_;
    const auto totals = statistics();
    if (self.stopped) { return {EquipmentError::Stopped}; }
    if (totals.actors != 0 || totals.jobs != 0 || totals.recipes != 0) { return {EquipmentError::NotEmpty}; }
    if (bytes.size() < 33 || bytes.size() > self.limits.snapshot_bytes) { return {EquipmentError::Corrupt}; }
    try {
        core::ByteReader checksum(bytes.last(4));
        if (checksum.read_u32() != core::crc32(bytes.first(bytes.size() - 4))) { return {EquipmentError::Corrupt}; }
        core::BinaryLimits limits;
        limits.max_total_bytes = self.limits.snapshot_bytes;
        limits.max_string_bytes = self.limits.name_bytes;
        core::ByteReader reader(bytes.first(bytes.size() - 4), core::ByteOrder::little, limits);
        reader.expect("GEQUP001");
        if (reader.read_u16() != 1) { return {EquipmentError::UnsupportedVersion}; }
        auto candidate = std::make_unique<Impl>(self.registry, self.inventory, self.limits);
        candidate->revision = std::max(reader.read_u64(), self.revision);
        candidate->completed = reader.read_u64();
        if (!candidate->room()) { return {EquipmentError::Exhausted}; }
        const auto count = [&](std::size_t maximum) {
            const auto value = reader.read_varuint();
            if (value > maximum) {
                throw core::BinaryException(core::BinaryError::limit_exceeded, reader.position(), "equipment count exceeds bound");
            }
            return static_cast<std::size_t>(value);
        };
        std::set<std::string, std::less<>> names;
        const auto recipes = count(self.limits.recipes);
        for (std::size_t index = 0; index < recipes; ++index) {
            CraftRecipe recipe;
            recipe.id.value = reader.read_u64();
            recipe.name = reader.read_string();
            recipe.duration_seconds = reader.read_f64();
            recipe.maximum_batch = reader.read_u32();
            recipe.inputs.resize(count(self.limits.ingredients));
            for (auto& input : recipe.inputs) {
                input.definition.value = reader.read_u64();
                input.quantity = reader.read_u32();
            }
            recipe.outputs.resize(count(self.limits.products));
            for (auto& output : recipe.outputs) {
                output.definition.value = reader.read_u64();
                output.quantity = reader.read_u32();
                output.state.variant = reader.read_u64();
                output.state.durability = reader.read_u32();
            }
            if (candidate->validate_recipe(recipe) != EquipmentError::None || !names.insert(recipe.name).second) {
                return {EquipmentError::Corrupt};
            }
            const auto id = recipe.id;
            if (!candidate->recipes.emplace(id, std::move(recipe)).second) { return {EquipmentError::Corrupt}; }
        }
        std::set<PersistentId> tokens;
        std::set<PersistentId> actors;
        const auto actor_count = count(self.limits.actors);
        for (std::size_t index = 0; index < actor_count; ++index) {
            auto actor = std::make_unique<Impl::Actor>();
            actor->persistent.value = reader.read_u64();
            actor->entity = self.registry.find(actor->persistent);
            actor->slots = reader.read_u64();
            if (!candidate->alive(actor->entity, actor->persistent) || actor->slots == 0 ||
                !actors.insert(actor->persistent).second) { return {EquipmentError::Corrupt}; }
            const auto attachments = count(64);
            for (std::size_t attachment_index = 0; attachment_index < attachments; ++attachment_index) {
                EquipmentAttachment value;
                value.slot = reader.read_u8();
                value.persistent_container.value = reader.read_u64();
                const auto sequence = reader.read_u64();
                value.container = self.inventory.find(value.persistent_container);
                value.stack = self.inventory.resolve_stack(value.persistent_container, sequence);
                value.definition.value = reader.read_u64();
                value.item_state.variant = reader.read_u64();
                value.item_state.durability = reader.read_u32();
                value.reservation.value = reader.read_u64();
                value.access.value = reader.read_u64();
                value.retiring = reader.read_bool();
                if (value.slot >= 64 || actor->attachments[value.slot] || !value.persistent_container || sequence == 0 ||
                    !value.definition || !value.reservation || !tokens.insert(value.reservation).second) {
                    return {EquipmentError::Corrupt};
                }
                if (!value.retiring && !candidate->attachment_valid(*actor, value)) { return {EquipmentError::Invalidated}; }
                if (!value.stack) { value.stack.sequence = sequence; }
                actor->attachments[value.slot] = value;
            }
            candidate->actors[index] = std::move(actor);
        }
        std::map<PersistentId, std::size_t> active_counts;
        const auto job_count = count(self.limits.jobs);
        for (std::size_t index = 0; index < job_count; ++index) {
            auto job = std::make_unique<CraftJobSnapshot>();
            job->id = {candidate->scope, candidate->next_job++};
            job->request.id.value = reader.read_u64();
            job->persistent_actor.value = reader.read_u64();
            job->request.actor = self.registry.find(job->persistent_actor);
            job->request.recipe.value = reader.read_u64();
            job->persistent_source.value = reader.read_u64();
            job->persistent_destination.value = reader.read_u64();
            job->request.source = self.inventory.find(job->persistent_source);
            job->request.destination = self.inventory.find(job->persistent_destination);
            job->request.batches = reader.read_u32();
            job->request.access.value = reader.read_u64();
            const auto state = reader.read_u8();
            job->remaining_seconds = reader.read_f64();
            const auto inventory_error = reader.read_u8();
            job->invalidating = reader.read_bool();
            const auto* recipe = candidate->find_recipe(job->request.recipe);
            if (!recipe || !job->request.id || !job->persistent_actor || !job->persistent_source || !job->persistent_destination ||
                !tokens.insert(job->request.id).second || state > static_cast<std::uint8_t>(CraftState::Invalidated) ||
                inventory_error > static_cast<std::uint8_t>(InventoryError::NotEmpty) || job->request.batches == 0 ||
                job->request.batches > recipe->maximum_batch || job->remaining_seconds < 0 ||
                job->remaining_seconds > recipe->duration_seconds) { return {EquipmentError::Corrupt}; }
            job->state = static_cast<CraftState>(state);
            job->inventory_error = static_cast<InventoryError>(inventory_error);
            if ((job->state == CraftState::Ready || job->state == CraftState::Blocked || job->state == CraftState::Completed) &&
                job->remaining_seconds != 0) { return {EquipmentError::Corrupt}; }
            job->inputs.resize(count(self.limits.reserved_stacks));
            std::set<std::uint64_t> sequences;
            std::map<PersistentId, std::uint64_t> quantities;
            for (auto& input : job->inputs) {
                const auto sequence = reader.read_u64();
                input.stack = self.inventory.resolve_stack(job->persistent_source, sequence);
                input.definition.value = reader.read_u64();
                input.state.variant = reader.read_u64();
                input.state.durability = reader.read_u32();
                input.quantity = reader.read_u32();
                if (sequence == 0 || !sequences.insert(sequence).second || !input.definition || input.quantity == 0) {
                    return {EquipmentError::Corrupt};
                }
                if (!input.stack) { input.stack.sequence = sequence; }
                quantities[input.definition] += input.quantity;
            }
            if (terminal(job->state)) {
                if (!job->inputs.empty()) { return {EquipmentError::Corrupt}; }
            } else {
                if (++active_counts[job->persistent_actor] > self.limits.jobs_per_actor ||
                    job->inputs.size() + recipe->outputs.size() > self.inventory.limits().operations_per_transaction ||
                    quantities.size() != recipe->inputs.size()) { return {EquipmentError::Corrupt}; }
                for (const auto& input : recipe->inputs) {
                    if (quantities[input.definition] != static_cast<std::uint64_t>(input.quantity) * job->request.batches) {
                        return {EquipmentError::Corrupt};
                    }
                }
                if (job->state != CraftState::Cancelling) {
                    const auto source = self.inventory.inspect(job->request.source);
                    const auto destination = self.inventory.inspect(job->request.destination);
                    if (!source || !destination || !candidate->inputs_valid(*job, *source) ||
                        !candidate->owned(*destination, job->request.actor, job->persistent_actor)) {
                        return {EquipmentError::Invalidated};
                    }
                }
            }
            candidate->jobs[index] = std::move(job);
        }
        reader.require_end();
        candidate->tick = self.tick;
        candidate->tick_seen = self.tick_seen;
        candidate->record(EquipmentChangeKind::Restored);
        impl_.swap(candidate);
        return {};
    } catch (const std::bad_alloc&) { return {EquipmentError::AllocationFailure}; }
    catch (const core::BinaryException&) { return {EquipmentError::Corrupt}; }
    catch (const std::length_error&) { return {EquipmentError::Corrupt}; }
    catch (const std::overflow_error&) { return {EquipmentError::Exhausted}; }
}

EquipmentResult EquipmentService::save(persistence::SaveJournal& journal, std::string_view key) const {
    std::vector<std::byte> bytes;
    const auto encoded = snapshot(bytes);
    if (!encoded) { return encoded; }
    persistence::TransactionId transaction{};
    if (!journal.begin(transaction, "equipment snapshot")) { return {EquipmentError::PersistenceFailure}; }
    if (!journal.put(transaction, key, bytes, equipment_value_type, 1) || !journal.commit(transaction)) {
        static_cast<void>(journal.abort(transaction));
        return {EquipmentError::PersistenceFailure};
    }
    return {};
}

EquipmentResult EquipmentService::load(const persistence::SaveJournal& journal, std::string_view key) {
    const auto* value = journal.find(key);
    if (!value) { return {EquipmentError::PersistenceFailure}; }
    if (value->type != equipment_value_type || value->version != 1) { return {EquipmentError::UnsupportedVersion}; }
    return restore(value->bytes);
}

namespace {

core::CommandResult command_result(EquipmentResult result) {
    std::string message(describe(result.error));
    if (result.inventory_error != InventoryError::None) {
        message += ": ";
        message += describe(result.inventory_error);
    }
    return {static_cast<bool>(result), std::move(message)};
}

ecs::Entity resolve_actor(ecs::Registry& registry, std::string_view text) {
    return ecs::parse_entity(text).value_or(registry.find_name(text));
}

} // namespace

void register_equipment(core::Kernel& kernel) {
    auto* registry = kernel.services().find<ecs::Registry>();
    auto* inventory = kernel.services().find<InventoryService>();
    if (!registry || !inventory) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "gameplay.equipment", "Registry and InventoryService are required");
        return;
    }
    if (!kernel.register_module({"gameplay.equipment", "Reserved equipment and transactional crafting", 1})) { return; }
    auto* service = &kernel.services().emplace<EquipmentService>(*registry, *inventory);
    static_cast<void>(kernel.add_system({"gameplay.equipment.update", core::Phase::PreSimulation, -3900,
        {"gameplay.inventory.update"}}, [service](core::Kernel& host, const core::Frame& frame) {
        service->update(frame);
        const auto totals = service->statistics();
        host.metric("equipment.attachments", static_cast<double>(totals.attachments));
        host.metric("equipment.craft.active", static_cast<double>(totals.active_jobs));
        host.metric("equipment.craft.blocked", static_cast<double>(totals.blocked_jobs));
        host.metric("equipment.craft.completed", static_cast<double>(totals.completed_jobs));
    }));
    static_cast<void>(kernel.add_shutdown("gameplay.equipment.shutdown", [service, host = &kernel] {
        service->shutdown();
        const auto totals = service->statistics();
        if (totals.attachments != 0 || totals.active_jobs != 0) {
            host->diagnose(core::Diagnostic::Severity::Warning, "gameplay.equipment", "Some reservation cleanup is blocked by inventory state");
        }
    }));
    static_cast<void>(kernel.add_command("equipment.bind", [service, registry](core::Kernel&, core::CommandArgs args) {
        std::uint64_t mask{};
        if (args.size() != 2 || !number(args[1], mask)) { return core::CommandResult{false, "equipment.bind ACTOR SLOT_MASK"}; }
        return command_result(service->bind(resolve_actor(*registry, args[0]), mask));
    }, "Bind an existing persistent actor to enabled equipment slots"));
    static_cast<void>(kernel.add_command("equipment.item", [inventory](core::Kernel&, core::CommandArgs args) {
        std::uint64_t id{}, slots{}, weight{};
        double modifier{};
        if ((args.size() != 4 && args.size() != 6) || !number(args[0], id) || !number(args[2], slots) ||
            !number(args[3], weight) || slots == 0 || (args.size() == 6 && (!seconds(args[5], modifier) || args[4].empty()))) {
            return core::CommandResult{false, "equipment.item ID NAME SLOT_MASK WEIGHT_GRAMS [STAT ADDITIVE_VALUE]"};
        }
        ItemDefinition definition;
        definition.id = {id}; definition.name = args[1]; definition.display_name = args[1];
        definition.maximum_stack = 1; definition.equipment_slots = slots; definition.unit_weight = weight;
        if (args.size() == 6) { definition.properties.emplace("equip." + args[4], modifier); }
        return command_result(inventory_result(inventory->define(std::move(definition))));
    }, "Define a real immutable inventory item with equipment eligibility and an optional modifier"));
    static_cast<void>(kernel.add_command("equipment.equip", [service, registry, inventory](core::Kernel&, core::CommandArgs args) {
        std::uint64_t slot{}, container{}, stack{}, token{}, access{};
        if (args.size() < 5 || args.size() > 6 || !number(args[1], slot) || slot >= 64 || !number(args[2], container) ||
            !number(args[3], stack) || !number(args[4], token) || (args.size() == 6 && !number(args[5], access))) {
            return core::CommandResult{false, "equipment.equip ACTOR SLOT CONTAINER_ID STACK_SEQUENCE TOKEN [ACCESS]"};
        }
        return command_result(service->equip(resolve_actor(*registry, args[0]), static_cast<std::uint8_t>(slot),
            inventory->find({container}), inventory->resolve_stack({container}, stack), {token}, {access}));
    }, "Reserve one real stack unit for an eligible actor slot"));
    static_cast<void>(kernel.add_command("equipment.unequip", [service, registry](core::Kernel&, core::CommandArgs args) {
        std::uint64_t slot{};
        if (args.size() != 2 || !number(args[1], slot) || slot >= 64) {
            return core::CommandResult{false, "equipment.unequip ACTOR SLOT"};
        }
        return command_result(service->unequip(resolve_actor(*registry, args[0]), static_cast<std::uint8_t>(slot)));
    }, "Release only the attachment's reservation"));
    static_cast<void>(kernel.add_command("equipment.inspect", [service, registry](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "equipment.inspect ACTOR"}; }
        const auto actor = resolve_actor(*registry, args[0]);
        const auto snapshot = service->inspect(actor);
        if (!snapshot) { return core::CommandResult{false, "actor is not bound"}; }
        const auto modifiers = service->modifiers(actor);
        std::string output = "attachments=" + std::to_string(snapshot->attachments.size()) + " modifiers=" + std::string(describe(modifiers.error));
        for (const auto& attachment : snapshot->attachments) {
            output += "\nslot=" + std::to_string(attachment.slot) + " item=" + std::to_string(attachment.definition.value) +
                " token=" + std::to_string(attachment.reservation.value) + (attachment.retiring ? " retiring" : " equipped");
        }
        for (const auto& [name, value] : modifiers.values) { output += "\n" + name + "=" + std::to_string(value); }
        return core::CommandResult{true, std::move(output)};
    }, "Inspect live slot ownership and finite additive equipment properties"));
    static_cast<void>(kernel.add_command("craft.recipe", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t id{}, input{}, input_count{}, output{}, output_count{}, batch{};
        double duration{};
        if (args.size() != 8 || !number(args[0], id) || !seconds(args[2], duration) || !number(args[3], input) ||
            !number(args[4], input_count) || !number(args[5], output) || !number(args[6], output_count) || !number(args[7], batch) ||
            input_count > std::numeric_limits<std::uint32_t>::max() || output_count > std::numeric_limits<std::uint32_t>::max() ||
            batch > std::numeric_limits<std::uint32_t>::max()) {
            return core::CommandResult{false, "craft.recipe ID NAME SECONDS INPUT_ID INPUT_COUNT OUTPUT_ID OUTPUT_COUNT MAX_BATCH"};
        }
        CraftRecipe recipe;
        recipe.id = {id}; recipe.name = args[1]; recipe.duration_seconds = duration;
        recipe.maximum_batch = static_cast<std::uint32_t>(batch);
        recipe.inputs.push_back({{input}, static_cast<std::uint32_t>(input_count)});
        recipe.outputs.push_back({{output}, static_cast<std::uint32_t>(output_count), {}});
        return command_result(service->define_recipe(std::move(recipe)));
    }, "Define an immutable one-input recipe; C++ API supports bounded multiple inputs and products"));
    static_cast<void>(kernel.add_command("craft.start", [service, registry, inventory](core::Kernel&, core::CommandArgs args) {
        std::uint64_t id{}, recipe{}, source{}, destination{}, batches{}, access{};
        if (args.size() < 6 || args.size() > 7 || !number(args[0], id) || !number(args[2], recipe) || !number(args[3], source) ||
            !number(args[4], destination) || !number(args[5], batches) || batches > std::numeric_limits<std::uint32_t>::max() ||
            (args.size() == 7 && !number(args[6], access))) {
            return core::CommandResult{false, "craft.start JOB_ID ACTOR RECIPE_ID SOURCE_ID DESTINATION_ID BATCHES [ACCESS]"};
        }
        const auto source_container = inventory->find({source});
        const auto destination_container = inventory->find({destination});
        if ((source != 0 && !source_container) || (destination != 0 && !destination_container)) {
            return core::CommandResult{false, "named inventory container is unavailable"};
        }
        const CraftRequest request{{id}, resolve_actor(*registry, args[1]), {recipe}, source_container,
            destination_container, static_cast<std::uint32_t>(batches), {access}};
        const auto admitted = service->start(request);
        if (!admitted.admitted()) { return command_result(admitted.result); }
        return core::CommandResult{true, "inputs reserved; job=" + std::to_string(id) + "; outputs are pending completion"};
    }, "Admit a craft job by atomically reserving its actual inputs"));
    for (const std::string action : {"complete", "cancel", "forget", "inspect"}) {
        static_cast<void>(kernel.add_command("craft." + action, [service, action](core::Kernel&, core::CommandArgs args) {
            std::uint64_t id{};
            if (args.size() != 1 || !number(args[0], id)) { return core::CommandResult{false, "craft." + action + " JOB_ID"}; }
            const auto job = service->find_job({id});
            if (action == "complete") { return command_result(service->complete(job)); }
            if (action == "cancel") { return command_result(service->cancel(job)); }
            if (action == "forget") { return command_result(service->forget(job)); }
            const auto snapshot = service->job(job);
            if (!snapshot) { return core::CommandResult{false, "job unavailable"}; }
            return core::CommandResult{true, std::string(describe(snapshot->state)) + " remaining=" +
                std::to_string(snapshot->remaining_seconds) + " inventory=" + std::string(describe(snapshot->inventory_error))};
        }, "Inspect or synchronously advance the named reserved craft job"));
    }
    for (const bool loading : {false, true}) {
        static_cast<void>(kernel.add_command(loading ? "equipment.load" : "equipment.save",
            [service, loading](core::Kernel& host, core::CommandArgs args) {
                auto* journal = host.services().find<persistence::SaveJournal>();
                if (!journal || args.size() != 1) { return core::CommandResult{false, "open journal and snapshot key required"}; }
                return command_result(loading ? service->load(*journal, args[0]) : service->save(*journal, args[0]));
            }, loading ? "Restore portable references after inventory restore" : "Save equipment and crafting metadata"));
    }
}

} // namespace ge::gameplay
