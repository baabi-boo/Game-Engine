#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>
#include <ge/render/image.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }
namespace ge::input { class InputSystem; }

namespace ge::render {

enum class ProjectionMode : std::uint8_t { Perspective, Orthographic };
enum class ViewportMode : std::uint8_t { Framebuffer, Pixels, Normalized };
enum class CameraMode : std::uint8_t { Fixed, FreeFly, Orbit, Follow };
enum class LookInputMode : std::uint8_t { Automatic, Relative, Rate };
enum class CameraError : std::uint8_t {
    None,
    MissingCamera,
    Capacity,
    InvalidProjection,
    InvalidViewport,
    InvalidController,
    InvalidTransform,
    InvalidTarget,
    InvalidBounds,
    SceneRejected,
    Disabled,
    PhysicsOwned
};

[[nodiscard]] std::string_view describe(CameraError error) noexcept;

struct CameraProjection {
    ProjectionMode mode{ProjectionMode::Perspective};
    double vertical_fov{math::radians(60.0)};
    double orthographic_height{10.0};
    double near_plane{0.05};
    double far_plane{1000.0};
    double aspect_override{};
    math::Vec2 lens_shift{};
    math::ClipDepth clip_depth{math::ClipDepth::zero_to_one};
};

struct CameraViewport {
    ViewportMode mode{ViewportMode::Framebuffer};
    // Pixels and normalized rectangles both use top-left x/y; z/w are width/height.
    math::Vec4 rectangle{0.0, 0.0, 1.0, 1.0};
};

// Input actions: move (+Y forward), look (mouse delta or angular-rate stick),
// camera.lift (rate), camera.zoom (relative steps), and sprint. Only the selected
// input view responds; follow targets must not be the camera or its descendants.
struct CameraController {
    CameraMode mode{CameraMode::Fixed};
    bool input_enabled{true};
    LookInputMode look_input{LookInputMode::Automatic};
    ecs::Entity target{};
    math::Vec3 focus_point{};
    math::Vec3 follow_offset{0.0, 2.0, 5.0};
    bool follow_target_rotation{true};
    double move_speed{5.0};
    double sprint_multiplier{4.0};
    double mouse_sensitivity{0.0025};
    double look_rate{2.0};
    double pan_speed{1.0};
    double zoom_speed{0.15};
    double distance{5.0};
    double minimum_distance{0.1};
    double maximum_distance{1000000.0};
    double minimum_pitch{math::radians(-89.0)};
    double maximum_pitch{math::radians(89.0)};
    double movement_half_life{0.05};
    double rotation_half_life{0.04};
    double follow_half_life{0.15};
};

struct CameraComponent {
    CameraProjection projection{};
    CameraViewport viewport{};
    CameraController controller{};
    bool enabled{true};
    std::uint64_t visibility_mask{~std::uint64_t{0}};
};

// All matrices are column-major and multiply column vectors. The exact Scene
// world matrix is inverted; parent shear/scale is not silently replaced by TRS.
struct CameraView {
    ecs::Entity entity{};
    std::size_t slot{};
    math::Mat4 view{};
    math::Mat4 projection{};
    math::Mat4 view_projection{};
    math::Mat4 inverse_view{};
    math::Mat4 inverse_projection{};
    math::Mat4 inverse_view_projection{};
    math::Vec3 position{};
    math::Vec3 forward{0.0, 0.0, -1.0};
    math::Vec3 up{0.0, 1.0, 0.0};
    Rect viewport{};
    math::Frustum frustum{};
    math::ClipDepth clip_depth{math::ClipDepth::zero_to_one};
    ProjectionMode projection_mode{ProjectionMode::Perspective};
    double near_plane{};
    double far_plane{};
    std::uint64_t visibility_mask{};
    std::uint64_t revision{};
    bool stale{};
};

struct CameraShake {
    double duration{0.3};
    double frequency{20.0};
    math::Vec3 translation{0.02, 0.02, 0.01};
    math::Vec3 rotation{0.01, 0.01, 0.005};
    std::uint64_t seed{1};
};

struct CameraRecoil {
    math::Vec3 translation{};
    math::Vec3 rotation{};
    double half_life{0.08};
};

struct CameraLimits {
    std::size_t cameras{256};
    std::size_t views{8};
    std::size_t shakes_per_camera{8};
    double maximum_delta_seconds{0.25};
    double maximum_effect_translation{2.0};
    double maximum_effect_rotation{0.5};
};

struct CameraStatistics {
    std::size_t cameras{};
    std::size_t active_views{};
    std::size_t stale_views{};
    std::size_t transitions{};
    std::size_t shakes{};
    std::uint64_t updates{};
    std::uint64_t rejected_views{};
    std::uint64_t view_refreshes{};
};

[[nodiscard]] bool valid(const CameraProjection& projection) noexcept;
[[nodiscard]] bool valid(const CameraViewport& viewport) noexcept;
[[nodiscard]] bool valid(const CameraController& controller) noexcept;
[[nodiscard]] std::optional<Rect> resolve_viewport(const CameraViewport& viewport,
                                                   std::uint32_t width,
                                                   std::uint32_t height) noexcept;
[[nodiscard]] std::optional<math::Mat4> camera_projection(const CameraProjection& settings,
                                                         Rect viewport) noexcept;
// Pixel coordinates are continuous top-left coordinates. Pass x+0.5,y+0.5 for
// texel centres. Rays originate on the near plane. Unprojection accepts depth
// in [0,1]; projection may return coordinates outside the viewport/depth range.
[[nodiscard]] std::optional<math::Ray> camera_ray(const CameraView& view,
                                                math::Vec2 pixel,
                                                bool require_inside = true) noexcept;
[[nodiscard]] std::optional<math::Vec3> camera_unproject(const CameraView& view,
                                                       math::Vec3 pixel_depth) noexcept;
[[nodiscard]] std::optional<math::Vec3> camera_project(const CameraView& view,
                                                     math::Vec3 world) noexcept;

// Single-writer service borrowing Scene and its Registry. Snapshot pointers and
// spans expire on update, detach or view edits. Invalid updates retain the last valid view
// with stale=true; disabled/dead cameras are removed from published views.
class CameraService final {
public:
    explicit CameraService(scene::Scene& scene, CameraLimits limits = {});
    ~CameraService();
    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;
    CameraService(CameraService&&) = delete;
    CameraService& operator=(CameraService&&) = delete;

    [[nodiscard]] ecs::Entity create(std::string_view name = "Camera",
                                     const math::Transform& transform = {},
                                     CameraComponent settings = {});
    bool attach(ecs::Entity entity, CameraComponent settings = {});
    bool detach(ecs::Entity entity);
    [[nodiscard]] CameraComponent* component(ecs::Entity entity) noexcept;
    [[nodiscard]] const CameraComponent* component(ecs::Entity entity) const noexcept;
    bool set_projection(ecs::Entity entity, const CameraProjection& projection);
    bool set_viewport(ecs::Entity entity, const CameraViewport& viewport);
    bool set_controller(ecs::Entity entity, const CameraController& controller);
    bool set_active(ecs::Entity entity, std::size_t slot = 0);
    bool clear_view(std::size_t slot = 0) noexcept;
    [[nodiscard]] ecs::Entity active_entity(std::size_t slot = 0) const noexcept;
    bool set_input_view(std::size_t slot) noexcept;
    bool set_framebuffer_size(std::uint32_t width, std::uint32_t height) noexcept;

    bool look_at(ecs::Entity entity, math::Vec3 point,
                 math::Vec3 up = {0.0, 1.0, 0.0});
    // World-point focus/fit releases a target; Follow becomes Orbit. Fit expands
    // the far plane as needed and rejects sheared poses or out-of-range distances.
    bool focus(ecs::Entity entity, math::Vec3 point, double duration = 0.0);
    bool fit(ecs::Entity entity, const math::Aabb& bounds,
             double padding = 1.1, double duration = 0.0);
    bool fit_entity(ecs::Entity entity, ecs::Entity target,
                    double padding = 1.1, double duration = 0.0);
    // Transitions require decomposable world endpoints and preserve finite scale.
    bool transition(ecs::Entity entity, const math::Transform& destination,
                    double duration);
    bool cancel_transition(ecs::Entity entity) noexcept;
    bool shake(ecs::Entity entity, CameraShake effect = {});
    bool recoil(ecs::Entity entity, CameraRecoil effect);
    bool clear_effects(ecs::Entity entity) noexcept;

    void update(double delta_seconds, const input::InputSystem* input = nullptr);
    // Rebuild matrices from current Scene/projection state and the last sampled
    // shake/recoil transform. Does not advance controls, transitions or effects.
    // Late follow-target movement still needs the next normal controller update.
    void refresh_views();
    // Apply once after an external world-origin shift. Only tracked cameras'
    // world focus points and active transition endpoints move; Scene poses do not.
    // Target-local offsets and direction/velocity vectors remain unchanged.
    // Published snapshots are invalidated until refresh_views() or update().
    [[nodiscard]] bool rebase_cached_world(math::Vec3 offset);
    [[nodiscard]] const CameraView* current_view() const noexcept;
    [[nodiscard]] const CameraView* view(std::size_t slot) const noexcept;
    [[nodiscard]] const CameraView* snapshot(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::span<const CameraView> views() const noexcept;
    [[nodiscard]] std::optional<math::Ray> ray(math::Vec2 pixel,
                                              std::size_t slot = 0) const noexcept;
    [[nodiscard]] CameraError last_error(ecs::Entity entity) const noexcept;
    [[nodiscard]] CameraStatistics statistics() const noexcept;
    [[nodiscard]] const CameraLimits& limits() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_camera(core::Kernel& kernel);

} // namespace ge::render
