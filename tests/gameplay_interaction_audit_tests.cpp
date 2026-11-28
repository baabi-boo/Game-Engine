#include <ge/core/runtime.hpp>
#include <ge/gameplay/interaction.hpp>
#include <ge/scene/scene.hpp>

#include "test_support.hpp"

#include <array>

namespace {

using namespace ge::gameplay;

struct Fixture {
    ge::core::Kernel kernel;
    ge::ecs::Registry registry{8};
    ge::scene::Scene scene{registry};
    InteractionService service{kernel, scene};
    ge::ecs::Entity actor{scene.create("actor")};
    ge::ecs::Entity target{scene.create("target", {}, {{0.0, 0.0, -1.0}})};
    std::uint64_t tick{};

    Fixture() {
        InteractionActorComponent actor_settings;
        actor_settings.eye_offset = {};
        actor_settings.camera_aim = false;
        CHECK(service.attach_actor(actor, actor_settings));
        InteractableComponent target_settings;
        target_settings.require_line_of_sight = false;
        target_settings.trigger_radius = 2.0;
        CHECK(service.attach_target(target, target_settings));
    }

    void update() { service.update({++tick, 0.1, static_cast<double>(tick) * 0.1}); }
};

InteractionAction native_action(const char* name,
                                InteractionEventKind event = InteractionEventKind::Activated) {
    InteractionAction action;
    action.kind = InteractionActionKind::Native;
    action.name = name;
    action.events = interaction_event_mask(event);
    return action;
}

void actor_replacement_revokes_authority_and_preserves_the_replacement() {
    Fixture fixture;
    auto* settings = fixture.registry.get<InteractableComponent>(fixture.target);
    settings->mode = InteractionMode::Hold;
    CHECK(fixture.service.select_player(fixture.actor));
    CHECK(fixture.service.use(fixture.actor, true));
    fixture.update();
    CHECK(fixture.service.target(fixture.target)->reserved_by == fixture.actor);
    CHECK(fixture.service.statistics().trigger_pairs == 1);

    fixture.registry.emplace_or_replace<InteractionActorComponent>(fixture.actor);
    const auto replacement = fixture.registry.component_instance<InteractionActorComponent>(fixture.actor);
    CHECK(!fixture.service.actor(fixture.actor));
    CHECK(!fixture.service.player().valid());
    CHECK(!fixture.service.use(fixture.actor, true));
    CHECK(!fixture.service.select_player(fixture.actor));
    CHECK(!fixture.service.focus(fixture.actor, fixture.target));
    CHECK(fixture.service.detach_actor(fixture.actor));
    CHECK(fixture.registry.component_instance<InteractionActorComponent>(fixture.actor) == replacement);
    CHECK(!fixture.service.target(fixture.target)->reserved_by.valid());
    CHECK(fixture.service.statistics().trigger_pairs == 0);
}

void target_replacement_drops_queued_callbacks_and_preserves_new_components() {
    for (const bool replace_actions : {false, true}) {
        Fixture fixture;
        int calls = 0;
        CHECK(fixture.service.register_native("enter", [&](InteractionActionContext&) {
            ++calls;
            return InteractionActionResult{};
        }));
        CHECK(fixture.service.add_action(fixture.target,
            native_action("enter", InteractionEventKind::FocusEnter)));
        CHECK(fixture.service.focus(fixture.actor, fixture.target));
        CHECK(fixture.service.statistics().pending_batches == 1);

        if (replace_actions) {
            CHECK(fixture.registry.remove<InteractionActionsComponent>(fixture.target));
            fixture.registry.emplace<InteractionActionsComponent>(fixture.target);
        } else {
            const auto settings = *fixture.registry.get<InteractableComponent>(fixture.target);
            fixture.registry.emplace_or_replace<InteractableComponent>(fixture.target, settings);
        }
        const auto replacement = replace_actions
            ? fixture.registry.component_instance<InteractionActionsComponent>(fixture.target)
            : fixture.registry.component_instance<InteractableComponent>(fixture.target);
        CHECK(!fixture.service.target(fixture.target));
        CHECK(!fixture.service.set_actions(fixture.target, {}));
        fixture.update();
        CHECK(calls == 0);
        CHECK(!fixture.service.actor(fixture.actor)->focus.valid());
        CHECK(fixture.service.statistics().targets == 0);
        CHECK(fixture.service.statistics().pending_batches == 0);
        if (replace_actions) {
            CHECK(!fixture.registry.has<InteractableComponent>(fixture.target));
            CHECK(fixture.registry.component_instance<InteractionActionsComponent>(fixture.target) == replacement);
        } else {
            CHECK(!fixture.registry.has<InteractionActionsComponent>(fixture.target));
            CHECK(fixture.registry.component_instance<InteractableComponent>(fixture.target) == replacement);
        }
    }
}

void callbacks_cannot_continue_or_commit_after_component_replacement() {
    for (const unsigned replacement_kind : {0U, 1U, 2U}) {
        Fixture fixture;
        int later_calls = 0;
        CHECK(fixture.service.register_native("replace", [&](InteractionActionContext&) {
            if (replacement_kind == 0) {
                fixture.registry.emplace_or_replace<InteractionActorComponent>(fixture.actor);
            } else if (replacement_kind == 1) {
                fixture.registry.emplace_or_replace<InteractableComponent>(fixture.target);
            } else {
                fixture.registry.emplace_or_replace<InteractionActionsComponent>(fixture.target);
            }
            return InteractionActionResult{};
        }));
        CHECK(fixture.service.register_native("later", [&](InteractionActionContext&) {
            ++later_calls;
            return InteractionActionResult{};
        }));
        auto replace = native_action("replace");
        replace.stop_on_failure = false;
        const std::array actions{replace, native_action("later")};
        // Also cover replacement by the final action in a batch.
        const auto count = replacement_kind == 2 ? 1U : 2U;
        CHECK(fixture.service.set_actions(fixture.target, {actions.data(), count}));
        CHECK(fixture.service.use(fixture.actor, true));
        fixture.update();
        CHECK(later_calls == 0);
        CHECK(fixture.service.statistics().activations == 0);
        CHECK(fixture.service.statistics().rejected == 1);
        bool rejected_stale = false;
        for (const auto& event : fixture.service.events_since(0).events) {
            rejected_stale |= event.kind == InteractionEventKind::Rejected &&
                event.error == InteractionError::Stale;
        }
        CHECK(rejected_stale);
        if (replacement_kind == 0) {
            CHECK(!fixture.service.actor(fixture.actor));
            CHECK(fixture.registry.has<InteractionActorComponent>(fixture.actor));
            CHECK(!fixture.service.target(fixture.target)->reserved_by.valid());
        } else {
            CHECK(!fixture.service.target(fixture.target));
            CHECK(!fixture.service.actor(fixture.actor)->focus.valid());
            CHECK(fixture.registry.has<InteractableComponent>(fixture.target) == (replacement_kind == 1));
            CHECK(fixture.registry.has<InteractionActionsComponent>(fixture.target) == (replacement_kind == 2));
        }
        CHECK(fixture.service.statistics().trigger_pairs == 0);
    }
}

void removal_releases_companion_components_for_reattachment() {
    Fixture fixture;
    CHECK(fixture.registry.remove<InteractableComponent>(fixture.target));
    fixture.update();
    CHECK(!fixture.registry.has<InteractionActionsComponent>(fixture.target));
    CHECK(fixture.service.attach_target(fixture.target));
    fixture.registry.emplace_or_replace<InteractableComponent>(fixture.target);
    const auto replacement = fixture.registry.component_instance<InteractableComponent>(fixture.target);
    CHECK(fixture.service.detach_target(fixture.target));
    CHECK(fixture.registry.component_instance<InteractableComponent>(fixture.target) == replacement);
    CHECK(!fixture.registry.has<InteractionActionsComponent>(fixture.target));
}

void authorized_edits_keep_enrollment() {
    Fixture fixture;
    fixture.registry.get<InteractionActorComponent>(fixture.actor)->range = 4.0;
    fixture.registry.get<InteractableComponent>(fixture.target)->prompt = "Open";
    int calls = 0;
    CHECK(fixture.service.register_native("activate", [&](InteractionActionContext&) {
        ++calls;
        return InteractionActionResult{};
    }));
    CHECK(fixture.service.add_action(fixture.target, native_action("activate")));
    const std::array actions{native_action("activate")};
    CHECK(fixture.service.set_actions(fixture.target, actions));
    CHECK(fixture.service.use(fixture.actor, true));
    fixture.update();
    CHECK(calls == 1);
    CHECK(fixture.service.actor(fixture.actor));
    CHECK(fixture.service.target(fixture.target)->activations == 1);
}

} // namespace

int main() {
    return ge::test::run("gameplay interaction audit", {
        {"actor replacement revokes authority", actor_replacement_revokes_authority_and_preserves_the_replacement},
        {"target replacement drops queued callbacks", target_replacement_drops_queued_callbacks_and_preserves_new_components},
        {"callbacks cannot retain replaced ownership", callbacks_cannot_continue_or_commit_after_component_replacement},
        {"removal releases companion components", removal_releases_companion_components_for_reattachment},
        {"authorized edits retain enrollment", authorized_edits_keep_enrollment},
    });
}
