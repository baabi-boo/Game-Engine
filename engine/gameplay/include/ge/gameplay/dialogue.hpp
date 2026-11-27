#pragma once

#include <ge/core/signals.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/text/localization.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }

namespace ge::gameplay {

enum class DialogueError : std::uint8_t {
    None, InvalidDefinition, Duplicate, UnknownGraph, InvalidActor, MissingIdentity,
    Prerequisite, Capacity, Busy, UnknownSession, InvalidState, StalePresentation,
    UnavailableChoice, MissingProvider, EffectRejected, RetryExhausted,
    TransitionLimit, Exhausted, AllocationFailure, InvalidArgument
};
enum class DialogueState : std::uint8_t {
    Running, AwaitingAdvance, AwaitingChoice, AwaitingEffects,
    Completed, Cancelled, Failed, TimedOut
};
enum class DialogueNodeKind : std::uint8_t { Line, Choices, Branch, End };
enum class DialogueSubject : std::uint8_t { Actor, Partner };
enum class DialogueComparison : std::uint8_t { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
enum class DialogueConditionKind : std::uint8_t {
    Always, Flag, Tag, InventoryAtLeast, MissionCompleted, MissionActive,
    CombatCanAct, HealthAtLeast, Provider
};
enum class DialogueTruth : std::uint8_t { Pass, Fail, Unavailable };
enum class DialogueEffectKind : std::uint8_t {
    SetFlag, InventoryCost, InventoryGrant, MissionEvent, Script, Provider
};
enum class DialogueEffectPhase : std::uint8_t { Apply, Poll, Cancel };
enum class DialogueEffectOutcome : std::uint8_t { Completed, Pending, Retry, Rejected };
enum class DialogueReceiptState : std::uint8_t { Queued, Pending, Completed, Rejected, Abandoned };
enum class DialogueEventKind : std::uint8_t {
    Started, NodeEntered, Presented, ChoiceSelected, EffectAdmitted,
    EffectCompleted, EffectRetry, EffectRejected, Completed, Cancelled, Failed, TimedOut
};

[[nodiscard]] std::string_view describe(DialogueError value) noexcept;
[[nodiscard]] std::string_view describe(DialogueState value) noexcept;

struct DialogueSessionId {
    std::uint64_t scope{};
    RuntimeHandle handle{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return scope != 0 && handle.valid();
    }
    auto operator<=>(const DialogueSessionId&) const = default;
};

struct DialogueEffectId {
    std::uint64_t producer{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return producer != 0 && sequence != 0;
    }
    auto operator<=>(const DialogueEffectId&) const = default;
};

struct DialogueResult {
    DialogueError error{DialogueError::None};
    DialogueSessionId session;
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept { return error == DialogueError::None; }
};

struct DialogueText {
    // LocalizationService formats key first; format_message formats fallback.
    // Built-ins: actor_name, partner_name, and flag.NAME for current local flags.
    // Authored arguments cannot override these names. An empty session locale
    // follows the LocalizationService locale and catalog fallback chain.
    std::string key;
    std::string fallback;
    std::vector<text::FormatArgument> arguments;
};

struct DialogueCondition {
    DialogueConditionKind kind{DialogueConditionKind::Always};
    DialogueSubject subject{DialogueSubject::Actor};
    DialogueComparison comparison{DialogueComparison::Equal};
    std::string key;
    std::string provider;
    std::int64_t value{};
    std::uint64_t quantity{1};
    double threshold{};
    bool negate{};
};

struct DialogueConditionResult {
    DialogueTruth truth{DialogueTruth::Pass};
    std::string reason;
};

struct DialogueCost {
    std::string item;
    std::uint64_t quantity{1};
};

struct DialogueEffect {
    DialogueEffectKind kind{DialogueEffectKind::SetFlag};
    DialogueSubject subject{DialogueSubject::Actor};
    std::string key;
    std::string provider;
    std::int64_t value{};
    std::uint64_t quantity{1};
    std::vector<DialogueCost> costs;
};

struct DialogueChoice {
    std::string id;
    DialogueText text;
    std::vector<DialogueCondition> conditions;
    std::vector<DialogueCost> costs;
    std::vector<DialogueEffect> effects;
    std::string next;
};

struct DialogueBranch {
    std::vector<DialogueCondition> conditions;
    std::string next;
};

struct DialogueNode {
    std::string id;
    DialogueNodeKind kind{DialogueNodeKind::Line};
    DialogueSubject speaker{DialogueSubject::Partner};
    DialogueText text;
    std::vector<DialogueCondition> conditions;
    std::vector<DialogueEffect> effects;
    std::vector<DialogueChoice> choices;
    std::vector<DialogueBranch> branches;
    std::string next;
    double timeout_seconds{};
};

struct DialogueGraph {
    std::string id;
    std::uint64_t revision{1};
    std::string entry;
    std::vector<DialogueCondition> prerequisites;
    std::vector<DialogueNode> nodes;
    double timeout_seconds{300.0};
};

struct DialogueContext {
    DialogueSessionId session;
    ecs::Entity actor{};
    ecs::Entity partner{};
    PersistentId persistent_actor{};
    PersistentId persistent_partner{};
    std::string graph;
    std::string node;
    std::uint64_t visit{};
    std::map<std::string, std::int64_t, std::less<>> flags;
};

// Read-only bounded queries. Negation is applied once by DialogueService to
// Pass/Fail; Unavailable is never inverted into permission. Missing flags are 0.
using DialogueConditionProvider = std::function<DialogueConditionResult(
    const DialogueContext&, const DialogueCondition&)>;

struct DialogueEffectRequest {
    DialogueEffectId id;
    DialogueEffectPhase phase{DialogueEffectPhase::Apply};
    DialogueContext context;
    DialogueEffect effect;
};

struct DialogueEffectResult {
    DialogueEffectOutcome outcome{DialogueEffectOutcome::Rejected};
    std::string message;
};

// Providers retain deduplication by the full immutable request ID. Pending means
// admitted, never completed: subsequent calls use Poll, then Cancel on retirement.
// Retry means no admission occurred. Throwing Apply calls are terminal failures;
// their external outcome is unknown and is never blindly reapplied. Callbacks
// must be bounded, synchronous and must not mutate this DialogueService.
using DialogueEffectProvider = std::function<DialogueEffectResult(const DialogueEffectRequest&)>;

struct DialogueRenderedText {
    std::string text;
    std::string key;
    std::string locale;
    std::string error;
    bool used_fallback{};
};

struct DialogueAvailableChoice {
    std::string id;
    DialogueRenderedText text;
    bool enabled{};
    std::string reason;
};

struct DialogueSession {
    DialogueContext context;
    DialogueState state{DialogueState::Running};
    std::uint64_t graph_revision{};
    std::uint64_t presentation_revision{};
    std::string locale;
    DialogueRenderedText line;
    ecs::Entity speaker{};
    std::vector<DialogueAvailableChoice> choices;
    std::string selected_choice;
    double elapsed_seconds{};
    double node_seconds{};
    std::size_t completed_effects{};
    std::size_t pending_effects{};
    DialogueError error{DialogueError::None};
    std::string message;
};

struct DialogueEffectReceipt {
    DialogueEffectId runtime_id;
    DialogueEffectRequest request;
    DialogueReceiptState state{DialogueReceiptState::Queued};
    std::uint32_t attempts{};
    std::uint64_t polls{};
    bool cancellation_pending{};
    std::string message;
};

struct DialogueEvent {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    DialogueEventKind kind{DialogueEventKind::Started};
    DialogueContext context;
    DialogueState state{DialogueState::Running};
    DialogueEffectId effect;
    std::string choice;
    std::string message;
};

struct DialogueHistory {
    std::vector<DialogueEvent> events;
    std::uint64_t next_cursor{};
    std::uint64_t newest_sequence{};
    bool history_lost{};
};

struct DialogueLimits {
    std::size_t graphs{256};
    std::size_t nodes_per_graph{256};
    std::size_t choices_per_node{32};
    std::size_t conditions_per_group{32};
    std::size_t effects_per_group{32};
    std::size_t costs_per_choice{16};
    std::size_t arguments_per_text{32};
    std::uint32_t sessions{128};
    std::size_t receipts{16384};
    std::size_t providers{64};
    std::size_t flags{128};
    std::size_t history{2048};
    std::size_t name_bytes{128};
    std::size_t text_bytes{4096};
    std::size_t graph_bytes{1024U * 1024U};
    std::size_t definition_bytes{32U * 1024U * 1024U};
    std::size_t steps_per_frame{1024};
    std::uint64_t visits_per_session{4096};
    std::uint32_t retry_attempts{8};
    double retry_seconds{0.5};
    double effect_timeout_seconds{30.0};
    double maximum_delta_seconds{10.0};
};

struct DialogueDeliveryCursor {
    std::uint64_t producer{};
    std::uint64_t next_sequence{1};
};

struct DialogueStatistics {
    std::size_t graphs{};
    std::size_t sessions{};
    std::size_t active{};
    std::size_t receipts{};
    std::size_t pending_effects{};
    std::uint64_t overwritten_history{};
    std::uint64_t lost_signals{};
    bool budget_exhausted{};
    bool invalid_delta{};
    bool delivery_enabled{};
};

// Registry is the sole entity owner. Graphs are validated and privately copied;
// every node must be reachable and able to reach an End. Runtime cycles have a
// visit budget. One live conversation per initiating actor; actors require stable
// identities and are cancelled when disabled, destroyed or rebound externally.
// Choice selection requires the current presentation revision and rechecks all
// conditions/costs. Costs commit atomically within InventoryService, then choice
// effects run in order. Earlier completed effects survive later failure/cancel.
// Script nodes start zero-argument programs with the selected entity as owner.
// Sessions/receipts are not serialized. Configure a globally unique delivery
// producer or restore its next sequence externally before durable effects. Zero
// disables Mission/Inventory/provider delivery visibly. Cursor changes require
// no active batches; a used producer is fixed and its cursor cannot decrease.
// Receipts never evict. Destruction never touches services. Graph byte budgets
// account for declared objects and text, excluding allocator overhead.
class DialogueService final {
public:
    explicit DialogueService(core::Kernel& kernel, ecs::Registry& registry,
                             DialogueLimits limits = {});
    ~DialogueService();
    DialogueService(const DialogueService&) = delete;
    DialogueService& operator=(const DialogueService&) = delete;
    DialogueResult define(DialogueGraph graph);
    [[nodiscard]] std::optional<DialogueGraph> definition(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> definitions() const;
    DialogueResult start(ecs::Entity actor, ecs::Entity partner, std::string_view graph,
                         std::string_view locale = {});
    DialogueResult advance(DialogueSessionId session, std::uint64_t presentation_revision);
    DialogueResult select(DialogueSessionId session, std::string_view choice,
                          std::uint64_t presentation_revision);
    DialogueResult refresh(DialogueSessionId session);
    DialogueResult set_flag(DialogueSessionId session, std::string key, std::int64_t value);
    DialogueResult cancel(DialogueSessionId session, std::string_view reason = {});
    DialogueResult release(DialogueSessionId session);
    DialogueResult register_condition(std::string name, DialogueConditionProvider provider);
    DialogueResult register_effect(std::string name, DialogueEffectProvider provider);
    DialogueResult unregister_condition(std::string_view name);
    DialogueResult unregister_effect(std::string_view name);
    DialogueResult configure_delivery(DialogueDeliveryCursor cursor);
    [[nodiscard]] DialogueDeliveryCursor delivery_cursor() const noexcept;
    [[nodiscard]] std::optional<DialogueSession> inspect(DialogueSessionId session) const;
    [[nodiscard]] std::vector<DialogueSession> sessions(std::size_t limit = 128) const;
    [[nodiscard]] std::optional<DialogueEffectReceipt> receipt(DialogueEffectId runtime_id) const;
    [[nodiscard]] DialogueHistory events_since(std::uint64_t cursor, std::size_t limit = 2048) const;
    [[nodiscard]] core::Signal<DialogueEvent> event_signal();
    [[nodiscard]] DialogueStatistics statistics() const noexcept;
    [[nodiscard]] const DialogueLimits& limits() const noexcept;
    void update(const core::Frame& frame);
    void shutdown() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_dialogue(core::Kernel& kernel);

} // namespace ge::gameplay
