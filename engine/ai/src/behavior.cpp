#include <ge/ai/behavior.hpp>
#include <ge/core/runtime.hpp>
#include <ge/navigation/pathfinding.hpp>
#include <ge/physics/character.hpp>
#include <ge/scene/partition.hpp>
#include <ge/script/vm.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace ge::ai {
namespace {
constexpr auto last_revision = std::numeric_limits<std::uint64_t>::max();
bool valid_text(std::string_view text, std::size_t limit, bool empty = false) {
    return (empty || !text.empty()) && text.size() <= limit &&
        std::none_of(text.begin(), text.end(), [](char character) {
            return static_cast<unsigned char>(character) < 32U;
        });
}
bool valid_value(const BlackboardValue& value, std::size_t string_limit) {
    if (value.valueless_by_exception()) { return false; }
    return std::visit([string_limit](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, double>) { return std::isfinite(item); }
        else if constexpr (std::is_same_v<T, math::Vec3>) { return math::finite(item); }
        else if constexpr (std::is_same_v<T, AbsolutePosition>) { return math::finite(item.value); }
        else if constexpr (std::is_same_v<T, std::string>) { return item.size() <= string_limit; }
        else { return true; }
    }, value);
}
bool terminal(BehaviorStatus status) {
    return status == BehaviorStatus::Success || status == BehaviorStatus::Failure;
}
void increment(std::uint64_t& value) {
    if (value != last_revision) { ++value; }
}
struct BusyGuard {
    bool& flag;
    explicit BusyGuard(bool& value) : flag(value) { flag = true; }
    ~BusyGuard() { flag = false; }
};
template<class T>
bool parse(std::string_view text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) { return false; }
    if constexpr (std::is_floating_point_v<T>) { return std::isfinite(value); }
    return true;
}
ecs::Entity resolve(core::Kernel& kernel, std::string_view path) {
    auto* active_scene = kernel.services().find<scene::Scene>();
    if (active_scene) { return active_scene->find_path(path).entity; }
    auto* registry = kernel.services().find<ecs::Registry>();
    return registry ? registry->find_name(path) : ecs::Entity{};
}
}

BlackboardType value_type(const BlackboardValue& value) noexcept {
    return static_cast<BlackboardType>(value.index());
}
Blackboard::Blackboard(BlackboardLimits limits) : limits_(limits) {
    if (limits.entries == 0 || limits.entries > 4096 || limits.key_bytes == 0 ||
        limits.key_bytes > 4096 || limits.string_bytes > 1048576 ||
        limits.history == 0 || limits.history > 65536) {
        throw std::invalid_argument("invalid blackboard limits");
    }
}
bool Blackboard::write(std::string_view key, BlackboardValue value) {
    if (!valid_text(key, limits_.key_bytes) || !valid_value(value, limits_.string_bytes)) { return false; }
    const auto found = values_.find(key);
    if (found != values_.end()) {
        if (found->second.index() != value.index()) { return false; }
        if (found->second == value) { return true; }
    } else if (values_.size() == limits_.entries) { return false; }
    if (revision_ == last_revision) { return false; }
    history_.push_back({revision_ + 1, std::string(key), false});
    try {
        if (found == values_.end()) { values_.emplace(key, std::move(value)); }
        else { found->second.swap(value); }
    } catch (...) {
        history_.pop_back();
        throw;
    }
    ++revision_;
    if (history_.size() > limits_.history) { history_.pop_front(); }
    return true;
}
const BlackboardValue* Blackboard::find(std::string_view key) const noexcept {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : &found->second;
}
bool Blackboard::contains(std::string_view key) const noexcept { return find(key) != nullptr; }
bool Blackboard::erase(std::string_view key) {
    const auto found = values_.find(key);
    if (found == values_.end() || revision_ == last_revision) { return false; }
    history_.push_back({revision_ + 1, std::string(key), true});
    values_.erase(found);
    ++revision_;
    if (history_.size() > limits_.history) { history_.pop_front(); }
    return true;
}
bool Blackboard::clear() {
    if (values_.size() > last_revision - revision_) { return false; }
    while (!values_.empty()) { (void)erase(values_.begin()->first); }
    return true;
}
std::size_t Blackboard::size() const noexcept { return values_.size(); }
std::uint64_t Blackboard::revision() const noexcept { return revision_; }
const BlackboardLimits& Blackboard::limits() const noexcept { return limits_; }
std::vector<std::pair<std::string, BlackboardValue>> Blackboard::snapshot() const {
    return {values_.begin(), values_.end()};
}
BlackboardHistory Blackboard::changes_since(std::uint64_t revision, std::size_t limit) const {
    BlackboardHistory result;
    result.newest_revision = revision_;
    result.history_lost = revision > revision_ ||
        (!history_.empty() && revision < history_.front().revision - 1);
    limit = std::min(limit, limits_.history);
    for (const auto& change : history_) {
        if (change.revision > revision && result.changes.size() < limit) {
            result.changes.push_back(change);
        }
    }
    return result;
}
std::string_view describe(BehaviorStatus status) noexcept {
    switch (status) {
    case BehaviorStatus::Idle: return "idle";
    case BehaviorStatus::Running: return "running";
    case BehaviorStatus::Success: return "success";
    case BehaviorStatus::Failure: return "failure";
    case BehaviorStatus::Aborted: return "aborted";
    }
    return "invalid";
}
std::string_view describe(AbortReason reason) noexcept {
    switch (reason) {
    case AbortReason::Completed: return "completed";
    case AbortReason::Requested: return "requested";
    case AbortReason::Detached: return "detached";
    case AbortReason::Disabled: return "disabled";
    case AbortReason::EntityLost: return "entity lost";
    case AbortReason::Preempted: return "preempted";
    case AbortReason::Timeout: return "timeout";
    case AbortReason::OriginChanged: return "origin changed";
    case AbortReason::OwnershipLost: return "ownership lost";
    case AbortReason::CallbackFault: return "callback fault";
    case AbortReason::Shutdown: return "shutdown";
    }
    return "invalid";
}
BehaviorTree::BehaviorTree(BehaviorTreeData data) : data_(std::move(data)) {}
const BehaviorTreeData& BehaviorTree::data() const noexcept { return data_; }
BehaviorTreeResult BehaviorTree::create(BehaviorTreeData data, BehaviorTreeLimits limits) {
    const auto reject = [](BehaviorNodeId node, std::string message) {
        return BehaviorTreeResult{{}, {node, std::move(message)}};
    };
    if (limits.nodes == 0 || limits.nodes > 4096 || limits.depth == 0 || limits.depth > 128 ||
        limits.children == 0 || limits.children > 256 || limits.text_bytes == 0 ||
        limits.text_bytes > 65536 || limits.repetitions == 0 || limits.repetitions > 1000000) {
        return reject(invalid_behavior_node, "invalid definition limits");
    }
    if (data.nodes.empty() || data.nodes.size() > limits.nodes || data.root >= data.nodes.size()) {
        return reject(data.root, "invalid node count or root");
    }
    std::vector<std::size_t> parents(data.nodes.size());
    for (std::size_t offset = 0; offset < data.nodes.size(); ++offset) {
        const auto id = static_cast<BehaviorNodeId>(offset);
        const auto& node = data.nodes[offset];
        if (node.kind > BehaviorNodeKind::Custom || node.comparison > BehaviorComparison::WithinDistance ||
            !valid_text(node.name, limits.text_bytes, true) ||
            !valid_text(node.key, limits.text_bytes, true) ||
            !valid_text(node.operation, limits.text_bytes, true) ||
            !valid_value(node.value, limits.text_bytes)) { return reject(id, "invalid node data"); }
        if (!std::isfinite(node.seconds) || node.seconds < 0.0 || node.seconds > 1.0e9 ||
            !std::isfinite(node.speed) || node.speed < 0.0 || node.speed > 1000000.0 ||
            !std::isfinite(node.tolerance) || node.tolerance < 0.0 ||
            !std::isfinite(node.snap_distance) || node.snap_distance < 0.0 ||
            node.count == 0 || node.count > limits.repetitions) { return reject(id, "invalid node parameters"); }
        const bool composite = node.kind <= BehaviorNodeKind::ParallelAll;
        const bool decorator = node.kind >= BehaviorNodeKind::Invert && node.kind <= BehaviorNodeKind::Cooldown;
        if ((composite && (node.children.empty() || node.children.size() > limits.children)) ||
            (decorator && node.children.size() != 1) ||
            (!composite && !decorator && !node.children.empty())) { return reject(id, "invalid child arity"); }
        const bool needs_key = node.kind == BehaviorNodeKind::Condition ||
            node.kind == BehaviorNodeKind::SetBlackboard || node.kind == BehaviorNodeKind::EraseBlackboard ||
            node.kind == BehaviorNodeKind::MoveTo;
        if (needs_key && node.key.empty()) { return reject(id, "node requires a blackboard key"); }
        if ((node.kind == BehaviorNodeKind::Script || node.kind == BehaviorNodeKind::Custom) &&
            node.operation.empty()) { return reject(id, "action requires an operation name"); }
        if ((node.kind == BehaviorNodeKind::MoveTo || node.kind == BehaviorNodeKind::Script ||
             node.kind == BehaviorNodeKind::Timeout) && node.seconds <= 0.0) {
            return reject(id, "asynchronous action or timeout requires a positive deadline");
        }
        for (const auto child : node.children) {
            if (child >= data.nodes.size() || child == id || ++parents[child] != 1) {
                return reject(id, "child is invalid, repeated, or shared");
            }
        }
        if (node.kind == BehaviorNodeKind::ReactiveSequence) {
            for (std::size_t index = 0; index + 1 < node.children.size(); ++index) {
                if (data.nodes[node.children[index]].kind != BehaviorNodeKind::Condition) {
                    return reject(id, "reactive prefixes must be side-effect-free conditions");
                }
            }
        }
    }
    if (parents[data.root] != 0) { return reject(data.root, "root has a parent"); }
    std::vector<std::pair<BehaviorNodeId, std::size_t>> pending{{data.root, 1}};
    std::vector<bool> seen(data.nodes.size());
    std::size_t visited = 0;
    while (!pending.empty()) {
        const auto [id, depth] = pending.back();
        pending.pop_back();
        if (seen[id] || depth > limits.depth) { return reject(id, "cycle or excessive depth"); }
        seen[id] = true;
        ++visited;
        for (const auto child : data.nodes[id].children) { pending.emplace_back(child, depth + 1); }
    }
    if (visited != data.nodes.size()) { return reject(data.root, "unreachable nodes or disconnected cycle"); }
    return {std::shared_ptr<const BehaviorTree>(new BehaviorTree(std::move(data))), {}};
}

struct BehaviorService::Impl {
    struct NodeState {
        BehaviorStatus status{BehaviorStatus::Idle};
        std::size_t cursor{};
        std::uint32_t iterations{};
        double started{};
        double cooldown_until{};
        navigation::RequestId request{};
        script::InstanceHandle script{};
        physics::BodyHandle character_body{};
        const navigation::PathFollower* follower{};
        const math::Vec3* path_storage{};
        std::size_t path_size{};
        std::uint64_t graph_revision{};
        scene::OriginState origin{};
        bool navigation_claim{};
        bool entered{};
        std::uint64_t memory{};
        std::shared_ptr<const BehaviorAction> action;
    };
    struct Record {
        ecs::Entity entity;
        std::uint64_t attachment;
        std::string name;
        std::shared_ptr<const BehaviorTree> tree;
        Blackboard board;
        std::vector<NodeState> nodes;
        BehaviorStatus status{BehaviorStatus::Idle};
        AbortReason reason{AbortReason::Completed};
        BehaviorNodeId motion{invalid_behavior_node};
        std::uint64_t last_tick{};
        bool evaluated{};
        bool enabled{true};
        bool repeat{true};
        Record(ecs::Entity owner, std::uint64_t token, std::string tree_name,
               std::shared_ptr<const BehaviorTree> definition, BlackboardLimits board_limits)
            : entity(owner), attachment(token), name(std::move(tree_name)),
              tree(std::move(definition)), board(board_limits), nodes(tree->data().nodes.size()) {}
    };
    core::Kernel& kernel;
    BehaviorLimits limits;
    std::map<std::string, std::shared_ptr<const BehaviorTree>, std::less<>> trees;
    std::map<std::string, std::shared_ptr<const BehaviorAction>, std::less<>> actions;
    std::map<ecs::Entity, std::unique_ptr<Record>> records;
    std::deque<BehaviorEvent> history;
    BehaviorStatistics statistics;
    core::Frame frame{};
    std::uint64_t next_attachment{1};
    std::uint64_t event_sequence{};
    ecs::Entity last_evaluated{};
    double clock{};
    bool busy{};
    bool closed{};
    bool advanced{};
    Impl(core::Kernel& host, BehaviorLimits configuration) : kernel(host), limits(configuration) {
        const Blackboard validate(limits.blackboard);
        (void)validate;
        if (limits.attachments == 0 || limits.attachments > 65536 || limits.trees == 0 ||
            limits.trees > 4096 || limits.actions > 4096 || limits.node_steps_per_agent == 0 ||
            limits.node_steps_per_agent > 1048576 || limits.node_steps_per_frame == 0 ||
            limits.node_steps_per_frame > 16777216 || limits.agents_per_frame == 0 ||
            limits.graph_scan == 0 || limits.graph_scan > 1048576 || limits.history == 0 ||
            limits.history > 1048576) { throw std::invalid_argument("invalid behavior limits"); }
    }
    ecs::Registry* registry() const noexcept { return kernel.services().find<ecs::Registry>(); }
    bool owns(const Record& record) const noexcept {
        const auto* owner = registry();
        const auto* marker = owner ? owner->get<BehaviorComponent>(record.entity) : nullptr;
        return marker && marker->attachment == record.attachment;
    }
    Record* find(ecs::Entity entity) const noexcept {
        const auto found = records.find(entity);
        return found == records.end() || !owns(*found->second) ? nullptr : found->second.get();
    }
    scene::OriginState coordinates() const noexcept {
        const auto* partition = kernel.services().find<scene::WorldPartition>();
        return partition ? partition->origin() : scene::OriginState{};
    }
    std::optional<math::Vec3> entity_position(ecs::Entity entity) const {
        auto* active_scene = kernel.services().find<scene::Scene>();
        if (!active_scene || !active_scene->registry().alive(entity)) { return std::nullopt; }
        const auto matrix = active_scene->world_matrix(entity);
        if (!matrix) { return std::nullopt; }
        const math::Vec3 position{(*matrix)(0, 3), (*matrix)(1, 3), (*matrix)(2, 3)};
        return math::finite(position) ? std::optional{position} : std::nullopt;
    }
    std::optional<math::Vec3> target(const Blackboard& board, std::string_view key) const {
        if (!coordinates().consistent) { return std::nullopt; }
        if (const auto* entity = board.get<ecs::Entity>(key)) { return entity_position(*entity); }
        const auto* absolute = board.get<AbsolutePosition>(key);
        if (!absolute) { return std::nullopt; }
        const auto* partition = kernel.services().find<scene::WorldPartition>();
        return partition ? partition->to_local(absolute->value) : std::optional{absolute->value};
    }
    void publish(Record& record) {
        if (auto* owner = registry(); owner && owns(record)) {
            auto* marker = owner->get<BehaviorComponent>(record.entity);
            marker->status = record.status;
            marker->enabled = record.enabled;
            (void)owner->mark_changed<BehaviorComponent>(record.entity);
        }
    }
    void emit(const Record& record) {
        if (event_sequence == last_revision) { return; }
        history.push_back({event_sequence + 1, record.entity, record.status, record.reason, frame.tick});
        ++event_sequence;
        if (history.size() > limits.history) { history.pop_front(); }
    }
    BehaviorActionContext context(Record& record, BehaviorNodeId id) {
        return {kernel, record.entity, record.board, frame, id, record.nodes[id].memory};
    }
    bool follower_owned(const Record& record, BehaviorNodeId id) const noexcept {
        const auto& state = record.nodes[id];
        const auto* owner = registry();
        const auto* follower = owner ? owner->get<navigation::PathFollower>(record.entity) : nullptr;
        const auto& node = record.tree->data().nodes[id];
        return state.follower && follower == state.follower &&
            follower->points.data() == state.path_storage && follower->points.size() == state.path_size &&
            follower->graph_revision == state.graph_revision && follower->speed == node.speed &&
            follower->stopping_distance == node.tolerance;
    }
    bool control_owned(const Record& record, BehaviorNodeId id) const noexcept {
        const auto& state = record.nodes[id];
        const auto* characters = kernel.services().find<physics::CharacterSystem>();
        const auto* character = characters ? characters->get(record.entity) : nullptr;
        const auto* owner = registry();
        const bool matching_path = state.follower ? follower_owned(record, id) :
            owner && !owner->has<navigation::PathFollower>(record.entity);
        return state.navigation_claim && record.motion == id && character && matching_path &&
            character->body == state.character_body &&
            character->state.authority == physics::CharacterAuthority::Navigation;
    }
    void release(Record& record, BehaviorNodeId id, BehaviorStatus outcome, AbortReason reason) {
        auto& state = record.nodes[id];
        if (state.request != 0) {
            if (auto* navigator = kernel.services().find<navigation::Navigator>()) {
                (void)navigator->cancel(state.request);
                (void)navigator->take_result(state.request);
            }
            state.request = 0;
        }
        const bool restore = control_owned(record, id);
        if (follower_owned(record, id)) {
            auto* owner = registry();
            auto* follower = owner->get<navigation::PathFollower>(record.entity);
            follower->pause = true;
            follower->desired_velocity = {};
            (void)owner->remove<navigation::PathFollower>(record.entity);
        }
        state.follower = nullptr;
        if (restore) {
            if (auto* characters = kernel.services().find<physics::CharacterSystem>()) {
                (void)characters->set_authority(record.entity, physics::CharacterAuthority::Manual);
            }
        }
        state.navigation_claim = false;
        if (record.motion == id) { record.motion = invalid_behavior_node; }
        if (state.script) {
            if (auto* scripts = kernel.services().find<script::ScriptService>()) {
                (void)scripts->stop(state.script);
                (void)scripts->release(state.script);
            }
            state.script = {};
        }
        if (state.entered && state.action) {
            state.entered = false;
            const auto pinned = std::move(state.action);
            try {
                auto action_context = context(record, id);
                pinned->exit(action_context, outcome, reason);
            } catch (...) { increment(statistics.callback_faults); }
        }
    }
    void reset(Record& record, BehaviorNodeId id, AbortReason reason) {
        for (const auto child : record.tree->data().nodes[id].children) { reset(record, child, reason); }
        auto& state = record.nodes[id];
        release(record, id, BehaviorStatus::Aborted, reason);
        const double cooldown = state.cooldown_until;
        state = NodeState{};
        state.cooldown_until = cooldown;
    }
    void stop(Record& record, AbortReason reason, BehaviorStatus outcome = BehaviorStatus::Aborted) {
        reset(record, record.tree->data().root, reason);
        record.nodes[record.tree->data().root].status = outcome;
        record.status = outcome;
        record.reason = reason;
        publish(record);
        emit(record);
    }
    BehaviorStatus finish(Record& record, BehaviorNodeId id, BehaviorStatus outcome,
                          AbortReason reason = AbortReason::Completed) {
        for (const auto child : record.tree->data().nodes[id].children) {
            if (record.nodes[child].status == BehaviorStatus::Running) {
                reset(record, child, reason == AbortReason::Completed ? AbortReason::Preempted : reason);
            }
        }
        release(record, id, outcome, reason);
        record.nodes[id].status = outcome;
        if (reason != AbortReason::Completed) { record.reason = reason; }
        return outcome;
    }
    bool condition(const Record& record, const BehaviorNode& node) const {
        const auto* value = record.board.find(node.key);
        if (node.comparison == BehaviorComparison::Missing) { return value == nullptr; }
        if (node.comparison == BehaviorComparison::Exists) { return value != nullptr; }
        if (!value) { return false; }
        if (node.comparison == BehaviorComparison::EntityAlive) {
            const auto* entity = std::get_if<ecs::Entity>(value);
            const auto* owner = registry();
            return entity && owner && owner->alive(*entity);
        }
        if (node.comparison == BehaviorComparison::WithinDistance) {
            const auto destination = target(record.board, node.key);
            const auto position = entity_position(record.entity);
            return destination && position && math::distance(*destination, *position) <= node.tolerance;
        }
        if (value->index() != node.value.index()) { return false; }
        if (node.comparison == BehaviorComparison::Equal) { return *value == node.value; }
        if (node.comparison == BehaviorComparison::NotEqual) { return *value != node.value; }
        const auto comparison = std::visit([&node](const auto& first) -> std::optional<int> {
            using T = std::decay_t<decltype(first)>;
            if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double> ||
                          std::is_same_v<T, std::string>) {
                const auto& second = std::get<T>(node.value);
                return first < second ? -1 : (second < first ? 1 : 0);
            } else { return std::nullopt; }
        }, *value);
        if (!comparison) { return false; }
        switch (node.comparison) {
        case BehaviorComparison::Less: return *comparison < 0;
        case BehaviorComparison::LessEqual: return *comparison <= 0;
        case BehaviorComparison::Greater: return *comparison > 0;
        case BehaviorComparison::GreaterEqual: return *comparison >= 0;
        default: return false;
        }
    }
    BehaviorStatus move(Record& record, BehaviorNodeId id) {
        auto& state = record.nodes[id];
        const auto& node = record.tree->data().nodes[id];
        auto* owner = registry();
        auto* navigator = kernel.services().find<navigation::Navigator>();
        auto* characters = kernel.services().find<physics::CharacterSystem>();
        if (!owner || !navigator || !characters) { return BehaviorStatus::Failure; }
        if (clock - state.started >= node.seconds) { record.reason = AbortReason::Timeout; return BehaviorStatus::Failure; }
        if (!state.navigation_claim) {
            const auto* character = characters->get(record.entity);
            const auto destination = target(record.board, node.key);
            const auto position = entity_position(record.entity);
            if (!character || !character->state.enabled || !destination || !position ||
                character->state.authority != physics::CharacterAuthority::Manual ||
                owner->has<navigation::PathFollower>(record.entity) || record.motion != invalid_behavior_node) {
                return BehaviorStatus::Failure;
            }
            const auto start = navigator->graph().nearest(*position, node.snap_distance, limits.graph_scan);
            const auto goal = navigator->graph().nearest(*destination, node.snap_distance, limits.graph_scan);
            if (!start || !goal) { return BehaviorStatus::Failure; }
            state.character_body = character->body;
            if (!characters->set_authority(record.entity, physics::CharacterAuthority::Navigation)) { return BehaviorStatus::Failure; }
            state.navigation_claim = true;
            record.motion = id;
            state.origin = coordinates();
            navigation::SearchOptions options;
            options.max_storage_nodes = limits.graph_scan;
            options.max_visited = limits.graph_scan;
            options.max_output = limits.graph_scan;
            state.request = navigator->submit(start, goal, options);
            return state.request == 0 ? BehaviorStatus::Failure : BehaviorStatus::Running;
        }
        if (!control_owned(record, id)) { record.reason = AbortReason::OwnershipLost; return BehaviorStatus::Failure; }
        if (!target(record.board, node.key)) { return BehaviorStatus::Failure; }
        if (state.request != 0) {
            auto result = navigator->take_result(state.request);
            if (!result) { return BehaviorStatus::Running; }
            state.request = 0;
            if (!*result || owner->has<navigation::PathFollower>(record.entity) ||
                !navigator->follow(*owner, record.entity, *result, node.speed)) { return BehaviorStatus::Failure; }
            auto* follower = owner->get<navigation::PathFollower>(record.entity);
            follower->stopping_distance = node.tolerance;
            state.follower = follower;
            state.path_storage = follower->points.data();
            state.path_size = follower->points.size();
            state.graph_revision = follower->graph_revision;
            (void)owner->mark_changed<navigation::PathFollower>(record.entity);
            return BehaviorStatus::Running;
        }
        const auto* follower = owner->get<navigation::PathFollower>(record.entity);
        if (!follower || !follower_owned(record, id)) { return BehaviorStatus::Failure; }
        switch (follower->state) {
        case navigation::FollowerState::Arrived: return BehaviorStatus::Success;
        case navigation::FollowerState::Following:
        case navigation::FollowerState::ControllerRequired: return BehaviorStatus::Running;
        default: return BehaviorStatus::Failure;
        }
    }
    BehaviorStatus script_action(Record& record, BehaviorNodeId id) {
        auto& state = record.nodes[id];
        const auto& node = record.tree->data().nodes[id];
        auto* scripts = kernel.services().find<script::ScriptService>();
        if (!scripts) { return BehaviorStatus::Failure; }
        if (clock - state.started >= node.seconds) { record.reason = AbortReason::Timeout; return BehaviorStatus::Failure; }
        if (!state.script) {
            state.script = scripts->start(node.operation, record.entity);
            return state.script ? BehaviorStatus::Running : BehaviorStatus::Failure;
        }
        const auto instance = scripts->inspect(state.script);
        if (!instance || instance->owner != record.entity) { return BehaviorStatus::Failure; }
        if (instance->state == script::ExecutionState::Faulted || instance->state == script::ExecutionState::Stopped) {
            return BehaviorStatus::Failure;
        }
        if (instance->state != script::ExecutionState::Completed) { return BehaviorStatus::Running; }
        if (!node.key.empty()) {
            const bool written = std::visit([&record, &node](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::monostate>) { return !record.board.contains(node.key) || record.board.erase(node.key); }
                else { return record.board.set(node.key, value); }
            }, instance->result.data);
            if (!written) { return BehaviorStatus::Failure; }
        }
        const auto* result = std::get_if<bool>(&instance->result.data);
        return result && !*result ? BehaviorStatus::Failure : BehaviorStatus::Success;
    }
    BehaviorStatus tick(Record& record, BehaviorNodeId id, std::size_t& budget) {
        if (budget == 0) { return BehaviorStatus::Running; }
        --budget;
        auto& state = record.nodes[id];
        const auto& node = record.tree->data().nodes[id];
        if (terminal(state.status)) { return state.status; }
        if (state.status != BehaviorStatus::Running) {
            state.status = BehaviorStatus::Running;
            state.started = clock;
        }
        const auto complete = [&](BehaviorStatus outcome) {
            const auto reason = outcome == BehaviorStatus::Success ? AbortReason::Completed : record.reason;
            return terminal(outcome) ? finish(record, id, outcome, reason) : outcome;
        };
        switch (node.kind) {
        case BehaviorNodeKind::Sequence:
        case BehaviorNodeKind::Selector: {
            const bool sequence = node.kind == BehaviorNodeKind::Sequence;
            while (state.cursor < node.children.size()) {
                const auto result = tick(record, node.children[state.cursor], budget);
                if (result == BehaviorStatus::Running) { return result; }
                if ((result == BehaviorStatus::Failure) == sequence) { return complete(result); }
                ++state.cursor;
            }
            return complete(sequence ? BehaviorStatus::Success : BehaviorStatus::Failure);
        }
        case BehaviorNodeKind::ReactiveSequence: {
            const auto last = node.children.back();
            for (std::size_t index = 0; index + 1 < node.children.size(); ++index) {
                const auto child = node.children[index];
                reset(record, child, AbortReason::Preempted);
                const auto result = tick(record, child, budget);
                if (result != BehaviorStatus::Success) {
                    reset(record, last, AbortReason::Preempted);
                    return complete(result);
                }
            }
            return complete(tick(record, last, budget));
        }
        case BehaviorNodeKind::ParallelAll: {
            bool all = true;
            for (const auto child : node.children) {
                const auto result = tick(record, child, budget);
                if (result == BehaviorStatus::Failure) { return complete(result); }
                all = all && result == BehaviorStatus::Success;
            }
            return all ? complete(BehaviorStatus::Success) : BehaviorStatus::Running;
        }
        case BehaviorNodeKind::Invert:
        case BehaviorNodeKind::ForceSuccess:
        case BehaviorNodeKind::ForceFailure: {
            const auto result = tick(record, node.children.front(), budget);
            if (result == BehaviorStatus::Running) { return result; }
            if (node.kind == BehaviorNodeKind::ForceSuccess) { return complete(BehaviorStatus::Success); }
            if (node.kind == BehaviorNodeKind::ForceFailure) { return complete(BehaviorStatus::Failure); }
            return complete(result == BehaviorStatus::Success ? BehaviorStatus::Failure : BehaviorStatus::Success);
        }
        case BehaviorNodeKind::Repeat:
        case BehaviorNodeKind::Retry: {
            const auto child = node.children.front();
            const auto result = tick(record, child, budget);
            if (result == BehaviorStatus::Running) { return result; }
            const bool repeat = node.kind == BehaviorNodeKind::Repeat;
            if ((result == BehaviorStatus::Failure) == repeat) { return complete(result); }
            ++state.iterations;
            if (state.iterations == node.count) { return complete(result); }
            reset(record, child, AbortReason::Completed);
            return BehaviorStatus::Running;
        }
        case BehaviorNodeKind::Timeout:
            if (clock - state.started >= node.seconds) {
                record.reason = AbortReason::Timeout;
                reset(record, node.children.front(), AbortReason::Timeout);
                return complete(BehaviorStatus::Failure);
            }
            return complete(tick(record, node.children.front(), budget));
        case BehaviorNodeKind::Cooldown: {
            if (state.cursor == 0) {
                if (clock < state.cooldown_until) { return complete(BehaviorStatus::Failure); }
                state.cursor = 1;
            }
            const auto result = tick(record, node.children.front(), budget);
            if (terminal(result)) { state.cooldown_until = clock + node.seconds; }
            return complete(result);
        }
        case BehaviorNodeKind::Wait:
            return clock - state.started >= node.seconds ? complete(BehaviorStatus::Success) : BehaviorStatus::Running;
        case BehaviorNodeKind::Condition:
            return complete(condition(record, node) ? BehaviorStatus::Success : BehaviorStatus::Failure);
        case BehaviorNodeKind::SetBlackboard:
            return complete(record.board.write(node.key, node.value) ? BehaviorStatus::Success : BehaviorStatus::Failure);
        case BehaviorNodeKind::EraseBlackboard:
            return complete(!record.board.contains(node.key) || record.board.erase(node.key) ?
                            BehaviorStatus::Success : BehaviorStatus::Failure);
        case BehaviorNodeKind::MoveTo: return complete(move(record, id));
        case BehaviorNodeKind::Script: return complete(script_action(record, id));
        case BehaviorNodeKind::Custom:
            try {
                if (!state.action) {
                    const auto action = actions.find(node.operation);
                    if (action == actions.end()) { return complete(BehaviorStatus::Failure); }
                    state.action = action->second;
                    state.entered = true;
                    auto action_context = context(record, id);
                    if (state.action->enter && !state.action->enter(action_context)) {
                        return complete(BehaviorStatus::Failure);
                    }
                }
                auto action_context = context(record, id);
                const auto result = state.action->tick(action_context);
                if (result != BehaviorStatus::Running && !terminal(result)) {
                    increment(statistics.callback_faults);
                    record.reason = AbortReason::CallbackFault;
                    return complete(BehaviorStatus::Failure);
                }
                return complete(result);
            } catch (...) {
                increment(statistics.callback_faults);
                record.reason = AbortReason::CallbackFault;
                return complete(BehaviorStatus::Failure);
            }
        }
        return complete(BehaviorStatus::Failure);
    }
};

BehaviorService::BehaviorService(core::Kernel& kernel, BehaviorLimits limits)
    : impl_(std::make_unique<Impl>(kernel, limits)) {}
BehaviorService::~BehaviorService() = default;
const BehaviorLimits& BehaviorService::limits() const noexcept { return impl_->limits; }
bool BehaviorService::install(std::string name, std::shared_ptr<const BehaviorTree> definition) {
    auto& self = *impl_;
    if (self.busy || self.closed || !definition || !valid_text(name, self.limits.tree.text_bytes) ||
        definition->data().nodes.size() > self.limits.node_steps_per_agent ||
        definition->data().nodes.size() > self.limits.node_steps_per_frame ||
        (!self.trees.contains(name) && self.trees.size() == self.limits.trees)) { return false; }
    auto checked = BehaviorTree::create(definition->data(), self.limits.tree);
    if (!checked) { return false; }
    self.trees.insert_or_assign(std::move(name), std::move(checked.tree));
    return true;
}
bool BehaviorService::unload(std::string_view name) {
    if (impl_->busy || impl_->closed) { return false; }
    const auto found = impl_->trees.find(name);
    if (found == impl_->trees.end()) { return false; }
    impl_->trees.erase(found);
    return true;
}
std::shared_ptr<const BehaviorTree> BehaviorService::tree(std::string_view name) const {
    const auto found = impl_->trees.find(name);
    return found == impl_->trees.end() ? nullptr : found->second;
}
bool BehaviorService::register_action(std::string name, BehaviorAction action) {
    auto& self = *impl_;
    if (self.busy || self.closed || !action.tick || !action.exit ||
        !valid_text(name, self.limits.tree.text_bytes) ||
        (!self.actions.contains(name) && self.actions.size() == self.limits.actions)) { return false; }
    self.actions.insert_or_assign(std::move(name), std::make_shared<const BehaviorAction>(std::move(action)));
    return true;
}
bool BehaviorService::unregister_action(std::string_view name) {
    if (impl_->busy || impl_->closed) { return false; }
    const auto found = impl_->actions.find(name);
    if (found == impl_->actions.end()) { return false; }
    impl_->actions.erase(found);
    return true;
}
bool BehaviorService::attach(ecs::Entity entity, std::string_view name, bool repeat) {
    auto& self = *impl_;
    auto* registry = self.registry();
    const auto definition = tree(name);
    if (self.busy || self.closed || !registry || registry->iterating() || !registry->alive(entity) ||
        !definition || self.records.contains(entity) || registry->has<BehaviorComponent>(entity) ||
        self.records.size() >= self.limits.attachments || self.next_attachment == last_revision) { return false; }
    auto record = std::make_unique<Impl::Record>(entity, self.next_attachment, std::string(name), definition, self.limits.blackboard);
    record->repeat = repeat;
    const auto inserted = self.records.emplace(entity, std::move(record));
    try { registry->emplace<BehaviorComponent>(entity, BehaviorComponent{self.next_attachment, BehaviorStatus::Idle, true}); }
    catch (...) { self.records.erase(inserted.first); throw; }
    ++self.next_attachment;
    self.statistics.attachments = self.records.size();
    self.emit(*inserted.first->second);
    return true;
}
bool BehaviorService::detach(ecs::Entity entity) {
    auto& self = *impl_;
    if (self.busy || self.closed) { return false; }
    const auto found = self.records.find(entity);
    if (found == self.records.end()) { return false; }
    BusyGuard guard(self.busy);
    self.stop(*found->second, AbortReason::Detached);
    if (self.owns(*found->second)) { (void)self.registry()->remove<BehaviorComponent>(entity); }
    self.records.erase(found);
    self.statistics.attachments = self.records.size();
    return true;
}
bool BehaviorService::abort(ecs::Entity entity, AbortReason reason) {
    auto& self = *impl_;
    auto* record = self.find(entity);
    if (self.busy || self.closed || !record || reason > AbortReason::Shutdown) { return false; }
    BusyGuard guard(self.busy);
    record->enabled = false;
    self.stop(*record, reason);
    return true;
}
bool BehaviorService::restart(ecs::Entity entity) {
    auto& self = *impl_;
    auto* record = self.find(entity);
    if (self.busy || self.closed || !record) { return false; }
    BusyGuard guard(self.busy);
    self.reset(*record, record->tree->data().root, AbortReason::Requested);
    record->status = BehaviorStatus::Idle;
    record->reason = AbortReason::Completed;
    record->enabled = true;
    self.publish(*record);
    self.emit(*record);
    return true;
}
bool BehaviorService::set_enabled(ecs::Entity entity, bool enabled) {
    auto* record = impl_->find(entity);
    if (!record || impl_->busy || impl_->closed) { return false; }
    if (record->enabled == enabled) { return true; }
    return enabled ? restart(entity) : abort(entity, AbortReason::Disabled);
}
Blackboard* BehaviorService::blackboard(ecs::Entity entity) noexcept {
    auto* record = impl_->find(entity);
    return record ? &record->board : nullptr;
}
const Blackboard* BehaviorService::blackboard(ecs::Entity entity) const noexcept {
    const auto* record = impl_->find(entity);
    return record ? &record->board : nullptr;
}
std::optional<BehaviorSnapshot> BehaviorService::inspect(ecs::Entity entity) const {
    const auto& self = *impl_;
    const auto* record = self.find(entity);
    if (!record) { return std::nullopt; }
    BehaviorSnapshot result;
    result.entity = entity;
    result.tree = record->name;
    result.status = record->status;
    result.reason = record->reason;
    result.attachment = record->attachment;
    result.last_tick = record->last_tick;
    result.enabled = record->enabled;
    result.repeat = record->repeat;
    result.nodes.reserve(record->nodes.size());
    for (std::size_t index = 0; index < record->nodes.size(); ++index) {
        const auto& state = record->nodes[index];
        result.nodes.push_back({static_cast<BehaviorNodeId>(index), state.status, state.cursor,
            state.iterations, state.status == BehaviorStatus::Running ? self.clock - state.started : 0.0,
            state.request, state.navigation_claim, static_cast<bool>(state.script)});
    }
    return result;
}
std::vector<ecs::Entity> BehaviorService::entities() const {
    std::vector<ecs::Entity> result;
    result.reserve(impl_->records.size());
    for (const auto& [entity, record] : impl_->records) {
        if (impl_->owns(*record)) { result.push_back(entity); }
    }
    return result;
}
BehaviorHistory BehaviorService::events_since(std::uint64_t sequence, std::size_t limit) const {
    BehaviorHistory result;
    result.newest_sequence = impl_->event_sequence;
    result.history_lost = sequence > result.newest_sequence ||
        (!impl_->history.empty() && sequence < impl_->history.front().sequence - 1);
    limit = std::min(limit, impl_->limits.history);
    for (const auto& event : impl_->history) {
        if (event.sequence > sequence && result.events.size() < limit) { result.events.push_back(event); }
    }
    return result;
}
BehaviorStatistics BehaviorService::statistics() const noexcept { return impl_->statistics; }
void BehaviorService::advance(const core::Frame& frame) {
    auto& self = *impl_;
    if (self.busy || self.closed) { return; }
    const auto faults = self.statistics.callback_faults;
    const auto origins = self.statistics.origin_invalidations;
    self.statistics = {};
    self.statistics.callback_faults = faults;
    self.statistics.origin_invalidations = origins;
    self.statistics.attachments = self.records.size();
    if (!std::isfinite(frame.delta_seconds) || frame.delta_seconds < 0.0 ||
        frame.delta_seconds > 3600.0 || self.clock > 1.0e15 - frame.delta_seconds ||
        (self.advanced && frame.tick <= self.frame.tick)) { self.statistics.invalid_frame = true; return; }
    BusyGuard guard(self.busy);
    self.frame = frame;
    self.advanced = true;
    self.clock += frame.delta_seconds;
    const auto origin = self.coordinates();
    for (auto iterator = self.records.begin(); iterator != self.records.end();) {
        auto& record = *iterator->second;
        auto* registry = self.registry();
        if (!self.owns(record)) {
            self.stop(record, AbortReason::EntityLost);
            iterator = self.records.erase(iterator);
            ++self.statistics.removed_stale;
            continue;
        }
        const auto* marker = registry->get<BehaviorComponent>(record.entity);
        auto* active_scene = self.kernel.services().find<scene::Scene>();
        const bool suspended = !registry->enabled(record.entity) || !marker->enabled ||
            (active_scene && active_scene->contains(record.entity) && !active_scene->effective_enabled(record.entity));
        if (suspended && record.enabled) {
            record.enabled = false;
            self.stop(record, AbortReason::Disabled);
        }
        if (record.motion != invalid_behavior_node) {
            const auto& previous = record.nodes[record.motion].origin;
            if (!origin.consistent || previous.epoch != origin.epoch || previous.absolute_origin != origin.absolute_origin) {
                self.stop(record, AbortReason::OriginChanged, BehaviorStatus::Failure);
                record.last_tick = frame.tick;
                record.evaluated = true;
                increment(self.statistics.origin_invalidations);
            }
        }
        ++iterator;
    }
    std::size_t remaining = self.limits.node_steps_per_frame;
    const auto count = self.records.size();
    auto iterator = self.records.upper_bound(self.last_evaluated);
    for (std::size_t visited = 0; visited < count && remaining != 0 &&
         self.statistics.evaluated < self.limits.agents_per_frame; ++visited) {
        if (iterator == self.records.end()) { iterator = self.records.begin(); }
        auto& record = *iterator->second;
        const auto current_entity = iterator->first;
        ++iterator;
        if (!record.enabled || (record.evaluated && record.last_tick == frame.tick) ||
            (!record.repeat && terminal(record.status))) { continue; }
        if (remaining < record.nodes.size()) { break; }
        self.last_evaluated = current_entity;
        const auto previous = record.status;
        if (terminal(record.status)) {
            self.reset(record, record.tree->data().root, AbortReason::Completed);
            record.reason = AbortReason::Completed;
        }
        std::size_t budget = std::min(remaining, self.limits.node_steps_per_agent);
        const auto initial = budget;
        try { record.status = self.tick(record, record.tree->data().root, budget); }
        catch (...) {
            increment(self.statistics.callback_faults);
            self.stop(record, AbortReason::CallbackFault, BehaviorStatus::Failure);
        }
        record.last_tick = frame.tick;
        record.evaluated = true;
        if (record.status == BehaviorStatus::Success) { record.reason = AbortReason::Completed; }
        remaining -= initial - budget;
        self.statistics.node_steps += initial - budget;
        ++self.statistics.evaluated;
        if (budget == 0 && record.status == BehaviorStatus::Running) { ++self.statistics.budget_deferred; }
        self.publish(record);
        if (record.status != previous || terminal(record.status)) { self.emit(record); }
    }
    self.statistics.attachments = self.records.size();
    for (const auto& [entity, record] : self.records) {
        (void)entity;
        if (record->status == BehaviorStatus::Running) { ++self.statistics.running; }
        if (record->enabled && (!record->evaluated || record->last_tick != frame.tick) &&
            (record->repeat || !terminal(record->status))) { ++self.statistics.budget_deferred; }
    }
}
void BehaviorService::shutdown() {
    auto& self = *impl_;
    if (self.busy || self.closed) { return; }
    BusyGuard guard(self.busy);
    for (auto& [entity, record] : self.records) {
        self.stop(*record, AbortReason::Shutdown);
        if (self.owns(*record)) { (void)self.registry()->remove<BehaviorComponent>(entity); }
    }
    self.records.clear();
    self.statistics.attachments = 0;
    self.statistics.running = 0;
    self.closed = true;
}

void register_behavior(core::Kernel& kernel) {
    if (kernel.services().find<BehaviorService>()) { return; }
    auto& service = kernel.services().emplace<BehaviorService>(kernel);
    if (!kernel.register_module({"ai.behavior", "Bounded behavior trees and typed owned blackboards", 1}) ||
        !kernel.add_shutdown("ai.behavior.shutdown", [owned = &service] { owned->shutdown(); })) {
        throw std::logic_error("behavior registration conflict");
    }
    BehaviorNode idle;
    idle.seconds = 0.25;
    auto idle_tree = BehaviorTree::create({{idle}, 0});
    if (!idle_tree || !service.install("idle", idle_tree.tree)) { throw std::logic_error("idle tree rejected"); }
    BehaviorNode movement;
    movement.kind = BehaviorNodeKind::MoveTo;
    movement.key = "target";
    movement.seconds = 30.0;
    auto move_tree = BehaviorTree::create({{movement}, 0});
    if (!move_tree || !service.install("move_to", move_tree.tree)) { throw std::logic_error("move tree rejected"); }
    if (!kernel.add_system({"ai.behavior.decide", core::Phase::PreSimulation, -8550, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& behavior = host.services().get<BehaviorService>();
            behavior.advance(frame);
            const auto statistics = behavior.statistics();
            host.metric("behavior.attachments", static_cast<double>(statistics.attachments));
            host.metric("behavior.running", static_cast<double>(statistics.running));
            host.metric("behavior.node_steps", static_cast<double>(statistics.node_steps));
            host.metric("behavior.deferred", static_cast<double>(statistics.budget_deferred));
            host.metric("behavior.callback_faults", static_cast<double>(statistics.callback_faults));
        })) { throw std::logic_error("behavior system conflict"); }
    (void)kernel.order_after("ai.behavior.decide", "scene.transforms.pre");
    (void)kernel.order_after("ai.behavior.decide", "scene.partition.update");
    (void)kernel.order_after("ai.behavior.decide", "navmesh.source");
    (void)kernel.order_after("navigation.update", "ai.behavior.decide");
    (void)kernel.order_after("physics.character.control", "ai.behavior.decide");
    (void)kernel.add_command("behavior.attach", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 3 ||
            (args.size() == 3 && args[2] != "once" && args[2] != "repeat")) {
            return {false, "usage: behavior.attach ENTITY TREE [once|repeat]"};
        }
        const bool accepted = host.services().get<BehaviorService>().attach(resolve(host, args[0]), args[1],
            args.size() != 3 || args[2] == "repeat");
        return {accepted, accepted ? "behavior attached" : "entity, tree, capacity, or existing ownership rejected"};
    }, "Attach an installed tree to an existing Scene entity");
    (void)kernel.add_command("behavior.control", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: behavior.control ENTITY abort|restart|pause|resume|detach"}; }
        auto& behavior = host.services().get<BehaviorService>();
        const auto entity = resolve(host, args[0]);
        bool accepted = false;
        if (args[1] == "abort") { accepted = behavior.abort(entity); }
        else if (args[1] == "restart") { accepted = behavior.restart(entity); }
        else if (args[1] == "pause") { accepted = behavior.set_enabled(entity, false); }
        else if (args[1] == "resume") { accepted = behavior.set_enabled(entity, true); }
        else if (args[1] == "detach") { accepted = behavior.detach(entity); }
        return {accepted, accepted ? "behavior control applied" : "behavior control rejected"};
    }, "Cancel owned work, resume from the root, or detach");
    (void)kernel.add_command("behavior.set", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 4) { return {false, "usage: behavior.set ENTITY KEY TYPE VALUE [Y Z]"}; }
        auto* board = host.services().get<BehaviorService>().blackboard(resolve(host, args[0]));
        if (!board) { return {false, "entity has no owned blackboard"}; }
        bool accepted = false;
        if ((args[2] == "position" || args[2] == "vector") && args.size() == 6) {
            math::Vec3 value;
            if (parse(args[3], value.x) && parse(args[4], value.y) && parse(args[5], value.z)) {
                accepted = args[2] == "position" ? board->set(args[1], AbsolutePosition{value}) : board->set(args[1], value);
            }
        } else if (args.size() == 4) {
            if (args[2] == "bool" && (args[3] == "true" || args[3] == "false")) { accepted = board->set(args[1], args[3] == "true"); }
            else if (args[2] == "int") {
                std::int64_t value{};
                accepted = parse(args[3], value) && board->set(args[1], value);
            } else if (args[2] == "number") {
                double value{};
                accepted = parse(args[3], value) && board->set(args[1], value);
            } else if (args[2] == "string") { accepted = board->set(args[1], args[3]); }
            else if (args[2] == "entity") {
                const auto value = resolve(host, args[3]);
                accepted = value && board->set(args[1], value);
            }
        }
        return {accepted, accepted ? "typed value written" : "type, value, or board capacity rejected"};
    }, "Write bool/int/number/string/entity/vector or absolute position; existing key types are fixed");
    (void)kernel.add_command("behavior.define", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 3) { return {false, "usage: behavior.define NAME wait SECONDS | move KEY SPEED TIMEOUT | script PROGRAM TIMEOUT"}; }
        BehaviorNode node;
        bool parsed = false;
        if (args[1] == "wait" && args.size() == 3) { parsed = parse(args[2], node.seconds); }
        else if (args[1] == "move" && args.size() == 5) {
            node.kind = BehaviorNodeKind::MoveTo;
            node.key = args[2];
            parsed = parse(args[3], node.speed) && parse(args[4], node.seconds);
        } else if (args[1] == "script" && args.size() == 4) {
            node.kind = BehaviorNodeKind::Script;
            node.operation = args[2];
            parsed = parse(args[3], node.seconds);
        }
        if (!parsed) { return {false, "invalid definition arguments"}; }
        auto result = BehaviorTree::create({{std::move(node)}, 0});
        if (!result) { return {false, result.error.message}; }
        const bool accepted = host.services().get<BehaviorService>().install(args[0], result.tree);
        return {accepted, accepted ? "immutable tree installed; running attachments retain their version" : "tree installation rejected"};
    }, "Install a bounded wait, navigation, or script action tree");
    (void)kernel.add_command("behavior.inspect", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: behavior.inspect ENTITY"}; }
        auto& behavior = host.services().get<BehaviorService>();
        const auto entity = resolve(host, args[0]);
        const auto snapshot = behavior.inspect(entity);
        if (!snapshot) { return {false, "entity has no owned behavior"}; }
        const auto* board = behavior.blackboard(entity);
        return {true, snapshot->tree + " " + std::string(describe(snapshot->status)) +
            "; reason=" + std::string(describe(snapshot->reason)) +
            "; nodes=" + std::to_string(snapshot->nodes.size()) +
            "; keys=" + std::to_string(board ? board->size() : 0) +
            "; tick=" + std::to_string(snapshot->last_tick)};
    }, "Inspect attachment state, pinned tree, and blackboard size");
}

} // namespace ge::ai
