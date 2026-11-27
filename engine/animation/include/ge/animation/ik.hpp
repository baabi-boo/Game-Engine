#pragma once

#include <ge/animation/skeleton.hpp>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::scene { class WorldPartition; }

namespace ge::animation {

inline constexpr int retarget_pose_order = 50;
inline constexpr int ik_pose_order = 100;
inline constexpr int post_ik_pose_order = 200;
inline constexpr std::string_view ik_system_name = "animation.ik.solve";

struct IKLimits {
    std::size_t max_constraints{64};
    std::size_t max_chain_bones{64};
    std::size_t max_rotation_limits{256};
    std::size_t max_pose_bones{4096};
    std::size_t max_resources{128};
    std::size_t max_entities{512};
    std::size_t max_operations_per_solve{262144};
    std::size_t max_operations_per_frame{1048576};
    std::uint32_t max_iterations{32};
    double coordinate_limit{1.0e9};
};

enum class IKError : std::uint8_t {
    None,
    InvalidLimits,
    Capacity,
    InvalidName,
    InvalidSkeleton,
    InvalidPose,
    InvalidBone,
    InvalidChain,
    InvalidTarget,
    InvalidWeight,
    InvalidConstraint,
    UnsupportedScale,
    DegenerateChain,
    MissingTarget,
    InvalidEntity,
    InvalidAttachment,
    StaleRevision,
    PhysicsOwned,
    SceneRejected,
    BudgetExceeded,
    NonFiniteResult,
    Busy
};

struct IKResult {
    IKError error{IKError::None};
    std::size_t element{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == IKError::None; }
};

[[nodiscard]] std::string_view describe(IKError error) noexcept;

enum class IKTargetSpace : std::uint8_t {
    ModelPoint,
    AbsolutePoint,
    EntityPoint,
    ModelDirection,
    WorldDirection,
    EntityDirection
};

// Entity coordinates use an exact generation handle and a local offset/vector.
// AbsolutePoint subtracts WorldPartition::origin().absolute_origin each frame.
// Direction kinds never subtract an origin or apply a translation.
struct IKTarget {
    IKTargetSpace space{IKTargetSpace::ModelPoint};
    math::Vec3 value{};
    ecs::Entity entity{};
};

enum class IKConstraintKind : std::uint8_t { TwoBone, CCD, LookAt, RotationLimit };

// Swing cone and signed twist are relative to reference in bone-local space.
// Angles are radians; reference need not equal the bind rotation.
struct IKRotationLimit {
    BoneIndex bone{no_bone};
    math::Quat reference{};
    math::Vec3 twist_axis{1.0, 0.0, 0.0};
    double minimum_twist{-math::pi};
    double maximum_twist{math::pi};
    double maximum_swing{math::pi};
};

struct IKConstraint {
    std::string name;
    IKConstraintKind kind{IKConstraintKind::CCD};
    std::vector<BoneIndex> chain;
    IKTarget target;
    std::optional<IKTarget> pole;
    math::Vec3 local_axis{0.0, 0.0, -1.0};
    std::vector<IKRotationLimit> rotation_limits;
    double weight{1.0};
    double tolerance{0.001};
    double maximum_step{math::pi};
    std::uint32_t iterations{12};
};

// Loaded rigs are immutable while published. Loading validates direct-parent
// chains; two-bone requires exactly three bones, look-at exactly one. Only
// two-bone accepts a pole. RotationLimit uses its limit list and an empty chain.
class IKRig final {
public:
    [[nodiscard]] IKResult load(const Skeleton& skeleton,
                                std::vector<IKConstraint> constraints,
                                IKLimits limits = {});
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool compatible(const Skeleton& skeleton) const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::span<const IKConstraint> constraints() const noexcept;
    [[nodiscard]] const IKLimits& limits() const noexcept;
    [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const noexcept;
private:
    std::vector<IKConstraint> constraints_;
    IKLimits limits_;
    std::uint64_t skeleton_signature_{};
    std::uint64_t revision_{};
};

struct IKResolvedTarget {
    math::Vec3 target{};
    math::Vec3 pole{};
    bool direction{};
    bool has_pole{};
};

struct IKConstraintStatus {
    std::size_t constraint{};
    std::uint32_t iterations{};
    double residual{};
    bool reached{};
};

struct IKSolveResult {
    IKResult result;
    std::size_t operations{};
    std::size_t limited_joints{};
    std::vector<IKConstraintStatus> constraints;
};

// Transactional rotation-only solve on the shared Pose. Translations/scales
// never stretch. All local scales must be positive uniform, with no shear or
// reflection; scale and coordinates are bounded. Targets are model-space.
// Weight blends each complete constraint correction once, in authored order.
// Limit guarantees therefore apply fully only at effective weight one.
// Repeated calls operate on their input pose; callers must resample an upstream
// pose when they require a fixed blend baseline during paused playback.
// Residual units are metres for chains and radians for look-at/rotation limits.
[[nodiscard]] IKSolveResult solve_ik(const Skeleton& skeleton, const IKRig& rig,
                                    std::span<const IKResolvedTarget> targets,
                                    Pose& pose, double weight = 1.0,
                                    std::size_t operation_budget = 262144);

struct IKHandle {
    ecs::Entity entity{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(entity) && generation != 0;
    }
    auto operator<=>(const IKHandle&) const = default;
};

struct IKComponent {
    IKComponent() = default;
    explicit IKComponent(IKHandle handle) : binding(handle) {}
    IKComponent(const IKComponent&) = delete;
    IKComponent& operator=(const IKComponent&) = delete;
    IKComponent(IKComponent&&) noexcept = default;
    IKComponent& operator=(IKComponent&&) noexcept = default;
    IKHandle binding{};
};

struct IKBindingOptions {
    // Null means identity model-to-world. Otherwise all bound skeleton roots
    // must be direct children of this exact Scene entity. Unbound poses also
    // use this explicit frame; the animated owner is not implicitly the frame.
    ecs::Entity model_entity{};
    double weight{1.0};
    bool write_scene{true};
    bool enabled{true};
};

struct IKAttachResult {
    IKResult result;
    IKHandle handle{};
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(result); }
};

struct IKBindingInfo {
    IKHandle handle{};
    std::uint64_t revision{};
    std::uint64_t rig_revision{};
    IKBindingOptions options;
    IKSolveResult last_solve;
};

struct IKUpdate {
    std::size_t solved{};
    std::size_t skipped{};
    std::size_t errors{};
    std::size_t retired{};
    std::size_t operations{};
    std::size_t budget_exhausted{};
};

// Single writer, scoped to one Registry identity until clear, with no async work.
// Destruction never dereferences borrowed dependencies. Markers cannot be cloned.
// Resources remain pinned by attachments after erase/republication. Explicit
// reattach adopts a new resource. Setters require the current binding revision;
// detach cancels the exact handle without restoring an old animation pose.
class IKService final {
public:
    explicit IKService(IKLimits limits = {});
    [[nodiscard]] IKResult publish(std::string name, std::shared_ptr<const IKRig> rig);
    [[nodiscard]] std::shared_ptr<const IKRig> find(std::string_view name) const;
    bool erase(std::string_view name);
    [[nodiscard]] IKAttachResult attach(ecs::Registry& registry, AnimationService& animation,
                                        scene::Scene* scene_graph, ecs::Entity entity,
                                        std::string_view rig, IKBindingOptions options = {});
    bool detach(ecs::Registry& registry, IKHandle handle);
    void clear(ecs::Registry& registry);
    [[nodiscard]] std::optional<IKBindingInfo> inspect(IKHandle handle) const;
    [[nodiscard]] IKHandle binding(ecs::Entity entity) const noexcept;
    [[nodiscard]] IKResult set_target(IKHandle handle, std::uint64_t revision,
                                      std::size_t constraint, IKTarget target);
    [[nodiscard]] IKResult set_pole(IKHandle handle, std::uint64_t revision,
                                    std::size_t constraint, std::optional<IKTarget> pole);
    [[nodiscard]] IKResult set_weight(IKHandle handle, std::uint64_t revision, double weight);
    [[nodiscard]] IKResult set_enabled(IKHandle handle, std::uint64_t revision, bool enabled);
    [[nodiscard]] IKUpdate update(ecs::Registry& registry, AnimationService& animation,
                                   scene::Scene* scene_graph,
                                   const scene::WorldPartition* partition = nullptr);
    [[nodiscard]] const IKLimits& limits() const noexcept;
private:
    struct Binding {
        IKHandle handle;
        std::uint64_t revision{1};
        std::uint64_t rig_revision{};
        std::uint64_t skeleton_signature{};
        SkeletonHandle skeleton;
        const AnimatedComponent* owner{};
        std::vector<ecs::Entity> bones;
        std::shared_ptr<const IKRig> rig;
        IKBindingOptions options;
        std::vector<IKTarget> targets;
        std::vector<std::optional<IKTarget>> poles;
        IKSolveResult last_solve;
    };
    [[nodiscard]] Binding* edit(IKHandle handle, std::uint64_t revision) noexcept;
    IKLimits limits_;
    const ecs::Registry* registry_identity_{};
    std::uint64_t next_generation_{1};
    std::map<std::string, std::shared_ptr<const IKRig>, std::less<>> rigs_;
    std::map<ecs::Entity, Binding> bindings_;
};

void register_ik(core::Kernel& kernel);

} // namespace ge::animation
