#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

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
namespace ge::scene { class Scene; }

namespace ge::gameplay {

enum class InteractionError : std::uint8_t {
    None, InvalidEntity, InvalidSettings, Capacity, Busy, MissingActor,
    MissingTarget, Disabled, Context, Layers, OutOfRange, OutsideCone,
    Obstructed, IncompleteQuery, Cooldown, Locked, AlreadyUsed, NoActions,
    Released, FocusLost, ActionFailed, MissingService, Budget, Stale
};

enum class InteractionMode : std::uint8_t { Press, Hold, Repeat };

enum class InteractionEventKind : std::uint8_t {
    FocusEnter, FocusLeave, HoldStarted, Cancelled, ActivationStarted,
    Activated, Rejected, TriggerEnter, TriggerLeave, ActionSucceeded, ActionFailed
};

enum class InteractionActionKind : std::uint8_t {
    Notify, Native, Command, Script, SetEnabled, ToggleEnabled,
    AddTag, RemoveTag, Translate, Impulse, SelectCharacter, SelectVehicle
};

enum class InteractionSubject : std::uint8_t { Target, Actor, Explicit };

[[nodiscard]] std::string_view describe(InteractionError value) noexcept;
[[nodiscard]] std::string_view describe(InteractionEventKind value) noexcept;
[[nodiscard]] std::string_view describe(InteractionActionKind value) noexcept;
[[nodiscard]] constexpr std::uint32_t interaction_event_mask(InteractionEventKind event) noexcept {
    const auto index = static_cast<unsigned>(event);
    return index < 32 ? std::uint32_t{1} << index : std::uint32_t{0};
}

struct InteractionActorComponent {
    math::Vec3 eye_offset{0.0, 1.6, 0.0};
    math::Vec3 local_forward{0.0, 0.0, -1.0};
    double range{3.0};
    double cone_cosine{0.5};
    double focus_bias{0.08};
    std::uint64_t contexts{1};
    std::uint64_t flags{};
    std::uint64_t target_layers{~std::uint64_t{0}};
    std::uint64_t obstruction_layers{~std::uint64_t{0}};
    bool enabled{true};
    bool camera_aim{true};
    bool consume_input{true};
};

struct InteractableComponent {
    std::string prompt{"Use"};
    math::Vec3 local_point{};
    double range{3.0};
    double hold_seconds{0.5};
    double repeat_seconds{0.2};
    double cooldown_seconds{};
    double trigger_radius{};
    double trigger_hysteresis{0.1};
    std::uint64_t contexts{~std::uint64_t{0}};
    std::uint64_t required_flags{};
    std::uint64_t forbidden_flags{};
    std::uint64_t actor_layers{~std::uint64_t{0}};
    int priority{};
    InteractionMode mode{InteractionMode::Press};
    bool enabled{true};
    bool require_line_of_sight{true};
    bool exclusive{true};
    bool once{};
};

struct InteractionAction {
    InteractionActionKind kind{InteractionActionKind::Notify};
    InteractionSubject subject{InteractionSubject::Target};
    ecs::Entity entity{};
    std::string name;
    // Command arguments substitute exact tokens $actor, $target and $subject.
    // Script arguments are actor, target and the authored vector, in that order.
    std::vector<std::string> arguments;
    math::Vec3 vector{};
    std::uint32_t events{interaction_event_mask(InteractionEventKind::Activated)};
    bool value{true};
    bool enabled{true};
    bool stop_on_failure{true};
};

struct InteractionActionsComponent {
    std::vector<InteractionAction> actions;
};

struct InteractionCandidate {
    ecs::Entity entity{};
    math::Vec3 point{};
    double distance{};
    double alignment{};
    double score{};
    int priority{};
    InteractionError error{InteractionError::None};
};

struct InteractionActorState {
    ecs::Entity entity{};
    ecs::Entity focus{};
    ecs::Entity holding{};
    double held_seconds{};
    double progress{};
    double repeat_remaining{};
    bool button_down{};
    bool activated_this_press{};
    InteractionError last_error{InteractionError::None};
};

struct InteractionTargetState {
    ecs::Entity entity{};
    ecs::Entity reserved_by{};
    double cooldown_remaining{};
    std::uint64_t activations{};
    bool used{};
};

struct InteractionEvent {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    std::uint64_t activation{};
    InteractionEventKind kind{InteractionEventKind::FocusEnter};
    InteractionError error{InteractionError::None};
    ecs::Entity actor{};
    ecs::Entity target{};
    math::Vec3 point{};
    std::size_t action_index{};
    std::string name;
    std::string message;
};

struct InteractionHistory {
    std::vector<InteractionEvent> events;
    // Continue with next_cursor, including after a partial batch. newest_sequence
    // describes the whole log and is not a safe cursor until that log is consumed.
    std::uint64_t next_cursor{};
    std::uint64_t newest_sequence{};
    bool history_lost{};
};

struct InteractionActionResult {
    bool success{true};
    std::string message;
};

struct InteractionActionContext {
    core::Kernel& kernel;
    ecs::Entity actor{};
    ecs::Entity target{};
    ecs::Entity subject{};
    InteractionEventKind event{InteractionEventKind::Activated};
    math::Vec3 point{};
    std::uint64_t activation{};
    const InteractionAction& action;
};

using InteractionCallback = std::function<InteractionActionResult(InteractionActionContext&)>;

struct InteractionLimits {
    std::size_t actors{128};
    std::size_t targets{4096};
    std::size_t actions_per_target{16};
    std::size_t callbacks{128};
    std::size_t pending_batches{2048};
    std::size_t history{8192};
    std::size_t trigger_pairs{4096};
    std::size_t query_candidates{512};
    std::size_t evaluations_per_frame{65536};
    std::size_t actions_per_frame{4096};
    double maximum_delta_seconds{0.25};
};

struct InteractionStatistics {
    std::size_t actors{};
    std::size_t targets{};
    std::size_t focused{};
    std::size_t holding{};
    std::size_t trigger_pairs{};
    std::size_t pending_batches{};
    std::size_t evaluations{};
    std::size_t actions{};
    std::uint64_t activations{};
    std::uint64_t rejected{};
    std::uint64_t overwritten_events{};
    std::uint64_t incomplete_queries{};
    bool invalid_delta{};
    bool budget_exhausted{};
};

// Single kernel-thread writer. Registry owns every entity and component; only
// explicitly attached entities participate. Cloning components does not enroll
// a new actor or target. Destruction never accesses borrowed services.
// Native callbacks receive copied action data. Reentrant service mutations are
// rejected; use Registry::defer or the Command action for structural changes.
// Activated means all actions succeeded. Script/Command success means admitted;
// their later completion belongs to those services. Actions are not transactional.
// No world coordinates are cached between updates: origin shifts are observed
// through current Scene matrices. History positions describe their event frame.
class InteractionService final {
public:
    explicit InteractionService(core::Kernel& kernel, scene::Scene& scene,
                                InteractionLimits limits = {});
    ~InteractionService();
    InteractionService(const InteractionService&) = delete;
    InteractionService& operator=(const InteractionService&) = delete;
    bool attach_actor(ecs::Entity entity, InteractionActorComponent settings = {});
    bool detach_actor(ecs::Entity entity);
    bool attach_target(ecs::Entity entity, InteractableComponent settings = {});
    bool detach_target(ecs::Entity entity);
    bool set_actions(ecs::Entity entity, std::span<const InteractionAction> actions);
    bool add_action(ecs::Entity entity, InteractionAction action);
    bool register_native(std::string name, InteractionCallback callback);
    bool unregister_native(std::string_view name);
    bool select_player(ecs::Entity actor);
    [[nodiscard]] ecs::Entity player() const noexcept;
    // Explicit focus uses the same geometric and eligibility checks as selection.
    bool focus(ecs::Entity actor, ecs::Entity target);
    bool refresh(ecs::Entity actor);
    // Stores a level; update detects edges. A release is required after an error.
    bool use(ecs::Entity actor, bool down);
    bool cancel(ecs::Entity actor);
    void update(const core::Frame& frame);
    bool clear();
    [[nodiscard]] std::optional<InteractionActorState> actor(ecs::Entity entity) const;
    [[nodiscard]] std::optional<InteractionTargetState> target(ecs::Entity entity) const;
    [[nodiscard]] std::vector<InteractionCandidate> candidates(ecs::Entity actor,
                                                             std::size_t limit = 32);
    [[nodiscard]] InteractionHistory events_since(std::uint64_t cursor,
                                                  std::size_t limit = 8192) const;
    [[nodiscard]] InteractionError last_error() const noexcept;
    [[nodiscard]] const InteractionLimits& limits() const noexcept;
    [[nodiscard]] InteractionStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_interaction(core::Kernel& kernel);

} // namespace ge::gameplay
