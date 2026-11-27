#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }
namespace ge::persistence { class SaveJournal; }

namespace ge::gameplay {

enum class MissionError : std::uint8_t {
    None, InvalidDefinition, DuplicateDefinition, UnknownMission, InvalidActor,
    MissingIdentity, Prerequisite, AlreadyStarted, Capacity, InvalidEvent,
    DuplicateEvent, EventConflict, UnknownRun, InvalidState, Busy, Exhausted,
    InvalidSnapshot, DefinitionMismatch, MissingService, JournalFailure
};

enum class MissionObjectiveKind : std::uint8_t {
    Custom, Interaction, Defeat, Collect, Dialogue, Location, Elapsed
};

enum class MissionObjectiveStatus : std::uint8_t { Locked, Active, Completed, Failed };
enum class MissionStatus : std::uint8_t { Active, AwaitingRewards, Completed, Failed, Cancelled };
enum class MissionRewardOutcome : std::uint8_t { Applied, AlreadyApplied, Retry, Rejected };
enum class MissionReceiptStatus : std::uint8_t { Pending, Applied, Rejected };
enum class MissionEventKind : std::uint8_t {
    Started, ObjectiveActivated, ObjectiveProgress, ObjectiveCompleted,
    ObjectiveFailed, ObjectivesCompleted, RewardApplied, RewardRetry,
    RewardRejected, Completed, Failed, Cancelled, ActorRebound, SourceHistoryLost
};

[[nodiscard]] std::string_view describe(MissionError value) noexcept;
[[nodiscard]] std::string_view describe(MissionStatus value) noexcept;
[[nodiscard]] std::string_view describe(MissionEventKind value) noexcept;

struct MissionResult {
    MissionError error{MissionError::None};
    std::uint64_t run{};
    std::size_t affected{};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == MissionError::None;
    }
};

struct MissionEventId {
    std::uint64_t producer{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return producer != 0 && sequence != 0;
    }
    auto operator<=>(const MissionEventId&) const = default;
};

struct MissionReceiptId {
    std::uint64_t scope{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return scope != 0 && sequence != 0;
    }
    auto operator<=>(const MissionReceiptId&) const = default;
};

struct MissionReward {
    std::string adapter;
    std::string item;
    std::uint64_t quantity{1};
};

struct MissionRewardRequest {
    MissionReceiptId receipt;
    ecs::Entity actor{};
    PersistentId persistent_actor{};
    std::string mission;
    std::uint64_t run{};
    std::uint32_t reward_index{};
    MissionReward reward;
};

struct MissionRewardResult {
    MissionRewardOutcome outcome{MissionRewardOutcome::Retry};
    std::string message;
};

// Adapters must apply synchronously or return Retry. Command/script admission
// is not Applied. An adapter owns its durable deduplication by the full receipt
// and exact request. After applying, repeated calls must return AlreadyApplied.
// Retain that record without eviction until the shared save lineage retires.
// A throwing adapter is retried with the same receipt; partial grants therefore
// require adapter-side transactionality. Different rewards are not atomic.
using MissionRewardAdapter = std::function<MissionRewardResult(const MissionRewardRequest&)>;

struct MissionObjectiveDefinition {
    std::string id;
    std::string label;
    MissionObjectiveKind kind{MissionObjectiveKind::Custom};
    std::string event_key;
    PersistentId target{};
    std::uint64_t required{1};
    // Dependencies must name earlier objectives, giving an authored DAG order.
    std::vector<std::string> prerequisites;
    bool optional{};
    double timeout_seconds{};
    double duration_seconds{1.0};
    math::Vec3 absolute_position{};
    double radius{1.0};
    double dwell_seconds{};
};

struct MissionDefinition {
    std::string id;
    std::string label;
    std::uint64_t revision{1};
    std::vector<std::string> prerequisites;
    std::vector<MissionObjectiveDefinition> objectives;
    std::vector<MissionReward> rewards;
    double timeout_seconds{};
    bool repeatable{};
};

// Producers own unique producer namespaces and monotonically unique IDs.
// 0x4d495353494e5445 is reserved for the InteractionService history bridge.
// External input cannot advance Location or Elapsed objectives.
struct MissionInput {
    MissionEventId id;
    ecs::Entity actor{};
    MissionObjectiveKind kind{MissionObjectiveKind::Custom};
    std::string key;
    PersistentId target{};
    std::uint64_t amount{1};
};

struct MissionObjectiveProgress {
    std::string id;
    MissionObjectiveStatus status{MissionObjectiveStatus::Locked};
    std::uint64_t count{};
    double active_seconds{};
    double dwell_seconds{};
};

struct MissionRun {
    std::uint64_t id{};
    std::string mission;
    std::uint64_t definition_revision{};
    std::uint64_t definition_fingerprint{};
    ecs::Entity actor{};
    PersistentId persistent_actor{};
    MissionStatus status{MissionStatus::Active};
    double elapsed_seconds{};
    std::vector<MissionObjectiveProgress> objectives;
    std::vector<MissionReceiptId> rewards;
    std::string reason;
};

struct MissionReceipt {
    MissionReceiptId id;
    std::uint64_t run{};
    std::uint32_t reward_index{};
    MissionReceiptStatus status{MissionReceiptStatus::Pending};
    MissionRewardOutcome last_outcome{MissionRewardOutcome::Retry};
    std::uint64_t attempts{};
    double retry_seconds{};
    std::string message;
};

struct MissionEvent {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    MissionEventKind kind{MissionEventKind::Started};
    std::uint64_t run{};
    ecs::Entity actor{};
    PersistentId persistent_actor{};
    std::string mission;
    std::string objective;
    MissionReceiptId receipt;
    std::uint64_t progress{};
    std::string message;
};

struct MissionHistory {
    std::vector<MissionEvent> events;
    std::uint64_t next_cursor{};
    std::uint64_t newest_sequence{};
    bool history_lost{};
};

struct MissionLimits {
    std::size_t definitions{512};
    std::size_t objectives_per_mission{64};
    std::size_t rewards_per_mission{32};
    std::size_t prerequisites{32};
    std::size_t runs{4096};
    std::size_t runs_per_actor{128};
    std::size_t accepted_events{65536};
    std::size_t receipts{65536};
    std::size_t adapters{64};
    std::size_t history{8192};
    std::size_t input_per_frame{1024};
    std::size_t reward_attempts_per_frame{256};
    std::size_t name_bytes{128};
    std::size_t text_bytes{512};
    std::size_t snapshot_bytes{16U * 1024U * 1024U};
    double retry_seconds{1.0};
    double maximum_delta_seconds{10.0};
};

struct MissionStatistics {
    std::size_t definitions{};
    std::size_t runs{};
    std::size_t active{};
    std::size_t awaiting_rewards{};
    std::size_t completed{};
    std::size_t accepted_events{};
    std::size_t receipts{};
    std::size_t pending_rewards{};
    std::size_t rejected_rewards{};
    std::uint64_t overwritten_history{};
    std::uint64_t signal_rejections{};
    std::uint64_t source_history_losses{};
    std::uint64_t interaction_cursor{};
    bool input_backpressure{};
    bool invalid_delta{};
    bool interaction_suspended{};
};

// Single kernel-thread owner. Registry owns identities; no entity allocator is
// introduced. Definitions are validated, copied and immutable after admission.
// Runs require a nonzero Registry persistent identity. All accepted input IDs,
// runs and receipts are retained; full capacity rejects admission without eviction.
// Independent readers use events_since, or SignalHub channel "gameplay.mission".
// Destruction never accesses borrowed services. No crash-atomic transaction is
// claimed across mission snapshots, inventory receipts and journal persistence.
// The host owns receipt_scope: use a stable, unique scope for each save lineage,
// restore it unchanged, and use a different scope for a divergent/new lineage.
class MissionService final {
public:
    explicit MissionService(core::Kernel& kernel, ecs::Registry& registry,
                            MissionLimits limits = {}, std::uint64_t receipt_scope = 1);
    ~MissionService();
    MissionService(const MissionService&) = delete;
    MissionService& operator=(const MissionService&) = delete;
    MissionResult define(MissionDefinition definition);
    [[nodiscard]] const MissionDefinition* definition(std::string_view id) const noexcept;
    // The same immutable fingerprint stored in runs and mission snapshots.
    [[nodiscard]] std::optional<std::uint64_t> definition_hash(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<std::string> definitions() const;
    MissionResult start(ecs::Entity actor, std::string_view mission);
    MissionResult submit(const MissionInput& input);
    MissionResult cancel(std::uint64_t run, std::string_view reason = {});
    MissionResult fail(std::uint64_t run, std::string_view reason);
    MissionResult retry_rewards(std::uint64_t run);
    MissionResult rebind_actor(PersistentId actor, ecs::Entity replacement);
    // After restore or source reset, explicitly choose the InteractionService
    // cursor for that source lifetime before its automatic bridge resumes.
    MissionResult resume_interactions(std::uint64_t cursor);
    MissionResult register_reward_adapter(std::string name, MissionRewardAdapter adapter);
    MissionResult unregister_reward_adapter(std::string_view name);
    void update(const core::Frame& frame);
    [[nodiscard]] std::optional<MissionRun> inspect(std::uint64_t run) const;
    [[nodiscard]] std::optional<MissionReceipt> receipt(MissionReceiptId id) const;
    [[nodiscard]] std::vector<MissionRun> runs(PersistentId actor = {}, std::size_t limit = 128) const;
    [[nodiscard]] bool completed(PersistentId actor, std::string_view mission) const noexcept;
    [[nodiscard]] MissionHistory events_since(std::uint64_t cursor, std::size_t limit = 8192) const;
    [[nodiscard]] MissionStatistics statistics() const noexcept;
    [[nodiscard]] const MissionLimits& limits() const noexcept;
    // GEMSN001 stores persistent identities, definition fingerprints, progress,
    // all dedup IDs and receipts. Definitions/adapters must be installed first.
    // Restore requires an empty service; active/pending actors must exist.
    // Output and live state are unchanged after a rejected encode/decode.
    MissionResult encode_snapshot(std::vector<std::byte>& output) const;
    MissionResult restore_snapshot(std::span<const std::byte> bytes);
    MissionResult save(persistence::SaveJournal& journal, std::string_view key) const;
    MissionResult load(const persistence::SaveJournal& journal, std::string_view key);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_mission(core::Kernel& kernel);

} // namespace ge::gameplay
