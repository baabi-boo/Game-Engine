#pragma once

#include <ge/animation/skeleton.hpp>
#include <ge/core/identity.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ge::physics { class PhysicsWorld; }

namespace ge::animation {

enum class CurveError : std::uint8_t {
    None, InvalidLimits, Capacity, InvalidTime, InvalidValue, InvalidKey,
    InvalidMode, InvalidName, DuplicateTrack, InvalidTrack, InvalidMarker,
    InvalidHandle, InvalidEntity, OwnershipConflict, PhysicsRouteRequired,
    PhysicsRejected, SceneRejected, RevisionExhausted, NonFiniteResult
};

[[nodiscard]] std::string_view describe(CurveError error) noexcept;

struct CurveResult {
    CurveError error{CurveError::None};
    std::size_t element{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == CurveError::None; }
};

struct CurveLimits {
    std::size_t max_keys{4096};
    std::size_t max_tracks{64};
    std::size_t max_markers{4096};
    std::size_t max_samples{65536};
    std::size_t max_name_bytes{256};
};

enum class CurveInterpolation : std::uint8_t { Step, Linear, Hermite };
enum class CurveTangentPolicy : std::uint8_t { Manual, Flat, Linear, Smooth, Monotone };
enum class CurveExtrapolation : std::uint8_t { Hold, Linear, Loop, PingPong };

template<class Value>
struct CurveKey {
    double time{};
    Value value{};
    Value incoming{};
    Value outgoing{};
    CurveInterpolation interpolation{CurveInterpolation::Linear};
    CurveTangentPolicy tangents{CurveTangentPolicy::Smooth};
};

template<class Value>
struct CurveSample {
    Value value{};
    Value velocity{};
    Value acceleration{};
    std::size_t segment{};
    bool discontinuous{};
};

// Times are strictly increasing, 0..86400 seconds, separated by at least 1e-6.
// Values and tangent components are finite and bounded by 1e12. Incoming and
// outgoing tangents are derivatives per second, not normalized segment slopes.
// Successful edits invalidate key spans; failed edits preserve the prior curve.
// Monotone slopes preserve each component's monotonicity when both segment
// endpoints use that policy. A curve must retain at least one key.
// Explicit instantiations support double and math::Vec3 only.
template<class Value>
class NumericCurve final {
public:
    [[nodiscard]] CurveResult assign(std::vector<CurveKey<Value>> keys,
                                     CurveLimits limits = {});
    [[nodiscard]] CurveResult insert(CurveKey<Value> key);
    [[nodiscard]] CurveResult erase(double time);
    [[nodiscard]] CurveResult set_extrapolation(CurveExtrapolation before,
                                                CurveExtrapolation after) noexcept;
    [[nodiscard]] std::optional<CurveSample<Value>> sample(double time) const noexcept;
    [[nodiscard]] CurveResult resample(double begin, double end, std::size_t count,
                                       std::vector<CurveKey<Value>>& output) const;
    [[nodiscard]] std::span<const CurveKey<Value>> keys() const noexcept;
    [[nodiscard]] const CurveLimits& limits() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] double begin_time() const noexcept;
    [[nodiscard]] double end_time() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] CurveExtrapolation before() const noexcept;
    [[nodiscard]] CurveExtrapolation after() const noexcept;
private:
    std::vector<CurveKey<Value>> keys_;
    CurveLimits limits_{};
    CurveExtrapolation before_{CurveExtrapolation::Hold};
    CurveExtrapolation after_{CurveExtrapolation::Hold};
    std::uint64_t revision_{};
};

using ScalarCurve = NumericCurve<double>;
using VectorCurve = NumericCurve<math::Vec3>;
extern template class NumericCurve<double>;
extern template class NumericCurve<math::Vec3>;

struct QuaternionCurveKey {
    double time{};
    math::Quat value{};
    CurveInterpolation interpolation{CurveInterpolation::Linear};
};

struct QuaternionCurveSample {
    math::Quat value{};
    math::Vec3 angular_velocity{};
    std::size_t segment{};
    bool discontinuous{};
};

// Quaternion keys support step and shortest-arc spherical linear interpolation.
// Angular velocity is expressed in the curve's parent coordinate frame.
class QuaternionCurve final {
public:
    [[nodiscard]] CurveResult assign(std::vector<QuaternionCurveKey> keys,
                                     CurveLimits limits = {});
    [[nodiscard]] CurveResult insert(QuaternionCurveKey key);
    [[nodiscard]] CurveResult erase(double time);
    [[nodiscard]] CurveResult set_extrapolation(CurveExtrapolation before,
                                                CurveExtrapolation after) noexcept;
    [[nodiscard]] std::optional<QuaternionCurveSample> sample(double time) const noexcept;
    [[nodiscard]] std::span<const QuaternionCurveKey> keys() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
private:
    std::vector<QuaternionCurveKey> keys_;
    CurveLimits limits_{};
    CurveExtrapolation before_{CurveExtrapolation::Hold};
    CurveExtrapolation after_{CurveExtrapolation::Hold};
    std::uint64_t revision_{};
};

struct CurveTimeMapping {
    double scale{1.0};
    double offset{};
};

struct MappedCurveTime {
    SampleTime wrapped{};
    double derivative{1.0};
};

// Affine mapping precedes wrapping. An optional scalar warp maps normalized
// time to normalized time; its value must remain in [0,1] at the query. Warps
// may reverse time, and derivative includes affine, wrap and warp derivatives.
[[nodiscard]] std::optional<MappedCurveTime> map_curve_time(
    double time, double duration, WrapMode wrap = WrapMode::Clamp,
    CurveTimeMapping mapping = {}, const ScalarCurve* warp = nullptr) noexcept;

struct ArcTimePoint {
    double time{};
    double distance{};
};

// Chord lengths approximate a continuous path. Step segments are rejected.
// Rebuild after editing the source curve; source_revision identifies the input.
class CurveArcTable final {
public:
    [[nodiscard]] CurveResult build(const VectorCurve& curve, double begin,
                                    double end, std::size_t samples = 256);
    [[nodiscard]] std::optional<double> time_at_distance(double distance) const noexcept;
    [[nodiscard]] std::optional<double> distance_at_time(double time) const noexcept;
    [[nodiscard]] std::span<const ArcTimePoint> points() const noexcept;
    [[nodiscard]] double length() const noexcept;
    [[nodiscard]] std::uint64_t source_revision() const noexcept;
private:
    std::vector<ArcTimePoint> points_;
    std::uint64_t source_revision_{};
};

struct CurveMarkerTraversal {
    CurveResult result{};
    std::vector<EventOccurrence> events;
    bool truncated{};
};

// Forward intervals are (from,to], reverse intervals [to,from). Equal-time
// markers retain authored order in both directions. Loop seam markers at zero
// and duration remain distinct. Ping-pong turn markers arrive exactly once.
// Work is O(markers + budget*log(markers)), independent of crossed cycle count.
[[nodiscard]] CurveMarkerTraversal traverse_curve_markers(
    std::span<const ClipEvent> markers, double duration, double from, double to,
    WrapMode wrap, std::size_t budget = 256);

enum class CurveProperty : std::uint8_t { Position, Rotation, Scale, UniformScale };
using CurveTrackData = std::variant<ScalarCurve, VectorCurve, QuaternionCurve>;

struct CurveTrack {
    std::string name;
    CurveProperty property{CurveProperty::Position};
    CurveTrackData data{VectorCurve{}};
    CurveTimeMapping time{};
};

class CurveTrackSet final {
public:
    [[nodiscard]] CurveResult assign(std::string name, double duration,
                                     std::vector<CurveTrack> tracks,
                                     std::vector<ClipEvent> markers = {},
                                     CurveLimits limits = {});
    [[nodiscard]] std::optional<math::Transform> sample(double time,
        const math::Transform& baseline = {}, WrapMode wrap = WrapMode::Clamp) const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] double duration() const noexcept;
    [[nodiscard]] std::span<const CurveTrack> tracks() const noexcept;
    [[nodiscard]] std::span<const ClipEvent> markers() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
private:
    std::string name_;
    double duration_{};
    std::vector<CurveTrack> tracks_;
    std::vector<ClipEvent> markers_;
};

struct CurveSetHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0 && generation != 0; }
    auto operator<=>(const CurveSetHandle&) const = default;
};

struct CurvePlayerHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept { return index != 0 && generation != 0; }
    auto operator<=>(const CurvePlayerHandle&) const = default;
};

enum class CurveBindingRoute : std::uint8_t { SceneLocal, PhysicsKinematic, PhysicsTeleport };

struct CurveTransformOwner {
    CurvePlayerHandle player{};
};

struct CurvePlayerState {
    CurvePlayerHandle handle{};
    CurveSetHandle set{};
    ecs::Entity entity{};
    CurveBindingRoute route{CurveBindingRoute::SceneLocal};
    WrapMode wrap{WrapMode::Loop};
    double time{};
    double speed{1.0};
    bool playing{true};
    std::uint64_t applied_revision{};
    CurveError last_error{CurveError::None};
};

struct CurveMarkerEvent {
    CurvePlayerHandle player{};
    CurveSetHandle set{};
    ecs::Entity entity{};
    ClipEvent marker;
    std::int64_t cycle{};
    bool reverse{};
};

struct CurveUpdate {
    std::size_t applied{};
    std::size_t rejected{};
    std::size_t retired{};
    std::size_t truncated_players{};
    std::vector<CurveMarkerEvent> events;
};

// Single writer. One player claims the complete entity transform, including
// while paused. Bone entities and AnimatedComponent owners are rejected. No
// canonical skeletal Pose is written. Scene-local curves run before physics;
// body curves must explicitly select a kinematic or teleport route. Missing
// channels preserve the captured baseline. Stop releases ownership and retains
// the last displayed transform. Seek is applied on the next update, without
// markers. Coordinate rebasing requires authored curves/baselines to be updated.
// One Scene/Registry is borrowed until clear_players(). Borrowed resource and
// state pointers may be invalidated by creation, replacement or removal; retain
// generation handles across those operations. Destruction touches no borrowers.
class CurveService final {
public:
    explicit CurveService(std::uint32_t sets = 512, std::uint32_t players = 1024);
    [[nodiscard]] CurveSetHandle create(CurveTrackSet set);
    [[nodiscard]] CurveResult replace(CurveSetHandle handle, CurveTrackSet set);
    bool destroy(CurveSetHandle handle) noexcept;
    [[nodiscard]] const CurveTrackSet* get(CurveSetHandle handle) const noexcept;
    [[nodiscard]] CurveSetHandle find(std::string_view name) const noexcept;
    [[nodiscard]] std::uint64_t revision(CurveSetHandle handle) const noexcept;
    [[nodiscard]] std::vector<CurveSetHandle> sets() const;
    [[nodiscard]] CurvePlayerHandle play(scene::Scene& scene_graph,
        physics::PhysicsWorld* physics_world, ecs::Entity entity, CurveSetHandle set,
        CurveBindingRoute route = CurveBindingRoute::SceneLocal,
        WrapMode wrap = WrapMode::Loop, double time = 0.0, double speed = 1.0);
    [[nodiscard]] CurvePlayerHandle player(ecs::Entity entity) const noexcept;
    [[nodiscard]] const CurvePlayerState* state(CurvePlayerHandle handle) const noexcept;
    [[nodiscard]] CurveResult seek(CurvePlayerHandle handle, double time) noexcept;
    bool pause(CurvePlayerHandle handle, bool paused = true) noexcept;
    bool set_speed(CurvePlayerHandle handle, double speed) noexcept;
    bool set_wrap(CurvePlayerHandle handle, WrapMode mode) noexcept;
    [[nodiscard]] CurveResult set_baseline(CurvePlayerHandle handle, const math::Transform& baseline) noexcept;
    bool stop(ecs::Registry& registry, CurvePlayerHandle handle);
    void clear_players(ecs::Registry& registry);
    [[nodiscard]] CurveUpdate update(scene::Scene& scene_graph,
        physics::PhysicsWorld* physics_world, double delta_seconds,
        std::size_t event_budget = 8192);
private:
    struct SetRecord {
        CurveSetHandle handle{};
        CurveTrackSet value;
        std::uint64_t revision{1};
    };
    struct PlayerRecord {
        CurvePlayerState state;
        math::Transform baseline{};
        bool dirty{true};
    };
    [[nodiscard]] SetRecord* set_record(CurveSetHandle handle) noexcept;
    [[nodiscard]] const SetRecord* set_record(CurveSetHandle handle) const noexcept;
    [[nodiscard]] PlayerRecord* player_record(CurvePlayerHandle handle) noexcept;
    [[nodiscard]] const PlayerRecord* player_record(CurvePlayerHandle handle) const noexcept;
    GenerationPool set_handles_;
    GenerationPool player_handles_;
    std::vector<std::optional<SetRecord>> sets_;
    std::vector<std::optional<PlayerRecord>> players_;
    scene::Scene* bound_scene_{};
    ecs::Registry* bound_registry_{};
};

void register_curves(core::Kernel& kernel);

} // namespace ge::animation
