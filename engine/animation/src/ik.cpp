#include <ge/animation/ik.hpp>

#include <ge/animation/blend.hpp>
#include <ge/core/runtime.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ge::animation {
namespace {

constexpr double minimum_length = 1.0e-8;
constexpr double maximum_scale = 1.0e6;
constexpr double absolute_coordinate_limit = 1.0e12;
constexpr std::size_t maximum_name = 128;

bool bounded(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool valid_limits(const IKLimits& value) noexcept {
    return value.max_constraints > 0 && value.max_constraints <= 256 &&
        value.max_chain_bones >= 3 && value.max_chain_bones <= 128 &&
        value.max_rotation_limits > 0 && value.max_rotation_limits <= 1024 &&
        value.max_pose_bones > 0 && value.max_pose_bones <= 4096 &&
        value.max_resources > 0 && value.max_resources <= 1024 &&
        value.max_entities > 0 && value.max_entities <= 4096 &&
        value.max_operations_per_solve > 0 && value.max_operations_per_solve <= 16777216 &&
        value.max_operations_per_frame > 0 && value.max_operations_per_frame <= 67108864 &&
        value.max_iterations > 0 && value.max_iterations <= 128 &&
        bounded(value.coordinate_limit, 1.0, absolute_coordinate_limit);
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > maximum_name) { return false; }
    return std::all_of(name.begin(), name.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 32 && byte != 127;
    });
}

bool bounded_vector(math::Vec3 value, double limit) noexcept {
    return math::finite(value) && std::abs(value.x) <= limit &&
        std::abs(value.y) <= limit && std::abs(value.z) <= limit;
}

bool uniform_scale(math::Vec3 value) noexcept {
    if (!bounded(value.x, minimum_length, maximum_scale)) { return false; }
    const double tolerance = 1.0e-8 * value.x;
    return math::finite(value) && std::abs(value.y - value.x) <= tolerance &&
        std::abs(value.z - value.x) <= tolerance;
}

bool direction_space(IKTargetSpace space) noexcept {
    return space == IKTargetSpace::ModelDirection || space == IKTargetSpace::WorldDirection ||
        space == IKTargetSpace::EntityDirection;
}

bool valid_target(const IKTarget& target, double limit) noexcept {
    const bool entity_space = target.space == IKTargetSpace::EntityPoint ||
        target.space == IKTargetSpace::EntityDirection;
    switch (target.space) {
    case IKTargetSpace::ModelPoint:
    case IKTargetSpace::AbsolutePoint:
    case IKTargetSpace::EntityPoint:
    case IKTargetSpace::ModelDirection:
    case IKTargetSpace::WorldDirection:
    case IKTargetSpace::EntityDirection: break;
    default: return false;
    }
    const double bound = target.space == IKTargetSpace::AbsolutePoint ? absolute_coordinate_limit : limit;
    return bounded_vector(target.value, bound) && entity_space == static_cast<bool>(target.entity) &&
        (!direction_space(target.space) || math::length_squared(target.value) > minimum_length * minimum_length);
}

bool valid_rotation_limit(const IKRotationLimit& limit, std::size_t bones) noexcept {
    return limit.bone < bones && math::finite(limit.reference) &&
        bounded(math::length(limit.reference), minimum_length, maximum_scale) &&
        bounded_vector(limit.twist_axis, maximum_scale) &&
        math::length_squared(limit.twist_axis) > minimum_length * minimum_length &&
        bounded(limit.minimum_twist, -math::pi, math::pi) &&
        bounded(limit.maximum_twist, limit.minimum_twist, math::pi) &&
        bounded(limit.maximum_swing, 0.0, math::pi);
}

math::Quat constrained_rotation(math::Quat rotation, const IKRotationLimit& limit) noexcept {
    const auto reference = math::normalized(limit.reference);
    auto relative = math::normalized(math::conjugate(reference) * rotation);
    if (relative.w < 0.0) { relative = -relative; }
    const auto axis = math::normalized(limit.twist_axis);
    const double projection = math::dot(math::Vec3{relative.x, relative.y, relative.z}, axis);
    auto twist = math::normalized(math::Quat{axis.x * projection, axis.y * projection,
        axis.z * projection, relative.w});
    double angle = 2.0 * std::atan2(math::dot(math::Vec3{twist.x, twist.y, twist.z}, axis), twist.w);
    angle = std::clamp(angle, limit.minimum_twist, limit.maximum_twist);
    auto swing = math::normalized(relative * math::conjugate(twist));
    const double swing_angle = math::angular_distance({}, swing);
    if (swing_angle > limit.maximum_swing && swing_angle > minimum_length) {
        swing = math::slerp({}, swing, limit.maximum_swing / swing_angle);
    }
    twist = math::from_axis_angle(axis, angle);
    return math::normalized(reference * swing * twist);
}

struct Solver {
    const Skeleton& skeleton;
    const IKLimits& limits;
    Pose pose;
    std::vector<math::Quat> rotations;
    std::size_t budget{};
    IKSolveResult report;

    bool charge(std::size_t count) {
        if (count > budget - report.operations) {
            report.result = {IKError::BudgetExceeded};
            return false;
        }
        report.operations += count;
        return true;
    }

    bool refresh() {
        if (!charge(skeleton.size() * 2)) { return false; }
        if (!pose.update(skeleton)) {
            report.result = {IKError::NonFiniteResult};
            return false;
        }
        for (const auto bone : skeleton.evaluation_order()) {
            const auto parent = skeleton.bones()[bone].parent;
            const auto local = math::normalized(pose.local[bone].rotation);
            rotations[bone] = parent == no_bone ? local : math::normalized(rotations[parent] * local);
            const auto global = math::decompose(pose.global[bone]);
            if (!global || !uniform_scale(global->scale) ||
                !bounded_vector(global->position, limits.coordinate_limit)) {
                report.result = {IKError::UnsupportedScale, bone};
                return false;
            }
        }
        return true;
    }

    math::Vec3 position(BoneIndex bone) const {
        const auto& matrix = pose.global[bone];
        return {matrix(0, 3), matrix(1, 3), matrix(2, 3)};
    }

    void apply_limit(const IKRotationLimit& limit) {
        auto& rotation = pose.local[limit.bone].rotation;
        const auto limited = constrained_rotation(rotation, limit);
        if (math::angular_distance(rotation, limited) > 1.0e-9) { ++report.limited_joints; }
        rotation = limited;
    }

    void limit_bone(BoneIndex bone, const IKConstraint& constraint) {
        for (const auto& limit : constraint.rotation_limits) {
            if (limit.bone != bone) { continue; }
            apply_limit(limit);
        }
    }

    bool turn(BoneIndex bone, math::Vec3 from, math::Vec3 to, const IKConstraint& constraint) {
        if (!charge(constraint.rotation_limits.size() + 1)) { return false; }
        if (math::length_squared(from) <= minimum_length * minimum_length ||
            math::length_squared(to) <= minimum_length * minimum_length) { return true; }
        auto delta = math::from_to(from, to);
        const double angle = math::angular_distance({}, delta);
        if (angle > constraint.maximum_step) {
            delta = math::slerp({}, delta, constraint.maximum_step / angle);
        }
        const auto parent = skeleton.bones()[bone].parent;
        const auto parent_rotation = parent == no_bone ? math::Quat{} : rotations[parent];
        pose.local[bone].rotation = math::normalized(math::conjugate(parent_rotation) * delta * rotations[bone]);
        limit_bone(bone, constraint);
        return refresh();
    }

    double residual(const IKConstraint& constraint, const IKResolvedTarget& target) const {
        if (constraint.kind == IKConstraintKind::RotationLimit) {
            double maximum = 0.0;
            for (const auto& limit : constraint.rotation_limits) {
                const auto rotation = pose.local[limit.bone].rotation;
                maximum = std::max(maximum, math::angular_distance(rotation, constrained_rotation(rotation, limit)));
            }
            return maximum;
        }
        const auto tip = constraint.chain.back();
        if (constraint.kind == IKConstraintKind::LookAt) {
            const auto direction = target.direction ? target.target : target.target - position(tip);
            if (math::length_squared(direction) <= minimum_length * minimum_length) { return 0.0; }
            return math::angle_between(rotations[tip] * constraint.local_axis, direction);
        }
        return math::distance(position(tip), target.target);
    }

    bool two_bone(const IKConstraint& constraint, const IKResolvedTarget& target, IKConstraintStatus& status) {
        const auto root = constraint.chain[0];
        const auto middle = constraint.chain[1];
        const auto tip = constraint.chain[2];
        const auto origin = position(root);
        const auto upper = position(middle) - origin;
        const auto lower = position(tip) - position(middle);
        const double first_length = math::length(upper);
        const double second_length = math::length(lower);
        if (first_length <= minimum_length || second_length <= minimum_length) {
            report.result = {IKError::DegenerateChain};
            return false;
        }
        const auto offset = target.target - origin;
        const auto direction = math::normalized(offset, math::normalized(position(tip) - origin, math::normalized(upper)));
        const double minimum = std::max(minimum_length, std::abs(first_length - second_length));
        const double distance = std::clamp(math::length(offset), minimum, first_length + second_length);
        auto bend = math::reject(target.has_pole ? target.pole - origin : upper, direction);
        if (math::length_squared(bend) <= minimum_length * minimum_length) {
            bend = math::orthonormal_basis(direction).first;
        }
        bend = math::normalized(bend);
        const double adjacent = std::clamp((first_length * first_length + distance * distance -
            second_length * second_length) / (2.0 * distance), -first_length, first_length);
        const double height = std::sqrt(std::max(0.0, first_length * first_length - adjacent * adjacent));
        const auto elbow = origin + direction * adjacent + bend * height;
        const auto reachable = origin + direction * distance;
        if (!turn(root, upper, elbow - origin, constraint)) { return false; }
        if (!turn(middle, position(tip) - position(middle), reachable - position(middle), constraint)) { return false; }
        status.iterations = 1;
        return true;
    }

    bool ccd(const IKConstraint& constraint, const IKResolvedTarget& target, IKConstraintStatus& status) {
        const auto tip = constraint.chain.back();
        for (std::uint32_t iteration = 0; iteration < constraint.iterations; ++iteration) {
            if (residual(constraint, target) <= constraint.tolerance) { break; }
            for (std::size_t index = constraint.chain.size() - 1; index > 0; --index) {
                const auto bone = constraint.chain[index - 1];
                const auto origin = position(bone);
                if (!turn(bone, position(tip) - origin, target.target - origin, constraint)) { return false; }
                if (residual(constraint, target) <= constraint.tolerance) { break; }
            }
            ++status.iterations;
        }
        return true;
    }

    bool apply(const IKConstraint& constraint, const IKResolvedTarget& target, IKConstraintStatus& status) {
        switch (constraint.kind) {
        case IKConstraintKind::TwoBone: return two_bone(constraint, target, status);
        case IKConstraintKind::CCD: return ccd(constraint, target, status);
        case IKConstraintKind::LookAt: {
            const auto bone = constraint.chain.front();
            const auto direction = target.direction ? target.target : target.target - position(bone);
            status.iterations = 1;
            return turn(bone, rotations[bone] * constraint.local_axis, direction, constraint);
        }
        case IKConstraintKind::RotationLimit:
            if (!charge(constraint.rotation_limits.size())) { return false; }
            for (const auto& limit : constraint.rotation_limits) { apply_limit(limit); }
            status.iterations = 1;
            return refresh();
        }
        report.result = {IKError::InvalidConstraint};
        return false;
    }
};

IKResult validate_binding(const ecs::Registry& registry, scene::Scene* scene_graph,
                          const Skeleton& skeleton, std::span<const ecs::Entity> bones,
                          const IKBindingOptions& options) {
    if (scene_graph && &scene_graph->registry() != &registry) { return {IKError::InvalidAttachment}; }
    if (options.model_entity && (!scene_graph || !scene_graph->contains(options.model_entity))) {
        return {IKError::InvalidEntity};
    }
    if (bones.empty() || !options.write_scene) { return {}; }
    if (!scene_graph || bones.size() != skeleton.size()) { return {IKError::InvalidAttachment}; }
    std::vector<ecs::Entity> unique(bones.begin(), bones.end());
    std::sort(unique.begin(), unique.end());
    if (std::adjacent_find(unique.begin(), unique.end()) != unique.end()) { return {IKError::InvalidAttachment}; }
    for (std::size_t index = 0; index < bones.size(); ++index) {
        const auto bone = bones[index];
        const auto parent = skeleton.bones()[index].parent;
        const auto expected = parent == no_bone ? options.model_entity : bones[parent];
        if (!scene_graph->contains(bone) || scene_graph->parent(bone) != expected) {
            return {IKError::InvalidAttachment, index};
        }
        if (registry.has<physics::BodyComponent>(bone)) { return {IKError::PhysicsOwned, index}; }
    }
    return {};
}

std::optional<math::Mat4> model_inverse(scene::Scene* scene_graph, ecs::Entity model) {
    if (!model) { return math::Mat4{}; }
    if (!scene_graph) { return std::nullopt; }
    const auto matrix = scene_graph->world_matrix(model);
    if (!matrix) { return std::nullopt; }
    const auto transform = math::decompose(*matrix);
    if (!transform || !uniform_scale(transform->scale)) { return std::nullopt; }
    return math::inverse(*matrix);
}

IKResult resolve_target(const IKTarget& target, scene::Scene* scene_graph, math::Vec3 absolute_origin,
                        const math::Mat4& world_to_model, double coordinate_limit,
                        math::Vec3& output, bool& direction) {
    if (!valid_target(target, coordinate_limit)) { return {IKError::InvalidTarget}; }
    direction = direction_space(target.space);
    auto value = target.value;
    switch (target.space) {
    case IKTargetSpace::ModelPoint:
    case IKTargetSpace::ModelDirection: break;
    case IKTargetSpace::AbsolutePoint:
        value = math::transform_point(world_to_model, value - absolute_origin);
        break;
    case IKTargetSpace::WorldDirection:
        value = math::transform_vector(world_to_model, value);
        break;
    case IKTargetSpace::EntityPoint:
    case IKTargetSpace::EntityDirection: {
        if (!scene_graph || !scene_graph->contains(target.entity)) { return {IKError::MissingTarget}; }
        const auto world = scene_graph->world_matrix(target.entity);
        if (!world) { return {IKError::MissingTarget}; }
        value = direction ? math::transform_vector(world_to_model, math::transform_vector(*world, value)) :
            math::transform_point(world_to_model, math::transform_point(*world, value));
        break;
    }
    }
    if (!bounded_vector(value, coordinate_limit)) { return {IKError::InvalidTarget}; }
    if (direction && !math::normalize(value)) { return {IKError::InvalidTarget}; }
    output = value;
    return {};
}

std::optional<double> parse_number(std::string_view text) {
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) { return std::nullopt; }
    return value;
}

std::optional<math::Vec3> parse_vector(core::CommandArgs args, std::size_t offset) {
    if (offset > args.size() || args.size() - offset < 3) { return std::nullopt; }
    const auto x = parse_number(args[offset]);
    const auto y = parse_number(args[offset + 1]);
    const auto z = parse_number(args[offset + 2]);
    if (!x || !y || !z) { return std::nullopt; }
    return math::Vec3{*x, *y, *z};
}

core::CommandResult command_result(IKResult result) {
    return {static_cast<bool>(result), std::string(describe(result.error)) + " element=" + std::to_string(result.element)};
}

ecs::Entity named_entity(ecs::Registry& registry, std::string_view name) {
    const auto matches = registry.find_all_named(name);
    return matches.size() == 1 ? matches.front() : ecs::Entity{};
}

} // namespace

std::string_view describe(IKError error) noexcept {
    switch (error) {
    case IKError::None: return "ok";
    case IKError::InvalidLimits: return "invalid IK limits";
    case IKError::Capacity: return "IK capacity exhausted";
    case IKError::InvalidName: return "invalid or duplicate name";
    case IKError::InvalidSkeleton: return "incompatible skeleton";
    case IKError::InvalidPose: return "invalid pose";
    case IKError::InvalidBone: return "invalid bone";
    case IKError::InvalidChain: return "chain must contain direct parent links";
    case IKError::InvalidTarget: return "invalid target coordinates";
    case IKError::InvalidWeight: return "weight must be between zero and one";
    case IKError::InvalidConstraint: return "invalid constraint";
    case IKError::UnsupportedScale: return "requires positive uniform scale without shear";
    case IKError::DegenerateChain: return "chain has a zero length segment";
    case IKError::MissingTarget: return "target entity unavailable";
    case IKError::InvalidEntity: return "entity unavailable or ambiguous";
    case IKError::InvalidAttachment: return "attachment no longer matches pose ownership";
    case IKError::StaleRevision: return "stale binding or resource revision";
    case IKError::PhysicsOwned: return "physics owns a bound bone transform";
    case IKError::SceneRejected: return "scene rejected staged bone edits";
    case IKError::BudgetExceeded: return "IK operation budget exhausted";
    case IKError::NonFiniteResult: return "nonfinite pose result";
    case IKError::Busy: return "registry is iterating";
    }
    return "unknown IK error";
}

IKResult IKRig::load(const Skeleton& skeleton, std::vector<IKConstraint> constraints, IKLimits limits) {
    if (!valid_limits(limits)) { return {IKError::InvalidLimits}; }
    if (!skeleton.valid() || skeleton.size() > limits.max_pose_bones) { return {IKError::InvalidSkeleton}; }
    if (constraints.empty() || constraints.size() > limits.max_constraints ||
        revision_ == std::numeric_limits<std::uint64_t>::max()) { return {IKError::Capacity}; }
    std::size_t limit_count = 0;
    for (std::size_t index = 0; index < constraints.size(); ++index) {
        auto& constraint = constraints[index];
        if (!valid_name(constraint.name)) { return {IKError::InvalidName, index}; }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (constraints[previous].name == constraint.name) { return {IKError::InvalidName, index}; }
        }
        if (!bounded(constraint.weight, 0.0, 1.0)) { return {IKError::InvalidWeight, index}; }
        if (!bounded(constraint.tolerance, minimum_length, limits.coordinate_limit) ||
            !bounded(constraint.maximum_step, minimum_length, math::pi) ||
            constraint.iterations == 0 || constraint.iterations > limits.max_iterations) {
            return {IKError::InvalidConstraint, index};
        }
        if (constraint.chain.size() > limits.max_chain_bones) { return {IKError::Capacity, index}; }
        switch (constraint.kind) {
        case IKConstraintKind::TwoBone:
            if (constraint.chain.size() != 3) { return {IKError::InvalidChain, index}; }
            break;
        case IKConstraintKind::CCD:
            if (constraint.chain.size() < 2 || constraint.pole) { return {IKError::InvalidChain, index}; }
            break;
        case IKConstraintKind::LookAt:
            if (constraint.chain.size() != 1 || constraint.pole ||
                !bounded_vector(constraint.local_axis, maximum_scale) || !math::normalize(constraint.local_axis)) {
                return {IKError::InvalidConstraint, index};
            }
            break;
        case IKConstraintKind::RotationLimit:
            if (!constraint.chain.empty() || constraint.rotation_limits.empty() || constraint.pole) {
                return {IKError::InvalidConstraint, index};
            }
            break;
        default: return {IKError::InvalidConstraint, index};
        }
        for (std::size_t link = 0; link < constraint.chain.size(); ++link) {
            const auto bone = constraint.chain[link];
            if (bone >= skeleton.size()) { return {IKError::InvalidBone, index}; }
            if (link > 0 && skeleton.bones()[bone].parent != constraint.chain[link - 1]) {
                return {IKError::InvalidChain, index};
            }
        }
        if (constraint.kind != IKConstraintKind::RotationLimit &&
            (!valid_target(constraint.target, limits.coordinate_limit) ||
             (constraint.kind != IKConstraintKind::LookAt && direction_space(constraint.target.space)))) {
            return {IKError::InvalidTarget, index};
        }
        if (constraint.pole && (!valid_target(*constraint.pole, limits.coordinate_limit) || direction_space(constraint.pole->space))) {
            return {IKError::InvalidTarget, index};
        }
        if (constraint.rotation_limits.size() > limits.max_rotation_limits - limit_count) { return {IKError::Capacity, index}; }
        limit_count += constraint.rotation_limits.size();
        for (std::size_t entry = 0; entry < constraint.rotation_limits.size(); ++entry) {
            auto& limit = constraint.rotation_limits[entry];
            if (!valid_rotation_limit(limit, skeleton.size())) { return {IKError::InvalidConstraint, index}; }
            if (constraint.kind != IKConstraintKind::RotationLimit &&
                std::find(constraint.chain.begin(), constraint.chain.end(), limit.bone) == constraint.chain.end()) {
                return {IKError::InvalidBone, index};
            }
            for (std::size_t previous = 0; previous < entry; ++previous) {
                if (constraint.rotation_limits[previous].bone == limit.bone) { return {IKError::InvalidConstraint, index}; }
            }
            limit.reference = math::normalized(limit.reference);
            limit.twist_axis = math::normalized(limit.twist_axis);
        }
    }
    constraints_ = std::move(constraints);
    limits_ = limits;
    skeleton_signature_ = skeleton.signature();
    ++revision_;
    return {};
}

bool IKRig::valid() const noexcept { return skeleton_signature_ != 0 && revision_ != 0 && !constraints_.empty(); }
bool IKRig::compatible(const Skeleton& skeleton) const noexcept { return valid() && skeleton.valid() && skeleton_signature_ == skeleton.signature(); }
std::uint64_t IKRig::revision() const noexcept { return revision_; }
std::span<const IKConstraint> IKRig::constraints() const noexcept { return constraints_; }
const IKLimits& IKRig::limits() const noexcept { return limits_; }
std::optional<std::size_t> IKRig::find(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < constraints_.size(); ++index) {
        if (constraints_[index].name == name) { return index; }
    }
    return std::nullopt;
}

IKSolveResult solve_ik(const Skeleton& skeleton, const IKRig& rig,
                       std::span<const IKResolvedTarget> targets, Pose& pose,
                       double weight, std::size_t operation_budget) {
    IKSolveResult failure;
    if (!rig.compatible(skeleton)) { failure.result = {IKError::InvalidSkeleton}; return failure; }
    if (!pose.compatible(skeleton)) { failure.result = {IKError::InvalidPose}; return failure; }
    if (!bounded(weight, 0.0, 1.0)) { failure.result = {IKError::InvalidWeight}; return failure; }
    if (targets.size() != rig.constraints().size()) { failure.result = {IKError::InvalidTarget}; return failure; }
    const auto budget = std::min(operation_budget, rig.limits().max_operations_per_solve);
    if (budget < skeleton.size() * 4) { failure.result = {IKError::BudgetExceeded}; return failure; }
    failure.operations = skeleton.size();
    for (std::size_t index = 0; index < pose.local.size(); ++index) {
        const auto& local = pose.local[index];
        if (!math::finite(local) || !bounded_vector(local.position, rig.limits().coordinate_limit) ||
            !bounded(math::length(local.rotation), minimum_length, maximum_scale)) {
            failure.result = {IKError::InvalidPose, index}; return failure;
        }
        if (!uniform_scale(local.scale)) { failure.result = {IKError::UnsupportedScale, index}; return failure; }
    }
    Pose input;
    input.local = pose.local;
    input.skeleton_signature = pose.skeleton_signature;
    Solver solver{skeleton, rig.limits(), std::move(input), std::vector<math::Quat>(skeleton.size()), budget, {}};
    solver.report.operations = skeleton.size() * 2;
    if (!solver.refresh()) { return solver.report; }
    solver.report.constraints.reserve(rig.constraints().size());
    for (std::size_t index = 0; index < rig.constraints().size(); ++index) {
        const auto& constraint = rig.constraints()[index];
        const auto& target = targets[index];
        IKConstraintStatus status;
        status.constraint = index;
        if (!solver.charge(constraint.chain.size())) { return solver.report; }
        for (std::size_t link = 1; link < constraint.chain.size(); ++link) {
            if (math::distance(solver.position(constraint.chain[link - 1]), solver.position(constraint.chain[link])) <= minimum_length) {
                solver.report.result = {IKError::DegenerateChain, index}; return solver.report;
            }
        }
        if (constraint.kind != IKConstraintKind::RotationLimit &&
            (!bounded_vector(target.target, rig.limits().coordinate_limit) ||
             (target.direction && constraint.kind != IKConstraintKind::LookAt) ||
             (target.direction && math::length_squared(target.target) <= minimum_length * minimum_length) ||
             (target.has_pole && (constraint.kind != IKConstraintKind::TwoBone ||
                                 !bounded_vector(target.pole, rig.limits().coordinate_limit))))) {
            solver.report.result = {IKError::InvalidTarget, index}; return solver.report;
        }
        const double effective_weight = weight * constraint.weight;
        if (effective_weight > 0.0) {
            if (!solver.charge(skeleton.size() * 2)) { return solver.report; }
            const auto before = solver.pose.local;
            if (!solver.apply(constraint, target, status)) {
                solver.report.result.element = index;
                return solver.report;
            }
            if (!solver.charge(constraint.rotation_limits.size())) { return solver.report; }
            for (const auto& limit : constraint.rotation_limits) { solver.apply_limit(limit); }
            for (std::size_t bone = 0; bone < before.size(); ++bone) {
                solver.pose.local[bone].rotation = math::slerp(before[bone].rotation,
                    solver.pose.local[bone].rotation, effective_weight);
            }
            if (!solver.refresh()) { return solver.report; }
        }
        solver.report.constraints.push_back(status);
    }
    for (auto& status : solver.report.constraints) {
        const auto& constraint = rig.constraints()[status.constraint];
        if (!solver.charge(constraint.rotation_limits.size() + 1)) { return solver.report; }
        status.residual = solver.residual(constraint, targets[status.constraint]);
        status.reached = status.residual <= constraint.tolerance;
        if (!std::isfinite(status.residual)) {
            solver.report.result = {IKError::NonFiniteResult, status.constraint}; return solver.report;
        }
    }
    pose = std::move(solver.pose);
    return solver.report;
}

IKService::IKService(IKLimits limits) : limits_(limits) {
    if (!valid_limits(limits_)) { throw std::invalid_argument("invalid IK service limits"); }
}

IKResult IKService::publish(std::string name, std::shared_ptr<const IKRig> rig) {
    if (!valid_name(name)) { return {IKError::InvalidName}; }
    if (!rig || !rig->valid()) { return {IKError::InvalidConstraint}; }
    if (rig->constraints().size() > limits_.max_constraints ||
        (!rigs_.contains(name) && rigs_.size() >= limits_.max_resources)) { return {IKError::Capacity}; }
    std::size_t rotations = 0;
    for (const auto& constraint : rig->constraints()) {
        if (constraint.chain.size() > limits_.max_chain_bones || constraint.iterations > limits_.max_iterations ||
            constraint.rotation_limits.size() > limits_.max_rotation_limits - rotations) { return {IKError::Capacity}; }
        rotations += constraint.rotation_limits.size();
    }
    if (rig->limits().coordinate_limit > limits_.coordinate_limit) { return {IKError::InvalidLimits}; }
    rigs_.insert_or_assign(std::move(name), std::move(rig));
    return {};
}

std::shared_ptr<const IKRig> IKService::find(std::string_view name) const {
    const auto found = rigs_.find(name);
    return found == rigs_.end() ? nullptr : found->second;
}

bool IKService::erase(std::string_view name) {
    const auto found = rigs_.find(name);
    if (found == rigs_.end()) { return false; }
    rigs_.erase(found);
    return true;
}

IKAttachResult IKService::attach(ecs::Registry& registry, AnimationService& animation,
                                scene::Scene* scene_graph, ecs::Entity entity,
                                std::string_view name, IKBindingOptions options) {
    if (registry.iterating()) { return {{IKError::Busy}, {}}; }
    if (registry_identity_ && registry_identity_ != &registry) { return {{IKError::InvalidAttachment}, {}}; }
    const auto* animated = registry.get<AnimatedComponent>(entity);
    const auto rig = find(name);
    if (!animated || !registry.alive(entity)) { return {{IKError::InvalidEntity}, {}}; }
    const auto* skeleton = animation.skeletons.get(animated->skeleton);
    if (!skeleton || !rig || !rig->compatible(*skeleton) || skeleton->size() > limits_.max_pose_bones) {
        return {{IKError::InvalidSkeleton}, {}};
    }
    const auto* pose = composed_pose(registry, entity);
    if (!pose || !pose->compatible(*skeleton)) { return {{IKError::InvalidPose}, {}}; }
    if (!bounded(options.weight, 0.0, 1.0)) { return {{IKError::InvalidWeight}, {}}; }
    if ((!bindings_.contains(entity) && bindings_.size() >= limits_.max_entities) ||
        next_generation_ == std::numeric_limits<std::uint64_t>::max()) { return {{IKError::Capacity}, {}}; }
    const auto checked = validate_binding(registry, scene_graph, *skeleton, animated->bone_entities, options);
    if (!checked) { return {checked, {}}; }
    if (!model_inverse(scene_graph, options.model_entity)) { return {{IKError::UnsupportedScale}, {}}; }
    Binding candidate;
    candidate.handle = {entity, next_generation_++};
    candidate.rig_revision = rig->revision();
    candidate.skeleton_signature = skeleton->signature();
    candidate.skeleton = animated->skeleton;
    candidate.owner = animated;
    candidate.bones = animated->bone_entities;
    candidate.rig = rig;
    candidate.options = options;
    candidate.targets.reserve(rig->constraints().size());
    candidate.poles.reserve(rig->constraints().size());
    for (const auto& constraint : rig->constraints()) {
        candidate.targets.push_back(constraint.target);
        candidate.poles.push_back(constraint.pole);
    }
    const auto handle = candidate.handle;
    auto insertion = bindings_.try_emplace(entity);
    try {
        registry.emplace_or_replace<IKComponent>(entity, IKComponent{handle});
    } catch (...) {
        if (insertion.second) { bindings_.erase(insertion.first); }
        throw;
    }
    insertion.first->second = std::move(candidate);
    registry_identity_ = &registry;
    return {{}, handle};
}

bool IKService::detach(ecs::Registry& registry, IKHandle handle) {
    if (registry.iterating() || registry_identity_ != &registry) { return false; }
    const auto found = bindings_.find(handle.entity);
    if (found == bindings_.end() || found->second.handle != handle) { return false; }
    const auto* component = registry.get<IKComponent>(handle.entity);
    if (component && component->binding == handle) { (void)registry.remove<IKComponent>(handle.entity); }
    bindings_.erase(found);
    return true;
}

void IKService::clear(ecs::Registry& registry) {
    if (registry.iterating() || (registry_identity_ && registry_identity_ != &registry)) { return; }
    for (const auto& entry : bindings_) {
        const auto* component = registry.get<IKComponent>(entry.first);
        if (component && component->binding == entry.second.handle) { (void)registry.remove<IKComponent>(entry.first); }
    }
    bindings_.clear();
    rigs_.clear();
    registry_identity_ = nullptr;
}

std::optional<IKBindingInfo> IKService::inspect(IKHandle handle) const {
    const auto found = bindings_.find(handle.entity);
    if (found == bindings_.end() || found->second.handle != handle) { return std::nullopt; }
    const auto& record = found->second;
    return IKBindingInfo{record.handle, record.revision, record.rig_revision, record.options, record.last_solve};
}

IKHandle IKService::binding(ecs::Entity entity) const noexcept {
    const auto found = bindings_.find(entity);
    return found == bindings_.end() ? IKHandle{} : found->second.handle;
}

IKService::Binding* IKService::edit(IKHandle handle, std::uint64_t revision) noexcept {
    const auto found = bindings_.find(handle.entity);
    if (found == bindings_.end()) { return nullptr; }
    auto& record = found->second;
    if (record.handle != handle || record.revision != revision || record.rig->revision() != record.rig_revision ||
        revision == std::numeric_limits<std::uint64_t>::max()) { return nullptr; }
    return &record;
}

IKResult IKService::set_target(IKHandle handle, std::uint64_t revision, std::size_t constraint, IKTarget target) {
    auto* record = edit(handle, revision);
    if (!record) { return {IKError::StaleRevision}; }
    if (constraint >= record->targets.size()) { return {IKError::InvalidConstraint}; }
    const auto kind = record->rig->constraints()[constraint].kind;
    if (kind == IKConstraintKind::RotationLimit || !valid_target(target, record->rig->limits().coordinate_limit) ||
        (kind != IKConstraintKind::LookAt && direction_space(target.space))) { return {IKError::InvalidTarget}; }
    record->targets[constraint] = target;
    ++record->revision;
    return {};
}

IKResult IKService::set_pole(IKHandle handle, std::uint64_t revision, std::size_t constraint, std::optional<IKTarget> pole) {
    auto* record = edit(handle, revision);
    if (!record) { return {IKError::StaleRevision}; }
    if (constraint >= record->poles.size() || record->rig->constraints()[constraint].kind != IKConstraintKind::TwoBone) {
        return {IKError::InvalidConstraint};
    }
    if (pole && (!valid_target(*pole, record->rig->limits().coordinate_limit) || direction_space(pole->space))) {
        return {IKError::InvalidTarget};
    }
    record->poles[constraint] = pole;
    ++record->revision;
    return {};
}

IKResult IKService::set_weight(IKHandle handle, std::uint64_t revision, double weight) {
    auto* record = edit(handle, revision);
    if (!record) { return {IKError::StaleRevision}; }
    if (!bounded(weight, 0.0, 1.0)) { return {IKError::InvalidWeight}; }
    record->options.weight = weight;
    ++record->revision;
    return {};
}

IKResult IKService::set_enabled(IKHandle handle, std::uint64_t revision, bool enabled) {
    auto* record = edit(handle, revision);
    if (!record) { return {IKError::StaleRevision}; }
    record->options.enabled = enabled;
    ++record->revision;
    return {};
}

IKUpdate IKService::update(ecs::Registry& registry, AnimationService& animation,
                           scene::Scene* scene_graph, const scene::WorldPartition* partition) {
    IKUpdate update;
    if (registry.iterating() || (registry_identity_ && registry_identity_ != &registry) ||
        (scene_graph && &scene_graph->registry() != &registry)) {
        update.errors = 1;
        return update;
    }
    const auto coordinates = partition ? partition->origin() : scene::OriginState{};
    for (auto iterator = bindings_.begin(); iterator != bindings_.end();) {
        auto& record = iterator->second;
        const auto entity = record.handle.entity;
        auto* animated = registry.get<AnimatedComponent>(entity);
        const auto* marker = registry.get<IKComponent>(entity);
        const auto* skeleton = animated ? animation.skeletons.get(animated->skeleton) : nullptr;
        const bool live = animated && marker && marker->binding == record.handle &&
            animated == record.owner && animated->skeleton == record.skeleton && skeleton &&
            skeleton->signature() == record.skeleton_signature && animated->bone_entities == record.bones &&
            record.rig->revision() == record.rig_revision;
        if (!live) {
            if (marker && marker->binding == record.handle) { (void)registry.remove<IKComponent>(entity); }
            iterator = bindings_.erase(iterator);
            ++update.retired;
            continue;
        }
        ++iterator;
        if (!record.options.enabled || record.options.weight == 0.0 || !registry.enabled(entity) ||
            (scene_graph && scene_graph->contains(entity) && !scene_graph->effective_enabled(entity))) {
            ++update.skipped;
            continue;
        }
        record.last_solve = {};
        const auto remaining = limits_.max_operations_per_frame - update.operations;
        if (std::min(remaining, limits_.max_operations_per_solve) < skeleton->size() * 4) {
            record.last_solve.result = {IKError::BudgetExceeded};
            ++update.errors;
            ++update.budget_exhausted;
            continue;
        }
        auto checked = validate_binding(registry, scene_graph, *skeleton, record.bones, record.options);
        if (checked.error == IKError::InvalidAttachment || checked.error == IKError::InvalidEntity) {
            const auto handle = record.handle;
            (void)detach(registry, handle);
            ++update.retired;
            ++update.errors;
            continue;
        }
        auto* pose = composed_pose(registry, entity);
        if (checked && (!pose || !pose->compatible(*skeleton))) { checked = {IKError::InvalidPose}; }
        if (checked && !coordinates.consistent) { checked = {IKError::InvalidTarget}; }
        const auto inverse = model_inverse(scene_graph, record.options.model_entity);
        if (checked && !inverse) { checked = {IKError::UnsupportedScale}; }
        if (!checked) {
            record.last_solve.result = checked;
            ++update.errors;
            continue;
        }
        std::vector<IKResolvedTarget> targets(record.targets.size());
        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (record.rig->constraints()[index].kind == IKConstraintKind::RotationLimit) { continue; }
            auto& target = targets[index];
            checked = resolve_target(record.targets[index], scene_graph, coordinates.absolute_origin, *inverse,
                record.rig->limits().coordinate_limit, target.target, target.direction);
            if (checked && record.poles[index]) {
                bool direction = false;
                checked = resolve_target(*record.poles[index], scene_graph, coordinates.absolute_origin, *inverse,
                    record.rig->limits().coordinate_limit, target.pole, direction);
                target.has_pole = static_cast<bool>(checked);
                if (direction) { checked = {IKError::InvalidTarget}; }
            }
            if (!checked) { checked.element = index; break; }
        }
        if (!checked) {
            record.last_solve.result = checked;
            ++update.errors;
            continue;
        }
        Pose staged;
        staged.local = pose->local;
        staged.skeleton_signature = pose->skeleton_signature;
        record.last_solve = solve_ik(*skeleton, *record.rig, targets, staged, record.options.weight,
            std::min(remaining, limits_.max_operations_per_solve));
        update.operations += record.last_solve.operations;
        if (!record.last_solve.result) {
            ++update.errors;
            if (record.last_solve.result.error == IKError::BudgetExceeded) { ++update.budget_exhausted; }
            continue;
        }
        if (record.options.write_scene && !record.bones.empty()) {
            std::vector<scene::LocalTransformEdit> edits;
            edits.reserve(record.bones.size());
            for (std::size_t bone = 0; bone < record.bones.size(); ++bone) {
                edits.push_back({record.bones[bone], staged.local[bone]});
            }
            if (!scene_graph->apply_local_transforms(edits)) {
                record.last_solve.result = {IKError::SceneRejected};
                ++update.errors;
                continue;
            }
        }
        *pose = std::move(staged);
        (void)registry.mark_changed<AnimatedComponent>(entity);
        ++update.solved;
    }
    return update;
}

const IKLimits& IKService::limits() const noexcept { return limits_; }

void register_ik(core::Kernel& kernel) {
    if (kernel.services().find<IKService>()) { return; }
    auto& service = kernel.services().emplace<IKService>();
    kernel.register_module({"animation.ik", "Bounded pose IK, aiming and swing/twist constraints", 1});
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        registry->register_component<IKComponent>("animation.ik");
    }
    kernel.add_system({std::string(ik_system_name), core::Phase::Animation, ik_pose_order, {"animation.skeleton"}},
        [](core::Kernel& active, const core::Frame&) {
            auto* registry = active.services().find<ecs::Registry>();
            auto* animation = active.services().find<AnimationService>();
            if (!registry || !animation) { return; }
            const auto update = active.services().get<IKService>().update(*registry, *animation,
                active.services().find<scene::Scene>(), active.services().find<scene::WorldPartition>());
            active.metric("animation.ik.solved", static_cast<double>(update.solved));
            active.metric("animation.ik.errors", static_cast<double>(update.errors));
            active.metric("animation.ik.operations", static_cast<double>(update.operations));
            active.metric("animation.ik.retired", static_cast<double>(update.retired));
            active.metric("animation.ik.budget_exhausted", static_cast<double>(update.budget_exhausted));
        });
    kernel.add_shutdown("animation.ik", [owner = &service, active = &kernel] {
        if (auto* registry = active->services().find<ecs::Registry>()) { owner->clear(*registry); }
    });
    kernel.add_command("animation.ik.chain", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        if (args.size() != 8 && args.size() != 11) {
            return core::CommandResult{false, "usage: animation.ik.chain rig skeleton two|ccd root tip x y z [pole_x pole_y pole_z]"};
        }
        auto* animation = active.services().find<AnimationService>();
        const auto* skeleton = animation ? animation->skeletons.get(animation->skeletons.find(args[1])) : nullptr;
        const auto target = parse_vector(args, 5);
        if (!skeleton || !target) { return command_result({IKError::InvalidTarget}); }
        IKConstraint constraint;
        constraint.name = "reach";
        if (args[2] == "two") { constraint.kind = IKConstraintKind::TwoBone; }
        else if (args[2] == "ccd") { constraint.kind = IKConstraintKind::CCD; }
        else { return command_result({IKError::InvalidConstraint}); }
        const auto root = skeleton->find(args[3]);
        auto bone = skeleton->find(args[4]);
        if (root == no_bone || bone == no_bone) { return command_result({IKError::InvalidBone}); }
        while (bone != no_bone && constraint.chain.size() < owner->limits().max_chain_bones) {
            constraint.chain.push_back(bone);
            if (bone == root) { break; }
            bone = skeleton->bones()[bone].parent;
        }
        if (constraint.chain.empty() || constraint.chain.back() != root) { return command_result({IKError::InvalidChain}); }
        std::reverse(constraint.chain.begin(), constraint.chain.end());
        constraint.target = {IKTargetSpace::AbsolutePoint, *target, {}};
        if (args.size() == 11) {
            const auto pole = parse_vector(args, 8);
            if (!pole) { return command_result({IKError::InvalidTarget}); }
            constraint.pole = IKTarget{IKTargetSpace::AbsolutePoint, *pole, {}};
        }
        auto rig = std::make_shared<IKRig>();
        const auto loaded = rig->load(*skeleton, {std::move(constraint)}, owner->limits());
        return command_result(loaded ? owner->publish(args[0], std::move(rig)) : loaded);
    }, "Create one direct-parent chain with an absolute goal and optional two-bone pole");
    kernel.add_command("animation.ik.look", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        if (args.size() != 6) { return core::CommandResult{false, "usage: animation.ik.look rig skeleton bone x y z"}; }
        auto* animation = active.services().find<AnimationService>();
        const auto* skeleton = animation ? animation->skeletons.get(animation->skeletons.find(args[1])) : nullptr;
        const auto target = parse_vector(args, 3);
        if (!skeleton || !target) { return command_result({IKError::InvalidTarget}); }
        IKConstraint constraint;
        constraint.name = "look";
        constraint.kind = IKConstraintKind::LookAt;
        constraint.chain = {skeleton->find(args[2])};
        constraint.target = {IKTargetSpace::AbsolutePoint, *target, {}};
        auto rig = std::make_shared<IKRig>();
        const auto loaded = rig->load(*skeleton, {std::move(constraint)}, owner->limits());
        return command_result(loaded ? owner->publish(args[0], std::move(rig)) : loaded);
    }, "Aim a bone's local -Z axis at an absolute position");
    kernel.add_command("animation.ik.limit", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        if (args.size() != 9) { return core::CommandResult{false, "usage: animation.ik.limit rig skeleton bone axis_x axis_y axis_z min_deg max_deg swing_deg"}; }
        auto* animation = active.services().find<AnimationService>();
        const auto* skeleton = animation ? animation->skeletons.get(animation->skeletons.find(args[1])) : nullptr;
        const auto axis = parse_vector(args, 3);
        const auto minimum = parse_number(args[6]);
        const auto maximum = parse_number(args[7]);
        const auto swing = parse_number(args[8]);
        if (!skeleton || !axis || !minimum || !maximum || !swing) { return command_result({IKError::InvalidConstraint}); }
        const auto bone = skeleton->find(args[2]);
        if (bone == no_bone) { return command_result({IKError::InvalidBone}); }
        IKConstraint constraint;
        constraint.name = "limit";
        constraint.kind = IKConstraintKind::RotationLimit;
        constraint.rotation_limits.push_back({bone, skeleton->bones()[bone].bind_local.rotation, *axis,
            math::radians(*minimum), math::radians(*maximum), math::radians(*swing)});
        auto rig = std::make_shared<IKRig>();
        const auto loaded = rig->load(*skeleton, {std::move(constraint)}, owner->limits());
        return command_result(loaded ? owner->publish(args[0], std::move(rig)) : loaded);
    }, "Create bind-relative swing/twist limits; command angles are degrees");
    kernel.add_command("animation.ik.attach", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        auto* animation = active.services().find<AnimationService>();
        if (!registry || !animation || (args.size() != 2 && args.size() != 3)) {
            return core::CommandResult{false, "usage: animation.ik.attach entity rig [model_entity]"};
        }
        IKBindingOptions options;
        if (args.size() == 3) {
            options.model_entity = named_entity(*registry, args[2]);
            if (!options.model_entity) { return command_result({IKError::InvalidEntity}); }
        }
        const auto attached = owner->attach(*registry, *animation, active.services().find<scene::Scene>(),
            named_entity(*registry, args[0]), args[1], options);
        return command_result(attached.result);
    }, "Attach IK; omitted model_entity means identity, with bound bones parented at world root");
    kernel.add_command("animation.ik.target", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || (args.size() != 6 && args.size() != 7)) {
            return core::CommandResult{false, "usage: animation.ik.target entity index absolute|model|entity|world-direction|model-direction|entity-direction x y z [target_entity]"};
        }
        const auto info = owner->inspect(owner->binding(named_entity(*registry, args[0])));
        const auto value = parse_vector(args, 3);
        std::size_t index{};
        const auto parsed = std::from_chars(args[1].data(), args[1].data() + args[1].size(), index);
        if (!info || !value || parsed.ec != std::errc{} || parsed.ptr != args[1].data() + args[1].size()) {
            return command_result({IKError::InvalidTarget});
        }
        IKTarget target;
        target.value = *value;
        if (args[2] == "absolute") { target.space = IKTargetSpace::AbsolutePoint; }
        else if (args[2] == "model") { target.space = IKTargetSpace::ModelPoint; }
        else if (args[2] == "entity") { target.space = IKTargetSpace::EntityPoint; }
        else if (args[2] == "world-direction") { target.space = IKTargetSpace::WorldDirection; }
        else if (args[2] == "model-direction") { target.space = IKTargetSpace::ModelDirection; }
        else if (args[2] == "entity-direction") { target.space = IKTargetSpace::EntityDirection; }
        else { return command_result({IKError::InvalidTarget}); }
        if (args.size() == 7) {
            target.entity = named_entity(*registry, args[6]);
            if (!target.entity) { return command_result({IKError::InvalidEntity}); }
        }
        return command_result(owner->set_target(info->handle, info->revision, index, target));
    }, "Set a generation-bound entity offset, absolute point or explicit direction");
    kernel.add_command("animation.ik.weight", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 2) { return core::CommandResult{false, "usage: animation.ik.weight entity weight"}; }
        const auto info = owner->inspect(owner->binding(named_entity(*registry, args[0])));
        const auto weight = parse_number(args[1]);
        if (!info || !weight) { return command_result({IKError::InvalidWeight}); }
        return command_result(owner->set_weight(info->handle, info->revision, *weight));
    }, "Set the complete constraint-stack weight in [0,1]");
    kernel.add_command("animation.ik.enable", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 2 || (args[1] != "true" && args[1] != "false")) {
            return core::CommandResult{false, "usage: animation.ik.enable entity true|false"};
        }
        const auto info = owner->inspect(owner->binding(named_entity(*registry, args[0])));
        if (!info) { return command_result({IKError::InvalidAttachment}); }
        return command_result(owner->set_enabled(info->handle, info->revision, args[1] == "true"));
    }, "Enable or pause an attachment without changing animation ownership");
    kernel.add_command("animation.ik.detach", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 1) { return core::CommandResult{false, "usage: animation.ik.detach entity"}; }
        const bool removed = owner->detach(*registry, owner->binding(named_entity(*registry, args[0])));
        return core::CommandResult{removed, removed ? "IK attachment cancelled" : "IK attachment unavailable"};
    }, "Cancel the current exact binding and release its resource pin");
    kernel.add_command("animation.ik.status", [owner = &service](core::Kernel& active, core::CommandArgs args) {
        auto* registry = active.services().find<ecs::Registry>();
        if (!registry || args.size() != 1) { return core::CommandResult{false, "usage: animation.ik.status entity"}; }
        const auto info = owner->inspect(owner->binding(named_entity(*registry, args[0])));
        if (!info) { return command_result({IKError::InvalidAttachment}); }
        std::string message = "generation=" + std::to_string(info->handle.generation) +
            " revision=" + std::to_string(info->revision) + " result=" + std::string(describe(info->last_solve.result.error)) +
            " operations=" + std::to_string(info->last_solve.operations);
        for (const auto& constraint : info->last_solve.constraints) {
            message += " constraint=" + std::to_string(constraint.constraint) +
                " residual=" + std::to_string(constraint.residual) + " iterations=" + std::to_string(constraint.iterations);
        }
        return core::CommandResult{true, std::move(message)};
    }, "Inspect the binding revision, solver error, work and residuals");
}

} // namespace ge::animation
