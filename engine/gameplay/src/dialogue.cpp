#include <ge/gameplay/dialogue.hpp>
#include <ge/core/runtime.hpp>
#include <ge/gameplay/combat.hpp>
#include <ge/gameplay/inventory.hpp>
#include <ge/gameplay/mission.hpp>
#include <ge/script/vm.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace ge::gameplay {
namespace {
constexpr std::uint64_t inventory_issuer = 0x4449414c4f475545ULL;
constexpr double maximum_time = 1.0e12;
struct MutationGuard {
    bool& flag;
    explicit MutationGuard(bool& value) : flag(value) { flag = true; }
    ~MutationGuard() { flag = false; }
};
DialogueResult result(DialogueError error, std::string message, DialogueSessionId id = {}) {
    return {error, id, std::move(message)};
}
bool terminal(DialogueState state) noexcept {
    return state >= DialogueState::Completed;
}
bool identifier(std::string_view value, std::size_t limit, bool empty = false) noexcept {
    if (value.empty()) { return empty; }
    if (value.size() > limit) { return false; }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        if (!letter && !(byte >= '0' && byte <= '9') && character != '_' && character != '-' &&
            character != '.' && character != '/' && character != ':') { return false; }
    }
    return true;
}
bool bounded_text(std::string_view value, std::size_t limit) {
    if (value.size() > limit || value.find('\0') != std::string_view::npos) { return false; }
    try { static_cast<void>(text::scalar_count(value)); return true; }
    catch (const text::TextException&) { return false; }
}
bool compare(std::int64_t left, DialogueComparison operation, std::int64_t right) noexcept {
    switch (operation) {
    case DialogueComparison::Equal: return left == right;
    case DialogueComparison::NotEqual: return left != right;
    case DialogueComparison::Less: return left < right;
    case DialogueComparison::LessEqual: return left <= right;
    case DialogueComparison::Greater: return left > right;
    case DialogueComparison::GreaterEqual: return left >= right;
    }
    return false;
}
std::uint64_t allocate_scope() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) { return value; }
    }
    throw std::overflow_error("dialogue runtime scopes exhausted");
}
bool durable(DialogueEffectKind kind) noexcept {
    return kind == DialogueEffectKind::InventoryCost || kind == DialogueEffectKind::InventoryGrant ||
           kind == DialogueEffectKind::MissionEvent || kind == DialogueEffectKind::Provider;
}
bool parse_unsigned(std::string_view value, std::uint64_t& output) noexcept {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
std::string session_name(DialogueSessionId id) {
    return std::to_string(id.scope) + ":" + std::to_string(id.handle.index) + ":" + std::to_string(id.handle.generation);
}
std::optional<DialogueSessionId> parse_session(std::string_view value) noexcept {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) { return {}; }
    std::uint64_t scope{};
    if (!parse_unsigned(value.substr(0, separator), scope) || scope == 0) { return {}; }
    const auto entity = ecs::parse_entity(value.substr(separator + 1));
    if (!entity) { return {}; }
    return DialogueSessionId{scope, *entity};
}
}

std::string_view describe(DialogueError value) noexcept {
    switch (value) {
    case DialogueError::None: return "none";
    case DialogueError::InvalidDefinition: return "invalid definition";
    case DialogueError::Duplicate: return "duplicate";
    case DialogueError::UnknownGraph: return "unknown graph";
    case DialogueError::InvalidActor: return "invalid actor";
    case DialogueError::MissingIdentity: return "missing persistent identity";
    case DialogueError::Prerequisite: return "prerequisite";
    case DialogueError::Capacity: return "capacity";
    case DialogueError::Busy: return "busy";
    case DialogueError::UnknownSession: return "unknown session";
    case DialogueError::InvalidState: return "invalid state";
    case DialogueError::StalePresentation: return "stale presentation";
    case DialogueError::UnavailableChoice: return "unavailable choice";
    case DialogueError::MissingProvider: return "missing provider";
    case DialogueError::EffectRejected: return "effect rejected";
    case DialogueError::RetryExhausted: return "retry exhausted";
    case DialogueError::TransitionLimit: return "transition limit";
    case DialogueError::Exhausted: return "identity exhausted";
    case DialogueError::AllocationFailure: return "allocation failure";
    case DialogueError::InvalidArgument: return "invalid argument";
    }
    return "unknown";
}
std::string_view describe(DialogueState value) noexcept {
    switch (value) {
    case DialogueState::Running: return "running";
    case DialogueState::AwaitingAdvance: return "awaiting advance";
    case DialogueState::AwaitingChoice: return "awaiting choice";
    case DialogueState::AwaitingEffects: return "awaiting effects";
    case DialogueState::Completed: return "completed";
    case DialogueState::Cancelled: return "cancelled";
    case DialogueState::Failed: return "failed";
    case DialogueState::TimedOut: return "timed out";
    }
    return "unknown";
}

struct DialogueService::Impl {
    struct Active {
        DialogueSession view;
        std::shared_ptr<const DialogueGraph> graph;
        std::vector<std::uint64_t> batch;
        std::size_t next_effect{};
        std::string destination;
        bool choice_batch{};
    };
    struct Receipt {
        DialogueEffectReceipt view;
        DialogueEffectProvider provider;
        script::InstanceHandle script;
        InventoryTransaction transaction;
        PersistentId item;
        bool prepared{};
        bool captured{};
        bool admitted{};
        bool uncertain{};
        double retry_remaining{};
        double elapsed{};
    };
    core::Kernel& kernel;
    ecs::Registry& registry;
    DialogueLimits limits;
    GenerationPool handles;
    std::uint64_t scope;
    std::uint64_t next_receipt{1};
    std::uint64_t sequence{};
    std::uint64_t tick{};
    std::size_t definition_bytes{};
    std::uint32_t round_robin{};
    DialogueDeliveryCursor delivery;
    bool delivery_started{};
    bool busy{};
    bool stopped{};
    std::map<std::string, std::shared_ptr<const DialogueGraph>, std::less<>> graphs;
    std::map<std::uint32_t, Active> active;
    std::map<std::uint64_t, Receipt> receipts;
    std::map<std::string, DialogueConditionProvider, std::less<>> conditions;
    std::map<std::string, DialogueEffectProvider, std::less<>> effects;
    std::deque<DialogueEvent> history;
    DialogueStatistics counters;

    Impl(core::Kernel& host, ecs::Registry& entities, DialogueLimits settings)
        : kernel(host), registry(entities), limits(settings), handles(settings.sessions), scope(allocate_scope()) {
        if (limits.graphs == 0 || limits.nodes_per_graph == 0 || limits.nodes_per_graph > 4096 ||
            limits.sessions == 0 || limits.sessions > 4096 || limits.flags > 4096 ||
            limits.name_bytes == 0 || limits.name_bytes > 4096 || limits.text_bytes < 32 || limits.text_bytes > 1048576 ||
            limits.arguments_per_text > 256 || limits.effects_per_group > 256 ||
            limits.conditions_per_group > 256 || limits.choices_per_node > 256 ||
            limits.costs_per_choice > 256 || limits.steps_per_frame == 0 || limits.retry_attempts == 0 ||
            !std::isfinite(limits.retry_seconds) || limits.retry_seconds <= 0.0 ||
            !std::isfinite(limits.effect_timeout_seconds) || limits.effect_timeout_seconds <= 0.0 || limits.effect_timeout_seconds > maximum_time ||
            !std::isfinite(limits.maximum_delta_seconds) || limits.maximum_delta_seconds <= 0.0) {
            throw std::invalid_argument("invalid dialogue limits");
        }
    }
    Active* find(DialogueSessionId id) noexcept {
        if (id.scope != scope || !handles.alive(id.handle)) { return nullptr; }
        const auto found = active.find(id.handle.index);
        return found == active.end() ? nullptr : &found->second;
    }
    const DialogueNode& node(const Active& session) const {
        const auto found = std::find_if(session.graph->nodes.begin(), session.graph->nodes.end(),
            [&](const DialogueNode& entry) { return entry.id == session.view.context.node; });
        if (found == session.graph->nodes.end()) { throw std::logic_error("validated dialogue node missing"); }
        return *found;
    }
    ecs::Entity subject(const DialogueContext& context, DialogueSubject selected) const noexcept {
        return selected == DialogueSubject::Actor ? context.actor : context.partner;
    }
    bool actors_valid(const DialogueContext& context) const noexcept {
        if (!registry.enabled(context.actor) || registry.persistent_id(context.actor) != context.persistent_actor) { return false; }
        return !context.partner || (registry.enabled(context.partner) &&
            registry.persistent_id(context.partner) == context.persistent_partner);
    }
    text::LocalizationLimits text_limits() const {
        text::LocalizationLimits value;
        value.max_key_bytes = std::max<std::size_t>(limits.name_bytes + 5, 12);
        value.max_pattern_bytes = limits.text_bytes;
        value.max_output_bytes = limits.text_bytes;
        value.max_arguments = limits.arguments_per_text + limits.flags + 2;
        return value;
    }
    bool valid_text(const DialogueText& value, bool required) const {
        if (!identifier(value.key, limits.name_bytes, true) || !bounded_text(value.fallback, limits.text_bytes) ||
            (required && value.key.empty() && value.fallback.empty()) || value.arguments.size() > limits.arguments_per_text) { return false; }
        std::set<std::string, std::less<>> names;
        for (const auto& argument : value.arguments) {
            if (!identifier(argument.name, limits.name_bytes) || argument.name == "actor_name" ||
                argument.name == "partner_name" || argument.name.starts_with("flag.") || !names.insert(argument.name).second) { return false; }
            if (const auto* string = std::get_if<std::string>(&argument.value); string && !bounded_text(*string, limits.text_bytes)) { return false; }
            if (const auto* number = std::get_if<double>(&argument.value); number && !std::isfinite(*number)) { return false; }
        }
        try {
            text::CatalogDefinition catalog;
            catalog.locale = "en";
            catalog.entries.emplace("v", value.fallback);
            const text::Catalog compiled(std::move(catalog), text_limits());
            static_cast<void>(compiled);
        } catch (const text::TextException&) { return false; }
        return true;
    }
    bool valid_conditions(const std::vector<DialogueCondition>& group) const {
        if (group.size() > limits.conditions_per_group) { return false; }
        for (const auto& condition : group) {
            if (condition.kind > DialogueConditionKind::Provider || condition.subject > DialogueSubject::Partner ||
                condition.comparison > DialogueComparison::GreaterEqual || !std::isfinite(condition.threshold)) { return false; }
            const bool no_key = condition.kind == DialogueConditionKind::Always || condition.kind == DialogueConditionKind::CombatCanAct ||
                                condition.kind == DialogueConditionKind::HealthAtLeast;
            if (!identifier(condition.key, limits.name_bytes, no_key) ||
                !identifier(condition.provider, limits.name_bytes, condition.kind != DialogueConditionKind::Provider)) { return false; }
            if (condition.kind == DialogueConditionKind::InventoryAtLeast && condition.quantity == 0) { return false; }
        }
        return true;
    }
    bool valid_costs(const std::vector<DialogueCost>& costs) const {
        if (costs.size() > limits.costs_per_choice) { return false; }
        std::set<std::string, std::less<>> names;
        for (const auto& cost : costs) {
            if (!identifier(cost.item, limits.name_bytes) || cost.quantity == 0 || !names.insert(cost.item).second) { return false; }
        }
        return true;
    }
    bool valid_effects(const std::vector<DialogueEffect>& group) const {
        if (group.size() > limits.effects_per_group) { return false; }
        for (const auto& effect : group) {
            if (effect.kind > DialogueEffectKind::Provider || effect.subject > DialogueSubject::Partner ||
                !identifier(effect.key, limits.name_bytes, effect.kind == DialogueEffectKind::InventoryCost) ||
                !identifier(effect.provider, limits.name_bytes, effect.kind != DialogueEffectKind::Provider) || !valid_costs(effect.costs)) { return false; }
            if (effect.kind == DialogueEffectKind::InventoryCost && effect.costs.empty()) { return false; }
            if ((effect.kind == DialogueEffectKind::InventoryGrant || effect.kind == DialogueEffectKind::MissionEvent) && effect.quantity == 0) { return false; }
        }
        return true;
    }
    bool validate(const DialogueGraph& graph) const {
        if (!identifier(graph.id, limits.name_bytes) || !identifier(graph.entry, limits.name_bytes) || graph.revision == 0 ||
            graph.nodes.empty() || graph.nodes.size() > limits.nodes_per_graph || !valid_conditions(graph.prerequisites) ||
            !std::isfinite(graph.timeout_seconds) || graph.timeout_seconds < 0.0 || graph.timeout_seconds > maximum_time) { return false; }
        std::map<std::string, std::size_t, std::less<>> indices;
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto& entry = graph.nodes[index];
            if (!identifier(entry.id, limits.name_bytes) || !indices.emplace(entry.id, index).second ||
                entry.kind > DialogueNodeKind::End || entry.speaker > DialogueSubject::Partner ||
                !std::isfinite(entry.timeout_seconds) || entry.timeout_seconds < 0.0 || entry.timeout_seconds > maximum_time ||
                !valid_text(entry.text, entry.kind == DialogueNodeKind::Line) ||
                !valid_conditions(entry.conditions) || !valid_effects(entry.effects) ||
                entry.choices.size() > limits.choices_per_node || entry.branches.size() > limits.choices_per_node) { return false; }
            if ((entry.kind != DialogueNodeKind::Choices && !entry.choices.empty()) ||
                (entry.kind != DialogueNodeKind::Branch && !entry.branches.empty()) ||
                (entry.kind == DialogueNodeKind::Choices && entry.choices.empty()) ||
                (entry.kind == DialogueNodeKind::Branch && entry.branches.empty()) ||
                ((entry.kind == DialogueNodeKind::End || entry.kind == DialogueNodeKind::Choices) && !entry.next.empty())) { return false; }
            std::set<std::string, std::less<>> choice_ids;
            for (const auto& choice : entry.choices) {
                if (!identifier(choice.id, limits.name_bytes) || !choice_ids.insert(choice.id).second ||
                    !valid_text(choice.text, true) || !valid_conditions(choice.conditions) ||
                    !valid_costs(choice.costs) || !valid_effects(choice.effects)) { return false; }
            }
            for (const auto& branch : entry.branches) { if (!valid_conditions(branch.conditions)) { return false; } }
        }
        const auto initial = indices.find(graph.entry);
        if (initial == indices.end()) { return false; }
        std::vector<std::vector<std::size_t>> edges(graph.nodes.size());
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto& entry = graph.nodes[index];
            const auto append = [&](std::string_view target) {
                const auto found = indices.find(target);
                if (found == indices.end()) { return false; }
                edges[index].push_back(found->second);
                return true;
            };
            if ((entry.kind == DialogueNodeKind::Line || !entry.next.empty()) && !append(entry.next)) { return false; }
            for (const auto& choice : entry.choices) { if (!append(choice.next)) { return false; } }
            for (const auto& branch : entry.branches) { if (!append(branch.next)) { return false; } }
        }
        std::vector<bool> reachable(graph.nodes.size()), ending(graph.nodes.size());
        reachable[initial->second] = true;
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) { ending[index] = graph.nodes[index].kind == DialogueNodeKind::End; }
        for (std::size_t pass = 0; pass < graph.nodes.size(); ++pass) {
            bool changed{};
            for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
                for (const auto target : edges[index]) {
                    if (reachable[index] && !reachable[target]) { reachable[target] = true; changed = true; }
                    if (ending[target] && !ending[index]) { ending[index] = true; changed = true; }
                }
            }
            if (!changed) { break; }
        }
        return std::all_of(reachable.begin(), reachable.end(), [](bool value) { return value; }) &&
               std::all_of(ending.begin(), ending.end(), [](bool value) { return value; });
    }
    std::optional<std::size_t> accounted_bytes(const DialogueGraph& graph) const {
        std::size_t bytes{};
        bool fits = true;
        const auto charge = [&](std::size_t amount) {
            if (amount > limits.graph_bytes - std::min(bytes, limits.graph_bytes)) { fits = false; }
            else { bytes += amount; }
        };
        const auto cost_bytes = [&](const std::vector<DialogueCost>& costs) {
            for (const auto& cost : costs) { charge(sizeof(DialogueCost) + cost.item.size()); }
        };
        const auto condition_bytes = [&](const std::vector<DialogueCondition>& group) {
            for (const auto& condition : group) { charge(sizeof(DialogueCondition) + condition.key.size() + condition.provider.size()); }
        };
        const auto effect_bytes = [&](const std::vector<DialogueEffect>& group) {
            for (const auto& effect : group) {
                charge(sizeof(DialogueEffect) + effect.key.size() + effect.provider.size());
                cost_bytes(effect.costs);
            }
        };
        const auto text_bytes = [&](const DialogueText& value) {
            charge(value.key.size() + value.fallback.size());
            for (const auto& argument : value.arguments) {
                charge(sizeof(text::FormatArgument) + argument.name.size());
                if (const auto* string = std::get_if<std::string>(&argument.value)) { charge(string->size()); }
            }
        };
        charge(sizeof(DialogueGraph) + graph.id.size() * 2 + graph.entry.size());
        condition_bytes(graph.prerequisites);
        for (const auto& entry : graph.nodes) {
            charge(sizeof(DialogueNode) + entry.id.size() + entry.next.size());
            text_bytes(entry.text);
            condition_bytes(entry.conditions);
            effect_bytes(entry.effects);
            for (const auto& choice : entry.choices) {
                charge(sizeof(DialogueChoice) + choice.id.size() + choice.next.size());
                text_bytes(choice.text);
                condition_bytes(choice.conditions);
                cost_bytes(choice.costs);
                effect_bytes(choice.effects);
            }
            for (const auto& branch : entry.branches) { charge(sizeof(DialogueBranch) + branch.next.size()); condition_bytes(branch.conditions); }
        }
        return fits ? std::optional<std::size_t>(bytes) : std::nullopt;
    }
    DialogueConditionResult evaluate(const DialogueContext& context, const DialogueCondition& condition) {
        const auto entity = subject(context, condition.subject);
        if (condition.kind != DialogueConditionKind::Always && condition.kind != DialogueConditionKind::Flag &&
            condition.kind != DialogueConditionKind::Provider && !registry.enabled(entity)) {
            return {DialogueTruth::Unavailable, "condition participant unavailable"};
        }
        bool passed{};
        switch (condition.kind) {
        case DialogueConditionKind::Always: passed = true; break;
        case DialogueConditionKind::Flag: {
            const auto found = context.flags.find(condition.key);
            passed = compare(found == context.flags.end() ? 0 : found->second, condition.comparison, condition.value);
            break;
        }
        case DialogueConditionKind::Tag: passed = registry.has_tag(entity, condition.key); break;
        case DialogueConditionKind::InventoryAtLeast: {
            const auto* inventory = kernel.services().find<InventoryService>();
            if (!inventory) { return {DialogueTruth::Unavailable, "inventory service unavailable"}; }
            const auto* item = inventory->definition(condition.key);
            const auto container = inventory->primary(entity);
            if (!item || !container) { return {DialogueTruth::Unavailable, "inventory item or container unavailable"}; }
            passed = inventory->quantity(container, item->id, false) >= condition.quantity;
            break;
        }
        case DialogueConditionKind::MissionCompleted:
        case DialogueConditionKind::MissionActive: {
            const auto* missions = kernel.services().find<MissionService>();
            if (!missions || !missions->definition(condition.key)) { return {DialogueTruth::Unavailable, "mission definition unavailable"}; }
            const auto identity = registry.persistent_id(entity);
            passed = missions->completed(identity, condition.key);
            if (condition.kind == DialogueConditionKind::MissionActive) {
                const auto runs = missions->runs(identity, missions->limits().runs_per_actor);
                passed = std::any_of(runs.begin(), runs.end(), [&](const MissionRun& run) {
                    return run.mission == condition.key && (run.status == MissionStatus::Active || run.status == MissionStatus::AwaitingRewards);
                });
            }
            break;
        }
        case DialogueConditionKind::CombatCanAct:
        case DialogueConditionKind::HealthAtLeast: {
            const auto* combat = kernel.services().find<CombatService>();
            const auto state = combat ? combat->inspect(entity) : std::nullopt;
            if (!state) { return {DialogueTruth::Unavailable, "combat actor unavailable"}; }
            passed = condition.kind == DialogueConditionKind::CombatCanAct ? state->can_act : state->combatant.health >= condition.threshold;
            break;
        }
        case DialogueConditionKind::Provider: {
            const auto found = conditions.find(condition.provider);
            if (found == conditions.end()) { return {DialogueTruth::Unavailable, "condition provider unavailable"}; }
            DialogueConditionResult answer;
            try { answer = found->second(context, condition); }
            catch (...) { return {DialogueTruth::Unavailable, "condition provider threw"}; }
            if (answer.truth > DialogueTruth::Unavailable || !bounded_text(answer.reason, limits.text_bytes)) {
                return {DialogueTruth::Unavailable, "invalid condition provider result"};
            }
            if (answer.truth == DialogueTruth::Unavailable) { return answer; }
            if (condition.negate) { answer.truth = answer.truth == DialogueTruth::Pass ? DialogueTruth::Fail : DialogueTruth::Pass; }
            return answer;
        }
        }
        if (condition.negate) { passed = !passed; }
        return {passed ? DialogueTruth::Pass : DialogueTruth::Fail, passed ? "" : "condition not satisfied: " + condition.key};
    }
    DialogueConditionResult eligible(const DialogueContext& context, const std::vector<DialogueCondition>& group) {
        for (const auto& condition : group) {
            const auto answer = evaluate(context, condition);
            if (answer.truth != DialogueTruth::Pass) { return answer; }
        }
        return actors_valid(context) ? DialogueConditionResult{} : DialogueConditionResult{DialogueTruth::Unavailable, "actor lifecycle changed"};
    }
    DialogueConditionResult affordable(ecs::Entity actor, const std::vector<DialogueCost>& costs) const {
        if (costs.empty()) { return {}; }
        const auto* inventory = kernel.services().find<InventoryService>();
        if (!inventory) { return {DialogueTruth::Unavailable, "inventory service unavailable"}; }
        const auto container = inventory->primary(actor);
        const auto snapshot = inventory->inspect(container);
        if (!snapshot || snapshot->lock) { return {DialogueTruth::Unavailable, "inventory absent or locked"}; }
        for (const auto& cost : costs) {
            const auto* item = inventory->definition(cost.item);
            if (!item) { return {DialogueTruth::Unavailable, "unknown cost item: " + cost.item}; }
            if (inventory->quantity(container, item->id, false) < cost.quantity) { return {DialogueTruth::Fail, "insufficient item: " + cost.item}; }
        }
        return {};
    }
    DialogueRenderedText render(const DialogueText& source, const DialogueSession& session) const {
        DialogueRenderedText output;
        output.key = source.key;
        auto arguments = source.arguments;
        arguments.push_back({"actor_name", std::string(registry.name(session.context.actor))});
        arguments.push_back({"partner_name", std::string(registry.name(session.context.partner))});
        for (const auto& [key, value] : session.context.flags) { arguments.push_back({"flag." + key, value}); }
        const auto* localization = kernel.services().find<text::LocalizationService>();
        output.locale = session.locale.empty() ? (localization ? std::string(localization->locale()) : "en") : session.locale;
        try {
            if (localization && !source.key.empty()) {
                auto localized = localization->format(source.key, arguments, output.locale, text::MissingKeyPolicy::Empty);
                if (localized.found && localized.text.size() <= limits.text_bytes) {
                    output.text = std::move(localized.text);
                    output.locale = std::move(localized.resolved_locale);
                    return output;
                }
                output.error = localized.found ? "localized output exceeds dialogue limit" : "localization key missing";
            }
        } catch (const std::exception&) { output.error = "localized formatting failed"; }
        output.used_fallback = true;
        try { output.text = text::format_message(source.fallback, arguments, output.locale, text_limits()); }
        catch (const std::exception&) { output.text = "[dialogue text unavailable]"; output.error = "fallback formatting failed"; }
        if (output.text.empty() && !source.key.empty()) { output.text = "[missing text]"; }
        return output;
    }
    void emit(DialogueEventKind kind, const Active& session, DialogueEffectId effect = {}, std::string_view message = {}) noexcept {
        try {
            if (sequence == std::numeric_limits<std::uint64_t>::max()) { ++counters.lost_signals; return; }
            DialogueEvent event{++sequence, tick, kind, session.view.context, session.view.state, effect,
                                session.view.selected_choice, std::string(message.substr(0, limits.text_bytes))};
            if (limits.history != 0) {
                if (history.size() == limits.history) { history.pop_front(); ++counters.overwritten_history; }
                history.push_back(event);
            }
            if (auto* hub = kernel.services().find<core::SignalHub>()) {
                const auto channel = hub->channel<DialogueEvent>("gameplay.dialogue");
                std::size_t bytes = event.message.size() + event.choice.size() + event.context.graph.size() + event.context.node.size();
                for (const auto& [key, value] : event.context.flags) { static_cast<void>(value); bytes += key.size() + sizeof(value); }
                if (!hub->enqueue(channel, std::move(event), bytes)) { ++counters.lost_signals; }
            }
        } catch (...) { ++counters.lost_signals; }
    }
    void present(Active& session, bool force = false) {
        const auto& current = node(session);
        auto line = render(current.text, session.view);
        std::vector<DialogueAvailableChoice> choices;
        for (const auto& choice : current.choices) {
            auto answer = eligible(session.view.context, current.conditions);
            if (answer.truth == DialogueTruth::Pass) { answer = eligible(session.view.context, choice.conditions); }
            if (answer.truth == DialogueTruth::Pass) { answer = affordable(session.view.context.actor, choice.costs); }
            if (answer.truth == DialogueTruth::Pass && session.view.state != DialogueState::AwaitingChoice) {
                answer = {DialogueTruth::Fail, "node effects pending"};
            }
            choices.push_back({choice.id, render(choice.text, session.view), answer.truth == DialogueTruth::Pass, std::move(answer.reason)});
        }
        const auto same_text = [](const DialogueRenderedText& left, const DialogueRenderedText& right) {
            return left.text == right.text && left.key == right.key && left.locale == right.locale &&
                   left.error == right.error && left.used_fallback == right.used_fallback;
        };
        bool same = same_text(line, session.view.line) && choices.size() == session.view.choices.size();
        for (std::size_t index = 0; same && index < choices.size(); ++index) {
            const auto& old = session.view.choices[index];
            const auto& fresh = choices[index];
            same = old.id == fresh.id && old.enabled == fresh.enabled && old.reason == fresh.reason && same_text(old.text, fresh.text);
        }
        if (same && !force) { return; }
        if (session.view.presentation_revision == std::numeric_limits<std::uint64_t>::max()) { throw std::overflow_error("presentation revision exhausted"); }
        session.view.line = std::move(line);
        session.view.choices = std::move(choices);
        session.view.speaker = subject(session.view.context, current.speaker);
        ++session.view.presentation_revision;
        emit(DialogueEventKind::Presented, session);
    }
    void abandon(Receipt& receipt) noexcept {
        if (receipt.view.state == DialogueReceiptState::Completed) { return; }
        try {
            if (receipt.script) {
                if (auto* scripts = kernel.services().find<script::ScriptService>()) {
                    static_cast<void>(scripts->stop(receipt.script));
                    if (scripts->release(receipt.script) || !scripts->inspect(receipt.script)) { receipt.script = {}; }
                }
                receipt.view.cancellation_pending = static_cast<bool>(receipt.script);
            }
            if (receipt.provider && (receipt.admitted || receipt.uncertain)) {
                receipt.view.request.phase = DialogueEffectPhase::Cancel;
                static_cast<void>(receipt.provider(receipt.view.request));
            }
        } catch (...) { receipt.uncertain = true; }
        receipt.view.cancellation_pending = static_cast<bool>(receipt.script);
        receipt.view.state = receipt.admitted || receipt.uncertain ? DialogueReceiptState::Abandoned : DialogueReceiptState::Rejected;
    }
    void finish(Active& session, DialogueState state, DialogueError error, std::string_view message) noexcept {
        for (const auto id : session.batch) {
            const auto found = receipts.find(id);
            if (found != receipts.end()) { abandon(found->second); }
        }
        session.batch.clear();
        session.view.pending_effects = 0;
        session.view.state = state;
        session.view.error = error;
        try { session.view.message = std::string(message.substr(0, limits.text_bytes)); }
        catch (...) { session.view.message.clear(); }
        for (auto& choice : session.view.choices) { choice.enabled = false; }
        const auto kind = state == DialogueState::Completed ? DialogueEventKind::Completed :
            state == DialogueState::Cancelled ? DialogueEventKind::Cancelled :
            state == DialogueState::TimedOut ? DialogueEventKind::TimedOut : DialogueEventKind::Failed;
        emit(kind, session, {}, session.view.message);
    }
    void move_to(Active& session, std::string target) {
        session.view.context.node = std::move(target);
        session.view.node_seconds = 0.0;
        session.view.selected_choice.clear();
        session.view.choices.clear();
        session.view.state = DialogueState::Running;
    }
    void settle(Active& session) {
        if (session.choice_batch) {
            session.choice_batch = false;
            move_to(session, std::move(session.destination));
            return;
        }
        const auto& current = node(session);
        if (current.kind == DialogueNodeKind::End) { finish(session, DialogueState::Completed, DialogueError::None, "conversation completed"); return; }
        if (current.kind == DialogueNodeKind::Branch) {
            std::string destination = current.next;
            for (const auto& branch : current.branches) {
                const auto answer = eligible(session.view.context, branch.conditions);
                if (answer.truth == DialogueTruth::Unavailable) {
                    finish(session, DialogueState::Failed, DialogueError::MissingProvider, answer.reason); return;
                }
                if (answer.truth == DialogueTruth::Pass) { destination = branch.next; break; }
            }
            if (destination.empty()) { finish(session, DialogueState::Failed, DialogueError::Prerequisite, "no branch matched"); return; }
            move_to(session, std::move(destination));
            return;
        }
        session.view.state = current.kind == DialogueNodeKind::Choices ? DialogueState::AwaitingChoice : DialogueState::AwaitingAdvance;
        present(session, true);
    }
    void batch(Active& session, const std::vector<DialogueEffect>& authored, const std::vector<DialogueCost>& costs = {}) {
        std::vector<DialogueEffect> work;
        work.reserve(authored.size() + (costs.empty() ? 0U : 1U));
        if (!costs.empty()) {
            DialogueEffect charge;
            charge.kind = DialogueEffectKind::InventoryCost;
            charge.costs = costs;
            work.push_back(std::move(charge));
        }
        work.insert(work.end(), authored.begin(), authored.end());
        if (work.size() > limits.receipts - std::min(receipts.size(), limits.receipts) ||
            work.size() > std::numeric_limits<std::uint64_t>::max() - next_receipt) { throw std::length_error("dialogue receipt capacity exhausted"); }
        const auto durable_count = static_cast<std::uint64_t>(std::count_if(work.begin(), work.end(), [](const DialogueEffect& effect) { return durable(effect.kind); }));
        if (delivery.producer != 0 && durable_count > std::numeric_limits<std::uint64_t>::max() - delivery.next_sequence) {
            throw std::overflow_error("delivery cursor exhausted");
        }
        std::vector<std::uint64_t> prepared;
        prepared.reserve(work.size());
        try {
            for (auto& effect : work) {
                Receipt receipt;
                receipt.view.runtime_id = {scope, next_receipt++};
                receipt.view.request.context = session.view.context;
                receipt.view.request.effect = std::move(effect);
                if (durable(receipt.view.request.effect.kind) && delivery.producer != 0) {
                    receipt.view.request.id = {delivery.producer, delivery.next_sequence++};
                    delivery_started = true;
                }
                if (receipt.view.request.effect.kind == DialogueEffectKind::Provider) {
                    const auto found = effects.find(receipt.view.request.effect.provider);
                    if (found != effects.end()) { receipt.provider = found->second; }
                }
                const auto id = receipt.view.runtime_id.sequence;
                receipts.emplace(id, std::move(receipt));
                prepared.push_back(id);
            }
        } catch (...) {
            for (const auto id : prepared) { receipts.erase(id); }
            throw;
        }
        session.batch = std::move(prepared);
        session.next_effect = 0;
        session.view.pending_effects = session.batch.size();
        session.view.state = DialogueState::AwaitingEffects;
    }
    DialogueEffectResult inventory_effect(Receipt& receipt, ecs::Entity entity) {
        auto* inventory = kernel.services().find<InventoryService>();
        if (!inventory) { return {DialogueEffectOutcome::Retry, "inventory service unavailable"}; }
        const auto& effect = receipt.view.request.effect;
        const auto id = receipt.view.request.id;
        const InventoryReceiptId identity{{inventory_issuer}, id.producer, id.sequence};
        if (!receipt.prepared) {
            if (effect.kind == DialogueEffectKind::InventoryCost) {
                const auto container = inventory->primary(entity);
                if (!container) { return {DialogueEffectOutcome::Rejected, "primary inventory absent"}; }
                InventoryTransaction prepared;
                prepared.receipt = identity;
                for (const auto& cost : effect.costs) {
                    const auto* item = inventory->definition(cost.item);
                    if (!item) { return {DialogueEffectOutcome::Rejected, "unknown cost item"}; }
                    InventoryOperation operation;
                    operation.kind = InventoryOperationKind::Remove;
                    operation.source = container;
                    operation.definition = item->id;
                    operation.quantity = cost.quantity;
                    prepared.operations.push_back(operation);
                }
                receipt.transaction = std::move(prepared);
            } else {
                const auto* item = inventory->definition(effect.key);
                if (!item) { return {DialogueEffectOutcome::Rejected, "unknown grant item"}; }
                receipt.item = item->id;
            }
            receipt.prepared = true;
        }
        const auto applied = effect.kind == DialogueEffectKind::InventoryCost ? inventory->transact(receipt.transaction) :
            inventory->grant(entity, registry.persistent_id(entity), receipt.item, effect.quantity, identity);
        if (applied) { return {DialogueEffectOutcome::Completed, std::string(describe(applied.error))}; }
        const bool retry = applied.error == InventoryError::Busy || applied.error == InventoryError::AllocationFailure ||
                           applied.error == InventoryError::ReceiptCapacity;
        return {retry ? DialogueEffectOutcome::Retry : DialogueEffectOutcome::Rejected, std::string(describe(applied.error))};
    }
    DialogueEffectResult apply(Active& session, Receipt& receipt) {
        const auto& effect = receipt.view.request.effect;
        if (durable(effect.kind) && !receipt.view.request.id) { return {DialogueEffectOutcome::Rejected, "durable delivery disabled: configure producer and next sequence"}; }
        const auto entity = subject(receipt.view.request.context, effect.subject);
        if (effect.kind != DialogueEffectKind::SetFlag && !registry.enabled(entity)) { return {DialogueEffectOutcome::Rejected, "effect actor unavailable"}; }
        switch (effect.kind) {
        case DialogueEffectKind::SetFlag:
            if (!session.view.context.flags.contains(effect.key) && session.view.context.flags.size() >= limits.flags) {
                return {DialogueEffectOutcome::Rejected, "conversation flag capacity exhausted"};
            }
            session.view.context.flags.insert_or_assign(effect.key, effect.value);
            return {DialogueEffectOutcome::Completed, "flag set"};
        case DialogueEffectKind::InventoryCost:
        case DialogueEffectKind::InventoryGrant: return inventory_effect(receipt, entity);
        case DialogueEffectKind::MissionEvent: {
            auto* missions = kernel.services().find<MissionService>();
            if (!missions) { return {DialogueEffectOutcome::Retry, "mission service unavailable"}; }
            const auto& context = receipt.view.request.context;
            const auto target = effect.subject == DialogueSubject::Actor ? context.persistent_partner : context.persistent_actor;
            const auto applied = missions->submit({{receipt.view.request.id.producer, receipt.view.request.id.sequence},
                entity, MissionObjectiveKind::Dialogue, effect.key, target, effect.quantity});
            if (applied || applied.error == MissionError::DuplicateEvent) { return {DialogueEffectOutcome::Completed, "dialogue input accepted by mission service"}; }
            const bool retry = applied.error == MissionError::Capacity || applied.error == MissionError::Busy;
            return {retry ? DialogueEffectOutcome::Retry : DialogueEffectOutcome::Rejected, applied.message};
        }
        case DialogueEffectKind::Script: {
            auto* scripts = kernel.services().find<script::ScriptService>();
            if (!scripts) { return {DialogueEffectOutcome::Retry, "script service unavailable"}; }
            if (!receipt.script) {
                std::string diagnostic;
                receipt.script = scripts->start(effect.key, entity, {}, &diagnostic);
                if (!receipt.script) { return {DialogueEffectOutcome::Rejected, std::move(diagnostic)}; }
                return {DialogueEffectOutcome::Pending, "script admitted"};
            }
            const auto state = scripts->inspect(receipt.script);
            if (!state) { return {DialogueEffectOutcome::Rejected, "owned script instance disappeared"}; }
            if (state->state == script::ExecutionState::Completed) {
                static_cast<void>(scripts->release(receipt.script));
                receipt.script = {};
                return {DialogueEffectOutcome::Completed, "script completed"};
            }
            if (state->state == script::ExecutionState::Faulted || state->state == script::ExecutionState::Stopped) {
                return {DialogueEffectOutcome::Rejected, "script terminated: " + state->error.message};
            }
            return {DialogueEffectOutcome::Pending, "script still executing"};
        }
        case DialogueEffectKind::Provider:
            if (!receipt.provider) { return {DialogueEffectOutcome::Rejected, "effect provider unavailable"}; }
            return receipt.provider(receipt.view.request);
        }
        return {DialogueEffectOutcome::Rejected, "invalid effect kind"};
    }
    void step(Active& session) {
        if (!actors_valid(session.view.context)) { finish(session, DialogueState::Cancelled, DialogueError::InvalidActor, "actor destroyed, disabled or persistent identity changed"); return; }
        if (session.view.state == DialogueState::Running) {
            if (session.view.context.visit >= limits.visits_per_session) { finish(session, DialogueState::Failed, DialogueError::TransitionLimit, "conversation visit budget exhausted"); return; }
            const auto& current = node(session);
            const auto answer = eligible(session.view.context, current.conditions);
            if (answer.truth != DialogueTruth::Pass) { finish(session, DialogueState::Failed, DialogueError::Prerequisite, answer.reason); return; }
            ++session.view.context.visit;
            emit(DialogueEventKind::NodeEntered, session);
            present(session, true);
            batch(session, current.effects);
            if (session.batch.empty()) { settle(session); }
            return;
        }
        if (session.view.state != DialogueState::AwaitingEffects) { present(session); return; }
        auto& receipt = receipts.at(session.batch.at(session.next_effect));
        if (receipt.retry_remaining > 0.0) { return; }
        if (!receipt.captured) { receipt.view.request.context = session.view.context; receipt.captured = true; }
        receipt.view.request.phase = receipt.admitted ? DialogueEffectPhase::Poll : DialogueEffectPhase::Apply;
        if (receipt.admitted) {
            if (receipt.view.polls == std::numeric_limits<std::uint64_t>::max()) { finish(session, DialogueState::Failed, DialogueError::Exhausted, "effect poll counter exhausted"); return; }
            ++receipt.view.polls;
        } else { ++receipt.view.attempts; }
        DialogueEffectResult applied;
        try { applied = apply(session, receipt); }
        catch (...) { receipt.uncertain = true; applied = {DialogueEffectOutcome::Rejected, "effect threw; external outcome unknown, no automatic reapplication"}; }
        if (applied.outcome > DialogueEffectOutcome::Rejected || !bounded_text(applied.message, limits.text_bytes)) {
            receipt.uncertain = true;
            applied = {DialogueEffectOutcome::Rejected, "invalid effect result; external outcome unknown"};
        }
        receipt.view.message = std::move(applied.message);
        if (applied.outcome == DialogueEffectOutcome::Completed) {
            receipt.view.state = DialogueReceiptState::Completed;
            ++session.view.completed_effects;
            --session.view.pending_effects;
            ++session.next_effect;
            emit(DialogueEventKind::EffectCompleted, session, receipt.view.runtime_id, receipt.view.message);
            if (session.next_effect == session.batch.size()) { session.batch.clear(); settle(session); }
            return;
        }
        if (applied.outcome == DialogueEffectOutcome::Pending || (receipt.admitted && applied.outcome == DialogueEffectOutcome::Retry)) {
            receipt.view.state = DialogueReceiptState::Pending;
            if (!receipt.admitted) { emit(DialogueEventKind::EffectAdmitted, session, receipt.view.runtime_id, receipt.view.message); }
            receipt.admitted = true;
            return;
        }
        if (applied.outcome == DialogueEffectOutcome::Retry && receipt.view.attempts < limits.retry_attempts) {
            receipt.retry_remaining = limits.retry_seconds;
            emit(DialogueEventKind::EffectRetry, session, receipt.view.runtime_id, receipt.view.message);
            return;
        }
        emit(DialogueEventKind::EffectRejected, session, receipt.view.runtime_id, receipt.view.message);
        const auto error = applied.outcome == DialogueEffectOutcome::Retry ? DialogueError::RetryExhausted : DialogueError::EffectRejected;
        const auto reason = receipt.view.message;
        finish(session, DialogueState::Failed, error, reason);
    }
};

DialogueService::DialogueService(core::Kernel& kernel, ecs::Registry& registry, DialogueLimits limits)
    : impl_(std::make_unique<Impl>(kernel, registry, limits)) {}
DialogueService::~DialogueService() = default;

DialogueResult DialogueService::define(DialogueGraph graph) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable"); }
    MutationGuard guard(impl_->busy);
    try {
        if (!impl_->validate(graph)) { return result(DialogueError::InvalidDefinition, "invalid graph, unreachable node, or no terminal path"); }
        if (impl_->graphs.contains(graph.id)) { return result(DialogueError::Duplicate, "graph definitions are immutable"); }
        if (impl_->graphs.size() >= impl_->limits.graphs) { return result(DialogueError::Capacity, "graph capacity exhausted"); }
        const auto bytes = impl_->accounted_bytes(graph);
        if (!bytes || *bytes > impl_->limits.definition_bytes - std::min(impl_->definition_bytes, impl_->limits.definition_bytes)) {
            return result(DialogueError::Capacity, "graph or aggregate definition byte budget exhausted");
        }
        auto id = graph.id;
        auto frozen = std::make_shared<const DialogueGraph>(graph);
        impl_->graphs.emplace(std::move(id), std::move(frozen));
        impl_->definition_bytes += *bytes;
        return result(DialogueError::None, "graph admitted");
    } catch (const std::bad_alloc&) { return result(DialogueError::AllocationFailure, "graph allocation failed"); }
}
std::optional<DialogueGraph> DialogueService::definition(std::string_view id) const {
    const auto found = impl_->graphs.find(id);
    return found == impl_->graphs.end() ? std::nullopt : std::optional<DialogueGraph>(*found->second);
}
std::vector<std::string> DialogueService::definitions() const {
    std::vector<std::string> output;
    for (const auto& [id, graph] : impl_->graphs) { static_cast<void>(graph); output.push_back(id); }
    return output;
}
DialogueResult DialogueService::start(ecs::Entity actor, ecs::Entity partner, std::string_view graph, std::string_view locale) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable"); }
    MutationGuard guard(impl_->busy);
    const auto found = impl_->graphs.find(graph);
    if (found == impl_->graphs.end()) { return result(DialogueError::UnknownGraph, "unknown dialogue graph"); }
    if (!impl_->registry.enabled(actor) || (partner && !impl_->registry.enabled(partner))) { return result(DialogueError::InvalidActor, "participants must be enabled live entities"); }
    if (!impl_->registry.persistent_id(actor) || (partner && !impl_->registry.persistent_id(partner))) { return result(DialogueError::MissingIdentity, "participants require persistent identities"); }
    for (const auto& [index, existing] : impl_->active) {
        static_cast<void>(index);
        if (!terminal(existing.view.state) && existing.view.context.actor == actor) { return result(DialogueError::Busy, "actor already has an active conversation"); }
    }
    std::optional<RuntimeHandle> handle;
    try {
        Impl::Active session;
        session.graph = found->second;
        session.view.locale = locale.empty() ? "" : text::canonical_locale(locale);
        session.view.graph_revision = found->second->revision;
        session.view.context = {{}, actor, partner, impl_->registry.persistent_id(actor), impl_->registry.persistent_id(partner), std::string(graph), found->second->entry, 0, {}};
        const auto answer = impl_->eligible(session.view.context, found->second->prerequisites);
        if (answer.truth != DialogueTruth::Pass) { return result(DialogueError::Prerequisite, answer.reason); }
        handle = impl_->handles.acquire();
        if (!handle) { return result(DialogueError::Capacity, "conversation capacity exhausted; release terminal sessions"); }
        const DialogueSessionId id{impl_->scope, *handle};
        session.view.context.session = id;
        const auto inserted = impl_->active.emplace(handle->index, std::move(session));
        impl_->emit(DialogueEventKind::Started, inserted.first->second);
        return result(DialogueError::None, "conversation started", id);
    } catch (const std::exception&) {
        if (handle) { impl_->active.erase(handle->index); static_cast<void>(impl_->handles.release(*handle)); }
        return result(DialogueError::InvalidArgument, "invalid locale or conversation allocation failure");
    }
}
DialogueResult DialogueService::advance(DialogueSessionId id, std::uint64_t revision) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable", id); }
    MutationGuard guard(impl_->busy);
    auto* session = impl_->find(id);
    if (!session) { return result(DialogueError::UnknownSession, "stale conversation handle", id); }
    if (session->view.state != DialogueState::AwaitingAdvance) { return result(DialogueError::InvalidState, "conversation is not awaiting advance", id); }
    impl_->present(*session);
    if (revision != session->view.presentation_revision) { return result(DialogueError::StalePresentation, "refresh displayed conversation", id); }
    const auto& current = impl_->node(*session);
    const auto answer = impl_->eligible(session->view.context, current.conditions);
    if (answer.truth != DialogueTruth::Pass) { return result(DialogueError::Prerequisite, answer.reason, id); }
    impl_->move_to(*session, current.next);
    return result(DialogueError::None, "advance admitted", id);
}
DialogueResult DialogueService::select(DialogueSessionId id, std::string_view choice_id, std::uint64_t revision) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable", id); }
    MutationGuard guard(impl_->busy);
    auto* session = impl_->find(id);
    if (!session) { return result(DialogueError::UnknownSession, "stale conversation handle", id); }
    if (session->view.state != DialogueState::AwaitingChoice) { return result(DialogueError::InvalidState, "conversation is not awaiting a choice", id); }
    try {
        impl_->present(*session);
        if (revision != session->view.presentation_revision) { return result(DialogueError::StalePresentation, "refresh available choices", id); }
        const auto& current = impl_->node(*session);
        const auto selected = std::find_if(current.choices.begin(), current.choices.end(), [&](const DialogueChoice& value) { return value.id == choice_id; });
        if (selected == current.choices.end()) { return result(DialogueError::UnavailableChoice, "choice does not belong to displayed node", id); }
        auto answer = impl_->eligible(session->view.context, current.conditions);
        if (answer.truth == DialogueTruth::Pass) { answer = impl_->eligible(session->view.context, selected->conditions); }
        if (answer.truth == DialogueTruth::Pass) { answer = impl_->affordable(session->view.context.actor, selected->costs); }
        if (answer.truth != DialogueTruth::Pass) { return result(DialogueError::UnavailableChoice, answer.reason, id); }
        session->view.selected_choice = selected->id;
        session->destination = selected->next;
        impl_->batch(*session, selected->effects, selected->costs);
        session->choice_batch = true;
        for (auto& choice : session->view.choices) { choice.enabled = false; }
        impl_->emit(DialogueEventKind::ChoiceSelected, *session);
        if (session->batch.empty()) { impl_->settle(*session); }
        return result(DialogueError::None, "choice admitted; inspect effect completion", id);
    } catch (const std::exception&) {
        impl_->finish(*session, DialogueState::Failed, DialogueError::Capacity, "choice preparation failed; earlier completed effects retained");
        return result(DialogueError::Capacity, session->view.message, id);
    }
}
DialogueResult DialogueService::refresh(DialogueSessionId id) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable", id); }
    MutationGuard guard(impl_->busy);
    auto* session = impl_->find(id);
    if (!session) { return result(DialogueError::UnknownSession, "stale conversation handle", id); }
    if (session->view.state != DialogueState::AwaitingChoice && session->view.state != DialogueState::AwaitingAdvance) {
        return result(DialogueError::InvalidState, "conversation is not displaying input", id);
    }
    impl_->present(*session);
    return result(DialogueError::None, "presentation refreshed", id);
}
DialogueResult DialogueService::set_flag(DialogueSessionId id, std::string key, std::int64_t value) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable", id); }
    MutationGuard guard(impl_->busy);
    auto* session = impl_->find(id);
    if (!session) { return result(DialogueError::UnknownSession, "stale conversation handle", id); }
    if (terminal(session->view.state) || session->view.state == DialogueState::AwaitingEffects) { return result(DialogueError::InvalidState, "cannot change flags during effects or after termination", id); }
    if (!identifier(key, impl_->limits.name_bytes)) { return result(DialogueError::InvalidArgument, "invalid flag name", id); }
    if (!session->view.context.flags.contains(key) && session->view.context.flags.size() >= impl_->limits.flags) { return result(DialogueError::Capacity, "flag capacity exhausted", id); }
    session->view.context.flags.insert_or_assign(std::move(key), value);
    if (session->view.state != DialogueState::Running) { impl_->present(*session); }
    return result(DialogueError::None, "flag updated", id);
}
DialogueResult DialogueService::cancel(DialogueSessionId id, std::string_view reason) {
    if (impl_->busy) { return result(DialogueError::Busy, "dialogue mutation is reentrant", id); }
    MutationGuard guard(impl_->busy);
    auto* session = impl_->find(id);
    if (!session) { return result(DialogueError::UnknownSession, "stale conversation handle", id); }
    if (terminal(session->view.state)) { return result(DialogueError::InvalidState, "conversation already terminated", id); }
    if (!bounded_text(reason, impl_->limits.text_bytes)) { return result(DialogueError::InvalidArgument, "invalid cancellation reason", id); }
    impl_->finish(*session, DialogueState::Cancelled, DialogueError::None, reason.empty() ? "conversation cancelled; completed effects retained" : reason);
    return result(DialogueError::None, session->view.message, id);
}
DialogueResult DialogueService::release(DialogueSessionId id) {
    if (impl_->busy) { return result(DialogueError::Busy, "dialogue mutation is reentrant", id); }
    const auto* session = impl_->find(id);
    if (!session) { return result(DialogueError::UnknownSession, "stale conversation handle", id); }
    if (!terminal(session->view.state)) { return result(DialogueError::InvalidState, "cancel or complete conversation before release", id); }
    impl_->active.erase(id.handle.index);
    static_cast<void>(impl_->handles.release(id.handle));
    return result(DialogueError::None, "session released; receipts retained", id);
}
DialogueResult DialogueService::register_condition(std::string name, DialogueConditionProvider provider) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable"); }
    if (!identifier(name, impl_->limits.name_bytes) || !provider) { return result(DialogueError::InvalidArgument, "invalid condition provider"); }
    if (impl_->conditions.contains(name)) { return result(DialogueError::Duplicate, "condition provider already registered"); }
    if (impl_->conditions.size() >= impl_->limits.providers) { return result(DialogueError::Capacity, "condition provider capacity exhausted"); }
    impl_->conditions.emplace(std::move(name), std::move(provider));
    return result(DialogueError::None, "condition provider registered");
}
DialogueResult DialogueService::register_effect(std::string name, DialogueEffectProvider provider) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable"); }
    if (!identifier(name, impl_->limits.name_bytes) || !provider) { return result(DialogueError::InvalidArgument, "invalid effect provider"); }
    if (impl_->effects.contains(name)) { return result(DialogueError::Duplicate, "effect provider already registered"); }
    if (impl_->effects.size() >= impl_->limits.providers) { return result(DialogueError::Capacity, "effect provider capacity exhausted"); }
    impl_->effects.emplace(std::move(name), std::move(provider));
    return result(DialogueError::None, "effect provider registered");
}
DialogueResult DialogueService::unregister_condition(std::string_view name) {
    if (impl_->busy) { return result(DialogueError::Busy, "dialogue mutation is reentrant"); }
    const auto found = impl_->conditions.find(name);
    if (found == impl_->conditions.end()) { return result(DialogueError::MissingProvider, "condition provider absent"); }
    impl_->conditions.erase(found);
    return result(DialogueError::None, "condition provider removed");
}
DialogueResult DialogueService::unregister_effect(std::string_view name) {
    if (impl_->busy) { return result(DialogueError::Busy, "dialogue mutation is reentrant"); }
    const auto found = impl_->effects.find(name);
    if (found == impl_->effects.end()) { return result(DialogueError::MissingProvider, "effect provider absent"); }
    impl_->effects.erase(found);
    return result(DialogueError::None, "effect provider removed; admitted receipts retain their provider");
}
DialogueResult DialogueService::configure_delivery(DialogueDeliveryCursor cursor) {
    if (impl_->busy || impl_->stopped) { return result(DialogueError::Busy, "dialogue service unavailable"); }
    if (cursor.next_sequence == 0 || cursor.producer == 0x4d495353494e5445ULL) { return result(DialogueError::InvalidArgument, "invalid cursor or reserved Interaction producer"); }
    for (const auto& [index, session] : impl_->active) {
        static_cast<void>(index);
        if (!session.batch.empty()) { return result(DialogueError::Busy, "delivery cursor changes require no pending batches"); }
    }
    if (impl_->delivery_started && (cursor.producer != impl_->delivery.producer || cursor.next_sequence < impl_->delivery.next_sequence)) {
        return result(DialogueError::InvalidArgument, "used producer is fixed and its sequence cannot decrease");
    }
    impl_->delivery = cursor;
    return result(DialogueError::None, cursor.producer == 0 ? "durable effect delivery disabled" : "delivery cursor configured");
}
DialogueDeliveryCursor DialogueService::delivery_cursor() const noexcept { return impl_->delivery; }
std::optional<DialogueSession> DialogueService::inspect(DialogueSessionId id) const {
    const auto* session = impl_->find(id);
    return session ? std::optional<DialogueSession>(session->view) : std::nullopt;
}
std::vector<DialogueSession> DialogueService::sessions(std::size_t limit) const {
    std::vector<DialogueSession> output;
    for (const auto& [index, session] : impl_->active) {
        static_cast<void>(index);
        if (output.size() >= limit) { break; }
        output.push_back(session.view);
    }
    return output;
}
std::optional<DialogueEffectReceipt> DialogueService::receipt(DialogueEffectId id) const {
    const auto found = impl_->receipts.find(id.sequence);
    return id.producer == impl_->scope && found != impl_->receipts.end() ? std::optional<DialogueEffectReceipt>(found->second.view) : std::nullopt;
}
DialogueHistory DialogueService::events_since(std::uint64_t cursor, std::size_t limit) const {
    DialogueHistory output;
    output.next_cursor = cursor;
    output.newest_sequence = impl_->sequence;
    output.history_lost = cursor > impl_->sequence || (cursor < impl_->sequence &&
        (impl_->history.empty() || cursor + 1 < impl_->history.front().sequence));
    for (const auto& event : impl_->history) {
        if (event.sequence <= cursor) { continue; }
        if (output.events.size() >= std::min(limit, impl_->limits.history)) { break; }
        if (event.sequence - output.next_cursor > 1) { output.history_lost = true; }
        output.events.push_back(event);
        output.next_cursor = event.sequence;
    }
    if (output.events.size() < std::min(limit, impl_->limits.history) && output.next_cursor < impl_->sequence &&
        (impl_->history.empty() || output.next_cursor >= impl_->history.back().sequence)) { output.history_lost = true; }
    return output;
}
core::Signal<DialogueEvent> DialogueService::event_signal() {
    auto* hub = impl_->kernel.services().find<core::SignalHub>();
    return hub ? hub->channel<DialogueEvent>("gameplay.dialogue") : core::Signal<DialogueEvent>{};
}
DialogueStatistics DialogueService::statistics() const noexcept {
    auto output = impl_->counters;
    output.graphs = impl_->graphs.size();
    output.sessions = impl_->active.size();
    output.receipts = impl_->receipts.size();
    output.delivery_enabled = impl_->delivery.producer != 0;
    for (const auto& [index, session] : impl_->active) {
        static_cast<void>(index);
        if (!terminal(session.view.state)) { ++output.active; }
        output.pending_effects += session.view.pending_effects;
    }
    return output;
}
const DialogueLimits& DialogueService::limits() const noexcept { return impl_->limits; }
void DialogueService::update(const core::Frame& frame) {
    if (impl_->busy || impl_->stopped) { return; }
    MutationGuard guard(impl_->busy);
    impl_->tick = frame.tick;
    impl_->counters.invalid_delta = !std::isfinite(frame.delta_seconds) || frame.delta_seconds < 0.0;
    const auto delta = impl_->counters.invalid_delta ? 0.0 : std::min(frame.delta_seconds, impl_->limits.maximum_delta_seconds);
    std::size_t cleaned{};
    for (auto& [id, receipt] : impl_->receipts) {
        static_cast<void>(id);
        if (cleaned >= impl_->limits.steps_per_frame) { break; }
        if (receipt.view.cancellation_pending) { impl_->abandon(receipt); ++cleaned; }
    }
    for (auto& [index, session] : impl_->active) {
        static_cast<void>(index);
        if (terminal(session.view.state)) { continue; }
        session.view.elapsed_seconds = std::min(maximum_time, session.view.elapsed_seconds + delta);
        session.view.node_seconds = std::min(maximum_time, session.view.node_seconds + delta);
        const auto timeout = impl_->node(session).timeout_seconds;
        if (!impl_->actors_valid(session.view.context)) { impl_->finish(session, DialogueState::Cancelled, DialogueError::InvalidActor, "actor lifecycle changed"); continue; }
        if ((session.graph->timeout_seconds > 0.0 && session.view.elapsed_seconds >= session.graph->timeout_seconds) ||
            (timeout > 0.0 && session.view.node_seconds >= timeout)) { impl_->finish(session, DialogueState::TimedOut, DialogueError::None, "conversation or node timeout; completed effects retained"); continue; }
        if (!session.batch.empty()) {
            auto& receipt = impl_->receipts.at(session.batch.at(session.next_effect));
            receipt.retry_remaining = std::max(0.0, receipt.retry_remaining - delta);
            receipt.elapsed = std::min(maximum_time, receipt.elapsed + delta);
            if (receipt.elapsed >= impl_->limits.effect_timeout_seconds) { impl_->finish(session, DialogueState::TimedOut, DialogueError::None, "effect completion timeout; admitted effects may be partial"); }
        }
    }
    std::size_t steps{};
    std::size_t visited{};
    const auto candidates = impl_->active.size();
    for (; visited < candidates && steps < impl_->limits.steps_per_frame; ++visited) {
        auto found = impl_->active.upper_bound(impl_->round_robin);
        if (found == impl_->active.end()) { found = impl_->active.begin(); }
        impl_->round_robin = found->first;
        auto& session = found->second;
        if (terminal(session.view.state)) { continue; }
        ++steps;
        try { impl_->step(session); }
        catch (const std::exception&) { impl_->finish(session, DialogueState::Failed, DialogueError::AllocationFailure, "dialogue step failed; earlier completed effects retained"); }
    }
    impl_->counters.budget_exhausted = visited < candidates;
}
void DialogueService::shutdown() noexcept {
    if (impl_->busy || impl_->stopped) { return; }
    MutationGuard guard(impl_->busy);
    impl_->stopped = true;
    for (auto& [index, session] : impl_->active) {
        static_cast<void>(index);
        if (terminal(session.view.state)) { continue; }
        impl_->finish(session, DialogueState::Cancelled, DialogueError::None, "dialogue service shutdown");
    }
    for (auto& [id, receipt] : impl_->receipts) {
        static_cast<void>(id);
        if (receipt.view.cancellation_pending) { impl_->abandon(receipt); }
    }
}

void register_dialogue(core::Kernel& kernel) {
    if (kernel.services().find<DialogueService>()) { return; }
    auto* registry = kernel.services().find<ecs::Registry>();
    if (!registry) { kernel.diagnose(core::Diagnostic::Severity::Error, "gameplay.dialogue", "Registry is required"); return; }
    auto* service = &kernel.services().emplace<DialogueService>(kernel, *registry);
    const auto producer = kernel.config().integer("dialogue.delivery_producer", 0);
    const auto next = kernel.config().integer("dialogue.next_sequence", 1);
    if (producer >= 0 && next > 0) {
        const auto configured = service->configure_delivery({static_cast<std::uint64_t>(producer), static_cast<std::uint64_t>(next)});
        if (!configured) { kernel.diagnose(core::Diagnostic::Severity::Warning, "gameplay.dialogue", configured.message); }
    } else { kernel.diagnose(core::Diagnostic::Severity::Warning, "gameplay.dialogue", "invalid delivery configuration; durable delivery remains disabled"); }
    static_cast<void>(kernel.register_module({"gameplay.dialogue", "Bounded dialogue graphs, localized presentation and ordered effect completion", 1}));
    static_cast<void>(kernel.add_system({"gameplay.dialogue.update", core::Phase::Visibility, -6800, {}},
        [service](core::Kernel& host, const core::Frame& frame) {
            service->update(frame);
            const auto stats = service->statistics();
            host.metric("dialogue.active", static_cast<double>(stats.active));
            host.metric("dialogue.pending_effects", static_cast<double>(stats.pending_effects));
            host.metric("dialogue.delivery_enabled", stats.delivery_enabled ? 1.0 : 0.0);
            host.metric("dialogue.budget_exhausted", stats.budget_exhausted ? 1.0 : 0.0);
        }));
    static_cast<void>(kernel.add_shutdown("gameplay.dialogue.shutdown", [service] { service->shutdown(); }));
    const auto command_result = [](const DialogueResult& answer) {
        return core::CommandResult{static_cast<bool>(answer), std::string(describe(answer.error)) + ": " + answer.message +
            (answer.session ? " [" + session_name(answer.session) + "]" : "")};
    };
    static_cast<void>(kernel.add_command("dialogue.define", [service, command_result](core::Kernel&, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 3) { return core::CommandResult{false, "usage: dialogue.define graph fallback [localization-key]"}; }
        DialogueGraph graph;
        graph.id = args[0];
        graph.entry = "line";
        DialogueNode line;
        line.id = "line";
        line.text.fallback = args[1];
        if (args.size() == 3) { line.text.key = args[2]; }
        line.next = "end";
        DialogueNode end;
        end.id = "end";
        end.kind = DialogueNodeKind::End;
        graph.nodes = {std::move(line), std::move(end)};
        return command_result(service->define(std::move(graph)));
    }, "Define a localized line followed by a terminal node"));
    static_cast<void>(kernel.add_command("dialogue.start", [service, command_result](core::Kernel&, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 4) { return core::CommandResult{false, "usage: dialogue.start actor graph [partner|-] [locale]"}; }
        const auto actor = ecs::parse_entity(args[0]);
        const auto partner = args.size() >= 3 && args[2] != "-" ? ecs::parse_entity(args[2]) : std::optional<ecs::Entity>(ecs::Entity{});
        if (!actor || !partner) { return core::CommandResult{false, "invalid entity handle"}; }
        return command_result(service->start(*actor, *partner, args[1], args.size() == 4 ? args[3] : ""));
    }, "Start dialogue with existing Registry participants"));
    static_cast<void>(kernel.add_command("dialogue.advance", [service, command_result](core::Kernel&, core::CommandArgs args) {
        std::uint64_t revision{};
        const auto id = args.size() == 2 ? parse_session(args[0]) : std::nullopt;
        if (!id || !parse_unsigned(args[1], revision)) { return core::CommandResult{false, "usage: dialogue.advance scope:index:generation revision"}; }
        return command_result(service->advance(*id, revision));
    }, "Advance the exact displayed line"));
    static_cast<void>(kernel.add_command("dialogue.select", [service, command_result](core::Kernel&, core::CommandArgs args) {
        std::uint64_t revision{};
        const auto id = args.size() == 3 ? parse_session(args[0]) : std::nullopt;
        if (!id || !parse_unsigned(args[2], revision)) { return core::CommandResult{false, "usage: dialogue.select scope:index:generation choice revision"}; }
        return command_result(service->select(*id, args[1], revision));
    }, "Revalidate and select a displayed choice"));
    static_cast<void>(kernel.add_command("dialogue.cancel", [service, command_result](core::Kernel&, core::CommandArgs args) {
        const auto id = args.size() == 1 ? parse_session(args[0]) : std::nullopt;
        return id ? command_result(service->cancel(*id)) : core::CommandResult{false, "usage: dialogue.cancel scope:index:generation"};
    }, "Cancel dialogue and stop its owned pending script"));
    static_cast<void>(kernel.add_command("dialogue.release", [service, command_result](core::Kernel&, core::CommandArgs args) {
        const auto id = args.size() == 1 ? parse_session(args[0]) : std::nullopt;
        return id ? command_result(service->release(*id)) : core::CommandResult{false, "usage: dialogue.release scope:index:generation"};
    }, "Retire a terminal session and invalidate its generation"));
    static_cast<void>(kernel.add_command("dialogue.delivery", [service, command_result](core::Kernel&, core::CommandArgs args) {
        DialogueDeliveryCursor cursor;
        if (args.size() != 2 || !parse_unsigned(args[0], cursor.producer) || !parse_unsigned(args[1], cursor.next_sequence)) {
            return core::CommandResult{false, "usage: dialogue.delivery producer next-sequence"};
        }
        return command_result(service->configure_delivery(cursor));
    }, "Configure durable effect identity before delivery; zero disables"));
    static_cast<void>(kernel.add_command("dialogue.status", [service](core::Kernel&, core::CommandArgs args) {
        if (args.size() > 1) { return core::CommandResult{false, "usage: dialogue.status [scope:index:generation]"}; }
        std::vector<DialogueSession> selected;
        if (args.empty()) { selected = service->sessions(); }
        else {
            const auto id = parse_session(args[0]);
            const auto session = id ? service->inspect(*id) : std::nullopt;
            if (!session) { return core::CommandResult{false, "unknown conversation"}; }
            selected.push_back(*session);
        }
        const auto cursor = service->delivery_cursor();
        std::string output = "delivery=" + std::to_string(cursor.producer) + ":" + std::to_string(cursor.next_sequence);
        for (const auto& session : selected) {
            output += "\n" + session_name(session.context.session) + " " + session.context.graph + "/" + session.context.node + " " +
                std::string(describe(session.state)) + " revision=" + std::to_string(session.presentation_revision) + "\n" + session.line.text;
            for (const auto& choice : session.choices) { output += "\n  " + choice.id + (choice.enabled ? " enabled: " : " disabled: ") + choice.text.text + " " + choice.reason; }
            if (!session.message.empty()) { output += "\n" + session.message; }
        }
        return core::CommandResult{true, std::move(output)};
    }, "Read actual localized lines, explicit available choices and delivery cursor"));
}

} // namespace ge::gameplay
