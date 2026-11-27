#include <ge/gameplay/mission.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/core/signals.hpp>
#include <ge/gameplay/interaction.hpp>
#include <ge/persistence/journal.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ge::gameplay {
namespace {

constexpr std::uint64_t interaction_producer = 0x4d495353494e5445ULL;
constexpr std::uint32_t mission_value_type = 0x4d49534eU;
constexpr double maximum_time = 1.0e12;

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) { ++value; }
}

bool finite_time(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= maximum_time;
}

bool valid_text(std::string_view value, std::size_t maximum, bool empty = true) {
    return (empty || !value.empty()) && value.size() <= maximum &&
        value.find('\0') == std::string_view::npos && core::valid_utf8(value);
}

bool valid_name(std::string_view value, std::size_t maximum) {
    return valid_text(value, maximum, false) && std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte > 32U && byte != 127U;
    });
}

bool terminal(MissionStatus status) noexcept {
    return status == MissionStatus::Completed || status == MissionStatus::Failed || status == MissionStatus::Cancelled;
}

MissionResult error(MissionError code, std::string message, std::uint64_t run = 0) {
    return {code, run, 0, std::move(message)};
}

struct MutationGuard {
    bool& busy;
    explicit MutationGuard(bool& value) : busy(value) { busy = true; }
    ~MutationGuard() { busy = false; }
};

template<class T>
bool parse_number(std::string_view text, T& output) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) { return false; }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) { return false; }
    }
    output = value;
    return true;
}

std::uint64_t definition_fingerprint(const MissionDefinition& definition, const MissionLimits& limits) {
    core::ByteWriter writer(limits.snapshot_bytes);
    writer.write_string(definition.id); writer.write_string(definition.label);
    writer.write_u64(definition.revision); writer.write_f64(definition.timeout_seconds);
    writer.write_bool(definition.repeatable);
    writer.write_u32(static_cast<std::uint32_t>(definition.prerequisites.size()));
    for (const auto& prerequisite : definition.prerequisites) { writer.write_string(prerequisite); }
    writer.write_u32(static_cast<std::uint32_t>(definition.objectives.size()));
    for (const auto& objective : definition.objectives) {
        writer.write_string(objective.id); writer.write_string(objective.label);
        writer.write_u8(static_cast<std::uint8_t>(objective.kind)); writer.write_string(objective.event_key);
        writer.write_u64(objective.target.value); writer.write_u64(objective.required);
        writer.write_bool(objective.optional); writer.write_f64(objective.timeout_seconds);
        writer.write_f64(objective.duration_seconds); writer.write_f64(objective.absolute_position.x);
        writer.write_f64(objective.absolute_position.y); writer.write_f64(objective.absolute_position.z);
        writer.write_f64(objective.radius); writer.write_f64(objective.dwell_seconds);
        writer.write_u32(static_cast<std::uint32_t>(objective.prerequisites.size()));
        for (const auto& prerequisite : objective.prerequisites) { writer.write_string(prerequisite); }
    }
    writer.write_u32(static_cast<std::uint32_t>(definition.rewards.size()));
    for (const auto& reward : definition.rewards) {
        writer.write_string(reward.adapter); writer.write_string(reward.item); writer.write_u64(reward.quantity);
    }
    return core::fnv1a64(writer.bytes());
}

} // namespace

std::string_view describe(MissionError value) noexcept {
    constexpr std::array names{"none", "invalid_definition", "duplicate_definition", "unknown_mission",
        "invalid_actor", "missing_identity", "prerequisite", "already_started", "capacity", "invalid_event",
        "duplicate_event", "event_conflict", "unknown_run", "invalid_state", "busy", "exhausted",
        "invalid_snapshot", "definition_mismatch", "missing_service", "journal_failure"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}

std::string_view describe(MissionStatus value) noexcept {
    constexpr std::array names{"active", "awaiting_rewards", "completed", "failed", "cancelled"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}

std::string_view describe(MissionEventKind value) noexcept {
    constexpr std::array names{"started", "objective_activated", "objective_progress", "objective_completed",
        "objective_failed", "objectives_completed", "reward_applied", "reward_retry", "reward_rejected",
        "completed", "failed", "cancelled", "actor_rebound", "source_history_lost"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}

struct MissionService::Impl {
    struct DefinitionEntry {
        MissionDefinition value;
        std::uint64_t fingerprint{};
        std::vector<std::vector<std::size_t>> dependencies;
    };
    struct AcceptedInput {
        PersistentId actor;
        MissionObjectiveKind kind{};
        std::string key;
        PersistentId target;
        std::uint64_t amount{};
        bool operator==(const AcceptedInput&) const = default;
    };
    core::Kernel& kernel;
    ecs::Registry& registry;
    MissionLimits limits;
    std::uint64_t scope{};
    std::map<std::string, DefinitionEntry, std::less<>> definitions;
    std::map<std::uint64_t, MissionRun> runs;
    std::map<MissionReceiptId, MissionReceipt> receipts;
    std::map<MissionEventId, AcceptedInput> accepted;
    std::map<std::string, MissionRewardAdapter, std::less<>> adapters;
    std::deque<MissionEvent> history;
    MissionStatistics counters;
    std::uint64_t next_run{1};
    std::uint64_t next_receipt{1};
    std::uint64_t next_bridge{1};
    std::uint64_t event_sequence{};
    std::uint64_t last_history_gap{};
    std::uint64_t interaction_cursor{};
    std::uint64_t reward_cursor{};
    std::uint64_t tick{};
    bool had_frame{};
    bool busy{};
    bool interaction_suspended{};

    Impl(core::Kernel& host, ecs::Registry& entities, MissionLimits configuration, std::uint64_t identity)
        : kernel(host), registry(entities), limits(configuration), scope(identity) {}

    bool actor_valid(const MissionRun& run) const noexcept {
        return registry.alive(run.actor) && registry.persistent_id(run.actor) == run.persistent_actor;
    }

    void emit(MissionEventKind kind, const MissionRun* run = nullptr, std::string_view objective = {},
              std::uint64_t progress = 0, MissionReceiptId receipt_id = {}, std::string_view message = {}) noexcept {
        if (event_sequence == std::numeric_limits<std::uint64_t>::max()) {
            increment(counters.overwritten_history); return;
        }
        const auto sequence = ++event_sequence;
        bool retained = false;
        try {
            MissionEvent event;
            event.sequence = sequence; event.tick = tick; event.kind = kind;
            event.objective = objective; event.progress = progress; event.receipt = receipt_id;
            event.message = message.substr(0, limits.text_bytes);
            if (run) {
                event.run = run->id; event.actor = run->actor; event.persistent_actor = run->persistent_actor;
                event.mission = run->mission;
            }
            history.push_back(event);
            retained = true;
            if (history.size() > limits.history) { history.pop_front(); increment(counters.overwritten_history); }
            if (auto* hub = kernel.services().find<core::SignalHub>()) {
                const auto channel = hub->channel<MissionEvent>("gameplay.mission");
                const auto bytes = event.mission.size() + event.objective.size() + event.message.size();
                if (!hub->enqueue(channel, std::move(event), bytes)) { increment(counters.signal_rejections); }
            }
        } catch (...) {
            if (!retained) { last_history_gap = sequence; increment(counters.overwritten_history); }
            increment(counters.signal_rejections);
        }
    }

    bool completed(PersistentId actor, std::string_view mission) const noexcept {
        return std::any_of(runs.begin(), runs.end(), [&](const auto& pair) {
            return pair.second.persistent_actor == actor && pair.second.mission == mission &&
                pair.second.status == MissionStatus::Completed;
        });
    }

    void stop(MissionRun& run, MissionStatus status, std::string_view reason) {
        run.reason = reason.substr(0, limits.text_bytes);
        run.status = status;
        emit(status == MissionStatus::Cancelled ? MissionEventKind::Cancelled : MissionEventKind::Failed,
             &run, {}, 0, {}, run.reason);
    }

    void settle(MissionRun& run) {
        if (run.status != MissionStatus::Active) { return; }
        const auto& definition = definitions.at(run.mission);
        for (std::size_t index = 0; index < run.objectives.size(); ++index) {
            auto& progress = run.objectives[index];
            if (progress.status != MissionObjectiveStatus::Locked) { continue; }
            bool ready = true;
            bool failed = false;
            for (const auto dependency : definition.dependencies[index]) {
                const auto status = run.objectives[dependency].status;
                ready = ready && status == MissionObjectiveStatus::Completed;
                failed = failed || status == MissionObjectiveStatus::Failed;
            }
            if (failed) {
                progress.status = MissionObjectiveStatus::Failed;
                emit(MissionEventKind::ObjectiveFailed, &run, progress.id, progress.count, {}, "dependency failed");
            } else if (ready) {
                progress.status = MissionObjectiveStatus::Active;
                emit(MissionEventKind::ObjectiveActivated, &run, progress.id);
            }
        }
        bool complete = true;
        for (std::size_t index = 0; index < run.objectives.size(); ++index) {
            if (definition.value.objectives[index].optional) { continue; }
            const auto status = run.objectives[index].status;
            if (status == MissionObjectiveStatus::Failed) { stop(run, MissionStatus::Failed, "required objective failed"); return; }
            complete = complete && status == MissionObjectiveStatus::Completed;
        }
        if (!complete) { return; }
        run.status = run.rewards.empty() ? MissionStatus::Completed : MissionStatus::AwaitingRewards;
        emit(MissionEventKind::ObjectivesCompleted, &run);
        if (run.status == MissionStatus::Completed) { emit(MissionEventKind::Completed, &run); }
    }

    void progress(MissionRun& run, std::size_t index, std::uint64_t amount) {
        auto& objective = run.objectives[index];
        const auto required = definitions.at(run.mission).value.objectives[index].required;
        objective.count += std::min(amount, required - objective.count);
        emit(MissionEventKind::ObjectiveProgress, &run, objective.id, objective.count);
        if (objective.count == required) {
            objective.status = MissionObjectiveStatus::Completed;
            emit(MissionEventKind::ObjectiveCompleted, &run, objective.id, objective.count);
        }
    }

    MissionResult admit(const MissionInput& input, bool internal) {
        if (!input.id || input.amount == 0 || input.kind > MissionObjectiveKind::Dialogue ||
            (!internal && input.id.producer == interaction_producer) ||
            !valid_text(input.key, limits.name_bytes)) { return error(MissionError::InvalidEvent, "invalid event identity or payload"); }
        if (!registry.alive(input.actor)) { return error(MissionError::InvalidActor, "actor generation is not live"); }
        const auto identity = registry.persistent_id(input.actor);
        if (!identity) { return error(MissionError::MissingIdentity, "actor needs a persistent identity"); }
        AcceptedInput event{identity, input.kind, input.key, input.target, input.amount};
        const auto previous = accepted.find(input.id);
        if (previous != accepted.end()) {
            return error(previous->second == event ? MissionError::DuplicateEvent : MissionError::EventConflict,
                         previous->second == event ? "event already admitted" : "event ID reused with different data");
        }
        if (accepted.size() >= limits.accepted_events) { return error(MissionError::Capacity, "accepted event retention is full"); }
        accepted.emplace(input.id, std::move(event));
        std::size_t changed{};
        for (auto& [run_id, run] : runs) {
            static_cast<void>(run_id);
            if (run.actor != input.actor || run.persistent_actor != identity || run.status != MissionStatus::Active) { continue; }
            const auto& definition = definitions.at(run.mission).value;
            for (std::size_t index = 0; index < run.objectives.size(); ++index) {
                const auto& objective = definition.objectives[index];
                if (run.objectives[index].status != MissionObjectiveStatus::Active || objective.kind != input.kind ||
                    (!objective.event_key.empty() && objective.event_key != input.key) ||
                    (objective.target && objective.target != input.target)) { continue; }
                progress(run, index, input.amount); ++changed;
            }
            settle(run);
        }
        return {MissionError::None, 0, changed, "event admitted"};
    }

    void poll_interactions() {
        counters.input_backpressure = false;
        auto* interactions = kernel.services().find<InteractionService>();
        if (!interactions || interaction_suspended) { return; }
        const auto batch = interactions->events_since(interaction_cursor, limits.input_per_frame);
        if (batch.newest_sequence < interaction_cursor || batch.history_lost) {
            increment(counters.source_history_losses);
            emit(MissionEventKind::SourceHistoryLost, nullptr, {}, 0, {}, "interaction cursor requires explicit resume");
            interaction_suspended = true; return;
        }
        for (const auto& event : batch.events) {
            if (event.kind != InteractionEventKind::Activated || !registry.alive(event.actor) ||
                !registry.persistent_id(event.actor)) { interaction_cursor = event.sequence; continue; }
            if (next_bridge == std::numeric_limits<std::uint64_t>::max()) { counters.input_backpressure = true; break; }
            MissionInput input;
            input.id = {interaction_producer, next_bridge}; input.actor = event.actor;
            input.kind = MissionObjectiveKind::Interaction; input.key = event.name;
            input.target = registry.persistent_id(event.target);
            const auto result = admit(input, true);
            if (result.error == MissionError::Capacity) { counters.input_backpressure = true; break; }
            if (result) { ++next_bridge; }
            interaction_cursor = event.sequence;
        }
    }

    void advance(MissionRun& run, double delta) {
        if (!actor_valid(run)) { stop(run, MissionStatus::Failed, "actor generation or persistent identity changed"); return; }
        const auto& definition = definitions.at(run.mission).value;
        run.elapsed_seconds = std::min(maximum_time, run.elapsed_seconds + delta);
        if (definition.timeout_seconds > 0.0 && run.elapsed_seconds >= definition.timeout_seconds) {
            stop(run, MissionStatus::Failed, "mission timeout"); return;
        }
        auto* scene_service = kernel.services().find<scene::Scene>();
        auto* partition = kernel.services().find<scene::WorldPartition>();
        std::optional<math::Vec3> absolute;
        if (scene_service) {
            const auto local = scene_service->local_to_world(run.actor, {});
            if (local) { absolute = partition ? partition->to_absolute(*local) : local; }
        }
        for (std::size_t index = 0; index < run.objectives.size(); ++index) {
            auto& state = run.objectives[index];
            if (state.status != MissionObjectiveStatus::Active) { continue; }
            const auto& objective = definition.objectives[index];
            state.active_seconds = std::min(maximum_time, state.active_seconds + delta);
            if (objective.timeout_seconds > 0.0 && state.active_seconds >= objective.timeout_seconds) {
                state.status = MissionObjectiveStatus::Failed;
                emit(MissionEventKind::ObjectiveFailed, &run, state.id, state.count, {}, "objective timeout"); continue;
            }
            if (objective.kind == MissionObjectiveKind::Elapsed && state.active_seconds >= objective.duration_seconds) {
                progress(run, index, 1);
            } else if (objective.kind == MissionObjectiveKind::Location) {
                const bool inside = absolute && std::hypot(absolute->x - objective.absolute_position.x,
                    absolute->y - objective.absolute_position.y, absolute->z - objective.absolute_position.z) <= objective.radius;
                state.dwell_seconds = inside ? std::min(maximum_time, state.dwell_seconds + delta) : 0.0;
                if (inside && state.dwell_seconds >= objective.dwell_seconds) { progress(run, index, 1); }
            }
        }
        settle(run);
    }

    void apply_rewards(double delta) {
        for (auto& [id, receipt] : receipts) {
            static_cast<void>(id);
            if (receipt.status == MissionReceiptStatus::Pending) { receipt.retry_seconds = std::max(0.0, receipt.retry_seconds - delta); }
        }
        std::size_t attempts{};
        const auto candidates = receipts.size();
        for (std::size_t examined = 0; examined < candidates && attempts < limits.reward_attempts_per_frame; ++examined) {
            auto iterator = receipts.upper_bound({scope, reward_cursor});
            if (iterator == receipts.end()) { iterator = receipts.begin(); }
            auto& receipt = iterator->second;
            reward_cursor = receipt.id.sequence;
            auto& run = runs.at(receipt.run);
            if (run.status != MissionStatus::AwaitingRewards || receipt.status != MissionReceiptStatus::Pending || receipt.retry_seconds > 0.0) { continue; }
            ++attempts;
            if (receipt.attempts == std::numeric_limits<std::uint64_t>::max()) {
                receipt.status = MissionReceiptStatus::Rejected; receipt.last_outcome = MissionRewardOutcome::Rejected;
                receipt.message = "reward attempt counter exhausted";
                emit(MissionEventKind::RewardRejected, &run, {}, 0, receipt.id, receipt.message); continue;
            }
            ++receipt.attempts;
            const auto& reward = definitions.at(run.mission).value.rewards[receipt.reward_index];
            MissionRewardResult result{MissionRewardOutcome::Retry, "reward adapter is unavailable"};
            const auto adapter = adapters.find(reward.adapter);
            if (!actor_valid(run)) { result.message = "actor requires explicit identity rebind"; }
            else if (adapter != adapters.end()) {
                const MissionRewardRequest request{receipt.id, run.actor, run.persistent_actor, run.mission,
                                                   run.id, receipt.reward_index, reward};
                try { result = adapter->second(request); }
                catch (...) { result = {MissionRewardOutcome::Retry, "reward adapter threw; retry retains receipt"}; }
            }
            if (result.outcome > MissionRewardOutcome::Rejected) { result = {MissionRewardOutcome::Retry, "invalid adapter outcome"}; }
            receipt.last_outcome = result.outcome;
            receipt.message = valid_text(result.message, limits.text_bytes) ? std::move(result.message) : "adapter returned invalid diagnostic";
            if (result.outcome == MissionRewardOutcome::Applied || result.outcome == MissionRewardOutcome::AlreadyApplied) {
                receipt.status = MissionReceiptStatus::Applied; receipt.retry_seconds = 0.0;
                emit(MissionEventKind::RewardApplied, &run, {}, reward.quantity, receipt.id, receipt.message);
                const bool done = std::all_of(run.rewards.begin(), run.rewards.end(), [&](MissionReceiptId id) {
                    return receipts.at(id).status == MissionReceiptStatus::Applied;
                });
                if (done) { run.status = MissionStatus::Completed; emit(MissionEventKind::Completed, &run); }
            } else if (result.outcome == MissionRewardOutcome::Rejected) {
                receipt.status = MissionReceiptStatus::Rejected;
                emit(MissionEventKind::RewardRejected, &run, {}, 0, receipt.id, receipt.message);
            } else {
                receipt.retry_seconds = limits.retry_seconds;
                emit(MissionEventKind::RewardRetry, &run, {}, 0, receipt.id, receipt.message);
            }
        }
    }
};

MissionService::MissionService(core::Kernel& kernel, ecs::Registry& registry, MissionLimits limits, std::uint64_t scope)
    : impl_(std::make_unique<Impl>(kernel, registry, limits, scope)) {
    if (scope == 0 || limits.definitions == 0 || limits.definitions > 65536 || limits.objectives_per_mission == 0 ||
        limits.objectives_per_mission > 1024 || limits.rewards_per_mission > 256 || limits.prerequisites > 256 ||
        limits.runs == 0 || limits.runs > 65536 || limits.runs_per_actor == 0 || limits.runs_per_actor > limits.runs ||
        limits.accepted_events > 1048576 || limits.receipts > 1048576 || limits.adapters > 4096 ||
        limits.history == 0 || limits.history > 65536 || limits.input_per_frame == 0 || limits.input_per_frame > 65536 ||
        limits.reward_attempts_per_frame == 0 || limits.reward_attempts_per_frame > 65536 ||
        limits.name_bytes == 0 || limits.name_bytes > 4096 || limits.text_bytes < limits.name_bytes || limits.text_bytes > 65536 ||
        limits.snapshot_bytes < 1024 || limits.snapshot_bytes > 64U * 1024U * 1024U ||
        !finite_time(limits.retry_seconds) || limits.retry_seconds == 0.0 ||
        !finite_time(limits.maximum_delta_seconds) || limits.maximum_delta_seconds == 0.0) {
        throw std::invalid_argument("invalid mission service bounds or receipt scope");
    }
}

MissionService::~MissionService() = default;

MissionResult MissionService::define(MissionDefinition definition) {
    auto& state = *impl_;
    if (state.busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    if (state.definitions.contains(definition.id)) { return error(MissionError::DuplicateDefinition, "mission definitions are immutable"); }
    const auto& limits = state.limits;
    if (state.definitions.size() >= limits.definitions) { return error(MissionError::Capacity, "definition capacity reached"); }
    if (!valid_name(definition.id, limits.name_bytes) || !valid_text(definition.label, limits.text_bytes) ||
        definition.revision == 0 || !finite_time(definition.timeout_seconds) || definition.objectives.empty() ||
        definition.objectives.size() > limits.objectives_per_mission || definition.rewards.size() > limits.rewards_per_mission ||
        definition.prerequisites.size() > limits.prerequisites) { return error(MissionError::InvalidDefinition, "invalid mission fields or bounds"); }
    std::set<std::string> prerequisites;
    for (const auto& dependency : definition.prerequisites) {
        if (!state.definitions.contains(dependency) || !prerequisites.insert(dependency).second) {
            return error(MissionError::InvalidDefinition, "mission prerequisites must be unique, already defined missions");
        }
    }
    Impl::DefinitionEntry entry;
    std::map<std::string, std::size_t, std::less<>> indices;
    bool required{};
    for (std::size_t index = 0; index < definition.objectives.size(); ++index) {
        const auto& objective = definition.objectives[index];
        if (!valid_name(objective.id, limits.name_bytes) || indices.contains(objective.id) ||
            !valid_text(objective.label, limits.text_bytes) || !valid_text(objective.event_key, limits.name_bytes) ||
            objective.kind > MissionObjectiveKind::Elapsed || objective.required == 0 ||
            !finite_time(objective.timeout_seconds) || !finite_time(objective.duration_seconds) ||
            !finite_time(objective.dwell_seconds) || objective.prerequisites.size() > limits.prerequisites ||
            !std::isfinite(objective.absolute_position.x) || !std::isfinite(objective.absolute_position.y) ||
            !std::isfinite(objective.absolute_position.z) || !std::isfinite(objective.radius) || objective.radius <= 0.0 ||
            ((objective.kind == MissionObjectiveKind::Location || objective.kind == MissionObjectiveKind::Elapsed) && objective.required != 1) ||
            (objective.kind == MissionObjectiveKind::Elapsed && objective.duration_seconds == 0.0)) {
            return error(MissionError::InvalidDefinition, "invalid objective fields or duplicate objective ID");
        }
        std::vector<std::size_t> dependencies;
        for (const auto& prerequisite : objective.prerequisites) {
            const auto found = indices.find(prerequisite);
            if (found == indices.end() || std::find(dependencies.begin(), dependencies.end(), found->second) != dependencies.end()) {
                return error(MissionError::InvalidDefinition, "objective prerequisites must uniquely name earlier objectives");
            }
            dependencies.push_back(found->second);
        }
        indices.emplace(objective.id, index); entry.dependencies.push_back(std::move(dependencies));
        required = required || !objective.optional;
    }
    if (!required) { return error(MissionError::InvalidDefinition, "at least one objective must be required"); }
    for (const auto& reward : definition.rewards) {
        if (!valid_name(reward.adapter, limits.name_bytes) || !valid_text(reward.item, limits.name_bytes, false) || reward.quantity == 0) {
            return error(MissionError::InvalidDefinition, "invalid reward adapter, item or quantity");
        }
    }
    try { entry.fingerprint = definition_fingerprint(definition, limits); }
    catch (const core::BinaryException& exception) { return error(MissionError::InvalidDefinition, exception.what()); }
    const auto id = definition.id;
    entry.value = std::move(definition);
    state.definitions.emplace(id, std::move(entry));
    return {MissionError::None, 0, 1, "immutable mission definition admitted"};
}

const MissionDefinition* MissionService::definition(std::string_view id) const noexcept {
    const auto found = impl_->definitions.find(id);
    return found == impl_->definitions.end() ? nullptr : &found->second.value;
}

std::optional<std::uint64_t> MissionService::definition_hash(std::string_view id) const noexcept {
    const auto found = impl_->definitions.find(id);
    if (found == impl_->definitions.end()) { return std::nullopt; }
    return found->second.fingerprint;
}

std::vector<std::string> MissionService::definitions() const {
    std::vector<std::string> result;
    result.reserve(impl_->definitions.size());
    for (const auto& [id, entry] : impl_->definitions) { static_cast<void>(entry); result.push_back(id); }
    return result;
}

MissionResult MissionService::start(ecs::Entity actor, std::string_view mission) {
    auto& state = *impl_;
    if (state.busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    const auto found = state.definitions.find(mission);
    if (found == state.definitions.end()) { return error(MissionError::UnknownMission, "mission is not defined"); }
    if (!state.registry.alive(actor)) { return error(MissionError::InvalidActor, "actor generation is not live"); }
    const auto identity = state.registry.persistent_id(actor);
    if (!identity) { return error(MissionError::MissingIdentity, "assign a Registry persistent identity before starting"); }
    const auto& definition = found->second.value;
    std::size_t actor_runs{};
    for (const auto& [id, run] : state.runs) {
        static_cast<void>(id);
        if (run.persistent_actor != identity) { continue; }
        ++actor_runs;
        if (run.mission == mission && (!definition.repeatable || !terminal(run.status))) {
            return error(MissionError::AlreadyStarted, "mission already retained for this actor", run.id);
        }
    }
    for (const auto& prerequisite : definition.prerequisites) {
        if (!state.completed(identity, prerequisite)) { return error(MissionError::Prerequisite, "prerequisite has not completed: " + prerequisite); }
    }
    if (actor_runs >= state.limits.runs_per_actor || state.runs.size() >= state.limits.runs ||
        definition.rewards.size() > state.limits.receipts - state.receipts.size()) {
        return error(MissionError::Capacity, "retained run or receipt capacity reached");
    }
    if (state.next_run == std::numeric_limits<std::uint64_t>::max() ||
        definition.rewards.size() > std::numeric_limits<std::uint64_t>::max() - state.next_receipt) {
        return error(MissionError::Exhausted, "run or receipt identity exhausted");
    }
    MissionRun run;
    run.reason.reserve(state.limits.text_bytes);
    run.id = state.next_run; run.mission = definition.id; run.definition_revision = definition.revision;
    run.definition_fingerprint = found->second.fingerprint; run.actor = actor; run.persistent_actor = identity;
    for (const auto& objective : definition.objectives) { run.objectives.push_back({objective.id}); }
    std::map<MissionReceiptId, MissionReceipt> new_receipts;
    auto next_receipt = state.next_receipt;
    for (std::size_t index = 0; index < definition.rewards.size(); ++index) {
        const MissionReceiptId id{state.scope, next_receipt++};
        MissionReceipt receipt;
        receipt.id = id; receipt.run = run.id; receipt.reward_index = static_cast<std::uint32_t>(index);
        new_receipts.emplace(id, std::move(receipt)); run.rewards.push_back(id);
    }
    auto inserted = state.runs.emplace(run.id, std::move(run));
    state.receipts.merge(new_receipts);
    ++state.next_run; state.next_receipt = next_receipt;
    MutationGuard guard(state.busy);
    state.emit(MissionEventKind::Started, &inserted.first->second);
    state.settle(inserted.first->second);
    return {MissionError::None, inserted.first->first, 1, "mission started with reserved reward receipts"};
}

MissionResult MissionService::submit(const MissionInput& input) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    MutationGuard guard(impl_->busy);
    return impl_->admit(input, false);
}

MissionResult MissionService::cancel(std::uint64_t id, std::string_view reason) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    const auto found = impl_->runs.find(id);
    if (found == impl_->runs.end()) { return error(MissionError::UnknownRun, "run is unknown"); }
    if (found->second.status != MissionStatus::Active) { return error(MissionError::InvalidState, "only active objectives can be cancelled", id); }
    if (!valid_text(reason, impl_->limits.text_bytes)) { return error(MissionError::InvalidState, "invalid reason", id); }
    impl_->stop(found->second, MissionStatus::Cancelled, reason);
    return {MissionError::None, id, 1, "mission cancelled; no reward application was attempted"};
}

MissionResult MissionService::fail(std::uint64_t id, std::string_view reason) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    const auto found = impl_->runs.find(id);
    if (found == impl_->runs.end()) { return error(MissionError::UnknownRun, "run is unknown"); }
    if (found->second.status != MissionStatus::Active || !valid_text(reason, impl_->limits.text_bytes, false)) {
        return error(MissionError::InvalidState, "failure requires active objectives and a bounded reason", id);
    }
    impl_->stop(found->second, MissionStatus::Failed, reason);
    return {MissionError::None, id, 1, "mission failed"};
}

MissionResult MissionService::retry_rewards(std::uint64_t id) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    const auto found = impl_->runs.find(id);
    if (found == impl_->runs.end()) { return error(MissionError::UnknownRun, "run is unknown"); }
    if (found->second.status != MissionStatus::AwaitingRewards) { return error(MissionError::InvalidState, "run is not awaiting rewards", id); }
    std::size_t changed{};
    for (const auto receipt_id : found->second.rewards) {
        auto& receipt = impl_->receipts.at(receipt_id);
        if (receipt.status == MissionReceiptStatus::Applied) { continue; }
        receipt.status = MissionReceiptStatus::Pending; receipt.last_outcome = MissionRewardOutcome::Retry;
        receipt.retry_seconds = 0.0; ++changed;
    }
    return {MissionError::None, id, changed, "reward retry scheduled with original receipt IDs"};
}

MissionResult MissionService::rebind_actor(PersistentId identity, ecs::Entity replacement) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    if (!identity || !impl_->registry.alive(replacement) || impl_->registry.persistent_id(replacement) != identity) {
        return error(MissionError::InvalidActor, "replacement must own the exact persistent actor identity");
    }
    std::size_t changed{};
    for (auto& [id, run] : impl_->runs) {
        static_cast<void>(id);
        if (run.persistent_actor == identity && run.actor != replacement) {
            run.actor = replacement; ++changed; impl_->emit(MissionEventKind::ActorRebound, &run);
        }
    }
    return {MissionError::None, 0, changed, "persistent actor rebound to a live generation"};
}

MissionResult MissionService::resume_interactions(std::uint64_t cursor) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    auto* interactions = impl_->kernel.services().find<InteractionService>();
    if (!interactions) { return error(MissionError::MissingService, "interaction service is unavailable"); }
    if (!impl_->interaction_suspended) { return error(MissionError::InvalidState, "interaction bridge is already active"); }
    const auto batch = interactions->events_since(cursor, 1);
    if (cursor > batch.newest_sequence || batch.history_lost) { return error(MissionError::InvalidState, "cursor is outside retained source history"); }
    impl_->interaction_cursor = cursor; impl_->interaction_suspended = false;
    return {MissionError::None, 0, 1, "interaction source lifecycle acknowledged"};
}

MissionResult MissionService::register_reward_adapter(std::string name, MissionRewardAdapter adapter) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    if (!valid_name(name, impl_->limits.name_bytes) || !adapter || impl_->adapters.contains(name)) {
        return error(MissionError::InvalidState, "adapter needs a valid unused name and callable");
    }
    if (impl_->adapters.size() >= impl_->limits.adapters) { return error(MissionError::Capacity, "adapter capacity reached"); }
    impl_->adapters.emplace(std::move(name), std::move(adapter));
    return {MissionError::None, 0, 1, "reward adapter registered"};
}

MissionResult MissionService::unregister_reward_adapter(std::string_view name) {
    if (impl_->busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    const auto found = impl_->adapters.find(name);
    if (found == impl_->adapters.end()) { return error(MissionError::InvalidState, "adapter is not registered"); }
    impl_->adapters.erase(found);
    return {MissionError::None, 0, 1, "adapter removed; pending receipts remain retained"};
}

void MissionService::update(const core::Frame& frame) {
    auto& state = *impl_;
    if (state.busy || (state.had_frame && frame.tick <= state.tick)) { return; }
    state.counters.invalid_delta = !std::isfinite(frame.delta_seconds) || frame.delta_seconds < 0.0 ||
        frame.delta_seconds > state.limits.maximum_delta_seconds;
    if (state.counters.invalid_delta) { return; }
    MutationGuard guard(state.busy);
    state.had_frame = true; state.tick = frame.tick;
    state.poll_interactions();
    for (auto& [id, run] : state.runs) {
        static_cast<void>(id);
        if (run.status == MissionStatus::Active) { state.advance(run, frame.delta_seconds); }
    }
    state.apply_rewards(frame.delta_seconds);
}

std::optional<MissionRun> MissionService::inspect(std::uint64_t id) const {
    const auto found = impl_->runs.find(id);
    return found == impl_->runs.end() ? std::nullopt : std::optional<MissionRun>{found->second};
}

std::optional<MissionReceipt> MissionService::receipt(MissionReceiptId id) const {
    const auto found = impl_->receipts.find(id);
    return found == impl_->receipts.end() ? std::nullopt : std::optional<MissionReceipt>{found->second};
}

std::vector<MissionRun> MissionService::runs(PersistentId actor, std::size_t limit) const {
    std::vector<MissionRun> result;
    for (const auto& [id, run] : impl_->runs) {
        static_cast<void>(id);
        if (result.size() >= limit) { break; }
        if (!actor || run.persistent_actor == actor) { result.push_back(run); }
    }
    return result;
}

bool MissionService::completed(PersistentId actor, std::string_view mission) const noexcept {
    return impl_->completed(actor, mission);
}

MissionHistory MissionService::events_since(std::uint64_t cursor, std::size_t limit) const {
    MissionHistory result;
    result.next_cursor = cursor; result.newest_sequence = impl_->event_sequence;
    result.history_lost = cursor > impl_->event_sequence || cursor < impl_->last_history_gap ||
        (impl_->history.empty() ? cursor < impl_->event_sequence : cursor < impl_->history.front().sequence - 1);
    for (const auto& event : impl_->history) {
        if (result.events.size() >= limit) { break; }
        if (event.sequence > cursor) { result.events.push_back(event); result.next_cursor = event.sequence; }
    }
    return result;
}

MissionStatistics MissionService::statistics() const noexcept {
    auto result = impl_->counters;
    result.definitions = impl_->definitions.size(); result.runs = impl_->runs.size();
    result.accepted_events = impl_->accepted.size(); result.receipts = impl_->receipts.size();
    result.interaction_cursor = impl_->interaction_cursor; result.interaction_suspended = impl_->interaction_suspended;
    for (const auto& [id, run] : impl_->runs) {
        static_cast<void>(id);
        result.active += run.status == MissionStatus::Active ? 1U : 0U;
        result.awaiting_rewards += run.status == MissionStatus::AwaitingRewards ? 1U : 0U;
        result.completed += run.status == MissionStatus::Completed ? 1U : 0U;
    }
    for (const auto& [id, receipt] : impl_->receipts) {
        static_cast<void>(id);
        const auto run = impl_->runs.find(receipt.run);
        if (run == impl_->runs.end() || run->second.status != MissionStatus::AwaitingRewards) { continue; }
        result.pending_rewards += receipt.status == MissionReceiptStatus::Pending ? 1U : 0U;
        result.rejected_rewards += receipt.status == MissionReceiptStatus::Rejected ? 1U : 0U;
    }
    return result;
}

const MissionLimits& MissionService::limits() const noexcept { return impl_->limits; }

MissionResult MissionService::encode_snapshot(std::vector<std::byte>& output) const {
    const auto& state = *impl_;
    if (state.busy) { return error(MissionError::Busy, "cannot snapshot inside a mutation or adapter"); }
    try {
        core::ByteWriter writer(state.limits.snapshot_bytes);
        writer.write_bytes("GEMSN001"); writer.write_u16(1); writer.write_u64(state.scope);
        writer.write_u64(state.next_run); writer.write_u64(state.next_receipt); writer.write_u64(state.next_bridge);
        writer.write_u64(state.event_sequence); writer.write_u64(state.interaction_cursor);
        writer.write_u32(static_cast<std::uint32_t>(state.runs.size()));
        for (const auto& [id, run] : state.runs) {
            writer.write_u64(id); writer.write_string(run.mission); writer.write_u64(run.definition_revision);
            writer.write_u64(run.definition_fingerprint); writer.write_u64(run.persistent_actor.value);
            writer.write_u8(static_cast<std::uint8_t>(run.status)); writer.write_f64(run.elapsed_seconds);
            writer.write_string(run.reason); writer.write_u32(static_cast<std::uint32_t>(run.objectives.size()));
            for (const auto& objective : run.objectives) {
                writer.write_string(objective.id); writer.write_u8(static_cast<std::uint8_t>(objective.status));
                writer.write_u64(objective.count); writer.write_f64(objective.active_seconds); writer.write_f64(objective.dwell_seconds);
            }
            writer.write_u32(static_cast<std::uint32_t>(run.rewards.size()));
            for (const auto id_value : run.rewards) { writer.write_u64(id_value.sequence); }
        }
        writer.write_u32(static_cast<std::uint32_t>(state.receipts.size()));
        for (const auto& [id, receipt] : state.receipts) {
            writer.write_u64(id.sequence); writer.write_u64(receipt.run); writer.write_u32(receipt.reward_index);
            writer.write_u8(static_cast<std::uint8_t>(receipt.status)); writer.write_u8(static_cast<std::uint8_t>(receipt.last_outcome));
            writer.write_u64(receipt.attempts); writer.write_f64(receipt.retry_seconds); writer.write_string(receipt.message);
        }
        writer.write_u32(static_cast<std::uint32_t>(state.accepted.size()));
        for (const auto& [id, event] : state.accepted) {
            writer.write_u64(id.producer); writer.write_u64(id.sequence); writer.write_u64(event.actor.value);
            writer.write_u8(static_cast<std::uint8_t>(event.kind)); writer.write_string(event.key);
            writer.write_u64(event.target.value); writer.write_u64(event.amount);
        }
        writer.write_u32(core::crc32(writer.bytes()));
        MissionResult result{MissionError::None, 0, writer.size(), "mission snapshot encoded"};
        output = writer.take();
        return result;
    } catch (const std::exception& exception) { return error(MissionError::InvalidSnapshot, exception.what()); }
}

MissionResult MissionService::restore_snapshot(std::span<const std::byte> bytes) {
    auto& state = *impl_;
    if (state.busy) { return error(MissionError::Busy, "mission mutation is reentrant"); }
    if (!state.runs.empty() || !state.receipts.empty() || !state.accepted.empty()) {
        return error(MissionError::InvalidState, "restore requires an empty service to prevent live receipt rollback");
    }
    if (bytes.size() < 4 || bytes.size() > state.limits.snapshot_bytes) { return error(MissionError::InvalidSnapshot, "snapshot byte bounds exceeded"); }
    try {
        core::BinaryLimits binary_limits;
        binary_limits.max_total_bytes = state.limits.snapshot_bytes;
        binary_limits.max_string_bytes = state.limits.text_bytes;
        core::ByteReader checksum(bytes.last(4));
        const auto payload = bytes.first(bytes.size() - 4);
        if (checksum.read_u32() != core::crc32(payload)) { return error(MissionError::InvalidSnapshot, "snapshot checksum mismatch"); }
        core::ByteReader reader(payload, core::ByteOrder::little, binary_limits);
        auto require = [](bool condition, const char* message) {
            if (!condition) { throw std::runtime_error(message); }
        };
        auto count = [&](std::size_t limit) {
            const auto value = static_cast<std::size_t>(reader.read_u32());
            require(value <= limit && value <= reader.remaining(), "snapshot collection exceeds bounds");
            return value;
        };
        reader.expect("GEMSN001");
        require(reader.read_u16() == 1, "unsupported mission snapshot version");
        require(reader.read_u64() == state.scope, "receipt scope differs from configured save lineage");
        const auto next_run = reader.read_u64(); const auto next_receipt = reader.read_u64();
        const auto next_bridge = reader.read_u64(); const auto event_sequence = reader.read_u64();
        const auto interaction_cursor = reader.read_u64();
        require(next_run != 0 && next_receipt != 0 && next_bridge != 0, "invalid next identity");
        decltype(state.runs) restored_runs;
        decltype(state.receipts) restored_receipts;
        decltype(state.accepted) restored_events;
        std::map<PersistentId, std::size_t> actor_counts;
        const auto run_count = count(state.limits.runs);
        for (std::size_t index = 0; index < run_count; ++index) {
            MissionRun run;
            run.id = reader.read_u64(); run.mission = reader.read_string(); run.definition_revision = reader.read_u64();
            run.definition_fingerprint = reader.read_u64(); run.persistent_actor.value = reader.read_u64();
            run.status = static_cast<MissionStatus>(reader.read_u8()); run.elapsed_seconds = reader.read_f64();
            run.reason = reader.read_string();
            require(run.id != 0 && run.id < next_run && run.persistent_actor.valid() &&
                run.status <= MissionStatus::Cancelled && finite_time(run.elapsed_seconds) &&
                valid_text(run.reason, state.limits.text_bytes), "invalid run state");
            run.reason.reserve(state.limits.text_bytes);
            const auto found = state.definitions.find(run.mission);
            if (found == state.definitions.end() || found->second.value.revision != run.definition_revision ||
                found->second.fingerprint != run.definition_fingerprint) {
                return error(MissionError::DefinitionMismatch, "snapshot requires the exact installed immutable mission definition", run.id);
            }
            const auto& definition = found->second.value;
            if (run.status != MissionStatus::Failed && run.status != MissionStatus::Cancelled) {
                require(definition.timeout_seconds == 0.0 || run.elapsed_seconds < definition.timeout_seconds, "live or successful run exceeded its deadline");
            }
            run.actor = state.registry.find(run.persistent_actor);
            require(terminal(run.status) || state.registry.alive(run.actor), "active or pending actor identity is unavailable");
            require(++actor_counts[run.persistent_actor] <= state.limits.runs_per_actor, "per-actor run retention exceeded");
            const auto objective_count = count(state.limits.objectives_per_mission);
            require(objective_count == definition.objectives.size(), "objective shape differs from definition");
            bool all_required = true;
            for (std::size_t objective_index = 0; objective_index < objective_count; ++objective_index) {
                MissionObjectiveProgress progress;
                progress.id = reader.read_string(); progress.status = static_cast<MissionObjectiveStatus>(reader.read_u8());
                progress.count = reader.read_u64(); progress.active_seconds = reader.read_f64(); progress.dwell_seconds = reader.read_f64();
                const auto& authored = definition.objectives[objective_index];
                require(progress.id == authored.id && progress.status <= MissionObjectiveStatus::Failed &&
                    progress.count <= authored.required && finite_time(progress.active_seconds) && finite_time(progress.dwell_seconds) &&
                    progress.active_seconds <= run.elapsed_seconds && progress.dwell_seconds <= progress.active_seconds,
                    "invalid objective progress or timing");
                require((progress.status == MissionObjectiveStatus::Completed) == (progress.count == authored.required), "objective completion/count mismatch");
                if (progress.status == MissionObjectiveStatus::Locked) {
                    require(progress.count == 0 && progress.active_seconds == 0.0 && progress.dwell_seconds == 0.0, "locked objective has progress");
                }
                bool dependencies_ready = true;
                bool dependency_failed = false;
                for (const auto dependency : found->second.dependencies[objective_index]) {
                    dependencies_ready = dependencies_ready && run.objectives[dependency].status == MissionObjectiveStatus::Completed;
                    dependency_failed = dependency_failed || run.objectives[dependency].status == MissionObjectiveStatus::Failed;
                }
                if (progress.status == MissionObjectiveStatus::Active || progress.status == MissionObjectiveStatus::Completed) {
                    require(dependencies_ready, "objective activated before its prerequisites");
                }
                if (run.status == MissionStatus::Active) {
                    require(!(progress.status == MissionObjectiveStatus::Locked && dependencies_ready), "active run has unactivated ready objective");
                    require(!(progress.status == MissionObjectiveStatus::Locked && dependency_failed), "active run retained a failed dependency");
                    require(authored.optional || progress.status != MissionObjectiveStatus::Failed, "active run has failed required objective");
                }
                if (authored.kind != MissionObjectiveKind::Location) { require(progress.dwell_seconds == 0.0, "non-location objective has dwell time"); }
                if (progress.status == MissionObjectiveStatus::Completed) {
                    require(authored.kind != MissionObjectiveKind::Elapsed || progress.active_seconds >= authored.duration_seconds, "elapsed objective completed too early");
                    require(authored.kind != MissionObjectiveKind::Location || progress.dwell_seconds >= authored.dwell_seconds, "location objective completed before dwell threshold");
                }
                if (progress.status == MissionObjectiveStatus::Active || progress.status == MissionObjectiveStatus::Completed) {
                    require(authored.timeout_seconds == 0.0 || progress.active_seconds < authored.timeout_seconds, "objective exceeded its deadline");
                }
                if (!authored.optional) { all_required = all_required && progress.status == MissionObjectiveStatus::Completed; }
                run.objectives.push_back(std::move(progress));
            }
            require(run.status != MissionStatus::Active || !all_required, "active run already completed its required objectives");
            require((run.status != MissionStatus::Failed && run.status != MissionStatus::Cancelled) || !all_required, "failed or cancelled run completed all required objectives");
            require((run.status != MissionStatus::Completed && run.status != MissionStatus::AwaitingRewards) || all_required,
                    "terminal success lacks required objective completion");
            const auto reward_count = count(state.limits.rewards_per_mission);
            require(reward_count == definition.rewards.size(), "reward shape differs from definition");
            for (std::size_t reward_index = 0; reward_index < reward_count; ++reward_index) {
                const auto sequence = reader.read_u64();
                require(sequence != 0 && sequence < next_receipt, "invalid reward receipt identity");
                run.rewards.push_back({state.scope, sequence});
            }
            const auto id = run.id;
            require(restored_runs.emplace(id, std::move(run)).second, "duplicate mission run identity");
        }
        const auto receipt_count = count(state.limits.receipts);
        for (std::size_t index = 0; index < receipt_count; ++index) {
            MissionReceipt receipt;
            receipt.id = {state.scope, reader.read_u64()}; receipt.run = reader.read_u64(); receipt.reward_index = reader.read_u32();
            receipt.status = static_cast<MissionReceiptStatus>(reader.read_u8()); receipt.last_outcome = static_cast<MissionRewardOutcome>(reader.read_u8());
            receipt.attempts = reader.read_u64(); receipt.retry_seconds = reader.read_f64(); receipt.message = reader.read_string();
            require(receipt.id.sequence != 0 && receipt.id.sequence < next_receipt && receipt.status <= MissionReceiptStatus::Rejected &&
                receipt.last_outcome <= MissionRewardOutcome::Rejected && finite_time(receipt.retry_seconds) &&
                valid_text(receipt.message, state.limits.text_bytes), "invalid reward receipt fields");
            const auto run = restored_runs.find(receipt.run);
            require(run != restored_runs.end() && receipt.reward_index < run->second.rewards.size() &&
                run->second.rewards[receipt.reward_index] == receipt.id, "receipt does not match its owning run and reward");
            const bool applied = receipt.last_outcome == MissionRewardOutcome::Applied || receipt.last_outcome == MissionRewardOutcome::AlreadyApplied;
            require((receipt.status == MissionReceiptStatus::Applied) == applied &&
                (receipt.status == MissionReceiptStatus::Rejected) == (receipt.last_outcome == MissionRewardOutcome::Rejected), "receipt state/outcome mismatch");
            require(receipt.status == MissionReceiptStatus::Pending || receipt.attempts > 0, "terminal receipt has never been attempted");
            if (run->second.status == MissionStatus::Active || run->second.status == MissionStatus::Failed || run->second.status == MissionStatus::Cancelled) {
                require(receipt.attempts == 0 && receipt.status == MissionReceiptStatus::Pending, "inactive reward was applied before objective completion");
            }
            const auto id = receipt.id;
            require(restored_receipts.emplace(id, std::move(receipt)).second, "duplicate receipt identity");
        }
        std::set<std::pair<PersistentId, std::string>> seen_missions;
        std::set<std::pair<PersistentId, std::string>> live_missions;
        std::set<std::pair<PersistentId, std::string>> completed_missions;
        for (const auto& [id, run] : restored_runs) {
            bool all_applied = true;
            for (const auto receipt_id : run.rewards) {
                const auto receipt = restored_receipts.find(receipt_id);
                require(receipt != restored_receipts.end() && receipt->second.run == id, "missing or shared reward receipt");
                all_applied = all_applied && receipt->second.status == MissionReceiptStatus::Applied;
            }
            require(run.status != MissionStatus::Completed || all_applied, "completed mission has unapplied rewards");
            require(run.status != MissionStatus::AwaitingRewards || !all_applied, "pending mission has no pending reward");
            const auto& definition = state.definitions.at(run.mission).value;
            for (const auto& prerequisite : definition.prerequisites) {
                require(completed_missions.contains({run.persistent_actor, prerequisite}), "snapshot mission prerequisite has no prior completed run");
            }
            const auto key = std::make_pair(run.persistent_actor, run.mission);
            const bool first = seen_missions.insert(key).second;
            require(definition.repeatable || first, "nonrepeatable mission has multiple runs");
            if (!terminal(run.status)) { require(live_missions.insert(key).second, "mission has multiple live runs for one actor"); }
            if (run.status == MissionStatus::Completed) { completed_missions.insert(key); }
        }
        const auto event_count = count(state.limits.accepted_events);
        for (std::size_t index = 0; index < event_count; ++index) {
            const MissionEventId id{reader.read_u64(), reader.read_u64()};
            Impl::AcceptedInput event;
            event.actor.value = reader.read_u64(); event.kind = static_cast<MissionObjectiveKind>(reader.read_u8());
            event.key = reader.read_string(); event.target.value = reader.read_u64(); event.amount = reader.read_u64();
            require(static_cast<bool>(id) && event.actor.valid() && event.kind <= MissionObjectiveKind::Dialogue &&
                valid_text(event.key, state.limits.name_bytes) && event.amount != 0, "invalid accepted event");
            if (id.producer == interaction_producer) { require(id.sequence < next_bridge && event.kind == MissionObjectiveKind::Interaction, "invalid interaction bridge event identity"); }
            require(restored_events.emplace(id, std::move(event)).second, "duplicate accepted event identity");
        }
        reader.require_end();
        MissionResult result{MissionError::None, 0, restored_runs.size(), "snapshot restored; acknowledge interaction source before resuming its bridge"};
        state.runs.swap(restored_runs); state.receipts.swap(restored_receipts); state.accepted.swap(restored_events);
        state.next_run = next_run; state.next_receipt = next_receipt; state.next_bridge = next_bridge;
        state.event_sequence = event_sequence; state.interaction_cursor = interaction_cursor;
        state.last_history_gap = event_sequence;
        state.reward_cursor = 0; state.had_frame = false; state.tick = 0; state.history.clear();
        state.interaction_suspended = true;
        return result;
    } catch (const std::exception& exception) { return error(MissionError::InvalidSnapshot, exception.what()); }
}

MissionResult MissionService::save(persistence::SaveJournal& journal, std::string_view key) const {
    std::vector<std::byte> bytes;
    const auto encoded = encode_snapshot(bytes);
    if (!encoded) { return encoded; }
    persistence::TransactionId transaction{};
    auto result = journal.begin(transaction, "mission snapshot");
    if (!result) { return error(MissionError::JournalFailure, result.message); }
    result = journal.put(transaction, key, bytes, mission_value_type, 1);
    if (result) { result = journal.commit(transaction); }
    if (!result) {
        if (journal.inspect().active_transaction == transaction) { static_cast<void>(journal.abort(transaction)); }
        return error(MissionError::JournalFailure, result.message);
    }
    return {MissionError::None, 0, bytes.size(), "mission state committed to journal; adapter persistence remains independent"};
}

MissionResult MissionService::load(const persistence::SaveJournal& journal, std::string_view key) {
    const auto* value = journal.find(key);
    if (!value || value->type != mission_value_type || value->version != 1) { return error(MissionError::JournalFailure, "missing or incompatible mission journal value"); }
    return restore_snapshot(value->bytes);
}

namespace {

core::CommandResult command_result(const MissionResult& result) {
    return {static_cast<bool>(result), std::string(describe(result.error)) + " run=" + std::to_string(result.run) +
        " affected=" + std::to_string(result.affected) + " " + result.message};
}

std::optional<MissionObjectiveKind> objective_kind(std::string_view name) {
    constexpr std::array names{"custom", "interaction", "defeat", "collect", "dialogue", "location", "elapsed"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (name == names[index]) { return static_cast<MissionObjectiveKind>(index); }
    }
    return std::nullopt;
}

ecs::Entity command_actor(core::Kernel& kernel, std::string_view name) {
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) { return {}; }
    const auto parsed = ecs::parse_entity(name);
    if (parsed && registry->alive(*parsed)) { return *parsed; }
    return registry->find_name(name);
}

} // namespace

void register_mission(core::Kernel& kernel) {
    if (kernel.services().find<MissionService>()) { return; }
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) { kernel.diagnose(core::Diagnostic::Severity::Error, "gameplay.mission", "Registry is required"); return; }
    const auto configured_scope = kernel.config().integer("mission.receipt_scope", 1);
    if (configured_scope <= 0) { kernel.diagnose(core::Diagnostic::Severity::Error, "gameplay.mission", "receipt scope must be positive"); return; }
    auto* service = &kernel.services().emplace<MissionService>(kernel, *registry, MissionLimits{}, static_cast<std::uint64_t>(configured_scope));
    static_cast<void>(kernel.register_module({"gameplay.mission", "Bounded mission objectives and idempotent reward receipts", 1}));
    std::vector<std::string> after;
    if (kernel.services().find<InteractionService>()) { after.emplace_back("gameplay.interaction.update"); }
    static_cast<void>(kernel.add_system({"gameplay.mission.update", core::Phase::Visibility, -6900, std::move(after)},
        [service](core::Kernel& host, const core::Frame& frame) {
            service->update(frame);
            const auto stats = service->statistics();
            host.metric("mission.active", static_cast<double>(stats.active));
            host.metric("mission.awaiting_rewards", static_cast<double>(stats.awaiting_rewards));
            host.metric("mission.completed", static_cast<double>(stats.completed));
            host.metric("mission.accepted_events", static_cast<double>(stats.accepted_events));
            host.metric("mission.input_backpressure", stats.input_backpressure ? 1.0 : 0.0);
        }));
    static_cast<void>(kernel.add_command("mission.define", [service](core::Kernel&, core::CommandArgs args) {
        if (args.size() < 3 || args.size() > 7) { return core::CommandResult{false, "mission.define ID KIND KEY COUNT [ITEM QUANTITY] | ID elapsed SECONDS | ID location X Y Z RADIUS [DWELL]"}; }
        const auto kind = objective_kind(args[1]);
        if (!kind) { return core::CommandResult{false, "unknown objective kind"}; }
        MissionDefinition definition; definition.id = args[0]; definition.label = args[0];
        MissionObjectiveDefinition objective; objective.id = "primary"; objective.kind = *kind;
        if (*kind == MissionObjectiveKind::Elapsed) {
            if (args.size() != 3 || !parse_number(args[2], objective.duration_seconds)) { return core::CommandResult{false, "elapsed needs finite seconds"}; }
        } else if (*kind == MissionObjectiveKind::Location) {
            if ((args.size() != 6 && args.size() != 7) || !parse_number(args[2], objective.absolute_position.x) ||
                !parse_number(args[3], objective.absolute_position.y) || !parse_number(args[4], objective.absolute_position.z) ||
                !parse_number(args[5], objective.radius) || (args.size() == 7 && !parse_number(args[6], objective.dwell_seconds))) {
                return core::CommandResult{false, "location needs absolute x y z radius and optional dwell seconds"};
            }
        } else {
            if ((args.size() != 4 && args.size() != 6) || !parse_number(args[3], objective.required)) { return core::CommandResult{false, "event objective needs key and count, optionally item and quantity"}; }
            objective.event_key = args[2] == "*" ? "" : args[2];
            if (args.size() == 6) {
                MissionReward reward{"inventory", args[4], 1};
                if (!parse_number(args[5], reward.quantity)) { return core::CommandResult{false, "invalid reward quantity"}; }
                definition.rewards.push_back(std::move(reward));
            }
        }
        definition.objectives.push_back(std::move(objective));
        return command_result(service->define(std::move(definition)));
    }, "Define an immutable mission using typed events, elapsed time or absolute location"));
    static_cast<void>(kernel.add_command("mission.start", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) { return core::CommandResult{false, "mission.start ACTOR MISSION"}; }
        return command_result(service->start(command_actor(host, args[0]), args[1]));
    }, "Start a mission for a live actor with a persistent Registry identity"));
    static_cast<void>(kernel.add_command("mission.event", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 6 && args.size() != 7) { return core::CommandResult{false, "mission.event ACTOR PRODUCER SEQUENCE KIND KEY AMOUNT [TARGET_ID]"}; }
        MissionInput input; input.actor = command_actor(host, args[0]); input.key = args[4];
        const auto kind = objective_kind(args[3]);
        if (!kind || !parse_number(args[1], input.id.producer) || !parse_number(args[2], input.id.sequence) ||
            !parse_number(args[5], input.amount) || (args.size() == 7 && !parse_number(args[6], input.target.value))) {
            return core::CommandResult{false, "invalid typed event or identity"};
        }
        input.kind = *kind;
        return command_result(service->submit(input));
    }, "Admit a typed event with retained producer/sequence deduplication"));
    static_cast<void>(kernel.add_command("mission.control", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t id{};
        if (args.size() < 2 || args.size() > 3 || !parse_number(args[0], id)) { return core::CommandResult{false, "mission.control RUN cancel|fail|retry [REASON]"}; }
        if (args[1] == "retry" && args.size() == 2) { return command_result(service->retry_rewards(id)); }
        const std::string_view reason = args.size() == 3 ? std::string_view(args[2]) : "operator request";
        if (args[1] == "cancel") { return command_result(service->cancel(id, reason)); }
        if (args[1] == "fail") { return command_result(service->fail(id, reason)); }
        return core::CommandResult{false, "unknown mission control"};
    }, "Cancel or fail active objectives, or retry unapplied reward receipts"));
    static_cast<void>(kernel.add_command("mission.inspect", [service](core::Kernel&, core::CommandArgs args) {
        std::ostringstream output;
        if (args.empty()) {
            const auto stats = service->statistics();
            output << "definitions=" << stats.definitions << " runs=" << stats.runs << " active=" << stats.active
                   << " awaiting=" << stats.awaiting_rewards << " completed=" << stats.completed << " events=" << stats.accepted_events
                   << " receipts=" << stats.receipts << " rejected_rewards=" << stats.rejected_rewards
                   << " interaction_cursor=" << stats.interaction_cursor << " interaction_suspended=" << stats.interaction_suspended;
            for (const auto& id : service->definitions()) { output << '\n' << id; }
        } else {
            std::uint64_t id{};
            if (args.size() != 1 || !parse_number(args[0], id)) { return core::CommandResult{false, "mission.inspect [RUN]"}; }
            const auto run = service->inspect(id);
            if (!run) { return core::CommandResult{false, "unknown mission run"}; }
            output << run->mission << ' ' << describe(run->status) << " actor=" << ecs::format_entity(run->actor) << " elapsed=" << run->elapsed_seconds;
            for (const auto& objective : run->objectives) { output << '\n' << objective.id << " state=" << static_cast<unsigned>(objective.status) << " count=" << objective.count; }
            for (const auto receipt_id : run->rewards) {
                const auto receipt = service->receipt(receipt_id);
                if (receipt) { output << '\n' << "receipt=" << receipt_id.scope << ':' << receipt_id.sequence << " state=" << static_cast<unsigned>(receipt->status) << " attempts=" << receipt->attempts << ' ' << receipt->message; }
            }
        }
        return core::CommandResult{true, output.str()};
    }, "Inspect retained definitions, objective progress, reward outcomes and bridge state"));
    static_cast<void>(kernel.add_command("mission.history", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t cursor{};
        if (args.size() > 1 || (!args.empty() && !parse_number(args[0], cursor))) { return core::CommandResult{false, "mission.history [CURSOR]"}; }
        const auto history = service->events_since(cursor, 64);
        std::ostringstream output;
        output << "cursor=" << history.next_cursor << " newest=" << history.newest_sequence << " lost=" << history.history_lost;
        for (const auto& event : history.events) { output << '\n' << event.sequence << ' ' << describe(event.kind) << " run=" << event.run << ' ' << event.objective << ' ' << event.message; }
        return core::CommandResult{true, output.str()};
    }, "Read independent mission event history for quests, dialogue and district logic"));
    static_cast<void>(kernel.add_command("mission.persist", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2 || (args[0] != "save" && args[0] != "load")) { return core::CommandResult{false, "mission.persist save|load KEY"}; }
        auto* journal = host.services().find<persistence::SaveJournal>();
        if (!journal) { return core::CommandResult{false, "SaveJournal is unavailable"}; }
        return command_result(args[0] == "save" ? service->save(*journal, args[1]) : service->load(*journal, args[1]));
    }, "Persist bounded mission snapshots through an already-open SaveJournal"));
    static_cast<void>(kernel.add_command("mission.interactions", [service](core::Kernel&, core::CommandArgs args) {
        std::uint64_t cursor{};
        if (args.size() != 1 || !parse_number(args[0], cursor)) { return core::CommandResult{false, "mission.interactions RESUME_CURSOR"}; }
        return command_result(service->resume_interactions(cursor));
    }, "Acknowledge the interaction source lifetime after restoration or history loss"));
    static_cast<void>(kernel.add_command("mission.rebind", [service](core::Kernel& host, core::CommandArgs args) {
        PersistentId identity;
        if (args.size() != 2 || !parse_number(args[0], identity.value)) { return core::CommandResult{false, "mission.rebind PERSISTENT_ID ENTITY"}; }
        return command_result(service->rebind_actor(identity, command_actor(host, args[1])));
    }, "Explicitly rebind retained actor identity to its new live Registry generation"));
}

} // namespace ge::gameplay
