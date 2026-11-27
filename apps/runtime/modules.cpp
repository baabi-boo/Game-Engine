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
#include <ge/animation/blend.hpp>
#include <ge/audio/spatial.hpp>
#include <ge/navigation/navmesh.hpp>
#include <ge/scene/partition.hpp>
#include <ge/terrain/heightfield.hpp>
#include <ge/terrain/vegetation.hpp>
#include <ge/render/lighting.hpp>
#include <ge/render/graph.hpp>
#include <ge/effects/particles.hpp>
#include <ge/physics/constraints.hpp>
#include <ge/scene/serialization.hpp>
#include <ge/script/vm.hpp>
#include <ge/gameplay/interaction.hpp>
#include <ge/core/signals.hpp>
#include <ge/animation/curves.hpp>
#include <ge/config/settings.hpp>

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
    ge::animation::register_blend(kernel);
    ge::audio::register_spatial(kernel);
    ge::navigation::register_navmesh(kernel);
    ge::scene::register_partition(kernel);
    ge::terrain::register_heightfield(kernel);
    ge::terrain::register_vegetation(kernel);
    ge::render::register_lighting(kernel);
    ge::render::register_graph(kernel);
    ge::effects::register_particles(kernel);
    ge::physics::register_constraints(kernel);
    ge::scene::register_serialization(kernel);
    ge::script::register_vm(kernel);
    ge::gameplay::register_interaction(kernel);
    ge::core::register_signals(kernel);
    ge::animation::register_curves(kernel);
    ge::config::register_settings(kernel);
    (void)kernel.order_after("spatial.sync", "geometry.mesh.bounds");
    (void)kernel.order_after("geometry.mesh.bounds", "camera.update");
    (void)kernel.order_after("geometry.mesh.bounds", "terrain.vegetation");
    (void)kernel.order_after("terrain.vegetation", "camera.update");
}
}
