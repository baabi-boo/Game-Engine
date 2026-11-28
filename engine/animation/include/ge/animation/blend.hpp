#pragma once

#include <ge/animation/skeleton.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::animation {

using BlendNodeId = std::uint32_t;
using BlendParameterId = std::uint32_t;
using BlendStateId = std::uint32_t;
inline constexpr BlendNodeId no_blend_node = std::numeric_limits<BlendNodeId>::max();
inline constexpr BlendParameterId no_blend_parameter = std::numeric_limits<BlendParameterId>::max();
inline constexpr BlendStateId no_blend_state = std::numeric_limits<BlendStateId>::max();

struct BlendLimits {
    std::size_t max_nodes{256};
    std::size_t max_inputs{32};
    std::size_t max_edges{2048};
    std::size_t max_depth{64};
    std::size_t max_parameters{128};
    std::size_t max_states{128};
    std::size_t max_transitions{1024};
    std::size_t max_conditions{4096};
    std::size_t max_mask_values{131072};
    std::size_t max_cached_transforms{131072};
    std::size_t max_resources{256};
    std::size_t max_entities{4096};
};

enum class BlendError : std::uint8_t {
    None,
    InvalidLimits,
    Capacity,
    InvalidName,
    DuplicateName,
    InvalidSkeleton,
    InvalidNode,
    InvalidParameter,
    InvalidWeight,
    InvalidTime,
    Cycle,
    DepthLimit,
    MissingClip,
    IncompatibleClip,
    InvalidState,
    InvalidTransition,
    InvalidBinding,
    NonFiniteResult,
    RegistryBusy,
    AnimationFailure
};

struct BlendResult {
    BlendError error{BlendError::None};
    std::size_t element{};
    AnimationError animation_error{AnimationError::None};
    [[nodiscard]] explicit operator bool() const noexcept { return error == BlendError::None; }
};

[[nodiscard]] std::string_view describe(BlendError error) noexcept;

enum class BlendParameterType : std::uint8_t { Number, Boolean, Trigger };

struct BlendParameter {
    std::string name;
    BlendParameterType type{BlendParameterType::Number};
    double initial{};
    double minimum{-1.0e6};
    double maximum{1.0e6};
};

// Triggers remain latched until explicitly reset or a successful transition
// consumes a Triggered condition. Numeric setters reject out-of-range values.
class BlendParameters final {
public:
    [[nodiscard]] BlendResult load(std::vector<BlendParameter> definitions, BlendLimits limits = {});
    [[nodiscard]] BlendParameterId find(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<double> value(BlendParameterId parameter) const noexcept;
    [[nodiscard]] const BlendParameter* definition(BlendParameterId parameter) const noexcept;
    [[nodiscard]] bool set_number(BlendParameterId parameter, double value) noexcept;
    [[nodiscard]] bool set_boolean(BlendParameterId parameter, bool value) noexcept;
    [[nodiscard]] bool trigger(BlendParameterId parameter) noexcept;
    [[nodiscard]] bool reset_trigger(BlendParameterId parameter) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint64_t signature() const noexcept;
    [[nodiscard]] std::span<const BlendParameter> definitions() const noexcept;
private:
    std::vector<BlendParameter> definitions_;
    std::vector<double> values_;
    std::uint64_t signature_{};
};

struct WeightedPose {
    const Pose* pose{};
    double weight{1.0};
    std::span<const double> mask;
};

// Per-bone normalized translation/scale and hemisphere-corrected quaternion
// averaging. Empty effective weight falls back to the skeleton's bind pose.
[[nodiscard]] BlendResult blend_poses(const Skeleton& skeleton,
                                      std::span<const WeightedPose> sources, Pose& output);
// Additive translation and scale are differences from reference. Rotation is a
// shortest local-space reference-relative delta composed after the base rotation.
// Weight is nonnegative and bounded to 1e6; values above one extrapolate the
// translation/scale difference and rotation angle, including near-identity deltas.
[[nodiscard]] BlendResult additive_pose(const Skeleton& skeleton, const Pose& base,
                                        const Pose& additive, const Pose& reference,
                                        double weight, std::span<const double> mask, Pose& output);

enum class BlendNodeKind : std::uint8_t { BindPose, Clip, Weighted, Additive, Blend1D, Blend2D };
enum class MissingClipPolicy : std::uint8_t { Fail, BindPose };

struct BlendInput {
    BlendNodeId node{no_blend_node};
    double weight{1.0};
    BlendParameterId weight_parameter{no_blend_parameter};
    math::Vec2 position{};
    std::vector<double> mask;
};

// 1D samples have strictly increasing x coordinates. 2D uses normalized
// inverse-square distance weights and exact sample snapping. Inputs can also
// carry weights/masks. Additive inputs are base, additive, optional reference;
// only the additive input's weight/mask affects its delta.
struct BlendNode {
    std::string name;
    BlendNodeKind kind{BlendNodeKind::BindPose};
    ClipHandle clip{};
    WrapMode wrap{WrapMode::Loop};
    MissingClipPolicy missing_clip{MissingClipPolicy::BindPose};
    double time_scale{1.0};
    double time_offset{};
    BlendParameterId parameter_x{no_blend_parameter};
    BlendParameterId parameter_y{no_blend_parameter};
    math::Vec2 coordinates{};
    std::vector<BlendInput> inputs;
};

struct BlendEvaluation {
    BlendResult result{};
    std::size_t evaluated_nodes{};
    std::size_t clip_samples{};
    std::size_t missing_clips{};
    std::size_t zero_weight_nodes{};
};

// Loads validate every node, including unreachable nodes. Evaluation caches only
// active dependencies, enforces a transform budget and publishes atomically.
class BlendTree final {
public:
    [[nodiscard]] BlendResult load(const Skeleton& skeleton,
                                   std::vector<BlendParameter> parameters,
                                   std::vector<BlendNode> nodes, BlendNodeId root,
                                   BlendLimits limits = {});
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool compatible(const Skeleton& skeleton) const noexcept;
    [[nodiscard]] BlendNodeId root() const noexcept;
    [[nodiscard]] BlendNodeId find(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const BlendNode> nodes() const noexcept;
    [[nodiscard]] const BlendParameters& defaults() const noexcept;
    [[nodiscard]] const BlendLimits& limits() const noexcept;
    [[nodiscard]] BlendEvaluation evaluate(const Skeleton& skeleton, const ClipLibrary& clips,
                                           const BlendParameters& parameters, double seconds,
                                           Pose& output, BlendNodeId root = no_blend_node) const;
private:
    BlendParameters defaults_;
    std::vector<BlendNode> nodes_;
    BlendLimits limits_;
    BlendNodeId root_{no_blend_node};
    std::uint64_t skeleton_signature_{};
};

enum class BlendConditionOp : std::uint8_t {
    Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, IsTrue, IsFalse, Triggered
};

struct BlendCondition {
    BlendParameterId parameter{no_blend_parameter};
    BlendConditionOp operation{BlendConditionOp::IsTrue};
    double threshold{};
    double tolerance{1.0e-6};
};

struct BlendState {
    std::string name;
    BlendNodeId root{no_blend_node};
    double speed{1.0};
    double cycle_seconds{1.0};
    double start_time{};
};

enum class BlendCurve : std::uint8_t { Linear, Smoothstep };

struct BlendTransition {
    BlendStateId from{no_blend_state}; // no_blend_state means any state.
    BlendStateId to{no_blend_state};
    std::vector<BlendCondition> conditions;
    std::optional<double> exit_time; // elapsed state cycles, not wrapped phase.
    double duration{0.2};
    double destination_time{};
    int priority{};
    BlendCurve curve{BlendCurve::Smoothstep};
    bool allow_self{};
    bool allow_interrupt{};
};

class BlendStateMachine final {
public:
    [[nodiscard]] BlendResult load(std::shared_ptr<const BlendTree> tree,
                                   std::vector<BlendState> states,
                                   std::vector<BlendTransition> transitions,
                                   BlendStateId entry, BlendLimits limits = {});
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] BlendStateId entry() const noexcept;
    [[nodiscard]] BlendStateId find(std::string_view name) const noexcept;
    [[nodiscard]] const BlendTree* tree() const noexcept;
    [[nodiscard]] std::span<const BlendState> states() const noexcept;
    [[nodiscard]] std::span<const BlendTransition> transitions() const noexcept;
private:
    std::shared_ptr<const BlendTree> tree_;
    std::vector<BlendState> states_;
    std::vector<BlendTransition> transitions_;
    BlendStateId entry_{no_blend_state};
};

struct BlendAdvance {
    BlendEvaluation evaluation;
    BlendStateId from{no_blend_state};
    BlendStateId to{no_blend_state};
    bool transitioned{};
    bool fading{};
};

// One automatic transition per advance prevents zero-time transition cycles.
// Conditions are evaluated at the end of the time step, highest priority first
// and authored order on ties. Crossfades freeze the displayed source. Seek
// cancels a fade; pause freezes time, parameter resampling and transitions.
class BlendPlayer final {
public:
    [[nodiscard]] BlendResult bind(std::shared_ptr<const BlendStateMachine> machine,
                                   const Skeleton& skeleton, const ClipLibrary& clips);
    [[nodiscard]] BlendAdvance advance(const Skeleton& skeleton, const ClipLibrary& clips,
                                       double delta_seconds);
    [[nodiscard]] BlendResult change_state(const Skeleton& skeleton, const ClipLibrary& clips,
                                           BlendStateId state, double fade_seconds = 0.0);
    [[nodiscard]] BlendResult seek(const Skeleton& skeleton, const ClipLibrary& clips, double seconds);
    void set_paused(bool paused) noexcept;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool fading() const noexcept;
    [[nodiscard]] double time() const noexcept;
    [[nodiscard]] BlendStateId state() const noexcept;
    [[nodiscard]] const BlendStateMachine* machine() const noexcept;
    [[nodiscard]] BlendParameters& parameters() noexcept;
    [[nodiscard]] const BlendParameters& parameters() const noexcept;
    [[nodiscard]] const Pose& pose() const noexcept;
private:
    std::shared_ptr<const BlendStateMachine> machine_;
    BlendParameters parameters_;
    Pose pose_;
    std::optional<Pose> fade_source_;
    BlendStateId state_{no_blend_state};
    double time_{};
    double state_elapsed_{};
    double fade_duration_{};
    double fade_elapsed_{};
    BlendCurve fade_curve_{BlendCurve::Smoothstep};
    bool paused_{};
};

// Attach/detach are the authority setters. The base AnimatedComponent remains
// disabled even while this player is paused. Replace/remove that component only
// after detach. Authority names the exact component instance in its registry;
// a copied BlendComponent never acquires its source's authority on another owner.
// Base play/seek calls also require detach first. External enabled=true writes
// are detected and reset at the next graph update, before base animation runs.
struct BlendComponent {
    BlendPlayer player;
    bool write_scene{true};
    std::uint64_t authority_owner{};
    const ecs::Registry* authority_registry{};
    SkeletonHandle authority_skeleton{};
    bool previous_animated_enabled{true};
};

[[nodiscard]] bool blend_owns_pose(const ecs::Registry& registry, ecs::Entity entity) noexcept;
// The canonical shared pose is always AnimatedComponent::animator.pose().
// Later IK/retarget passes can edit it, update(skeleton), and publish bone edits.
[[nodiscard]] Pose* composed_pose(ecs::Registry& registry, ecs::Entity entity) noexcept;
[[nodiscard]] const Pose* composed_pose(const ecs::Registry& registry, ecs::Entity entity) noexcept;

struct BlendStateEvent {
    ecs::Entity entity{};
    BlendStateId from{no_blend_state};
    BlendStateId to{no_blend_state};
    std::string state_name;
};

struct BlendServiceUpdate {
    std::size_t animated{};
    std::size_t skipped{};
    std::size_t errors{};
    std::size_t ownership_reassertions{};
    std::size_t missing_clips{};
    std::size_t evaluated_nodes{};
    std::vector<BlendStateEvent> transitions;
};

class BlendService final {
public:
    // Resource/entity counts apply here; trees and machines enforce the limits
    // supplied to their own loads. Published shared resources must stay immutable.
    explicit BlendService(BlendLimits limits = {});
    [[nodiscard]] BlendResult publish(std::string name, std::shared_ptr<const BlendStateMachine> machine);
    [[nodiscard]] std::shared_ptr<const BlendStateMachine> find(std::string_view name) const;
    bool erase(std::string_view name);
    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] BlendResult attach(ecs::Registry& registry, ecs::Entity entity,
                                     AnimationService& animation, std::string_view machine);
    // Restoration is conditional on the original component still being present
    // and disabled. Its preserved Animator playback resumes on the next frame.
    [[nodiscard]] BlendResult detach(ecs::Registry& registry, ecs::Entity entity);
    [[nodiscard]] BlendServiceUpdate update(ecs::Registry& registry, AnimationService& animation,
                                            scene::Scene* scene_graph, double delta_seconds);
    [[nodiscard]] const BlendLimits& limits() const noexcept;
private:
    BlendLimits limits_;
    std::map<std::string, std::shared_ptr<const BlendStateMachine>, std::less<>> machines_;
};

void register_blend(core::Kernel& kernel);

} // namespace ge::animation
