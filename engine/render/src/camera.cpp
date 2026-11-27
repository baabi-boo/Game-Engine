#include <ge/render/camera.hpp>

#include <ge/core/runtime.hpp>
#include <ge/input/actions.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ge::render {
namespace {

constexpr double largest_coordinate = 1.0e12;
constexpr double minimum_size = 1.0e-6;

bool range(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool bounded(math::Vec3 value, double maximum = largest_coordinate) noexcept {
    return math::finite(value) && std::abs(value.x) <= maximum &&
           std::abs(value.y) <= maximum && std::abs(value.z) <= maximum;
}

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

bool valid_pose(const math::Transform& pose) noexcept {
    return math::finite(pose) && bounded(pose.position) &&
           range(math::length(pose.rotation), math::epsilon * 2.0, largest_coordinate) &&
           std::abs(pose.scale.x) > minimum_size &&
           std::abs(pose.scale.y) > minimum_size &&
           std::abs(pose.scale.z) > minimum_size;
}

math::Vec3 forward(math::Quat rotation) noexcept {
    return math::rotate(rotation, {0.0, 0.0, -1.0});
}

math::Vec3 right(math::Quat rotation) noexcept {
    return math::rotate(rotation, {1.0, 0.0, 0.0});
}

math::Quat orientation(double yaw, double pitch) noexcept {
    const double cosine = std::cos(pitch);
    return math::look_rotation({std::sin(yaw) * cosine, std::sin(pitch),
                               -std::cos(yaw) * cosine});
}

bool inside(Rect viewport, math::Vec2 pixel) noexcept {
    return math::finite(pixel) && !viewport.empty() &&
           pixel.x >= static_cast<double>(viewport.x) &&
           pixel.y >= static_cast<double>(viewport.y) &&
           pixel.x < static_cast<double>(viewport.x) + viewport.width &&
           pixel.y < static_cast<double>(viewport.y) + viewport.height;
}

math::Vec4 local_math_viewport(Rect viewport) noexcept {
    return {0.0, 0.0, static_cast<double>(viewport.width),
            static_cast<double>(viewport.height)};
}

math::Vec2 lower_left_pixel(Rect viewport, math::Vec2 pixel) noexcept {
    return {pixel.x - static_cast<double>(viewport.x),
            static_cast<double>(viewport.height) -
                (pixel.y - static_cast<double>(viewport.y))};
}

std::optional<double> number(std::string_view text) noexcept {
    double value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::size_t> slot_number(std::string_view text) noexcept {
    std::size_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<CameraMode> mode_named(std::string_view name) noexcept {
    if (name == "fixed") { return CameraMode::Fixed; }
    if (name == "fly") { return CameraMode::FreeFly; }
    if (name == "orbit") { return CameraMode::Orbit; }
    if (name == "follow") { return CameraMode::Follow; }
    return std::nullopt;
}

std::array<math::Vec3, 8> box_corners(const math::Aabb& box) noexcept {
    std::array<math::Vec3, 8> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = {(index & 1U) != 0 ? box.max.x : box.min.x,
                         (index & 2U) != 0 ? box.max.y : box.min.y,
                         (index & 4U) != 0 ? box.max.z : box.min.z};
    }
    return result;
}

std::uint64_t scrambled(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double sample_noise(std::uint64_t seed, double time, std::uint64_t channel) noexcept {
    const double cell = std::floor(time);
    const auto index = static_cast<std::uint64_t>(cell);
    const auto first = scrambled(seed ^ (index + channel * 0x9e3779b97f4a7c15ULL));
    const auto second = scrambled(seed ^ (index + 1U + channel * 0x9e3779b97f4a7c15ULL));
    constexpr double divisor = 1.0 / 9007199254740991.0;
    const double a = static_cast<double>(first >> 11U) * divisor * 2.0 - 1.0;
    const double b = static_cast<double>(second >> 11U) * divisor * 2.0 - 1.0;
    const double fraction = time - cell;
    return math::lerp(a, b, fraction * fraction * (3.0 - 2.0 * fraction));
}

struct ControlSample {
    math::Vec2 move{};
    math::Vec2 look{};
    double lift{};
    double zoom{};
    bool sprint{};
    bool relative{true};
};

ControlSample sample_input(const input::InputSystem* system) {
    ControlSample sample;
    if (system == nullptr || !system->snapshot().focused) {
        return sample;
    }
    sample.move = system->vector2("move");
    sample.lift = system->axis("camera.lift");
    sample.zoom = system->axis("camera.zoom");
    sample.sprint = system->down("sprint");
    const auto* action = system->action("look");
    if (action != nullptr && !action->consumed) {
        sample.look = action->vector;
        for (const auto& context : system->contexts()) {
            const auto found = std::find_if(context.bindings.begin(), context.bindings.end(),
                [action](const input::Binding& binding) { return binding.id == action->source_binding; });
            if (found == context.bindings.end()) {
                continue;
            }
            sample.relative = false;
            for (std::size_t index = 0; index < found->control_count; ++index) {
                const auto kind = found->controls[index].kind;
                sample.relative = sample.relative || kind == input::ControlKind::PointerX ||
                                  kind == input::ControlKind::PointerY;
            }
            break;
        }
    }
    if (!math::finite(sample.move)) { sample.move = {}; }
    if (!math::finite(sample.look)) { sample.look = {}; }
    if (!std::isfinite(sample.lift)) { sample.lift = 0.0; }
    if (!std::isfinite(sample.zoom)) { sample.zoom = 0.0; }
    sample.move.x = std::clamp(sample.move.x, -1.0, 1.0);
    sample.move.y = std::clamp(sample.move.y, -1.0, 1.0);
    sample.look.x = std::clamp(sample.look.x, -1000000.0, 1000000.0);
    sample.look.y = std::clamp(sample.look.y, -1000000.0, 1000000.0);
    sample.lift = std::clamp(sample.lift, -1.0, 1.0);
    sample.zoom = std::clamp(sample.zoom, -100.0, 100.0);
    return sample;
}

} // namespace

std::string_view describe(CameraError error) noexcept {
    switch (error) {
    case CameraError::None: return "none";
    case CameraError::MissingCamera: return "camera is missing";
    case CameraError::Capacity: return "camera capacity exceeded";
    case CameraError::InvalidProjection: return "invalid or noninvertible projection";
    case CameraError::InvalidViewport: return "viewport is empty or outside the framebuffer";
    case CameraError::InvalidController: return "invalid controller settings";
    case CameraError::InvalidTransform: return "invalid or noninvertible camera transform";
    case CameraError::InvalidTarget: return "camera target is missing or invalid";
    case CameraError::InvalidBounds: return "bounds cannot be fitted by this camera";
    case CameraError::SceneRejected: return "scene rejected the camera pose";
    case CameraError::Disabled: return "camera is disabled";
    case CameraError::PhysicsOwned: return "camera pose is owned by physics";
    }
    return "unknown camera error";
}

bool valid(const CameraProjection& settings) noexcept {
    return (settings.mode == ProjectionMode::Perspective || settings.mode == ProjectionMode::Orthographic) &&
           (settings.clip_depth == math::ClipDepth::negative_one_to_one ||
            settings.clip_depth == math::ClipDepth::zero_to_one) &&
           range(settings.vertical_fov, 0.001, math::pi - 0.001) &&
           range(settings.orthographic_height, minimum_size, 1.0e9) &&
           range(settings.near_plane, minimum_size, 1.0e9) &&
           range(settings.far_plane, settings.near_plane + minimum_size, largest_coordinate) &&
           (settings.aspect_override == 0.0 || range(settings.aspect_override, 0.0001, 10000.0)) &&
           math::finite(settings.lens_shift) &&
           std::abs(settings.lens_shift.x) <= 10.0 && std::abs(settings.lens_shift.y) <= 10.0;
}

bool valid(const CameraViewport& settings) noexcept {
    if (settings.mode == ViewportMode::Framebuffer) {
        return true;
    }
    const auto rectangle = settings.rectangle;
    if (!math::finite(rectangle) || rectangle.x < 0.0 || rectangle.y < 0.0 ||
        rectangle.z <= 0.0 || rectangle.w <= 0.0) {
        return false;
    }
    if (settings.mode == ViewportMode::Normalized) {
        return rectangle.x + rectangle.z <= 1.0 && rectangle.y + rectangle.w <= 1.0;
    }
    if (settings.mode != ViewportMode::Pixels) {
        return false;
    }
    constexpr double maximum = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    return rectangle.x + rectangle.z <= maximum && rectangle.y + rectangle.w <= maximum &&
           std::floor(rectangle.x) == rectangle.x && std::floor(rectangle.y) == rectangle.y &&
           std::floor(rectangle.z) == rectangle.z && std::floor(rectangle.w) == rectangle.w;
}

bool valid(const CameraController& settings) noexcept {
    return settings.mode >= CameraMode::Fixed && settings.mode <= CameraMode::Follow &&
           settings.look_input >= LookInputMode::Automatic && settings.look_input <= LookInputMode::Rate &&
           bounded(settings.focus_point) && bounded(settings.follow_offset, 1.0e9) &&
           range(settings.move_speed, 0.0, 1000000.0) && range(settings.sprint_multiplier, 1.0, 100.0) &&
           range(settings.mouse_sensitivity, 0.0, 1.0) && range(settings.look_rate, 0.0, 100.0) &&
           range(settings.pan_speed, 0.0, 1000.0) && range(settings.zoom_speed, 0.0, 10.0) &&
           range(settings.minimum_distance, minimum_size, 1.0e9) &&
           range(settings.maximum_distance, settings.minimum_distance, 1.0e9) &&
           range(settings.distance, settings.minimum_distance, settings.maximum_distance) &&
           range(settings.minimum_pitch, -math::pi * 0.4999, math::pi * 0.4999) &&
           range(settings.maximum_pitch, settings.minimum_pitch, math::pi * 0.4999) &&
           range(settings.movement_half_life, 0.0, 60.0) &&
           range(settings.rotation_half_life, 0.0, 60.0) && range(settings.follow_half_life, 0.0, 60.0);
}

std::optional<Rect> resolve_viewport(const CameraViewport& settings,
                                      std::uint32_t width, std::uint32_t height) noexcept {
    constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (!valid(settings) || width == 0 || height == 0 || width > maximum || height > maximum) {
        return std::nullopt;
    }
    if (settings.mode == ViewportMode::Framebuffer) {
        return Rect{0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    }
    auto rectangle = settings.rectangle;
    if (settings.mode == ViewportMode::Normalized) {
        const double end_x = std::floor((rectangle.x + rectangle.z) * static_cast<double>(width));
        const double end_y = std::floor((rectangle.y + rectangle.w) * static_cast<double>(height));
        rectangle.x = std::floor(rectangle.x * static_cast<double>(width));
        rectangle.y = std::floor(rectangle.y * static_cast<double>(height));
        rectangle.z = end_x - rectangle.x;
        rectangle.w = end_y - rectangle.y;
    }
    if (rectangle.z < 1.0 || rectangle.w < 1.0 ||
        rectangle.x + rectangle.z > static_cast<double>(width) ||
        rectangle.y + rectangle.w > static_cast<double>(height)) {
        return std::nullopt;
    }
    return Rect{static_cast<std::int32_t>(rectangle.x), static_cast<std::int32_t>(rectangle.y),
                static_cast<std::int32_t>(rectangle.z), static_cast<std::int32_t>(rectangle.w)};
}

std::optional<math::Mat4> camera_projection(const CameraProjection& settings, Rect viewport) noexcept {
    if (!valid(settings) || viewport.empty()) {
        return std::nullopt;
    }
    const double aspect = settings.aspect_override > 0.0 ? settings.aspect_override :
        static_cast<double>(viewport.width) / static_cast<double>(viewport.height);
    math::Mat4 projection;
    try {
        if (settings.mode == ProjectionMode::Perspective) {
            projection = settings.clip_depth == math::ClipDepth::zero_to_one ?
                math::perspective_zero_to_one(settings.vertical_fov, aspect, settings.near_plane, settings.far_plane) :
                math::perspective(settings.vertical_fov, aspect, settings.near_plane, settings.far_plane);
        } else {
            const double half_y = settings.orthographic_height * 0.5;
            const double half_x = half_y * aspect;
            projection = settings.clip_depth == math::ClipDepth::zero_to_one ?
                math::orthographic_zero_to_one(-half_x, half_x, -half_y, half_y, settings.near_plane, settings.far_plane) :
                math::orthographic(-half_x, half_x, -half_y, half_y, settings.near_plane, settings.far_plane);
        }
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    math::Mat4 shift;
    shift(0, 3) = settings.lens_shift.x;
    shift(1, 3) = settings.lens_shift.y;
    projection = shift * projection;
    return math::finite(projection) && math::inverse(projection) ?
        std::optional<math::Mat4>{projection} : std::nullopt;
}

std::optional<math::Ray> camera_ray(const CameraView& view, math::Vec2 pixel, bool require_inside) noexcept {
    if (!math::finite(pixel) || view.viewport.empty() || (require_inside && !inside(view.viewport, pixel))) {
        return std::nullopt;
    }
    return math::picking_ray(lower_left_pixel(view.viewport, pixel), view.inverse_view_projection,
                             local_math_viewport(view.viewport), view.clip_depth);
}

std::optional<math::Vec3> camera_unproject(const CameraView& view, math::Vec3 pixel_depth) noexcept {
    if (!math::finite(pixel_depth) || view.viewport.empty()) {
        return std::nullopt;
    }
    const auto point = lower_left_pixel(view.viewport, {pixel_depth.x, pixel_depth.y});
    return math::unproject({point.x, point.y, pixel_depth.z}, view.inverse_view_projection,
                            local_math_viewport(view.viewport), view.clip_depth);
}

std::optional<math::Vec3> camera_project(const CameraView& view, math::Vec3 world) noexcept {
    if (!math::finite(world) || view.viewport.empty()) {
        return std::nullopt;
    }
    const auto clip = view.view_projection * math::homogeneous(world);
    if (!math::finite(clip) || clip.w <= math::epsilon) {
        return std::nullopt;
    }
    const math::Vec3 ndc{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    const double depth = view.clip_depth == math::ClipDepth::zero_to_one ? ndc.z : (ndc.z + 1.0) * 0.5;
    const math::Vec3 result{
        static_cast<double>(view.viewport.x) + (ndc.x + 1.0) * 0.5 * view.viewport.width,
        static_cast<double>(view.viewport.y) + (1.0 - ndc.y) * 0.5 * view.viewport.height,
        depth};
    return math::finite(result) ? std::optional<math::Vec3>{result} : std::nullopt;
}

struct CameraService::Impl {
    struct Transition {
        math::Transform source{};
        math::Transform destination{};
        double elapsed{};
        double duration{};
        bool active{};
    };
    struct Shake {
        CameraShake settings{};
        double elapsed{};
    };
    struct Entry {
        ecs::Entity entity{};
        std::optional<CameraView> last;
        math::Mat4 sampled_effect{};
        CameraError error{CameraError::None};
        Transition transition{};
        std::vector<Shake> shakes;
        math::Vec3 velocity{};
        math::Vec3 recoil_position{};
        math::Vec3 recoil_rotation{};
        double recoil_half_life{0.08};
        double yaw{};
        double pitch{};
        double follow_yaw{};
        double follow_pitch{};
        bool angles_initialized{};
        CameraMode observed_mode{CameraMode::Fixed};
    };
    scene::Scene& scene;
    ecs::Registry& registry;
    CameraLimits limits;
    std::vector<Entry> entries;
    std::vector<ecs::Entity> active;
    std::vector<CameraView> published;
    std::size_t input_slot{};
    std::uint32_t width{640};
    std::uint32_t height{360};
    CameraStatistics statistics{};
    std::uint64_t view_revision{};

    Impl(scene::Scene& shared_scene, CameraLimits configuration)
        : scene(shared_scene), registry(shared_scene.registry()), limits(configuration) {
        if (limits.cameras == 0 || limits.cameras > 65536 || limits.views == 0 || limits.views > 64 ||
            limits.shakes_per_camera > 64 || !range(limits.maximum_delta_seconds, 0.001, 1.0) ||
            !range(limits.maximum_effect_translation, 0.0, 1000.0) ||
            !range(limits.maximum_effect_rotation, 0.0, math::pi)) {
            throw std::invalid_argument("invalid camera service limits");
        }
        entries.reserve(limits.cameras);
        active.resize(limits.views);
        published.reserve(limits.views);
        (void)registry.register_component<CameraComponent>("render.camera");
    }

    Entry* entry(ecs::Entity entity) noexcept {
        const auto found = std::find_if(entries.begin(), entries.end(),
            [entity](const Entry& value) { return value.entity == entity; });
        return found != entries.end() ? &*found : nullptr;
    }

    const Entry* entry(ecs::Entity entity) const noexcept {
        const auto found = std::find_if(entries.begin(), entries.end(),
            [entity](const Entry& value) { return value.entity == entity; });
        return found != entries.end() ? &*found : nullptr;
    }

    bool reject(Entry* value, CameraError error) noexcept {
        if (value != nullptr) { value->error = error; }
        return false;
    }

    bool track(ecs::Entity entity) {
        if (entry(entity) != nullptr) { return true; }
        if (entries.size() >= limits.cameras) { return false; }
        Entry value;
        value.entity = entity;
        value.shakes.reserve(limits.shakes_per_camera);
        entries.push_back(std::move(value));
        return true;
    }

    void sync_entries() {
        std::erase_if(entries, [this](const Entry& value) {
            return !scene.contains(value.entity) || registry.get<CameraComponent>(value.entity) == nullptr;
        });
        for (auto& entity : active) {
            if (entry(entity) == nullptr) { entity = {}; }
        }
        for (const auto entity : registry.view<CameraComponent>(true)) {
            if (scene.contains(entity) && entries.size() < limits.cameras) {
                (void)track(entity);
            }
        }
    }

    void initialize_angles(Entry& value, const math::Transform& pose) noexcept {
        const auto direction = forward(pose.rotation);
        value.yaw = std::atan2(direction.x, -direction.z);
        value.pitch = std::asin(std::clamp(direction.y, -1.0, 1.0));
        value.angles_initialized = true;
    }

    bool write_pose(Entry& value, const math::Transform& pose) {
        if (registry.has<physics::BodyComponent>(value.entity)) {
            return reject(&value, CameraError::PhysicsOwned);
        }
        if (!valid_pose(pose)) { return reject(&value, CameraError::InvalidTransform); }
        if (!scene.set_world_transform(value.entity, pose)) {
            return reject(&value, CameraError::SceneRejected);
        }
        return true;
    }

    bool advance_transition(Entry& value, double delta) {
        auto& transition = value.transition;
        transition.elapsed = std::min(transition.duration, transition.elapsed + delta);
        const double amount = math::smootherstep(0.0, transition.duration, transition.elapsed);
        const auto pose = math::interpolate(transition.source, transition.destination, amount);
        if (!write_pose(value, pose)) { return false; }
        initialize_angles(value, pose);
        value.velocity = {};
        if (transition.elapsed >= transition.duration) { transition.active = false; }
        return true;
    }

    bool controller(Entry& value, CameraComponent& camera, ControlSample controls, double delta) {
        auto& settings = camera.controller;
        if (!valid(settings)) { return reject(&value, CameraError::InvalidController); }
        if (settings.mode == CameraMode::Fixed) { return true; }
        const auto exact = scene.world_matrix(value.entity);
        const auto current = exact ? math::decompose(*exact) : std::nullopt;
        if (!current) { return reject(&value, CameraError::InvalidTransform); }
        if (!value.angles_initialized || value.observed_mode != settings.mode) {
            initialize_angles(value, *current);
            value.velocity = {};
            value.observed_mode = settings.mode;
            value.follow_yaw = 0.0;
            value.follow_pitch = 0.0;
        }
        if (!settings.input_enabled || active[input_slot] != value.entity) { controls = {}; }
        const bool relative = settings.look_input == LookInputMode::Relative ||
            (settings.look_input == LookInputMode::Automatic && controls.relative);
        const double sensitivity = relative ? settings.mouse_sensitivity : settings.look_rate * delta;
        value.yaw = math::wrap_angle(value.yaw + controls.look.x * sensitivity);
        value.pitch = std::clamp(value.pitch - controls.look.y * sensitivity,
                                settings.minimum_pitch, settings.maximum_pitch);
        math::Transform desired = *current;
        const auto wanted_rotation = orientation(value.yaw, value.pitch);
        const double turn = math::damping_factor(settings.rotation_half_life, delta);
        desired.rotation = math::slerp(current->rotation, wanted_rotation, turn);
        if (settings.mode == CameraMode::FreeFly) {
            auto direction = right(wanted_rotation) * controls.move.x +
                             forward(wanted_rotation) * controls.move.y + math::Vec3{0.0, controls.lift, 0.0};
            direction = math::clamp_length(direction, 1.0);
            const double speed = settings.move_speed * (controls.sprint ? settings.sprint_multiplier : 1.0);
            const auto target_velocity = direction * speed;
            value.velocity = math::lerp(value.velocity, target_velocity,
                math::damping_factor(settings.movement_half_life, delta));
            desired.position += value.velocity * delta;
            return write_pose(value, desired);
        }
        math::Vec3 focus_point = settings.focus_point;
        std::optional<math::Mat4> target_matrix;
        if (settings.target) {
            if (settings.target == value.entity || scene.is_ancestor(value.entity, settings.target)) {
                return reject(&value, CameraError::InvalidTarget);
            }
            target_matrix = scene.world_matrix(settings.target);
            if (!target_matrix) { return reject(&value, CameraError::InvalidTarget); }
            focus_point = math::transform_point(*target_matrix, settings.focus_point);
        }
        if (!bounded(focus_point)) { return reject(&value, CameraError::InvalidTarget); }
        if (settings.mode == CameraMode::Orbit) {
            const double zoom = std::exp(std::clamp(-controls.zoom * settings.zoom_speed, -10.0, 10.0));
            settings.distance = std::clamp(settings.distance * zoom, settings.minimum_distance, settings.maximum_distance);
            const auto facing = forward(wanted_rotation);
            const auto ground_forward = math::normalized(math::Vec3{facing.x, 0.0, facing.z}, {0.0, 0.0, -1.0});
            auto pan = right(wanted_rotation) * controls.move.x + ground_forward * controls.move.y +
                       math::Vec3{0.0, controls.lift, 0.0};
            pan = math::clamp_length(pan, 1.0) * (settings.pan_speed * settings.distance * delta);
            if (!settings.target) {
                const auto next_focus = settings.focus_point + pan;
                if (!bounded(next_focus)) { return reject(&value, CameraError::InvalidTarget); }
                settings.focus_point = next_focus;
                focus_point = next_focus;
            }
            desired.position = focus_point - forward(desired.rotation) * settings.distance;
            return write_pose(value, desired);
        }
        if (!settings.target || !target_matrix) { return reject(&value, CameraError::InvalidTarget); }
        value.follow_yaw = math::wrap_angle(value.follow_yaw + controls.look.x * sensitivity);
        value.follow_pitch = std::clamp(value.follow_pitch - controls.look.y * sensitivity,
                                        settings.minimum_pitch, settings.maximum_pitch);
        const double zoom = std::exp(std::clamp(-controls.zoom * settings.zoom_speed, -10.0, 10.0));
        const double offset_length = math::length(settings.follow_offset);
        if (offset_length <= minimum_size) { return reject(&value, CameraError::InvalidTarget); }
        settings.follow_offset *= std::clamp(offset_length * zoom, settings.minimum_distance,
                                             settings.maximum_distance) / offset_length;
        auto offset = math::rotate(orientation(value.follow_yaw, value.follow_pitch), settings.follow_offset);
        if (settings.follow_target_rotation) {
            const auto direction = math::transform_vector(*target_matrix, {0.0, 0.0, -1.0});
            const auto up = math::transform_vector(*target_matrix, {0.0, 1.0, 0.0});
            if (math::length_squared(direction) <= math::epsilon ||
                math::length_squared(math::cross(direction, up)) <= math::epsilon) {
                return reject(&value, CameraError::InvalidTarget);
            }
            offset = math::rotate(math::look_rotation(direction, up), offset);
        }
        const auto destination = focus_point + offset;
        const auto direction = focus_point - destination;
        if (!bounded(destination) || math::length_squared(direction) <= math::epsilon) {
            return reject(&value, CameraError::InvalidTarget);
        }
        const double following = math::damping_factor(settings.follow_half_life, delta);
        desired.position = math::lerp(current->position, destination, following);
        desired.rotation = math::slerp(current->rotation, math::look_rotation(direction), turn);
        initialize_angles(value, desired);
        return write_pose(value, desired);
    }

    math::Mat4 effects(Entry& value, double delta) {
        math::Vec3 position = value.recoil_position;
        math::Vec3 rotation = value.recoil_rotation;
        for (auto& shake : value.shakes) {
            shake.elapsed = std::min(shake.settings.duration, shake.elapsed + delta);
            const double remaining = 1.0 - shake.elapsed / shake.settings.duration;
            const double envelope = remaining * remaining;
            const double time = shake.elapsed * shake.settings.frequency;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto channel = static_cast<std::uint64_t>(axis);
                position[axis] += shake.settings.translation[axis] * envelope *
                                  sample_noise(shake.settings.seed, time, channel);
                rotation[axis] += shake.settings.rotation[axis] * envelope *
                                  sample_noise(shake.settings.seed, time, channel + 3U);
            }
        }
        std::erase_if(value.shakes, [](const Shake& shake) { return shake.elapsed >= shake.settings.duration; });
        const double retention = 1.0 - math::damping_factor(value.recoil_half_life, delta);
        value.recoil_position *= retention;
        value.recoil_rotation *= retention;
        position = math::clamp_length(position, limits.maximum_effect_translation);
        rotation = math::clamp_length(rotation, limits.maximum_effect_rotation);
        return math::trs_matrix(position, math::from_euler_xyz(rotation), {1.0, 1.0, 1.0});
    }

    bool build_view(Entry& value, const CameraComponent& camera, const math::Mat4& effect) {
        const auto viewport = resolve_viewport(camera.viewport, width, height);
        if (!viewport) { return reject(&value, CameraError::InvalidViewport); }
        const auto projection = camera_projection(camera.projection, *viewport);
        if (!projection) { return reject(&value, CameraError::InvalidProjection); }
        const auto world = scene.world_matrix(value.entity);
        if (!world) { return reject(&value, CameraError::InvalidTransform); }
        const auto render_world = *world * effect;
        const auto inverted = math::inverse(render_world);
        if (!inverted) { return reject(&value, CameraError::InvalidTransform); }
        CameraView result;
        result.entity = value.entity;
        result.view = *inverted;
        result.inverse_view = render_world;
        result.projection = *projection;
        result.view_projection = result.projection * result.view;
        const auto inverse_projection = math::inverse(result.projection);
        const auto inverse_combined = math::inverse(result.view_projection);
        const auto frustum = math::extract_frustum(result.view_projection, camera.projection.clip_depth);
        if (!inverse_projection || !inverse_combined || !frustum) {
            return reject(&value, CameraError::InvalidProjection);
        }
        result.inverse_projection = *inverse_projection;
        result.inverse_view_projection = *inverse_combined;
        result.position = math::transform_point(render_world, {});
        result.forward = math::normalized(math::transform_vector(render_world, {0.0, 0.0, -1.0}));
        result.up = math::normalized(math::transform_vector(render_world, {0.0, 1.0, 0.0}));
        if (!bounded(result.position) || math::length_squared(result.forward) < 0.5 ||
            math::length_squared(result.up) < 0.5) {
            return reject(&value, CameraError::InvalidTransform);
        }
        result.viewport = *viewport;
        result.frustum = *frustum;
        result.clip_depth = camera.projection.clip_depth;
        result.projection_mode = camera.projection.mode;
        result.near_plane = camera.projection.near_plane;
        result.far_plane = camera.projection.far_plane;
        result.visibility_mask = camera.visibility_mask;
        increment(view_revision);
        result.revision = view_revision;
        result.stale = value.error != CameraError::None;
        value.last = result;
        return true;
    }

    void publish_views() {
        published.clear();
        for (std::size_t slot = 0; slot < active.size(); ++slot) {
            const auto* value = entry(active[slot]);
            if (value == nullptr || !value->last || value->error == CameraError::Disabled) { continue; }
            auto snapshot = *value->last;
            snapshot.slot = slot;
            published.push_back(snapshot);
        }
    }
};

CameraService::CameraService(scene::Scene& scene, CameraLimits limits)
    : impl_(std::make_unique<Impl>(scene, limits)) {}
CameraService::~CameraService() = default;

const CameraLimits& CameraService::limits() const noexcept { return impl_->limits; }

CameraComponent* CameraService::component(ecs::Entity entity) noexcept {
    return impl_->registry.get<CameraComponent>(entity);
}

const CameraComponent* CameraService::component(ecs::Entity entity) const noexcept {
    return impl_->registry.get<CameraComponent>(entity);
}

ecs::Entity CameraService::create(std::string_view name, const math::Transform& transform,
                                  CameraComponent settings) {
    if (impl_->entries.size() >= impl_->limits.cameras || !valid_pose(transform) ||
        !valid(settings.projection) || !valid(settings.viewport) || !valid(settings.controller)) {
        return {};
    }
    const auto entity = impl_->scene.create(name, {}, transform);
    if (!entity) { return {}; }
    try {
        if (!attach(entity, settings)) {
            (void)impl_->scene.destroy(entity);
            return {};
        }
    } catch (...) {
        (void)impl_->scene.destroy(entity);
        throw;
    }
    if (!active_entity()) { (void)set_active(entity); }
    return entity;
}

bool CameraService::attach(ecs::Entity entity, CameraComponent settings) {
    auto* value = impl_->entry(entity);
    if (!impl_->scene.contains(entity)) { return impl_->reject(value, CameraError::MissingCamera); }
    if (!valid(settings.projection)) { return impl_->reject(value, CameraError::InvalidProjection); }
    if (!valid(settings.viewport)) { return impl_->reject(value, CameraError::InvalidViewport); }
    if (!valid(settings.controller)) { return impl_->reject(value, CameraError::InvalidController); }
    const auto viewport = resolve_viewport(settings.viewport, impl_->width, impl_->height);
    if (!camera_projection(settings.projection, viewport.value_or(Rect{0, 0, 1, 1}))) {
        return impl_->reject(value, CameraError::InvalidProjection);
    }
    const bool previously_tracked = value != nullptr;
    if (!impl_->track(entity)) { return impl_->reject(value, CameraError::Capacity); }
    try {
        impl_->registry.emplace<CameraComponent>(entity) = settings;
    } catch (...) {
        if (!previously_tracked) {
            std::erase_if(impl_->entries, [entity](const Impl::Entry& entry) { return entry.entity == entity; });
        }
        throw;
    }
    value = impl_->entry(entity);
    value->angles_initialized = false;
    value->velocity = {};
    value->error = CameraError::None;
    value->transition.active = false;
    return true;
}

bool CameraService::detach(ecs::Entity entity) {
    if (!impl_->registry.remove<CameraComponent>(entity)) { return false; }
    std::erase_if(impl_->entries, [entity](const Impl::Entry& value) { return value.entity == entity; });
    std::erase_if(impl_->published, [entity](const CameraView& value) { return value.entity == entity; });
    for (auto& active : impl_->active) {
        if (active == entity) { active = {}; }
    }
    return true;
}

bool CameraService::set_projection(ecs::Entity entity, const CameraProjection& projection) {
    auto* camera = component(entity);
    auto* value = impl_->entry(entity);
    if (camera == nullptr) { return impl_->reject(value, CameraError::MissingCamera); }
    const auto viewport = resolve_viewport(camera->viewport, impl_->width, impl_->height);
    if (!camera_projection(projection, viewport.value_or(Rect{0, 0, 1, 1}))) {
        return impl_->reject(value, CameraError::InvalidProjection);
    }
    camera->projection = projection;
    return true;
}

bool CameraService::set_viewport(ecs::Entity entity, const CameraViewport& viewport) {
    auto* camera = component(entity);
    auto* value = impl_->entry(entity);
    if (camera == nullptr) { return impl_->reject(value, CameraError::MissingCamera); }
    if (!valid(viewport)) { return impl_->reject(value, CameraError::InvalidViewport); }
    camera->viewport = viewport;
    return true;
}

bool CameraService::set_controller(ecs::Entity entity, const CameraController& controller) {
    auto* camera = component(entity);
    auto* value = impl_->entry(entity);
    if (camera == nullptr || value == nullptr) { return impl_->reject(value, CameraError::MissingCamera); }
    if (!valid(controller)) { return impl_->reject(value, CameraError::InvalidController); }
    camera->controller = controller;
    value->angles_initialized = false;
    value->velocity = {};
    return true;
}

bool CameraService::set_active(ecs::Entity entity, std::size_t slot) {
    if (slot >= impl_->active.size() || component(entity) == nullptr ||
        !impl_->scene.contains(entity) || !impl_->track(entity)) {
        return false;
    }
    impl_->active[slot] = entity;
    return true;
}

bool CameraService::clear_view(std::size_t slot) noexcept {
    if (slot >= impl_->active.size()) { return false; }
    impl_->active[slot] = {};
    std::erase_if(impl_->published, [slot](const CameraView& value) { return value.slot == slot; });
    return true;
}

ecs::Entity CameraService::active_entity(std::size_t slot) const noexcept {
    return slot < impl_->active.size() ? impl_->active[slot] : ecs::Entity{};
}

bool CameraService::set_input_view(std::size_t slot) noexcept {
    if (slot >= impl_->active.size()) { return false; }
    impl_->input_slot = slot;
    return true;
}

bool CameraService::set_framebuffer_size(std::uint32_t width, std::uint32_t height) noexcept {
    constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (width > maximum || height > maximum || ((width == 0) != (height == 0))) { return false; }
    impl_->width = width;
    impl_->height = height;
    return true;
}

bool CameraService::look_at(ecs::Entity entity, math::Vec3 point, math::Vec3 up) {
    auto* value = impl_->entry(entity);
    if (value == nullptr || component(entity) == nullptr) { return false; }
    const auto world = impl_->scene.world_matrix(entity);
    if (!world || !bounded(point) || !math::normalize(up)) {
        return impl_->reject(value, CameraError::InvalidTarget);
    }
    auto direction = point - math::transform_point(*world, {});
    if (!math::normalize(direction) || math::length_squared(math::cross(direction, up)) <= math::epsilon) {
        return impl_->reject(value, CameraError::InvalidTarget);
    }
    if (!impl_->scene.look_at(entity, point, up)) { return impl_->reject(value, CameraError::SceneRejected); }
    value->angles_initialized = false;
    value->transition.active = false;
    return true;
}

bool CameraService::focus(ecs::Entity entity, math::Vec3 point, double duration) {
    auto* camera = component(entity);
    auto* value = impl_->entry(entity);
    if (camera == nullptr || value == nullptr) { return false; }
    if (!valid(camera->controller)) { return impl_->reject(value, CameraError::InvalidController); }
    const auto world = impl_->scene.world_matrix(entity);
    auto pose = world ? math::decompose(*world) : std::nullopt;
    if (!pose || !bounded(point)) { return impl_->reject(value, CameraError::InvalidTarget); }
    const auto delta = point - pose->position;
    const double distance = math::length(delta);
    if (!range(distance, minimum_size, largest_coordinate) ||
        ((camera->controller.mode == CameraMode::Orbit || camera->controller.mode == CameraMode::Follow) &&
         !range(distance, camera->controller.minimum_distance, camera->controller.maximum_distance))) {
        return impl_->reject(value, CameraError::InvalidTarget);
    }
    pose->rotation = math::look_rotation(delta);
    if (!transition(entity, *pose, duration)) { return false; }
    camera->controller.focus_point = point;
    camera->controller.target = {};
    if (camera->controller.mode == CameraMode::Follow) { camera->controller.mode = CameraMode::Orbit; }
    camera->controller.distance = std::clamp(distance, camera->controller.minimum_distance,
                                            camera->controller.maximum_distance);
    return true;
}

bool CameraService::fit(ecs::Entity entity, const math::Aabb& bounds, double padding, double duration) {
    auto* camera = component(entity);
    auto* value = impl_->entry(entity);
    if (camera == nullptr || value == nullptr) { return false; }
    if (!math::valid(bounds) || !bounded(bounds.min) || !bounded(bounds.max) || !range(padding, 1.0, 100.0)) {
        return impl_->reject(value, CameraError::InvalidBounds);
    }
    const auto world = impl_->scene.world_matrix(entity);
    auto pose = world ? math::decompose(*world) : std::nullopt;
    const auto viewport = resolve_viewport(camera->viewport, impl_->width, impl_->height);
    if (!pose || !viewport || !valid(camera->projection) || !valid(camera->controller) ||
        pose->scale.z <= minimum_size || std::abs(camera->projection.lens_shift.x) >= 1.0 ||
        std::abs(camera->projection.lens_shift.y) >= 1.0) {
        return impl_->reject(value, CameraError::InvalidBounds);
    }
    auto settings = camera->projection;
    const double aspect = settings.aspect_override > 0.0 ? settings.aspect_override :
        static_cast<double>(viewport->width) / static_cast<double>(viewport->height);
    const auto center = bounds.min * 0.5 + bounds.max * 0.5;
    const auto inverse_rotation = math::conjugate(math::normalized(pose->rotation));
    double distance = camera->controller.minimum_distance;
    double required_height = minimum_size;
    double minimum_z = largest_coordinate;
    for (const auto corner : box_corners(bounds)) {
        const auto offset = math::rotate(inverse_rotation, corner - center);
        const double x = offset.x / pose->scale.x;
        const double y = offset.y / pose->scale.y;
        const double horizontal = std::abs(x) / (1.0 - (x >= 0.0 ? settings.lens_shift.x : -settings.lens_shift.x));
        const double vertical = std::abs(y) / (1.0 - (y >= 0.0 ? settings.lens_shift.y : -settings.lens_shift.y));
        const double extent = std::max(horizontal / aspect, vertical) * padding;
        required_height = std::max(required_height, extent * 2.0);
        double depth = settings.near_plane * padding;
        if (settings.mode == ProjectionMode::Perspective) {
            depth = std::max(depth, extent / std::tan(settings.vertical_fov * 0.5));
        }
        distance = std::max(distance, offset.z + depth * pose->scale.z);
        minimum_z = std::min(minimum_z, offset.z);
    }
    if (settings.mode == ProjectionMode::Orthographic) {
        settings.orthographic_height = required_height;
    }
    settings.far_plane = std::max(settings.far_plane, (distance - minimum_z) / pose->scale.z * 1.01);
    if (!range(distance, camera->controller.minimum_distance, camera->controller.maximum_distance) ||
        !camera_projection(settings, *viewport)) {
        return impl_->reject(value, CameraError::InvalidBounds);
    }
    pose->position = center - forward(pose->rotation) * distance;
    if (!transition(entity, *pose, duration)) { return false; }
    camera->projection = settings;
    camera->controller.focus_point = center;
    camera->controller.target = {};
    if (camera->controller.mode == CameraMode::Follow) { camera->controller.mode = CameraMode::Orbit; }
    camera->controller.distance = distance;
    return true;
}

bool CameraService::fit_entity(ecs::Entity entity, ecs::Entity target, double padding, double duration) {
    const auto bounds = impl_->scene.subtree_bounds(target);
    return bounds ? fit(entity, *bounds, padding, duration) : impl_->reject(impl_->entry(entity), CameraError::InvalidBounds);
}

bool CameraService::transition(ecs::Entity entity, const math::Transform& destination, double duration) {
    auto* value = impl_->entry(entity);
    if (value == nullptr || component(entity) == nullptr) { return false; }
    if (!range(duration, 0.0, 600.0) || !valid_pose(destination)) {
        return impl_->reject(value, CameraError::InvalidTransform);
    }
    const auto world = impl_->scene.world_matrix(entity);
    const auto source = world ? math::decompose(*world) : std::nullopt;
    if (!source) { return impl_->reject(value, CameraError::InvalidTransform); }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (std::signbit(source->scale[axis]) != std::signbit(destination.scale[axis])) {
            return impl_->reject(value, CameraError::InvalidTransform);
        }
    }
    if (duration == 0.0) {
        if (!impl_->write_pose(*value, destination)) { return false; }
        value->transition.active = false;
        impl_->initialize_angles(*value, destination);
        value->velocity = {};
        return true;
    }
    value->transition = {*source, destination, 0.0, duration, true};
    return true;
}

bool CameraService::cancel_transition(ecs::Entity entity) noexcept {
    auto* value = impl_->entry(entity);
    if (value == nullptr) { return false; }
    value->transition.active = false;
    return true;
}

bool CameraService::shake(ecs::Entity entity, CameraShake effect) {
    auto* value = impl_->entry(entity);
    if (value == nullptr || value->shakes.size() >= impl_->limits.shakes_per_camera ||
        !range(effect.duration, 0.001, 60.0) || !range(effect.frequency, 0.0, 1000.0) ||
        !bounded(effect.translation, impl_->limits.maximum_effect_translation) ||
        !bounded(effect.rotation, impl_->limits.maximum_effect_rotation)) {
        return false;
    }
    value->shakes.push_back({effect, 0.0});
    return true;
}

bool CameraService::recoil(ecs::Entity entity, CameraRecoil effect) {
    auto* value = impl_->entry(entity);
    if (value == nullptr || !range(effect.half_life, 0.001, 60.0) ||
        !bounded(effect.translation, impl_->limits.maximum_effect_translation) ||
        !bounded(effect.rotation, impl_->limits.maximum_effect_rotation)) {
        return false;
    }
    value->recoil_position = math::clamp_length(value->recoil_position + effect.translation,
                                               impl_->limits.maximum_effect_translation);
    value->recoil_rotation = math::clamp_length(value->recoil_rotation + effect.rotation,
                                               impl_->limits.maximum_effect_rotation);
    value->recoil_half_life = effect.half_life;
    return true;
}

bool CameraService::clear_effects(ecs::Entity entity) noexcept {
    auto* value = impl_->entry(entity);
    if (value == nullptr) { return false; }
    value->shakes.clear();
    value->recoil_position = {};
    value->recoil_rotation = {};
    value->sampled_effect = {};
    return true;
}

void CameraService::update(double delta_seconds, const input::InputSystem* input) {
    const double delta = std::isfinite(delta_seconds) ?
        std::clamp(delta_seconds, 0.0, impl_->limits.maximum_delta_seconds) : 0.0;
    increment(impl_->statistics.updates);
    (void)impl_->scene.update();
    impl_->sync_entries();
    const auto controls = sample_input(input);
    for (auto& value : impl_->entries) {
        auto* camera = component(value.entity);
        value.error = CameraError::None;
        if (camera == nullptr || !camera->enabled || !impl_->scene.effective_enabled(value.entity)) {
            value.error = CameraError::Disabled;
            continue;
        }
        const bool moved = value.transition.active ? impl_->advance_transition(value, delta) :
            impl_->controller(value, *camera, controls, delta);
        value.sampled_effect = impl_->effects(value, delta);
        const bool refreshed = moved && impl_->build_view(value, *camera, value.sampled_effect);
        if (!refreshed) {
            increment(impl_->statistics.rejected_views);
            if (value.last) { value.last->stale = true; }
        }
    }
    impl_->publish_views();
}

void CameraService::refresh_views() {
    increment(impl_->statistics.view_refreshes);
    (void)impl_->scene.update();
    impl_->sync_entries();
    for (auto& value : impl_->entries) {
        const auto* camera = component(value.entity);
        value.error = CameraError::None;
        if (camera == nullptr || !camera->enabled || !impl_->scene.effective_enabled(value.entity)) {
            value.error = CameraError::Disabled;
            continue;
        }
        if (!impl_->build_view(value, *camera, value.sampled_effect)) {
            increment(impl_->statistics.rejected_views);
            if (value.last) { value.last->stale = true; }
        }
    }
    impl_->publish_views();
}

bool CameraService::rebase_cached_world(math::Vec3 offset) {
    if (!bounded(offset) || impl_->registry.iterating()) { return false; }
    impl_->sync_entries();
    for (const auto& value : impl_->entries) {
        const auto* camera = component(value.entity);
        if (camera == nullptr) { return false; }
        if (!camera->controller.target && !bounded(camera->controller.focus_point - offset)) {
            return false;
        }
        if (value.transition.active &&
            (!bounded(value.transition.source.position - offset) ||
             !bounded(value.transition.destination.position - offset))) {
            return false;
        }
    }
    for (auto& value : impl_->entries) {
        auto* camera = component(value.entity);
        if (!camera->controller.target) {
            camera->controller.focus_point -= offset;
            (void)impl_->registry.mark_changed<CameraComponent>(value.entity);
        }
        if (value.transition.active) {
            value.transition.source.position -= offset;
            value.transition.destination.position -= offset;
        }
        value.last.reset();
    }
    impl_->published.clear();
    return true;
}

const CameraView* CameraService::current_view() const noexcept { return view(0); }

const CameraView* CameraService::view(std::size_t slot) const noexcept {
    const auto found = std::find_if(impl_->published.begin(), impl_->published.end(),
        [slot](const CameraView& value) { return value.slot == slot; });
    return found == impl_->published.end() ? nullptr : &*found;
}

const CameraView* CameraService::snapshot(ecs::Entity entity) const noexcept {
    const auto* value = impl_->entry(entity);
    return value != nullptr && value->last ? &*value->last : nullptr;
}

std::span<const CameraView> CameraService::views() const noexcept { return impl_->published; }

std::optional<math::Ray> CameraService::ray(math::Vec2 pixel, std::size_t slot) const noexcept {
    const auto* current = view(slot);
    return current != nullptr ? camera_ray(*current, pixel) : std::nullopt;
}

CameraError CameraService::last_error(ecs::Entity entity) const noexcept {
    const auto* value = impl_->entry(entity);
    return value != nullptr ? value->error : CameraError::MissingCamera;
}

CameraStatistics CameraService::statistics() const noexcept {
    auto result = impl_->statistics;
    result.cameras = impl_->entries.size();
    result.active_views = impl_->published.size();
    for (const auto& camera_view : impl_->published) { result.stale_views += camera_view.stale ? 1U : 0U; }
    for (const auto& value : impl_->entries) {
        result.transitions += value.transition.active ? 1U : 0U;
        result.shakes += value.shakes.size();
    }
    return result;
}

void register_camera(core::Kernel& kernel) {
    if (!kernel.register_module({"render.camera", "Scene cameras, controllers and viewport picking", 1})) { return; }
    auto* scene_service = kernel.services().find<scene::Scene>();
    if (scene_service == nullptr) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "render.camera", "Scene service is required");
        return;
    }
    auto& service = kernel.services().emplace<CameraService>(*scene_service);
    if (const auto* images = kernel.services().find<ImageService>()) {
        (void)service.set_framebuffer_size(images->framebuffer().width(), images->framebuffer().height());
    }
    CameraComponent initial;
    initial.controller.mode = CameraMode::FreeFly;
    math::Transform pose;
    pose.position = {0.0, 2.0, 5.0};
    const auto initial_camera = service.create("MainCamera", pose, initial);
    if (initial_camera) { (void)service.look_at(initial_camera, {}); }
    (void)kernel.add_system({"camera.update", core::Phase::Visibility, -8000, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& cameras = host.services().get<CameraService>();
            if (const auto* images = host.services().find<ImageService>()) {
                (void)cameras.set_framebuffer_size(images->framebuffer().width(), images->framebuffer().height());
            }
            cameras.update(frame.delta_seconds, host.services().find<input::InputSystem>());
            const auto status = cameras.statistics();
            host.metric("camera.views", static_cast<double>(status.active_views));
            host.metric("camera.stale_views", static_cast<double>(status.stale_views));
            host.metric("camera.rejected_views", static_cast<double>(status.rejected_views));
        });
    (void)kernel.add_command("camera.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2) { return {false, "usage: camera.create name [perspective|orthographic]"}; }
        CameraComponent settings;
        if (args.size() == 2) {
            if (args[1] == "orthographic") { settings.projection.mode = ProjectionMode::Orthographic; }
            else if (args[1] != "perspective") { return {false, "projection must be perspective or orthographic"}; }
        }
        auto& cameras = host.services().get<CameraService>();
        const auto entity = cameras.create(args[0], {}, settings);
        return entity ? core::CommandResult{true, ecs::format_entity(entity)} :
                        core::CommandResult{false, "camera creation rejected"};
    }, "Create a scene camera and return its entity handle");
    (void)kernel.add_command("camera.active", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2) { return {false, "usage: camera.active entity [slot]"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const auto slot = args.size() == 2 ? slot_number(args[1]) : std::optional<std::size_t>{0};
        if (!entity || !slot) { return {false, "invalid entity or view slot"}; }
        const bool changed = host.services().get<CameraService>().set_active(*entity, *slot);
        return {changed, changed ? "active camera changed" : "camera or view slot is unavailable"};
    }, "Assign a camera to an active view slot");
    (void)kernel.add_command("camera.mode", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 3) { return {false, "usage: camera.mode entity fixed|fly|orbit|follow [target]"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const auto mode = mode_named(args[1]);
        auto& cameras = host.services().get<CameraService>();
        const auto* camera = entity ? cameras.component(*entity) : nullptr;
        if (camera == nullptr || !mode) { return {false, "invalid camera or controller mode"}; }
        auto settings = camera->controller;
        settings.mode = *mode;
        if (args.size() == 3) {
            const auto target = ecs::parse_entity(args[2]);
            if (!target) { return {false, "invalid target entity"}; }
            settings.target = *target;
        }
        if (*mode == CameraMode::Follow && !settings.target) { return {false, "follow mode requires a target"}; }
        const bool changed = cameras.set_controller(*entity, settings);
        return {changed, changed ? "camera controller changed" : "controller settings rejected"};
    }, "Set the camera controller and optional scene target");
    (void)kernel.add_command("camera.look", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4 && args.size() != 5) { return {false, "usage: camera.look entity x y z [seconds]"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const auto x = number(args[1]);
        const auto y = number(args[2]);
        const auto z = number(args[3]);
        const auto duration = args.size() == 5 ? number(args[4]) : std::optional<double>{0.0};
        if (!entity || !x || !y || !z || !duration) { return {false, "invalid camera, coordinates or duration"}; }
        auto& cameras = host.services().get<CameraService>();
        const bool changed = cameras.focus(*entity, {*x, *y, *z}, *duration);
        return {changed, changed ? "camera focus changed" : std::string(describe(cameras.last_error(*entity)))};
    }, "Focus a camera on a world point, optionally using a smooth transition");
    (void)kernel.add_command("camera.fit", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 4) { return {false, "usage: camera.fit camera target [padding] [seconds]"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const auto target = ecs::parse_entity(args[1]);
        const auto padding = args.size() >= 3 ? number(args[2]) : std::optional<double>{1.1};
        const auto duration = args.size() == 4 ? number(args[3]) : std::optional<double>{0.0};
        if (!entity || !target || !padding || !duration) { return {false, "invalid camera, target or fit parameters"}; }
        auto& cameras = host.services().get<CameraService>();
        const bool changed = cameras.fit_entity(*entity, *target, *padding, *duration);
        return {changed, changed ? "camera fitted to target bounds" : std::string(describe(cameras.last_error(*entity)))};
    }, "Fit a camera to a target subtree's world bounds");
    (void)kernel.add_command("camera.ray", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 3) { return {false, "usage: camera.ray pixel_x pixel_y [slot]"}; }
        const auto x = number(args[0]);
        const auto y = number(args[1]);
        const auto slot = args.size() == 3 ? slot_number(args[2]) : std::optional<std::size_t>{0};
        if (!x || !y || !slot) { return {false, "invalid pixel coordinates or slot"}; }
        const auto ray = host.services().get<CameraService>().ray({*x, *y}, *slot);
        if (!ray) { return {false, "view is unavailable or pixel is outside its viewport"}; }
        return {true, "origin " + std::to_string(ray->origin.x) + " " + std::to_string(ray->origin.y) + " " +
            std::to_string(ray->origin.z) + " direction " + std::to_string(ray->direction.x) + " " +
            std::to_string(ray->direction.y) + " " + std::to_string(ray->direction.z)};
    }, "Construct a world ray from continuous top-left pixel coordinates");
}

} // namespace ge::render
