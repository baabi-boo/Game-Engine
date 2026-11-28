#pragma once

#include <ge/core/identity.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/math/math.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::animation {

using BoneIndex = std::uint32_t;
inline constexpr BoneIndex no_bone = std::numeric_limits<BoneIndex>::max();
inline constexpr std::size_t max_skin_influences = 8;

struct AnimationLimits {
    std::size_t max_bones{4096};
    std::size_t max_channels{4096};
    std::size_t max_keys{1'048'576};
    std::size_t max_events{8192};
    std::size_t max_name_bytes{1024};
    double max_duration{86400.0};
};

enum class AnimationError : std::uint8_t {
    None,
    InvalidLimits,
    Capacity,
    InvalidName,
    DuplicateName,
    InvalidBone,
    Cycle,
    InvalidTransform,
    InvalidInverseBind,
    IncompatibleSkeleton,
    InvalidTime,
    InvalidKey,
    DuplicateChannel,
    InvalidEvent,
    InvalidHandle,
    InvalidInfluence,
    InvalidMesh,
    SingularTransform,
    NonFiniteResult,
    InvalidEntity
};

[[nodiscard]] std::string_view describe(AnimationError error) noexcept;

struct AnimationResult {
    AnimationError error{AnimationError::None};
    std::size_t element{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == AnimationError::None;
    }
};

struct Bone {
    std::string name;
    BoneIndex parent{no_bone};
    math::Transform bind_local{};
    math::Mat4 inverse_bind{};
};

// Bone indices retain authored order. evaluation_order() is parent before child.
// Failed loads leave all previously published data intact; allocation errors throw.
class Skeleton final {
public:
    [[nodiscard]] AnimationResult load(std::vector<Bone> bones,
                                       bool calculate_inverse_binds = true,
                                       AnimationLimits limits = {});
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<const Bone> bones() const noexcept;
    [[nodiscard]] std::span<const BoneIndex> evaluation_order() const noexcept;
    [[nodiscard]] std::span<const math::Mat4> bind_globals() const noexcept;
    [[nodiscard]] BoneIndex find(std::string_view name) const;
    [[nodiscard]] std::vector<BoneIndex> children(BoneIndex parent) const;
    [[nodiscard]] std::uint64_t signature() const noexcept;
private:
    std::vector<Bone> bones_;
    std::vector<BoneIndex> order_;
    std::vector<math::Mat4> bind_globals_;
    std::unordered_map<std::string, BoneIndex> names_;
    std::uint64_t signature_{};
};

// Local transforms are editable. Call update() after edits before using global or
// skin. Global and skin matrices remain in skeleton/model space unless a root
// matrix is explicitly supplied. Outputs preserve affine shear exactly.
struct Pose {
    std::vector<math::Transform> local;
    std::vector<math::Mat4> global;
    std::vector<math::Mat4> skin;
    std::uint64_t skeleton_signature{};
    [[nodiscard]] AnimationResult reset(const Skeleton& skeleton);
    [[nodiscard]] AnimationResult update(const Skeleton& skeleton,
                                         const math::Mat4& root = {});
    [[nodiscard]] bool compatible(const Skeleton& skeleton) const noexcept;
};

enum class Interpolation : std::uint8_t { Step, Linear };
enum class WrapMode : std::uint8_t { Clamp, Loop, PingPong };

struct VectorKey {
    double time{};
    math::Vec3 value{};
};

struct RotationKey {
    double time{};
    math::Quat value{};
};

struct BoneChannel {
    BoneIndex bone{no_bone};
    Interpolation interpolation{Interpolation::Linear};
    std::vector<VectorKey> translations;
    std::vector<RotationKey> rotations;
    std::vector<VectorKey> scales;
};

struct ClipEvent {
    double time{};
    std::string name;
    std::string payload;
};

struct SampleTime {
    double time{};
    std::int64_t cycle{};
    bool reverse{};
    bool finished{};
};

// Times are finite, at most 1e12 seconds in magnitude. Positive durations are
// 1e-6..86400 seconds; repeating modes also bound |time/duration| to 2^48 so
// adjacent cycles remain distinguishable when long double has double precision.
[[nodiscard]] std::optional<SampleTime> wrap_time(double time, double duration,
                                                 WrapMode mode) noexcept;

struct EventOccurrence {
    std::size_t event_index{};
    double clip_time{};
    double traversal_time{};
    std::int64_t cycle{};
    bool reverse{};
};

struct EventTraversal {
    AnimationResult result{};
    std::vector<EventOccurrence> events;
    bool truncated{};
};

class AnimationClip final {
public:
    [[nodiscard]] AnimationResult load(std::string name, double duration,
                                       const Skeleton& skeleton,
                                       std::vector<BoneChannel> channels,
                                       std::vector<ClipEvent> events = {},
                                       AnimationLimits limits = {});
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool compatible(const Skeleton& skeleton) const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] double duration() const noexcept;
    [[nodiscard]] std::uint64_t skeleton_signature() const noexcept;
    [[nodiscard]] std::span<const BoneChannel> channels() const noexcept;
    [[nodiscard]] std::span<const ClipEvent> events() const noexcept;
    [[nodiscard]] AnimationResult sample(const Skeleton& skeleton, double time,
                                         Pose& output, WrapMode mode = WrapMode::Clamp) const;
    // Forward traversal uses (from,to]; reverse uses [to,from). Equal endpoints
    // produce no events, as do zero-duration clips. Loop seam events at 0 and duration are
    // distinct. Ping-pong endpoints occur once, in the arriving direction.
    [[nodiscard]] EventTraversal traverse(double from, double to, WrapMode mode,
                                          std::size_t limit = 256) const;
private:
    std::string name_;
    double duration_{};
    std::uint64_t skeleton_signature_{};
    std::vector<BoneChannel> channels_;
    std::vector<ClipEvent> events_;
};

struct SkeletonHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0 && generation != 0; }
    auto operator<=>(const SkeletonHandle&) const = default;
};

struct ClipHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0 && generation != 0; }
    auto operator<=>(const ClipHandle&) const = default;
};

// Handles are scoped to one library lifetime. Replacement preserves the handle;
// borrowed pointers last until replacement/removal. Skeleton signatures prevent
// sampling clips or poses against a changed layout or bind configuration.
template<class Resource, class Handle>
class AnimationLibrary final {
public:
    explicit AnimationLibrary(std::uint32_t capacity = 1024);
    [[nodiscard]] Handle create(Resource resource, std::string_view name);
    bool replace(Handle handle, Resource resource);
    bool destroy(Handle handle) noexcept;
    void clear() noexcept;
    [[nodiscard]] const Resource* get(Handle handle) const noexcept;
    [[nodiscard]] Handle find(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<Handle> handles() const;
    [[nodiscard]] std::string_view name(Handle handle) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
private:
    struct Record {
        Handle handle{};
        std::string name;
        Resource resource;
    };
    ge::GenerationPool generations_;
    std::vector<std::unique_ptr<Record>> records_;
};

using SkeletonLibrary = AnimationLibrary<Skeleton, SkeletonHandle>;
using ClipLibrary = AnimationLibrary<AnimationClip, ClipHandle>;
extern template class AnimationLibrary<Skeleton, SkeletonHandle>;
extern template class AnimationLibrary<AnimationClip, ClipHandle>;

struct PlaybackState {
    ClipHandle clip{};
    double time{};
    double speed{1.0};
    WrapMode wrap{WrapMode::Loop};
    bool playing{};
};

struct PlaybackUpdate {
    AnimationResult result{};
    std::vector<EventOccurrence> events;
    bool events_truncated{};
    bool finished{};
};

// Crossfades blend from a frozen snapshot of the currently displayed local pose.
// Layered blending, root motion, IK and retargeting can operate on pose().local.
class Animator final {
public:
    [[nodiscard]] AnimationResult play(const Skeleton& skeleton, const ClipLibrary& clips,
                                       ClipHandle clip, double fade_seconds = 0.0,
                                       WrapMode wrap = WrapMode::Loop);
    [[nodiscard]] AnimationResult seek(const Skeleton& skeleton, const ClipLibrary& clips,
                                       double time);
    [[nodiscard]] PlaybackUpdate advance(const Skeleton& skeleton, const ClipLibrary& clips,
                                          double delta_seconds, std::size_t event_limit = 256);
    bool set_speed(double speed) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void stop() noexcept; // Pauses and resets the cursor while retaining the displayed pose.
    [[nodiscard]] const PlaybackState& state() const noexcept;
    [[nodiscard]] Pose& pose() noexcept;
    [[nodiscard]] const Pose& pose() const noexcept;
    [[nodiscard]] bool fading() const noexcept;
private:
    PlaybackState state_;
    Pose pose_;
    std::optional<Pose> fade_source_;
    double fade_duration_{};
    double fade_elapsed_{};
};

struct BoneInfluence {
    BoneIndex bone{no_bone};
    double weight{};
};

struct VertexInfluences {
    std::array<BoneInfluence, max_skin_influences> values{};
    std::uint8_t count{};
};

[[nodiscard]] AnimationResult normalize_influences(VertexInfluences& influences,
                                                    std::size_t bone_count) noexcept;
// Empty influences preserve the source vertex. Positive weights are normalized;
// duplicate bone weights are merged. Normal uses inverse transpose of the
// blended skin matrix, tangent uses its linear part and is orthogonalized.
[[nodiscard]] AnimationResult skin_vertices(std::span<const geometry::Vertex> source,
                                             std::span<const VertexInfluences> influences,
                                             const Pose& pose,
                                             std::vector<geometry::Vertex>& output);
[[nodiscard]] AnimationResult skin_mesh(const geometry::MeshData& source,
                                         std::span<const VertexInfluences> influences,
                                         const Pose& pose, geometry::MeshData& output);

struct AnimatedComponent {
    SkeletonHandle skeleton{};
    Animator animator;
    // Optional existing scene entities, indexed by bone. Their hierarchy must
    // match the skeleton; the root entity's existing scene parent is retained.
    std::vector<ecs::Entity> bone_entities;
    bool enabled{true};
};

struct AnimationEvent {
    ecs::Entity entity{};
    ClipHandle clip{};
    std::string name;
    std::string payload;
    double time{};
    bool reverse{};
};

struct AnimationUpdate {
    std::size_t animated{};
    std::size_t skipped{};
    std::size_t errors{};
    std::size_t truncated_entities{};
    std::vector<AnimationEvent> events;
};

class AnimationService final {
public:
    SkeletonLibrary skeletons;
    ClipLibrary clips;
    bool attach(ecs::Registry& registry, ecs::Entity entity, SkeletonHandle skeleton);
    [[nodiscard]] AnimationResult play(ecs::Registry& registry, ecs::Entity entity,
                                       ClipHandle clip, double fade_seconds = 0.0,
                                       WrapMode wrap = WrapMode::Loop);
    // A rejected bone binding or scene batch preserves that entity's animator
    // and emits no events; successful entities publish independently.
    [[nodiscard]] AnimationUpdate update(ecs::Registry& registry, scene::Scene* scene_graph,
                                         double delta_seconds, std::size_t event_limit = 8192);
};

void register_skeleton(core::Kernel& kernel);

} // namespace ge::animation
