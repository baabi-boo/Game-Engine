#include <ge/animation/skeleton.hpp>

#include <ge/core/runtime.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ge::animation {
namespace {

constexpr double maximum_time = 1.0e12;
constexpr double minimum_duration = 1.0e-6;
constexpr AnimationLimits hard_limits{};

bool valid_limits(const AnimationLimits& limits) noexcept {
    return limits.max_bones > 0 && limits.max_bones <= hard_limits.max_bones &&
        limits.max_channels <= hard_limits.max_channels &&
        limits.max_keys <= hard_limits.max_keys && limits.max_events <= hard_limits.max_events &&
        limits.max_name_bytes > 0 && limits.max_name_bytes <= hard_limits.max_name_bytes &&
        std::isfinite(limits.max_duration) && limits.max_duration >= minimum_duration &&
        limits.max_duration <= hard_limits.max_duration;
}

bool valid_name(std::string_view name, std::size_t maximum = 1024) noexcept {
    return !name.empty() && name.size() <= maximum &&
        std::none_of(name.begin(), name.end(), [](char value) {
            const auto byte = static_cast<unsigned char>(value);
            return byte < 32U || byte == 127U;
        });
}

bool valid_rotation(math::Quat value) noexcept {
    const double squared = math::dot(value, value);
    return math::finite(value) && std::isfinite(squared) && squared > math::epsilon * math::epsilon;
}

bool valid_transform(const math::Transform& value) noexcept {
    return math::finite(value) && valid_rotation(value.rotation);
}

bool affine(const math::Mat4& value) noexcept {
    return math::finite(value) && std::abs(value(3, 0)) <= math::epsilon &&
        std::abs(value(3, 1)) <= math::epsilon && std::abs(value(3, 2)) <= math::epsilon &&
        std::abs(value(3, 3) - 1.0) <= math::epsilon;
}

void hash_word(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 255U;
        hash *= 1099511628211ULL;
    }
}

std::uint64_t skeleton_hash(std::span<const Bone> bones) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash_word(hash, static_cast<std::uint64_t>(bones.size()));
    for (const auto& bone : bones) {
        hash_word(hash, bone.parent);
        hash_word(hash, static_cast<std::uint64_t>(bone.name.size()));
        for (const char letter : bone.name) {
            hash_word(hash, static_cast<unsigned char>(letter));
        }
        for (const double value : math::matrix(bone.bind_local).data) {
            hash_word(hash, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
        }
        for (const double value : bone.inverse_bind.data) {
            hash_word(hash, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
        }
    }
    return hash == 0 ? 1 : hash;
}

template<class Key>
AnimationResult validate_keys(std::vector<Key>& keys, double duration, std::size_t& total,
                               const AnimationLimits& limits) {
    if (keys.size() > limits.max_keys - total) {
        return {AnimationError::Capacity, total};
    }
    total += keys.size();
    double previous = -1.0;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto& key = keys[index];
        if (!std::isfinite(key.time) || key.time < 0.0 || key.time > duration || key.time <= previous ||
            !math::finite(key.value)) {
            return {AnimationError::InvalidKey, index};
        }
        if constexpr (std::is_same_v<Key, RotationKey>) {
            if (!valid_rotation(key.value)) {
                return {AnimationError::InvalidKey, index};
            }
            key.value = math::normalized(key.value);
            if (index > 0 && math::dot(keys[index - 1].value, key.value) < 0.0) {
                key.value = -key.value;
            }
        }
        previous = key.time;
    }
    return {};
}

template<class Key, class Value>
Value sample_keys(const std::vector<Key>& keys, double time, Value fallback,
                  Interpolation interpolation) noexcept {
    if (keys.empty()) { return fallback; }
    if (time <= keys.front().time) { return keys.front().value; }
    if (time >= keys.back().time) { return keys.back().value; }
    const auto after = std::upper_bound(keys.begin(), keys.end(), time,
        [](double value, const Key& key) { return value < key.time; });
    const auto& before = *(after - 1);
    if (interpolation == Interpolation::Step) { return before.value; }
    const double alpha = (time - before.time) / (after->time - before.time);
    if constexpr (std::is_same_v<Value, math::Quat>) {
        return math::slerp(before.value, after->value, alpha);
    } else {
        return math::lerp(before.value, after->value, alpha);
    }
}

bool number(std::string_view text, double& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

bool unsigned_number(std::string_view text, std::uint32_t& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

core::CommandResult command_result(AnimationResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error))};
}

} // namespace

std::string_view describe(AnimationError error) noexcept {
    switch (error) {
    case AnimationError::None: return "ok";
    case AnimationError::InvalidLimits: return "invalid animation limits";
    case AnimationError::Capacity: return "animation capacity exceeded";
    case AnimationError::InvalidName: return "invalid animation name";
    case AnimationError::DuplicateName: return "duplicate bone name";
    case AnimationError::InvalidBone: return "invalid bone index";
    case AnimationError::Cycle: return "skeleton contains a parent cycle";
    case AnimationError::InvalidTransform: return "invalid bone transform";
    case AnimationError::InvalidInverseBind: return "invalid inverse bind matrix";
    case AnimationError::IncompatibleSkeleton: return "incompatible skeleton";
    case AnimationError::InvalidTime: return "invalid animation time";
    case AnimationError::InvalidKey: return "invalid or unordered animation key";
    case AnimationError::DuplicateChannel: return "duplicate bone channel";
    case AnimationError::InvalidEvent: return "invalid animation event";
    case AnimationError::InvalidHandle: return "stale or missing animation resource";
    case AnimationError::InvalidInfluence: return "invalid skin influence";
    case AnimationError::InvalidMesh: return "invalid skin source mesh";
    case AnimationError::SingularTransform: return "singular skin transform";
    case AnimationError::NonFiniteResult: return "nonfinite animation result";
    case AnimationError::InvalidEntity: return "invalid animation entity binding";
    }
    return "unknown animation error";
}

AnimationResult Skeleton::load(std::vector<Bone> bones, bool calculate_inverse_binds,
                               AnimationLimits limits) {
    if (!valid_limits(limits)) { return {AnimationError::InvalidLimits}; }
    if (bones.empty() || bones.size() > limits.max_bones) { return {AnimationError::Capacity}; }
    std::unordered_map<std::string, BoneIndex> names;
    std::vector<std::vector<BoneIndex>> descendants(bones.size());
    std::vector<BoneIndex> order;
    order.reserve(bones.size());
    names.reserve(bones.size());
    for (std::size_t index = 0; index < bones.size(); ++index) {
        auto& bone = bones[index];
        if (!valid_name(bone.name, limits.max_name_bytes)) { return {AnimationError::InvalidName, index}; }
        if (!names.emplace(bone.name, static_cast<BoneIndex>(index)).second) {
            return {AnimationError::DuplicateName, index};
        }
        if (!valid_transform(bone.bind_local)) { return {AnimationError::InvalidTransform, index}; }
        bone.bind_local.rotation = math::normalized(bone.bind_local.rotation);
        if (bone.parent == no_bone) {
            order.push_back(static_cast<BoneIndex>(index));
        } else {
            if (bone.parent >= bones.size() || bone.parent == index) {
                return {AnimationError::InvalidBone, index};
            }
            descendants[bone.parent].push_back(static_cast<BoneIndex>(index));
        }
    }
    for (std::size_t cursor = 0; cursor < order.size(); ++cursor) {
        const auto& children = descendants[order[cursor]];
        // Each bone has one parent, so each reachable child is appended once.
        for (const auto child : children) {
            order.push_back(child);
        }
    }
    if (order.size() != bones.size()) { return {AnimationError::Cycle}; }
    std::vector<math::Mat4> globals(bones.size());
    for (const auto index : order) {
        auto& bone = bones[index];
        const auto local = math::matrix(bone.bind_local);
        globals[index] = bone.parent == no_bone ? local : globals[bone.parent] * local;
        const auto inverse = math::inverse(globals[index]);
        if (!affine(globals[index]) || !inverse) { return {AnimationError::InvalidTransform, index}; }
        if (calculate_inverse_binds) {
            bone.inverse_bind = *inverse;
        } else if (!affine(bone.inverse_bind) ||
                   !math::almost_equal(globals[index] * bone.inverse_bind, math::Mat4{}, 1.0e-6)) {
            return {AnimationError::InvalidInverseBind, index};
        }
    }
    const auto signature = skeleton_hash(bones);
    bones_.swap(bones);
    order_.swap(order);
    bind_globals_.swap(globals);
    names_.swap(names);
    signature_ = signature;
    return {};
}

bool Skeleton::valid() const noexcept { return signature_ != 0 && !bones_.empty(); }
std::size_t Skeleton::size() const noexcept { return bones_.size(); }
std::span<const Bone> Skeleton::bones() const noexcept { return bones_; }
std::span<const BoneIndex> Skeleton::evaluation_order() const noexcept { return order_; }
std::span<const math::Mat4> Skeleton::bind_globals() const noexcept { return bind_globals_; }
std::uint64_t Skeleton::signature() const noexcept { return signature_; }

BoneIndex Skeleton::find(std::string_view name) const {
    if (!valid_name(name)) { return no_bone; }
    const auto found = names_.find(std::string(name));
    return found == names_.end() ? no_bone : found->second;
}

std::vector<BoneIndex> Skeleton::children(BoneIndex parent) const {
    std::vector<BoneIndex> result;
    for (std::size_t index = 0; index < bones_.size(); ++index) {
        if (bones_[index].parent == parent) { result.push_back(static_cast<BoneIndex>(index)); }
    }
    return result;
}

bool Pose::compatible(const Skeleton& skeleton) const noexcept {
    return skeleton.valid() && skeleton_signature == skeleton.signature() && local.size() == skeleton.size();
}

AnimationResult Pose::reset(const Skeleton& skeleton) {
    if (!skeleton.valid()) { return {AnimationError::IncompatibleSkeleton}; }
    Pose candidate;
    candidate.local.reserve(skeleton.size());
    for (const auto& bone : skeleton.bones()) { candidate.local.push_back(bone.bind_local); }
    candidate.skeleton_signature = skeleton.signature();
    const auto result = candidate.update(skeleton);
    if (result) { *this = std::move(candidate); }
    return result;
}

AnimationResult Pose::update(const Skeleton& skeleton, const math::Mat4& root) {
    if (!compatible(skeleton)) { return {AnimationError::IncompatibleSkeleton}; }
    if (!affine(root)) { return {AnimationError::InvalidTransform}; }
    std::vector<math::Mat4> next_global(local.size());
    std::vector<math::Mat4> next_skin(local.size());
    for (const auto index : skeleton.evaluation_order()) {
        if (!valid_transform(local[index])) { return {AnimationError::InvalidTransform, index}; }
        const auto& bone = skeleton.bones()[index];
        const auto matrix = math::matrix(local[index]);
        next_global[index] = (bone.parent == no_bone ? root : next_global[bone.parent]) * matrix;
        next_skin[index] = next_global[index] * bone.inverse_bind;
        if (!affine(next_global[index]) || !affine(next_skin[index])) {
            return {AnimationError::NonFiniteResult, index};
        }
    }
    global.swap(next_global);
    skin.swap(next_skin);
    return {};
}

std::optional<SampleTime> wrap_time(double time, double duration, WrapMode mode) noexcept {
    if (!std::isfinite(time) || std::abs(time) > maximum_time || !std::isfinite(duration) ||
        duration < 0.0 || duration > hard_limits.max_duration ||
        (duration > 0.0 && duration < minimum_duration)) { return std::nullopt; }
    if (mode != WrapMode::Clamp && mode != WrapMode::Loop && mode != WrapMode::PingPong) {
        return std::nullopt;
    }
    if (duration == 0.0) { return SampleTime{0.0, 0, false, true}; }
    if (mode == WrapMode::Clamp) {
        return SampleTime{math::clamp(time, 0.0, duration), 0, false, time <= 0.0 || time >= duration};
    }
    if (std::abs(time) / duration > 281474976710656.0) { return std::nullopt; }
    const long double period = static_cast<long double>(duration) * (mode == WrapMode::PingPong ? 2.0L : 1.0L);
    const long double cycle = std::floor(static_cast<long double>(time) / period);
    long double remainder = std::fmod(static_cast<long double>(time), period);
    if (remainder < 0.0L) { remainder += period; }
    const bool reverse = mode == WrapMode::PingPong && remainder > duration;
    const double sampled = static_cast<double>(reverse ? period - remainder : remainder);
    return SampleTime{math::clamp(sampled, 0.0, duration), static_cast<std::int64_t>(cycle), reverse, false};
}

AnimationResult AnimationClip::load(std::string name, double duration, const Skeleton& skeleton,
                                    std::vector<BoneChannel> channels, std::vector<ClipEvent> events,
                                    AnimationLimits limits) {
    if (!valid_limits(limits)) { return {AnimationError::InvalidLimits}; }
    if (!skeleton.valid() || skeleton.size() > limits.max_bones) { return {AnimationError::IncompatibleSkeleton}; }
    if (!valid_name(name, limits.max_name_bytes)) { return {AnimationError::InvalidName}; }
    if (!std::isfinite(duration) || duration < 0.0 || duration > limits.max_duration ||
        (duration > 0.0 && duration < minimum_duration)) { return {AnimationError::InvalidTime}; }
    if (channels.size() > limits.max_channels || events.size() > limits.max_events) {
        return {AnimationError::Capacity};
    }
    std::vector<bool> occupied(skeleton.size());
    std::size_t total_keys{};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        auto& channel = channels[index];
        if (channel.bone >= skeleton.size()) { return {AnimationError::InvalidBone, index}; }
        if (occupied[channel.bone]) { return {AnimationError::DuplicateChannel, index}; }
        occupied[channel.bone] = true;
        if (channel.interpolation != Interpolation::Step && channel.interpolation != Interpolation::Linear) {
            return {AnimationError::InvalidKey, index};
        }
        for (const auto result : {validate_keys(channel.translations, duration, total_keys, limits),
                                  validate_keys(channel.rotations, duration, total_keys, limits),
                                  validate_keys(channel.scales, duration, total_keys, limits)}) {
            if (!result) { return {result.error, index}; }
        }
    }
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (!std::isfinite(event.time) || event.time < 0.0 || event.time > duration ||
            !valid_name(event.name, limits.max_name_bytes) || event.payload.size() > limits.max_name_bytes) {
            return {AnimationError::InvalidEvent, index};
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const ClipEvent& first, const ClipEvent& second) {
        return first.time < second.time;
    });
    name_.swap(name);
    channels_.swap(channels);
    events_.swap(events);
    duration_ = duration;
    skeleton_signature_ = skeleton.signature();
    return {};
}

bool AnimationClip::valid() const noexcept { return skeleton_signature_ != 0 && !name_.empty(); }
bool AnimationClip::compatible(const Skeleton& skeleton) const noexcept {
    return valid() && skeleton.valid() && skeleton_signature_ == skeleton.signature();
}
std::string_view AnimationClip::name() const noexcept { return name_; }
double AnimationClip::duration() const noexcept { return duration_; }
std::uint64_t AnimationClip::skeleton_signature() const noexcept { return skeleton_signature_; }
std::span<const BoneChannel> AnimationClip::channels() const noexcept { return channels_; }
std::span<const ClipEvent> AnimationClip::events() const noexcept { return events_; }

AnimationResult AnimationClip::sample(const Skeleton& skeleton, double time, Pose& output, WrapMode mode) const {
    if (!compatible(skeleton)) { return {AnimationError::IncompatibleSkeleton}; }
    const auto mapped = wrap_time(time, duration_, mode);
    if (!mapped) { return {AnimationError::InvalidTime}; }
    Pose candidate;
    candidate.skeleton_signature = skeleton.signature();
    candidate.local.reserve(skeleton.size());
    for (const auto& bone : skeleton.bones()) { candidate.local.push_back(bone.bind_local); }
    for (const auto& channel : channels_) {
        auto& local = candidate.local[channel.bone];
        local.position = sample_keys(channel.translations, mapped->time, local.position, channel.interpolation);
        local.rotation = sample_keys(channel.rotations, mapped->time, local.rotation, channel.interpolation);
        local.scale = sample_keys(channel.scales, mapped->time, local.scale, channel.interpolation);
    }
    const auto result = candidate.update(skeleton);
    if (result) { output = std::move(candidate); }
    return result;
}

EventTraversal AnimationClip::traverse(double from, double to, WrapMode mode, std::size_t limit) const {
    EventTraversal output;
    if (!valid() || !wrap_time(from, duration_, mode) || !wrap_time(to, duration_, mode)) {
        output.result = {AnimationError::InvalidTime};
        return output;
    }
    if (limit > hard_limits.max_events) { output.result = {AnimationError::Capacity}; return output; }
    if (from == to || events_.empty() || duration_ == 0.0) { return output; }
    struct Candidate {
        long double time{};
        long double base{};
        long double period{};
        std::int64_t cycle{};
        std::size_t event{};
        bool reverse{};
    };
    const bool forward = to > from;
    const auto compare = [forward](const Candidate& first, const Candidate& second) {
        if (first.time == second.time) { return first.event > second.event; }
        return forward ? first.time > second.time : first.time < second.time;
    };
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(compare)> pending(compare);
    const auto in_interval = [from, to, forward](long double time) {
        return forward ? time > from && time <= to : time < from && time >= to;
    };
    const auto add_sequence = [&](std::size_t index, long double base, long double period, bool reverse) {
        long double cycle = 0.0L;
        if (period > 0.0L) {
            const long double quotient = (static_cast<long double>(from) - base) / period;
            cycle = forward ? std::floor(quotient) + 1.0L : std::ceil(quotient) - 1.0L;
        }
        const long double time = base + cycle * period;
        if (in_interval(time)) {
            pending.push({time, base, period, static_cast<std::int64_t>(cycle), index, reverse});
        }
    };
    for (std::size_t index = 0; index < events_.size(); ++index) {
        const double event_time = events_[index].time;
        if (mode == WrapMode::Clamp) {
            add_sequence(index, event_time, 0.0L, !forward);
        } else if (mode == WrapMode::Loop) {
            add_sequence(index, event_time, duration_, !forward);
        } else {
            const long double period = static_cast<long double>(duration_) * 2.0L;
            const bool endpoint = event_time == 0.0 || event_time == duration_;
            const bool arriving_reverse = event_time == 0.0;
            add_sequence(index, event_time, period, endpoint ? arriving_reverse : !forward);
            if (!endpoint) { add_sequence(index, period - event_time, period, forward); }
        }
    }
    output.events.reserve(std::min(limit, pending.size()));
    while (!pending.empty() && output.events.size() < limit) {
        auto next = pending.top();
        pending.pop();
        output.events.push_back({next.event, events_[next.event].time, static_cast<double>(next.time),
                                 next.cycle, next.reverse});
        if (next.period > 0.0L) {
            next.cycle += forward ? 1 : -1;
            next.time = next.base + static_cast<long double>(next.cycle) * next.period;
            if (in_interval(next.time)) { pending.push(next); }
        }
    }
    output.truncated = !pending.empty();
    return output;
}

template<class Resource, class Handle>
AnimationLibrary<Resource, Handle>::AnimationLibrary(std::uint32_t capacity) : generations_(capacity) {
    if (capacity > 8192) { throw std::invalid_argument("animation library capacity exceeds 8192"); }
    records_.reserve(capacity);
}

template<class Resource, class Handle>
Handle AnimationLibrary<Resource, Handle>::create(Resource resource, std::string_view resource_name) {
    if (!resource.valid() || !valid_name(resource_name) || find(resource_name)) { return {}; }
    auto record = std::make_unique<Record>(Record{{}, std::string(resource_name), std::move(resource)});
    const auto identity = generations_.acquire();
    if (!identity) { return {}; }
    record->handle = {identity->index, identity->generation};
    const auto handle = record->handle;
    if (records_.size() < handle.index) { records_.resize(handle.index); }
    records_[static_cast<std::size_t>(handle.index - 1)] = std::move(record);
    return handle;
}

template<class Resource, class Handle>
bool AnimationLibrary<Resource, Handle>::replace(Handle handle, Resource resource) {
    if (!get(handle) || !resource.valid()) { return false; }
    auto& record = records_[static_cast<std::size_t>(handle.index - 1)];
    auto replacement = std::make_unique<Record>(Record{handle, record->name, std::move(resource)});
    record.swap(replacement);
    return true;
}

template<class Resource, class Handle>
bool AnimationLibrary<Resource, Handle>::destroy(Handle handle) noexcept {
    if (!generations_.release({handle.index, handle.generation})) { return false; }
    records_[static_cast<std::size_t>(handle.index - 1)].reset();
    return true;
}

template<class Resource, class Handle>
void AnimationLibrary<Resource, Handle>::clear() noexcept {
    for (auto& record : records_) {
        if (record) {
            (void)generations_.release({record->handle.index, record->handle.generation});
            record.reset();
        }
    }
}

template<class Resource, class Handle>
const Resource* AnimationLibrary<Resource, Handle>::get(Handle handle) const noexcept {
    if (!generations_.alive({handle.index, handle.generation})) { return nullptr; }
    return &records_[static_cast<std::size_t>(handle.index - 1)]->resource;
}

template<class Resource, class Handle>
Handle AnimationLibrary<Resource, Handle>::find(std::string_view resource_name) const noexcept {
    for (const auto& record : records_) {
        if (record && record->name == resource_name) { return record->handle; }
    }
    return {};
}

template<class Resource, class Handle>
std::vector<Handle> AnimationLibrary<Resource, Handle>::handles() const {
    std::vector<Handle> result;
    result.reserve(size());
    for (const auto& record : records_) {
        if (record) { result.push_back(record->handle); }
    }
    return result;
}

template<class Resource, class Handle>
std::string_view AnimationLibrary<Resource, Handle>::name(Handle handle) const noexcept {
    return get(handle) ? std::string_view(records_[static_cast<std::size_t>(handle.index - 1)]->name) : std::string_view{};
}

template<class Resource, class Handle>
std::size_t AnimationLibrary<Resource, Handle>::size() const noexcept { return generations_.size(); }

template class AnimationLibrary<Skeleton, SkeletonHandle>;
template class AnimationLibrary<AnimationClip, ClipHandle>;

AnimationResult Animator::play(const Skeleton& skeleton, const ClipLibrary& clips, ClipHandle handle,
                               double fade_seconds, WrapMode wrap) {
    const auto* clip = clips.get(handle);
    if (!clip) { return {AnimationError::InvalidHandle}; }
    if (!std::isfinite(fade_seconds) || fade_seconds < 0.0 || fade_seconds > hard_limits.max_duration) {
        return {AnimationError::InvalidTime};
    }
    Pose target;
    const double start = state_.speed < 0.0 && wrap == WrapMode::Clamp ? clip->duration() : 0.0;
    const auto result = clip->sample(skeleton, start, target, wrap);
    if (!result) { return result; }
    std::optional<Pose> fade;
    if (fade_seconds > 0.0 && pose_.compatible(skeleton)) {
        fade = pose_;
        const auto source_result = fade->update(skeleton);
        if (!source_result) { return source_result; }
    }
    state_.clip = handle;
    state_.time = start;
    state_.wrap = wrap;
    state_.playing = true;
    fade_source_ = std::move(fade);
    fade_duration_ = fade_source_ ? fade_seconds : 0.0;
    fade_elapsed_ = 0.0;
    if (!fade_source_) { pose_ = std::move(target); }
    return {};
}

AnimationResult Animator::seek(const Skeleton& skeleton, const ClipLibrary& clips, double time) {
    const auto* clip = clips.get(state_.clip);
    if (!clip) { return {AnimationError::InvalidHandle}; }
    Pose target;
    const auto result = clip->sample(skeleton, time, target, state_.wrap);
    if (!result) { return result; }
    state_.time = state_.wrap == WrapMode::Clamp ? math::clamp(time, 0.0, clip->duration()) : time;
    pose_ = std::move(target);
    fade_source_.reset();
    return {};
}

PlaybackUpdate Animator::advance(const Skeleton& skeleton, const ClipLibrary& clips,
                                  double delta_seconds, std::size_t event_limit) {
    PlaybackUpdate output;
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0 || delta_seconds > hard_limits.max_duration) {
        output.result = {AnimationError::InvalidTime};
        return output;
    }
    if (event_limit > hard_limits.max_events) { output.result = {AnimationError::Capacity}; return output; }
    if (!pose_.compatible(skeleton)) { output.result = {AnimationError::IncompatibleSkeleton}; return output; }
    if (!state_.playing) { return output; }
    const auto* clip = clips.get(state_.clip);
    if (!clip) { output.result = {AnimationError::InvalidHandle}; return output; }
    double next_time = state_.time + delta_seconds * state_.speed;
    if (state_.wrap == WrapMode::Clamp) { next_time = math::clamp(next_time, 0.0, clip->duration()); }
    Pose target;
    output.result = clip->sample(skeleton, next_time, target, state_.wrap);
    if (!output.result) { return output; }
    const double next_fade = std::min(fade_duration_, fade_elapsed_ + delta_seconds);
    if (fade_source_) {
        if (!fade_source_->compatible(skeleton)) {
            output.result = {AnimationError::IncompatibleSkeleton};
            return output;
        }
        const double weight = fade_duration_ > 0.0 ? next_fade / fade_duration_ : 1.0;
        for (std::size_t index = 0; index < target.local.size(); ++index) {
            target.local[index] = math::interpolate(fade_source_->local[index], target.local[index], weight);
        }
        output.result = target.update(skeleton);
        if (!output.result) { return output; }
    }
    auto traversal = clip->traverse(state_.time, next_time, state_.wrap, event_limit);
    if (!traversal.result) { output.result = traversal.result; return output; }
    output.events = std::move(traversal.events);
    output.events_truncated = traversal.truncated;
    output.finished = clip->duration() == 0.0 || (state_.wrap == WrapMode::Clamp &&
        ((state_.speed > 0.0 && next_time >= clip->duration()) || (state_.speed < 0.0 && next_time <= 0.0)));
    state_.time = next_time;
    fade_elapsed_ = next_fade;
    pose_ = std::move(target);
    if (fade_source_ && next_fade >= fade_duration_) { fade_source_.reset(); }
    if (output.finished && !fade_source_) { state_.playing = false; }
    return output;
}

bool Animator::set_speed(double speed) noexcept {
    if (!std::isfinite(speed) || std::abs(speed) > 1024.0) { return false; }
    state_.speed = speed;
    return true;
}
void Animator::pause() noexcept { state_.playing = false; }
void Animator::resume() noexcept { state_.playing = static_cast<bool>(state_.clip); }
void Animator::stop() noexcept { state_.playing = false; state_.time = 0.0; fade_source_.reset(); }
const PlaybackState& Animator::state() const noexcept { return state_; }
Pose& Animator::pose() noexcept { return pose_; }
const Pose& Animator::pose() const noexcept { return pose_; }
bool Animator::fading() const noexcept { return fade_source_.has_value(); }

AnimationResult normalize_influences(VertexInfluences& influences, std::size_t bone_count) noexcept {
    if (bone_count == 0 || bone_count > hard_limits.max_bones || influences.count > max_skin_influences) {
        return {AnimationError::InvalidInfluence};
    }
    VertexInfluences candidate;
    double maximum{};
    for (std::size_t index = 0; index < influences.count; ++index) {
        const auto& influence = influences.values[index];
        if (!std::isfinite(influence.weight) || influence.weight < 0.0 ||
            (influence.weight > 0.0 && influence.bone >= bone_count)) {
            return {AnimationError::InvalidInfluence, index};
        }
        maximum = std::max(maximum, influence.weight);
    }
    if (maximum == 0.0) { influences = candidate; return {}; }
    double total{};
    for (std::size_t index = 0; index < influences.count; ++index) {
        const auto& influence = influences.values[index];
        if (influence.weight == 0.0) { continue; }
        const double weight = influence.weight / maximum;
        total += weight;
        std::size_t destination{};
        while (destination < candidate.count && candidate.values[destination].bone != influence.bone) { ++destination; }
        if (destination == candidate.count) {
            candidate.values[destination].bone = influence.bone;
            ++candidate.count;
        }
        candidate.values[destination].weight += weight;
    }
    for (std::size_t index = 0; index < candidate.count; ++index) { candidate.values[index].weight /= total; }
    influences = candidate;
    return {};
}

AnimationResult skin_vertices(std::span<const geometry::Vertex> source,
                              std::span<const VertexInfluences> influences,
                              const Pose& pose, std::vector<geometry::Vertex>& output) {
    if (source.size() > geometry::MeshLimits{}.max_vertices || source.size() != influences.size()) {
        return {AnimationError::Capacity};
    }
    if (pose.skin.empty() || pose.skin.size() > hard_limits.max_bones || pose.skeleton_signature == 0) {
        return {AnimationError::IncompatibleSkeleton};
    }
    std::vector<geometry::Vertex> candidate;
    candidate.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        auto vertex = source[index];
        if (!math::finite(vertex.position) || !math::finite(vertex.normal) || !math::finite(vertex.tangent) ||
            !math::finite(vertex.color) || !math::finite(vertex.uv)) { return {AnimationError::InvalidMesh, index}; }
        auto weights = influences[index];
        const auto result = normalize_influences(weights, pose.skin.size());
        if (!result) { return {result.error, index}; }
        if (weights.count == 0) { candidate.push_back(vertex); continue; }
        auto blended = math::Mat4::zero();
        for (std::size_t influence = 0; influence < weights.count; ++influence) {
            const auto& entry = weights.values[influence];
            if (!affine(pose.skin[entry.bone])) { return {AnimationError::InvalidTransform, entry.bone}; }
            blended = blended + pose.skin[entry.bone] * entry.weight;
        }
        const auto normal_matrix = math::normal_matrix(blended);
        if (!normal_matrix) { return {AnimationError::SingularTransform, index}; }
        vertex.position = math::transform_point(blended, vertex.position);
        auto normal = *normal_matrix * vertex.normal;
        auto tangent = math::transform_vector(blended, math::xyz(vertex.tangent));
        if (!math::finite(vertex.position) || !math::finite(normal) || !math::finite(tangent)) {
            return {AnimationError::NonFiniteResult, index};
        }
        normal = math::normalized(normal, {0.0, 1.0, 0.0});
        tangent = math::normalized(tangent);
        tangent = math::normalized(tangent - normal * math::dot(normal, tangent), math::orthonormal_basis(normal).first);
        const double orientation = math::determinant(math::upper3x3(blended));
        if (!std::isfinite(orientation)) { return {AnimationError::NonFiniteResult, index}; }
        vertex.normal = normal;
        vertex.tangent = math::homogeneous(tangent, (vertex.tangent.w < 0.0 ? -1.0 : 1.0) * (orientation < 0.0 ? -1.0 : 1.0));
        candidate.push_back(vertex);
    }
    output.swap(candidate);
    return {};
}

AnimationResult skin_mesh(const geometry::MeshData& source, std::span<const VertexInfluences> influences,
                          const Pose& pose, geometry::MeshData& output) {
    if (!geometry::validate_mesh(source)) { return {AnimationError::InvalidMesh}; }
    geometry::MeshData candidate;
    const auto result = skin_vertices(source.vertices, influences, pose, candidate.vertices);
    if (!result) { return result; }
    candidate.indices = source.indices;
    candidate.submeshes = source.submeshes;
    output = std::move(candidate);
    return {};
}

bool AnimationService::attach(ecs::Registry& registry, ecs::Entity entity, SkeletonHandle handle) {
    const auto* skeleton = skeletons.get(handle);
    if (!skeleton || !registry.alive(entity) || registry.iterating()) { return false; }
    AnimatedComponent candidate;
    candidate.skeleton = handle;
    if (!candidate.animator.pose().reset(*skeleton)) { return false; }
    registry.emplace_or_replace<AnimatedComponent>(entity, std::move(candidate));
    return true;
}

AnimationResult AnimationService::play(ecs::Registry& registry, ecs::Entity entity, ClipHandle clip,
                                       double fade_seconds, WrapMode wrap) {
    auto* component = registry.get<AnimatedComponent>(entity);
    if (!component) { return {AnimationError::InvalidEntity}; }
    const auto* skeleton = skeletons.get(component->skeleton);
    if (!skeleton) { return {AnimationError::InvalidHandle}; }
    const auto result = component->animator.play(*skeleton, clips, clip, fade_seconds, wrap);
    if (result) { (void)registry.mark_changed<AnimatedComponent>(entity); }
    return result;
}

AnimationUpdate AnimationService::update(ecs::Registry& registry, scene::Scene* scene_graph,
                                         double delta_seconds, std::size_t event_limit) {
    AnimationUpdate output;
    if (event_limit > hard_limits.max_events || !std::isfinite(delta_seconds) || delta_seconds < 0.0 ||
        delta_seconds > hard_limits.max_duration || (scene_graph && &scene_graph->registry() != &registry)) {
        output.errors = 1;
        return output;
    }
    for (const auto entity : registry.view<AnimatedComponent>()) {
        auto& component = registry.require<AnimatedComponent>(entity);
        if (!component.enabled || (scene_graph && scene_graph->contains(entity) && !scene_graph->effective_enabled(entity))) {
            ++output.skipped;
            continue;
        }
        const auto* skeleton = skeletons.get(component.skeleton);
        if (!skeleton) { ++output.errors; continue; }
        const auto update = component.animator.advance(*skeleton, clips, delta_seconds,
            std::min<std::size_t>(256, event_limit - output.events.size()));
        if (!update.result) { ++output.errors; continue; }
        if (update.events_truncated) { ++output.truncated_entities; }
        const auto& state = component.animator.state();
        if (const auto* clip = clips.get(state.clip)) {
            for (const auto& occurrence : update.events) {
                const auto& event = clip->events()[occurrence.event_index];
                output.events.push_back({entity, state.clip, event.name, event.payload, occurrence.clip_time, occurrence.reverse});
            }
        }
        if (!component.bone_entities.empty()) {
            bool valid_binding = scene_graph && component.bone_entities.size() == skeleton->size() &&
                component.animator.pose().compatible(*skeleton);
            std::vector<scene::LocalTransformEdit> edits;
            if (valid_binding) {
                auto unique = component.bone_entities;
                std::sort(unique.begin(), unique.end());
                valid_binding = std::adjacent_find(unique.begin(), unique.end()) == unique.end();
                edits.reserve(skeleton->size());
            }
            for (std::size_t index = 0; valid_binding && index < skeleton->size(); ++index) {
                const auto bone_entity = component.bone_entities[index];
                const auto parent = skeleton->bones()[index].parent;
                valid_binding = scene_graph->contains(bone_entity) &&
                    !registry.has<physics::BodyComponent>(bone_entity) && (parent == no_bone ||
                    scene_graph->parent(bone_entity) == component.bone_entities[parent]);
                if (valid_binding) { edits.push_back({bone_entity, component.animator.pose().local[index]}); }
            }
            if (!valid_binding || !scene_graph->apply_local_transforms(edits)) { ++output.errors; }
        }
        (void)registry.mark_changed<AnimatedComponent>(entity);
        ++output.animated;
    }
    return output;
}

void register_skeleton(core::Kernel& kernel) {
    if (kernel.services().find<AnimationService>()) { return; }
    auto& service = kernel.services().emplace<AnimationService>();
    kernel.register_module({"animation.skeleton", "Skeletal poses, keyframes, playback and CPU skinning", 1});
    kernel.add_system({"animation.skeleton", core::Phase::Animation, 0, {}},
        [](core::Kernel& active, const core::Frame& frame) {
            auto* registry = active.services().find<ecs::Registry>();
            if (!registry) { return; }
            auto& animation = active.services().get<AnimationService>();
            auto update = animation.update(*registry, active.services().find<scene::Scene>(), frame.delta_seconds);
            for (auto& event : update.events) { (void)active.events().publish<AnimationEvent>(std::move(event)); }
            active.metric("animation.animated", static_cast<double>(update.animated));
            active.metric("animation.errors", static_cast<double>(update.errors));
            active.metric("animation.truncated_entities", static_cast<double>(update.truncated_entities));
        });
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        registry->register_component<AnimatedComponent>("animation.animated");
    }
    kernel.add_command("animation.skeleton", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) { return core::CommandResult{false, "usage: animation.skeleton name [bones]"}; }
        std::uint32_t count = 1;
        if ((args.size() == 2 && !unsigned_number(args[1], count)) || count == 0 || count > 4096) {
            return core::CommandResult{false, "bone count must be 1..4096"};
        }
        std::vector<Bone> bones(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            bones[index].name = "bone" + std::to_string(index);
            bones[index].parent = index == 0 ? no_bone : index - 1;
            bones[index].bind_local.position.y = index == 0 ? 0.0 : 0.5;
        }
        Skeleton skeleton;
        const auto result = skeleton.load(std::move(bones));
        if (!result) { return command_result(result); }
        const auto handle = owner->skeletons.create(std::move(skeleton), args[0]);
        return core::CommandResult{static_cast<bool>(handle), handle ? "created skeleton" : "name or library capacity rejected"};
    }, "Create a named rest-pose bone chain");
    kernel.add_command("animation.clip", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 8) { return core::CommandResult{false, "usage: animation.clip name skeleton duration bone dx dy dz wrap_return"}; }
        const auto* skeleton = owner->skeletons.get(owner->skeletons.find(args[1]));
        double duration{}, x{}, y{}, z{};
        std::uint32_t bone{}, return_to_start{};
        if (!skeleton || !number(args[2], duration) || !unsigned_number(args[3], bone) ||
            !number(args[4], x) || !number(args[5], y) || !number(args[6], z) ||
            !unsigned_number(args[7], return_to_start) || return_to_start > 1 || bone >= skeleton->size() ||
            duration < minimum_duration) { return core::CommandResult{false, "invalid clip arguments"}; }
        BoneChannel channel;
        channel.bone = bone;
        const auto start = skeleton->bones()[bone].bind_local.position;
        channel.translations = {{0.0, start}, {return_to_start != 0 ? duration * 0.5 : duration, start + math::Vec3{x, y, z}}};
        if (return_to_start != 0) { channel.translations.push_back({duration, start}); }
        AnimationClip clip;
        const auto result = clip.load(args[0], duration, *skeleton, {std::move(channel)}, {{duration, "complete", {}}});
        if (!result) { return command_result(result); }
        const auto handle = owner->clips.create(std::move(clip), args[0]);
        return core::CommandResult{static_cast<bool>(handle), handle ? "created clip" : "name or library capacity rejected"};
    }, "Create a translation clip; wrap_return 1 returns to its starting pose");
    kernel.add_command("animation.attach", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 2) { return core::CommandResult{false, "usage: animation.attach entity_name skeleton"}; }
        const bool success = owner->attach(*registry, registry->find_name(args[0]), owner->skeletons.find(args[1]));
        return core::CommandResult{success, success ? "attached skeleton" : "entity or skeleton unavailable"};
    }, "Attach a rest pose to an existing named ECS entity");
    kernel.add_command("animation.play", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() < 2 || args.size() > 4) {
            return core::CommandResult{false, "usage: animation.play entity_name clip [fade_seconds] [clamp|loop|pingpong]"};
        }
        double fade{};
        if (args.size() >= 3 && !number(args[2], fade)) { return core::CommandResult{false, "invalid fade duration"}; }
        WrapMode wrap = WrapMode::Loop;
        if (args.size() == 4) {
            if (args[3] == "clamp") { wrap = WrapMode::Clamp; }
            else if (args[3] == "pingpong") { wrap = WrapMode::PingPong; }
            else if (args[3] != "loop") { return core::CommandResult{false, "unknown wrap mode"}; }
        }
        return command_result(owner->play(*registry, registry->find_name(args[0]), owner->clips.find(args[1]), fade, wrap));
    }, "Play or crossfade a clip on an attached entity");
    kernel.add_command("animation.sample", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 3) { return core::CommandResult{false, "usage: animation.sample skeleton clip seconds"}; }
        const auto* skeleton = owner->skeletons.get(owner->skeletons.find(args[0]));
        const auto* clip = owner->clips.get(owner->clips.find(args[1]));
        double time{};
        if (!skeleton || !clip || !number(args[2], time)) { return core::CommandResult{false, "invalid sample arguments"}; }
        Pose pose;
        const auto result = clip->sample(*skeleton, time, pose);
        if (!result) { return command_result(result); }
        std::ostringstream message;
        message << "sampled " << pose.local.size() << " bones; first position " << pose.local.front().position.x << ' '
                << pose.local.front().position.y << ' ' << pose.local.front().position.z;
        return core::CommandResult{true, message.str()};
    }, "Sample a clip without changing playback");
    kernel.add_command("animation.list", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (!args.empty()) { return core::CommandResult{false, "usage: animation.list"}; }
        std::ostringstream message;
        for (const auto handle : owner->skeletons.handles()) {
            message << "skeleton " << owner->skeletons.name(handle) << " bones=" << owner->skeletons.get(handle)->size() << '\n';
        }
        for (const auto handle : owner->clips.handles()) {
            message << "clip " << owner->clips.name(handle) << " seconds=" << owner->clips.get(handle)->duration() << '\n';
        }
        return core::CommandResult{true, message.str()};
    }, "List authored animation resources");
}

} // namespace ge::animation
