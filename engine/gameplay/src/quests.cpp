#include <ge/gameplay/quests.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/gameplay/dialogue.hpp>
#include <ge/gameplay/inventory.hpp>
#include <ge/persistence/journal.hpp>
#include <ge/text/localization.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::gameplay {
namespace {
constexpr std::uint32_t quest_value_type = 0x51555354U;
constexpr std::array<std::string_view, 4> provider_names{
    "quest.available", "quest.active", "quest.completed", "quest.failed"};
using QuestKey = std::pair<std::uint64_t, std::string>;

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) { ++value; }
}
bool terminal(QuestStatus status) noexcept {
    return status == QuestStatus::Completed || status == QuestStatus::Failed || status == QuestStatus::Abandoned;
}
bool text_valid(std::string_view value, std::size_t maximum) {
    return value.size() <= maximum && value.find('\0') == std::string_view::npos && core::valid_utf8(value);
}
bool name_valid(std::string_view value, std::size_t maximum) {
    return !value.empty() && text_valid(value, maximum) &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte > 32U && byte != 127U;
        });
}
QuestResult failure(QuestError error, std::string message) {
    return {error, 0, std::move(message)};
}
struct Guard {
    bool& busy;
    explicit Guard(bool& flag) : busy(flag) { busy = true; }
    ~Guard() { busy = false; }
};
bool parse_u64(std::string_view value, std::uint64_t& destination) {
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) { return false; }
    destination = parsed;
    return true;
}
std::uint64_t progress_fingerprint(const MissionRun& run, std::size_t maximum) {
    core::ByteWriter out(maximum);
    out.write_u8(static_cast<std::uint8_t>(run.status)); out.write_u64(core::fnv1a64(run.reason));
    for (const auto& objective : run.objectives) {
        out.write_string(objective.id); out.write_u8(static_cast<std::uint8_t>(objective.status));
        out.write_u64(objective.count); out.write_f64(objective.active_seconds); out.write_f64(objective.dwell_seconds);
    }
    return core::fnv1a64(out.bytes());
}
QuestStageStatus stage_status(MissionStatus status) noexcept {
    switch (status) {
    case MissionStatus::Active: return QuestStageStatus::Active;
    case MissionStatus::AwaitingRewards: return QuestStageStatus::AwaitingRewards;
    case MissionStatus::Completed: return QuestStageStatus::Completed;
    case MissionStatus::Failed: return QuestStageStatus::Failed;
    case MissionStatus::Cancelled: return QuestStageStatus::Cancelled;
    }
    return QuestStageStatus::MissingRun;
}
core::CommandResult command_result(const QuestResult& result) {
    return {static_cast<bool>(result), std::string(describe(result.error)) + ": " + result.message};
}
ecs::Entity command_actor(core::Kernel& kernel, std::string_view value) {
    auto& registry = kernel.services().get<ecs::Registry>();
    if (const auto parsed = ecs::parse_entity(value); parsed && registry.alive(*parsed)) { return *parsed; }
    return registry.find_name(value);
}
}

std::string_view describe(QuestError value) noexcept {
    constexpr std::array names{"none", "invalid_definition", "duplicate", "unknown_quest", "invalid_actor",
        "missing_identity", "prerequisite", "already_accepted", "capacity", "unknown_record", "invalid_state",
        "missing_service", "mission_conflict", "busy", "exhausted", "invalid_snapshot", "definition_mismatch",
        "partial_snapshot", "journal_failure", "invalid_argument"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}
std::string_view describe(QuestStatus value) noexcept {
    constexpr std::array names{"locked", "available", "active", "awaiting_rewards", "suspended", "completed", "failed", "abandoned"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}
std::string_view describe(QuestStageStatus value) noexcept {
    constexpr std::array names{"locked", "ready", "active", "awaiting_rewards", "completed", "failed", "cancelled", "missing_run"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "unknown";
}

struct QuestService::Impl {
    struct Definition {
        QuestDefinition value;
        std::uint64_t fingerprint{};
        std::size_t order{};
        std::vector<std::vector<std::size_t>> dependencies;
        std::vector<std::uint64_t> mission_fingerprints;
        std::vector<std::uint64_t> mission_revisions;
    };
    struct Work { QuestRecord* record{}; std::size_t stage{}; };
    core::Kernel& kernel;
    ecs::Registry& registry;
    QuestLimits limits;
    std::uint64_t lineage{};
    std::map<std::string, Definition, std::less<>> definitions;
    std::map<QuestKey, QuestRecord> records;
    std::vector<Work> work;
    std::size_t work_cursor{};
    std::deque<QuestEvent> history;
    QuestStatistics counters;
    std::uint64_t sequence{};
    std::uint64_t history_gap{};
    std::uint64_t source_cursor{};
    std::uint64_t tick{};
    bool had_frame{};
    bool busy{};
    bool stopped{};
    std::array<bool, 4> installed{};
    std::shared_ptr<int> lifetime{std::make_shared<int>(0)};

    Impl(core::Kernel& host, ecs::Registry& entities, QuestLimits bounds, std::uint64_t scope)
        : kernel(host), registry(entities), limits(bounds), lineage(scope) {}
    QuestRecord* find(PersistentId actor, std::string_view quest) {
        const auto found = records.find({actor.value, std::string(quest)});
        return found == records.end() ? nullptr : &found->second;
    }
    const QuestRecord* find(PersistentId actor, std::string_view quest) const {
        const auto found = records.find({actor.value, std::string(quest)});
        return found == records.end() ? nullptr : &found->second;
    }
    void emit(QuestEventKind kind, const QuestRecord* record = nullptr,
              const QuestStageProgress* stage = nullptr, std::string_view message = {}) noexcept {
        if (sequence == std::numeric_limits<std::uint64_t>::max()) {
            history_gap = sequence; increment(counters.overwritten_history); return;
        }
        const auto ordinal = ++sequence;
        bool retained{};
        try {
            QuestEvent event;
            event.sequence = ordinal; event.tick = tick; event.kind = kind;
            event.message = message.substr(0, limits.text_bytes);
            if (record) {
                event.persistent_actor = record->persistent_actor; event.quest = record->quest; event.status = record->status;
            }
            if (stage) { event.stage = stage->id; event.mission_run = stage->mission_run; }
            history.push_back(event); retained = true;
            if (history.size() > limits.history) { history.pop_front(); increment(counters.overwritten_history); }
            if (auto* hub = kernel.services().find<core::SignalHub>()) {
                const auto signal = hub->channel<QuestEvent>("gameplay.quests.changed");
                const auto bytes = event.quest.size() + event.stage.size() + event.message.size();
                if (!hub->enqueue(signal, std::move(event), bytes)) { increment(counters.lost_signals); }
            }
        } catch (...) {
            if (!retained) { history_gap = ordinal; increment(counters.overwritten_history); }
            increment(counters.lost_signals);
        }
    }
    void touch(QuestRecord& record) noexcept {
        increment(record.revision); record.changed_tick = tick;
    }
    void transition(QuestRecord& record, QuestStatus status, std::string_view reason) {
        if (record.status == status) { return; }
        const auto previous = record.status;
        record.reason.assign(reason.substr(0, limits.text_bytes)); record.status = status; touch(record);
        QuestEventKind kind = QuestEventKind::Resumed;
        if (status == QuestStatus::Completed) { kind = QuestEventKind::Completed; record.tracked = false; }
        if (status == QuestStatus::Failed) { kind = QuestEventKind::Failed; record.stop_owned = true; record.tracked = false; }
        if (status == QuestStatus::Suspended) { kind = QuestEventKind::Suspended; }
        if (status == QuestStatus::Abandoned) { kind = QuestEventKind::Abandoned; record.tracked = false; }
        if (previous != QuestStatus::Suspended && kind == QuestEventKind::Resumed) { kind = QuestEventKind::StageChanged; }
        emit(kind, &record, nullptr, record.reason);
    }
    bool ready(const QuestRecord& record, const Definition& definition, std::size_t index) const {
        return std::all_of(definition.dependencies[index].begin(), definition.dependencies[index].end(),
            [&](std::size_t dependency) { return record.stages[dependency].status == QuestStageStatus::Completed; });
    }
    bool matches(const QuestRecord& record, const QuestStageProgress& stage, const MissionRun& run) const noexcept {
        return run.id == stage.mission_run && run.persistent_actor == record.persistent_actor &&
            run.mission == stage.mission && run.definition_revision == stage.mission_revision &&
            run.definition_fingerprint == stage.mission_fingerprint &&
            std::all_of(run.rewards.begin(), run.rewards.end(), [&](MissionReceiptId receipt) { return receipt.scope == lineage; });
    }
    void aggregate(QuestRecord& record) {
        record.cleanup_pending = false;
        if (record.status == QuestStatus::Failed || record.status == QuestStatus::Abandoned) {
            for (const auto& stage : record.stages) {
                if (record.stop_owned && stage.mission_run != 0 && stage.ownership == QuestMissionOwnership::Owned &&
                    (stage.status == QuestStageStatus::Active || stage.status == QuestStageStatus::AwaitingRewards ||
                     stage.status == QuestStageStatus::MissingRun)) { record.cleanup_pending = true; }
            }
            return;
        }
        for (const auto& prerequisite : definitions.at(record.quest).value.prerequisites) {
            const auto* prior = find(record.persistent_actor, prerequisite);
            if (!prior || prior->status != QuestStatus::Completed) {
                transition(record, QuestStatus::Suspended, "prerequisite completion evidence is unavailable"); return;
            }
        }
        bool missing{}, failed{}, waiting{}, runnable{}, complete{true};
        for (const auto& stage : record.stages) {
            missing = missing || stage.status == QuestStageStatus::MissingRun;
            failed = failed || stage.status == QuestStageStatus::Failed || stage.status == QuestStageStatus::Cancelled;
            waiting = waiting || stage.status == QuestStageStatus::AwaitingRewards;
            runnable = runnable || stage.status == QuestStageStatus::Active || stage.status == QuestStageStatus::Ready;
            complete = complete && stage.status == QuestStageStatus::Completed;
        }
        if (failed) { transition(record, QuestStatus::Failed, "a required mission failed or was cancelled"); aggregate(record); }
        else if (missing) { transition(record, QuestStatus::Suspended, "saved mission binding is absent or mismatched"); }
        else if (complete) { transition(record, QuestStatus::Completed, "every mission and reward has completed"); }
        else if (!registry.alive(record.actor) || registry.persistent_id(record.actor) != record.persistent_actor) {
            transition(record, QuestStatus::Suspended, "persistent actor is unavailable");
        } else { transition(record, waiting && !runnable ? QuestStatus::AwaitingRewards : QuestStatus::Active, {}); }
    }
    QuestAvailability available(PersistentId actor, std::string_view quest) const {
        QuestAvailability result;
        const auto definition = definitions.find(quest);
        if (definition == definitions.end()) { result.error = QuestError::UnknownQuest; result.reason = "quest is not defined"; return result; }
        if (!actor) { result.error = QuestError::MissingIdentity; result.reason = "persistent actor is required"; return result; }
        const auto entity = registry.find(actor);
        if (!registry.alive(entity)) { result.error = QuestError::InvalidActor; result.reason = "persistent actor is unavailable"; return result; }
        if (find(actor, quest)) { result.error = QuestError::AlreadyAccepted; result.reason = "quest already retained for actor"; return result; }
        auto* missions = kernel.services().find<MissionService>();
        if (!missions) { result.error = QuestError::MissingService; result.reason = "MissionService is unavailable"; return result; }
        for (const auto& prerequisite : definition->second.value.prerequisites) {
            const auto* record = find(actor, prerequisite);
            if (!record || record->status != QuestStatus::Completed) { result.incomplete_prerequisites.push_back(prerequisite); }
        }
        std::set<std::string, std::less<>> internal;
        for (const auto& stage : definition->second.value.stages) { internal.insert(stage.mission); }
        std::set<std::string, std::less<>> external;
        for (const auto& stage : definition->second.value.stages) {
            const auto* mission = missions->definition(stage.mission);
            if (!mission) { result.error = QuestError::MissingService; result.reason = "mission definition is unavailable"; return result; }
            for (const auto& prerequisite : mission->prerequisites) {
                if (!internal.contains(prerequisite) && !missions->completed(actor, prerequisite)) { external.insert(prerequisite); }
            }
        }
        result.incomplete_missions.assign(external.begin(), external.end());
        if (!definition->second.value.items.empty()) {
            const auto* inventory = kernel.services().find<InventoryService>();
            if (!inventory) { result.error = QuestError::MissingService; result.reason = "InventoryService is unavailable"; return result; }
            const auto container = inventory->primary(entity);
            for (const auto& requirement : definition->second.value.items) {
                if (!inventory->definition(requirement.item) ||
                    inventory->quantity(container, requirement.item, requirement.include_reserved) < requirement.quantity) {
                    result.missing_items.push_back(requirement);
                }
            }
        }
        result.available = result.incomplete_prerequisites.empty() && result.incomplete_missions.empty() && result.missing_items.empty();
        result.error = result.available ? QuestError::None : QuestError::Prerequisite;
        result.reason = result.available ? "available" : "quest prerequisites or inventory requirements are unmet";
        return result;
    }
    void install_dialogue() {
        auto* dialogue = kernel.services().find<DialogueService>();
        if (!dialogue || stopped) { return; }
        for (std::size_t index = 0; index < provider_names.size(); ++index) {
            if (installed[index]) { continue; }
            const std::weak_ptr<int> weak = lifetime;
            const auto result = dialogue->register_condition(std::string(provider_names[index]),
                [this, weak, index](const DialogueContext& context, const DialogueCondition& condition) {
                    if (!weak.lock()) { return DialogueConditionResult{DialogueTruth::Unavailable, "quest service retired"}; }
                    const auto actor = condition.subject == DialogueSubject::Actor ? context.persistent_actor : context.persistent_partner;
                    if (!actor || !definitions.contains(condition.key)) {
                        return DialogueConditionResult{DialogueTruth::Unavailable, "quest or persistent subject is unavailable"};
                    }
                    if (index == 0) {
                        const auto status = available(actor, condition.key);
                        const bool unavailable = status.error == QuestError::MissingService || status.error == QuestError::InvalidActor;
                        return DialogueConditionResult{unavailable ? DialogueTruth::Unavailable :
                            (status.available ? DialogueTruth::Pass : DialogueTruth::Fail), status.reason};
                    }
                    const auto* record = find(actor, condition.key);
                    const bool pass = record && ((index == 1 && !terminal(record->status)) ||
                        (index == 2 && record->status == QuestStatus::Completed) || (index == 3 && record->status == QuestStatus::Failed));
                    return DialogueConditionResult{pass ? DialogueTruth::Pass : DialogueTruth::Fail, {}};
                });
            installed[index] = static_cast<bool>(result);
        }
    }
    void observe(QuestRecord& record, std::size_t index, MissionService& missions) {
        if (record.revision == std::numeric_limits<std::uint64_t>::max()) { return; }
        auto& stage = record.stages[index];
        const auto& definition = definitions.at(record.quest);
        const auto previous = stage.status;
        const auto previous_hash = stage.observed_progress;
        const auto previous_reason = core::fnv1a64(stage.reason);
        const auto set_reason = [&](std::string_view reason) {
            if (!core::valid_utf8(reason)) { reason = "mission message contains invalid UTF-8"; }
            stage.reason = text::truncate_utf8(reason, limits.text_bytes);
        };
        if (stage.mission_run != 0) {
            auto run = missions.inspect(stage.mission_run);
            if (!run || !matches(record, stage, *run)) {
                stage.status = QuestStageStatus::MissingRun; set_reason("retained mission binding unavailable");
            } else {
                if (terminal(record.status) && record.stop_owned && stage.ownership == QuestMissionOwnership::Owned && run->status == MissionStatus::Active) {
                    // Quest and mission diagnostics have independent byte bounds.
                    auto reason = text::truncate_utf8(record.reason, missions.limits().text_bytes);
                    if (reason.empty()) {
                        reason = std::string_view("quest terminated").substr(0, missions.limits().text_bytes);
                    }
                    const auto result = record.status == QuestStatus::Failed ? missions.fail(run->id, reason) : missions.cancel(run->id, reason);
                    if (result) { run = missions.inspect(stage.mission_run); }
                }
                if (run) {
                    stage.status = stage_status(run->status);
                    stage.observed_progress = progress_fingerprint(*run, limits.snapshot_bytes);
                    stage.total_objectives = static_cast<std::uint32_t>(run->objectives.size());
                    stage.completed_objectives = static_cast<std::uint32_t>(std::count_if(run->objectives.begin(), run->objectives.end(),
                        [](const auto& objective) { return objective.status == MissionObjectiveStatus::Completed; }));
                    set_reason(run->reason);
                    if ((run->status == MissionStatus::Active || run->status == MissionStatus::AwaitingRewards) && run->actor != record.actor) {
                        stage.status = QuestStageStatus::MissingRun; set_reason("mission actor needs explicit MissionService rebinding");
                    }
                }
            }
        } else if (!terminal(record.status) && stage.status != QuestStageStatus::MissingRun && ready(record, definition, index)) {
            stage.status = QuestStageStatus::Ready;
            if (definition.value.stages[index].start_policy == QuestStartPolicy::RequireAdoption) { set_reason("awaiting explicit mission adoption"); }
            if (definition.value.stages[index].start_policy == QuestStartPolicy::CreateOwned &&
                registry.alive(record.actor) && registry.persistent_id(record.actor) == record.persistent_actor && record.status != QuestStatus::Suspended) {
                try {
                    const auto result = missions.start(record.actor, stage.mission);
                    if (result) {
                        stage.mission_run = result.run; stage.ownership = QuestMissionOwnership::Owned;
                        stage.mission_revision = definition.mission_revisions[index];
                        stage.mission_fingerprint = definition.mission_fingerprints[index];
                        stage.status = QuestStageStatus::Active; stage.reason.clear();
                    } else { set_reason(result.message); }
                } catch (...) {
                    stage.status = QuestStageStatus::MissingRun;
                    set_reason("mission admission threw; inspect retained runs and explicitly adopt, never retry blindly");
                }
            }
        }
        if (stage.status != previous || stage.observed_progress != previous_hash || core::fnv1a64(stage.reason) != previous_reason) {
            touch(record); emit(QuestEventKind::StageChanged, &record, &stage, stage.reason);
        }
        aggregate(record);
    }
    QuestLocalizedText render(const QuestText& value, std::string_view locale) const {
        QuestLocalizedText result;
        result.key = value.key;
        if (!value.key.empty()) {
            if (const auto* localization = kernel.services().find<text::LocalizationService>()) {
                try {
                    auto lookup = localization->lookup(value.key, locale, text::MissingKeyPolicy::Empty);
                    if (lookup.found) { result.text = text::truncate_utf8(lookup.text, limits.text_bytes); result.locale = std::move(lookup.resolved_locale); return result; }
                } catch (const text::TextException&) {}
            }
        }
        result.text = value.fallback.empty() ? value.key : value.fallback;
        result.locale = locale; result.used_fallback = true;
        return result;
    }
};

QuestService::QuestService(core::Kernel& kernel, ecs::Registry& registry, QuestLimits limits, std::uint64_t lineage)
    : impl_(std::make_unique<Impl>(kernel, registry, limits, lineage)) {
    std::size_t slots{};
    if (lineage == 0 || limits.definitions == 0 || limits.definitions > 4096 || limits.stages_per_quest == 0 ||
        limits.stages_per_quest > 128 || limits.prerequisites > 128 || limits.item_requirements > 128 ||
        limits.records == 0 || limits.records > 16384 || limits.records_per_actor == 0 ||
        limits.records_per_actor > limits.records || limits.history == 0 || limits.history > 65536 ||
        limits.stage_work_per_frame == 0 || limits.stage_work_per_frame > 65536 ||
        limits.source_events_per_frame == 0 || limits.source_events_per_frame > 65536 ||
        limits.journal_page == 0 || limits.journal_page > 1024 || limits.name_bytes == 0 || limits.name_bytes > 512 ||
        limits.text_bytes == 0 || limits.text_bytes > 16384 || limits.snapshot_bytes < 128 ||
        limits.snapshot_bytes > 64U * 1024U * 1024U ||
        !core::checked_multiply(limits.records, limits.stages_per_quest, slots) || slots > 1048576) {
        throw std::invalid_argument("invalid bounded quest configuration");
    }
    impl_->work.reserve(slots);
    try { impl_->install_dialogue(); } catch (...) {}
}
QuestService::~QuestService() = default;

QuestResult QuestService::define(QuestDefinition definition) {
    auto& state = *impl_;
    if (state.busy || state.stopped) { return failure(QuestError::Busy, "quest mutations are unavailable"); }
    auto* missions = state.kernel.services().find<MissionService>();
    if (!missions) { return failure(QuestError::MissingService, "define missions before quests"); }
    const auto& limits = state.limits;
    const auto valid_text = [&](const QuestText& value) {
        return (value.key.empty() || name_valid(value.key, limits.name_bytes)) && text_valid(value.fallback, limits.text_bytes);
    };
    if (!name_valid(definition.id, limits.name_bytes) || definition.revision == 0 ||
        !valid_text(definition.title) || !valid_text(definition.summary) || definition.stages.empty() ||
        definition.stages.size() > limits.stages_per_quest || definition.prerequisites.size() > limits.prerequisites ||
        definition.items.size() > limits.item_requirements) { return failure(QuestError::InvalidDefinition, "invalid quest shape or text"); }
    if (state.definitions.contains(definition.id)) { return failure(QuestError::Duplicate, "quest definition is immutable"); }
    if (state.definitions.size() >= limits.definitions) { return failure(QuestError::Capacity, "quest definition capacity reached"); }
    Guard guard(state.busy);
    try {
        Impl::Definition compiled;
        compiled.order = state.definitions.size();
        std::set<std::string, std::less<>> parents;
        std::set<std::uint64_t> items;
        for (const auto& prerequisite : definition.prerequisites) {
            if (!state.definitions.contains(prerequisite) || !parents.insert(prerequisite).second) {
                return failure(QuestError::InvalidDefinition, "quest prerequisites must uniquely name earlier definitions");
            }
        }
        for (const auto& requirement : definition.items) {
            if (!requirement.item || requirement.quantity == 0 || !items.insert(requirement.item.value).second) {
                return failure(QuestError::InvalidDefinition, "item requirements need unique persistent definitions and positive quantities");
            }
        }
        std::map<std::string, std::size_t, std::less<>> indices;
        std::map<std::string, std::size_t, std::less<>> mission_indices;
        for (std::size_t index = 0; index < definition.stages.size(); ++index) {
            const auto& stage = definition.stages[index];
            const auto* mission = missions->definition(stage.mission);
            const auto mission_hash = missions->definition_hash(stage.mission);
            if (!name_valid(stage.id, limits.name_bytes) || !name_valid(stage.mission, limits.name_bytes) || !valid_text(stage.title) ||
                !mission || !mission_hash || indices.contains(stage.id) || stage.prerequisites.size() > limits.prerequisites ||
                static_cast<std::uint8_t>(stage.start_policy) > static_cast<std::uint8_t>(QuestStartPolicy::RequireAdoption) ||
                !mission_indices.emplace(stage.mission, index).second) {
                return failure(QuestError::InvalidDefinition, "stages need unique IDs, distinct existing missions and bounded text");
            }
            std::vector<std::size_t> dependencies;
            for (const auto& prerequisite : stage.prerequisites) {
                const auto found = indices.find(prerequisite);
                if (found == indices.end() || std::find(dependencies.begin(), dependencies.end(), found->second) != dependencies.end()) {
                    return failure(QuestError::InvalidDefinition, "stage prerequisites must uniquely name earlier stages");
                }
                dependencies.push_back(found->second);
            }
            indices.emplace(stage.id, index); compiled.dependencies.push_back(std::move(dependencies));
            compiled.mission_revisions.push_back(mission->revision);
            compiled.mission_fingerprints.push_back(*mission_hash);
        }
        core::ByteWriter hash(limits.snapshot_bytes);
        hash.write_string(definition.id); hash.write_u64(definition.revision);
        hash.write_string(definition.title.key); hash.write_string(definition.title.fallback);
        hash.write_string(definition.summary.key); hash.write_string(definition.summary.fallback);
        hash.write_u32(static_cast<std::uint32_t>(definition.prerequisites.size()));
        for (const auto& prerequisite : definition.prerequisites) { hash.write_string(prerequisite); hash.write_u64(state.definitions.at(prerequisite).fingerprint); }
        hash.write_u32(static_cast<std::uint32_t>(definition.items.size()));
        for (const auto& item : definition.items) { hash.write_u64(item.item.value); hash.write_u64(item.quantity); hash.write_bool(item.include_reserved); }
        hash.write_u32(static_cast<std::uint32_t>(definition.stages.size()));
        for (std::size_t index = 0; index < definition.stages.size(); ++index) {
            const auto& stage = definition.stages[index];
            std::vector<bool> ancestors(definition.stages.size());
            for (const auto dependency : compiled.dependencies[index]) { ancestors[dependency] = true; }
            for (std::size_t prior = index; prior > 0; --prior) {
                if (ancestors[prior - 1]) { for (const auto dependency : compiled.dependencies[prior - 1]) { ancestors[dependency] = true; } }
            }
            for (const auto& prerequisite : missions->definition(stage.mission)->prerequisites) {
                const auto found = mission_indices.find(prerequisite);
                if (found != mission_indices.end() && !ancestors[found->second]) {
                    return failure(QuestError::InvalidDefinition, "internal mission prerequisites must be stage ancestors");
                }
            }
            hash.write_string(stage.id); hash.write_string(stage.mission); hash.write_string(stage.title.key); hash.write_string(stage.title.fallback);
            hash.write_u8(static_cast<std::uint8_t>(stage.start_policy)); hash.write_u64(compiled.mission_fingerprints[index]);
            hash.write_u32(static_cast<std::uint32_t>(stage.prerequisites.size()));
            for (const auto& dependency : stage.prerequisites) { hash.write_string(dependency); }
        }
        compiled.fingerprint = core::fnv1a64(hash.bytes());
        const auto id = definition.id; compiled.value = std::move(definition);
        state.definitions.emplace(id, std::move(compiled));
        return {QuestError::None, 1, "immutable quest DAG admitted"};
    } catch (const core::BinaryException& exception) { return failure(QuestError::InvalidDefinition, exception.what()); }
}
const QuestDefinition* QuestService::definition(std::string_view id) const noexcept {
    const auto found = impl_->definitions.find(id);
    return found == impl_->definitions.end() ? nullptr : &found->second.value;
}
std::vector<std::string> QuestService::definitions() const {
    std::vector<std::string> result;
    result.reserve(impl_->definitions.size());
    for (const auto& entry : impl_->definitions) { result.push_back(entry.first); }
    return result;
}
QuestAvailability QuestService::availability(PersistentId actor, std::string_view quest) const { return impl_->available(actor, quest); }

QuestResult QuestService::accept(ecs::Entity actor, std::string_view quest) {
    auto& state = *impl_;
    if (state.busy || state.stopped) { return failure(QuestError::Busy, "quest mutations are unavailable"); }
    if (!state.registry.alive(actor)) { return failure(QuestError::InvalidActor, "actor generation is not live"); }
    const auto identity = state.registry.persistent_id(actor);
    const auto admission = state.available(identity, quest);
    if (!admission.available) { return failure(admission.error, admission.reason); }
    const auto actor_count = static_cast<std::size_t>(std::count_if(state.records.begin(), state.records.end(),
        [&](const auto& entry) { return entry.second.persistent_actor == identity; }));
    if (state.records.size() >= state.limits.records || actor_count >= state.limits.records_per_actor) {
        return failure(QuestError::Capacity, "retained quest record capacity reached");
    }
    Guard guard(state.busy);
    const auto& definition = state.definitions.find(quest)->second;
    QuestRecord record;
    record.actor = actor; record.persistent_actor = identity; record.quest = quest;
    record.definition_revision = definition.value.revision; record.definition_fingerprint = definition.fingerprint;
    record.accepted_tick = state.tick; record.changed_tick = state.tick; record.reason.reserve(state.limits.text_bytes);
    for (std::size_t index = 0; index < definition.value.stages.size(); ++index) {
        const auto& source = definition.value.stages[index];
        QuestStageProgress stage;
        stage.id = source.id; stage.mission = source.mission; stage.reason.reserve(state.limits.text_bytes);
        stage.status = definition.dependencies[index].empty() ? QuestStageStatus::Ready : QuestStageStatus::Locked;
        record.stages.push_back(std::move(stage));
    }
    const auto inserted = state.records.emplace(QuestKey{identity.value, std::string(quest)}, std::move(record));
    auto& retained = inserted.first->second;
    for (std::size_t index = 0; index < retained.stages.size(); ++index) { state.work.push_back({&retained, index}); }
    state.emit(QuestEventKind::Accepted, &retained);
    return {QuestError::None, 1, "quest accepted; owned missions start during bounded updates"};
}
QuestResult QuestService::adopt(PersistentId actor, std::string_view quest, std::string_view stage_id, std::uint64_t mission_run) {
    auto& state = *impl_;
    if (state.busy || state.stopped) { return failure(QuestError::Busy, "quest mutations are unavailable"); }
    auto* record = state.find(actor, quest);
    if (!record) { return failure(QuestError::UnknownRecord, "quest has not been accepted"); }
    if (terminal(record->status)) { return failure(QuestError::InvalidState, "terminal quests cannot adopt missions"); }
    if (record->revision == std::numeric_limits<std::uint64_t>::max()) { return failure(QuestError::Exhausted, "record revision exhausted"); }
    const auto found = std::find_if(record->stages.begin(), record->stages.end(), [&](const auto& value) { return value.id == stage_id; });
    if (found == record->stages.end() || found->mission_run != 0) { return failure(QuestError::InvalidState, "stage is unknown or already bound"); }
    const auto index = static_cast<std::size_t>(found - record->stages.begin());
    const auto& definition = state.definitions.at(record->quest);
    if (!state.ready(*record, definition, index)) { return failure(QuestError::Prerequisite, "stage dependencies are incomplete"); }
    auto* missions = state.kernel.services().find<MissionService>();
    if (!missions) { return failure(QuestError::MissingService, "MissionService is unavailable"); }
    const auto run = missions->inspect(mission_run);
    if (!run || run->persistent_actor != actor || run->mission != found->mission ||
        run->definition_revision != definition.mission_revisions[index] || run->definition_fingerprint != definition.mission_fingerprints[index] ||
        std::any_of(run->rewards.begin(), run->rewards.end(), [&](MissionReceiptId receipt) { return receipt.scope != state.lineage; })) {
        return failure(QuestError::MissionConflict, "adoption requires the exact actor, mission and immutable definition");
    }
    Guard guard(state.busy);
    found->mission_run = mission_run; found->mission_revision = run->definition_revision;
    found->mission_fingerprint = run->definition_fingerprint; found->ownership = QuestMissionOwnership::Adopted;
    state.touch(*record); state.observe(*record, index, *missions);
    return {QuestError::None, 1, "mission explicitly adopted without acquiring cancellation or reward ownership"};
}

QuestResult QuestService::abandon(PersistentId actor, std::string_view quest, QuestTerminationPolicy policy, std::string_view reason) {
    auto& state = *impl_;
    if (state.busy || state.stopped) { return failure(QuestError::Busy, "quest mutations are unavailable"); }
    auto* record = state.find(actor, quest);
    if (!record) { return failure(QuestError::UnknownRecord, "quest has not been accepted"); }
    if (terminal(record->status)) { return failure(QuestError::InvalidState, "quest is already terminal"); }
    if (record->revision == std::numeric_limits<std::uint64_t>::max()) { return failure(QuestError::Exhausted, "record revision exhausted"); }
    if (!text_valid(reason, state.limits.text_bytes) || static_cast<std::uint8_t>(policy) > 1) { return failure(QuestError::InvalidArgument, "invalid reason or termination policy"); }
    Guard guard(state.busy);
    record->stop_owned = policy == QuestTerminationPolicy::CancelOwned;
    state.transition(*record, QuestStatus::Abandoned, reason.empty() ? "quest abandoned" : reason); state.aggregate(*record);
    return {QuestError::None, 1, "quest abandoned; selected owned cleanup is incremental and pending rewards remain irrevocable"};
}
QuestResult QuestService::fail(PersistentId actor, std::string_view quest, std::string_view reason, QuestTerminationPolicy policy) {
    auto& state = *impl_;
    if (state.busy || state.stopped) { return failure(QuestError::Busy, "quest mutations are unavailable"); }
    auto* record = state.find(actor, quest);
    if (!record) { return failure(QuestError::UnknownRecord, "quest has not been accepted"); }
    if (terminal(record->status)) { return failure(QuestError::InvalidState, "quest is already terminal"); }
    if (record->revision == std::numeric_limits<std::uint64_t>::max()) { return failure(QuestError::Exhausted, "record revision exhausted"); }
    if (!text_valid(reason, state.limits.text_bytes) || static_cast<std::uint8_t>(policy) > 1) { return failure(QuestError::InvalidArgument, "invalid reason or termination policy"); }
    Guard guard(state.busy);
    state.transition(*record, QuestStatus::Failed, reason.empty() ? "quest failed" : reason);
    record->stop_owned = policy == QuestTerminationPolicy::CancelOwned; state.aggregate(*record);
    return {QuestError::None, 1, "quest failed; selected owned cleanup is incremental and adopted runs are untouched"};
}
QuestResult QuestService::track(PersistentId actor, std::string_view quest, bool tracked) {
    auto& state = *impl_;
    if (state.busy || state.stopped) { return failure(QuestError::Busy, "quest mutations are unavailable"); }
    auto* record = state.find(actor, quest);
    if (!record) { return failure(QuestError::UnknownRecord, "quest has not been accepted"); }
    if (tracked && terminal(record->status)) { return failure(QuestError::InvalidState, "terminal quests cannot be tracked"); }
    if (record->tracked == tracked) { return {}; }
    if (record->revision == std::numeric_limits<std::uint64_t>::max()) { return failure(QuestError::Exhausted, "record revision exhausted"); }
    record->tracked = tracked; state.touch(*record); state.emit(QuestEventKind::Tracked, record);
    return {QuestError::None, 1, "journal tracking updated"};
}
std::optional<QuestRecord> QuestService::inspect(PersistentId actor, std::string_view quest) const {
    const auto* record = impl_->find(actor, quest);
    return record ? std::optional<QuestRecord>(*record) : std::nullopt;
}

std::optional<QuestJournalEntry> QuestService::journal_entry(PersistentId actor, std::string_view quest, std::string_view locale) const {
    if (!text_valid(locale, 63)) { return std::nullopt; }
    const auto found = impl_->definitions.find(quest);
    if (found == impl_->definitions.end()) { return std::nullopt; }
    const auto& definition = found->second.value;
    QuestJournalEntry result;
    result.quest = quest; result.title = impl_->render(definition.title, locale); result.summary = impl_->render(definition.summary, locale);
    result.availability = availability(actor, quest); result.record = inspect(actor, quest);
    result.status = result.record ? result.record->status : (result.availability.available ? QuestStatus::Available : QuestStatus::Locked);
    const auto* missions = impl_->kernel.services().find<MissionService>();
    for (std::size_t index = 0; index < definition.stages.size(); ++index) {
        const auto& authored = definition.stages[index];
        QuestJournalStage stage;
        stage.title = impl_->render(authored.title, locale);
        if (result.record) { stage.progress = result.record->stages[index]; }
        else { stage.progress.id = authored.id; stage.progress.mission = authored.mission; }
        if (missions && stage.progress.mission_run != 0) {
            const auto run = missions->inspect(stage.progress.mission_run);
            const auto* mission = missions->definition(authored.mission);
            if (run && mission && result.record && impl_->matches(*result.record, stage.progress, *run)) {
                for (std::size_t objective = 0; objective < run->objectives.size() && objective < mission->objectives.size(); ++objective) {
                    const auto& source = mission->objectives[objective];
                    const auto label = source.label.empty() ? source.id : source.label;
                    stage.objectives.push_back({impl_->render({label, label}, locale), run->objectives[objective], source.required, source.optional});
                }
            }
        }
        result.stages.push_back(std::move(stage));
    }
    return result;
}
QuestJournalPage QuestService::journal(PersistentId actor, QuestJournalQuery query) const {
    QuestJournalPage result;
    if (!text_valid(query.locale, 63) || !text_valid(query.after_quest, impl_->limits.name_bytes)) { return result; }
    const auto limit = std::min(query.limit, impl_->limits.journal_page);
    if (const auto* localization = impl_->kernel.services().find<text::LocalizationService>()) { result.localization_revision = localization->revision(); }
    for (auto current = impl_->definitions.upper_bound(query.after_quest); current != impl_->definitions.end(); ++current) {
        auto entry = journal_entry(actor, current->first, query.locale);
        if ((!query.include_locked && entry->status == QuestStatus::Locked) ||
            (query.tracked_only && (!entry->record || !entry->record->tracked)) || (query.status && entry->status != *query.status)) { continue; }
        if (result.entries.size() == limit) { result.more = true; break; }
        result.next_after = current->first; result.entries.push_back(std::move(*entry));
    }
    return result;
}
QuestHistory QuestService::events_since(std::uint64_t cursor, std::size_t limit) const {
    QuestHistory result;
    result.next_cursor = std::min(cursor, impl_->sequence); result.newest_sequence = impl_->sequence;
    result.history_lost = cursor > impl_->sequence || (impl_->history_gap != 0 && cursor < impl_->history_gap);
    if (!impl_->history.empty() && cursor < impl_->history.front().sequence - 1) { result.history_lost = true; }
    const auto effective_cursor = result.next_cursor;
    for (const auto& event : impl_->history) {
        if (result.events.size() >= std::min(limit, impl_->limits.history)) { break; }
        if (event.sequence <= effective_cursor) { continue; }
        result.events.push_back(event); result.next_cursor = event.sequence;
    }
    if (impl_->history.empty() && cursor != impl_->sequence) { result.history_lost = true; result.next_cursor = impl_->sequence; }
    return result;
}
core::Signal<QuestEvent> QuestService::event_signal() {
    auto* hub = impl_->kernel.services().find<core::SignalHub>();
    return hub ? hub->channel<QuestEvent>("gameplay.quests.changed") : core::Signal<QuestEvent>{};
}
QuestStatistics QuestService::statistics() const noexcept {
    auto result = impl_->counters;
    result.definitions = impl_->definitions.size(); result.records = impl_->records.size(); result.source_cursor = impl_->source_cursor;
    for (const auto& entry : impl_->records) {
        const auto& record = entry.second;
        result.active += record.status == QuestStatus::Active ? 1U : 0U;
        result.awaiting_rewards += record.status == QuestStatus::AwaitingRewards ? 1U : 0U;
        result.suspended += record.status == QuestStatus::Suspended ? 1U : 0U;
        result.completed += record.status == QuestStatus::Completed ? 1U : 0U;
        result.cleanup_pending += record.cleanup_pending ? 1U : 0U;
    }
    return result;
}
const QuestLimits& QuestService::limits() const noexcept { return impl_->limits; }
void QuestService::update(const core::Frame& frame) {
    auto& state = *impl_;
    if (state.busy || state.stopped || (state.had_frame && frame.tick <= state.tick)) { return; }
    state.tick = frame.tick; state.had_frame = true; state.install_dialogue();
    auto* missions = state.kernel.services().find<MissionService>();
    if (!missions) { return; }
    Guard guard(state.busy);
    const auto source = missions->events_since(state.source_cursor, state.limits.source_events_per_frame);
    if (source.history_lost || source.newest_sequence < state.source_cursor) {
        increment(state.counters.source_history_losses);
        state.emit(QuestEventKind::SourceHistoryLost, nullptr, nullptr, "mission history gap; current retained runs remain authoritative");
    }
    state.source_cursor = source.newest_sequence < state.source_cursor || (source.history_lost && source.events.empty()) ?
        source.newest_sequence : source.next_cursor;
    const auto budget = std::min(state.limits.stage_work_per_frame, state.work.size());
    state.counters.budget_exhausted = budget < state.work.size();
    for (std::size_t used = 0; used < budget; ++used) {
        if (state.work_cursor >= state.work.size()) { state.work_cursor = 0; }
        const auto task = state.work[state.work_cursor++];
        task.record->actor = state.registry.find(task.record->persistent_actor);
        state.observe(*task.record, task.stage, *missions);
    }
}
void QuestService::shutdown() noexcept {
    if (impl_->stopped) { return; }
    impl_->stopped = true; impl_->lifetime.reset();
    try {
        if (auto* dialogue = impl_->kernel.services().find<DialogueService>()) {
            for (std::size_t index = 0; index < provider_names.size(); ++index) {
                if (impl_->installed[index]) { static_cast<void>(dialogue->unregister_condition(provider_names[index])); }
            }
        }
    } catch (...) {}
}

QuestResult QuestService::encode_snapshot(std::vector<std::byte>& output) const {
    const auto& state = *impl_;
    if (state.busy) { return failure(QuestError::Busy, "snapshot cannot run during mutation"); }
    try {
        core::ByteWriter writer(state.limits.snapshot_bytes);
        writer.write_bytes("GEQST001"); writer.write_u16(1); writer.write_u64(state.lineage);
        writer.write_u64(state.sequence); writer.write_u64(state.source_cursor);
        writer.write_u32(static_cast<std::uint32_t>(state.records.size()));
        for (const auto& entry : state.records) {
            const auto& record = entry.second;
            writer.write_u64(record.persistent_actor.value); writer.write_string(record.quest);
            writer.write_u64(record.definition_revision); writer.write_u64(record.definition_fingerprint);
            writer.write_u8(static_cast<std::uint8_t>(record.status)); writer.write_bool(record.tracked); writer.write_bool(record.stop_owned);
            writer.write_u64(record.accepted_tick); writer.write_u64(record.changed_tick); writer.write_u64(record.revision);
            writer.write_string(record.reason); writer.write_u32(static_cast<std::uint32_t>(record.stages.size()));
            for (const auto& stage : record.stages) {
                writer.write_string(stage.id); writer.write_string(stage.mission);
                writer.write_u8(static_cast<std::uint8_t>(stage.status)); writer.write_u8(static_cast<std::uint8_t>(stage.ownership));
                writer.write_u64(stage.mission_run); writer.write_u64(stage.mission_revision); writer.write_u64(stage.mission_fingerprint);
                writer.write_u64(stage.observed_progress); writer.write_u32(stage.completed_objectives); writer.write_u32(stage.total_objectives);
                writer.write_string(stage.reason);
            }
        }
        writer.write_u32(core::crc32(writer.bytes()));
        QuestResult success{QuestError::None, state.records.size(), "quest snapshot encoded"};
        output = writer.take();
        return success;
    } catch (const std::exception& exception) { return failure(QuestError::InvalidSnapshot, exception.what()); }
}
QuestResult QuestService::restore_snapshot(std::span<const std::byte> bytes, QuestRestorePolicy policy) {
    auto& state = *impl_;
    if (state.busy || state.stopped) { return failure(QuestError::Busy, "quest mutations are unavailable"); }
    if (!state.records.empty()) { return failure(QuestError::InvalidState, "restore requires an empty quest record store"); }
    if (static_cast<std::uint8_t>(policy) > 1 || bytes.size() < 42 || bytes.size() > state.limits.snapshot_bytes) {
        return failure(QuestError::InvalidSnapshot, "invalid snapshot size or restore policy");
    }
    Guard guard(state.busy);
    try {
        core::ByteReader checksum(bytes.last(4));
        if (checksum.read_u32() != core::crc32(bytes.first(bytes.size() - 4))) { return failure(QuestError::InvalidSnapshot, "quest snapshot checksum mismatch"); }
        core::BinaryLimits limits;
        limits.max_total_bytes = state.limits.snapshot_bytes;
        limits.max_string_bytes = std::max(state.limits.name_bytes, state.limits.text_bytes);
        core::ByteReader reader(bytes.first(bytes.size() - 4), core::ByteOrder::little, limits);
        reader.expect("GEQST001");
        if (reader.read_u16() != 1 || reader.read_u64() != state.lineage) { return failure(QuestError::DefinitionMismatch, "snapshot version or save lineage mismatch"); }
        const auto saved_sequence = reader.read_u64();
        const auto saved_cursor = reader.read_u64();
        const auto count = reader.read_u32();
        if (count > state.limits.records) { return failure(QuestError::Capacity, "snapshot record count exceeds bound"); }
        std::map<QuestKey, QuestRecord> decoded;
        std::map<std::uint64_t, std::size_t> actor_counts;
        std::set<std::uint64_t> owned_runs;
        auto* missions = state.kernel.services().find<MissionService>();
        for (std::uint32_t ordinal = 0; ordinal < count; ++ordinal) {
            QuestRecord record;
            record.persistent_actor = PersistentId{reader.read_u64()}; record.quest = reader.read_string();
            record.definition_revision = reader.read_u64(); record.definition_fingerprint = reader.read_u64();
            const auto definition = state.definitions.find(record.quest);
            if (!record.persistent_actor || definition == state.definitions.end() ||
                record.definition_revision != definition->second.value.revision || record.definition_fingerprint != definition->second.fingerprint) {
                return failure(QuestError::DefinitionMismatch, "snapshot actor or quest definition mismatch");
            }
            const auto raw_status = reader.read_u8();
            if (raw_status < static_cast<std::uint8_t>(QuestStatus::Active) || raw_status > static_cast<std::uint8_t>(QuestStatus::Abandoned)) {
                return failure(QuestError::InvalidSnapshot, "invalid retained quest status");
            }
            record.status = static_cast<QuestStatus>(raw_status); record.tracked = reader.read_bool(); record.stop_owned = reader.read_bool();
            record.accepted_tick = reader.read_u64(); record.changed_tick = reader.read_u64(); record.revision = reader.read_u64();
            record.reason = reader.read_string(); record.reason.reserve(state.limits.text_bytes);
            const auto stage_count = reader.read_u32();
            if (record.revision == 0 || !text_valid(record.reason, state.limits.text_bytes) ||
                stage_count != definition->second.value.stages.size() || (record.tracked && terminal(record.status)) ||
                (record.stop_owned && record.status != QuestStatus::Failed && record.status != QuestStatus::Abandoned)) {
                return failure(QuestError::InvalidSnapshot, "inconsistent quest state, revision or stage count");
            }
            record.actor = state.registry.find(record.persistent_actor);
            bool unresolved = !terminal(record.status) && !state.registry.alive(record.actor);
            for (std::uint32_t index = 0; index < stage_count; ++index) {
                QuestStageProgress stage;
                stage.id = reader.read_string(); stage.mission = reader.read_string();
                const auto raw_stage = reader.read_u8(); const auto raw_owner = reader.read_u8();
                if (raw_stage > static_cast<std::uint8_t>(QuestStageStatus::MissingRun) || raw_owner > 1 ||
                    stage.id != definition->second.value.stages[index].id || stage.mission != definition->second.value.stages[index].mission) {
                    return failure(QuestError::InvalidSnapshot, "invalid stage identity, status or ownership");
                }
                stage.status = static_cast<QuestStageStatus>(raw_stage); stage.ownership = static_cast<QuestMissionOwnership>(raw_owner);
                stage.mission_run = reader.read_u64(); stage.mission_revision = reader.read_u64(); stage.mission_fingerprint = reader.read_u64();
                stage.observed_progress = reader.read_u64(); stage.completed_objectives = reader.read_u32(); stage.total_objectives = reader.read_u32();
                stage.reason = reader.read_string(); stage.reason.reserve(state.limits.text_bytes);
                if (!text_valid(stage.reason, state.limits.text_bytes) || stage.completed_objectives > stage.total_objectives) {
                    return failure(QuestError::InvalidSnapshot, "invalid stage summary");
                }
                if (stage.mission_run == 0) {
                    if (stage.mission_revision != 0 || stage.mission_fingerprint != 0 || stage.ownership != QuestMissionOwnership::Owned ||
                        stage.observed_progress != 0 || stage.total_objectives != 0 ||
                        (stage.status != QuestStageStatus::Locked && stage.status != QuestStageStatus::Ready && stage.status != QuestStageStatus::MissingRun)) {
                        return failure(QuestError::InvalidSnapshot, "unbound stage carries mission state");
                    }
                    unresolved = unresolved || stage.status == QuestStageStatus::MissingRun;
                } else {
                    if (stage.mission_revision != definition->second.mission_revisions[index] ||
                        stage.mission_fingerprint != definition->second.mission_fingerprints[index] ||
                        (stage.ownership == QuestMissionOwnership::Owned && !owned_runs.insert(stage.mission_run).second)) {
                        return failure(QuestError::DefinitionMismatch, "mission definition or exclusive ownership mismatch");
                    }
                    const auto run = missions ? missions->inspect(stage.mission_run) : std::nullopt;
                    if (!run || !state.matches(record, stage, *run)) {
                        if (policy == QuestRestorePolicy::RequireMissions) { return failure(QuestError::PartialSnapshot, "restore the matching MissionService snapshot before quests"); }
                        stage.status = QuestStageStatus::MissingRun; unresolved = true;
                    } else {
                        if ((record.status == QuestStatus::Completed || stage.status == QuestStageStatus::Completed) && run->status != MissionStatus::Completed) {
                            return failure(QuestError::PartialSnapshot, "completed quest evidence is newer than retained mission state");
                        }
                        stage.status = stage_status(run->status);
                        stage.observed_progress = progress_fingerprint(*run, state.limits.snapshot_bytes);
                        stage.total_objectives = static_cast<std::uint32_t>(run->objectives.size());
                        stage.completed_objectives = static_cast<std::uint32_t>(std::count_if(run->objectives.begin(), run->objectives.end(),
                            [](const auto& objective) { return objective.status == MissionObjectiveStatus::Completed; }));
                        if ((run->status == MissionStatus::Active || run->status == MissionStatus::AwaitingRewards) && run->actor != record.actor) {
                            stage.status = QuestStageStatus::MissingRun; unresolved = true;
                        }
                    }
                }
                record.stages.push_back(std::move(stage));
            }
            if (record.status == QuestStatus::Completed && std::any_of(record.stages.begin(), record.stages.end(),
                [](const auto& stage) { return stage.mission_run == 0; })) { return failure(QuestError::InvalidSnapshot, "completed quest contains an unbound stage"); }
            if (unresolved && policy == QuestRestorePolicy::RequireMissions) { return failure(QuestError::PartialSnapshot, "actor or mission binding requires restoration or rebinding"); }
            if (unresolved && record.status != QuestStatus::Failed && record.status != QuestStatus::Abandoned) { record.status = QuestStatus::Suspended; }
            if (++actor_counts[record.persistent_actor.value] > state.limits.records_per_actor ||
                !decoded.emplace(QuestKey{record.persistent_actor.value, record.quest}, std::move(record)).second) {
                return failure(QuestError::InvalidSnapshot, "duplicate quest or per-actor capacity exceeded");
            }
        }
        reader.require_end();
        std::vector<Impl::Work> schedule;
        schedule.reserve(state.work.capacity());
        std::vector<QuestRecord*> ordered;
        ordered.reserve(decoded.size());
        for (auto& entry : decoded) { ordered.push_back(&entry.second); }
        std::sort(ordered.begin(), ordered.end(), [&](const auto* left, const auto* right) {
            return state.definitions.at(left->quest).order < state.definitions.at(right->quest).order;
        });
        for (auto* record : ordered) {
            for (const auto& prerequisite : state.definitions.at(record->quest).value.prerequisites) {
                const auto found = decoded.find({record->persistent_actor.value, prerequisite});
                if (found != decoded.end() && found->second.status == QuestStatus::Suspended &&
                    record->status != QuestStatus::Failed && record->status != QuestStatus::Abandoned) { record->status = QuestStatus::Suspended; }
            }
        }
        for (auto& entry : decoded) {
            auto& record = entry.second;
            const auto& definition = state.definitions.at(record.quest);
            for (const auto& prerequisite : definition.value.prerequisites) {
                const auto found = decoded.find({record.persistent_actor.value, prerequisite});
                if (found == decoded.end()) { return failure(QuestError::InvalidSnapshot, "accepted quest lacks its prerequisite record"); }
                if (found->second.status != QuestStatus::Completed &&
                    (policy == QuestRestorePolicy::RequireMissions || found->second.status != QuestStatus::Suspended)) {
                    return failure(QuestError::PartialSnapshot, "accepted quest lacks completed prerequisite evidence");
                }
            }
            for (std::size_t index = 0; index < record.stages.size(); ++index) {
                if (record.stages[index].mission_run != 0) {
                    for (const auto dependency : definition.dependencies[index]) {
                        if (record.stages[dependency].status != QuestStageStatus::Completed &&
                            (policy == QuestRestorePolicy::RequireMissions || record.stages[dependency].status != QuestStageStatus::MissingRun)) {
                            return failure(QuestError::PartialSnapshot, "bound stage lacks completed dependency evidence");
                        }
                    }
                }
                schedule.push_back({&record, index});
            }
            if (record.status == QuestStatus::Failed || record.status == QuestStatus::Abandoned) { state.aggregate(record); }
        }
        QuestResult success{QuestError::None, decoded.size(), "quest snapshot restored; independently saved services are not an atomic checkpoint"};
        state.records.swap(decoded); state.work.swap(schedule); state.work_cursor = 0;
        state.sequence = std::max(state.sequence, saved_sequence); state.history.clear(); state.history_gap = state.sequence;
        state.source_cursor = saved_cursor; state.had_frame = false;
        state.emit(QuestEventKind::Restored, nullptr, nullptr, "quest records restored; mission state will be reconciled without event replay");
        return success;
    } catch (const std::exception& exception) { return failure(QuestError::InvalidSnapshot, exception.what()); }
}
QuestResult QuestService::save(persistence::SaveJournal& journal, std::string_view key) const {
    std::vector<std::byte> bytes;
    const auto encoded = encode_snapshot(bytes);
    if (!encoded) { return encoded; }
    persistence::TransactionId transaction{};
    auto result = journal.begin(transaction, "quest snapshot");
    if (!result) { return failure(QuestError::JournalFailure, result.message); }
    result = journal.put(transaction, key, bytes, quest_value_type, 1);
    if (result) { result = journal.commit(transaction); }
    if (!result) { static_cast<void>(journal.abort(transaction)); return failure(QuestError::JournalFailure, result.message); }
    return {QuestError::None, impl_->records.size(), "quest snapshot committed"};
}
QuestResult QuestService::load(const persistence::SaveJournal& journal, std::string_view key, QuestRestorePolicy policy) {
    const auto* value = journal.find(key);
    if (!value || value->type != quest_value_type || value->version != 1) { return failure(QuestError::JournalFailure, "missing or incompatible quest journal value"); }
    return restore_snapshot(value->bytes, policy);
}

void register_quests(core::Kernel& kernel) {
    if (kernel.services().find<QuestService>()) { return; }
    auto* registry = kernel.services().find<ecs::Registry>();
    const auto scope = kernel.config().integer("mission.receipt_scope", 1);
    if (!registry || scope <= 0) { kernel.diagnose(core::Diagnostic::Severity::Error, "gameplay.quests", "Registry and positive mission.receipt_scope are required"); return; }
    auto* service = &kernel.services().emplace<QuestService>(kernel, *registry, QuestLimits{}, static_cast<std::uint64_t>(scope));
    static_cast<void>(kernel.register_module({"gameplay.quests", "Immutable quest DAGs, mission ownership and localized journal", 1}));
    std::vector<std::string> after;
    if (kernel.services().find<MissionService>()) { after.emplace_back("gameplay.mission.update"); }
    static_cast<void>(kernel.add_system({"gameplay.quests.update", core::Phase::Visibility, -6700, std::move(after)},
        [service](core::Kernel& host, const core::Frame& frame) {
            service->update(frame);
            const auto stats = service->statistics();
            host.metric("quests.active", static_cast<double>(stats.active));
            host.metric("quests.awaiting_rewards", static_cast<double>(stats.awaiting_rewards));
            host.metric("quests.suspended", static_cast<double>(stats.suspended));
            host.metric("quests.cleanup_pending", static_cast<double>(stats.cleanup_pending));
        }));
    static_cast<void>(kernel.add_shutdown("gameplay.quests.shutdown", [service] { service->shutdown(); }));
    static_cast<void>(kernel.add_command("quest.define", [service](core::Kernel&, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 4) { return core::CommandResult{false, "quest.define ID MISSION [TITLE_KEY] [PREREQUISITE]"}; }
        QuestDefinition definition;
        definition.id = args[0]; definition.title = {args.size() > 2 ? args[2] : args[0], args[0]};
        QuestStageDefinition stage;
        stage.id = "primary"; stage.mission = args[1]; stage.title = definition.title;
        definition.stages.push_back(std::move(stage));
        if (args.size() == 4) { definition.prerequisites.push_back(args[3]); }
        return command_result(service->define(std::move(definition)));
    }, "Define an immutable one-mission quest; C++ authoring supports full DAGs"));
    static_cast<void>(kernel.add_command("quest.accept", [service](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) { return core::CommandResult{false, "quest.accept ACTOR QUEST"}; }
        return command_result(service->accept(command_actor(host, args[0]), args[1]));
    }, "Accept a quest for an existing Registry actor"));
    static_cast<void>(kernel.add_command("quest.adopt", [service](core::Kernel&, core::CommandArgs args) {
        PersistentId actor; std::uint64_t run{};
        if (args.size() != 4 || !parse_u64(args[0], actor.value) || !parse_u64(args[3], run)) { return core::CommandResult{false, "quest.adopt ACTOR_ID QUEST STAGE MISSION_RUN"}; }
        return command_result(service->adopt(actor, args[1], args[2], run));
    }, "Explicitly observe a retained mission without acquiring cancellation ownership"));
    static_cast<void>(kernel.add_command("quest.control", [service](core::Kernel&, core::CommandArgs args) {
        PersistentId actor;
        if (args.size() < 3 || args.size() > 4 || !parse_u64(args[0], actor.value)) { return core::CommandResult{false, "quest.control ACTOR_ID QUEST abandon|detach|fail|track|untrack [REASON]"}; }
        const std::string_view reason = args.size() == 4 ? std::string_view(args[3]) : std::string_view{};
        if (args[2] == "abandon" || args[2] == "detach") { return command_result(service->abandon(actor, args[1], args[2] == "detach" ? QuestTerminationPolicy::Detach : QuestTerminationPolicy::CancelOwned, reason)); }
        if (args[2] == "fail") { return command_result(service->fail(actor, args[1], reason)); }
        if (args[2] == "track" || args[2] == "untrack") { return command_result(service->track(actor, args[1], args[2] == "track")); }
        return core::CommandResult{false, "unknown quest control"};
    }, "Change journal tracking or terminate a quest with explicit owned cleanup"));
    static_cast<void>(kernel.add_command("quest.journal", [service](core::Kernel&, core::CommandArgs args) {
        PersistentId actor;
        if (args.empty() || args.size() > 3 || !parse_u64(args[0], actor.value)) { return core::CommandResult{false, "quest.journal ACTOR_ID [LOCALE] [AFTER_QUEST]"}; }
        QuestJournalQuery query;
        if (args.size() > 1) { query.locale = args[1]; }
        if (args.size() > 2) { query.after_quest = args[2]; }
        const auto page = service->journal(actor, std::move(query));
        std::ostringstream output;
        for (const auto& entry : page.entries) { output << entry.quest << " " << describe(entry.status) << " " << entry.title.text << '\n'; }
        if (page.more) { output << "next_after=" << page.next_after; }
        return core::CommandResult{true, output.str()};
    }, "Read localized paginated quest and mission progress"));
    static_cast<void>(kernel.add_command("quest.persist", [service](core::Kernel& host, core::CommandArgs args) {
        auto* journal = host.services().find<persistence::SaveJournal>();
        if (!journal || args.size() != 2) { return core::CommandResult{false, "quest.persist save|load|load-suspended KEY requires an open SaveJournal"}; }
        if (args[0] == "save") { return command_result(service->save(*journal, args[1])); }
        if (args[0] == "load" || args[0] == "load-suspended") { return command_result(service->load(*journal, args[1], args[0] == "load" ? QuestRestorePolicy::RequireMissions : QuestRestorePolicy::SuspendMissing)); }
        return core::CommandResult{false, "unknown persistence operation"};
    }, "Save quests or restore after identities, inventory receipts and missions"));
}

} // namespace ge::gameplay
