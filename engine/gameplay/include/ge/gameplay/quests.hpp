#pragma once

#include <ge/core/signals.hpp>
#include <ge/gameplay/mission.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::gameplay {

enum class QuestError : std::uint8_t {
    None, InvalidDefinition, Duplicate, UnknownQuest, InvalidActor, MissingIdentity,
    Prerequisite, AlreadyAccepted, Capacity, UnknownRecord, InvalidState,
    MissingService, MissionConflict, Busy, Exhausted, InvalidSnapshot,
    DefinitionMismatch, PartialSnapshot, JournalFailure, InvalidArgument
};
enum class QuestStatus : std::uint8_t {
    Locked, Available, Active, AwaitingRewards, Suspended, Completed, Failed, Abandoned
};
enum class QuestStageStatus : std::uint8_t {
    Locked, Ready, Active, AwaitingRewards, Completed, Failed, Cancelled, MissingRun
};
enum class QuestMissionOwnership : std::uint8_t { Owned, Adopted };
enum class QuestStartPolicy : std::uint8_t { CreateOwned, RequireAdoption };
enum class QuestTerminationPolicy : std::uint8_t { CancelOwned, Detach };
enum class QuestRestorePolicy : std::uint8_t { RequireMissions, SuspendMissing };
enum class QuestEventKind : std::uint8_t {
    Accepted, StageChanged, Completed, Failed, Abandoned, Suspended, Resumed,
    Tracked, SourceHistoryLost, Restored
};

[[nodiscard]] std::string_view describe(QuestError value) noexcept;
[[nodiscard]] std::string_view describe(QuestStatus value) noexcept;
[[nodiscard]] std::string_view describe(QuestStageStatus value) noexcept;

struct QuestResult {
    QuestError error{QuestError::None};
    std::size_t affected{};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept { return error == QuestError::None; }
};

struct QuestText {
    std::string key;
    std::string fallback;
};

struct QuestItemRequirement {
    PersistentId item{};
    std::uint64_t quantity{1};
    bool include_reserved{};
};

struct QuestStageDefinition {
    std::string id;
    std::string mission;
    QuestText title;
    std::vector<std::string> prerequisites;
    QuestStartPolicy start_policy{QuestStartPolicy::CreateOwned};
};

struct QuestDefinition {
    std::string id;
    std::uint64_t revision{1};
    QuestText title;
    QuestText summary;
    std::vector<std::string> prerequisites;
    std::vector<QuestItemRequirement> items;
    std::vector<QuestStageDefinition> stages;
};

struct QuestAvailability {
    bool available{};
    QuestError error{QuestError::None};
    std::string reason;
    std::vector<std::string> incomplete_prerequisites;
    std::vector<std::string> incomplete_missions;
    std::vector<QuestItemRequirement> missing_items;
};

struct QuestStageProgress {
    std::string id;
    std::string mission;
    QuestStageStatus status{QuestStageStatus::Locked};
    QuestMissionOwnership ownership{QuestMissionOwnership::Owned};
    std::uint64_t mission_run{};
    std::uint64_t mission_revision{};
    std::uint64_t mission_fingerprint{};
    std::uint64_t observed_progress{};
    std::uint32_t completed_objectives{};
    std::uint32_t total_objectives{};
    std::string reason;
};

struct QuestRecord {
    PersistentId persistent_actor{};
    ecs::Entity actor{};
    std::string quest;
    std::uint64_t definition_revision{};
    std::uint64_t definition_fingerprint{};
    QuestStatus status{QuestStatus::Active};
    bool tracked{};
    bool stop_owned{};
    bool cleanup_pending{};
    std::uint64_t accepted_tick{};
    std::uint64_t changed_tick{};
    std::uint64_t revision{1};
    std::vector<QuestStageProgress> stages;
    std::string reason;
};

struct QuestLocalizedText {
    std::string text;
    std::string key;
    std::string locale;
    bool used_fallback{};
};

struct QuestJournalObjective {
    QuestLocalizedText title;
    MissionObjectiveProgress progress;
    std::uint64_t required{};
    bool optional{};
};

struct QuestJournalStage {
    QuestLocalizedText title;
    QuestStageProgress progress;
    std::vector<QuestJournalObjective> objectives;
};

struct QuestJournalEntry {
    std::string quest;
    QuestLocalizedText title;
    QuestLocalizedText summary;
    QuestStatus status{QuestStatus::Locked};
    QuestAvailability availability;
    std::optional<QuestRecord> record;
    std::vector<QuestJournalStage> stages;
};

struct QuestJournalQuery {
    std::string locale;
    std::string after_quest;
    std::size_t limit{64};
    bool include_locked{true};
    bool tracked_only{};
    std::optional<QuestStatus> status;
};

struct QuestJournalPage {
    std::vector<QuestJournalEntry> entries;
    std::string next_after;
    std::uint64_t localization_revision{};
    bool more{};
};

struct QuestEvent {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    QuestEventKind kind{QuestEventKind::Accepted};
    PersistentId persistent_actor{};
    std::string quest;
    std::string stage;
    std::uint64_t mission_run{};
    QuestStatus status{QuestStatus::Active};
    std::string message;
};

struct QuestHistory {
    std::vector<QuestEvent> events;
    std::uint64_t next_cursor{};
    std::uint64_t newest_sequence{};
    bool history_lost{};
};

struct QuestLimits {
    std::size_t definitions{256};
    std::size_t stages_per_quest{32};
    std::size_t prerequisites{32};
    std::size_t item_requirements{32};
    std::size_t records{4096};
    std::size_t records_per_actor{128};
    std::size_t history{2048};
    std::size_t stage_work_per_frame{256};
    std::size_t source_events_per_frame{1024};
    std::size_t journal_page{128};
    std::size_t name_bytes{128};
    std::size_t text_bytes{4096};
    std::size_t snapshot_bytes{16U * 1024U * 1024U};
};

struct QuestStatistics {
    std::size_t definitions{};
    std::size_t records{};
    std::size_t active{};
    std::size_t awaiting_rewards{};
    std::size_t suspended{};
    std::size_t completed{};
    std::size_t cleanup_pending{};
    std::uint64_t source_cursor{};
    std::uint64_t source_history_losses{};
    std::uint64_t overwritten_history{};
    std::uint64_t lost_signals{};
    bool budget_exhausted{};
};

// Single Kernel-thread owner. Definitions and both authored DAGs are immutable;
// prerequisites name earlier definitions/stages. Every stage is mandatory. Each
// actor may accept a definition once; records and bindings never silently evict.
// Owned runs start lazily in bounded updates. adopt() is explicit and never
// changes the adopted mission. Only true MissionStatus::Completed completes a
// stage; AwaitingRewards remains visible and blocks dependent stages.
// Abandon/fail commits a quest decision. CancelOwned then cancels/fails active
// owned missions incrementally; pending rewards continue, and adopted runs are
// untouched. Detach deliberately leaves every mission running. No reward undo.
// Restore Registry identities, inventory receipts and MissionService first.
// Missing saved bindings either reject or suspend; they never start replacement
// runs. Snapshot lineage must match the coordinated mission save lineage.
// Independent saves are not atomic. Stage observations recover current retained
// state after history loss, never reconstruct missed events. Destruction never
// dereferences other services; call shutdown while Kernel services remain live.
class QuestService final {
public:
    explicit QuestService(core::Kernel& kernel, ecs::Registry& registry,
                          QuestLimits limits = {}, std::uint64_t lineage = 1);
    ~QuestService();
    QuestService(const QuestService&) = delete;
    QuestService& operator=(const QuestService&) = delete;
    QuestResult define(QuestDefinition definition);
    [[nodiscard]] const QuestDefinition* definition(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<std::string> definitions() const;
    [[nodiscard]] QuestAvailability availability(PersistentId actor, std::string_view quest) const;
    QuestResult accept(ecs::Entity actor, std::string_view quest);
    QuestResult adopt(PersistentId actor, std::string_view quest, std::string_view stage,
                      std::uint64_t mission_run);
    QuestResult abandon(PersistentId actor, std::string_view quest,
                        QuestTerminationPolicy policy = QuestTerminationPolicy::CancelOwned,
                        std::string_view reason = {});
    QuestResult fail(PersistentId actor, std::string_view quest, std::string_view reason,
                     QuestTerminationPolicy policy = QuestTerminationPolicy::CancelOwned);
    QuestResult track(PersistentId actor, std::string_view quest, bool tracked);
    [[nodiscard]] std::optional<QuestRecord> inspect(PersistentId actor, std::string_view quest) const;
    [[nodiscard]] std::optional<QuestJournalEntry> journal_entry(PersistentId actor,
        std::string_view quest, std::string_view locale = {}) const;
    [[nodiscard]] QuestJournalPage journal(PersistentId actor, QuestJournalQuery query = {}) const;
    [[nodiscard]] QuestHistory events_since(std::uint64_t cursor, std::size_t limit = 2048) const;
    [[nodiscard]] core::Signal<QuestEvent> event_signal();
    [[nodiscard]] QuestStatistics statistics() const noexcept;
    [[nodiscard]] const QuestLimits& limits() const noexcept;
    void update(const core::Frame& frame);
    void shutdown() noexcept;
    QuestResult encode_snapshot(std::vector<std::byte>& output) const;
    QuestResult restore_snapshot(std::span<const std::byte> bytes,
        QuestRestorePolicy policy = QuestRestorePolicy::RequireMissions);
    QuestResult save(persistence::SaveJournal& journal, std::string_view key) const;
    QuestResult load(const persistence::SaveJournal& journal, std::string_view key,
        QuestRestorePolicy policy = QuestRestorePolicy::RequireMissions);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_quests(core::Kernel& kernel);

} // namespace ge::gameplay
