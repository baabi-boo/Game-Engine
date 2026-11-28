#include <ge/animation/retarget.hpp>

#include <ge/animation/blend.hpp>
#include <ge/animation/ik.hpp>
#include <ge/core/runtime.hpp>
#include <ge/physics/character.hpp>
#include <ge/scene/partition.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace ge::animation {
namespace {

constexpr RetargetLimits hard_limits{};
constexpr auto exhausted = std::numeric_limits<std::uint64_t>::max();
constexpr auto absent = std::numeric_limits<std::size_t>::max();

bool bounded(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool valid_limits(const RetargetLimits& value) noexcept {
    return value.max_bones > 0 && value.max_bones <= hard_limits.max_bones &&
        value.max_links > 0 && value.max_links <= hard_limits.max_links &&
        value.max_resources > 0 && value.max_resources <= hard_limits.max_resources &&
        value.max_bindings > 0 && value.max_bindings <= hard_limits.max_bindings &&
        value.max_scene_nodes > 0 && value.max_scene_nodes <= hard_limits.max_scene_nodes &&
        value.max_frame_bones > 0 && value.max_frame_bones <= hard_limits.max_frame_bones &&
        value.max_clip_segments > 0 && value.max_clip_segments <= hard_limits.max_clip_segments &&
        bounded(value.coordinate_limit, 1.0, hard_limits.coordinate_limit);
}

bool valid_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 &&
        std::none_of(value.begin(), value.end(), [](char letter) {
            const auto byte = static_cast<unsigned char>(letter);
            return byte < 32U || byte == 127U;
        });
}

bool supported(const math::Transform& value, double coordinate_limit) noexcept {
    const double norm = math::dot(value.rotation, value.rotation);
    const auto position = math::abs(value.position);
    return math::finite(value) && bounded(norm, 1.0e-12, 1.0e12) &&
        position.x <= coordinate_limit && position.y <= coordinate_limit && position.z <= coordinate_limit &&
        bounded(value.scale.x, 1.0e-6, 1.0e6) && value.scale.y > 0.0 && value.scale.z > 0.0 &&
        std::abs(value.scale.x - value.scale.y) <= 1.0e-7 * value.scale.x &&
        std::abs(value.scale.x - value.scale.z) <= 1.0e-7 * value.scale.x;
}

RetargetResult check_skeleton(const Skeleton& skeleton, const RetargetLimits& limits) {
    if (!skeleton.valid()) { return {RetargetError::InvalidSkeleton}; }
    if (skeleton.size() > limits.max_bones) { return {RetargetError::Capacity}; }
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        const auto global = math::decompose(skeleton.bind_globals()[index]);
        if (!supported(skeleton.bones()[index].bind_local, limits.coordinate_limit) ||
            !global || !supported(*global, limits.coordinate_limit)) {
            return {RetargetError::UnsupportedTransform, index};
        }
    }
    return {};
}

RetargetResult check_pose(const Skeleton& skeleton, const Pose& pose, const RetargetLimits& limits) {
    if (!pose.compatible(skeleton)) { return {RetargetError::InvalidPose}; }
    if (skeleton.size() > limits.max_bones || pose.global.size() > limits.max_bones ||
        pose.skin.size() > limits.max_bones) { return {RetargetError::Capacity}; }
    for (std::size_t index = 0; index < pose.local.size(); ++index) {
        if (!supported(pose.local[index], limits.coordinate_limit)) {
            return {RetargetError::UnsupportedTransform, index};
        }
    }
    return {};
}

math::Quat global_rotation(const Skeleton& skeleton, BoneIndex bone) {
    if (bone == no_bone) { return {}; }
    return math::decompose(skeleton.bind_globals()[bone])->rotation;
}

bool valid_settings(const RootMotionSettings& value) noexcept {
    return (value.horizontal || value.vertical || value.yaw) &&
        bounded(value.maximum_translation, 0.0, 1.0e6) &&
        bounded(value.maximum_yaw, 0.0, math::pi) &&
        bounded(value.maximum_seconds, 1.0e-6, 3600.0);
}

RetargetResult root_value(const Skeleton& skeleton, const Pose& pose,
                          const RootMotionSettings& settings, RootMotionDelta& value) {
    if (settings.bone >= skeleton.size() || skeleton.bones()[settings.bone].parent != no_bone) {
        return {RetargetError::InvalidBone, settings.bone};
    }
    const auto checked = check_pose(skeleton, pose, hard_limits);
    if (!checked) { return checked; }
    const auto& bind = skeleton.bones()[settings.bone].bind_local;
    const auto& local = pose.local[settings.bone];
    value.yaw = 0.0;
    if (settings.yaw) {
        const auto rotation = math::normalized(local.rotation) * math::conjugate(math::normalized(bind.rotation));
        if (std::hypot(rotation.y, rotation.w) < 1.0e-8) { return {RetargetError::UnsupportedTransform}; }
        value.yaw = math::wrap_angle(2.0 * std::atan2(rotation.y, rotation.w));
    }
    value.translation = local.position - math::rotate(math::from_axis_angle({0.0, 1.0, 0.0}, value.yaw), bind.position);
    if (!settings.horizontal) { value.translation.x = 0.0; value.translation.z = 0.0; }
    if (!settings.vertical) { value.translation.y = 0.0; }
    return {};
}

RootMotionDelta difference(const RootMotionDelta& from, const RootMotionDelta& to, double seconds) {
    return {math::rotate(math::from_axis_angle({0.0, 1.0, 0.0}, -from.yaw), to.translation - from.translation),
            math::delta_angle(from.yaw, to.yaw), seconds};
}

RootMotionDelta concatenate(const RootMotionDelta& first, const RootMotionDelta& second) {
    return {first.translation + math::rotate(math::from_axis_angle({0.0, 1.0, 0.0}, first.yaw), second.translation),
            math::wrap_angle(first.yaw + second.yaw), first.seconds + second.seconds};
}

bool acceptable(const RootMotionDelta& delta, const RootMotionSettings& settings) noexcept {
    return math::finite(delta.translation) && std::isfinite(delta.yaw) &&
        math::length(delta.translation) <= settings.maximum_translation &&
        std::abs(delta.yaw) <= settings.maximum_yaw &&
        bounded(delta.seconds, 0.0, settings.maximum_seconds);
}

math::Transform moved(const math::Transform& frame, const RootMotionDelta& delta) {
    auto output = frame;
    output.position += math::rotate(frame.rotation, delta.translation * frame.scale.x);
    output.rotation = math::normalized(frame.rotation * math::from_axis_angle({0.0, 1.0, 0.0}, delta.yaw));
    return output;
}

RetargetResult bone_edits(const ecs::Registry& registry, scene::Scene* scene_graph,
    const AnimatedComponent& component, const Skeleton& skeleton, const Pose& pose,
    std::vector<scene::LocalTransformEdit>& edits, std::optional<ecs::Entity> model = std::nullopt) {
    if (component.bone_entities.empty()) { return {}; }
    if (!scene_graph || &scene_graph->registry() != &registry ||
        component.bone_entities.size() != skeleton.size()) { return {RetargetError::SceneRejected}; }
    auto unique = component.bone_entities;
    std::sort(unique.begin(), unique.end());
    if (std::adjacent_find(unique.begin(), unique.end()) != unique.end()) {
        return {RetargetError::InvalidEntity};
    }
    edits.reserve(edits.size() + skeleton.size());
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        const auto entity = component.bone_entities[index];
        if (registry.has<physics::BodyComponent>(entity) || registry.has<physics::CharacterComponent>(entity)) {
            return {RetargetError::PhysicsOwned, index};
        }
        const auto parent = skeleton.bones()[index].parent;
        if (!scene_graph->contains(entity) || (model && entity == *model) ||
            (parent != no_bone && scene_graph->parent(entity) != component.bone_entities[parent]) ||
            (parent == no_bone && model && scene_graph->parent(entity) != *model)) {
            return {RetargetError::SceneRejected, index};
        }
        edits.push_back({entity, pose.local[index]});
    }
    return {};
}

RetargetResult scene_actor(ecs::Registry& registry, scene::Scene* scene_graph,
                           ecs::Entity entity, const RetargetLimits& limits) {
    if (!scene_graph || &scene_graph->registry() != &registry || !scene_graph->contains(entity)) {
        return {RetargetError::InvalidEntity};
    }
    if (scene_graph->size() > limits.max_scene_nodes) { return {RetargetError::Capacity}; }
    for (const auto child : scene_graph->descendants(entity, true)) {
        if (registry.has<physics::BodyComponent>(child) || registry.has<physics::CharacterComponent>(child)) {
            return {RetargetError::PhysicsOwned};
        }
    }
    const auto world = scene_graph->world_matrix(entity);
    const auto transform = world ? math::decompose(*world) : std::nullopt;
    return transform && supported(*transform, limits.coordinate_limit)
        ? RetargetResult{} : RetargetResult{RetargetError::UnsupportedTransform};
}

core::CommandResult command_result(RetargetResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error)) + " element=" + std::to_string(result.element)};
}

bool parse_number(std::string_view text, double& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

} // namespace

std::string_view describe(RetargetError error) noexcept {
    switch (error) {
    case RetargetError::None: return "ok";
    case RetargetError::InvalidLimits: return "invalid retarget limits";
    case RetargetError::Capacity: return "retarget capacity or work budget exceeded";
    case RetargetError::InvalidName: return "invalid resource name";
    case RetargetError::InvalidSkeleton: return "incompatible skeleton";
    case RetargetError::InvalidPose: return "incompatible or invalid pose";
    case RetargetError::InvalidBone: return "invalid bone or non-root extraction bone";
    case RetargetError::DuplicateTarget: return "duplicate target bone";
    case RetargetError::InvalidWeight: return "invalid weight or translation scale";
    case RetargetError::UnsupportedTransform: return "unsupported shear reflection scale or twist";
    case RetargetError::InvalidHandle: return "invalid or exhausted generation handle";
    case RetargetError::StaleBinding: return "attachment identity or resource revision changed";
    case RetargetError::Cycle: return "retarget dependency cycle";
    case RetargetError::MissingResource: return "retarget map unavailable";
    case RetargetError::InvalidEntity: return "invalid entity or conflicting ownership";
    case RetargetError::PhysicsOwned: return "physical or character transform ownership rejected";
    case RetargetError::SceneRejected: return "scene hierarchy or transform rejected";
    case RetargetError::MissingPhysics: return "matching physics service required";
    case RetargetError::WrongBodyMode: return "exclusive enabled kinematic body required";
    case RetargetError::InvalidTime: return "invalid time range or unsupported wrap mode";
    case RetargetError::Discontinuity: return "motion discontinuity requires a new baseline";
    case RetargetError::PendingMotion: return "previous root motion has not been consumed";
    case RetargetError::Busy: return "registry or physics is busy";
    }
    return "unknown retarget error";
}

RetargetResult RetargetMap::load(const Skeleton& source, const Skeleton& target,
                                 std::vector<RetargetLink> links, RetargetLimits limits) {
    if (!valid_limits(limits)) { return {RetargetError::InvalidLimits}; }
    if (links.empty() || links.size() > limits.max_links) { return {RetargetError::Capacity}; }
    if (revision_ == exhausted) { return {RetargetError::InvalidHandle}; }
    auto checked = check_skeleton(source, limits);
    if (!checked) { return checked; }
    checked = check_skeleton(target, limits);
    if (!checked) { return checked; }
    std::vector<std::size_t> indices(target.size(), absent);
    std::vector<math::Quat> rotations;
    std::vector<math::Quat> translations;
    rotations.reserve(links.size());
    translations.reserve(links.size());
    for (std::size_t index = 0; index < links.size(); ++index) {
        const auto& link = links[index];
        if (link.source >= source.size() || link.target >= target.size()) { return {RetargetError::InvalidBone, index}; }
        if (indices[link.target] != absent) { return {RetargetError::DuplicateTarget, index}; }
        if (!bounded(link.weight, 0.0, 1.0) || !bounded(link.translation_scale, 0.0, 1.0e6) ||
            (link.translation != RetargetTranslation::Bind && link.translation != RetargetTranslation::ScaledLocalDelta)) {
            return {RetargetError::InvalidWeight, index};
        }
        const auto correction = math::normalized(global_rotation(target, link.target) *
            math::conjugate(global_rotation(source, link.source)));
        rotations.push_back(correction);
        translations.push_back(math::normalized(math::conjugate(global_rotation(target, target.bones()[link.target].parent)) *
            correction * global_rotation(source, source.bones()[link.source].parent)));
        indices[link.target] = index;
    }
    links_ = std::move(links);
    target_links_ = std::move(indices);
    rotations_ = std::move(rotations);
    translations_ = std::move(translations);
    limits_ = limits;
    source_signature_ = source.signature();
    target_signature_ = target.signature();
    ++revision_;
    return {};
}

RetargetResult RetargetMap::match_names(const Skeleton& source, const Skeleton& target,
                                       double translation_scale, RetargetLimits limits) {
    if (!valid_limits(limits)) { return {RetargetError::InvalidLimits}; }
    if (source.size() > limits.max_bones || target.size() > limits.max_bones) { return {RetargetError::Capacity}; }
    if (!bounded(translation_scale, 0.0, 1.0e6)) { return {RetargetError::InvalidWeight}; }
    std::vector<RetargetLink> links;
    for (std::size_t index = 0; index < target.size(); ++index) {
        const auto source_bone = source.find(target.bones()[index].name);
        if (source_bone == no_bone) { continue; }
        if (links.size() == limits.max_links) { return {RetargetError::Capacity}; }
        const auto policy = target.bones()[index].parent == no_bone
            ? RetargetTranslation::ScaledLocalDelta : RetargetTranslation::Bind;
        links.push_back({source_bone, static_cast<BoneIndex>(index), policy, translation_scale, 1.0, false});
    }
    return load(source, target, std::move(links), limits);
}

RetargetResult RetargetMap::apply(const Skeleton& source, const Pose& source_pose,
    const Skeleton& target, Pose& target_pose, double weight) const {
    if (!compatible(source, target)) { return {RetargetError::InvalidSkeleton}; }
    if (!bounded(weight, 0.0, 1.0)) { return {RetargetError::InvalidWeight}; }
    auto result = check_pose(source, source_pose, limits_);
    if (!result) { return result; }
    result = check_pose(target, target_pose, limits_);
    if (!result) { return result; }
    Pose input = source_pose;
    if (!input.update(source)) { return {RetargetError::InvalidPose}; }
    std::vector<math::Transform> input_globals;
    input_globals.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto transform = math::decompose(input.global[index]);
        if (!transform || !supported(*transform, limits_.coordinate_limit)) {
            return {RetargetError::UnsupportedTransform, index};
        }
        input_globals.push_back(*transform);
    }
    Pose candidate = target_pose;
    std::vector<math::Transform> output_globals(target.size());
    for (const auto bone : target.evaluation_order()) {
        const auto parent = target.bones()[bone].parent;
        const auto index = target_links_[bone];
        if (index != absent) {
            const auto& link = links_[index];
            const auto& source_bind = source.bones()[link.source].bind_local;
            auto local = target.bones()[bone].bind_local;
            const auto parent_rotation = parent == no_bone ? math::Quat{} : output_globals[parent].rotation;
            local.rotation = math::normalized(math::conjugate(parent_rotation) * rotations_[index] * input_globals[link.source].rotation);
            if (link.translation == RetargetTranslation::ScaledLocalDelta) {
                local.position += math::rotate(translations_[index], input.local[link.source].position - source_bind.position) * link.translation_scale;
            }
            if (link.copy_uniform_scale) { local.scale *= input.local[link.source].scale.x / source_bind.scale.x; }
            candidate.local[bone] = math::interpolate(candidate.local[bone], local, weight * link.weight);
        }
        const auto global = parent == no_bone ? math::matrix(candidate.local[bone])
            : math::matrix(output_globals[parent]) * math::matrix(candidate.local[bone]);
        const auto transform = math::decompose(global);
        if (!supported(candidate.local[bone], limits_.coordinate_limit) || !transform ||
            !supported(*transform, limits_.coordinate_limit)) { return {RetargetError::UnsupportedTransform, bone}; }
        output_globals[bone] = *transform;
    }
    if (!candidate.update(target)) { return {RetargetError::InvalidPose}; }
    target_pose = std::move(candidate);
    return {};
}

bool RetargetMap::compatible(const Skeleton& source, const Skeleton& target) const noexcept {
    return valid() && source.valid() && target.valid() &&
        source.signature() == source_signature_ && target.signature() == target_signature_;
}
bool RetargetMap::valid() const noexcept { return revision_ != 0 && !links_.empty(); }
std::uint64_t RetargetMap::revision() const noexcept { return revision_; }
std::span<const RetargetLink> RetargetMap::links() const noexcept { return links_; }
const RetargetLimits& RetargetMap::limits() const noexcept { return limits_; }

RetargetResult RootMotionTracker::configure(const Skeleton& skeleton, RootMotionSettings settings) {
    const auto result = check_skeleton(skeleton, hard_limits);
    if (!result) { return result; }
    if (!valid_settings(settings)) { return {RetargetError::InvalidLimits}; }
    if (settings.bone >= skeleton.size() || skeleton.bones()[settings.bone].parent != no_bone) {
        return {RetargetError::InvalidBone, settings.bone};
    }
    settings_ = settings;
    skeleton_signature_ = skeleton.signature();
    reset();
    return {};
}
void RootMotionTracker::reset() noexcept { primed_ = false; }
bool RootMotionTracker::primed() const noexcept { return primed_; }
const RootMotionSettings& RootMotionTracker::settings() const noexcept { return settings_; }

RootMotionResult RootMotionTracker::advance(const Skeleton& skeleton, Pose& pose,
    double delta_seconds, std::uint64_t continuity, bool strip) {
    RootMotionResult output;
    if (!skeleton.valid() || skeleton.signature() != skeleton_signature_) {
        output.result = {RetargetError::InvalidSkeleton}; return output;
    }
    if (!bounded(delta_seconds, 0.0, settings_.maximum_seconds)) {
        output.result = {RetargetError::InvalidTime}; return output;
    }
    RootMotionDelta current;
    output.result = root_value(skeleton, pose, settings_, current);
    if (!output.result) { return output; }
    output.primed = !primed_ || continuity != continuity_;
    if (!output.primed) {
        output.delta = difference({previous_translation_, previous_yaw_, 0.0}, current, delta_seconds);
        if (!acceptable(output.delta, settings_) || (delta_seconds == 0.0 &&
            (math::length_squared(output.delta.translation) > 1.0e-18 || std::abs(output.delta.yaw) > 1.0e-9))) {
            output.result = {RetargetError::Discontinuity}; return output;
        }
    }
    if (strip) {
        Pose candidate = pose;
        auto& root = candidate.local[settings_.bone];
        const auto inverse_yaw = math::from_axis_angle({0.0, 1.0, 0.0}, -current.yaw);
        root.position = math::rotate(inverse_yaw, root.position - current.translation);
        root.rotation = math::normalized(inverse_yaw * root.rotation);
        if (!candidate.update(skeleton)) { output.result = {RetargetError::InvalidPose}; return output; }
        pose = std::move(candidate);
    }
    previous_translation_ = current.translation;
    previous_yaw_ = current.yaw;
    continuity_ = continuity;
    primed_ = true;
    return output;
}

RootMotionResult extract_clip_root_motion(const Skeleton& skeleton, const AnimationClip& clip,
    double from, double to, WrapMode wrap, RootMotionSettings settings, RetargetLimits limits) {
    RootMotionResult output;
    if (!valid_limits(limits) || !valid_settings(settings)) { output.result = {RetargetError::InvalidLimits}; return output; }
    output.result = check_skeleton(skeleton, limits);
    if (!output.result) { return output; }
    if (settings.bone >= skeleton.size() || skeleton.bones()[settings.bone].parent != no_bone) {
        output.result = {RetargetError::InvalidBone, settings.bone}; return output;
    }
    if (!clip.compatible(skeleton)) { output.result = {RetargetError::InvalidSkeleton}; return output; }
    if ((wrap != WrapMode::Loop && wrap != WrapMode::Clamp) || !wrap_time(from, clip.duration(), wrap) ||
        !wrap_time(to, clip.duration(), wrap) || std::abs(to - from) > settings.maximum_seconds) {
        output.result = {RetargetError::InvalidTime}; return output;
    }
    const double duration = clip.duration();
    if (wrap == WrapMode::Clamp || duration == 0.0) {
        from = math::clamp(from, 0.0, duration);
        to = math::clamp(to, 0.0, duration);
    }
    if (from == to) { return output; }
    const bool forward = to > from;
    double cursor = from;
    std::size_t segments = 0;
    while (cursor != to) {
        if (++segments > limits.max_clip_segments || segments > limits.max_frame_bones / (2 * skeleton.size())) {
            output.result = {RetargetError::Capacity, segments}; return output;
        }
        double local_from = cursor;
        double local_to = to;
        double next = to;
        if (wrap == WrapMode::Loop && duration > 0.0) {
            const auto wrapped = wrap_time(cursor, duration, WrapMode::Loop);
            if (!wrapped) { output.result = {RetargetError::InvalidTime}; return output; }
            local_from = wrapped->time;
            if (!forward && local_from == 0.0) { local_from = duration; }
            const double available = forward ? duration - local_from : local_from;
            const double distance = std::min(std::abs(to - cursor), available);
            next = cursor + (forward ? distance : -distance);
            local_to = local_from + (forward ? distance : -distance);
            if (distance == std::abs(to - cursor)) { next = to; }
        }
        if (next == cursor) { output.result = {RetargetError::InvalidTime}; return output; }
        Pose first;
        Pose second;
        if (!clip.sample(skeleton, local_from, first, WrapMode::Clamp) ||
            !clip.sample(skeleton, local_to, second, WrapMode::Clamp)) {
            output.result = {RetargetError::InvalidPose}; return output;
        }
        RootMotionDelta start;
        RootMotionDelta finish;
        output.result = root_value(skeleton, first, settings, start);
        if (!output.result) { return output; }
        output.result = root_value(skeleton, second, settings, finish);
        if (!output.result) { return output; }
        output.delta = concatenate(output.delta, difference(start, finish, std::abs(next - cursor)));
        if (!acceptable(output.delta, settings)) { output.result = {RetargetError::Discontinuity}; return output; }
        cursor = next;
    }
    return output;
}

struct RetargetService::Impl {
    struct Identity {
        ecs::Entity entity{};
        const ecs::Registry* registry_identity{};
        std::uint64_t component_instance{};
        SkeletonHandle skeleton{};
        std::uint64_t signature{};
        std::vector<ecs::Entity> bones;
        bool matches(const ecs::Registry& registry, const AnimationService& animation) const {
            const auto* current = registry.get<AnimatedComponent>(entity);
            const auto* resource = animation.skeletons.get(skeleton);
            return registry_identity == &registry && current &&
                registry.component_instance<AnimatedComponent>(entity) == component_instance &&
                current->skeleton == skeleton &&
                current->bone_entities == bones && resource && resource->signature() == signature;
        }
    };
    struct Binding {
        RetargetBindingInfo info;
        Identity source;
        Identity target;
        std::shared_ptr<const RetargetMap> map;
    };
    struct Root {
        RootMotionBindingInfo info;
        Identity owner;
        RootMotionTracker tracker;
        physics::BodyHandle body{};
        RootMotionDelta pending;
        std::uint64_t pending_tick{};
        PlaybackState playback;
        bool clock_valid{};
        std::optional<std::uint64_t> sample_tick;
    };
    explicit Impl(RetargetLimits configuration) : limits(configuration) {}
    RetargetLimits limits;
    std::uint64_t generation{1};
    std::map<std::string, std::shared_ptr<const RetargetMap>, std::less<>> resources;
    std::map<ecs::Entity, Binding> bindings;
    std::map<ecs::Entity, Root> roots;
    static std::optional<Identity> identity(const ecs::Registry& registry,
        const AnimationService& animation, ecs::Entity entity, const RetargetLimits& limits) {
        const auto* component = registry.get<AnimatedComponent>(entity);
        const auto* skeleton = component ? animation.skeletons.get(component->skeleton) : nullptr;
        if (!skeleton || skeleton->size() > limits.max_bones ||
            component->bone_entities.size() > limits.max_bones || !component->animator.pose().compatible(*skeleton)) {
            return std::nullopt;
        }
        return Identity{entity, &registry, registry.component_instance<AnimatedComponent>(entity),
            component->skeleton, skeleton->signature(), component->bone_entities};
    }
    bool creates_cycle(ecs::Entity source, ecs::Entity target) const {
        for (std::size_t depth = 0; depth <= bindings.size(); ++depth) {
            if (source == target) { return true; }
            const auto found = bindings.find(source);
            if (found == bindings.end()) { return false; }
            source = found->second.source.entity;
        }
        return true;
    }
    std::vector<ecs::Entity> ordered() const {
        std::set<ecs::Entity> ready;
        std::multimap<ecs::Entity, ecs::Entity> children;
        for (const auto& [entity, binding] : bindings) {
            if (bindings.contains(binding.source.entity)) { children.emplace(binding.source.entity, entity); }
            else { ready.insert(entity); }
        }
        std::vector<ecs::Entity> result;
        result.reserve(bindings.size());
        while (!ready.empty()) {
            const auto entity = *ready.begin();
            ready.erase(ready.begin());
            result.push_back(entity);
            const auto [begin, end] = children.equal_range(entity);
            for (auto current = begin; current != end; ++current) { ready.insert(current->second); }
        }
        return result;
    }
};

RetargetService::RetargetService(RetargetLimits limits) {
    if (!valid_limits(limits)) { throw std::invalid_argument("invalid retarget service limits"); }
    impl_ = std::make_unique<Impl>(limits);
}
RetargetService::~RetargetService() = default;
const RetargetLimits& RetargetService::limits() const noexcept { return impl_->limits; }

RetargetResult RetargetService::publish(std::string name, std::shared_ptr<const RetargetMap> map) {
    if (!valid_name(name)) { return {RetargetError::InvalidName}; }
    if (!map || !map->valid()) { return {RetargetError::MissingResource}; }
    if (!impl_->resources.contains(name) && impl_->resources.size() == limits().max_resources) {
        return {RetargetError::Capacity};
    }
    impl_->resources.insert_or_assign(std::move(name), std::move(map));
    return {};
}
std::shared_ptr<const RetargetMap> RetargetService::find(std::string_view name) const {
    const auto found = impl_->resources.find(name);
    return found == impl_->resources.end() ? nullptr : found->second;
}
bool RetargetService::erase(std::string_view name) {
    const auto found = impl_->resources.find(name);
    if (found == impl_->resources.end()) { return false; }
    impl_->resources.erase(found);
    return true;
}

RetargetAttachResult RetargetService::attach(ecs::Registry& registry, AnimationService& animation,
    scene::Scene* scene_graph, ecs::Entity source, ecs::Entity target,
    std::string_view name, RetargetBindingOptions options) {
    if (registry.iterating()) { return {{RetargetError::Busy}, {}}; }
    if (!bounded(options.weight, 0.0, 1.0)) { return {{RetargetError::InvalidWeight}, {}}; }
    if (impl_->generation == exhausted) { return {{RetargetError::InvalidHandle}, {}}; }
    if (!impl_->bindings.contains(target) && impl_->bindings.size() == limits().max_bindings) {
        return {{RetargetError::Capacity}, {}};
    }
    if (impl_->creates_cycle(source, target)) { return {{RetargetError::Cycle}, {}}; }
    const auto root = impl_->roots.find(source);
    if (root != impl_->roots.end() && root->second.info.options.strip) { return {{RetargetError::InvalidEntity}, {}}; }
    auto source_identity = Impl::identity(registry, animation, source, limits());
    auto target_identity = Impl::identity(registry, animation, target, limits());
    if (!source_identity || !target_identity) { return {{RetargetError::InvalidEntity}, {}}; }
    auto map = find(name);
    if (!map) { return {{RetargetError::MissingResource}, {}}; }
    const bool weighted = options.weight < 1.0 || std::any_of(map->links().begin(), map->links().end(),
        [](const RetargetLink& link) { return link.weight < 1.0; });
    const auto& target_component = registry.require<AnimatedComponent>(target);
    if (weighted && !blend_owns_pose(registry, target) &&
        (!target_component.enabled || !target_component.animator.state().playing)) {
        return {{RetargetError::InvalidPose}, {}};
    }
    const auto& source_skeleton = *animation.skeletons.get(source_identity->skeleton);
    const auto& target_skeleton = *animation.skeletons.get(target_identity->skeleton);
    if (!map->compatible(source_skeleton, target_skeleton)) { return {{RetargetError::InvalidSkeleton}, {}}; }
    std::vector<scene::LocalTransformEdit> edits;
    if (options.write_scene) {
        const auto result = bone_edits(registry, scene_graph, target_component, target_skeleton,
            target_component.animator.pose(), edits);
        if (!result) { return {result, {}}; }
    }
    const RetargetHandle handle{target, impl_->generation};
    Impl::Binding candidate{{handle, source, map->revision(), options, {}},
        std::move(*source_identity), std::move(*target_identity), std::move(map)};
    impl_->bindings.insert_or_assign(target, std::move(candidate));
    ++impl_->generation;
    if (const auto existing = root_binding(target)) { (void)reset_root(existing); }
    return {{}, handle};
}

RetargetAttachResult RetargetService::attach_root(ecs::Registry& registry, AnimationService& animation,
    scene::Scene* scene_graph, physics::PhysicsWorld* physics, ecs::Entity entity,
    RootMotionBindingOptions options) {
    if (registry.iterating()) { return {{RetargetError::Busy}, {}}; }
    if (impl_->generation == exhausted) { return {{RetargetError::InvalidHandle}, {}}; }
    if (!impl_->roots.contains(entity) && impl_->roots.size() == limits().max_bindings) {
        return {{RetargetError::Capacity}, {}};
    }
    if (options.application != RootMotionApplication::Observe && options.application != RootMotionApplication::Scene &&
        options.application != RootMotionApplication::KinematicTarget) { return {{RetargetError::InvalidEntity}, {}}; }
    for (const auto& [target, binding] : impl_->bindings) {
        (void)target;
        if (options.strip && binding.source.entity == entity) { return {{RetargetError::InvalidEntity}, {}}; }
    }
    for (const auto& [owner, root] : impl_->roots) {
        if (owner != entity && root.info.options.model_entity == options.model_entity &&
            root.info.options.application != RootMotionApplication::Observe &&
            options.application != RootMotionApplication::Observe) { return {{RetargetError::InvalidEntity}, {}}; }
    }
    auto identity = Impl::identity(registry, animation, entity, limits());
    if (!identity) { return {{RetargetError::InvalidEntity}, {}}; }
    const auto& skeleton = *animation.skeletons.get(identity->skeleton);
    Impl::Root candidate;
    auto result = check_skeleton(skeleton, limits());
    if (!result) { return {result, {}}; }
    result = candidate.tracker.configure(skeleton, options.settings);
    if (!result) { return {result, {}}; }
    if (options.application == RootMotionApplication::Scene) {
        result = scene_actor(registry, scene_graph, options.model_entity, limits());
        if (!result) { return {result, {}}; }
    }
    if (options.application == RootMotionApplication::KinematicTarget) {
        if (!physics || &physics->registry() != &registry || &physics->scene() != scene_graph) {
            return {{RetargetError::MissingPhysics}, {}};
        }
        if (physics->stepping()) { return {{RetargetError::Busy}, {}}; }
        const auto* body = physics->get(options.model_entity);
        if (registry.has<physics::CharacterComponent>(options.model_entity)) { return {{RetargetError::PhysicsOwned}, {}}; }
        if (!body || body->mode != physics::BodyMode::Kinematic || !body->enabled ||
            !supported(body->pose, limits().coordinate_limit)) { return {{RetargetError::WrongBodyMode}, {}}; }
        candidate.body = body->handle;
    }
    std::vector<scene::LocalTransformEdit> edits;
    if (options.write_scene) {
        const auto& component = registry.require<AnimatedComponent>(entity);
        result = bone_edits(registry, scene_graph, component, skeleton,
            component.animator.pose(), edits, options.model_entity);
        if (!result) { return {result, {}}; }
    }
    const RetargetHandle handle{entity, impl_->generation};
    candidate.info.handle = handle;
    candidate.info.options = options;
    candidate.owner = std::move(*identity);
    impl_->roots.insert_or_assign(entity, std::move(candidate));
    ++impl_->generation;
    return {{}, handle};
}

bool RetargetService::detach(RetargetHandle handle) noexcept {
    const auto found = impl_->bindings.find(handle.entity);
    if (found == impl_->bindings.end() || found->second.info.handle != handle) { return false; }
    impl_->bindings.erase(found);
    const auto root = impl_->roots.find(handle.entity);
    if (root != impl_->roots.end()) { root->second.tracker.reset(); root->second.info.pending = false; }
    return true;
}
bool RetargetService::detach_root(RetargetHandle handle) noexcept {
    const auto found = impl_->roots.find(handle.entity);
    if (found == impl_->roots.end() || found->second.info.handle != handle) { return false; }
    impl_->roots.erase(found);
    return true;
}
RetargetResult RetargetService::reset_root(RetargetHandle handle) {
    const auto found = impl_->roots.find(handle.entity);
    if (found == impl_->roots.end() || found->second.info.handle != handle) { return {RetargetError::InvalidHandle}; }
    auto& root = found->second;
    if (root.info.continuity == exhausted) { return {RetargetError::InvalidHandle}; }
    ++root.info.continuity;
    root.info.pending = false;
    root.info.last_sample = {};
    root.info.last_application = {};
    root.clock_valid = false;
    root.sample_tick.reset();
    root.tracker.reset();
    return {};
}
std::optional<RetargetBindingInfo> RetargetService::inspect(RetargetHandle handle) const {
    const auto found = impl_->bindings.find(handle.entity);
    if (found == impl_->bindings.end() || found->second.info.handle != handle) { return std::nullopt; }
    return found->second.info;
}
std::optional<RootMotionBindingInfo> RetargetService::inspect_root(RetargetHandle handle) const {
    const auto found = impl_->roots.find(handle.entity);
    if (found == impl_->roots.end() || found->second.info.handle != handle) { return std::nullopt; }
    return found->second.info;
}
RetargetHandle RetargetService::binding(ecs::Entity entity) const noexcept {
    const auto found = impl_->bindings.find(entity);
    return found == impl_->bindings.end() ? RetargetHandle{} : found->second.info.handle;
}
RetargetHandle RetargetService::root_binding(ecs::Entity entity) const noexcept {
    const auto found = impl_->roots.find(entity);
    return found == impl_->roots.end() ? RetargetHandle{} : found->second.info.handle;
}

RetargetUpdate RetargetService::update(ecs::Registry& registry, AnimationService& animation,
    scene::Scene* scene_graph, double delta_seconds, std::uint64_t tick,
    const scene::WorldPartition* partition) {
    RetargetUpdate output;
    if (registry.iterating() || !bounded(delta_seconds, 0.0, 3600.0) ||
        (scene_graph && &scene_graph->registry() != &registry) || (partition && !partition->origin().consistent)) {
        output.errors = 1; return output;
    }
    std::set<ecs::Entity> failed;
    for (auto current = impl_->bindings.begin(); current != impl_->bindings.end();) {
        const auto& binding = current->second;
        if (!binding.source.matches(registry, animation) || !binding.target.matches(registry, animation) ||
            binding.map->revision() != binding.info.map_revision) {
            failed.insert(current->first);
            current = impl_->bindings.erase(current);
            ++output.retired;
        } else { ++current; }
    }
    const auto order = impl_->ordered();
    if (order.size() != impl_->bindings.size()) { output.errors = 1; return output; }
    for (const auto entity : order) {
        auto& binding = impl_->bindings.at(entity);
        auto& status = binding.info.last_result;
        status = {};
        if (!binding.info.options.enabled || !registry.enabled(entity) || !registry.enabled(binding.source.entity) ||
            failed.contains(binding.source.entity) || (scene_graph &&
            ((scene_graph->contains(entity) && !scene_graph->effective_enabled(entity)) ||
             (scene_graph->contains(binding.source.entity) && !scene_graph->effective_enabled(binding.source.entity))))) {
            failed.insert(entity); ++output.skipped; continue;
        }
        const auto& source = *animation.skeletons.get(binding.source.skeleton);
        const auto& target = *animation.skeletons.get(binding.target.skeleton);
        const bool weighted = binding.info.options.weight < 1.0 ||
            std::any_of(binding.map->links().begin(), binding.map->links().end(), [](const RetargetLink& link) { return link.weight < 1.0; });
        const auto& target_component = registry.require<AnimatedComponent>(entity);
        if (weighted && !blend_owns_pose(registry, entity) &&
            (!target_component.enabled || !target_component.animator.state().playing)) {
            status = {RetargetError::InvalidPose}; failed.insert(entity); ++output.errors; continue;
        }
        const auto work = source.size() + target.size();
        if (work > limits().max_frame_bones - output.bones) { status = {RetargetError::Capacity}; }
        else {
            output.bones += work;
            auto* target_pose = composed_pose(registry, entity);
            const auto* source_pose = composed_pose(registry, binding.source.entity);
            if (!target_pose || !source_pose) { status = {RetargetError::InvalidPose}; }
            else { status = check_pose(target, *target_pose, limits()); }
            if (status) {
                Pose candidate = *target_pose;
                status = binding.map->apply(source, *source_pose, target, candidate, binding.info.options.weight);
                std::vector<scene::LocalTransformEdit> edits;
                if (status && binding.info.options.write_scene) {
                    status = bone_edits(registry, scene_graph, target_component, target, candidate, edits);
                }
                if (status && !edits.empty() && !scene_graph->apply_local_transforms(edits)) {
                    status = {RetargetError::SceneRejected};
                }
                if (status) { *target_pose = std::move(candidate); (void)registry.mark_changed<AnimatedComponent>(entity); }
            }
        }
        if (status) { ++output.retargeted; }
        else { failed.insert(entity); ++output.errors; }
    }
    for (auto current = impl_->roots.begin(); current != impl_->roots.end();) {
        auto& root = current->second;
        const auto entity = current->first;
        if (!root.owner.matches(registry, animation)) {
            current = impl_->roots.erase(current); ++output.retired; continue;
        }
        ++current;
        const auto& options = root.info.options;
        if (root.sample_tick && tick <= *root.sample_tick) { ++output.skipped; continue; }
        if (!options.enabled || !registry.enabled(entity) || failed.contains(entity) ||
            (scene_graph && scene_graph->contains(entity) && !scene_graph->effective_enabled(entity))) {
            root.tracker.reset(); root.info.pending = false; ++output.skipped; continue;
        }
        const auto& skeleton = *animation.skeletons.get(root.owner.skeleton);
        auto* component = registry.get<AnimatedComponent>(entity);
        const auto playback = component->animator.state();
        const bool composed = impl_->bindings.contains(entity) || blend_owns_pose(registry, entity);
        // Active playback republishes raw locals even at zero speed or zero
        // delta time, so those samples still need their root motion stripped.
        if (!composed && (!component->enabled || !playback.playing) &&
            root.clock_valid && playback.clip == root.playback.clip &&
            playback.wrap == root.playback.wrap && playback.time == root.playback.time) {
            ++output.skipped; continue;
        }
        if (!composed && root.clock_valid) {
            double expected = root.playback.time + delta_seconds * playback.speed;
            bool discontinuity = playback.clip != root.playback.clip || playback.wrap != root.playback.wrap;
            if (const auto* clip = animation.clips.get(playback.clip)) {
                if (playback.wrap == WrapMode::Clamp) { expected = math::clamp(expected, 0.0, clip->duration()); }
                const auto before = wrap_time(root.playback.time, clip->duration(), playback.wrap);
                const auto after = wrap_time(playback.time, clip->duration(), playback.wrap);
                discontinuity = discontinuity || !before || !after || before->cycle != after->cycle || before->reverse != after->reverse;
            }
            discontinuity = discontinuity || std::abs(playback.time - expected) > 1.0e-6;
            if (discontinuity) { root.tracker.reset(); root.info.pending = false; }
        }
        root.playback = playback;
        root.clock_valid = true;
        RetargetResult status;
        if (root.info.pending) { status = {RetargetError::PendingMotion}; }
        if (skeleton.size() > limits().max_frame_bones - output.bones) { status = {RetargetError::Capacity}; }
        if (!status) {
            root.info.last_sample.result = status; root.tracker.reset(); root.info.pending = false;
            ++output.errors; continue;
        }
        output.bones += skeleton.size();
        auto* canonical = composed_pose(registry, entity);
        if (!canonical) { root.info.last_sample.result = {RetargetError::InvalidPose}; ++output.errors; continue; }
        status = check_pose(skeleton, *canonical, limits());
        if (options.application == RootMotionApplication::KinematicTarget) {
            const auto* body = registry.get<physics::BodyComponent>(options.model_entity);
            if (!body || body->body != root.body || registry.has<physics::CharacterComponent>(options.model_entity)) {
                status = {RetargetError::PhysicsOwned};
            }
        }
        if (!status) { root.info.last_sample.result = status; root.tracker.reset(); ++output.errors; continue; }
        Pose candidate = *canonical;
        auto tracker = root.tracker;
        auto sample = tracker.advance(skeleton, candidate, delta_seconds, root.info.continuity, options.strip);
        status = sample.result;
        std::vector<scene::LocalTransformEdit> edits;
        if (status && options.write_scene) {
            status = bone_edits(registry, scene_graph, *component, skeleton, candidate, edits, options.model_entity);
        }
        if (status && options.application == RootMotionApplication::Scene) {
            status = scene_actor(registry, scene_graph, options.model_entity, limits());
            if (status) {
                const auto world = math::decompose(*scene_graph->world_matrix(options.model_entity));
                const auto target = moved(*world, sample.delta);
                const auto parent = scene_graph->parent(options.model_entity);
                math::Mat4 parent_matrix;
                if (parent) {
                    const auto matrix = scene_graph->world_matrix(parent);
                    if (!matrix) { status = {RetargetError::SceneRejected}; }
                    else { parent_matrix = *matrix; }
                }
                const auto inverse = math::inverse(parent_matrix);
                const auto local = inverse ? math::decompose(*inverse * math::matrix(target)) : std::nullopt;
                if (!local || !supported(*local, limits().coordinate_limit) || !supported(target, limits().coordinate_limit)) {
                    status = {RetargetError::UnsupportedTransform};
                }
                if (status) { edits.push_back({options.model_entity, *local}); }
            }
        }
        if (status && !edits.empty() && !scene_graph->apply_local_transforms(edits)) { status = {RetargetError::SceneRejected}; }
        sample.result = status;
        root.info.last_sample = sample;
        if (!status) { root.tracker.reset(); root.info.pending = false; ++output.errors; continue; }
        *canonical = std::move(candidate);
        root.tracker = tracker;
        root.sample_tick = tick;
        if (options.application == RootMotionApplication::KinematicTarget && !sample.primed) {
            root.pending = sample.delta;
            root.pending_tick = tick;
            root.info.pending = true;
        }
        if (options.application == RootMotionApplication::Scene && !sample.primed) { ++output.applied; }
        (void)registry.mark_changed<AnimatedComponent>(entity);
        ++output.captured;
    }
    return output;
}

RetargetUpdate RetargetService::apply_pending(ecs::Registry& registry, const AnimationService& animation, scene::Scene* scene_graph,
    physics::PhysicsWorld* physics, std::uint64_t tick, const scene::WorldPartition* partition) {
    RetargetUpdate output;
    for (auto& [entity, root] : impl_->roots) {
        if (!root.info.pending) { continue; }
        if (tick == root.pending_tick) { ++output.skipped; continue; }
        root.info.pending = false;
        auto& result = root.info.last_application;
        result = {};
        const auto model = root.info.options.model_entity;
        if (root.pending_tick == exhausted || tick != root.pending_tick + 1 ||
            (partition && !partition->origin().consistent)) { result = {RetargetError::Discontinuity}; }
        else if (registry.iterating() || (physics && physics->stepping())) { result = {RetargetError::Busy}; }
        else if (!physics || &physics->registry() != &registry || &physics->scene() != scene_graph) {
            result = {RetargetError::MissingPhysics};
        } else if (!root.owner.matches(registry, animation) || !registry.enabled(entity) || !registry.enabled(model)) {
            result = {RetargetError::StaleBinding};
        } else if (registry.has<physics::CharacterComponent>(model)) { result = {RetargetError::PhysicsOwned}; }
        else {
            const auto* body = physics->get(root.body);
            const auto* ownership = registry.get<physics::BodyComponent>(model);
            if (!body || body->entity != model || !ownership || ownership->body != root.body ||
                body->mode != physics::BodyMode::Kinematic || !body->enabled || !scene_graph->effective_enabled(model)) {
                result = {RetargetError::WrongBodyMode};
            } else {
                const auto target = moved(body->pose, root.pending);
                if (!supported(body->pose, limits().coordinate_limit) || !supported(target, limits().coordinate_limit)) {
                    result = {RetargetError::UnsupportedTransform};
                } else if (!physics->set_kinematic_target(root.body, target)) { result = {RetargetError::WrongBodyMode}; }
            }
        }
        if (result) { ++output.applied; }
        else { root.tracker.reset(); ++output.errors; }
    }
    return output;
}

void register_retarget(core::Kernel& kernel) {
    if (kernel.services().find<RetargetService>()) { return; }
    auto& service = kernel.services().emplace<RetargetService>();
    (void)kernel.register_module({"animation.retarget", "Bind-space retarget maps and explicit root motion ownership", 1});
    (void)kernel.add_system({std::string(retarget_system_name), core::Phase::Animation, retarget_pose_order, {"animation.skeleton"}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto* registry = host.services().find<ecs::Registry>();
            auto* animation = host.services().find<AnimationService>();
            if (!registry || !animation) { return; }
            const auto result = host.services().get<RetargetService>().update(*registry, *animation,
                host.services().find<scene::Scene>(), frame.delta_seconds, frame.tick, host.services().find<scene::WorldPartition>());
            host.metric("animation.retarget.poses", static_cast<double>(result.retargeted));
            host.metric("animation.retarget.errors", static_cast<double>(result.errors));
            host.metric("animation.retarget.retired", static_cast<double>(result.retired));
            host.metric("animation.root_motion.captured", static_cast<double>(result.captured));
        });
    if (kernel.services().find<IKService>()) { (void)kernel.order_after(ik_system_name, retarget_system_name); }
    (void)kernel.add_system({std::string(root_motion_system_name), core::Phase::PreSimulation, -8000, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto* registry = host.services().find<ecs::Registry>();
            auto* animation = host.services().find<AnimationService>();
            if (!registry || !animation) { return; }
            auto* physics = host.config().boolean("physics.enabled", true) && frame.delta_seconds > 0.0
                ? host.services().find<physics::PhysicsWorld>() : nullptr;
            const auto result = host.services().get<RetargetService>().apply_pending(*registry, *animation,
                host.services().find<scene::Scene>(), physics, frame.tick,
                host.services().find<scene::WorldPartition>());
            host.metric("animation.root_motion.applied", static_cast<double>(result.applied));
            host.metric("animation.root_motion.errors", static_cast<double>(result.errors));
        });
    if (kernel.services().find<scene::WorldPartition>()) {
        (void)kernel.order_after(root_motion_system_name, "scene.partition.update");
    }
    (void)kernel.add_command("animation.retarget.map", [owner = &service](core::Kernel& host, core::CommandArgs args) {
        auto* animation = host.services().find<AnimationService>();
        if (!animation || args.size() < 3 || args.size() > 4) {
            return core::CommandResult{false, "usage: animation.retarget.map name source_skeleton target_skeleton [root_scale]"};
        }
        const auto* source = animation->skeletons.get(animation->skeletons.find(args[1]));
        const auto* target = animation->skeletons.get(animation->skeletons.find(args[2]));
        double scale = 1.0;
        if (!source || !target || (args.size() == 4 && !parse_number(args[3], scale))) {
            return core::CommandResult{false, "valid skeleton names and numeric scale required"};
        }
        auto map = std::make_shared<RetargetMap>();
        auto result = map->match_names(*source, *target, scale, owner->limits());
        if (result) { result = owner->publish(args[0], std::move(map)); }
        return command_result(result);
    }, "Create a map from exact bone-name matches; scale root translation explicitly");
    (void)kernel.add_command("animation.retarget.attach", [owner = &service](core::Kernel& host, core::CommandArgs args) {
        auto* registry = host.services().find<ecs::Registry>();
        auto* animation = host.services().find<AnimationService>();
        if (!registry || !animation || args.size() != 3) {
            return core::CommandResult{false, "usage: animation.retarget.attach source_entity target_entity map"};
        }
        const auto source = ecs::parse_entity(args[0]);
        const auto target = ecs::parse_entity(args[1]);
        if (!source || !target) { return core::CommandResult{false, "exact index:generation entities required"}; }
        return command_result(owner->attach(*registry, *animation, host.services().find<scene::Scene>(), *source, *target, args[2]).result);
    }, "Attach a deterministic source-to-target retarget dependency");
    (void)kernel.add_command("animation.retarget.detach", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "usage: animation.retarget.detach target_entity"}; }
        const auto entity = ecs::parse_entity(args[0]);
        return core::CommandResult{entity && owner->detach(owner->binding(*entity)), "retarget attachment removed if current"};
    }, "Remove a retarget dependency without restoring an old pose");
    (void)kernel.add_command("animation.root_motion.attach", [owner = &service](core::Kernel& host, core::CommandArgs args) {
        auto* registry = host.services().find<ecs::Registry>();
        auto* animation = host.services().find<AnimationService>();
        if (!registry || !animation || args.size() != 4) {
            return core::CommandResult{false, "usage: animation.root_motion.attach animated_entity model_entity bone_name observe|scene|kinematic"};
        }
        const auto entity = ecs::parse_entity(args[0]);
        const auto model = ecs::parse_entity(args[1]);
        const auto* component = entity ? registry->get<AnimatedComponent>(*entity) : nullptr;
        const auto* skeleton = component ? animation->skeletons.get(component->skeleton) : nullptr;
        if (!model || !skeleton) { return core::CommandResult{false, "live animated and model entities required"}; }
        RootMotionBindingOptions options;
        options.model_entity = *model;
        options.settings.bone = skeleton->find(args[2]);
        if (args[3] == "scene") { options.application = RootMotionApplication::Scene; }
        else if (args[3] == "kinematic") { options.application = RootMotionApplication::KinematicTarget; }
        else if (args[3] != "observe") { return core::CommandResult{false, "application must be observe scene or kinematic"}; }
        return command_result(owner->attach_root(*registry, *animation, host.services().find<scene::Scene>(),
            host.services().find<physics::PhysicsWorld>(), *entity, options).result);
    }, "Extract horizontal translation and yaw; explicitly select the transform owner");
    (void)kernel.add_command("animation.root_motion.reset", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "usage: animation.root_motion.reset animated_entity"}; }
        const auto entity = ecs::parse_entity(args[0]);
        return command_result(entity ? owner->reset_root(owner->root_binding(*entity)) : RetargetResult{RetargetError::InvalidEntity});
    }, "Cancel pending motion and prime a new baseline after a graph seam or seek");
    (void)kernel.add_command("animation.root_motion.detach", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "usage: animation.root_motion.detach animated_entity"}; }
        const auto entity = ecs::parse_entity(args[0]);
        return core::CommandResult{entity && owner->detach_root(owner->root_binding(*entity)), "root motion removed if current"};
    }, "Cancel the exact current root attachment and its queued physical delta");
    (void)kernel.add_command("animation.root_motion.inspect", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (args.size() != 1) { return core::CommandResult{false, "usage: animation.root_motion.inspect animated_entity"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const auto info = entity ? owner->inspect_root(owner->root_binding(*entity)) : std::nullopt;
        if (!info) { return core::CommandResult{false, "root attachment unavailable"}; }
        const auto& delta = info->last_sample.delta;
        return core::CommandResult{true, "translation=" + std::to_string(delta.translation.x) + "," +
            std::to_string(delta.translation.y) + "," + std::to_string(delta.translation.z) + " yaw=" +
            std::to_string(delta.yaw) + " pending=" + std::to_string(info->pending) + " capture=" +
            std::string(describe(info->last_sample.result.error)) + " application=" + std::string(describe(info->last_application.error))};
    }, "Inspect actual captured displacement and last application result");
}

} // namespace ge::animation
