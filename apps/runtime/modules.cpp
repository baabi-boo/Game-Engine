#include "modules.hpp"
#include <ge/core/runtime.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/jobs/executor.hpp>
#include <ge/io/filesystem.hpp>
#include <ge/scene/scene.hpp>
#include <ge/input/actions.hpp>

namespace ge::application {
void install_modules(core::Kernel& kernel) {
    ge::ecs::register_registry(kernel);
    ge::jobs::register_executor(kernel);
    ge::io::register_filesystem(kernel);
    ge::scene::register_scene(kernel);
    ge::input::register_actions(kernel);
}
}
