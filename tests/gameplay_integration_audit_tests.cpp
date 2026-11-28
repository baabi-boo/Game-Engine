#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/gameplay/interaction.hpp>
#include <ge/gameplay/mission.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <string>

namespace {

using namespace ge;
using namespace ge::gameplay;

struct Fixture {
    core::Kernel kernel;
    ecs::Registry& registry = kernel.services().emplace<ecs::Registry>(8);
    scene::Scene& scene = kernel.services().emplace<scene::Scene>(registry);
    InteractionService& interactions = kernel.services().emplace<InteractionService>(kernel, scene);
    MissionService& missions = kernel.services().emplace<MissionService>(
        kernel, registry, MissionLimits{.input_per_frame = 1});
    ecs::Entity actor = scene.create("actor");
    ecs::Entity target = scene.create("target", {}, {{0.0, 0.0, -1.0}});
    std::uint64_t tick{};

    Fixture() {
        CHECK(registry.set_persistent_id(actor, {11}));
        CHECK(registry.set_persistent_id(target, {21}));
        InteractionActorComponent actor_settings;
        actor_settings.eye_offset = {};
        actor_settings.camera_aim = false;
        CHECK(interactions.attach_actor(actor, actor_settings));
        InteractableComponent target_settings;
        target_settings.require_line_of_sight = false;
        CHECK(interactions.attach_target(target, target_settings));
        InteractionAction action;
        action.name = "use";
        CHECK(interactions.add_action(target, action));
    }

    std::uint64_t start_mission(std::string name, PersistentId target_id = {}) {
        MissionDefinition definition;
        definition.id = std::move(name);
        MissionObjectiveDefinition objective;
        objective.id = "interact";
        objective.kind = MissionObjectiveKind::Interaction;
        objective.target = target_id;
        definition.objectives.push_back(objective);
        const auto id = definition.id;
        CHECK(missions.define(std::move(definition)));
        const auto started = missions.start(actor, id);
        CHECK(started);
        return started.run;
    }

    void update() {
        const core::Frame frame{++tick, 0.1, static_cast<double>(tick) * 0.1};
        // This is the registered gameplay order: interaction publishes, then
        // missions consume a bounded prefix of its independent history.
        interactions.update(frame);
        missions.update(frame);
    }

    void activate() {
        CHECK(interactions.use(actor, true));
        update();
        const auto history = interactions.events_since(0);
        const auto activated = std::find_if(history.events.begin(), history.events.end(),
            [](const InteractionEvent& event) { return event.kind == InteractionEventKind::Activated; });
        CHECK(activated != history.events.end());
        CHECK(missions.statistics().interaction_cursor < activated->sequence);
        CHECK(interactions.use(actor, false));
    }

    void consume_backlog() {
        const auto end = interactions.events_since(0).newest_sequence;
        for (unsigned attempt = 0; attempt < 16 && missions.statistics().interaction_cursor < end; ++attempt) {
            update();
        }
        CHECK(missions.statistics().interaction_cursor >= end);
        CHECK(!missions.statistics().interaction_suspended);
    }
};

void delayed_target_history_keeps_the_identity_that_was_interacted_with() {
    Fixture fixture;
    const auto original = fixture.start_mission("original_target", {21});
    const auto replacement = fixture.start_mission("replacement_target", {22});
    fixture.activate();
    CHECK(fixture.registry.set_persistent_id(fixture.target, {22}));
    fixture.consume_backlog();
    CHECK(fixture.missions.inspect(replacement)->objectives.front().count == 0);
    CHECK(fixture.missions.inspect(original)->status == MissionStatus::Completed);
}

void delayed_actor_history_cannot_credit_a_new_identity_on_the_same_handle() {
    Fixture fixture;
    fixture.start_mission("original_actor");
    fixture.activate();
    CHECK(fixture.registry.set_persistent_id(fixture.actor, {12}));
    const auto replacement = fixture.start_mission("replacement_actor");
    fixture.consume_backlog();
    CHECK(fixture.missions.inspect(replacement)->objectives.front().count == 0);
    CHECK(fixture.missions.inspect(replacement)->status == MissionStatus::Active);
}

} // namespace

int main() {
    return ge::test::run("gameplay_integration_audit", {
        {"delayed interaction keeps target identity", delayed_target_history_keeps_the_identity_that_was_interacted_with},
        {"delayed interaction cannot change actor identity", delayed_actor_history_cannot_credit_a_new_identity_on_the_same_handle},
    });
}
