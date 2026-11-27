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
#include <ge/text/localization.hpp>
#include <ge/network/protocol.hpp>
#include <ge/network/transport.hpp>
#include <ge/network/replication.hpp>
#include <ge/network/prediction.hpp>
#include <ge/persistence/journal.hpp>
#include <ge/ai/behavior.hpp>
#include <ge/ai/perception.hpp>
#include <ge/traffic/roads.hpp>
#include <ge/traffic/simulation.hpp>
#include <ge/gameplay/mission.hpp>
#include <ge/gameplay/inventory.hpp>
#include <ge/gameplay/equipment.hpp>
#include <ge/gameplay/combat.hpp>
#include <ge/gameplay/dialogue.hpp>
#include <ge/gameplay/quests.hpp>
#include <ge/render/postprocess.hpp>
#include <ge/render/visibility.hpp>
#include <ge/render/batching.hpp>
#include <ge/render/decals.hpp>
#include <ge/editor/selection.hpp>

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
    ge::text::register_localization(kernel);
    ge::network::register_protocol(kernel);
    ge::network::register_transport(kernel);
    ge::network::register_replication(kernel);
    ge::network::register_prediction(kernel);
    ge::persistence::register_journal(kernel);
    ge::ai::register_behavior(kernel);
    ge::ai::register_perception(kernel);
    ge::traffic::register_roads(kernel);
    ge::traffic::register_simulation(kernel);
    ge::gameplay::register_mission(kernel);
    ge::gameplay::register_inventory(kernel);
    ge::gameplay::register_equipment(kernel);
    ge::gameplay::register_combat(kernel);
    ge::gameplay::register_dialogue(kernel);
    ge::gameplay::register_quests(kernel);
    ge::render::register_postprocess(kernel);
    ge::render::register_visibility(kernel);
    ge::render::register_batching(kernel);
    ge::render::register_decals(kernel);
    ge::editor::register_selection(kernel);
    (void)kernel.order_after("spatial.sync", "geometry.mesh.bounds");
    (void)kernel.order_after("geometry.mesh.bounds", "camera.update");
    (void)kernel.order_after("geometry.mesh.bounds", "terrain.vegetation");
    (void)kernel.order_after("terrain.vegetation", "camera.update");
}
}
