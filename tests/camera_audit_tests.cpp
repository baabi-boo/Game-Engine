#include "test_support.hpp"

#include <ge/input/actions.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/render/camera.hpp>
#include <ge/scene/scene.hpp>

#include <cmath>
#include <limits>

namespace {

using namespace ge;

struct Fixture {
    ecs::Registry registry{64};
    scene::Scene scene{registry};
    render::CameraService cameras;

    explicit Fixture(render::CameraLimits limits = {}) : cameras(scene, limits) {}
};

void near(double actual, double expected, double tolerance = 1.0e-8) {
    CHECK(std::isfinite(actual));
    CHECK(std::abs(actual - expected) <= tolerance);
}

void selection_publishes_the_selected_snapshot() {
    Fixture fixture;
    const auto first = fixture.cameras.create("first");
    math::Transform pose;
    pose.position = {12.0, 0.0, 0.0};
    const auto second = fixture.cameras.create("second", pose);
    CHECK(first && second);
    fixture.cameras.refresh_views();
    CHECK(fixture.cameras.current_view()->entity == first);
    CHECK(fixture.cameras.set_active(second));
    CHECK(fixture.cameras.active_entity() == second);
    CHECK(fixture.cameras.current_view() != nullptr);
    CHECK(fixture.cameras.current_view()->entity == second);
    const auto ray = fixture.cameras.ray({320.0, 180.0});
    CHECK(ray);
    near(ray->origin.x, 12.0);
    CHECK(fixture.cameras.set_active(first, 1));
    CHECK(fixture.cameras.view(1)->entity == first);
    CHECK(fixture.cameras.clear_view(1));
    CHECK(fixture.cameras.view(1) == nullptr);

    fixture.cameras.component(second)->enabled = false;
    CHECK(fixture.cameras.set_active(second));
    CHECK(fixture.cameras.current_view() == nullptr);
    CHECK(!fixture.cameras.ray({320.0, 180.0}));
    CHECK(fixture.cameras.set_active(first));
    CHECK(fixture.scene.set_enabled_recursive(first, false));
    CHECK(fixture.cameras.set_active(first));
    CHECK(fixture.cameras.views().empty());
}

void configuration_edits_mark_snapshots_stale_until_refreshed() {
    Fixture fixture;
    const auto camera = fixture.cameras.create();
    CHECK(camera);
    fixture.cameras.refresh_views();
    const auto before = *fixture.cameras.current_view();
    auto projection = fixture.cameras.component(camera)->projection;
    projection.vertical_fov = math::radians(40.0);
    CHECK(fixture.cameras.set_projection(camera, projection));
    CHECK(fixture.cameras.current_view()->stale);
    CHECK(fixture.cameras.snapshot(camera)->stale);
    fixture.cameras.refresh_views();
    CHECK(!fixture.cameras.current_view()->stale);
    CHECK(fixture.cameras.current_view()->revision > before.revision);
    CHECK(fixture.cameras.current_view()->projection(0, 0) > before.projection(0, 0));

    render::CameraViewport viewport;
    viewport.mode = render::ViewportMode::Normalized;
    viewport.rectangle = {0.5, 0.0, 0.5, 1.0};
    CHECK(fixture.cameras.set_viewport(camera, viewport));
    CHECK(fixture.cameras.current_view()->stale);
    fixture.cameras.refresh_views();
    CHECK(fixture.cameras.current_view()->viewport.x == 320);
    CHECK(fixture.cameras.set_framebuffer_size(800, 600));
    CHECK(fixture.cameras.current_view()->stale);
    fixture.cameras.refresh_views();
    CHECK(fixture.cameras.current_view()->viewport.x == 400);
    CHECK(fixture.cameras.current_view()->viewport.width == 400);
    CHECK(fixture.cameras.current_view()->viewport.height == 600);
    CHECK(fixture.cameras.set_framebuffer_size(800, 600));
    CHECK(!fixture.cameras.current_view()->stale);

    const auto valid = *fixture.cameras.current_view();
    projection.near_plane = -1.0;
    CHECK(!fixture.cameras.set_projection(camera, projection));
    CHECK(!fixture.cameras.current_view()->stale);
    fixture.cameras.component(camera)->projection = projection;
    fixture.cameras.refresh_views();
    CHECK(fixture.cameras.current_view()->stale);
    CHECK(fixture.cameras.current_view()->revision == valid.revision);
    CHECK(fixture.cameras.current_view()->projection == valid.projection);
}

void dead_and_removed_cameras_release_capacity_before_creation() {
    render::CameraLimits limits;
    limits.cameras = 1;
    Fixture fixture(limits);
    const auto old = fixture.cameras.create();
    CHECK(old);
    fixture.cameras.refresh_views();
    CHECK(fixture.scene.destroy(old));
    CHECK(!fixture.cameras.active_entity());
    const auto replacement = fixture.cameras.create();
    CHECK(replacement);
    CHECK(replacement != old);
    CHECK(fixture.cameras.active_entity() == replacement);
    CHECK(fixture.cameras.current_view() == nullptr);
    fixture.cameras.refresh_views();
    CHECK(fixture.cameras.current_view()->entity == replacement);

    CHECK(fixture.registry.remove<render::CameraComponent>(replacement));
    const auto attached = fixture.scene.create("attached");
    CHECK(fixture.cameras.attach(attached));
    CHECK(fixture.cameras.set_active(attached));
    fixture.cameras.refresh_views();
    CHECK(fixture.cameras.current_view()->entity == attached);
    CHECK(fixture.cameras.detach(attached));
    CHECK(fixture.cameras.views().empty());
}

void far_origin_views_keep_inverses_rays_and_frusta() {
    for (const auto depth : {math::ClipDepth::zero_to_one, math::ClipDepth::negative_one_to_one}) {
        for (const auto mode : {render::ProjectionMode::Perspective, render::ProjectionMode::Orthographic}) {
            Fixture fixture;
            math::Transform pose;
            pose.position = {1.0e11, -2.0e11, 3.0e11};
            render::CameraComponent settings;
            settings.projection.mode = mode;
            settings.projection.clip_depth = depth;
            const auto camera = fixture.cameras.create("distant", pose, settings);
            CHECK(camera);
            fixture.cameras.refresh_views();
            const auto* view = fixture.cameras.current_view();
            CHECK(view != nullptr);
            CHECK(!view->stale);
            CHECK(fixture.cameras.last_error(camera) == render::CameraError::None);
            CHECK(math::finite(view->inverse_view_projection));
            const auto ray = fixture.cameras.ray({320.0, 180.0});
            CHECK(ray);
            near(ray->origin.x, pose.position.x, 0.001);
            near(ray->origin.y, pose.position.y, 0.001);
            near(ray->origin.z - pose.position.z, -settings.projection.near_plane, 0.001);
            near(ray->direction.z, -1.0, 0.001);
            CHECK(math::contains(view->frustum, pose.position + math::Vec3{0.0, 0.0, -10.0}));
            CHECK(!math::contains(view->frustum, pose.position + math::Vec3{0.0, 0.0, 10.0}));
            const auto projected = render::camera_project(*view, pose.position + math::Vec3{0.0, 0.0, -10.0});
            CHECK(projected);
            near(projected->x, 320.0, 0.001);
            near(projected->y, 180.0, 0.001);
        }
    }
}

void shared_refresh_preserves_exact_affine_pose_and_rejects_atomically() {
    render::CameraView view;
    const auto projection = render::camera_projection({}, {0, 0, 640, 360});
    CHECK(projection);
    view.projection = *projection;
    view.revision = 73;
    view.stale = true;
    auto world = math::translation_matrix({4.0, -5.0, 6.0});
    world(0, 1) = 0.25;
    world(1, 1) = 2.0;
    CHECK(render::refresh_camera_matrices(view, world));
    CHECK(view.inverse_view == world);
    CHECK(math::almost_equal(view.view * world, math::Mat4{}));
    CHECK(view.revision == 73);
    CHECK(view.stale);
    const auto before = view;
    world(0, 0) = std::numeric_limits<double>::quiet_NaN();
    CHECK(!render::refresh_camera_matrices(view, world));
    CHECK(view.view == before.view);
    CHECK(view.inverse_view_projection == before.inverse_view_projection);
    CHECK(view.frustum.corners == before.frustum.corners);
    CHECK(view.position == before.position);
}

void fixed_mode_reentry_resets_controller_angles_and_momentum() {
    Fixture fixture;
    input::InputSystem input;
    const auto context = input.create_context("camera");
    CHECK(input.define_action(context, {"move", input::ActionKind::Vector2}));
    input::Binding binding;
    binding.action = "move";
    binding.kind = input::ActionKind::Vector2;
    binding.control_count = 4;
    binding.controls = {input::key(65), input::key(68), input::key(83), input::key(87)};
    binding.response.deadzone = 0.0;
    CHECK(input.bind(context, binding) != 0);
    render::CameraComponent settings;
    settings.controller.mode = render::CameraMode::FreeFly;
    settings.controller.rotation_half_life = 0.0;
    const auto camera = fixture.cameras.create("controlled", {}, settings);
    const auto inactive = fixture.cameras.create("inactive", {}, settings);
    CHECK(camera && inactive);
    input::InputEvent event;
    event.device = input.keyboard();
    event.kind = input::EventKind::ButtonDown;
    event.code = 87;
    CHECK(input.enqueue(event));
    input.update(1, 0.1);
    fixture.cameras.update(0.1, &input);
    CHECK(fixture.cameras.snapshot(camera)->position.z < 0.0);
    near(fixture.cameras.snapshot(inactive)->position.z, 0.0);

    fixture.cameras.component(camera)->controller.mode = render::CameraMode::Fixed;
    fixture.cameras.update(0.1);
    auto pose = *fixture.scene.world_transform(camera);
    pose.rotation = math::look_rotation({1.0, 0.0, 0.0});
    CHECK(fixture.scene.set_world_transform(camera, pose));
    fixture.cameras.component(camera)->controller.mode = render::CameraMode::FreeFly;
    fixture.cameras.update(0.1);
    near(fixture.cameras.snapshot(camera)->forward.x, 1.0);
    near(fixture.cameras.snapshot(camera)->position.z, pose.position.z);
}

void look_at_respects_physics_pose_ownership() {
    Fixture fixture;
    const auto camera = fixture.cameras.create();
    CHECK(camera);
    fixture.cameras.refresh_views();
    const auto before = *fixture.scene.world_matrix(camera);
    fixture.registry.emplace<physics::BodyComponent>(camera);
    CHECK(!fixture.cameras.look_at(camera, {1.0, 0.0, 0.0}));
    CHECK(fixture.cameras.last_error(camera) == render::CameraError::PhysicsOwned);
    CHECK(*fixture.scene.world_matrix(camera) == before);
}

} // namespace

int main() {
    return test::run("camera audit", {
        {"selection publishes selected snapshot", selection_publishes_the_selected_snapshot},
        {"configuration edits invalidate snapshots", configuration_edits_mark_snapshots_stale_until_refreshed},
        {"dead cameras release capacity", dead_and_removed_cameras_release_capacity_before_creation},
        {"far-origin views remain usable", far_origin_views_keep_inverses_rays_and_frusta},
        {"shared refresh is affine and atomic", shared_refresh_preserves_exact_affine_pose_and_rejects_atomically},
        {"fixed mode resets controller state", fixed_mode_reentry_resets_controller_angles_and_momentum},
        {"look-at respects physics ownership", look_at_respects_physics_pose_ownership},
    });
}
