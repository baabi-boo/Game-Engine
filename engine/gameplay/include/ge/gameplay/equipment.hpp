#pragma once

#include <ge/gameplay/inventory.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::gameplay {

enum class EquipmentError : std::uint8_t {
    None, InvalidArgument, InvalidActor, NotBound, Duplicate, Capacity,
    UnknownRecipe, UnknownItem, MissingContainer, MissingStack, IneligibleSlot,
    SlotOccupied, EmptySlot, TokenConflict, InventoryRejected, NotReady,
    Invalidated, MissingJob, TerminalJob, Busy, Exhausted, AllocationFailure,
    ModifierOverflow, Corrupt, UnsupportedVersion, PersistenceFailure, NotEmpty,
    Stopped
};

[[nodiscard]] std::string_view describe(EquipmentError error) noexcept;

struct EquipmentResult {
    EquipmentError error{EquipmentError::None};
    InventoryError inventory_error{InventoryError::None};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == EquipmentError::None;
    }
};

struct EquipmentAttachment {
    std::uint8_t slot{};
    InventoryContainerId container{};
    PersistentId persistent_container{};
    InventoryStackId stack{};
    PersistentId definition{};
    InventoryItemState item_state{};
    PersistentId reservation{};
    PersistentId access{};
    bool retiring{};
};

struct EquipmentActorSnapshot {
    ecs::Entity actor{};
    PersistentId persistent_actor{};
    std::uint64_t enabled_slots{};
    std::vector<EquipmentAttachment> attachments;
};

struct EquipmentModifierSnapshot {
    EquipmentError error{EquipmentError::None};
    // Metadata history only; external inventory/Registry changes are not cached.
    std::uint64_t revision{};
    std::size_t equipped{};
    std::map<std::string, double, std::less<>> values;
    [[nodiscard]] double additive(std::string_view name) const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == EquipmentError::None;
    }
};

struct CraftIngredient {
    PersistentId definition{};
    std::uint32_t quantity{};
};

struct CraftProduct {
    PersistentId definition{};
    std::uint32_t quantity{};
    InventoryItemState state{};
};

struct CraftRecipe {
    PersistentId id{};
    std::string name;
    std::vector<CraftIngredient> inputs;
    std::vector<CraftProduct> outputs;
    double duration_seconds{};
    std::uint32_t maximum_batch{1};
};

struct CraftJobId {
    std::uint64_t scope{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return scope != 0 && sequence != 0;
    }
    auto operator<=>(const CraftJobId&) const = default;
};

enum class CraftState : std::uint8_t {
    Working, Ready, Blocked, Cancelling, Completed, Cancelled, Invalidated
};

[[nodiscard]] std::string_view describe(CraftState state) noexcept;

struct CraftRequest {
    // This stable job identity is also its inventory reservation capability.
    PersistentId id{};
    ecs::Entity actor{};
    PersistentId recipe{};
    InventoryContainerId source{};
    InventoryContainerId destination{};
    std::uint32_t batches{1};
    PersistentId access{};
};

struct CraftReservedInput {
    InventoryStackId stack{};
    PersistentId definition{};
    InventoryItemState state{};
    std::uint32_t quantity{};
};

struct CraftJobSnapshot {
    CraftJobId id{};
    CraftRequest request;
    PersistentId persistent_actor{};
    PersistentId persistent_source{};
    PersistentId persistent_destination{};
    CraftState state{CraftState::Working};
    double remaining_seconds{};
    std::vector<CraftReservedInput> inputs;
    InventoryError inventory_error{InventoryError::None};
    bool invalidating{};
};

struct CraftAdmission {
    EquipmentResult result;
    CraftJobId job{};
    // Admission reserves inputs. Only a Completed job has produced outputs.
    [[nodiscard]] bool admitted() const noexcept {
        return static_cast<bool>(result) && static_cast<bool>(job);
    }
};

enum class EquipmentChangeKind : std::uint8_t {
    Bound, Unbound, Equipped, Unequipped, AttachmentInvalidated,
    RecipeDefined, RecipeRemoved, CraftStarted, CraftReady, CraftBlocked,
    CraftCompleted, CraftCancelling, CraftCancelled, CraftInvalidated,
    CraftForgotten, Restored
};

struct EquipmentChange {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    EquipmentChangeKind kind{EquipmentChangeKind::Bound};
    PersistentId actor{};
    PersistentId subject{};
    std::uint8_t slot{};
};

struct EquipmentHistory {
    std::vector<EquipmentChange> changes;
    std::uint64_t next_cursor{};
    std::uint64_t newest_sequence{};
    bool history_lost{};
};

struct EquipmentLimits {
    std::size_t actors{512};
    std::size_t recipes{1024};
    std::size_t jobs{256};
    std::size_t ingredients{32};
    std::size_t products{32};
    std::size_t reserved_stacks{96};
    std::size_t jobs_per_actor{8};
    std::size_t job_attempts_per_frame{32};
    std::size_t history{4096};
    std::size_t name_bytes{128};
    std::size_t snapshot_bytes{16U * 1024U * 1024U};
    double maximum_duration_seconds{86400.0};
};

struct EquipmentStatistics {
    std::size_t actors{};
    std::size_t attachments{};
    std::size_t retiring_attachments{};
    std::size_t recipes{};
    std::size_t jobs{};
    std::size_t active_jobs{};
    std::size_t blocked_jobs{};
    std::uint64_t revision{};
    std::uint64_t completed_jobs{};
};

// One kernel-thread writer. Registry and InventoryService must outlive calls;
// destruction itself never touches either borrowed service. Attachments hold
// references, not item copies: equip reserves exactly one unit on one stack.
// Slot i is eligible exactly when actor.enabled_slots and item.equipment_slots
// both contain bit i (i < 64). One attachment occupies one slot. Each finite
// property "equip.<name>" adds to modifier <name>, once per equipped unit.
// Broken durable items contribute nothing. No durability mutation is performed.
// Combat owns interpretation/clamping of attack_damage, armor, damage_dealt,
// damage_taken, crit_chance, crit_multiplier and other authored property names.
// Supplied reservation IDs are local capabilities, not security credentials.
// Use distinct IDs for independently owned reservations. Unequip/cancel release
// only their exact stack quantities under those IDs; locks can defer cleanup.
class EquipmentService final {
public:
    EquipmentService(ecs::Registry& registry, InventoryService& inventory,
                     EquipmentLimits limits = {});
    ~EquipmentService();
    EquipmentService(const EquipmentService&) = delete;
    EquipmentService& operator=(const EquipmentService&) = delete;
    EquipmentResult bind(ecs::Entity actor, std::uint64_t enabled_slots);
    // Releases slots independently; an inventory lock may leave a partial unbind.
    EquipmentResult unbind(ecs::Entity actor);
    EquipmentResult equip(ecs::Entity actor, std::uint8_t slot,
                          InventoryContainerId container, InventoryStackId stack,
                          PersistentId reservation, PersistentId access = {});
    EquipmentResult unequip(ecs::Entity actor, std::uint8_t slot);
    [[nodiscard]] std::optional<EquipmentActorSnapshot> inspect(ecs::Entity actor) const;
    // Revalidates ownership and reservation on every call, including between ticks.
    [[nodiscard]] EquipmentModifierSnapshot modifiers(ecs::Entity actor) const;
    EquipmentResult define_recipe(CraftRecipe recipe);
    EquipmentResult remove_recipe(PersistentId id);
    [[nodiscard]] const CraftRecipe* recipe(PersistentId id) const noexcept;
    [[nodiscard]] std::vector<CraftRecipe> recipes() const;
    CraftAdmission start(const CraftRequest& request);
    // Success means inputs were consumed and outputs committed synchronously.
    // Blocked jobs retain inputs and require explicit complete() to retry.
    EquipmentResult complete(CraftJobId job);
    EquipmentResult cancel(CraftJobId job);
    EquipmentResult forget(CraftJobId job);
    [[nodiscard]] std::optional<CraftJobSnapshot> job(CraftJobId id) const;
    [[nodiscard]] CraftJobId find_job(PersistentId id) const noexcept;
    [[nodiscard]] std::vector<CraftJobId> jobs(ecs::Entity actor = {}) const;
    [[nodiscard]] EquipmentHistory changes_since(std::uint64_t cursor,
                                                  std::size_t limit = 4096) const;
    void update(const core::Frame& frame);
    void shutdown();
    // Restore into an empty service after inventory and Registry restore. Only
    // persistent actor/container IDs, stack sequences and reservation IDs are
    // serialized. Runtime scopes/handles are always resolved again. Separate
    // save calls are not crash-atomic across services; stage both snapshots in
    // the same SaveJournal transaction when a coordinated save is required.
    EquipmentResult snapshot(std::vector<std::byte>& destination) const;
    EquipmentResult restore(std::span<const std::byte> bytes);
    EquipmentResult save(persistence::SaveJournal& journal, std::string_view key) const;
    EquipmentResult load(const persistence::SaveJournal& journal, std::string_view key);
    [[nodiscard]] EquipmentStatistics statistics() const noexcept;
    [[nodiscard]] const EquipmentLimits& limits() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_equipment(core::Kernel& kernel);

} // namespace ge::gameplay
