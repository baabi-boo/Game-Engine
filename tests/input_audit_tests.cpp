#include "test_support.hpp"

#include <ge/input/actions.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace ge::input;

void send(InputSystem& input, DeviceId device, EventKind kind, std::uint16_t code = 0,
          double value = 0.0) {
    InputEvent event;
    event.device = device;
    event.kind = kind;
    event.code = code;
    event.value = value;
    CHECK(input.enqueue(event));
}

void bind(InputSystem& input, std::uint64_t context, std::string name, Control control,
          Interaction interaction = Interaction::Immediate, Control chord = {}) {
    CHECK(input.define_action(context, {name, ActionKind::Button}));
    Binding binding;
    binding.action = std::move(name);
    binding.controls[0] = control;
    binding.interaction = interaction;
    binding.hold_seconds = 0.05;
    if (chord != Control{}) {
        binding.chord.push_back(chord);
    }
    CHECK(input.bind(context, std::move(binding)) != 0);
}

const ActionState& action(const InputSystem& input, std::string_view name) {
    const auto* state = input.action(name);
    CHECK(state != nullptr);
    return *state;
}

void check_continuing(const InputSystem& input, std::string_view name, double held) {
    const auto& state = action(input, name);
    CHECK(state.down);
    CHECK(!state.pressed);
    CHECK(!state.triggered);
    CHECK(!state.released);
    CHECK(std::abs(state.held_seconds - held) < 1.0e-9);
}

void remove_device(InputSystem& input, DeviceId device, bool queued) {
    if (queued) {
        send(input, device, EventKind::Disconnected);
    } else {
        CHECK(input.disconnect_device(device));
    }
}

void unrelated_disconnect_preserves_edges_and_duration() {
    for (const bool queued : {false, true}) {
        InputSystem input;
        const auto context = input.create_context("gameplay");
        const auto pad = input.connect_device(DeviceKind::Gamepad, "idle pad");
        CHECK(pad.valid());
        bind(input, context, "jump", key(32));
        bind(input, context, "hold", key(32), Interaction::Hold);
        send(input, input.keyboard(), EventKind::ButtonDown, 32);
        input.update(1, 0.1);
        CHECK(input.triggered("jump"));
        CHECK(input.triggered("hold"));
        input.update(2, 0.1);
        remove_device(input, pad, queued);
        input.update(3, 0.1);
        CHECK(!input.connected(pad));
        check_continuing(input, "jump", 0.3);
        check_continuing(input, "hold", 0.3);
        send(input, input.keyboard(), EventKind::ButtonUp, 32);
        input.update(4, 0.1);
        CHECK(input.released("jump"));
        CHECK(input.released("hold"));
    }
}

void wildcard_survivors_preserve_controls_chords_and_taps() {
    for (const bool queued : {false, true}) {
        InputSystem input;
        const auto context = input.create_context("gameplay");
        const auto first = input.connect_device(DeviceKind::Gamepad, "first pad");
        const auto second = input.connect_device(DeviceKind::Gamepad, "second pad");
        CHECK(first.valid());
        CHECK(second.valid());
        bind(input, context, "wildcard", gamepad_button(0));
        bind(input, context, "hold", gamepad_button(0), Interaction::Hold);
        bind(input, context, "specific", gamepad_button(0, second), Interaction::Hold);
        bind(input, context, "tap", gamepad_button(0), Interaction::Tap);
        bind(input, context, "chord", key(32), Interaction::Hold, gamepad_button(1));
        send(input, input.keyboard(), EventKind::ButtonDown, 32);
        for (const auto pad : {first, second}) {
            send(input, pad, EventKind::ButtonDown, 0);
            send(input, pad, EventKind::ButtonDown, 1);
        }
        input.update(1, 0.1);
        remove_device(input, first, queued);
        input.update(2, 0.1);
        for (const auto name : {"wildcard", "hold", "specific", "chord"}) {
            check_continuing(input, name, 0.2);
        }
        CHECK(!input.triggered("tap"));
        send(input, second, EventKind::ButtonUp, 0);
        send(input, second, EventKind::ButtonUp, 1);
        input.update(3, 0.01);
        CHECK(input.triggered("tap"));
        CHECK(input.released("wildcard"));
        CHECK(input.released("hold"));
        CHECK(input.released("specific"));
        CHECK(input.released("chord"));
    }
}

void disconnect_cancels_taps_and_respects_device_generations() {
    for (const bool queued : {false, true}) {
        InputSystem input;
        const auto context = input.create_context("gameplay");
        const auto pad = input.connect_device(DeviceKind::Gamepad, "old pad");
        CHECK(pad.valid());
        bind(input, context, "tap", gamepad_button(0, pad), Interaction::Tap);
        bind(input, context, "double", gamepad_button(0, pad), Interaction::DoubleTap);
        bind(input, context, "wildcard", gamepad_button(0));
        send(input, pad, EventKind::ButtonDown, 0);
        input.update(1, 0.01);
        send(input, pad, EventKind::ButtonUp, 0);
        input.update(2, 0.01);
        CHECK(input.triggered("tap"));
        CHECK(!input.triggered("double"));
        send(input, pad, EventKind::ButtonDown, 0);
        input.update(3, 0.01);
        remove_device(input, pad, queued);
        input.update(4, 0.01);
        CHECK(!input.triggered("tap"));
        CHECK(!input.triggered("double"));
        CHECK(input.released("wildcard"));

        const auto replacement = input.connect_device(DeviceKind::Gamepad, "replacement pad");
        CHECK(replacement.slot == pad.slot);
        CHECK(replacement.generation != pad.generation);
        send(input, replacement, EventKind::ButtonDown, 0);
        input.update(5, 0.01);
        send(input, pad, EventKind::Disconnected);
        input.update(6, 0.01);
        CHECK(input.connected(replacement));
        check_continuing(input, "wildcard", 0.02);
        send(input, replacement, EventKind::ButtonUp, 0);
        input.update(7, 0.01);
        CHECK(!input.triggered("tap"));
        CHECK(!input.triggered("double"));
    }
}

void consuming_context_cancels_without_completing_interactions() {
    for (const auto interaction : {Interaction::Immediate, Interaction::Hold,
                                   Interaction::Tap, Interaction::DoubleTap}) {
        InputSystem input;
        const auto lower = input.create_context("gameplay");
        const auto upper = input.create_context("menu", 10);
        bind(input, lower, "lower", key(32), interaction);
        Binding consuming;
        consuming.action = "upper";
        consuming.controls[0] = key(32);
        CHECK(input.define_action(upper, {"upper", ActionKind::Button}));
        CHECK(input.bind(upper, consuming) != 0);
        CHECK(input.set_context_enabled(upper, false));
        std::uint64_t tick = 0;
        const auto press = [&] {
            send(input, input.keyboard(), EventKind::ButtonDown, 32);
            input.update(++tick, 0.05);
        };
        const auto release = [&] {
            send(input, input.keyboard(), EventKind::ButtonUp, 32);
            input.update(++tick, 0.01);
        };
        if (interaction == Interaction::DoubleTap) {
            press();
            release();
            CHECK(!input.triggered("lower"));
        }
        press();
        CHECK(input.set_context_enabled(upper, true));
        input.update(++tick, 0.01);
        CHECK(!input.down("lower"));
        CHECK(!input.pressed("lower"));
        CHECK(!input.triggered("lower"));
        CHECK(input.released("lower") ==
              (interaction == Interaction::Immediate || interaction == Interaction::Hold));
        CHECK(input.button(input.keyboard(), 32)->down);
        CHECK(input.set_context_enabled(upper, false));
        release();
        CHECK(!input.triggered("lower"));
        press();
        release();
        CHECK(input.triggered("lower") == (interaction == Interaction::Tap));
        if (interaction == Interaction::DoubleTap) {
            press();
            release();
            CHECK(input.triggered("lower"));
        }
    }
}

void capture_suppression_cancels_axis_tap() {
    InputSystem input;
    const auto context = input.create_context("gameplay");
    const auto pad = input.connect_device(DeviceKind::Gamepad, "pad");
    bind(input, context, "tap", gamepad_axis(0), Interaction::Tap);
    send(input, pad, EventKind::Axis, 0, 1.0);
    input.update(1, 0.05);
    CaptureOptions options;
    options.buttons = false;
    CHECK(input.begin_capture(options));
    send(input, pad, EventKind::Axis, 0, 0.0);
    send(input, pad, EventKind::Axis, 0, 1.0);
    input.update(2, 0.01);
    CHECK(input.capture().status == CaptureStatus::Captured);
    CHECK(input.snapshot().find(pad)->axes[0] == 1.0);
    CHECK(!input.triggered("tap"));
    send(input, pad, EventKind::Axis, 0, 0.0);
    input.update(3, 0.01);
    CHECK(!input.triggered("tap"));
    send(input, pad, EventKind::Axis, 0, 1.0);
    input.update(4, 0.05);
    send(input, pad, EventKind::Axis, 0, 0.0);
    input.update(5, 0.01);
    CHECK(input.triggered("tap"));
}

} // namespace

int main() {
    return ge::test::run("input audit", {
        {"unrelated disconnect continuity", unrelated_disconnect_preserves_edges_and_duration},
        {"wildcard control and chord continuity", wildcard_survivors_preserve_controls_chords_and_taps},
        {"disconnect cancellation and generations", disconnect_cancels_taps_and_respects_device_generations},
        {"context consumption cancels interactions", consuming_context_cancels_without_completing_interactions},
        {"capture suppression cancels axis tap", capture_suppression_cancels_axis_tap},
    });
}
