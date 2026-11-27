#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }

namespace ge::ai {

// Positions are absolute: runtime coordinates + WorldPartition::origin().
// Bare Vec3 values are directions/free vectors and never shift with the origin.
struct AbsolutePosition {
    math::Vec3 value{};
    bool operator==(const AbsolutePosition&) const = default;
};
using BlackboardValue = std::variant<bool, std::int64_t, double, std::string,
                                     math::Vec3, ecs::Entity, AbsolutePosition>;
enum class BlackboardType : std::uint8_t {
    Boolean, Integer, Number, String, Vector, Entity, Position
};
[[nodiscard]] BlackboardType value_type(const BlackboardValue& value) noexcept;
struct BlackboardLimits {
    std::size_t entries{128};
    std::size_t key_bytes{96};
    std::size_t string_bytes{4096};
    std::size_t history{256};
};
struct BlackboardChange {
    std::uint64_t revision{};
    std::string key;
    bool erased{};
};
struct BlackboardHistory {
    std::vector<BlackboardChange> changes;
    std::uint64_t newest_revision{};
    bool history_lost{};
};
// Single writer. Existing keys retain their type until explicitly erased.
// References remain valid until that key is written/erased or the board dies.
class Blackboard final {
public:
    explicit Blackboard(BlackboardLimits limits = {});
    bool write(std::string_view key, BlackboardValue value);
    template<class T>
    bool set(std::string_view key, T value) {
        return write(key, BlackboardValue(std::move(value)));
    }
    template<class T>
    [[nodiscard]] const T* get(std::string_view key) const noexcept {
        const auto* value = find(key);
        return value ? std::get_if<T>(value) : nullptr;
    }
    [[nodiscard]] const BlackboardValue* find(std::string_view key) const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    bool erase(std::string_view key);
    bool clear();
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const BlackboardLimits& limits() const noexcept;
    [[nodiscard]] std::vector<std::pair<std::string, BlackboardValue>> snapshot() const;
    // Limited readers advance to the last returned revision, not newest_revision.
    [[nodiscard]] BlackboardHistory changes_since(std::uint64_t revision,
                                                 std::size_t limit = 256) const;
private:
    BlackboardLimits limits_;
    std::map<std::string, BlackboardValue, std::less<>> values_;
    std::deque<BlackboardChange> history_;
    std::uint64_t revision_{};
};

enum class BehaviorStatus : std::uint8_t { Idle, Running, Success, Failure, Aborted };
enum class AbortReason : std::uint8_t {
    Completed, Requested, Detached, Disabled, EntityLost, Preempted,
    Timeout, OriginChanged, OwnershipLost, CallbackFault, Shutdown
};
[[nodiscard]] std::string_view describe(BehaviorStatus status) noexcept;
[[nodiscard]] std::string_view describe(AbortReason reason) noexcept;
using BehaviorNodeId = std::uint32_t;
inline constexpr BehaviorNodeId invalid_behavior_node =
    std::numeric_limits<BehaviorNodeId>::max();
enum class BehaviorNodeKind : std::uint8_t {
    Sequence, Selector, ReactiveSequence, ParallelAll, Invert, ForceSuccess,
    ForceFailure, Repeat, Retry, Timeout, Cooldown, Wait, Condition,
    SetBlackboard, EraseBlackboard, MoveTo, Script, Custom
};
enum class BehaviorComparison : std::uint8_t {
    Exists, Missing, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    EntityAlive, WithinDistance
};
struct BehaviorNode {
    BehaviorNodeKind kind{BehaviorNodeKind::Wait};
    std::string name;
    std::vector<BehaviorNodeId> children;
    std::string key;
    BlackboardValue value{false};
    std::string operation;
    BehaviorComparison comparison{BehaviorComparison::Exists};
    double seconds{};
    double speed{3.0};
    double tolerance{0.15};
    double snap_distance{4.0};
    std::uint32_t count{1};
};
struct BehaviorTreeLimits {
    std::size_t nodes{512};
    std::size_t depth{64};
    std::size_t children{64};
    std::size_t text_bytes{4096};
    std::uint32_t repetitions{1000000};
};
struct BehaviorTreeData {
    std::vector<BehaviorNode> nodes;
    BehaviorNodeId root{};
};
struct BehaviorTreeError {
    BehaviorNodeId node{invalid_behavior_node};
    std::string message;
};
class BehaviorTree;
struct BehaviorTreeResult {
    std::shared_ptr<const BehaviorTree> tree;
    BehaviorTreeError error;
    [[nodiscard]] explicit operator bool() const noexcept { return tree != nullptr; }
};
// Definitions own immutable storage; running attachments pin the installed version.
// The graph must be one rooted tree, without shared children, cycles, or orphans.
// ReactiveSequence permits Condition prefixes and one final resumable subtree.
class BehaviorTree final {
public:
    [[nodiscard]] static BehaviorTreeResult create(BehaviorTreeData data,
                                                    BehaviorTreeLimits limits = {});
    [[nodiscard]] const BehaviorTreeData& data() const noexcept;
private:
    explicit BehaviorTree(BehaviorTreeData data);
    BehaviorTreeData data_;
};
struct BehaviorComponent {
    std::uint64_t attachment{};
    BehaviorStatus status{BehaviorStatus::Idle};
    bool enabled{true};
};
struct BehaviorActionContext {
    core::Kernel& kernel;
    ecs::Entity entity;
    Blackboard& blackboard;
    const core::Frame& frame;
    BehaviorNodeId node;
    std::uint64_t& memory;
};
// Trusted callbacks are bounded and nonreentrant. exit owns resource cleanup and
// runs once after entry is attempted, including rejected entry, aborts and faults.
// Running actions pin callbacks; replacing a registration cannot alter them.
struct BehaviorAction {
    std::function<bool(BehaviorActionContext&)> enter;
    std::function<BehaviorStatus(BehaviorActionContext&)> tick;
    std::function<void(BehaviorActionContext&, BehaviorStatus, AbortReason)> exit;
};
struct BehaviorLimits {
    BlackboardLimits blackboard{};
    BehaviorTreeLimits tree{};
    std::size_t attachments{1024};
    std::size_t trees{128};
    std::size_t actions{128};
    std::size_t node_steps_per_agent{512};
    std::size_t node_steps_per_frame{16384};
    std::size_t agents_per_frame{1024};
    std::size_t graph_scan{65536};
    std::size_t history{2048};
};
struct BehaviorNodeSnapshot {
    BehaviorNodeId node{};
    BehaviorStatus status{BehaviorStatus::Idle};
    std::size_t cursor{};
    std::uint32_t iterations{};
    double elapsed{};
    std::uint64_t navigation_request{};
    bool owns_navigation{};
    bool owns_script{};
};
struct BehaviorSnapshot {
    ecs::Entity entity{};
    std::string tree;
    BehaviorStatus status{BehaviorStatus::Idle};
    AbortReason reason{AbortReason::Completed};
    std::uint64_t attachment{};
    std::uint64_t last_tick{};
    bool enabled{};
    bool repeat{};
    std::vector<BehaviorNodeSnapshot> nodes;
};
struct BehaviorEvent {
    std::uint64_t sequence{};
    ecs::Entity entity{};
    BehaviorStatus status{BehaviorStatus::Idle};
    AbortReason reason{AbortReason::Completed};
    std::uint64_t tick{};
};
struct BehaviorHistory {
    std::vector<BehaviorEvent> events;
    std::uint64_t newest_sequence{};
    bool history_lost{};
};
struct BehaviorStatistics {
    std::size_t attachments{};
    std::size_t running{};
    std::size_t evaluated{};
    std::size_t node_steps{};
    std::size_t budget_deferred{};
    std::size_t removed_stale{};
    std::uint64_t callback_faults{};
    std::uint64_t origin_invalidations{};
    bool invalid_frame{};
};
// Registry is the only entity owner. A cloned BehaviorComponent never transfers
// ownership. Boards and node state belong to this service and are not cloned.
// MoveTo requires an existing Manual CharacterSystem character and no follower;
// it temporarily claims Navigation authority, owns its request/follower, and
// follows the graph node nearest the target within snap_distance. It never
// writes Scene poses. External control/follower edits require detach first.
// Targets are sampled at entry; invalidated entity targets cancel the action.
// Origin changes abort active paths; absolute targets remain valid for restart.
// Installation requires a complete traversal to fit both configured step budgets.
// Mutations must run outside ScriptService and CharacterSystem callbacks; their
// nonreentrant ownership APIs must be available for synchronous cancellation.
// Call shutdown while borrowed services are alive; destruction does not use them.
class BehaviorService final {
public:
    explicit BehaviorService(core::Kernel& kernel, BehaviorLimits limits = {});
    ~BehaviorService();
    BehaviorService(const BehaviorService&) = delete;
    BehaviorService& operator=(const BehaviorService&) = delete;
    [[nodiscard]] const BehaviorLimits& limits() const noexcept;
    bool install(std::string name, std::shared_ptr<const BehaviorTree> tree);
    bool unload(std::string_view name);
    [[nodiscard]] std::shared_ptr<const BehaviorTree> tree(std::string_view name) const;
    bool register_action(std::string name, BehaviorAction action);
    bool unregister_action(std::string_view name);
    bool attach(ecs::Entity entity, std::string_view tree, bool repeat = true);
    bool detach(ecs::Entity entity);
    bool abort(ecs::Entity entity, AbortReason reason = AbortReason::Requested);
    bool restart(ecs::Entity entity);
    bool set_enabled(ecs::Entity entity, bool enabled);
    [[nodiscard]] Blackboard* blackboard(ecs::Entity entity) noexcept;
    [[nodiscard]] const Blackboard* blackboard(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::optional<BehaviorSnapshot> inspect(ecs::Entity entity) const;
    [[nodiscard]] std::vector<ecs::Entity> entities() const;
    [[nodiscard]] BehaviorHistory events_since(std::uint64_t sequence,
                                               std::size_t limit = 256) const;
    [[nodiscard]] BehaviorStatistics statistics() const noexcept;
    void advance(const core::Frame& frame);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_behavior(core::Kernel& kernel);

} // namespace ge::ai
