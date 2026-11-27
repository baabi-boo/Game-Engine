#include "modules.hpp"
#include <ge/core/runtime.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/jobs/executor.hpp>
#include <ge/io/filesystem.hpp>
#include <ge/scene/scene.hpp>
#include <ge/input/actions.hpp>
#include <ge/assets/catalog.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/render/image.hpp>
#include <ge/spatial/index.hpp>
#include <ge/physics/collision.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/animation/skeleton.hpp>
#include <ge/audio/mixer.hpp>
#include <ge/navigation/pathfinding.hpp>
#include <ge/render/material.hpp>
#include <ge/render/camera.hpp>
#include <ge/assets/streaming.hpp>
#include <ge/render/rasterizer.hpp>
#include <ge/physics/character.hpp>
#include <ge/physics/vehicle.hpp>

namespace ge::application {
void install_modules(core::Kernel& kernel) {
    ge::ecs::register_registry(kernel);
    ge::jobs::register_executor(kernel);
    ge::io::register_filesystem(kernel);
    ge::scene::register_scene(kernel);
    ge::input::register_actions(kernel);
    ge::assets::register_catalog(kernel);
    ge::geometry::register_mesh(kernel);
    ge::render::register_image(kernel);
    ge::spatial::register_index(kernel);
    ge::physics::register_collision(kernel);
    ge::physics::register_dynamics(kernel);
    ge::animation::register_skeleton(kernel);
    ge::audio::register_mixer(kernel);
    ge::navigation::register_pathfinding(kernel);
    ge::render::register_material(kernel);
    ge::render::register_camera(kernel);
    ge::assets::register_streaming(kernel);
    ge::render::register_rasterizer(kernel);
    ge::physics::register_character(kernel);
    ge::physics::register_vehicle(kernel);
    (void)kernel.order_after("spatial.sync", "geometry.mesh.bounds");
    (void)kernel.order_after("geometry.mesh.bounds", "camera.update");
}
}
