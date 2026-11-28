#include "test_support.hpp"
#include "modules.hpp"

#include <ge/core/runtime.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/input/actions.hpp>
#include <ge/physics/collision.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/render/batching.hpp>
#include <ge/render/camera.hpp>
#include <ge/render/graph.hpp>
#include <ge/render/image.hpp>
#include <ge/render/material.hpp>
#include <ge/render/postprocess.hpp>
#include <ge/render/rasterizer.hpp>
#include <ge/render/visibility.hpp>
#include <ge/scene/scene.hpp>
#include <ge/spatial/index.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t image_width = 64;
constexpr std::uint32_t image_height = 48;
constexpr double step_seconds = 1.0 / 60.0;

void install_runtime(ge::core::Kernel& kernel) {
    CHECK(kernel.config().set("image.width", std::to_string(image_width)));
    CHECK(kernel.config().set("image.height", std::to_string(image_height)));
    CHECK(kernel.config().set("jobs.workers", "1"));
    ge::application::install_modules(kernel);
    const auto& target = kernel.services().get<ge::render::ImageService>().framebuffer();
    CHECK(target.valid());
    CHECK(target.width() == image_width);
    CHECK(target.height() == image_height);
}

void check_near(double actual, double expected) {
    CHECK(std::isfinite(actual));
    CHECK(std::abs(actual - expected) < 1.0e-8);
}

void check_system_health(const ge::core::Kernel& kernel) {
    for (const auto& diagnostic : kernel.diagnostics()) {
        CHECK_MSG(diagnostic.severity != ge::core::Diagnostic::Severity::Error,
                  diagnostic.subsystem + ": " + diagnostic.message);
    }
    for (const auto& system : kernel.system_statistics()) {
        CHECK_MSG(system.failures == 0, system.name);
        if (system.enabled) {
            CHECK_MSG(system.invocations == kernel.frame().tick, system.name);
        }
    }
}

void check_render_health(const ge::core::Kernel& kernel) {
    const auto& graph = kernel.services().get<ge::render::RenderGraph>();
    CHECK_MSG(graph.last_result().error == ge::render::GraphError::None,
              graph.last_result().message);
    CHECK(graph.statistics().frame == kernel.frame().tick);
    CHECK(graph.statistics().completed_passes > 0);
    CHECK(graph.statistics().failed_passes == 0);
    CHECK(graph.statistics().skipped_passes == 0);
    const auto& visibility = kernel.services().get<ge::render::VisibilityService>().statistics();
    CHECK(visibility.error == ge::render::VisibilityError::None);
    CHECK(visibility.raster_error == ge::render::RasterError::None);
    CHECK(visibility.complete);
    CHECK(kernel.services().get<ge::render::SoftwareRenderer>().last_error() ==
          ge::render::RasterError::None);
    CHECK(kernel.services().get<ge::render::BatchingService>().statistics().error ==
          ge::render::BatchError::None);
    const auto& postprocess = kernel.services().get<ge::render::PostprocessService>();
    CHECK(postprocess.last_error() == ge::render::PostprocessError::None);
    CHECK(postprocess.attachment_error() == ge::render::PostprocessError::None);
}

void tick(ge::core::Kernel& kernel) {
    CHECK_MSG(kernel.tick(step_seconds), kernel.report_json());
    check_system_health(kernel);
    check_render_health(kernel);
}

void require_before(const std::vector<std::string>& order,
                    std::string_view first, std::string_view second) {
    const auto first_position = std::find(order.begin(), order.end(), first);
    const auto second_position = std::find(order.begin(), order.end(), second);
    CHECK_MSG(first_position != order.end(), first);
    CHECK_MSG(second_position != order.end(), second);
    CHECK_MSG(first_position < second_position,
              std::string(first) + " must precede " + std::string(second));
}

void all_modules_install_and_enabled_systems_run() {
    ge::core::Kernel kernel;
    install_runtime(kernel);

    // Required names catch missing registrations without forbidding new adapters.
    constexpr std::string_view required_modules[]{
        "ecs.registry", "jobs.executor", "io.filesystem", "scene.scene",
        "input.actions", "assets.catalog", "geometry.mesh", "render.image",
        "spatial.index", "physics.collision", "physics.dynamics", "animation.skeleton",
        "audio.mixer", "navigation.pathfinding", "render.material", "render.camera",
        "assets.streaming", "render.rasterizer", "physics.character", "physics.vehicle",
        "animation.blend", "audio.spatial", "navigation.navmesh", "scene.partition",
        "terrain.heightfield", "terrain.vegetation", "render.lighting", "render.graph",
        "effects.particles", "physics.constraints", "scene.serialization", "script.vm",
        "gameplay.interaction", "core.signals", "animation.curves", "config.settings",
        "text.localization", "network.protocol", "network.transport", "network.replication",
        "network.prediction", "persistence.journal", "ai.behavior", "ai.perception",
        "traffic.roads", "traffic.simulation", "gameplay.mission", "gameplay.inventory",
        "gameplay.equipment", "gameplay.combat", "gameplay.dialogue", "gameplay.quests",
        "render.postprocess", "render.visibility", "render.batching", "render.decals",
        "editor.selection", "animation.ik", "animation.retarget", "animation.skinning", "audio.dsp",
    };
    std::set<std::string_view> installed;
    for (const auto& module : kernel.modules()) {
        CHECK_MSG(installed.insert(module.name).second, module.name);
        CHECK_MSG(module.version > 0, module.name);
    }
    for (const auto name : required_modules) {
        CHECK_MSG(installed.contains(name), name);
    }

    const auto order = kernel.execution_order();
    CHECK(!order.empty());
    CHECK(order.size() == kernel.system_statistics().size());
    CHECK(std::set<std::string>(order.begin(), order.end()).size() == order.size());
    require_before(order, "input.actions", "physics.character.control");
    require_before(order, "physics.character.control", "physics.dynamics.step");
    require_before(order, "physics.dynamics.step", "scene.transforms.post");
    require_before(order, "camera.update", "geometry.mesh.bounds");
    require_before(order, "animation.ik.solve", "animation.skinning");
    require_before(order, "animation.retarget.pose", "animation.skinning");
    require_before(order, "animation.skinning", "geometry.mesh.bounds");
    require_before(order, "geometry.mesh.bounds", "spatial.sync");
    require_before(order, "render.visibility.install", "render.batching.install");
    require_before(order, "render.batching.install", "render.graph.frame");
    require_before(order, "render.decals.install", "render.graph.frame");
    require_before(order, "render.graph.frame", "render.postprocess.metrics");

    // The graph owns rendering and deliberately disables the standalone pass.
    // All other currently enabled systems must execute on every accepted tick.
    tick(kernel);
    tick(kernel);
    CHECK(kernel.frame().tick == 2);
    CHECK(kernel.events().dropped() == 0);
}

std::size_t dominant_pixels(const ge::render::Image& image, bool red) {
    std::size_t count{};
    for (const auto color : image.pixels()) {
        CHECK(ge::render::finite(color));
        const double primary = red ? color.r : color.g;
        const double secondary = red ? color.g : color.r;
        if (primary > 0.1 && primary > secondary * 2.0 && primary > color.b * 2.0) {
            ++count;
        }
    }
    return count;
}

std::vector<std::byte> export_image(ge::render::ImageService& images) {
    CHECK(images.export_ppm());
    const auto bytes = images.last_export();
    const std::string header = "P6\n64 48\n255\n";
    CHECK(bytes.size() == header.size() +
          static_cast<std::size_t>(image_width) * image_height * 3);
    CHECK(std::equal(header.begin(), header.end(), bytes.begin(),
        [](char expected, std::byte actual) {
            return static_cast<unsigned char>(expected) == std::to_integer<unsigned char>(actual);
        }));
    return {bytes.begin(), bytes.end()};
}

void scene_physics_material_and_graph_produce_pixels() {
    ge::core::Kernel kernel;
    install_runtime(kernel);
    auto& scene = kernel.services().get<ge::scene::Scene>();
    auto& registry = kernel.services().get<ge::ecs::Registry>();
    auto& meshes = kernel.services().get<ge::geometry::MeshLibrary>();
    auto& materials = kernel.services().get<ge::render::MaterialService>().materials();
    auto& cameras = kernel.services().get<ge::render::CameraService>();
    auto& images = kernel.services().get<ge::render::ImageService>();
    auto& physics = kernel.services().get<ge::physics::PhysicsWorld>();
    auto& collision = kernel.services().get<ge::physics::CollisionWorld>();
    const auto& spatial = kernel.services().get<ge::spatial::SpatialIndex>();

    ge::math::Transform camera_pose;
    camera_pose.position = {0.0, 0.0, 4.0};
    const auto camera = cameras.create("IntegrationCamera", camera_pose);
    CHECK(camera);
    CHECK(cameras.set_active(camera));

    const auto entity = scene.create("MovingBox");
    CHECK(entity);
    const auto mesh = meshes.create(ge::geometry::make_box({1.25, 1.25, 1.25}), "IntegrationBox");
    CHECK(mesh);
    CHECK(meshes.assign(registry, entity, mesh));
    ge::render::PbrMaterial appearance;
    appearance.shading_model = ge::render::ShadingModel::Unlit;
    appearance.albedo = {0.8, 0.03, 0.02, 1.0};
    const auto material = materials.create("IntegrationMaterial", appearance);
    CHECK(material);
    CHECK(materials.assign(registry, entity, material));

    ge::physics::BodyDescription body_description;
    body_description.linear_velocity = {1.0, 0.0, 0.0};
    body_description.linear_drag = 0.0;
    body_description.gravity_scale = 0.0;
    body_description.allow_sleep = false;
    const auto body = physics.add(entity, body_description);
    CHECK(body);
    const auto collider = collision.add(entity);
    CHECK(collider);

    tick(kernel);
    const auto body_state = physics.state(body);
    CHECK(body_state.has_value());
    check_near(body_state->pose.position.x, step_seconds);
    const auto& transform = registry.require<ge::ecs::TransformComponent>(entity);
    check_near(transform.world.position.x, body_state->pose.position.x);
    check_near(registry.require<ge::ecs::VelocityComponent>(entity).linear.x, 1.0);
    const auto* collider_state = collision.get(collider);
    CHECK(collider_state != nullptr);
    CHECK(collider_state->active);
    check_near(collider_state->transform.position.x, body_state->pose.position.x);
    const auto bounds = spatial.world_bounds(entity);
    CHECK(bounds.has_value());
    check_near(bounds->min.x, body_state->pose.position.x - 0.625);
    check_near(bounds->max.x, body_state->pose.position.x + 0.625);
    CHECK(physics.statistics().scene_rejections == 0);
    CHECK(physics.statistics().collider_rejections == 0);
    CHECK(physics.statistics().solver_failures == 0);
    CHECK(!physics.statistics().invalid_delta);
    const auto& raster = kernel.services().get<ge::render::SoftwareRenderer>();
    CHECK(raster.statistics().draws_rendered > 0);
    CHECK(raster.statistics().input_triangles > 0);
    CHECK(raster.statistics().fragments_written > 0);
    CHECK(kernel.services().get<ge::render::VisibilityService>().statistics().selected_draws > 0);
    CHECK(kernel.services().get<ge::render::BatchingService>().statistics().emitted > 0);
    CHECK(dominant_pixels(images.framebuffer().color(), true) > 0);
    const auto red_image = export_image(images);

    // Scene enablement must reach physics, the spatial index, and graph draws.
    CHECK(scene.set_enabled_recursive(entity, false));
    tick(kernel);
    CHECK(!spatial.world_bounds(entity).has_value());
    CHECK(raster.statistics().fragments_written == 0);
    CHECK(dominant_pixels(images.framebuffer().color(), true) == 0);
    CHECK(export_image(images) != red_image);

    // A material replacement must invalidate any prepared submission cache.
    appearance.albedo = {0.02, 0.8, 0.03, 1.0};
    CHECK(materials.replace(material, appearance));
    CHECK(scene.set_enabled_recursive(entity, true));
    tick(kernel);
    CHECK(raster.statistics().fragments_written > 0);
    CHECK(dominant_pixels(images.framebuffer().color(), false) > 0);
    CHECK(dominant_pixels(images.framebuffer().color(), true) == 0);
    CHECK(export_image(images) != red_image);

    CHECK(scene.destroy(entity));
    tick(kernel);
    CHECK(!registry.alive(entity));
    CHECK(!physics.state(body).has_value());
    CHECK(collision.get(collider) == nullptr);
    CHECK(!spatial.world_bounds(entity).has_value());
    CHECK(raster.statistics().fragments_written == 0);
    CHECK(kernel.events().dropped() == 0);
}

void input_events_drive_camera_and_clear_edges() {
    ge::core::Kernel kernel;
    install_runtime(kernel);
    auto& input = kernel.services().get<ge::input::InputSystem>();
    auto& cameras = kernel.services().get<ge::render::CameraService>();
    const auto& registry = kernel.services().get<ge::ecs::Registry>();
    ge::math::Transform pose;
    pose.position = {0.0, 0.0, 4.0};
    ge::render::CameraComponent settings;
    settings.controller.mode = ge::render::CameraMode::FreeFly;
    settings.controller.move_speed = 3.0;
    settings.controller.movement_half_life = 0.0;
    const auto camera = cameras.create("InputCamera", pose, settings);
    CHECK(camera);
    CHECK(cameras.set_active(camera));

    const auto send_key = [&](ge::input::EventKind kind, std::uint16_t code) {
        ge::input::InputEvent event;
        event.kind = kind;
        event.device = input.keyboard();
        event.code = code;
        CHECK(kernel.events().publish(event));
    };
    send_key(ge::input::EventKind::ButtonDown, 87); // W: forward in the default map.
    send_key(ge::input::EventKind::ButtonDown, 32); // Space: jump button edges.
    tick(kernel);
    CHECK(kernel.events().pending<ge::input::InputEvent>() == 0);
    CHECK(input.snapshot().tick == kernel.frame().tick);
    CHECK(input.pressed("jump"));
    CHECK(input.down("jump"));
    check_near(input.vector2("move").y, 1.0);
    CHECK(cameras.current_view() != nullptr);
    CHECK(cameras.current_view()->entity == camera);
    CHECK(!cameras.current_view()->stale);
    check_near(cameras.current_view()->position.z, 4.0 - 3.0 * step_seconds);
    check_near(registry.require<ge::ecs::TransformComponent>(camera).world.position.z,
               cameras.current_view()->position.z);

    tick(kernel);
    CHECK(!input.pressed("jump"));
    CHECK(input.down("jump"));
    check_near(cameras.current_view()->position.z, 4.0 - 6.0 * step_seconds);

    send_key(ge::input::EventKind::ButtonUp, 87);
    send_key(ge::input::EventKind::ButtonUp, 32);
    tick(kernel);
    CHECK(input.released("jump"));
    CHECK(!input.down("jump"));
    check_near(input.vector2("move").y, 0.0);
    check_near(cameras.current_view()->position.z, 4.0 - 6.0 * step_seconds);
    tick(kernel);
    CHECK(!input.released("jump"));
    check_near(cameras.current_view()->position.z, 4.0 - 6.0 * step_seconds);
    CHECK(input.statistics().rejected_events == 0);
    CHECK(kernel.events().dropped() == 0);
}

} // namespace

int main() {
    return ge::test::run("runtime integration", {
        {"module registration and enabled schedules", all_modules_install_and_enabled_systems_run},
        {"scene physics material and rendered output", scene_physics_material_and_graph_produce_pixels},
        {"input events drive camera and clear edges", input_events_drive_camera_and_clear_edges},
    });
}
