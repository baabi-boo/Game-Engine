#include <ge/animation/curves.hpp>

#include <ge/core/runtime.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::animation {
namespace {

constexpr double minimum_spacing = 1.0e-6;
constexpr double maximum_duration = 86400.0;
constexpr double maximum_time = 1.0e12;
constexpr double maximum_value = 1.0e12;
constexpr std::size_t maximum_events = 8192;

bool bounded(double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= maximum_value;
}

bool bounded(math::Vec3 value) noexcept {
    return bounded(value.x) && bounded(value.y) && bounded(value.z);
}

bool finite_value(double value) noexcept { return std::isfinite(value); }
bool finite_value(math::Vec3 value) noexcept { return math::finite(value); }

bool valid_time(double time) noexcept {
    return std::isfinite(time) && std::abs(time) <= maximum_time;
}

bool valid_duration(double duration) noexcept {
    return std::isfinite(duration) && duration >= minimum_spacing && duration <= maximum_duration;
}

bool valid_limits(const CurveLimits& limits) noexcept {
    return limits.max_keys > 0 && limits.max_keys <= 65536 &&
        limits.max_tracks > 0 && limits.max_tracks <= 256 &&
        limits.max_markers <= maximum_events && limits.max_samples >= 2 &&
        limits.max_samples <= 262144 && limits.max_name_bytes > 0 && limits.max_name_bytes <= 1024;
}

bool valid_name(std::string_view name, std::size_t limit) noexcept {
    return !name.empty() && name.size() <= limit &&
        std::none_of(name.begin(), name.end(), [](char c) { return c == '\0' || c == '\n' || c == '\r'; });
}

bool valid_extrapolation(CurveExtrapolation mode) noexcept {
    return mode == CurveExtrapolation::Hold || mode == CurveExtrapolation::Linear ||
        mode == CurveExtrapolation::Loop || mode == CurveExtrapolation::PingPong;
}

bool valid_interpolation(CurveInterpolation mode) noexcept {
    return mode == CurveInterpolation::Step || mode == CurveInterpolation::Linear ||
        mode == CurveInterpolation::Hermite;
}

bool valid_tangent(CurveTangentPolicy mode) noexcept {
    return mode == CurveTangentPolicy::Manual || mode == CurveTangentPolicy::Flat ||
        mode == CurveTangentPolicy::Linear || mode == CurveTangentPolicy::Smooth ||
        mode == CurveTangentPolicy::Monotone;
}

double monotone_slope(double before, double after, double left, double right) noexcept {
    if (before == 0.0 || after == 0.0 || std::signbit(before) != std::signbit(after)) { return 0.0; }
    const double first_weight = 2.0 * right + left;
    const double second_weight = right + 2.0 * left;
    return (first_weight + second_weight) / (first_weight / before + second_weight / after);
}

math::Vec3 monotone_slope(math::Vec3 before, math::Vec3 after, double left, double right) noexcept {
    return {monotone_slope(before.x, after.x, left, right),
        monotone_slope(before.y, after.y, left, right),
        monotone_slope(before.z, after.z, left, right)};
}

template<class Value>
CurveResult prepare_keys(std::vector<CurveKey<Value>>& keys, const CurveLimits& limits) {
    if (!valid_limits(limits)) { return {CurveError::InvalidLimits}; }
    if (keys.empty() || keys.size() > limits.max_keys) { return {CurveError::Capacity}; }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        if (!std::isfinite(key.time) || key.time < 0.0 || key.time > maximum_duration ||
            (i > 0 && key.time - keys[i - 1].time < minimum_spacing)) {
            return {CurveError::InvalidTime, i};
        }
        if (!bounded(key.value) || !bounded(key.incoming) || !bounded(key.outgoing)) {
            return {CurveError::InvalidValue, i};
        }
        if (!valid_interpolation(key.interpolation) || !valid_tangent(key.tangents)) {
            return {CurveError::InvalidMode, i};
        }
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        auto& key = keys[i];
        if (key.tangents == CurveTangentPolicy::Manual) { continue; }
        if (keys.size() == 1 || key.tangents == CurveTangentPolicy::Flat) {
            key.incoming = Value{};
            key.outgoing = Value{};
            continue;
        }
        const std::size_t left = i == 0 ? 0 : i - 1;
        const std::size_t right = i + 1 == keys.size() ? i : i + 1;
        const double left_duration = i == 0 ? keys[1].time - key.time : key.time - keys[left].time;
        const double right_duration = right == i ? left_duration : keys[right].time - key.time;
        const Value left_slope = i == 0 ? (keys[1].value - key.value) / left_duration :
            (key.value - keys[left].value) / left_duration;
        const Value right_slope = right == i ? left_slope : (keys[right].value - key.value) / right_duration;
        if (key.tangents == CurveTangentPolicy::Linear) {
            key.incoming = left_slope;
            key.outgoing = right_slope;
        } else {
            const Value slope = key.tangents == CurveTangentPolicy::Monotone ?
                monotone_slope(left_slope, right_slope, left_duration, right_duration) :
                (left_slope * left_duration + right_slope * right_duration) / (left_duration + right_duration);
            key.incoming = slope;
            key.outgoing = slope;
        }
        if (!bounded(key.incoming) || !bounded(key.outgoing)) { return {CurveError::InvalidValue, i}; }
    }
    return {};
}

struct CurveLocation {
    double time{};
    double derivative{1.0};
    double extension{};
};

std::optional<CurveLocation> locate(double time, double begin, double end,
    CurveExtrapolation before, CurveExtrapolation after) noexcept {
    if (!valid_time(time)) { return std::nullopt; }
    if (time >= begin && time <= end) { return CurveLocation{time, 1.0, 0.0}; }
    const double edge = time < begin ? begin : end;
    const auto mode = time < begin ? before : after;
    if (mode == CurveExtrapolation::Hold) { return CurveLocation{edge, 0.0, 0.0}; }
    if (mode == CurveExtrapolation::Linear) { return CurveLocation{edge, 1.0, time - edge}; }
    const auto mapped = wrap_time(time - begin, end - begin,
        mode == CurveExtrapolation::Loop ? WrapMode::Loop : WrapMode::PingPong);
    if (!mapped) { return std::nullopt; }
    return CurveLocation{begin + mapped->time, mapped->reverse ? -1.0 : 1.0, 0.0};
}

template<class Key>
std::size_t segment_for(std::span<const Key> keys, double time) noexcept {
    const auto found = std::upper_bound(keys.begin(), keys.end(), time,
        [](double value, const Key& key) { return value < key.time; });
    if (found == keys.begin()) { return 0; }
    return std::min(static_cast<std::size_t>(found - keys.begin() - 1), keys.size() - 2);
}

template<class Value>
CurveSample<Value> evaluate_segment(const CurveKey<Value>& first,
    const CurveKey<Value>& second, double time, std::size_t segment) noexcept {
    CurveSample<Value> output;
    output.segment = segment;
    const double duration = second.time - first.time;
    const double u = math::clamp((time - first.time) / duration, 0.0, 1.0);
    if (first.interpolation == CurveInterpolation::Step) {
        output.value = u == 1.0 ? second.value : first.value;
        output.discontinuous = u == 1.0;
    } else if (first.interpolation == CurveInterpolation::Linear) {
        output.value = first.value * (1.0 - u) + second.value * u;
        output.velocity = (second.value - first.value) / duration;
    } else {
        const double square = u * u;
        const double cube = square * u;
        output.value = first.value * (2.0 * cube - 3.0 * square + 1.0) +
            first.outgoing * ((cube - 2.0 * square + u) * duration) +
            second.value * (-2.0 * cube + 3.0 * square) +
            second.incoming * ((cube - square) * duration);
        output.velocity = (first.value * (6.0 * square - 6.0 * u) +
            second.value * (-6.0 * square + 6.0 * u)) / duration +
            first.outgoing * (3.0 * square - 4.0 * u + 1.0) +
            second.incoming * (3.0 * square - 2.0 * u);
        output.acceleration = (first.value * (12.0 * u - 6.0) +
            second.value * (-12.0 * u + 6.0)) / (duration * duration) +
            (first.outgoing * (6.0 * u - 4.0) + second.incoming * (6.0 * u - 2.0)) / duration;
    }
    return output;
}

math::Vec3 rotation_velocity(math::Quat first, math::Quat second, double duration) noexcept {
    auto delta = math::normalized(second * math::conjugate(first));
    if (delta.w < 0.0) { delta = -delta; }
    const auto [axis, angle] = math::to_axis_angle(delta);
    return axis * (angle / duration);
}

bool valid_mapping(CurveTimeMapping mapping) noexcept {
    return std::isfinite(mapping.scale) && std::abs(mapping.scale) <= 1.0e6 && valid_time(mapping.offset);
}

bool valid_transform(const math::Transform& transform) noexcept {
    const double rotation_length = math::length(transform.rotation);
    return math::finite(transform) && std::isfinite(rotation_length) && rotation_length > 1.0e-8 &&
        std::abs(transform.scale.x) >= 1.0e-8 && std::abs(transform.scale.y) >= 1.0e-8 &&
        std::abs(transform.scale.z) >= 1.0e-8;
}

bool number(std::string_view text, double& output) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(output);
}

core::CommandResult command_result(CurveResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error))};
}

} // namespace

std::string_view describe(CurveError error) noexcept {
    switch (error) {
    case CurveError::None: return "ok";
    case CurveError::InvalidLimits: return "invalid curve limits";
    case CurveError::Capacity: return "curve capacity exceeded or required data empty";
    case CurveError::InvalidTime: return "time is nonfinite, out of bounds or not increasing";
    case CurveError::InvalidValue: return "invalid curve value or tangent";
    case CurveError::InvalidKey: return "key was not found";
    case CurveError::InvalidMode: return "unsupported interpolation or playback mode";
    case CurveError::InvalidName: return "invalid or duplicate name";
    case CurveError::DuplicateTrack: return "overlapping transform tracks";
    case CurveError::InvalidTrack: return "track type does not match its property";
    case CurveError::InvalidMarker: return "invalid or unsorted marker";
    case CurveError::InvalidHandle: return "stale curve handle";
    case CurveError::InvalidEntity: return "entity is unavailable";
    case CurveError::OwnershipConflict: return "transform belongs to another animation owner";
    case CurveError::PhysicsRouteRequired: return "body requires an explicit physics route";
    case CurveError::PhysicsRejected: return "physics rejected the curve binding or target";
    case CurveError::SceneRejected: return "scene rejected the curve transform";
    case CurveError::RevisionExhausted: return "curve revision exhausted";
    case CurveError::NonFiniteResult: return "curve evaluation produced an invalid result";
    }
    return "unknown curve error";
}

template<class Value>
CurveResult NumericCurve<Value>::assign(std::vector<CurveKey<Value>> keys, CurveLimits limits) {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) { return {CurveError::RevisionExhausted}; }
    const auto result = prepare_keys(keys, limits);
    if (!result) { return result; }
    keys_.swap(keys);
    limits_ = limits;
    ++revision_;
    return {};
}

template<class Value>
CurveResult NumericCurve<Value>::insert(CurveKey<Value> key) {
    if (!std::isfinite(key.time)) { return {CurveError::InvalidTime}; }
    auto candidate = keys_;
    const auto found = std::lower_bound(candidate.begin(), candidate.end(), key.time,
        [](const auto& stored, double time) { return stored.time < time; });
    if (found != candidate.end() && found->time == key.time) { *found = key; }
    else {
        if (candidate.size() >= limits_.max_keys) { return {CurveError::Capacity}; }
        candidate.insert(found, key);
    }
    return assign(std::move(candidate), limits_);
}

template<class Value>
CurveResult NumericCurve<Value>::erase(double time) {
    if (!std::isfinite(time)) { return {CurveError::InvalidTime}; }
    auto candidate = keys_;
    const auto found = std::lower_bound(candidate.begin(), candidate.end(), time,
        [](const auto& key, double value) { return key.time < value; });
    if (found == candidate.end() || found->time != time) { return {CurveError::InvalidKey}; }
    candidate.erase(found);
    return assign(std::move(candidate), limits_);
}

template<class Value>
CurveResult NumericCurve<Value>::set_extrapolation(CurveExtrapolation before, CurveExtrapolation after) noexcept {
    if (!valid_extrapolation(before) || !valid_extrapolation(after)) { return {CurveError::InvalidMode}; }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) { return {CurveError::RevisionExhausted}; }
    before_ = before;
    after_ = after;
    ++revision_;
    return {};
}

template<class Value>
std::optional<CurveSample<Value>> NumericCurve<Value>::sample(double time) const noexcept {
    if (keys_.empty() || !valid_time(time)) { return std::nullopt; }
    if (keys_.size() == 1) { return CurveSample<Value>{keys_.front().value}; }
    const auto location = locate(time, keys_.front().time, keys_.back().time, before_, after_);
    if (!location) { return std::nullopt; }
    const auto segment = segment_for<CurveKey<Value>>(keys_, location->time);
    auto output = evaluate_segment(keys_[segment], keys_[segment + 1], location->time, segment);
    if (location->extension != 0.0) {
        output.value = output.value + output.velocity * location->extension;
        output.acceleration = Value{};
    }
    output.velocity = output.velocity * location->derivative;
    output.acceleration = output.acceleration * (location->derivative * location->derivative);
    if (segment > 0 && location->time == keys_[segment].time &&
        keys_[segment - 1].interpolation == CurveInterpolation::Step) { output.discontinuous = true; }
    if (location->derivative == 0.0 || location->extension != 0.0) { output.discontinuous = false; }
    if (!finite_value(output.value) || !finite_value(output.velocity) || !finite_value(output.acceleration)) {
        return std::nullopt;
    }
    return output;
}

template<class Value>
CurveResult NumericCurve<Value>::resample(double begin, double end, std::size_t count,
    std::vector<CurveKey<Value>>& output) const {
    if (!valid_time(begin) || !valid_time(end) || begin < 0.0 || end > maximum_duration || end <= begin) {
        return {CurveError::InvalidTime};
    }
    if (count < 2 || count > limits_.max_samples || count > limits_.max_keys) { return {CurveError::Capacity}; }
    if ((end - begin) / static_cast<double>(count - 1) < minimum_spacing) { return {CurveError::InvalidTime}; }
    std::vector<CurveKey<Value>> candidate;
    candidate.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double time = math::lerp(begin, end, static_cast<double>(i) / static_cast<double>(count - 1));
        const auto value = sample(time);
        if (!value) { return {CurveError::NonFiniteResult, i}; }
        if (!bounded(value->value) || !bounded(value->velocity)) { return {CurveError::InvalidValue, i}; }
        if (i > 0 && time - candidate.back().time < minimum_spacing) { return {CurveError::InvalidTime, i}; }
        candidate.push_back({time, value->value, value->velocity, value->velocity,
            CurveInterpolation::Linear, CurveTangentPolicy::Manual});
    }
    output.swap(candidate);
    return {};
}

template<class Value>
std::span<const CurveKey<Value>> NumericCurve<Value>::keys() const noexcept { return keys_; }
template<class Value>
const CurveLimits& NumericCurve<Value>::limits() const noexcept { return limits_; }
template<class Value>
std::uint64_t NumericCurve<Value>::revision() const noexcept { return revision_; }
template<class Value>
double NumericCurve<Value>::begin_time() const noexcept { return keys_.empty() ? 0.0 : keys_.front().time; }
template<class Value>
double NumericCurve<Value>::end_time() const noexcept { return keys_.empty() ? 0.0 : keys_.back().time; }
template<class Value>
bool NumericCurve<Value>::valid() const noexcept { return !keys_.empty(); }
template<class Value>
CurveExtrapolation NumericCurve<Value>::before() const noexcept { return before_; }
template<class Value>
CurveExtrapolation NumericCurve<Value>::after() const noexcept { return after_; }

template class NumericCurve<double>;
template class NumericCurve<math::Vec3>;

CurveResult QuaternionCurve::assign(std::vector<QuaternionCurveKey> keys, CurveLimits limits) {
    if (!valid_limits(limits)) { return {CurveError::InvalidLimits}; }
    if (keys.empty() || keys.size() > limits.max_keys) { return {CurveError::Capacity}; }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) { return {CurveError::RevisionExhausted}; }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        auto& key = keys[i];
        if (!std::isfinite(key.time) || key.time < 0.0 || key.time > maximum_duration ||
            (i > 0 && key.time - keys[i - 1].time < minimum_spacing)) { return {CurveError::InvalidTime, i}; }
        const double magnitude = math::length(key.value);
        if (!math::finite(key.value) || !std::isfinite(magnitude) || magnitude < 1.0e-8 || magnitude > maximum_value) {
            return {CurveError::InvalidValue, i};
        }
        if (key.interpolation != CurveInterpolation::Step && key.interpolation != CurveInterpolation::Linear) {
            return {CurveError::InvalidMode, i};
        }
        key.value = math::normalized(key.value);
        if (i > 0 && math::dot(keys[i - 1].value, key.value) < 0.0) { key.value = -key.value; }
    }
    keys_.swap(keys);
    limits_ = limits;
    ++revision_;
    return {};
}

CurveResult QuaternionCurve::insert(QuaternionCurveKey key) {
    if (!std::isfinite(key.time)) { return {CurveError::InvalidTime}; }
    auto candidate = keys_;
    const auto found = std::lower_bound(candidate.begin(), candidate.end(), key.time,
        [](const auto& stored, double time) { return stored.time < time; });
    if (found != candidate.end() && found->time == key.time) { *found = key; }
    else {
        if (candidate.size() >= limits_.max_keys) { return {CurveError::Capacity}; }
        candidate.insert(found, key);
    }
    return assign(std::move(candidate), limits_);
}

CurveResult QuaternionCurve::erase(double time) {
    if (!std::isfinite(time)) { return {CurveError::InvalidTime}; }
    auto candidate = keys_;
    const auto found = std::lower_bound(candidate.begin(), candidate.end(), time,
        [](const auto& key, double value) { return key.time < value; });
    if (found == candidate.end() || found->time != time) { return {CurveError::InvalidKey}; }
    candidate.erase(found);
    return assign(std::move(candidate), limits_);
}

CurveResult QuaternionCurve::set_extrapolation(CurveExtrapolation before, CurveExtrapolation after) noexcept {
    if (!valid_extrapolation(before) || !valid_extrapolation(after)) { return {CurveError::InvalidMode}; }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) { return {CurveError::RevisionExhausted}; }
    before_ = before;
    after_ = after;
    ++revision_;
    return {};
}

std::optional<QuaternionCurveSample> QuaternionCurve::sample(double time) const noexcept {
    if (keys_.empty() || !valid_time(time)) { return std::nullopt; }
    if (keys_.size() == 1) { return QuaternionCurveSample{keys_.front().value}; }
    const auto location = locate(time, keys_.front().time, keys_.back().time, before_, after_);
    if (!location) { return std::nullopt; }
    const auto segment = segment_for<QuaternionCurveKey>(keys_, location->time);
    const auto& first = keys_[segment];
    const auto& second = keys_[segment + 1];
    const double duration = second.time - first.time;
    const double u = math::clamp((location->time - first.time) / duration, 0.0, 1.0);
    QuaternionCurveSample output;
    output.segment = segment;
    if (first.interpolation == CurveInterpolation::Step) {
        output.value = u == 1.0 ? second.value : first.value;
        output.discontinuous = u == 1.0;
    } else {
        output.angular_velocity = rotation_velocity(first.value, second.value, duration);
        const double speed = math::length(output.angular_velocity);
        output.value = u == 1.0 ? second.value : first.value;
        if (speed > 0.0) {
            const double angle = std::remainder(speed * (u * duration + location->extension), math::tau);
            output.value = math::normalized(math::from_axis_angle(output.angular_velocity / speed, angle) * first.value);
        }
        output.angular_velocity *= location->derivative;
    }
    if (segment > 0 && location->time == first.time &&
        keys_[segment - 1].interpolation == CurveInterpolation::Step) { output.discontinuous = true; }
    if (location->derivative == 0.0 || location->extension != 0.0) { output.discontinuous = false; }
    if (!math::finite(output.value) || !math::finite(output.angular_velocity)) { return std::nullopt; }
    return output;
}

std::span<const QuaternionCurveKey> QuaternionCurve::keys() const noexcept { return keys_; }
std::uint64_t QuaternionCurve::revision() const noexcept { return revision_; }
bool QuaternionCurve::valid() const noexcept { return !keys_.empty(); }

std::optional<MappedCurveTime> map_curve_time(double time, double duration, WrapMode wrap,
    CurveTimeMapping mapping, const ScalarCurve* warp) noexcept {
    if (!valid_time(time) || !valid_mapping(mapping)) { return std::nullopt; }
    const double affine = time * mapping.scale + mapping.offset;
    const auto wrapped = wrap_time(affine, duration, wrap);
    if (!wrapped) { return std::nullopt; }
    MappedCurveTime result{*wrapped, wrapped->reverse ? -mapping.scale : mapping.scale};
    if (duration == 0.0 || (wrap == WrapMode::Clamp && (affine < 0.0 || affine > duration))) {
        result.derivative = 0.0;
    }
    if (warp && duration > 0.0) {
        const auto remapped = warp->sample(wrapped->time / duration);
        if (!remapped || remapped->value < 0.0 || remapped->value > 1.0) { return std::nullopt; }
        result.wrapped.time = remapped->value * duration;
        result.derivative *= remapped->velocity;
    }
    if (!std::isfinite(result.derivative)) { return std::nullopt; }
    return result;
}

CurveResult CurveArcTable::build(const VectorCurve& curve, double begin, double end, std::size_t samples) {
    if (!valid_time(begin) || !valid_time(end) || end <= begin) { return {CurveError::InvalidTime}; }
    if (!curve.valid() || samples < 2 || samples > curve.limits().max_samples) { return {CurveError::Capacity}; }
    if (begin < curve.begin_time() || end > curve.end_time()) { return {CurveError::InvalidTime}; }
    for (std::size_t i = 0; i + 1 < curve.keys().size(); ++i) {
        if (curve.keys()[i].time < end && curve.keys()[i + 1].time > begin &&
            curve.keys()[i].interpolation == CurveInterpolation::Step) { return {CurveError::InvalidMode, i}; }
    }
    std::vector<ArcTimePoint> candidate;
    candidate.reserve(samples);
    math::Vec3 previous{};
    double distance = 0.0;
    for (std::size_t i = 0; i < samples; ++i) {
        const double time = math::lerp(begin, end, static_cast<double>(i) / static_cast<double>(samples - 1));
        const auto sampled = curve.sample(time);
        if (!sampled) { return {CurveError::NonFiniteResult, i}; }
        if (i > 0) { distance += math::distance(previous, sampled->value); }
        if (!std::isfinite(distance)) { return {CurveError::NonFiniteResult, i}; }
        candidate.push_back({time, distance});
        previous = sampled->value;
    }
    points_.swap(candidate);
    source_revision_ = curve.revision();
    return {};
}

std::optional<double> CurveArcTable::time_at_distance(double distance) const noexcept {
    if (points_.empty() || !std::isfinite(distance)) { return std::nullopt; }
    if (distance <= 0.0) { return points_.front().time; }
    if (distance >= points_.back().distance) { return points_.back().time; }
    const auto after = std::lower_bound(points_.begin(), points_.end(), distance,
        [](const auto& point, double target) { return point.distance < target; });
    const auto& before = *(after - 1);
    const double span = after->distance - before.distance;
    return span == 0.0 ? after->time : math::lerp(before.time, after->time, (distance - before.distance) / span);
}

std::optional<double> CurveArcTable::distance_at_time(double time) const noexcept {
    if (points_.empty() || !valid_time(time)) { return std::nullopt; }
    if (time <= points_.front().time) { return 0.0; }
    if (time >= points_.back().time) { return points_.back().distance; }
    const auto after = std::lower_bound(points_.begin(), points_.end(), time,
        [](const auto& point, double target) { return point.time < target; });
    const auto& before = *(after - 1);
    return math::lerp(before.distance, after->distance, (time - before.time) / (after->time - before.time));
}

std::span<const ArcTimePoint> CurveArcTable::points() const noexcept { return points_; }
double CurveArcTable::length() const noexcept { return points_.empty() ? 0.0 : points_.back().distance; }
std::uint64_t CurveArcTable::source_revision() const noexcept { return source_revision_; }

namespace {

struct MarkerCursor {
    long double position{};
    long double offset{};
    long double period{};
    std::int64_t cycle{};
    std::size_t marker{};
    bool reverse{};
};

struct MarkerOrder {
    bool forward{};
    bool operator()(const MarkerCursor& first, const MarkerCursor& second) const noexcept {
        if (first.position != second.position) {
            return forward ? first.position > second.position : first.position < second.position;
        }
        if (first.marker != second.marker) { return first.marker > second.marker; }
        return first.reverse && !second.reverse;
    }
};

CurveResult validate_markers(std::span<const ClipEvent> markers, double duration, const CurveLimits& limits) {
    if (markers.size() > limits.max_markers) { return {CurveError::Capacity}; }
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const auto& marker = markers[i];
        if (!std::isfinite(marker.time) || marker.time < 0.0 || marker.time > duration ||
            (i > 0 && marker.time < markers[i - 1].time) ||
            !valid_name(marker.name, limits.max_name_bytes) || marker.payload.size() > 4096) {
            return {CurveError::InvalidMarker, i};
        }
    }
    return {};
}

std::optional<std::vector<ecs::Entity>> skeletal_claims(const ecs::Registry& registry) {
    constexpr std::size_t inspection_limit = 1'048'576;
    std::vector<ecs::Entity> claims;
    for (const auto owner : registry.view<AnimatedComponent>(true)) {
        const auto* component = registry.get<AnimatedComponent>(owner);
        if (!component) { continue; }
        if (claims.size() == inspection_limit || component->bone_entities.size() > inspection_limit - claims.size() - 1) {
            return std::nullopt;
        }
        claims.push_back(owner);
        claims.insert(claims.end(), component->bone_entities.begin(), component->bone_entities.end());
    }
    std::sort(claims.begin(), claims.end());
    claims.erase(std::unique(claims.begin(), claims.end()), claims.end());
    return claims;
}

CurveError binding_error(scene::Scene& scene_graph, physics::PhysicsWorld* physics_world,
    ecs::Entity entity, CurveBindingRoute route, std::span<const ecs::Entity> animated) {
    auto& registry = scene_graph.registry();
    if (!registry.alive(entity) || !scene_graph.contains(entity)) { return CurveError::InvalidEntity; }
    if (std::binary_search(animated.begin(), animated.end(), entity)) { return CurveError::OwnershipConflict; }
    const bool matching_world = physics_world && &physics_world->scene() == &scene_graph;
    const auto* body = matching_world ? physics_world->get(entity) : nullptr;
    if (route == CurveBindingRoute::SceneLocal) {
        return body || registry.has<physics::BodyComponent>(entity) ?
            CurveError::PhysicsRouteRequired : CurveError::None;
    }
    if (route != CurveBindingRoute::PhysicsKinematic && route != CurveBindingRoute::PhysicsTeleport) {
        return CurveError::InvalidMode;
    }
    if (!body || physics_world->stepping() ||
        (route == CurveBindingRoute::PhysicsKinematic && body->mode != physics::BodyMode::Kinematic)) {
        return CurveError::PhysicsRejected;
    }
    return CurveError::None;
}

std::optional<WrapMode> parse_wrap(std::string_view text) noexcept {
    if (text == "clamp") { return WrapMode::Clamp; }
    if (text == "loop") { return WrapMode::Loop; }
    if (text == "pingpong") { return WrapMode::PingPong; }
    return std::nullopt;
}

} // namespace

CurveMarkerTraversal traverse_curve_markers(std::span<const ClipEvent> markers, double duration,
    double from, double to, WrapMode wrap, std::size_t budget) {
    CurveMarkerTraversal output;
    if (!valid_duration(duration) || !wrap_time(from, duration, wrap) || !wrap_time(to, duration, wrap)) {
        output.result = {CurveError::InvalidTime};
        return output;
    }
    CurveLimits limits;
    limits.max_markers = maximum_events;
    limits.max_name_bytes = 1024;
    output.result = validate_markers(markers, duration, limits);
    if (!output.result || from == to || markers.empty()) { return output; }
    budget = std::min(budget, maximum_events);
    const bool forward = to > from;
    const long double start = from;
    const long double finish = to;
    const auto within = [forward, start, finish](long double position) {
        return forward ? position > start && position <= finish : position < start && position >= finish;
    };
    std::vector<MarkerCursor> heap;
    heap.reserve(markers.size() * 2);
    const long double period = wrap == WrapMode::Clamp ? 0.0L :
        static_cast<long double>(duration) * (wrap == WrapMode::PingPong ? 2.0L : 1.0L);
    const auto append = [&](std::size_t index, long double offset, bool reverse) {
        std::int64_t cycle = 0;
        long double position = offset;
        if (period > 0.0L) {
            const long double quotient = (start - offset) / period;
            cycle = static_cast<std::int64_t>(forward ? std::floor(quotient) + 1.0L : std::ceil(quotient) - 1.0L);
            position = offset + static_cast<long double>(cycle) * period;
            if (forward ? position <= start : position >= start) {
                cycle += forward ? 1 : -1;
                position = offset + static_cast<long double>(cycle) * period;
            }
            const auto previous_cycle = cycle - (forward ? 1 : -1);
            const long double previous = offset + static_cast<long double>(previous_cycle) * period;
            if (forward ? previous > start : previous < start) {
                cycle = previous_cycle;
                position = previous;
            }
        }
        if (within(position)) { heap.push_back({position, offset, period, cycle, index, reverse}); }
    };
    for (std::size_t i = 0; i < markers.size(); ++i) {
        append(i, markers[i].time, false);
        if (wrap == WrapMode::PingPong && markers[i].time > 0.0 && markers[i].time < duration) {
            append(i, period - markers[i].time, true);
        }
    }
    const MarkerOrder order{forward};
    std::make_heap(heap.begin(), heap.end(), order);
    output.events.reserve(std::min(budget, markers.size()));
    while (!heap.empty() && output.events.size() < budget) {
        std::pop_heap(heap.begin(), heap.end(), order);
        auto cursor = heap.back();
        heap.pop_back();
        bool reverse = cursor.reverse != !forward;
        if (wrap == WrapMode::PingPong && markers[cursor.marker].time == 0.0) { reverse = true; }
        if (wrap == WrapMode::PingPong && markers[cursor.marker].time == duration) { reverse = false; }
        output.events.push_back({cursor.marker, markers[cursor.marker].time,
            static_cast<double>(cursor.position), cursor.cycle, reverse});
        if (cursor.period == 0.0L) { continue; }
        cursor.cycle += forward ? 1 : -1;
        cursor.position = cursor.offset + static_cast<long double>(cursor.cycle) * cursor.period;
        if (within(cursor.position)) {
            heap.push_back(cursor);
            std::push_heap(heap.begin(), heap.end(), order);
        }
    }
    output.truncated = !heap.empty();
    return output;
}

CurveResult CurveTrackSet::assign(std::string name, double duration, std::vector<CurveTrack> tracks,
    std::vector<ClipEvent> markers, CurveLimits limits) {
    if (!valid_limits(limits)) { return {CurveError::InvalidLimits}; }
    if (!valid_name(name, limits.max_name_bytes)) { return {CurveError::InvalidName}; }
    if (!valid_duration(duration)) { return {CurveError::InvalidTime}; }
    if (tracks.size() > limits.max_tracks) { return {CurveError::Capacity}; }
    std::array<bool, 4> properties{};
    std::size_t key_count = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const auto& track = tracks[i];
        if (!valid_name(track.name, limits.max_name_bytes)) { return {CurveError::InvalidName, i}; }
        for (std::size_t earlier = 0; earlier < i; ++earlier) {
            if (track.name == tracks[earlier].name) { return {CurveError::InvalidName, i}; }
        }
        const auto property = static_cast<std::size_t>(track.property);
        if (property >= properties.size()) { return {CurveError::InvalidTrack, i}; }
        if (properties[property] || (track.property == CurveProperty::Scale && properties[3]) ||
            (track.property == CurveProperty::UniformScale && properties[2])) { return {CurveError::DuplicateTrack, i}; }
        properties[property] = true;
        const bool correct_type = track.property == CurveProperty::Rotation ?
            std::holds_alternative<QuaternionCurve>(track.data) : track.property == CurveProperty::UniformScale ?
            std::holds_alternative<ScalarCurve>(track.data) : std::holds_alternative<VectorCurve>(track.data);
        if (!correct_type || !valid_mapping(track.time) ||
            !std::visit([](const auto& curve) { return curve.valid(); }, track.data)) { return {CurveError::InvalidTrack, i}; }
        const auto count = std::visit([](const auto& curve) { return curve.keys().size(); }, track.data);
        if (count > limits.max_keys - key_count) { return {CurveError::Capacity, i}; }
        key_count += count;
    }
    const auto marker_result = validate_markers(markers, duration, limits);
    if (!marker_result) { return marker_result; }
    name_ = std::move(name);
    duration_ = duration;
    tracks_.swap(tracks);
    markers_.swap(markers);
    return {};
}

std::optional<math::Transform> CurveTrackSet::sample(double time, const math::Transform& baseline,
    WrapMode wrap) const noexcept {
    if (!valid() || !valid_transform(baseline)) { return std::nullopt; }
    const auto mapped = wrap_time(time, duration_, wrap);
    if (!mapped) { return std::nullopt; }
    auto output = baseline;
    for (const auto& track : tracks_) {
        const double local_time = mapped->time * track.time.scale + track.time.offset;
        if (const auto* vector = std::get_if<VectorCurve>(&track.data)) {
            const auto value = vector->sample(local_time);
            if (!value) { return std::nullopt; }
            if (track.property == CurveProperty::Position) { output.position = value->value; }
            else { output.scale = value->value; }
        } else if (const auto* rotation = std::get_if<QuaternionCurve>(&track.data)) {
            const auto value = rotation->sample(local_time);
            if (!value) { return std::nullopt; }
            output.rotation = value->value;
        } else {
            const auto value = std::get<ScalarCurve>(track.data).sample(local_time);
            if (!value) { return std::nullopt; }
            output.scale = {value->value, value->value, value->value};
        }
    }
    return valid_transform(output) ? std::optional<math::Transform>(output) : std::nullopt;
}

std::string_view CurveTrackSet::name() const noexcept { return name_; }
double CurveTrackSet::duration() const noexcept { return duration_; }
std::span<const CurveTrack> CurveTrackSet::tracks() const noexcept { return tracks_; }
std::span<const ClipEvent> CurveTrackSet::markers() const noexcept { return markers_; }
bool CurveTrackSet::valid() const noexcept { return !name_.empty() && valid_duration(duration_); }

CurveService::CurveService(std::uint32_t sets, std::uint32_t players)
    : set_handles_(sets), player_handles_(players) {
    if (sets > 4096 || players > 16384) { throw std::invalid_argument("curve service capacity exceeds hard bounds"); }
}

CurveService::SetRecord* CurveService::set_record(CurveSetHandle handle) noexcept {
    if (!set_handles_.alive({handle.index, handle.generation}) || handle.index > sets_.size()) { return nullptr; }
    auto& entry = sets_[static_cast<std::size_t>(handle.index - 1)];
    return entry ? &*entry : nullptr;
}

const CurveService::SetRecord* CurveService::set_record(CurveSetHandle handle) const noexcept {
    if (!set_handles_.alive({handle.index, handle.generation}) || handle.index > sets_.size()) { return nullptr; }
    const auto& entry = sets_[static_cast<std::size_t>(handle.index - 1)];
    return entry ? &*entry : nullptr;
}

CurveService::PlayerRecord* CurveService::player_record(CurvePlayerHandle handle) noexcept {
    if (!player_handles_.alive({handle.index, handle.generation}) || handle.index > players_.size()) { return nullptr; }
    auto& entry = players_[static_cast<std::size_t>(handle.index - 1)];
    return entry ? &*entry : nullptr;
}

const CurveService::PlayerRecord* CurveService::player_record(CurvePlayerHandle handle) const noexcept {
    if (!player_handles_.alive({handle.index, handle.generation}) || handle.index > players_.size()) { return nullptr; }
    const auto& entry = players_[static_cast<std::size_t>(handle.index - 1)];
    return entry ? &*entry : nullptr;
}

CurveSetHandle CurveService::create(CurveTrackSet set) {
    if (!set.valid() || find(set.name())) { return {}; }
    const auto token = set_handles_.acquire();
    if (!token) { return {}; }
    const CurveSetHandle handle{token->index, token->generation};
    try {
        if (sets_.size() < handle.index) { sets_.resize(handle.index); }
        sets_[static_cast<std::size_t>(handle.index - 1)].emplace(SetRecord{handle, std::move(set), 1});
    } catch (...) {
        (void)set_handles_.release(*token);
        throw;
    }
    return handle;
}

CurveResult CurveService::replace(CurveSetHandle handle, CurveTrackSet set) {
    auto* entry = set_record(handle);
    if (!entry) { return {CurveError::InvalidHandle}; }
    if (!set.valid()) { return {CurveError::InvalidTrack}; }
    const auto duplicate = find(set.name());
    if (duplicate && duplicate != handle) { return {CurveError::InvalidName}; }
    if (entry->revision == std::numeric_limits<std::uint64_t>::max()) { return {CurveError::RevisionExhausted}; }
    entry->value = std::move(set);
    ++entry->revision;
    return {};
}

bool CurveService::destroy(CurveSetHandle handle) noexcept {
    if (!set_record(handle)) { return false; }
    sets_[static_cast<std::size_t>(handle.index - 1)].reset();
    return set_handles_.release({handle.index, handle.generation});
}

const CurveTrackSet* CurveService::get(CurveSetHandle handle) const noexcept {
    const auto* entry = set_record(handle);
    return entry ? &entry->value : nullptr;
}

CurveSetHandle CurveService::find(std::string_view name) const noexcept {
    for (const auto& entry : sets_) {
        if (entry && entry->value.name() == name) { return entry->handle; }
    }
    return {};
}

std::uint64_t CurveService::revision(CurveSetHandle handle) const noexcept {
    const auto* entry = set_record(handle);
    return entry ? entry->revision : 0;
}

std::vector<CurveSetHandle> CurveService::sets() const {
    std::vector<CurveSetHandle> output;
    output.reserve(set_handles_.size());
    for (const auto& entry : sets_) { if (entry) { output.push_back(entry->handle); } }
    return output;
}

CurvePlayerHandle CurveService::play(scene::Scene& scene_graph, physics::PhysicsWorld* physics_world,
    ecs::Entity entity, CurveSetHandle set, CurveBindingRoute route, WrapMode wrap, double time, double speed) {
    if (bound_scene_ && bound_scene_ != &scene_graph) { return {}; }
    const auto* resource = get(set);
    auto& registry = scene_graph.registry();
    if (!resource || !std::isfinite(speed) || std::abs(speed) > 1.0e4 || player(entity) ||
        registry.has<CurveTransformOwner>(entity)) { return {}; }
    const auto mapped = wrap_time(time, resource->duration(), wrap);
    const auto claimed = skeletal_claims(registry);
    if (!mapped || !claimed || binding_error(scene_graph, physics_world, entity, route, *claimed) != CurveError::None) {
        return {};
    }
    const auto baseline = route == CurveBindingRoute::SceneLocal ? scene_graph.local_transform(entity) :
        std::optional<math::Transform>(physics_world->get(entity)->pose);
    const auto initial = baseline ? resource->sample(time, *baseline, wrap) : std::nullopt;
    if (!initial || (route == CurveBindingRoute::PhysicsKinematic &&
        !math::almost_equal(initial->scale, baseline->scale))) { return {}; }
    const auto token = player_handles_.acquire();
    if (!token) { return {}; }
    const CurvePlayerHandle handle{token->index, token->generation};
    try {
        if (players_.size() < handle.index) { players_.resize(handle.index); }
        registry.emplace<CurveTransformOwner>(entity, CurveTransformOwner{handle});
        CurvePlayerState state_value{handle, set, entity, route, wrap,
            wrap == WrapMode::Clamp ? mapped->time : time, speed, true, 0, CurveError::None};
        players_[static_cast<std::size_t>(handle.index - 1)].emplace(PlayerRecord{state_value, *baseline, true});
    } catch (...) {
        (void)player_handles_.release(*token);
        throw;
    }
    bound_scene_ = &scene_graph;
    bound_registry_ = &registry;
    return handle;
}

CurvePlayerHandle CurveService::player(ecs::Entity entity) const noexcept {
    for (const auto& entry : players_) {
        if (entry && entry->state.entity == entity) { return entry->state.handle; }
    }
    return {};
}

const CurvePlayerState* CurveService::state(CurvePlayerHandle handle) const noexcept {
    const auto* entry = player_record(handle);
    return entry ? &entry->state : nullptr;
}

CurveResult CurveService::seek(CurvePlayerHandle handle, double time) noexcept {
    auto* entry = player_record(handle);
    const auto* resource = entry ? get(entry->state.set) : nullptr;
    if (!resource) { return {CurveError::InvalidHandle}; }
    const auto mapped = wrap_time(time, resource->duration(), entry->state.wrap);
    if (!mapped) { return {CurveError::InvalidTime}; }
    entry->state.time = entry->state.wrap == WrapMode::Clamp ? mapped->time : time;
    entry->dirty = true;
    return {};
}

bool CurveService::pause(CurvePlayerHandle handle, bool paused) noexcept {
    auto* entry = player_record(handle);
    if (!entry) { return false; }
    entry->state.playing = !paused;
    return true;
}

bool CurveService::set_speed(CurvePlayerHandle handle, double speed) noexcept {
    auto* entry = player_record(handle);
    if (!entry || !std::isfinite(speed) || std::abs(speed) > 1.0e4) { return false; }
    entry->state.speed = speed;
    return true;
}

bool CurveService::set_wrap(CurvePlayerHandle handle, WrapMode mode) noexcept {
    auto* entry = player_record(handle);
    const auto* resource = entry ? get(entry->state.set) : nullptr;
    if (!resource) { return false; }
    const auto mapped = wrap_time(entry->state.time, resource->duration(), mode);
    if (!mapped) { return false; }
    entry->state.wrap = mode;
    if (mode == WrapMode::Clamp) { entry->state.time = mapped->time; }
    entry->dirty = true;
    return true;
}

CurveResult CurveService::set_baseline(CurvePlayerHandle handle, const math::Transform& baseline) noexcept {
    auto* entry = player_record(handle);
    if (!entry) { return {CurveError::InvalidHandle}; }
    if (!valid_transform(baseline)) { return {CurveError::InvalidValue}; }
    entry->baseline = baseline;
    entry->dirty = true;
    return {};
}

bool CurveService::stop(ecs::Registry& registry, CurvePlayerHandle handle) {
    if (bound_registry_ && bound_registry_ != &registry) { return false; }
    auto* entry = player_record(handle);
    if (!entry) { return false; }
    const auto* owner = registry.get<CurveTransformOwner>(entry->state.entity);
    if (owner && owner->player == handle) { (void)registry.remove<CurveTransformOwner>(entry->state.entity); }
    players_[static_cast<std::size_t>(handle.index - 1)].reset();
    return player_handles_.release({handle.index, handle.generation});
}

void CurveService::clear_players(ecs::Registry& registry) {
    if (bound_registry_ && bound_registry_ != &registry) { return; }
    for (const auto& entry : players_) { if (entry) { (void)stop(registry, entry->state.handle); } }
    bound_scene_ = nullptr;
    bound_registry_ = nullptr;
}

CurveUpdate CurveService::update(scene::Scene& scene_graph, physics::PhysicsWorld* physics_world,
    double delta_seconds, std::size_t event_budget) {
    CurveUpdate report;
    if (player_handles_.size() == 0) { return report; }
    if (bound_scene_ && bound_scene_ != &scene_graph) {
        report.rejected = player_handles_.size();
        return report;
    }
    auto& registry = scene_graph.registry();
    const auto animated = skeletal_claims(registry);
    if (!animated) {
        for (auto& entry : players_) {
            if (entry) { entry->state.last_error = CurveError::Capacity; entry->state.playing = false; }
        }
        report.rejected = player_handles_.size();
        return report;
    }
    event_budget = std::min(event_budget, maximum_events);
    report.events.reserve(std::min(event_budget, static_cast<std::size_t>(player_handles_.size())));
    for (auto& entry : players_) {
        if (!entry) { continue; }
        auto& playback = entry->state;
        const auto* resource = set_record(playback.set);
        if (!resource || !registry.alive(playback.entity) || !scene_graph.contains(playback.entity)) {
            (void)stop(registry, playback.handle);
            ++report.retired;
            continue;
        }
        CurveError error = binding_error(scene_graph, physics_world, playback.entity, playback.route, *animated);
        const auto* owner = registry.get<CurveTransformOwner>(playback.entity);
        if (!owner || owner->player != playback.handle) { error = CurveError::OwnershipConflict; }
        if (!std::isfinite(delta_seconds) || delta_seconds < 0.0 || delta_seconds > maximum_duration) {
            error = CurveError::InvalidTime;
        }
        if (error != CurveError::None) {
            playback.last_error = error;
            playback.playing = false;
            ++report.rejected;
            continue;
        }
        if (!scene_graph.effective_enabled(playback.entity) ||
            (!playback.playing && !entry->dirty && playback.applied_revision == resource->revision)) { continue; }
        const double current = playback.wrap == WrapMode::Clamp ?
            math::clamp(playback.time, 0.0, resource->value.duration()) : playback.time;
        double next = current + (playback.playing ? delta_seconds * playback.speed : 0.0);
        const auto mapped = wrap_time(next, resource->value.duration(), playback.wrap);
        if (!mapped) {
            playback.last_error = CurveError::InvalidTime;
            playback.playing = false;
            ++report.rejected;
            continue;
        }
        if (playback.wrap == WrapMode::Clamp) { next = mapped->time; }
        const auto transform = resource->value.sample(next, entry->baseline, playback.wrap);
        const auto traversal = traverse_curve_markers(resource->value.markers(), resource->value.duration(),
            current, next, playback.wrap, event_budget - report.events.size());
        if (!transform || !traversal.result) { error = CurveError::NonFiniteResult; }
        else if (playback.route == CurveBindingRoute::SceneLocal) {
            if (!scene_graph.set_local_transform(playback.entity, *transform)) { error = CurveError::SceneRejected; }
        } else {
            const auto body = physics_world->find(playback.entity);
            const bool applied = playback.route == CurveBindingRoute::PhysicsKinematic ?
                physics_world->set_kinematic_target(body, *transform) : physics_world->set_pose(body, *transform, true);
            if (!applied) { error = CurveError::PhysicsRejected; }
        }
        playback.last_error = error;
        if (error != CurveError::None) {
            playback.playing = false;
            ++report.rejected;
            continue;
        }
        for (const auto& event : traversal.events) {
            report.events.push_back({playback.handle, playback.set, playback.entity,
                resource->value.markers()[event.event_index], event.cycle, event.reverse});
        }
        if (traversal.truncated) { ++report.truncated_players; }
        playback.time = next;
        playback.applied_revision = resource->revision;
        entry->dirty = false;
        if (playback.wrap == WrapMode::Clamp && ((playback.speed > 0.0 && next >= resource->value.duration()) ||
            (playback.speed < 0.0 && next <= 0.0))) { playback.playing = false; }
        ++report.applied;
    }
    return report;
}

void register_curves(core::Kernel& kernel) {
    auto& service = kernel.services().emplace<CurveService>();
    (void)kernel.register_module({"animation.curves", "Reusable typed curves, transform tracks and bounded timeline traversal", 1});
    (void)kernel.add_system({"animation.curves.advance", core::Phase::PreSimulation, -1000, {}},
        [owner = &service](core::Kernel& active, const core::Frame& frame) {
            auto* scene_graph = active.services().find<scene::Scene>();
            if (!scene_graph) { return; }
            auto result = owner->update(*scene_graph, active.services().find<physics::PhysicsWorld>(), frame.delta_seconds);
            std::size_t dropped = 0;
            for (auto& event : result.events) { if (!active.events().publish(std::move(event))) { ++dropped; } }
            active.metric("animation.curves.applied", static_cast<double>(result.applied));
            active.metric("animation.curves.rejected", static_cast<double>(result.rejected));
            active.metric("animation.curves.truncated", static_cast<double>(result.truncated_players));
            active.metric("animation.curves.dropped_events", static_cast<double>(dropped));
        });
    (void)kernel.add_shutdown("animation.curves.release", [owner = &service, host = &kernel] {
        if (auto* scene_graph = host->services().find<scene::Scene>()) { owner->clear_players(scene_graph->registry()); }
    });
    (void)kernel.add_command("curve.create", [owner = &service](core::Kernel&, core::CommandArgs args) {
        double duration = 0.0;
        if (args.size() != 2 || !number(args[1], duration)) {
            return core::CommandResult{false, "usage: curve.create name duration_seconds"};
        }
        CurveTrackSet set;
        const auto loaded = set.assign(args[0], duration, {});
        if (!loaded) { return command_result(loaded); }
        const auto handle = owner->create(std::move(set));
        return core::CommandResult{static_cast<bool>(handle), handle ? "curve set created" : "duplicate name or capacity exhausted"};
    }, "Create an editable transform track set");
    (void)kernel.add_command("curve.key", [owner = &service](core::Kernel&, core::CommandArgs args) {
        const auto usage = core::CommandResult{false,
            "usage: curve.key set position|scale|rotation|uniform time values... [step|linear|smooth|monotone]"};
        if (args.size() < 4) { return usage; }
        CurveProperty property{};
        std::size_t values = 0;
        if (args[1] == "position") { property = CurveProperty::Position; values = 3; }
        else if (args[1] == "scale") { property = CurveProperty::Scale; values = 3; }
        else if (args[1] == "rotation") { property = CurveProperty::Rotation; values = 4; }
        else if (args[1] == "uniform") { property = CurveProperty::UniformScale; values = 1; }
        else { return usage; }
        if (args.size() != values + 3 && args.size() != values + 4) { return usage; }
        std::array<double, 5> numbers{};
        for (std::size_t i = 0; i <= values; ++i) { if (!number(args[i + 2], numbers[i])) { return usage; } }
        auto interpolation = CurveInterpolation::Linear;
        auto tangent = CurveTangentPolicy::Smooth;
        if (args.size() == values + 4) {
            const auto& mode = args.back();
            if (mode == "step") { interpolation = CurveInterpolation::Step; }
            else if (mode == "smooth" || mode == "monotone") {
                interpolation = CurveInterpolation::Hermite;
                tangent = mode == "smooth" ? CurveTangentPolicy::Smooth : CurveTangentPolicy::Monotone;
            } else if (mode != "linear") { return usage; }
        }
        const auto handle = owner->find(args[0]);
        const auto* source = owner->get(handle);
        if (!source) { return command_result({CurveError::InvalidHandle}); }
        auto tracks = std::vector<CurveTrack>(source->tracks().begin(), source->tracks().end());
        auto found = std::find_if(tracks.begin(), tracks.end(), [property](const auto& track) { return track.property == property; });
        if (found == tracks.end()) {
            CurveTrackData data = property == CurveProperty::Rotation ? CurveTrackData{QuaternionCurve{}} :
                property == CurveProperty::UniformScale ? CurveTrackData{ScalarCurve{}} : CurveTrackData{VectorCurve{}};
            tracks.push_back({args[1], property, std::move(data), {}});
            found = tracks.end() - 1;
        }
        CurveResult inserted;
        if (property == CurveProperty::Rotation) {
            inserted = std::get<QuaternionCurve>(found->data).insert({numbers[0],
                {numbers[1], numbers[2], numbers[3], numbers[4]}, interpolation});
        } else if (property == CurveProperty::UniformScale) {
            inserted = std::get<ScalarCurve>(found->data).insert({numbers[0], numbers[1], 0.0, 0.0, interpolation, tangent});
        } else {
            inserted = std::get<VectorCurve>(found->data).insert({numbers[0],
                {numbers[1], numbers[2], numbers[3]}, {}, {}, interpolation, tangent});
        }
        if (!inserted) { return command_result(inserted); }
        CurveTrackSet replacement;
        const auto loaded = replacement.assign(std::string(source->name()), source->duration(), std::move(tracks),
            std::vector<ClipEvent>(source->markers().begin(), source->markers().end()));
        return loaded ? command_result(owner->replace(handle, std::move(replacement))) : command_result(loaded);
    }, "Insert or replace a scalar, vector or quaternion key; rotation values are x y z w");
    (void)kernel.add_command("curve.marker", [owner = &service](core::Kernel&, core::CommandArgs args) {
        double time = 0.0;
        if ((args.size() != 3 && args.size() != 4) || !number(args[1], time)) {
            return core::CommandResult{false, "usage: curve.marker set time name [payload]"};
        }
        const auto handle = owner->find(args[0]);
        const auto* source = owner->get(handle);
        if (!source) { return command_result({CurveError::InvalidHandle}); }
        auto markers = std::vector<ClipEvent>(source->markers().begin(), source->markers().end());
        if (markers.size() >= CurveLimits{}.max_markers) { return command_result({CurveError::Capacity}); }
        const auto location = std::upper_bound(markers.begin(), markers.end(), time,
            [](double value, const auto& marker) { return value < marker.time; });
        markers.insert(location, ClipEvent{time, args[2], args.size() == 4 ? args[3] : std::string{}});
        CurveTrackSet replacement;
        const auto loaded = replacement.assign(std::string(source->name()), source->duration(),
            std::vector<CurveTrack>(source->tracks().begin(), source->tracks().end()), std::move(markers));
        return loaded ? command_result(owner->replace(handle, std::move(replacement))) : command_result(loaded);
    }, "Append an ordered timeline marker");
    (void)kernel.add_command("curve.play", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* scene_graph = active.services().find<scene::Scene>();
        if (!scene_graph || args.size() < 2 || args.size() > 4) {
            return core::CommandResult{false, "usage: curve.play set entity [local|kinematic|teleport] [clamp|loop|pingpong]"};
        }
        auto route = CurveBindingRoute::SceneLocal;
        if (args.size() >= 3) {
            if (args[2] == "kinematic") { route = CurveBindingRoute::PhysicsKinematic; }
            else if (args[2] == "teleport") { route = CurveBindingRoute::PhysicsTeleport; }
            else if (args[2] != "local") { return command_result({CurveError::InvalidMode}); }
        }
        const auto wrap = args.size() == 4 ? parse_wrap(args[3]) : std::optional<WrapMode>(WrapMode::Loop);
        if (!wrap) { return command_result({CurveError::InvalidMode}); }
        const auto entity = scene_graph->registry().find_name(args[1]);
        const auto handle = owner->play(*scene_graph, active.services().find<physics::PhysicsWorld>(), entity,
            owner->find(args[0]), route, *wrap);
        return core::CommandResult{static_cast<bool>(handle), handle ? "curve player started" :
            "binding rejected: inspect entity ownership, physics route and resource"};
    }, "Claim an entity transform and start timeline playback");
    (void)kernel.add_command("curve.seek", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        double time = 0.0;
        if (!registry || args.size() != 2 || !number(args[1], time)) {
            return core::CommandResult{false, "usage: curve.seek entity time"};
        }
        return command_result(owner->seek(owner->player(registry->find_name(args[0])), time));
    }, "Seek without replaying skipped markers");
    (void)kernel.add_command("curve.control", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() < 2 || args.size() > 3) {
            return core::CommandResult{false, "usage: curve.control entity pause|resume|stop|speed|wrap [value]"};
        }
        const auto handle = owner->player(registry->find_name(args[0]));
        bool changed = false;
        if (args.size() == 2 && args[1] == "pause") { changed = owner->pause(handle); }
        else if (args.size() == 2 && args[1] == "resume") { changed = owner->pause(handle, false); }
        else if (args.size() == 2 && args[1] == "stop") { changed = owner->stop(*registry, handle); }
        else if (args.size() == 3 && args[1] == "speed") {
            double speed = 0.0;
            changed = number(args[2], speed) && owner->set_speed(handle, speed);
        } else if (args.size() == 3 && args[1] == "wrap") {
            const auto wrap = parse_wrap(args[2]);
            changed = wrap && owner->set_wrap(handle, *wrap);
        }
        return core::CommandResult{changed, changed ? "curve player updated" : "invalid player control"};
    }, "Pause, resume, release, reverse or change loop behavior");
    (void)kernel.add_command("curve.inspect", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        if (args.size() > 1) { return core::CommandResult{false, "usage: curve.inspect [set_or_entity]"}; }
        std::ostringstream message;
        for (const auto handle : owner->sets()) {
            const auto* set = owner->get(handle);
            if (!args.empty() && args[0] != set->name()) { continue; }
            message << set->name() << " duration=" << set->duration() << " tracks=" << set->tracks().size()
                << " markers=" << set->markers().size() << " revision=" << owner->revision(handle) << '\n';
        }
        auto* registry = active.services().find<ecs::Registry>();
        if (registry && !args.empty()) {
            const auto* playback = owner->state(owner->player(registry->find_name(args[0])));
            if (playback) {
                message << "time=" << playback->time << " speed=" << playback->speed << " playing=" << playback->playing
                    << " last_error=" << describe(playback->last_error) << '\n';
            }
        }
        return core::CommandResult{true, message.str()};
    }, "Inspect track resources or entity playback state");
}

} // namespace ge::animation
