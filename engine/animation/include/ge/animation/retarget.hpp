#pragma once

#include <ge/animation/skeleton.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::physics { class PhysicsWorld; }
namespace ge::scene { class WorldPartition; }

namespace ge::animation {

inline constexpr std::string_view retarget_system_name = "animation.retarget.pose";
inline constexpr std::string_view root_motion_system_name = "animation.root_motion.apply";

struct RetargetLimits {
    std::size_t max_bones{4096};
    std::size_t max_links{4096};
    std::size_t max_resources{128};
    std::size_t max_bindings{512};
    std::size_t max_scene_nodes{65536};
    std::size_t max_frame_bones{1048576};
    std::size_t max_clip_segments{256};
    double coordinate_limit{1.0e9};
};

enum class RetargetError : std::uint8_t {
    None,
    InvalidLimits,
    Capacity,
    InvalidName,
    InvalidSkeleton,
    InvalidPose,
    InvalidBone,
    DuplicateTarget,
    InvalidWeight,
    UnsupportedTransform,
    InvalidHandle,
    StaleBinding,
    Cycle,
    MissingResource,
    InvalidEntity,
    PhysicsOwned,
    SceneRejected,
    MissingPhysics,
    WrongBodyMode,
    InvalidTime,
    Discontinuity,
    PendingMotion,
    Busy
};

struct RetargetResult {
    RetargetError error{RetargetError::None};
    std::size_t element{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == RetargetError::None; }
};

[[nodiscard]] std::string_view describe(RetargetError error) noexcept;

enum class RetargetTranslation : std::uint8_t { Bind, ScaledLocalDelta };

struct RetargetLink {
    BoneIndex source{no_bone};
    BoneIndex target{no_bone};
    RetargetTranslation translation{RetargetTranslation::Bind};
    double translation_scale{1.0};
    double weight{1.0};
    bool copy_uniform_scale{};
};

// Bind-space global rotation correction, evaluated parent before child. Local
// translation deltas are converted between bind parent frames and explicitly
// scaled. Positive uniform scale only; shear and reflection are rejected.
// The target pose supplies the base for weighted links and unmapped bones.
// A published map must remain immutable; successful reload increments revision.
class RetargetMap final {
public:
    [[nodiscard]] RetargetResult load(const Skeleton& source, const Skeleton& target,
                                      std::vector<RetargetLink> links,
                                      RetargetLimits limits = {});
    [[nodiscard]] RetargetResult match_names(const Skeleton& source, const Skeleton& target,
                                             double translation_scale = 1.0,
                                             RetargetLimits limits = {});
    [[nodiscard]] RetargetResult apply(const Skeleton& source, const Pose& source_pose,
                                       const Skeleton& target, Pose& target_pose,
                                       double weight = 1.0) const;
    [[nodiscard]] bool compatible(const Skeleton& source, const Skeleton& target) const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::span<const RetargetLink> links() const noexcept;
    [[nodiscard]] const RetargetLimits& limits() const noexcept;
private:
    std::vector<RetargetLink> links_;
    std::vector<std::size_t> target_links_;
    std::vector<math::Quat> rotations_;
    std::vector<math::Quat> translations_;
    RetargetLimits limits_;
    std::uint64_t source_signature_{};
    std::uint64_t target_signature_{};
    std::uint64_t revision_{};
};

struct RootMotionSettings {
    BoneIndex bone{};
    bool horizontal{true};
    bool vertical{};
    bool yaw{true};
    double maximum_translation{10.0};
    double maximum_yaw{math::pi};
    double maximum_seconds{0.25};
    bool operator==(const RootMotionSettings&) const = default;
};

// Translation is in the previous extracted root frame; yaw is around +Y.
// Compose onto the explicit actor frame. No world position is retained.
struct RootMotionDelta {
    math::Vec3 translation{};
    double yaw{};
    double seconds{};
};

struct RootMotionResult {
    RetargetResult result;
    RootMotionDelta delta;
    bool primed{};
};

// Supports one actual skeleton root. The first sample and changed continuity
// token establish a baseline and emit zero displacement. Callers must change
// the token after seeks, graph transitions, looping seams and producer changes.
// Failed extraction leaves both pose and tracker unchanged. Strip factors out
// the selected rigid motion, retaining swing and unselected translation in the
// residual frame. Translation uses the rotated bind pivot, including nonzero roots.
class RootMotionTracker final {
public:
    [[nodiscard]] RetargetResult configure(const Skeleton& skeleton,
                                           RootMotionSettings settings = {});
    void reset() noexcept;
    [[nodiscard]] RootMotionResult advance(const Skeleton& skeleton, Pose& pose,
                                           double delta_seconds, std::uint64_t continuity,
                                           bool strip = true);
    [[nodiscard]] bool primed() const noexcept;
    [[nodiscard]] const RootMotionSettings& settings() const noexcept;
private:
    RootMotionSettings settings_;
    math::Vec3 previous_translation_{};
    double previous_yaw_{};
    std::uint64_t skeleton_signature_{};
    std::uint64_t continuity_{};
    bool primed_{};
};

// Standalone exact endpoint integration for Clamp/Loop traversal. Each seam is
// split and rigid deltas are composed, including reverse traversal. PingPong is
// explicitly unsupported. Budgeted segments bound repeated sampling/allocation.
// Endpoint yaw uses shortest arcs: author segments below pi radians.
[[nodiscard]] RootMotionResult extract_clip_root_motion(const Skeleton& skeleton,
    const AnimationClip& clip, double from, double to, WrapMode wrap,
    RootMotionSettings settings = {}, RetargetLimits limits = {});

struct RetargetHandle {
    ecs::Entity entity{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(entity) && generation != 0;
    }
    auto operator<=>(const RetargetHandle&) const = default;
};

struct RetargetBindingOptions {
    double weight{1.0};
    bool write_scene{true};
    bool enabled{true};
};

enum class RootMotionApplication : std::uint8_t { Observe, Scene, KinematicTarget };

struct RootMotionBindingOptions {
    ecs::Entity model_entity{};
    RootMotionSettings settings;
    RootMotionApplication application{RootMotionApplication::Observe};
    bool strip{true};
    bool write_scene{true};
    bool enabled{true};
};

struct RetargetAttachResult {
    RetargetResult result;
    RetargetHandle handle{};
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(result); }
};

struct RetargetBindingInfo {
    RetargetHandle handle;
    ecs::Entity source{};
    std::uint64_t map_revision{};
    RetargetBindingOptions options;
    RetargetResult last_result;
};

struct RootMotionBindingInfo {
    RetargetHandle handle;
    RootMotionBindingOptions options;
    RootMotionResult last_sample;
    RetargetResult last_application;
    std::uint64_t continuity{};
    bool pending{};
};

struct RetargetUpdate {
    std::size_t retargeted{};
    std::size_t captured{};
    std::size_t applied{};
    std::size_t skipped{};
    std::size_t errors{};
    std::size_t retired{};
    std::size_t bones{};
};

// Single writer. Stored registry addresses are identity guards only and are not
// dereferenced. Exact component instance, skeleton and entity identities are
// checked; component replacement retires attachments even at a reused address.
// Source->target edges are topologically ordered with entity-order tie breaks.
// Weighted bindings require an active base player or an owned blend producer;
// a paused unrefreshed target cannot repeatedly blend against its prior result.
// Root capture follows all retargets, before IK. A stripped root cannot be a
// retarget source. Pose producers must refresh raw locals before each capture;
// base poses without a fresh successful Animator publication are skipped. Fresh
// same-time seeks, clamped endpoints and zero-speed samples are still stripped.
// Base clip changes and seams reset automatically; composed graphs require
// reset_root() for every discontinuity. Call reset_root() after caller-owned
// Animator assignment/restoration; copied samplers can share a publication serial.
// Kinematic deltas supersede no other controller: caller grants exclusive target
// control. One pending delta expires unless applied on the immediately next tick,
// after origin shifts. Dynamic bodies and CharacterComponent owners are rejected.
class RetargetService final {
public:
    explicit RetargetService(RetargetLimits limits = {});
    ~RetargetService();
    RetargetService(const RetargetService&) = delete;
    RetargetService& operator=(const RetargetService&) = delete;
    [[nodiscard]] const RetargetLimits& limits() const noexcept;
    [[nodiscard]] RetargetResult publish(std::string name, std::shared_ptr<const RetargetMap> map);
    [[nodiscard]] std::shared_ptr<const RetargetMap> find(std::string_view name) const;
    bool erase(std::string_view name);
    [[nodiscard]] RetargetAttachResult attach(ecs::Registry& registry, AnimationService& animation,
        scene::Scene* scene_graph, ecs::Entity source, ecs::Entity target,
        std::string_view map, RetargetBindingOptions options = {});
    [[nodiscard]] RetargetAttachResult attach_root(ecs::Registry& registry, AnimationService& animation,
        scene::Scene* scene_graph, physics::PhysicsWorld* physics, ecs::Entity entity,
        RootMotionBindingOptions options = {});
    bool detach(RetargetHandle handle) noexcept;
    bool detach_root(RetargetHandle handle) noexcept;
    [[nodiscard]] RetargetResult reset_root(RetargetHandle handle);
    [[nodiscard]] std::optional<RetargetBindingInfo> inspect(RetargetHandle handle) const;
    [[nodiscard]] std::optional<RootMotionBindingInfo> inspect_root(RetargetHandle handle) const;
    [[nodiscard]] RetargetHandle binding(ecs::Entity entity) const noexcept;
    [[nodiscard]] RetargetHandle root_binding(ecs::Entity entity) const noexcept;
    [[nodiscard]] RetargetUpdate update(ecs::Registry& registry, AnimationService& animation,
        scene::Scene* scene_graph, double delta_seconds, std::uint64_t tick,
        const scene::WorldPartition* partition = nullptr);
    [[nodiscard]] RetargetUpdate apply_pending(ecs::Registry& registry, const AnimationService& animation,
        scene::Scene* scene_graph, physics::PhysicsWorld* physics, std::uint64_t tick,
        const scene::WorldPartition* partition = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_retarget(core::Kernel& kernel);

} // namespace ge::animation
