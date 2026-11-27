#include <ge/animation/blend.hpp>

#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ge::animation {
namespace {

constexpr BlendLimits hard_limits{};
constexpr double maximum_time = 1.0e12;
constexpr double maximum_step = 86400.0;
constexpr double maximum_scalar = 1.0e6;
constexpr double point_tolerance_squared = 1.0e-12;

bool valid_limits(const BlendLimits& limits) noexcept {
    return limits.max_nodes > 0 && limits.max_nodes <= hard_limits.max_nodes &&
        limits.max_inputs > 0 && limits.max_inputs <= hard_limits.max_inputs &&
        limits.max_edges <= hard_limits.max_edges && limits.max_depth > 0 &&
        limits.max_depth <= hard_limits.max_depth && limits.max_parameters <= hard_limits.max_parameters &&
        limits.max_states > 0 && limits.max_states <= hard_limits.max_states &&
        limits.max_transitions <= hard_limits.max_transitions && limits.max_conditions <= hard_limits.max_conditions &&
        limits.max_mask_values <= hard_limits.max_mask_values && limits.max_cached_transforms > 0 &&
        limits.max_cached_transforms <= hard_limits.max_cached_transforms && limits.max_resources > 0 &&
        limits.max_resources <= hard_limits.max_resources && limits.max_entities > 0 &&
        limits.max_entities <= hard_limits.max_entities;
}

bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 128 && std::none_of(name.begin(), name.end(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 32U || byte == 127U;
    });
}

bool bounded(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool valid_transform(const math::Transform& value) noexcept {
    const double magnitude = math::dot(value.rotation, value.rotation);
    return math::finite(value) && std::isfinite(magnitude) && magnitude > math::epsilon * math::epsilon;
}

bool valid_mask(std::span<const double> mask, std::size_t bone_count) noexcept {
    return (mask.empty() || mask.size() == bone_count) &&
        std::all_of(mask.begin(), mask.end(), [](double value) { return bounded(value, 0.0, 1.0); });
}

bool valid_pose(const Pose& pose, const Skeleton& skeleton) noexcept {
    return pose.compatible(skeleton) && std::all_of(pose.local.begin(), pose.local.end(), valid_transform);
}

BlendResult converted(AnimationResult result) noexcept {
    return result ? BlendResult{} : BlendResult{BlendError::AnimationFailure, result.element, result.error};
}

Pose bind_locals(const Skeleton& skeleton) {
    Pose result;
    result.skeleton_signature = skeleton.signature();
    result.local.reserve(skeleton.size());
    for (const auto& bone : skeleton.bones()) { result.local.push_back(bone.bind_local); }
    return result;
}

void hash_word(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 255U;
        hash *= 1099511628211ULL;
    }
}

bool numeric_parameter(const BlendParameters& parameters, BlendParameterId parameter) noexcept {
    const auto* definition = parameters.definition(parameter);
    return parameter == no_blend_parameter || (definition && definition->type == BlendParameterType::Number);
}

BlendResult local_blend(const Skeleton& skeleton, std::span<const WeightedPose> sources, Pose& output) {
    if (!skeleton.valid()) { return {BlendError::InvalidSkeleton}; }
    if (sources.size() > hard_limits.max_inputs) { return {BlendError::Capacity}; }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (!source.pose || !valid_pose(*source.pose, skeleton)) { return {BlendError::InvalidSkeleton, index}; }
        if (!bounded(source.weight, 0.0, maximum_scalar) || !valid_mask(source.mask, skeleton.size())) {
            return {BlendError::InvalidWeight, index};
        }
    }
    Pose candidate = bind_locals(skeleton);
    for (std::size_t bone = 0; bone < skeleton.size(); ++bone) {
        double total = 0.0;
        double largest = 0.0;
        math::Quat reference = skeleton.bones()[bone].bind_local.rotation;
        for (const auto& source : sources) {
            const double weight = source.weight * (source.mask.empty() ? 1.0 : source.mask[bone]);
            total += weight;
            if (weight > largest) { largest = weight; reference = math::normalized(source.pose->local[bone].rotation); }
        }
        if (total == 0.0) { continue; }
        auto& result = candidate.local[bone];
        result.position = {};
        result.scale = {};
        math::Quat rotation{0.0, 0.0, 0.0, 0.0};
        for (const auto& source : sources) {
            const double weight = source.weight * (source.mask.empty() ? 1.0 : source.mask[bone]) / total;
            if (weight == 0.0) { continue; }
            const auto& transform = source.pose->local[bone];
            result.position += transform.position * weight;
            result.scale += transform.scale * weight;
            auto quaternion = math::normalized(transform.rotation);
            if (math::dot(reference, quaternion) < 0.0) { quaternion = -quaternion; }
            rotation = rotation + quaternion * weight;
        }
        result.rotation = math::normalized(rotation, reference);
        if (!valid_transform(result)) { return {BlendError::NonFiniteResult, bone}; }
    }
    output = std::move(candidate);
    return {};
}

BlendResult local_additive(const Skeleton& skeleton, const Pose& base, const Pose& additive,
                           const Pose& reference, double weight, std::span<const double> mask, Pose& output) {
    if (!valid_pose(base, skeleton) || !valid_pose(additive, skeleton) || !valid_pose(reference, skeleton)) {
        return {BlendError::InvalidSkeleton};
    }
    if (!bounded(weight, 0.0, maximum_scalar) || !valid_mask(mask, skeleton.size())) {
        return {BlendError::InvalidWeight};
    }
    Pose candidate = bind_locals(skeleton);
    for (std::size_t bone = 0; bone < skeleton.size(); ++bone) {
        const double strength = weight * (mask.empty() ? 1.0 : mask[bone]);
        const auto& first = base.local[bone];
        const auto& second = additive.local[bone];
        const auto& origin = reference.local[bone];
        auto& result = candidate.local[bone];
        if (strength == 0.0) { result = first; continue; }
        result.position = first.position + (second.position - origin.position) * strength;
        result.scale = first.scale + (second.scale - origin.scale) * strength;
        const auto delta = math::conjugate(math::normalized(origin.rotation)) * math::normalized(second.rotation);
        result.rotation = math::normalized(math::normalized(first.rotation) * math::slerp({}, delta, strength));
        if (!valid_transform(result)) { return {BlendError::NonFiniteResult, bone}; }
    }
    output = std::move(candidate);
    return {};
}

double curve_value(BlendCurve curve, double fraction) noexcept {
    const double value = math::saturate(fraction);
    return curve == BlendCurve::Smoothstep ? value * value * (3.0 - 2.0 * value) : value;
}

bool condition_met(const BlendCondition& condition, const BlendParameters& parameters) noexcept {
    const auto value = parameters.value(condition.parameter);
    if (!value) { return false; }
    switch (condition.operation) {
    case BlendConditionOp::Less: return *value < condition.threshold;
    case BlendConditionOp::LessEqual: return *value <= condition.threshold;
    case BlendConditionOp::Greater: return *value > condition.threshold;
    case BlendConditionOp::GreaterEqual: return *value >= condition.threshold;
    case BlendConditionOp::Equal: return std::abs(*value - condition.threshold) <= condition.tolerance;
    case BlendConditionOp::NotEqual: return std::abs(*value - condition.threshold) > condition.tolerance;
    case BlendConditionOp::IsTrue:
    case BlendConditionOp::Triggered: return *value == 1.0;
    case BlendConditionOp::IsFalse: return *value == 0.0;
    }
    return false;
}

bool number(std::string_view text, double& output) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(output);
}

core::CommandResult command_result(BlendResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error))};
}

} // namespace

std::string_view describe(BlendError error) noexcept {
    switch (error) {
    case BlendError::None: return "ok";
    case BlendError::InvalidLimits: return "invalid blend limits";
    case BlendError::Capacity: return "blend capacity exhausted";
    case BlendError::InvalidName: return "invalid blend name";
    case BlendError::DuplicateName: return "duplicate blend name";
    case BlendError::InvalidSkeleton: return "incompatible blend skeleton or pose";
    case BlendError::InvalidNode: return "invalid blend node";
    case BlendError::InvalidParameter: return "invalid blend parameter";
    case BlendError::InvalidWeight: return "invalid blend weight or mask";
    case BlendError::InvalidTime: return "invalid blend time";
    case BlendError::Cycle: return "blend graph contains a cycle";
    case BlendError::DepthLimit: return "blend graph depth exceeded";
    case BlendError::MissingClip: return "blend clip is missing";
    case BlendError::IncompatibleClip: return "blend clip skeleton changed";
    case BlendError::InvalidState: return "invalid blend state";
    case BlendError::InvalidTransition: return "invalid blend transition";
    case BlendError::InvalidBinding: return "blend authority or scene binding changed";
    case BlendError::NonFiniteResult: return "nonfinite blended transform";
    case BlendError::RegistryBusy: return "registry structural mutation is unavailable";
    case BlendError::AnimationFailure: return "animation sampling or pose update failed";
    }
    return "unknown blend error";
}

BlendResult BlendParameters::load(std::vector<BlendParameter> definitions, BlendLimits limits) {
    if (!valid_limits(limits)) { return {BlendError::InvalidLimits}; }
    if (definitions.size() > limits.max_parameters) { return {BlendError::Capacity}; }
    std::vector<double> values;
    values.reserve(definitions.size());
    std::uint64_t signature = 14695981039346656037ULL;
    hash_word(signature, static_cast<std::uint64_t>(definitions.size()));
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        auto& definition = definitions[index];
        if (!valid_name(definition.name)) { return {BlendError::InvalidName, index}; }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (definitions[prior].name == definition.name) { return {BlendError::DuplicateName, index}; }
        }
        if (definition.type == BlendParameterType::Boolean || definition.type == BlendParameterType::Trigger) {
            if (definition.initial != 0.0 && definition.initial != 1.0) { return {BlendError::InvalidParameter, index}; }
            if (definition.type == BlendParameterType::Trigger && definition.initial != 0.0) {
                return {BlendError::InvalidParameter, index};
            }
            definition.minimum = 0.0;
            definition.maximum = 1.0;
        } else if (definition.type != BlendParameterType::Number) { return {BlendError::InvalidParameter, index}; }
        if (!bounded(definition.minimum, -maximum_scalar, maximum_scalar) ||
            !bounded(definition.maximum, definition.minimum, maximum_scalar) ||
            !bounded(definition.initial, definition.minimum, definition.maximum)) {
            return {BlendError::InvalidParameter, index};
        }
        hash_word(signature, static_cast<std::uint64_t>(definition.name.size()));
        for (const char letter : definition.name) { hash_word(signature, static_cast<unsigned char>(letter)); }
        hash_word(signature, static_cast<std::uint64_t>(definition.type));
        for (const double value : {definition.initial, definition.minimum, definition.maximum}) {
            hash_word(signature, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
        }
        values.push_back(definition.initial);
    }
    definitions_ = std::move(definitions);
    values_ = std::move(values);
    signature_ = signature == 0 ? 1 : signature;
    return {};
}

BlendParameterId BlendParameters::find(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < definitions_.size(); ++index) {
        if (definitions_[index].name == name) { return static_cast<BlendParameterId>(index); }
    }
    return no_blend_parameter;
}

std::optional<double> BlendParameters::value(BlendParameterId parameter) const noexcept {
    return parameter < values_.size() ? std::optional<double>{values_[parameter]} : std::nullopt;
}

const BlendParameter* BlendParameters::definition(BlendParameterId parameter) const noexcept {
    return parameter < definitions_.size() ? &definitions_[parameter] : nullptr;
}

bool BlendParameters::set_number(BlendParameterId parameter, double value) noexcept {
    const auto* item = definition(parameter);
    if (!item || item->type != BlendParameterType::Number || !bounded(value, item->minimum, item->maximum)) { return false; }
    values_[parameter] = value;
    return true;
}

bool BlendParameters::set_boolean(BlendParameterId parameter, bool value) noexcept {
    const auto* item = definition(parameter);
    if (!item || item->type != BlendParameterType::Boolean) { return false; }
    values_[parameter] = value ? 1.0 : 0.0;
    return true;
}

bool BlendParameters::trigger(BlendParameterId parameter) noexcept {
    const auto* item = definition(parameter);
    if (!item || item->type != BlendParameterType::Trigger) { return false; }
    values_[parameter] = 1.0;
    return true;
}

bool BlendParameters::reset_trigger(BlendParameterId parameter) noexcept {
    const auto* item = definition(parameter);
    if (!item || item->type != BlendParameterType::Trigger) { return false; }
    values_[parameter] = 0.0;
    return true;
}

void BlendParameters::reset() noexcept {
    for (std::size_t index = 0; index < values_.size(); ++index) { values_[index] = definitions_[index].initial; }
}
std::uint64_t BlendParameters::signature() const noexcept { return signature_; }
std::span<const BlendParameter> BlendParameters::definitions() const noexcept { return definitions_; }

BlendResult blend_poses(const Skeleton& skeleton, std::span<const WeightedPose> sources, Pose& output) {
    Pose candidate;
    const auto result = local_blend(skeleton, sources, candidate);
    if (!result) { return result; }
    const auto updated = converted(candidate.update(skeleton));
    if (updated) { output = std::move(candidate); }
    return updated;
}

BlendResult additive_pose(const Skeleton& skeleton, const Pose& base, const Pose& additive,
                          const Pose& reference, double weight, std::span<const double> mask, Pose& output) {
    Pose candidate;
    const auto result = local_additive(skeleton, base, additive, reference, weight, mask, candidate);
    if (!result) { return result; }
    const auto updated = converted(candidate.update(skeleton));
    if (updated) { output = std::move(candidate); }
    return updated;
}

BlendResult BlendTree::load(const Skeleton& skeleton, std::vector<BlendParameter> parameters,
                            std::vector<BlendNode> nodes, BlendNodeId root, BlendLimits limits) {
    if (!valid_limits(limits)) { return {BlendError::InvalidLimits}; }
    if (!skeleton.valid()) { return {BlendError::InvalidSkeleton}; }
    if (nodes.empty() || nodes.size() > limits.max_nodes || skeleton.size() > limits.max_cached_transforms) {
        return {BlendError::Capacity};
    }
    if (root >= nodes.size()) { return {BlendError::InvalidNode, root}; }
    BlendParameters defaults;
    const auto loaded = defaults.load(std::move(parameters), limits);
    if (!loaded) { return loaded; }
    std::size_t edges = 0;
    std::size_t mask_values = 0;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (!valid_name(node.name)) { return {BlendError::InvalidName, index}; }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (nodes[prior].name == node.name) { return {BlendError::DuplicateName, index}; }
        }
        if (node.inputs.size() > limits.max_inputs || node.inputs.size() > limits.max_edges - edges) {
            return {BlendError::Capacity, index};
        }
        edges += node.inputs.size();
        switch (node.kind) {
        case BlendNodeKind::BindPose:
        case BlendNodeKind::Clip:
            if (!node.inputs.empty()) { return {BlendError::InvalidNode, index}; }
            break;
        case BlendNodeKind::Additive:
            if (node.inputs.size() < 2 || node.inputs.size() > 3) { return {BlendError::InvalidNode, index}; }
            break;
        case BlendNodeKind::Weighted:
        case BlendNodeKind::Blend1D:
        case BlendNodeKind::Blend2D:
            if (node.inputs.empty()) { return {BlendError::InvalidNode, index}; }
            break;
        default: return {BlendError::InvalidNode, index};
        }
        if (!bounded(node.time_scale, -1024.0, 1024.0) || !bounded(node.time_offset, -maximum_time, maximum_time)) {
            return {BlendError::InvalidTime, index};
        }
        if ((node.wrap != WrapMode::Clamp && node.wrap != WrapMode::Loop && node.wrap != WrapMode::PingPong) ||
            (node.missing_clip != MissingClipPolicy::Fail && node.missing_clip != MissingClipPolicy::BindPose)) {
            return {BlendError::InvalidNode, index};
        }
        if (!numeric_parameter(defaults, node.parameter_x) || !numeric_parameter(defaults, node.parameter_y) ||
            !bounded(node.coordinates.x, -maximum_scalar, maximum_scalar) ||
            !bounded(node.coordinates.y, -maximum_scalar, maximum_scalar)) {
            return {BlendError::InvalidParameter, index};
        }
        for (std::size_t input_index = 0; input_index < node.inputs.size(); ++input_index) {
            const auto& input = node.inputs[input_index];
            if (input.node >= nodes.size()) { return {BlendError::InvalidNode, index}; }
            if (!bounded(input.weight, 0.0, maximum_scalar) || !valid_mask(input.mask, skeleton.size()) ||
                !numeric_parameter(defaults, input.weight_parameter)) { return {BlendError::InvalidWeight, index}; }
            if (input.mask.size() > limits.max_mask_values - mask_values) { return {BlendError::Capacity, index}; }
            mask_values += input.mask.size();
            if (!bounded(input.position.x, -maximum_scalar, maximum_scalar) ||
                !bounded(input.position.y, -maximum_scalar, maximum_scalar)) { return {BlendError::InvalidNode, index}; }
            if (node.kind == BlendNodeKind::Blend1D && input_index > 0 &&
                input.position.x - node.inputs[input_index - 1].position.x <= math::epsilon) {
                return {BlendError::InvalidNode, index};
            }
            if (node.kind == BlendNodeKind::Blend2D) {
                for (std::size_t prior = 0; prior < input_index; ++prior) {
                    if (math::length_squared(input.position - node.inputs[prior].position) <= point_tolerance_squared) {
                        return {BlendError::InvalidNode, index};
                    }
                }
            }
        }
    }
    struct Visit { BlendNodeId node; std::size_t next; };
    std::vector<std::uint8_t> colors(nodes.size());
    std::vector<std::size_t> depths(nodes.size(), 1);
    std::vector<Visit> stack;
    stack.reserve(nodes.size());
    for (std::size_t start = 0; start < nodes.size(); ++start) {
        if (colors[start] != 0) { continue; }
        stack.push_back({static_cast<BlendNodeId>(start), 0});
        colors[start] = 1;
        while (!stack.empty()) {
            auto& visit = stack.back();
            const auto& inputs = nodes[visit.node].inputs;
            if (visit.next < inputs.size()) {
                const auto child = inputs[visit.next++].node;
                if (colors[child] == 1) { return {BlendError::Cycle, child}; }
                if (colors[child] == 0) {
                    colors[child] = 1;
                    stack.push_back({child, 0});
                    if (stack.size() > limits.max_depth) { return {BlendError::DepthLimit, child}; }
                }
            } else {
                for (const auto& input : inputs) { depths[visit.node] = std::max(depths[visit.node], depths[input.node] + 1); }
                if (depths[visit.node] > limits.max_depth) { return {BlendError::DepthLimit, visit.node}; }
                colors[visit.node] = 2;
                stack.pop_back();
            }
        }
    }
    defaults_ = std::move(defaults);
    nodes_ = std::move(nodes);
    limits_ = limits;
    root_ = root;
    skeleton_signature_ = skeleton.signature();
    return {};
}

bool BlendTree::valid() const noexcept { return skeleton_signature_ != 0 && root_ < nodes_.size(); }
bool BlendTree::compatible(const Skeleton& skeleton) const noexcept {
    return valid() && skeleton.valid() && skeleton.signature() == skeleton_signature_;
}
BlendNodeId BlendTree::root() const noexcept { return root_; }
BlendNodeId BlendTree::find(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        if (nodes_[index].name == name) { return static_cast<BlendNodeId>(index); }
    }
    return no_blend_node;
}
std::span<const BlendNode> BlendTree::nodes() const noexcept { return nodes_; }
const BlendParameters& BlendTree::defaults() const noexcept { return defaults_; }
const BlendLimits& BlendTree::limits() const noexcept { return limits_; }

namespace {

class TreeEvaluator final {
public:
    TreeEvaluator(const BlendTree& tree, const Skeleton& skeleton, const ClipLibrary& clips,
                  const BlendParameters& parameters, double seconds)
        : tree_(tree), skeleton_(skeleton), clips_(clips), parameters_(parameters), seconds_(seconds),
          poses_(tree.nodes().size()), colors_(tree.nodes().size()) {}

    BlendEvaluation run(BlendNodeId root, Pose& output) {
        result_.result = evaluate(root, 1);
        if (result_.result) {
            Pose candidate = std::move(poses_[root]);
            result_.result = converted(candidate.update(skeleton_));
            if (result_.result) { output = std::move(candidate); }
        }
        return result_;
    }
private:
    double parameter(BlendParameterId id, double fallback) const noexcept {
        return id == no_blend_parameter ? fallback : parameters_.value(id).value_or(fallback);
    }

    BlendResult effective_weight(const BlendInput& input, double& weight) const noexcept {
        weight *= input.weight * parameter(input.weight_parameter, 1.0);
        return bounded(weight, 0.0, maximum_scalar) ? BlendResult{} : BlendResult{BlendError::InvalidWeight, input.node};
    }

    std::vector<double> spatial_weights(const BlendNode& node) const {
        std::vector<double> weights(node.inputs.size(), node.kind == BlendNodeKind::Weighted ? 1.0 : 0.0);
        const double x = parameter(node.parameter_x, node.coordinates.x);
        if (node.kind == BlendNodeKind::Blend1D) {
            if (x <= node.inputs.front().position.x) { weights.front() = 1.0; }
            else if (x >= node.inputs.back().position.x) { weights.back() = 1.0; }
            else {
                const auto after = std::upper_bound(node.inputs.begin(), node.inputs.end(), x,
                    [](double value, const BlendInput& input) { return value < input.position.x; });
                const auto index = static_cast<std::size_t>(after - node.inputs.begin());
                const double first = node.inputs[index - 1].position.x;
                const double alpha = (x - first) / (after->position.x - first);
                weights[index - 1] = 1.0 - alpha;
                weights[index] = alpha;
            }
        } else if (node.kind == BlendNodeKind::Blend2D) {
            const math::Vec2 point{x, parameter(node.parameter_y, node.coordinates.y)};
            std::size_t nearest = 0;
            double distance = math::length_squared(point - node.inputs.front().position);
            for (std::size_t index = 0; index < node.inputs.size(); ++index) {
                weights[index] = math::length_squared(point - node.inputs[index].position);
                if (weights[index] < distance) { nearest = index; distance = weights[index]; }
            }
            if (distance <= point_tolerance_squared) {
                std::fill(weights.begin(), weights.end(), 0.0);
                weights[nearest] = 1.0;
            } else {
                for (double& weight : weights) { weight = distance / weight; }
            }
        }
        return weights;
    }

    BlendResult evaluate(BlendNodeId id, std::size_t depth) {
        if (id >= poses_.size()) { return {BlendError::InvalidNode, id}; }
        if (colors_[id] == 2) { return {}; }
        if (colors_[id] == 1) { return {BlendError::Cycle, id}; }
        if (depth > tree_.limits().max_depth) { return {BlendError::DepthLimit, id}; }
        if (reservations_ >= tree_.limits().max_cached_transforms / skeleton_.size()) {
            return {BlendError::Capacity, id};
        }
        ++reservations_;
        colors_[id] = 1;
        const auto& node = tree_.nodes()[id];
        Pose candidate;
        if (node.kind == BlendNodeKind::BindPose) { candidate = bind_locals(skeleton_); }
        else if (node.kind == BlendNodeKind::Clip) {
            const double time = seconds_ * node.time_scale + node.time_offset;
            if (!bounded(time, -maximum_time, maximum_time)) { return {BlendError::InvalidTime, id}; }
            const auto* clip = clips_.get(node.clip);
            if (!clip || !clip->compatible(skeleton_)) {
                ++result_.missing_clips;
                if (node.missing_clip == MissingClipPolicy::Fail) {
                    return {clip ? BlendError::IncompatibleClip : BlendError::MissingClip, id};
                }
                candidate = bind_locals(skeleton_);
            } else {
                Pose sampled;
                const auto sampled_result = converted(clip->sample(skeleton_, time, sampled, node.wrap));
                if (!sampled_result) { return sampled_result; }
                candidate.local = std::move(sampled.local);
                candidate.skeleton_signature = sampled.skeleton_signature;
                ++result_.clip_samples;
            }
        } else if (node.kind == BlendNodeKind::Additive) {
            const auto base = node.inputs[0].node;
            auto status = evaluate(base, depth + 1);
            if (!status) { return status; }
            double weight = 1.0;
            status = effective_weight(node.inputs[1], weight);
            if (!status) { return status; }
            const auto& mask = node.inputs[1].mask;
            const bool empty_mask = !mask.empty() && std::all_of(mask.begin(), mask.end(), [](double value) { return value == 0.0; });
            if (weight == 0.0 || empty_mask) {
                candidate = poses_[base];
                ++result_.zero_weight_nodes;
            } else {
                status = evaluate(node.inputs[1].node, depth + 1);
                if (!status) { return status; }
                Pose reference;
                const Pose* reference_pose = nullptr;
                if (node.inputs.size() == 3) {
                    status = evaluate(node.inputs[2].node, depth + 1);
                    if (!status) { return status; }
                    reference_pose = &poses_[node.inputs[2].node];
                } else { reference = bind_locals(skeleton_); reference_pose = &reference; }
                status = local_additive(skeleton_, poses_[base], poses_[node.inputs[1].node], *reference_pose, weight, mask, candidate);
                if (!status) { return status; }
            }
        } else {
            auto weights = spatial_weights(node);
            std::vector<WeightedPose> sources;
            sources.reserve(node.inputs.size());
            for (std::size_t index = 0; index < node.inputs.size(); ++index) {
                if (weights[index] == 0.0) { continue; }
                const auto& input = node.inputs[index];
                auto status = effective_weight(input, weights[index]);
                if (!status) { return status; }
                if (weights[index] == 0.0 || (!input.mask.empty() &&
                    std::all_of(input.mask.begin(), input.mask.end(), [](double value) { return value == 0.0; }))) { continue; }
                status = evaluate(input.node, depth + 1);
                if (!status) { return status; }
                sources.push_back({&poses_[input.node], weights[index], input.mask});
            }
            if (sources.empty()) { ++result_.zero_weight_nodes; }
            const auto status = local_blend(skeleton_, sources, candidate);
            if (!status) { return status; }
        }
        poses_[id] = std::move(candidate);
        colors_[id] = 2;
        ++result_.evaluated_nodes;
        return {};
    }

    const BlendTree& tree_;
    const Skeleton& skeleton_;
    const ClipLibrary& clips_;
    const BlendParameters& parameters_;
    double seconds_;
    std::vector<Pose> poses_;
    std::vector<std::uint8_t> colors_;
    std::size_t reservations_{};
    BlendEvaluation result_;
};

} // namespace

BlendEvaluation BlendTree::evaluate(const Skeleton& skeleton, const ClipLibrary& clips,
                                   const BlendParameters& parameters, double seconds, Pose& output, BlendNodeId root) const {
    if (!compatible(skeleton)) { return {{BlendError::InvalidSkeleton}}; }
    if (parameters.signature() != defaults_.signature()) { return {{BlendError::InvalidParameter}}; }
    if (!bounded(seconds, -maximum_time, maximum_time)) { return {{BlendError::InvalidTime}}; }
    const auto selected = root == no_blend_node ? root_ : root;
    if (selected >= nodes_.size()) { return {{BlendError::InvalidNode, selected}}; }
    TreeEvaluator evaluator(*this, skeleton, clips, parameters, seconds);
    return evaluator.run(selected, output);
}

BlendResult BlendStateMachine::load(std::shared_ptr<const BlendTree> tree, std::vector<BlendState> states,
                                    std::vector<BlendTransition> transitions, BlendStateId entry, BlendLimits limits) {
    if (!valid_limits(limits)) { return {BlendError::InvalidLimits}; }
    if (!tree || !tree->valid()) { return {BlendError::InvalidNode}; }
    if (states.empty() || states.size() > limits.max_states || transitions.size() > limits.max_transitions) {
        return {BlendError::Capacity};
    }
    if (entry >= states.size()) { return {BlendError::InvalidState, entry}; }
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto& state = states[index];
        if (!valid_name(state.name)) { return {BlendError::InvalidName, index}; }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (states[prior].name == state.name) { return {BlendError::DuplicateName, index}; }
        }
        if (state.root == no_blend_node) { state.root = tree->root(); }
        if (state.root >= tree->nodes().size() || !bounded(state.speed, 0.0, 1024.0) ||
            !bounded(state.cycle_seconds, 1.0e-6, maximum_step) || !bounded(state.start_time, 0.0, maximum_time)) {
            return {BlendError::InvalidState, index};
        }
    }
    std::size_t condition_count = 0;
    for (std::size_t index = 0; index < transitions.size(); ++index) {
        const auto& transition = transitions[index];
        if ((transition.from != no_blend_state && transition.from >= states.size()) || transition.to >= states.size() ||
            !bounded(transition.duration, 0.0, maximum_step) || !bounded(transition.destination_time, 0.0, maximum_time) ||
            !bounded(states[transition.to].start_time + transition.destination_time, 0.0, maximum_time) ||
            (transition.exit_time && !bounded(*transition.exit_time, 0.0, maximum_scalar)) ||
            (transition.curve != BlendCurve::Linear && transition.curve != BlendCurve::Smoothstep)) {
            return {BlendError::InvalidTransition, index};
        }
        if (transition.conditions.size() > limits.max_conditions - condition_count) { return {BlendError::Capacity, index}; }
        condition_count += transition.conditions.size();
        for (const auto& condition : transition.conditions) {
            const auto* parameter = tree->defaults().definition(condition.parameter);
            if (!parameter || !bounded(condition.threshold, -maximum_scalar, maximum_scalar) ||
                !bounded(condition.tolerance, 0.0, maximum_scalar)) { return {BlendError::InvalidParameter, index}; }
            switch (condition.operation) {
            case BlendConditionOp::Less:
            case BlendConditionOp::LessEqual:
            case BlendConditionOp::Greater:
            case BlendConditionOp::GreaterEqual:
            case BlendConditionOp::Equal:
            case BlendConditionOp::NotEqual:
                if (parameter->type != BlendParameterType::Number) { return {BlendError::InvalidParameter, index}; }
                break;
            case BlendConditionOp::IsTrue:
            case BlendConditionOp::IsFalse:
                if (parameter->type != BlendParameterType::Boolean) { return {BlendError::InvalidParameter, index}; }
                break;
            case BlendConditionOp::Triggered:
                if (parameter->type != BlendParameterType::Trigger) { return {BlendError::InvalidParameter, index}; }
                break;
            default: return {BlendError::InvalidTransition, index};
            }
        }
    }
    std::stable_sort(transitions.begin(), transitions.end(), [](const BlendTransition& first, const BlendTransition& second) {
        return first.priority > second.priority;
    });
    tree_ = std::move(tree);
    states_ = std::move(states);
    transitions_ = std::move(transitions);
    entry_ = entry;
    return {};
}

bool BlendStateMachine::valid() const noexcept { return tree_ && tree_->valid() && entry_ < states_.size(); }
BlendStateId BlendStateMachine::entry() const noexcept { return entry_; }
BlendStateId BlendStateMachine::find(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < states_.size(); ++index) {
        if (states_[index].name == name) { return static_cast<BlendStateId>(index); }
    }
    return no_blend_state;
}
const BlendTree* BlendStateMachine::tree() const noexcept { return tree_.get(); }
std::span<const BlendState> BlendStateMachine::states() const noexcept { return states_; }
std::span<const BlendTransition> BlendStateMachine::transitions() const noexcept { return transitions_; }

BlendResult BlendPlayer::bind(std::shared_ptr<const BlendStateMachine> machine,
                             const Skeleton& skeleton, const ClipLibrary& clips) {
    if (!machine || !machine->valid()) { return {BlendError::InvalidState}; }
    BlendPlayer candidate;
    candidate.machine_ = std::move(machine);
    candidate.parameters_ = candidate.machine_->tree()->defaults();
    candidate.state_ = candidate.machine_->entry();
    const auto& state = candidate.machine_->states()[candidate.state_];
    candidate.time_ = state.start_time;
    const auto evaluated = candidate.machine_->tree()->evaluate(skeleton, clips, candidate.parameters_, candidate.time_, candidate.pose_, state.root);
    if (!evaluated.result) { return evaluated.result; }
    *this = std::move(candidate);
    return {};
}

BlendAdvance BlendPlayer::advance(const Skeleton& skeleton, const ClipLibrary& clips, double delta_seconds) {
    BlendAdvance result;
    result.from = state_;
    result.to = state_;
    result.fading = fading();
    if (!machine_ || !machine_->valid() || state_ >= machine_->states().size()) {
        result.evaluation.result = {BlendError::InvalidState};
        return result;
    }
    if (!machine_->tree()->compatible(skeleton) || !pose_.compatible(skeleton)) {
        result.evaluation.result = {BlendError::InvalidSkeleton};
        return result;
    }
    if (!bounded(delta_seconds, 0.0, maximum_step)) { result.evaluation.result = {BlendError::InvalidTime}; return result; }
    if (paused_) { return result; }
    const auto& current = machine_->states()[state_];
    double next_time = time_ + delta_seconds * current.speed;
    double next_elapsed = state_elapsed_ + delta_seconds * current.speed;
    if (!bounded(next_time, 0.0, maximum_time) || !bounded(next_elapsed, 0.0, maximum_time)) {
        result.evaluation.result = {BlendError::InvalidTime};
        return result;
    }
    const BlendTransition* selected = nullptr;
    for (const auto& transition : machine_->transitions()) {
        if ((transition.from != no_blend_state && transition.from != state_) ||
            (transition.to == state_ && !transition.allow_self) || (fading() && !transition.allow_interrupt) ||
            (transition.exit_time && next_elapsed / current.cycle_seconds < *transition.exit_time)) { continue; }
        if (std::all_of(transition.conditions.begin(), transition.conditions.end(), [this](const BlendCondition& condition) {
            return condition_met(condition, parameters_);
        })) { selected = &transition; break; }
    }
    const auto next_state = selected ? selected->to : state_;
    const auto& destination = machine_->states()[next_state];
    if (selected) { next_time = destination.start_time + selected->destination_time; next_elapsed = 0.0; }
    Pose candidate;
    result.evaluation = machine_->tree()->evaluate(skeleton, clips, parameters_, next_time, candidate, destination.root);
    if (!result.evaluation.result) { return result; }
    const double duration = selected ? selected->duration : fade_duration_;
    const double fade_elapsed = selected ? 0.0 : std::min(fade_elapsed_ + delta_seconds, duration);
    const auto curve = selected ? selected->curve : fade_curve_;
    std::optional<Pose> activated_source;
    if (selected && duration > 0.0) { activated_source = pose_; }
    const Pose* source = selected ? (activated_source ? &*activated_source : nullptr) : (fade_source_ ? &*fade_source_ : nullptr);
    const bool active_fade = source && duration > 0.0 && fade_elapsed < duration;
    if (active_fade) {
        const double alpha = curve_value(curve, fade_elapsed / duration);
        const std::array<WeightedPose, 2> poses{{{source, 1.0 - alpha, {}}, {&candidate, alpha, {}}}};
        result.evaluation.result = blend_poses(skeleton, poses, candidate);
        if (!result.evaluation.result) { return result; }
    }
    pose_ = std::move(candidate);
    state_ = next_state;
    time_ = next_time;
    state_elapsed_ = next_elapsed;
    fade_duration_ = duration;
    fade_elapsed_ = fade_elapsed;
    fade_curve_ = curve;
    if (selected) {
        fade_source_ = std::move(activated_source);
        for (const auto& condition : selected->conditions) {
            if (condition.operation == BlendConditionOp::Triggered) { (void)parameters_.reset_trigger(condition.parameter); }
        }
        result.transitioned = true;
        result.to = state_;
    }
    if (!active_fade) { fade_source_.reset(); }
    result.fading = active_fade;
    return result;
}

BlendResult BlendPlayer::change_state(const Skeleton& skeleton, const ClipLibrary& clips,
                                     BlendStateId state, double fade_seconds) {
    if (!machine_ || state >= machine_->states().size()) { return {BlendError::InvalidState, state}; }
    if (!bounded(fade_seconds, 0.0, maximum_step)) { return {BlendError::InvalidTime}; }
    const auto& target = machine_->states()[state];
    Pose candidate;
    const auto evaluated = machine_->tree()->evaluate(skeleton, clips, parameters_, target.start_time, candidate, target.root);
    if (!evaluated.result) { return evaluated.result; }
    std::optional<Pose> source;
    if (fade_seconds > 0.0) {
        if (!valid_pose(pose_, skeleton)) { return {BlendError::InvalidSkeleton}; }
        source = pose_;
        candidate = pose_;
    }
    pose_ = std::move(candidate);
    fade_source_ = std::move(source);
    state_ = state;
    time_ = target.start_time;
    state_elapsed_ = 0.0;
    fade_duration_ = fade_seconds;
    fade_elapsed_ = 0.0;
    fade_curve_ = BlendCurve::Smoothstep;
    return {};
}

BlendResult BlendPlayer::seek(const Skeleton& skeleton, const ClipLibrary& clips, double seconds) {
    if (!machine_ || state_ >= machine_->states().size()) { return {BlendError::InvalidState}; }
    if (!bounded(seconds, 0.0, maximum_time)) { return {BlendError::InvalidTime}; }
    Pose candidate;
    const auto& state = machine_->states()[state_];
    const auto evaluated = machine_->tree()->evaluate(skeleton, clips, parameters_, seconds, candidate, state.root);
    if (!evaluated.result) { return evaluated.result; }
    pose_ = std::move(candidate);
    time_ = seconds;
    state_elapsed_ = std::max(0.0, seconds - state.start_time);
    fade_source_.reset();
    fade_duration_ = 0.0;
    fade_elapsed_ = 0.0;
    return {};
}

void BlendPlayer::set_paused(bool paused) noexcept { paused_ = paused; }
bool BlendPlayer::paused() const noexcept { return paused_; }
bool BlendPlayer::fading() const noexcept { return fade_source_.has_value(); }
double BlendPlayer::time() const noexcept { return time_; }
BlendStateId BlendPlayer::state() const noexcept { return state_; }
const BlendStateMachine* BlendPlayer::machine() const noexcept { return machine_.get(); }
BlendParameters& BlendPlayer::parameters() noexcept { return parameters_; }
const BlendParameters& BlendPlayer::parameters() const noexcept { return parameters_; }
const Pose& BlendPlayer::pose() const noexcept { return pose_; }

bool blend_owns_pose(const ecs::Registry& registry, ecs::Entity entity) noexcept {
    const auto* blend = registry.get<BlendComponent>(entity);
    const auto* animated = registry.get<AnimatedComponent>(entity);
    return blend && animated && blend->authority_owner == animated &&
        blend->authority_skeleton == animated->skeleton && blend->player.machine();
}

Pose* composed_pose(ecs::Registry& registry, ecs::Entity entity) noexcept {
    auto* animated = registry.get<AnimatedComponent>(entity);
    return animated ? &animated->animator.pose() : nullptr;
}

const Pose* composed_pose(const ecs::Registry& registry, ecs::Entity entity) noexcept {
    const auto* animated = registry.get<AnimatedComponent>(entity);
    return animated ? &animated->animator.pose() : nullptr;
}

BlendService::BlendService(BlendLimits limits) : limits_(limits) {
    if (!valid_limits(limits)) { throw std::invalid_argument("invalid blend service limits"); }
}

BlendResult BlendService::publish(std::string name, std::shared_ptr<const BlendStateMachine> machine) {
    if (!valid_name(name)) { return {BlendError::InvalidName}; }
    if (!machine || !machine->valid()) { return {BlendError::InvalidState}; }
    if (!machines_.contains(name) && machines_.size() >= limits_.max_resources) { return {BlendError::Capacity}; }
    machines_.insert_or_assign(std::move(name), std::move(machine));
    return {};
}

std::shared_ptr<const BlendStateMachine> BlendService::find(std::string_view name) const {
    const auto found = machines_.find(name);
    return found == machines_.end() ? nullptr : found->second;
}

bool BlendService::erase(std::string_view name) {
    const auto found = machines_.find(name);
    if (found == machines_.end()) { return false; }
    machines_.erase(found);
    return true;
}

std::vector<std::string> BlendService::names() const {
    std::vector<std::string> result;
    result.reserve(machines_.size());
    for (const auto& [name, machine] : machines_) { (void)machine; result.push_back(name); }
    return result;
}

BlendResult BlendService::attach(ecs::Registry& registry, ecs::Entity entity,
                                AnimationService& animation, std::string_view name) {
    if (registry.iterating()) { return {BlendError::RegistryBusy}; }
    auto* animated = registry.get<AnimatedComponent>(entity);
    if (!animated) { return {BlendError::InvalidBinding}; }
    const auto* skeleton = animation.skeletons.get(animated->skeleton);
    if (!skeleton) { return {BlendError::InvalidSkeleton}; }
    const auto* previous = registry.get<BlendComponent>(entity);
    if (!previous && registry.view<BlendComponent>(true).size() >= limits_.max_entities) { return {BlendError::Capacity}; }
    BlendComponent candidate;
    const auto bound = candidate.player.bind(find(name), *skeleton, animation.clips);
    if (!bound) { return bound; }
    candidate.authority_owner = animated;
    candidate.authority_skeleton = animated->skeleton;
    candidate.previous_animated_enabled = blend_owns_pose(registry, entity) ? previous->previous_animated_enabled : animated->enabled;
    if (previous) { candidate.write_scene = previous->write_scene; }
    Pose pose = candidate.player.pose();
    registry.emplace_or_replace<BlendComponent>(entity, std::move(candidate));
    animated->enabled = false;
    animated->animator.pose() = std::move(pose);
    (void)registry.mark_changed<AnimatedComponent>(entity);
    return {};
}

BlendResult BlendService::detach(ecs::Registry& registry, ecs::Entity entity) {
    if (registry.iterating()) { return {BlendError::RegistryBusy}; }
    const auto* component = registry.get<BlendComponent>(entity);
    if (!component) { return {BlendError::InvalidBinding}; }
    auto* animated = registry.get<AnimatedComponent>(entity);
    const bool restore = blend_owns_pose(registry, entity) && !animated->enabled;
    const bool previous_enabled = component->previous_animated_enabled;
    if (!registry.remove<BlendComponent>(entity)) { return {BlendError::InvalidBinding}; }
    if (restore) {
        animated->enabled = previous_enabled;
        (void)registry.mark_changed<AnimatedComponent>(entity);
    }
    return {};
}

BlendServiceUpdate BlendService::update(ecs::Registry& registry, AnimationService& animation,
                                       scene::Scene* scene_graph, double delta_seconds) {
    BlendServiceUpdate result;
    if (registry.iterating() || !bounded(delta_seconds, 0.0, maximum_step) ||
        (scene_graph && &scene_graph->registry() != &registry)) {
        result.errors = 1;
        return result;
    }
    const auto entities = registry.view<BlendComponent>(true);
    std::size_t processed = 0;
    for (const auto entity : entities) {
        if (processed++ >= limits_.max_entities) { ++result.skipped; ++result.errors; continue; }
        auto& component = registry.require<BlendComponent>(entity);
        if (!blend_owns_pose(registry, entity)) { ++result.errors; continue; }
        auto& animated = registry.require<AnimatedComponent>(entity);
        if (animated.enabled) {
            animated.enabled = false;
            ++result.ownership_reassertions;
            (void)registry.mark_changed<AnimatedComponent>(entity);
        }
        if (!registry.enabled(entity) || (scene_graph && scene_graph->contains(entity) && !scene_graph->effective_enabled(entity))) {
            ++result.skipped;
            continue;
        }
        const auto* skeleton = animation.skeletons.get(animated.skeleton);
        if (!skeleton) { ++result.errors; continue; }
        std::vector<scene::LocalTransformEdit> edits;
        if (component.write_scene && !animated.bone_entities.empty()) {
            if (!scene_graph || animated.bone_entities.size() != skeleton->size()) { ++result.errors; continue; }
            auto unique = animated.bone_entities;
            std::sort(unique.begin(), unique.end());
            bool valid = std::adjacent_find(unique.begin(), unique.end()) == unique.end();
            for (std::size_t bone = 0; valid && bone < skeleton->size(); ++bone) {
                const auto parent = skeleton->bones()[bone].parent;
                const auto child = animated.bone_entities[bone];
                valid = scene_graph->contains(child) &&
                    (parent == no_bone || scene_graph->parent(child) == animated.bone_entities[parent]);
            }
            if (!valid) { ++result.errors; continue; }
            edits.reserve(skeleton->size());
        }
        const auto update = component.player.advance(*skeleton, animation.clips, delta_seconds);
        result.evaluated_nodes += update.evaluation.evaluated_nodes;
        result.missing_clips += update.evaluation.missing_clips;
        if (!update.evaluation.result) { ++result.errors; continue; }
        Pose pose = component.player.pose();
        if (component.write_scene && !animated.bone_entities.empty()) {
            for (std::size_t bone = 0; bone < skeleton->size(); ++bone) {
                edits.push_back({animated.bone_entities[bone], pose.local[bone]});
            }
            if (!scene_graph->apply_local_transforms(edits)) { ++result.errors; continue; }
        }
        animated.animator.pose() = std::move(pose);
        if (update.transitioned) {
            const auto& state = component.player.machine()->states()[update.to];
            result.transitions.push_back({entity, update.from, update.to, state.name});
        }
        (void)registry.mark_changed<AnimatedComponent>(entity);
        (void)registry.mark_changed<BlendComponent>(entity);
        ++result.animated;
    }
    return result;
}

const BlendLimits& BlendService::limits() const noexcept { return limits_; }

void register_blend(core::Kernel& kernel) {
    if (kernel.services().find<BlendService>()) { return; }
    auto& service = kernel.services().emplace<BlendService>();
    kernel.register_module({"animation.blend", "Bounded skeletal blend trees and explicit state machines", 1});
    if (auto* registry = kernel.services().find<ecs::Registry>()) { registry->register_component<BlendComponent>("animation.blend"); }
    kernel.add_system({"animation.blend", core::Phase::Animation, -10, {}}, [](core::Kernel& active, const core::Frame& frame) {
        auto* registry = active.services().find<ecs::Registry>();
        auto* animation = active.services().find<AnimationService>();
        if (!registry || !animation) { return; }
        auto update = active.services().get<BlendService>().update(*registry, *animation, active.services().find<scene::Scene>(), frame.delta_seconds);
        for (auto& transition : update.transitions) { (void)active.events().publish<BlendStateEvent>(std::move(transition)); }
        active.metric("animation.blend.animated", static_cast<double>(update.animated));
        active.metric("animation.blend.errors", static_cast<double>(update.errors));
        active.metric("animation.blend.nodes", static_cast<double>(update.evaluated_nodes));
        active.metric("animation.blend.missing_clips", static_cast<double>(update.missing_clips));
        active.metric("animation.blend.ownership_reassertions", static_cast<double>(update.ownership_reassertions));
    });
    kernel.add_command("animation.blend.create", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* animation = active.services().find<AnimationService>();
        if (!animation || args.size() != 4) { return core::CommandResult{false, "usage: animation.blend.create name skeleton first_clip second_clip"}; }
        const auto* skeleton = animation->skeletons.get(animation->skeletons.find(args[1]));
        const auto first_clip = animation->clips.find(args[2]);
        const auto second_clip = animation->clips.find(args[3]);
        if (!skeleton || !first_clip || !second_clip || !animation->clips.get(first_clip)->compatible(*skeleton) ||
            !animation->clips.get(second_clip)->compatible(*skeleton)) { return core::CommandResult{false, "compatible skeleton and clips required"}; }
        BlendNode first;
        first.name = "first"; first.kind = BlendNodeKind::Clip; first.clip = first_clip;
        BlendNode second;
        second.name = "second"; second.kind = BlendNodeKind::Clip; second.clip = second_clip;
        BlendNode mixed;
        mixed.name = "mixed"; mixed.kind = BlendNodeKind::Blend1D; mixed.parameter_x = 0;
        mixed.inputs = {{0, 1.0, no_blend_parameter, {0.0, 0.0}, {}}, {1, 1.0, no_blend_parameter, {1.0, 0.0}, {}}};
        auto tree = std::make_shared<BlendTree>();
        auto status = tree->load(*skeleton, {{"mix", BlendParameterType::Number, 0.0, 0.0, 1.0},
            {"locked", BlendParameterType::Boolean, 0.0, 0.0, 1.0}}, {std::move(first), std::move(second), std::move(mixed)}, 2, owner->limits());
        if (!status) { return command_result(status); }
        BlendTransition lock;
        lock.from = 0; lock.to = 1; lock.allow_interrupt = true;
        lock.conditions = {{1, BlendConditionOp::IsTrue, 0.0, 0.0}};
        BlendTransition unlock = lock;
        unlock.from = 1; unlock.to = 0; unlock.conditions.front().operation = BlendConditionOp::IsFalse;
        auto machine = std::make_shared<BlendStateMachine>();
        status = machine->load(std::move(tree), {{"blend", 2}, {"hold", 1}}, {std::move(lock), std::move(unlock)}, 0, owner->limits());
        return status ? command_result(owner->publish(args[0], std::move(machine))) : command_result(status);
    }, "Create a two-clip 1D graph with mix and locked parameters and blend/hold states");
    kernel.add_command("animation.blend.attach", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        auto* animation = active.services().find<AnimationService>();
        if (!registry || !animation || args.size() != 2) { return core::CommandResult{false, "usage: animation.blend.attach entity machine"}; }
        return command_result(owner->attach(*registry, registry->find_name(args[0]), *animation, args[1]));
    }, "Acquire pose authority on an entity with an AnimatedComponent");
    kernel.add_command("animation.blend.detach", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 1) { return core::CommandResult{false, "usage: animation.blend.detach entity"}; }
        return command_result(owner->detach(*registry, registry->find_name(args[0])));
    }, "Release graph authority and restore prior base animation enablement");
    kernel.add_command("animation.blend.param", [](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 3) { return core::CommandResult{false, "usage: animation.blend.param entity parameter number|true|false|fire|reset"}; }
        const auto entity = registry->find_name(args[0]);
        auto* component = registry->get<BlendComponent>(entity);
        if (!component) { return core::CommandResult{false, "entity has no blend player"}; }
        auto& parameters = component->player.parameters();
        const auto id = parameters.find(args[1]);
        const auto* definition = parameters.definition(id);
        bool success = false;
        double value{};
        if (definition && definition->type == BlendParameterType::Number) { success = number(args[2], value) && parameters.set_number(id, value); }
        else if (definition && definition->type == BlendParameterType::Boolean && (args[2] == "true" || args[2] == "false")) {
            success = parameters.set_boolean(id, args[2] == "true");
        } else if (definition && definition->type == BlendParameterType::Trigger) {
            success = args[2] == "fire" ? parameters.trigger(id) : (args[2] == "reset" && parameters.reset_trigger(id));
        }
        if (success) { (void)registry->mark_changed<BlendComponent>(entity); }
        return core::CommandResult{success, success ? "parameter changed" : "parameter type or value rejected"};
    }, "Set typed blend parameters or latch/reset a transition trigger");
    kernel.add_command("animation.blend.state", [](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        auto* animation = active.services().find<AnimationService>();
        if (!registry || !animation || args.size() < 2 || args.size() > 3) { return core::CommandResult{false, "usage: animation.blend.state entity state [fade_seconds]"}; }
        const auto entity = registry->find_name(args[0]);
        auto* component = registry->get<BlendComponent>(entity);
        const auto* animated = registry->get<AnimatedComponent>(entity);
        double fade = 0.2;
        if (!component || !animated || !blend_owns_pose(*registry, entity) || (args.size() == 3 && !number(args[2], fade))) {
            return core::CommandResult{false, "invalid entity authority or fade duration"};
        }
        const auto* skeleton = animation->skeletons.get(animated->skeleton);
        if (!skeleton) { return core::CommandResult{false, "skeleton unavailable"}; }
        const auto previous = component->player.state();
        const auto state = component->player.machine()->find(args[1]);
        const auto result = component->player.change_state(*skeleton, animation->clips, state, fade);
        if (result) {
            (void)registry->mark_changed<BlendComponent>(entity);
            (void)active.events().publish<BlendStateEvent>({entity, previous, state, args[1]});
        }
        return command_result(result);
    }, "Select an explicit state, optionally fading from the displayed graph pose");
    kernel.add_command("animation.blend.pause", [](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 2 || (args[1] != "true" && args[1] != "false")) {
            return core::CommandResult{false, "usage: animation.blend.pause entity true|false"};
        }
        const auto entity = registry->find_name(args[0]);
        auto* component = registry->get<BlendComponent>(entity);
        if (!component) { return core::CommandResult{false, "entity has no blend player"}; }
        component->player.set_paused(args[1] == "true");
        (void)registry->mark_changed<BlendComponent>(entity);
        return core::CommandResult{true, "pause changed; graph retains pose authority"};
    }, "Freeze or resume graph time and transitions while retaining pose authority");
    kernel.add_command("animation.blend.list", [owner = &service](core::Kernel&, core::CommandArgs args) {
        if (!args.empty()) { return core::CommandResult{false, "usage: animation.blend.list"}; }
        std::ostringstream message;
        for (const auto& name : owner->names()) {
            const auto machine = owner->find(name);
            message << name << " states=" << machine->states().size() << " nodes=" << machine->tree()->nodes().size() << '\n';
        }
        return core::CommandResult{true, message.str()};
    }, "List published immutable blend state machines");
}

} // namespace ge::animation
