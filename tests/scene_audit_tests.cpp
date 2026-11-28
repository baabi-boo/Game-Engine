#include <ge/scene/scene.hpp>

#include "test_support.hpp"
#include <iostream>

void stale_ancestor_reparent_respects_subtree_depth() {
    for (const auto mode : {ge::scene::ReparentMode::KeepLocal,
                            ge::scene::ReparentMode::KeepWorld}) {
        for (const bool remove_component : {false, true}) {
            ge::ecs::Registry registry;
            ge::scene::SceneLimits limits;
            limits.max_depth = 3;
            ge::scene::Scene scene(registry, limits);
            const auto old_root = scene.create("old-root");
            const auto source = scene.create("source", old_root);
            const auto child = scene.create("child", source);
            const auto new_root = scene.create("new-root");
            const auto destination = scene.create("destination", new_root);
            CHECK(old_root && source && child && new_root && destination);
            CHECK(scene.update().error == ge::scene::SceneError::None);
            if (remove_component) {
                CHECK(registry.remove<ge::ecs::TransformComponent>(old_root));
            } else {
                CHECK(registry.destroy(old_root));
            }

            const auto moved = scene.reparent(source, destination, mode);
            std::cout << "mode=" << static_cast<int>(mode)
                      << " remove_component=" << remove_component
                      << " result=" << ge::scene::describe(moved.error)
                      << " child_depth=" << scene.depth(child).value_or(999)
                      << " child_world_available=" << scene.world_matrix(child).has_value()
                      << std::endl;
            CHECK(moved.error == ge::scene::SceneError::DepthLimit);
            CHECK(scene.update().error == ge::scene::SceneError::None);
            CHECK(!scene.parent(source));
            CHECK(scene.parent(child) == source);
            CHECK(scene.depth(child) == 1);
            CHECK(scene.world_matrix(child).has_value());
        }
    }
}

int main() {
    return ge::test::run("scene_audit", {{"stale_ancestor_reparent_respects_subtree_depth", stale_ancestor_reparent_respects_subtree_depth}});
}
