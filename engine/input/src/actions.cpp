#include <ge/input/actions.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <bitset>
#include <charconv>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ge::input {
namespace {

constexpr std::size_t maximum_name = 96;
constexpr double maximum_motion = 1000000.0;
constexpr double maximum_elapsed = 1000000000000.0;
constexpr std::size_t control_slots = keyboard_button_count + gamepad_axis_count + 4;

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

void increment(std::uint16_t& value) noexcept {
    if (value != std::numeric_limits<std::uint16_t>::max()) {
        ++value;
    }
}

bool valid_device_kind(DeviceKind kind) noexcept {
    return kind == DeviceKind::Keyboard || kind == DeviceKind::Mouse ||
           kind == DeviceKind::Gamepad;
}

bool valid_action_kind(ActionKind kind) noexcept {
    return kind == ActionKind::Button || kind == ActionKind::Axis ||
           kind == ActionKind::Vector2;
}

std::size_t button_count(DeviceKind kind) noexcept {
    switch (kind) {
    case DeviceKind::Keyboard: return keyboard_button_count;
    case DeviceKind::Mouse: return mouse_button_count;
    case DeviceKind::Gamepad: return gamepad_button_count;
    }
    return 0;
}

bool valid_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > maximum_name || !core::valid_utf8(value)) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 32U || byte == 127U;
    });
}

bool valid_codepoint(char32_t value) noexcept {
    return value != 0 && value <= 0x10ffffU &&
           !(value >= 0xd800U && value <= 0xdfffU);
}

bool bounded_number(double value, double bound = maximum_motion) noexcept {
    return std::isfinite(value) && std::abs(value) <= bound;
}

bool bounded_vector(ge::math::Vec2 value) noexcept {
    return bounded_number(value.x) && bounded_number(value.y);
}

void validate_limits(const InputLimits& limits) {
    if (limits.devices < 2 || limits.devices > 64 ||
        limits.queued_events == 0 || limits.queued_events > 65536 ||
        limits.events_per_device == 0 || limits.events_per_device > 65536 ||
        limits.contexts == 0 || limits.contexts > 256 ||
        limits.actions == 0 || limits.actions > 4096 ||
        limits.bindings == 0 || limits.bindings > 16384 ||
        limits.text_codepoints > 65536 ||
        limits.recording_frames == 0 || limits.recording_frames > 100000 ||
        limits.recording_bytes < 1024 || limits.recording_bytes > 512U * 1024U * 1024U) {
        throw std::invalid_argument("input limits are outside supported bounds");
    }
}

bool valid_control(const Control& control) noexcept {
    if (!valid_device_kind(control.device)) {
        return false;
    }
    if (!control.instance.valid() && control.instance.generation != 0) {
        return false;
    }
    switch (control.kind) {
    case ControlKind::Button:
        return control.code < button_count(control.device);
    case ControlKind::Axis:
        return control.device == DeviceKind::Gamepad && control.code < gamepad_axis_count;
    case ControlKind::PointerX:
    case ControlKind::PointerY:
    case ControlKind::WheelX:
    case ControlKind::WheelY:
        return control.device == DeviceKind::Mouse && control.code == 0;
    }
    return false;
}

bool valid_curve(const ResponseCurve& curve) noexcept {
    return std::isfinite(curve.deadzone) && curve.deadzone >= 0.0 &&
           curve.deadzone < 1.0 && std::isfinite(curve.saturation) &&
           curve.saturation > curve.deadzone && curve.saturation <= 1.0 &&
           std::isfinite(curve.exponent) && curve.exponent >= 0.05 &&
           curve.exponent <= 16.0 && std::isfinite(curve.sensitivity) &&
           curve.sensitivity >= 0.0 && curve.sensitivity <= 1000.0;
}

bool valid_binding(const Binding& binding) noexcept {
    if (!valid_name(binding.action) || !valid_action_kind(binding.kind) ||
        !valid_curve(binding.response) || !bounded_vector(binding.scale) ||
        binding.control_count == 0 || binding.control_count > 4 ||
        binding.chord.size() > 4 || !std::isfinite(binding.threshold) ||
        binding.threshold <= 0.0 || binding.threshold > 1.0 ||
        !std::isfinite(binding.tap_seconds) || binding.tap_seconds <= 0.0 ||
        binding.tap_seconds > 10.0 || !std::isfinite(binding.hold_seconds) ||
        binding.hold_seconds <= 0.0 || binding.hold_seconds > 60.0 ||
        !std::isfinite(binding.double_tap_seconds) || binding.double_tap_seconds <= 0.0 ||
        binding.double_tap_seconds > 10.0) {
        return false;
    }
    if (binding.interaction != Interaction::Immediate && binding.interaction != Interaction::Tap &&
        binding.interaction != Interaction::Hold && binding.interaction != Interaction::DoubleTap) {
        return false;
    }
    if (binding.kind != ActionKind::Button && binding.interaction != Interaction::Immediate) {
        return false;
    }
    if ((binding.kind == ActionKind::Button && binding.control_count != 1) ||
        (binding.kind == ActionKind::Axis && binding.control_count > 2) ||
        (binding.kind == ActionKind::Vector2 && binding.control_count != 2 && binding.control_count != 4)) {
        return false;
    }
    for (std::size_t index = 0; index < binding.control_count; ++index) {
        if (!valid_control(binding.controls[index])) {
            return false;
        }
    }
    return std::all_of(binding.chord.begin(), binding.chord.end(), [](const Control& control) {
        return valid_control(control) && control.kind == ControlKind::Button;
    });
}

std::size_t control_index(const Control& control) noexcept {
    switch (control.kind) {
    case ControlKind::Button: return control.code;
    case ControlKind::Axis: return keyboard_button_count + control.code;
    case ControlKind::PointerX: return keyboard_button_count + gamepad_axis_count;
    case ControlKind::PointerY: return keyboard_button_count + gamepad_axis_count + 1;
    case ControlKind::WheelX: return keyboard_button_count + gamepad_axis_count + 2;
    case ControlKind::WheelY: return keyboard_button_count + gamepad_axis_count + 3;
    }
    return 0;
}

std::size_t snapshot_bytes(const InputSnapshot& frame) noexcept {
    std::size_t count = sizeof(InputSnapshot) + frame.text.size() * sizeof(char32_t);
    count += frame.devices.size() * sizeof(DeviceSnapshot);
    count += frame.actions.size() * sizeof(ActionState);
    for (const auto& device : frame.devices) {
        count += device.name.size();
    }
    for (const auto& action : frame.actions) {
        count += action.name.size();
    }
    return count;
}

void validate_snapshot(const InputSnapshot& frame, const InputLimits& limits) {
    if (!std::isfinite(frame.delta_seconds) || frame.delta_seconds < 0.0 ||
        frame.delta_seconds > 60.0 || frame.devices.size() > limits.devices ||
        frame.actions.size() > limits.actions || frame.text.size() > limits.text_codepoints) {
        throw std::invalid_argument("recorded input frame exceeds limits");
    }
    std::uint16_t previous_slot{};
    bool first = true;
    for (const auto& device : frame.devices) {
        if (!device.id.valid() || device.id.slot >= limits.devices ||
            (!first && device.id.slot <= previous_slot) || !valid_device_kind(device.kind) ||
            !valid_name(device.name) || !bounded_vector(device.pointer) ||
            !bounded_vector(device.pointer_delta) || !bounded_vector(device.wheel)) {
            throw std::invalid_argument("recorded input device is invalid");
        }
        previous_slot = device.id.slot;
        first = false;
        for (std::size_t index = button_count(device.kind); index < device.buttons.size(); ++index) {
            const auto& button = device.buttons[index];
            if (button.down || button.pressed || button.released || button.repeats != 0) {
                throw std::invalid_argument("recording contains unsupported button data");
            }
        }
        for (double axis : device.axes) {
            if (!std::isfinite(axis) || std::abs(axis) > 1.0 ||
                (device.kind != DeviceKind::Gamepad && axis != 0.0)) {
                throw std::invalid_argument("recorded gamepad axis is invalid");
            }
        }
    }
    std::string_view previous_action;
    for (const auto& action : frame.actions) {
        if (!valid_name(action.name) || (!previous_action.empty() && action.name <= previous_action) ||
            !valid_action_kind(action.kind) || !bounded_number(action.value) ||
            !bounded_vector(action.vector) || !std::isfinite(action.held_seconds) ||
            action.held_seconds < 0.0 || action.held_seconds > maximum_elapsed) {
            throw std::invalid_argument("recorded input action is invalid");
        }
        previous_action = action.name;
    }
    for (char32_t codepoint : frame.text) {
        if (!valid_codepoint(codepoint)) {
            throw std::invalid_argument("recorded input text is invalid");
        }
    }
}

void write_vector(core::ByteWriter& writer, ge::math::Vec2 value) {
    writer.write_f64(value.x);
    writer.write_f64(value.y);
}

ge::math::Vec2 read_vector(core::ByteReader& reader) {
    const double x = reader.read_f64();
    const double y = reader.read_f64();
    return {x, y};
}

void write_snapshot(core::ByteWriter& writer, const InputSnapshot& frame) {
    writer.write_u64(frame.tick);
    writer.write_f64(frame.delta_seconds);
    writer.write_bool(frame.focused);
    writer.write_bool(frame.overflow_recovered);
    writer.write_u16(static_cast<std::uint16_t>(frame.devices.size()));
    for (const auto& device : frame.devices) {
        writer.write_u16(device.id.slot);
        writer.write_u32(device.id.generation);
        writer.write_u8(static_cast<std::uint8_t>(device.kind));
        writer.write_string(device.name, maximum_name);
        for (std::size_t index = 0; index < button_count(device.kind); ++index) {
            const auto& button = device.buttons[index];
            const unsigned flags = (button.down ? 1U : 0U) | (button.pressed ? 2U : 0U) |
                                   (button.released ? 4U : 0U);
            writer.write_u8(static_cast<std::uint8_t>(flags));
            writer.write_u16(button.repeats);
        }
        if (device.kind == DeviceKind::Gamepad) {
            for (double value : device.axes) {
                writer.write_f64(value);
            }
        }
        write_vector(writer, device.pointer);
        write_vector(writer, device.pointer_delta);
        write_vector(writer, device.wheel);
    }
    writer.write_u16(static_cast<std::uint16_t>(frame.actions.size()));
    for (const auto& action : frame.actions) {
        writer.write_string(action.name, maximum_name);
        writer.write_u8(static_cast<std::uint8_t>(action.kind));
        writer.write_f64(action.value);
        write_vector(writer, action.vector);
        writer.write_bool(action.down);
        writer.write_bool(action.pressed);
        writer.write_bool(action.released);
        writer.write_bool(action.triggered);
        writer.write_bool(action.consumed);
        writer.write_f64(action.held_seconds);
        writer.write_u64(action.source_binding);
    }
    writer.write_u32(static_cast<std::uint32_t>(frame.text.size()));
    for (char32_t codepoint : frame.text) {
        writer.write_u32(static_cast<std::uint32_t>(codepoint));
    }
}

InputSnapshot read_snapshot(core::ByteReader& reader, const InputLimits& limits) {
    InputSnapshot frame;
    frame.tick = reader.read_u64();
    frame.delta_seconds = reader.read_f64();
    frame.focused = reader.read_bool();
    frame.overflow_recovered = reader.read_bool();
    const auto device_count = reader.read_u16();
    if (device_count > limits.devices) {
        throw std::length_error("too many recorded input devices");
    }
    frame.devices.reserve(device_count);
    for (std::size_t index = 0; index < device_count; ++index) {
        DeviceSnapshot device;
        device.id.slot = reader.read_u16();
        device.id.generation = reader.read_u32();
        device.kind = static_cast<DeviceKind>(reader.read_u8());
        if (!valid_device_kind(device.kind)) {
            throw std::invalid_argument("unknown recorded input device kind");
        }
        device.name = reader.read_string();
        for (std::size_t code = 0; code < button_count(device.kind); ++code) {
            auto& button = device.buttons[code];
            const auto flags = reader.read_u8();
            if ((flags & 0xf8U) != 0U) {
                throw std::invalid_argument("unknown recorded button flags");
            }
            button.down = (flags & 1U) != 0U;
            button.pressed = (flags & 2U) != 0U;
            button.released = (flags & 4U) != 0U;
            button.repeats = reader.read_u16();
        }
        if (device.kind == DeviceKind::Gamepad) {
            for (double& value : device.axes) {
                value = reader.read_f64();
            }
        }
        device.pointer = read_vector(reader);
        device.pointer_delta = read_vector(reader);
        device.wheel = read_vector(reader);
        frame.devices.push_back(std::move(device));
    }
    const auto action_count = reader.read_u16();
    if (action_count > limits.actions) {
        throw std::length_error("too many recorded input actions");
    }
    frame.actions.reserve(action_count);
    for (std::size_t index = 0; index < action_count; ++index) {
        ActionState action;
        action.name = reader.read_string();
        action.kind = static_cast<ActionKind>(reader.read_u8());
        action.value = reader.read_f64();
        action.vector = read_vector(reader);
        action.down = reader.read_bool();
        action.pressed = reader.read_bool();
        action.released = reader.read_bool();
        action.triggered = reader.read_bool();
        action.consumed = reader.read_bool();
        action.held_seconds = reader.read_f64();
        action.source_binding = reader.read_u64();
        frame.actions.push_back(std::move(action));
    }
    const auto text_count = reader.read_u32();
    if (text_count > limits.text_codepoints) {
        throw std::length_error("too many recorded text codepoints");
    }
    frame.text.reserve(text_count);
    for (std::size_t index = 0; index < text_count; ++index) {
        frame.text.push_back(static_cast<char32_t>(reader.read_u32()));
    }
    validate_snapshot(frame, limits);
    return frame;
}

bool parse_integer(std::string_view text, std::uint16_t& value) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_real(std::string_view text, double& value) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value);
}

} // namespace

const ActionState* InputSnapshot::find(std::string_view name) const noexcept {
    const auto found = std::lower_bound(actions.begin(), actions.end(), name,
        [](const ActionState& value, std::string_view requested) { return value.name < requested; });
    return found != actions.end() && found->name == name ? &*found : nullptr;
}

const DeviceSnapshot* InputSnapshot::find(DeviceId id) const noexcept {
    const auto found = std::find_if(devices.begin(), devices.end(),
        [id](const DeviceSnapshot& value) { return value.id == id; });
    return found == devices.end() ? nullptr : &*found;
}

double apply_response(double value, const ResponseCurve& curve) noexcept {
    if (!std::isfinite(value) || !valid_curve(curve)) {
        return 0.0;
    }
    const double magnitude = std::abs(value);
    if (magnitude <= curve.deadzone) {
        return 0.0;
    }
    const double unit = std::clamp((magnitude - curve.deadzone) /
                                   (curve.saturation - curve.deadzone), 0.0, 1.0);
    const double shaped = std::pow(unit, curve.exponent) * curve.sensitivity;
    return std::copysign(shaped, curve.invert ? -value : value);
}

ge::math::Vec2 apply_response(ge::math::Vec2 value, const ResponseCurve& curve) noexcept {
    if (!bounded_vector(value) || !valid_curve(curve)) {
        return {};
    }
    if (!curve.radial) {
        return {apply_response(value.x, curve), apply_response(value.y, curve)};
    }
    const double magnitude = std::hypot(value.x, value.y);
    if (magnitude <= curve.deadzone || magnitude == 0.0) {
        return {};
    }
    const double shaped = apply_response(magnitude, curve);
    return value * (shaped / magnitude);
}

Control key(std::uint16_t code) noexcept {
    return {DeviceKind::Keyboard, ControlKind::Button, code, {}};
}

Control mouse_button(std::uint16_t code) noexcept {
    return {DeviceKind::Mouse, ControlKind::Button, code, {}};
}

Control gamepad_button(std::uint16_t code, DeviceId id) noexcept {
    return {DeviceKind::Gamepad, ControlKind::Button, code, id};
}

Control gamepad_axis(std::uint16_t code, DeviceId id) noexcept {
    return {DeviceKind::Gamepad, ControlKind::Axis, code, id};
}

std::vector<std::byte> encode_recording(std::span<const InputSnapshot> frames, InputLimits limits) {
    validate_limits(limits);
    if (frames.size() > limits.recording_frames) {
        throw std::length_error("input recording frame limit exceeded");
    }
    core::ByteWriter writer(limits.recording_bytes);
    writer.write_bytes("GEINPUT1");
    writer.write_u32(1);
    writer.write_u32(static_cast<std::uint32_t>(frames.size()));
    std::size_t memory = 0;
    std::optional<std::uint64_t> previous_tick;
    for (const auto& frame : frames) {
        validate_snapshot(frame, limits);
        const auto bytes = snapshot_bytes(frame);
        if (bytes > limits.recording_bytes - memory ||
            (previous_tick && frame.tick <= *previous_tick)) {
            throw std::length_error("input recording size or tick order is invalid");
        }
        memory += bytes;
        previous_tick = frame.tick;
        write_snapshot(writer, frame);
    }
    writer.write_u32(core::crc32(writer.bytes()));
    return writer.take();
}

std::vector<InputSnapshot> decode_recording(std::span<const std::byte> bytes, InputLimits limits) {
    validate_limits(limits);
    if (bytes.size() < 20 || bytes.size() > limits.recording_bytes) {
        throw std::length_error("input recording byte count is invalid");
    }
    core::BinaryLimits binary_limits;
    binary_limits.max_total_bytes = limits.recording_bytes;
    binary_limits.max_string_bytes = maximum_name;
    core::ByteReader checksum_reader(bytes.last(4));
    if (core::crc32(bytes.first(bytes.size() - 4)) != checksum_reader.read_u32()) {
        throw std::invalid_argument("input recording checksum does not match");
    }
    core::ByteReader reader(bytes.first(bytes.size() - 4), core::ByteOrder::little, binary_limits);
    reader.expect("GEINPUT1");
    if (reader.read_u32() != 1) {
        throw std::invalid_argument("unsupported input recording version");
    }
    const auto count = reader.read_u32();
    if (count > limits.recording_frames || count > reader.remaining() / 24) {
        throw std::length_error("input recording frame count is invalid");
    }
    std::vector<InputSnapshot> frames;
    frames.reserve(count);
    std::size_t memory = 0;
    for (std::size_t index = 0; index < count; ++index) {
        auto frame = read_snapshot(reader, limits);
        const auto required = snapshot_bytes(frame);
        if (required > limits.recording_bytes - memory ||
            (!frames.empty() && frame.tick <= frames.back().tick)) {
            throw std::length_error("input recording memory or tick order is invalid");
        }
        memory += required;
        frames.push_back(std::move(frame));
    }
    reader.require_end();
    return frames;
}

struct InputSystem::Impl {
    struct DeviceSlot {
        std::uint32_t generation{1};
        bool connected{};
        bool retired{};
        DeviceSnapshot state;
        std::array<std::uint16_t, keyboard_button_count> presses{};
        std::array<std::uint16_t, keyboard_button_count> releases{};
        std::bitset<control_slots> consumed;
        std::bitset<control_slots> suppressed;
        std::bitset<control_slots> capture_waiting_neutral;
    };

    struct BindingRuntime {
        bool previous_down{};
        bool hold_fired{};
        bool awaiting_second{};
        double held{};
        double since_tap{};
    };

    struct Sample {
        double value{};
        bool available{};
        bool hidden{};
        bool pressed{};
        bool released{};
        std::uint16_t presses{};
        std::uint16_t releases{};
        std::vector<std::pair<std::size_t, std::size_t>> sources;
    };

    explicit Impl(InputLimits requested) : limits(requested), slots(requested.devices),
        queued_per_device(requested.devices), pending_disconnections(requested.devices) {}

    InputLimits limits;
    std::vector<DeviceSlot> slots;
    std::vector<ContextDescription> contexts;
    std::map<std::uint64_t, BindingRuntime> runtime;
    std::uint64_t next_context{1};
    std::uint64_t next_binding{1};
    mutable std::mutex queue_mutex;
    std::deque<InputEvent> queue;
    std::vector<std::size_t> queued_per_device;
    bool overflow_pending{};
    std::optional<bool> pending_focus;
    std::vector<DeviceId> pending_disconnections;
    std::uint64_t accepted_events{};
    std::uint64_t rejected_events{};
    std::uint64_t stale_events{};
    std::uint64_t overflow_recoveries{};
    std::uint64_t processed_events{};
    bool focused{true};
    InputSnapshot snapshot;
    std::uint64_t last_tick{};
    bool has_tick{};
    CaptureOptions capture_options;
    CaptureResult capture_result;
    double capture_elapsed{};
    std::vector<InputSnapshot> recorded;
    std::size_t recorded_bytes{};
    bool recording{};
    std::vector<InputSnapshot> replay;
    std::size_t replay_position{};
    bool replaying{};
    bool replay_loop{};

    DeviceSlot* device(DeviceId id) noexcept {
        if (!id.valid() || id.slot >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[id.slot];
        return slot.connected && slot.state.id == id ? &slot : nullptr;
    }

    const DeviceSlot* device(DeviceId id) const noexcept {
        if (!id.valid() || id.slot >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[id.slot];
        return slot.connected && slot.state.id == id ? &slot : nullptr;
    }

    ContextDescription* context(std::uint64_t id) noexcept {
        const auto found = std::find_if(contexts.begin(), contexts.end(),
            [id](const ContextDescription& value) { return value.id == id; });
        return found == contexts.end() ? nullptr : &*found;
    }

    std::size_t binding_count() const noexcept {
        std::size_t count = 0;
        for (const auto& context_value : contexts) {
            count += context_value.bindings.size();
        }
        return count;
    }

    std::size_t action_count() const noexcept {
        std::size_t count = 0;
        for (const auto& context_value : contexts) {
            count += context_value.actions.size();
        }
        return count;
    }

    void clear_device(DeviceSlot& slot) {
        for (std::size_t code = 0; code < button_count(slot.state.kind); ++code) {
            auto& button = slot.state.buttons[code];
            if (button.down) {
                button.released = true;
                increment(slot.releases[code]);
            }
            button.down = false;
            button.pressed = false;
            button.repeats = 0;
            slot.presses[code] = 0;
        }
        slot.state.axes.fill(0.0);
        slot.state.pointer_delta = {};
        slot.state.wheel = {};
        slot.suppressed.reset();
        slot.capture_waiting_neutral.reset();
    }

    void clear_held() {
        for (auto& slot : slots) {
            if (slot.connected) {
                clear_device(slot);
            }
        }
        for (auto& [id, state] : runtime) {
            static_cast<void>(id);
            state = {};
        }
    }

    void begin_frame() {
        for (auto& slot : slots) {
            slot.consumed.reset();
            for (std::size_t index = keyboard_button_count + gamepad_axis_count; index < control_slots; ++index) {
                slot.suppressed.reset(index);
            }
            slot.presses.fill(0);
            slot.releases.fill(0);
            slot.state.pointer_delta = {};
            slot.state.wheel = {};
            for (auto& button : slot.state.buttons) {
                button.pressed = false;
                button.released = false;
                button.repeats = 0;
            }
        }
    }

    bool capture_event(const InputEvent& event, DeviceSlot& slot) {
        if (capture_result.status != CaptureStatus::Waiting) {
            return false;
        }
        if (slot.state.kind == DeviceKind::Keyboard && event.kind == EventKind::ButtonDown &&
            event.code == capture_options.cancel_key && !event.repeat) {
            capture_result = {CaptureStatus::Cancelled, std::nullopt, 1.0};
            slot.suppressed.set(event.code);
            slot.consumed.set(event.code);
            return true;
        }
        const bool allowed_device =
            (slot.state.kind == DeviceKind::Keyboard && capture_options.keyboard) ||
            (slot.state.kind == DeviceKind::Mouse && capture_options.mouse) ||
            (slot.state.kind == DeviceKind::Gamepad && capture_options.gamepad);
        if (!allowed_device) {
            return false;
        }
        Control candidate{slot.state.kind, ControlKind::Button, event.code, {}};
        double direction = 1.0;
        bool accepted = false;
        if (event.kind == EventKind::ButtonDown && capture_options.buttons && !event.repeat) {
            accepted = !slot.state.buttons[event.code].down &&
                       !slot.capture_waiting_neutral.test(event.code);
        } else if (event.kind == EventKind::Axis && capture_options.axes) {
            candidate.kind = ControlKind::Axis;
            const auto index = control_index(candidate);
            if (std::abs(event.value) < capture_options.axis_threshold * 0.5) {
                slot.capture_waiting_neutral.reset(index);
            }
            accepted = std::abs(event.value) >= capture_options.axis_threshold &&
                       !slot.capture_waiting_neutral.test(index);
            direction = event.value < 0.0 ? -1.0 : 1.0;
        } else if (event.kind == EventKind::Pointer && capture_options.pointer) {
            const bool horizontal = std::abs(event.delta.x) >= std::abs(event.delta.y);
            candidate.kind = horizontal ? ControlKind::PointerX : ControlKind::PointerY;
            candidate.code = 0;
            const double motion = horizontal ? event.delta.x : event.delta.y;
            accepted = std::abs(motion) >= capture_options.axis_threshold;
            direction = motion < 0.0 ? -1.0 : 1.0;
        } else if (event.kind == EventKind::Wheel && capture_options.wheel) {
            const bool horizontal = std::abs(event.delta.x) > std::abs(event.delta.y);
            candidate.kind = horizontal ? ControlKind::WheelX : ControlKind::WheelY;
            candidate.code = 0;
            const double motion = horizontal ? event.delta.x : event.delta.y;
            accepted = std::abs(motion) >= capture_options.axis_threshold;
            direction = motion < 0.0 ? -1.0 : 1.0;
        }
        if (!accepted) {
            return false;
        }
        if (capture_options.bind_device_instance) {
            candidate.instance = slot.state.id;
        }
        capture_result = {CaptureStatus::Captured, candidate, direction};
        slot.suppressed.set(control_index(candidate));
        slot.consumed.set(control_index(candidate));
        return true;
    }

    void apply_event(const InputEvent& event, std::u32string& text) {
        increment(processed_events);
        if (event.kind == EventKind::FocusLost) {
            focused = false;
            clear_held();
            text.clear();
            if (capture_result.status == CaptureStatus::Waiting) {
                capture_result = {CaptureStatus::Cancelled, std::nullopt, 1.0};
            }
            return;
        }
        if (event.kind == EventKind::FocusGained) {
            focused = true;
            return;
        }
        auto* slot = device(event.device);
        if (slot == nullptr) {
            increment(stale_events);
            return;
        }
        if (event.kind == EventKind::Disconnected) {
            disconnect(*slot);
            return;
        }
        if (!focused) {
            return;
        }
        const bool button_event = event.kind == EventKind::ButtonDown || event.kind == EventKind::ButtonUp;
        if ((button_event && event.code >= button_count(slot->state.kind)) ||
            (event.kind == EventKind::Axis && (slot->state.kind != DeviceKind::Gamepad ||
                                             event.code >= gamepad_axis_count)) ||
            ((event.kind == EventKind::Pointer || event.kind == EventKind::Wheel) &&
             slot->state.kind != DeviceKind::Mouse) ||
            (event.kind == EventKind::Text && slot->state.kind != DeviceKind::Keyboard)) {
            increment(stale_events);
            return;
        }
        static_cast<void>(capture_event(event, *slot));
        switch (event.kind) {
        case EventKind::ButtonDown: {
            auto& button = slot->state.buttons[event.code];
            if (!button.down && !event.repeat) {
                button.down = true;
                button.pressed = true;
                increment(slot->presses[event.code]);
            } else if (button.down && event.repeat) {
                increment(button.repeats);
            }
            break;
        }
        case EventKind::ButtonUp: {
            auto& button = slot->state.buttons[event.code];
            if (button.down) {
                button.down = false;
                button.released = true;
                increment(slot->releases[event.code]);
            }
            slot->suppressed.reset(event.code);
            slot->capture_waiting_neutral.reset(event.code);
            break;
        }
        case EventKind::Axis:
            slot->state.axes[event.code] = std::clamp(event.value, -1.0, 1.0);
            if (std::abs(event.value) < 0.1) {
                slot->suppressed.reset(keyboard_button_count + event.code);
            }
            break;
        case EventKind::Pointer:
            slot->state.pointer = event.position;
            slot->state.pointer_delta.x = std::clamp(slot->state.pointer_delta.x + event.delta.x,
                                                   -maximum_motion, maximum_motion);
            slot->state.pointer_delta.y = std::clamp(slot->state.pointer_delta.y + event.delta.y,
                                                   -maximum_motion, maximum_motion);
            break;
        case EventKind::Wheel:
            slot->state.wheel.x = std::clamp(slot->state.wheel.x + event.delta.x,
                                           -maximum_motion, maximum_motion);
            slot->state.wheel.y = std::clamp(slot->state.wheel.y + event.delta.y,
                                           -maximum_motion, maximum_motion);
            break;
        case EventKind::Text:
            if (text.size() < limits.text_codepoints) {
                text.push_back(event.text);
            }
            break;
        case EventKind::FocusGained:
        case EventKind::FocusLost:
        case EventKind::Disconnected:
            break;
        }
    }

    static double control_value(const DeviceSlot& slot, const Control& control) {
        switch (control.kind) {
        case ControlKind::Button: return slot.state.buttons[control.code].down ? 1.0 : 0.0;
        case ControlKind::Axis: return slot.state.axes[control.code];
        case ControlKind::PointerX: return slot.state.pointer_delta.x;
        case ControlKind::PointerY: return slot.state.pointer_delta.y;
        case ControlKind::WheelX: return slot.state.wheel.x;
        case ControlKind::WheelY: return slot.state.wheel.y;
        }
        return 0.0;
    }

    Sample sample(const Control& control, bool include_hidden = false) const {
        Sample result;
        const auto index = control_index(control);
        for (std::size_t slot_index = 0; slot_index < slots.size(); ++slot_index) {
            const auto& slot = slots[slot_index];
            if (!slot.connected || slot.state.kind != control.device ||
                (control.instance.valid() && slot.state.id != control.instance)) {
                continue;
            }
            if (!include_hidden && (slot.consumed.test(index) || slot.suppressed.test(index))) {
                result.hidden = true;
                continue;
            }
            result.available = true;
            const double value = control_value(slot, control);
            if (control.kind == ControlKind::Button) {
                const auto& button = slot.state.buttons[control.code];
                result.pressed = result.pressed || button.pressed;
                result.released = result.released || button.released;
                result.presses = std::max(result.presses, slot.presses[control.code]);
                result.releases = std::max(result.releases, slot.releases[control.code]);
                if (button.pressed || button.released) {
                    result.sources.emplace_back(slot_index, index);
                }
            }
            if (value != 0.0) {
                result.sources.emplace_back(slot_index, index);
            }
            if (std::abs(value) > std::abs(result.value)) {
                result.value = value;
            }
        }
        return result;
    }

    void disconnect(DeviceSlot& slot) {
        slot.connected = false;
        for (const auto& context_value : contexts) {
            for (const auto& binding : context_value.bindings) {
                // Immediate actions need their existing edge state even when the
                // aggregate value changes. Timed interactions cancel on input loss.
                if (binding.interaction == Interaction::Immediate) {
                    continue;
                }
                const auto lost = [&](const Control& control, bool chord) {
                    if (control.device != slot.state.kind ||
                        (control.instance.valid() && control.instance != slot.state.id)) {
                        return false;
                    }
                    // Direct removal may follow evaluation, when surviving controls
                    // still carry the preceding frame's consumption masks.
                    const auto remaining = sample(control, true);
                    if (!remaining.available) {
                        return true;
                    }
                    const auto active = [&](double value) {
                        if (chord) {
                            return value >= binding.threshold;
                        }
                        const bool relative = control.kind == ControlKind::PointerX ||
                            control.kind == ControlKind::PointerY || control.kind == ControlKind::WheelX ||
                            control.kind == ControlKind::WheelY;
                        const double response = relative ? value * binding.response.sensitivity :
                            apply_response(value, binding.response);
                        return std::abs(response * binding.scale.x) >= binding.threshold;
                    };
                    return active(control_value(slot, control)) && !active(remaining.value);
                };
                if (lost(binding.controls[0], false) ||
                    std::any_of(binding.chord.begin(), binding.chord.end(),
                        [&](const Control& control) { return lost(control, true); })) {
                    runtime.at(binding.id) = {};
                }
            }
        }
        clear_device(slot);
        if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
            slot.retired = true;
        } else {
            ++slot.generation;
        }
    }

    ActionState evaluate_binding(const Binding& binding, double delta,
                                 std::vector<std::pair<std::size_t, std::size_t>>& used) {
        ActionState result;
        result.name = binding.action;
        result.kind = binding.kind;
        result.source_binding = binding.id;
        auto& state = runtime.at(binding.id);
        bool chord_active = true;
        std::vector<std::pair<std::size_t, std::size_t>> sources;
        for (const auto& control : binding.chord) {
            const auto value = sample(control);
            chord_active = chord_active && value.value >= binding.threshold;
            sources.insert(sources.end(), value.sources.begin(), value.sources.end());
        }
        if (!chord_active) {
            state = {};
            return result;
        }
        std::array<Sample, 4> values;
        for (std::size_t index = 0; index < binding.control_count; ++index) {
            values[index] = sample(binding.controls[index]);
            sources.insert(sources.end(), values[index].sources.begin(), values[index].sources.end());
        }
        ge::math::Vec2 raw;
        if (binding.kind == ActionKind::Vector2 && binding.control_count == 4) {
            raw = {values[1].value - values[0].value, values[3].value - values[2].value};
        } else if (binding.kind == ActionKind::Vector2) {
            raw = {values[0].value, values[1].value};
        } else if (binding.control_count == 2) {
            raw.x = values[1].value - values[0].value;
        } else {
            raw.x = values[0].value;
        }
        bool relative = false;
        for (std::size_t index = 0; index < binding.control_count; ++index) {
            const auto kind = binding.controls[index].kind;
            relative = relative || kind == ControlKind::PointerX || kind == ControlKind::PointerY ||
                       kind == ControlKind::WheelX || kind == ControlKind::WheelY;
        }
        if (relative) {
            const double multiplier = binding.response.sensitivity * (binding.response.invert ? -1.0 : 1.0);
            result.vector = raw * multiplier;
        } else if (binding.kind == ActionKind::Vector2) {
            result.vector = apply_response(raw, binding.response);
        } else {
            result.vector.x = apply_response(raw.x, binding.response);
        }
        result.vector.x *= binding.scale.x;
        result.vector.y *= binding.scale.y;
        result.vector.x = std::clamp(result.vector.x, -maximum_motion, maximum_motion);
        result.vector.y = std::clamp(result.vector.y, -maximum_motion, maximum_motion);
        if (binding.kind == ActionKind::Vector2 && binding.normalize_vector) {
            const double magnitude = std::hypot(result.vector.x, result.vector.y);
            if (magnitude > 1.0) {
                result.vector = result.vector / magnitude;
            }
        }
        result.value = binding.kind == ActionKind::Vector2 ?
            std::min(maximum_motion, std::hypot(result.vector.x, result.vector.y)) : result.vector.x;
        const bool raw_down = binding.kind == ActionKind::Button ?
            std::abs(result.value) >= binding.threshold : std::abs(result.value) > 1.0e-8;
        const bool hidden = std::any_of(values.begin(), values.begin() + binding.control_count,
            [](const Sample& value) { return value.hidden; });
        if (!raw_down && hidden && !values[0].released) {
            // A consumed/captured control does not complete a tap. A visible
            // wildcard source can still supply a genuine release or stay held.
            state = {};
            result.value = 0.0;
            result.vector = {};
            return result;
        }
        const bool rising = (raw_down && !state.previous_down) || values[0].pressed;
        const bool falling = (!raw_down && state.previous_down) || values[0].released;
        const bool completed_cycle = falling && (state.previous_down || values[0].pressed);
        const double previous_held = state.held;
        state.held = raw_down ? std::min(maximum_elapsed, state.held + delta) : 0.0;
        if (state.awaiting_second) {
            state.since_tap += delta;
            if (state.since_tap > binding.double_tap_seconds) {
                state.awaiting_second = false;
            }
        }
        result.held_seconds = state.held;
        switch (binding.interaction) {
        case Interaction::Immediate:
            result.down = raw_down;
            result.pressed = rising;
            result.released = falling;
            result.triggered = rising;
            break;
        case Interaction::Tap: {
            const bool short_release = completed_cycle && previous_held <= binding.tap_seconds;
            result.triggered = short_release;
            result.pressed = short_release;
            result.value = short_release ? 1.0 : 0.0;
            result.vector = {result.value, 0.0};
            break;
        }
        case Interaction::Hold:
            result.down = raw_down && state.held >= binding.hold_seconds;
            result.triggered = result.down && !state.hold_fired;
            result.pressed = result.triggered;
            result.released = falling && state.hold_fired;
            result.value = result.down ? 1.0 : 0.0;
            result.vector = {result.value, 0.0};
            if (result.triggered) {
                state.hold_fired = true;
            }
            if (!raw_down) {
                state.hold_fired = false;
            }
            break;
        case Interaction::DoubleTap:
            if (completed_cycle && previous_held <= binding.tap_seconds) {
                if (state.awaiting_second || values[0].releases >= 2) {
                    result.triggered = true;
                    state.awaiting_second = false;
                } else {
                    state.awaiting_second = true;
                    state.since_tap = 0.0;
                }
            }
            result.pressed = result.triggered;
            result.value = result.triggered ? 1.0 : 0.0;
            result.vector = {result.value, 0.0};
            break;
        }
        state.previous_down = raw_down;
        if (binding.consume && (raw_down || rising || falling)) {
            used.insert(used.end(), sources.begin(), sources.end());
        }
        return result;
    }

    void evaluate(InputSnapshot& frame) {
        std::vector<const ContextDescription*> ordered;
        ordered.reserve(contexts.size());
        for (const auto& context_value : contexts) {
            if (context_value.enabled) {
                ordered.push_back(&context_value);
            } else {
                for (const auto& binding : context_value.bindings) {
                    runtime.at(binding.id) = {};
                }
            }
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
            return left->priority != right->priority ? left->priority > right->priority : left->id < right->id;
        });
        std::map<std::string, ActionState, std::less<>> actions;
        bool blocked = !focused;
        for (const auto* context_value : ordered) {
            std::vector<std::string_view> owned;
            for (const auto& definition : context_value->actions) {
                ActionState state;
                state.name = definition.name;
                state.kind = definition.kind;
                const auto inserted = actions.emplace(definition.name, std::move(state));
                if (inserted.second) {
                    owned.push_back(definition.name);
                }
            }
            std::vector<std::pair<std::size_t, std::size_t>> used;
            for (const auto& binding : context_value->bindings) {
                if (blocked || std::find(owned.begin(), owned.end(), binding.action) == owned.end()) {
                    runtime.at(binding.id) = {};
                    continue;
                }
                auto value = evaluate_binding(binding, frame.delta_seconds, used);
                auto& combined = actions.at(binding.action);
                const bool replace = std::abs(value.value) > std::abs(combined.value) ||
                                     (combined.source_binding == 0 && (value.pressed || value.released));
                if (replace) {
                    combined.value = value.value;
                    combined.vector = value.vector;
                    combined.source_binding = value.source_binding;
                }
                combined.down = combined.down || value.down;
                combined.pressed = combined.pressed || value.pressed;
                combined.released = combined.released || value.released;
                combined.triggered = combined.triggered || value.triggered;
                combined.held_seconds = std::max(combined.held_seconds, value.held_seconds);
            }
            for (const auto& [slot, index] : used) {
                slots[slot].consumed.set(index);
            }
            blocked = blocked || context_value->block_lower;
        }
        frame.actions.reserve(actions.size());
        for (auto& [name, value] : actions) {
            const auto* previous = snapshot.find(name);
            value.released = (previous != nullptr && previous->down && !value.down) ||
                             (value.released && !value.down);
            value.pressed = value.pressed || (value.down && (previous == nullptr || !previous->down));
            frame.actions.push_back(std::move(value));
        }
        for (const auto& old : snapshot.actions) {
            if (old.down && actions.find(old.name) == actions.end() && frame.actions.size() < limits.actions) {
                ActionState released;
                released.name = old.name;
                released.kind = old.kind;
                released.released = true;
                frame.actions.push_back(std::move(released));
            }
        }
        std::sort(frame.actions.begin(), frame.actions.end(),
            [](const ActionState& left, const ActionState& right) { return left.name < right.name; });
    }
};

InputSystem::InputSystem(InputLimits limits) {
    validate_limits(limits);
    impl_ = std::make_unique<Impl>(limits);
    static_cast<void>(connect_device(DeviceKind::Keyboard, "Keyboard"));
    static_cast<void>(connect_device(DeviceKind::Mouse, "Mouse"));
}

InputSystem::~InputSystem() = default;

const InputLimits& InputSystem::limits() const noexcept {
    return impl_->limits;
}

DeviceId InputSystem::keyboard() const noexcept {
    for (const auto& slot : impl_->slots) {
        if (slot.connected && slot.state.kind == DeviceKind::Keyboard) {
            return slot.state.id;
        }
    }
    return {};
}

DeviceId InputSystem::mouse() const noexcept {
    for (const auto& slot : impl_->slots) {
        if (slot.connected && slot.state.kind == DeviceKind::Mouse) {
            return slot.state.id;
        }
    }
    return {};
}

DeviceId InputSystem::connect_device(DeviceKind kind, std::string name) {
    if (!valid_device_kind(kind) || !valid_name(name)) {
        return {};
    }
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        auto& slot = impl_->slots[index];
        if (slot.connected || slot.retired) {
            continue;
        }
        DeviceSnapshot state;
        state.id = {static_cast<std::uint16_t>(index), slot.generation};
        state.kind = kind;
        state.name = std::move(name);
        slot.state = std::move(state);
        slot.connected = true;
        slot.presses.fill(0);
        slot.releases.fill(0);
        slot.consumed.reset();
        slot.suppressed.reset();
        slot.capture_waiting_neutral.reset();
        return slot.state.id;
    }
    return {};
}

bool InputSystem::disconnect_device(DeviceId id) {
    auto* slot = impl_->device(id);
    if (slot == nullptr) {
        return false;
    }
    impl_->disconnect(*slot);
    return true;
}

bool InputSystem::connected(DeviceId id) const noexcept {
    return impl_->device(id) != nullptr;
}

std::vector<DeviceId> InputSystem::devices(DeviceKind kind) const {
    std::vector<DeviceId> result;
    for (const auto& slot : impl_->slots) {
        if (slot.connected && slot.state.kind == kind) {
            result.push_back(slot.state.id);
        }
    }
    return result;
}

bool InputSystem::enqueue(InputEvent event) {
    const bool focus_event = event.kind == EventKind::FocusLost || event.kind == EventKind::FocusGained;
    const bool known_kind = event.kind >= EventKind::ButtonDown && event.kind <= EventKind::Disconnected;
    const bool valid = known_kind && bounded_number(event.value) && bounded_vector(event.position) &&
                       bounded_vector(event.delta) &&
                       (event.kind != EventKind::Text || valid_codepoint(event.text)) &&
                       (focus_event || (event.device.valid() && event.device.slot < impl_->limits.devices)) &&
                       (event.kind != EventKind::Axis || event.code < gamepad_axis_count) &&
                       ((event.kind != EventKind::ButtonDown && event.kind != EventKind::ButtonUp) ||
                        event.code < keyboard_button_count);
    std::scoped_lock lock(impl_->queue_mutex);
    if (!valid) {
        increment(impl_->rejected_events);
        return false;
    }
    const std::size_t slot = focus_event ? 0 : event.device.slot;
    if (impl_->queue.size() >= impl_->limits.queued_events ||
        impl_->queued_per_device[slot] >= impl_->limits.events_per_device) {
        increment(impl_->rejected_events);
        impl_->overflow_pending = true;
        if (focus_event) {
            impl_->pending_focus = event.kind == EventKind::FocusGained;
        } else if (event.kind == EventKind::Disconnected &&
                   event.device.generation > impl_->pending_disconnections[slot].generation) {
            impl_->pending_disconnections[slot] = event.device;
        }
        return false;
    }
    impl_->queue.push_back(event);
    ++impl_->queued_per_device[slot];
    increment(impl_->accepted_events);
    return true;
}

void InputSystem::update(std::uint64_t tick, double delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0 || delta_seconds > 60.0 ||
        (impl_->has_tick && tick <= impl_->last_tick)) {
        throw std::invalid_argument("input update requires increasing ticks and a finite delta in [0,60]");
    }
    impl_->last_tick = tick;
    impl_->has_tick = true;
    std::deque<InputEvent> incoming;
    bool overflow = false;
    std::optional<bool> pending_focus;
    std::vector<DeviceId> pending_disconnections(impl_->slots.size());
    {
        std::scoped_lock lock(impl_->queue_mutex);
        incoming.swap(impl_->queue);
        impl_->queued_per_device.assign(impl_->slots.size(), 0);
        overflow = std::exchange(impl_->overflow_pending, false);
        pending_focus = std::exchange(impl_->pending_focus, std::nullopt);
        pending_disconnections.swap(impl_->pending_disconnections);
    }
    if (impl_->replaying && impl_->replay_position == impl_->replay.size()) {
        if (impl_->replay_loop) {
            impl_->replay_position = 0;
        } else {
            impl_->replaying = false;
            impl_->clear_held();
            std::erase_if(incoming, [](const InputEvent& event) {
                return event.kind != EventKind::FocusLost && event.kind != EventKind::FocusGained &&
                       event.kind != EventKind::Disconnected;
            });
        }
    }
    if (impl_->replaying) {
        std::u32string discarded_text;
        for (const auto& event : incoming) {
            if (event.kind == EventKind::FocusLost || event.kind == EventKind::FocusGained ||
                event.kind == EventKind::Disconnected) {
                impl_->apply_event(event, discarded_text);
            }
        }
        for (const auto id : pending_disconnections) {
            if (id.valid()) {
                InputEvent event;
                event.kind = EventKind::Disconnected;
                event.device = id;
                impl_->apply_event(event, discarded_text);
            }
        }
        if (pending_focus) {
            InputEvent event;
            event.kind = *pending_focus ? EventKind::FocusGained : EventKind::FocusLost;
            impl_->apply_event(event, discarded_text);
        }
        impl_->snapshot = impl_->replay[impl_->replay_position];
        ++impl_->replay_position;
        return;
    }
    impl_->begin_frame();
    InputSnapshot frame;
    frame.tick = tick;
    frame.delta_seconds = delta_seconds;
    frame.overflow_recovered = overflow;
    if (overflow) {
        impl_->clear_held();
        increment(impl_->overflow_recoveries);
        for (const auto& event : incoming) {
            if (event.kind == EventKind::FocusLost || event.kind == EventKind::FocusGained ||
                event.kind == EventKind::Disconnected) {
                impl_->apply_event(event, frame.text);
            }
        }
    } else {
        for (const auto& event : incoming) {
            impl_->apply_event(event, frame.text);
        }
    }
    for (const auto id : pending_disconnections) {
        if (id.valid()) {
            InputEvent event;
            event.kind = EventKind::Disconnected;
            event.device = id;
            impl_->apply_event(event, frame.text);
        }
    }
    if (pending_focus) {
        InputEvent event;
        event.kind = *pending_focus ? EventKind::FocusGained : EventKind::FocusLost;
        impl_->apply_event(event, frame.text);
    }
    if (impl_->capture_result.status == CaptureStatus::Waiting) {
        impl_->capture_elapsed += delta_seconds;
        if (impl_->capture_elapsed >= impl_->capture_options.timeout_seconds) {
            impl_->capture_result = {CaptureStatus::TimedOut, std::nullopt, 1.0};
        }
    }
    frame.focused = impl_->focused;
    for (const auto& slot : impl_->slots) {
        if (slot.connected) {
            frame.devices.push_back(slot.state);
        }
    }
    impl_->evaluate(frame);
    impl_->snapshot = std::move(frame);
    if (impl_->recording) {
        const auto bytes = snapshot_bytes(impl_->snapshot);
        if (impl_->recorded.size() >= impl_->limits.recording_frames ||
            bytes > impl_->limits.recording_bytes - impl_->recorded_bytes) {
            impl_->recording = false;
        } else {
            impl_->recorded.push_back(impl_->snapshot);
            impl_->recorded_bytes += bytes;
        }
    }
}

void InputSystem::clear() {
    {
        std::scoped_lock lock(impl_->queue_mutex);
        impl_->queue.clear();
        std::fill(impl_->queued_per_device.begin(), impl_->queued_per_device.end(), 0);
        impl_->overflow_pending = false;
        impl_->pending_focus.reset();
        std::fill(impl_->pending_disconnections.begin(), impl_->pending_disconnections.end(), DeviceId{});
    }
    impl_->clear_held();
    cancel_capture();
    impl_->snapshot.text.clear();
    for (auto& action_state : impl_->snapshot.actions) {
        action_state.released = action_state.down;
        action_state.down = false;
        action_state.pressed = false;
        action_state.triggered = false;
        action_state.value = 0.0;
        action_state.vector = {};
        action_state.held_seconds = 0.0;
        action_state.consumed = false;
    }
    impl_->snapshot.devices.clear();
    for (const auto& slot : impl_->slots) {
        if (slot.connected) {
            impl_->snapshot.devices.push_back(slot.state);
        }
    }
}

const InputSnapshot& InputSystem::snapshot() const noexcept {
    return impl_->snapshot;
}

const ActionState* InputSystem::action(std::string_view name) const noexcept {
    return impl_->snapshot.find(name);
}

bool InputSystem::down(std::string_view name) const noexcept {
    const auto* value = action(name);
    return value != nullptr && !value->consumed && value->down;
}

bool InputSystem::pressed(std::string_view name) const noexcept {
    const auto* value = action(name);
    return value != nullptr && !value->consumed && value->pressed;
}

bool InputSystem::released(std::string_view name) const noexcept {
    const auto* value = action(name);
    return value != nullptr && !value->consumed && value->released;
}

bool InputSystem::triggered(std::string_view name) const noexcept {
    const auto* value = action(name);
    return value != nullptr && !value->consumed && value->triggered;
}

double InputSystem::axis(std::string_view name) const noexcept {
    const auto* value = action(name);
    return value != nullptr && !value->consumed ? value->value : 0.0;
}

ge::math::Vec2 InputSystem::vector2(std::string_view name) const noexcept {
    const auto* value = action(name);
    return value != nullptr && !value->consumed ? value->vector : ge::math::Vec2{};
}

bool InputSystem::consume(std::string_view name) {
    const auto found = std::lower_bound(impl_->snapshot.actions.begin(), impl_->snapshot.actions.end(), name,
        [](const ActionState& value, std::string_view requested) { return value.name < requested; });
    if (found == impl_->snapshot.actions.end() || found->name != name || found->consumed) {
        return false;
    }
    found->consumed = true;
    return true;
}

const ButtonState* InputSystem::button(DeviceId id, std::uint16_t code) const noexcept {
    const auto* device = impl_->snapshot.find(id);
    return device != nullptr && code < button_count(device->kind) ? &device->buttons[code] : nullptr;
}

std::uint64_t InputSystem::create_context(std::string name, int priority) {
    if (!valid_name(name) || impl_->contexts.size() >= impl_->limits.contexts ||
        impl_->next_context == std::numeric_limits<std::uint64_t>::max() || context_id(name) != 0) {
        return 0;
    }
    ContextDescription context_value;
    context_value.id = impl_->next_context;
    context_value.name = std::move(name);
    context_value.priority = priority;
    impl_->contexts.push_back(std::move(context_value));
    return impl_->next_context++;
}

bool InputSystem::remove_context(std::uint64_t id) {
    const auto found = std::find_if(impl_->contexts.begin(), impl_->contexts.end(),
        [id](const ContextDescription& value) { return value.id == id; });
    if (found == impl_->contexts.end()) {
        return false;
    }
    for (const auto& binding : found->bindings) {
        impl_->runtime.erase(binding.id);
    }
    impl_->contexts.erase(found);
    return true;
}

bool InputSystem::set_context_enabled(std::uint64_t id, bool enabled) {
    auto* found = impl_->context(id);
    if (found == nullptr) {
        return false;
    }
    if (found->enabled != enabled) {
        for (const auto& binding : found->bindings) {
            impl_->runtime.at(binding.id) = {};
        }
    }
    found->enabled = enabled;
    return true;
}

bool InputSystem::set_context_priority(std::uint64_t id, int priority) {
    auto* found = impl_->context(id);
    if (found == nullptr) {
        return false;
    }
    found->priority = priority;
    return true;
}

bool InputSystem::set_context_blocking(std::uint64_t id, bool blocking) {
    auto* found = impl_->context(id);
    if (found == nullptr) {
        return false;
    }
    found->block_lower = blocking;
    return true;
}

const ContextDescription* InputSystem::context(std::uint64_t id) const noexcept {
    return impl_->context(id);
}

std::uint64_t InputSystem::context_id(std::string_view name) const noexcept {
    const auto found = std::find_if(impl_->contexts.begin(), impl_->contexts.end(),
        [name](const ContextDescription& value) { return value.name == name; });
    return found == impl_->contexts.end() ? 0 : found->id;
}

std::vector<ContextDescription> InputSystem::contexts() const {
    return impl_->contexts;
}

bool InputSystem::define_action(std::uint64_t context_value, ActionDefinition definition) {
    auto* found = impl_->context(context_value);
    if (found == nullptr || !valid_name(definition.name) || !valid_action_kind(definition.kind) ||
        impl_->action_count() >= impl_->limits.actions) {
        return false;
    }
    for (const auto& existing_context : impl_->contexts) {
        for (const auto& existing : existing_context.actions) {
            if (existing.name == definition.name &&
                (existing_context.id == context_value || existing.kind != definition.kind)) {
                return false;
            }
        }
    }
    found->actions.push_back(std::move(definition));
    return true;
}

std::uint64_t InputSystem::bind(std::uint64_t context_value, Binding binding) {
    auto* found = impl_->context(context_value);
    if (found == nullptr || !valid_binding(binding) || impl_->binding_count() >= impl_->limits.bindings ||
        impl_->next_binding == std::numeric_limits<std::uint64_t>::max()) {
        return 0;
    }
    const auto definition = std::find_if(found->actions.begin(), found->actions.end(),
        [&binding](const ActionDefinition& value) {
            return value.name == binding.action && value.kind == binding.kind;
        });
    if (definition == found->actions.end()) {
        return 0;
    }
    binding.id = impl_->next_binding;
    const auto inserted = impl_->runtime.emplace(binding.id, Impl::BindingRuntime{});
    try {
        found->bindings.push_back(std::move(binding));
    } catch (...) {
        impl_->runtime.erase(inserted.first);
        throw;
    }
    return impl_->next_binding++;
}

bool InputSystem::unbind(std::uint64_t binding) {
    for (auto& context_value : impl_->contexts) {
        const auto found = std::find_if(context_value.bindings.begin(), context_value.bindings.end(),
            [binding](const Binding& value) { return value.id == binding; });
        if (found != context_value.bindings.end()) {
            context_value.bindings.erase(found);
            impl_->runtime.erase(binding);
            return true;
        }
    }
    return false;
}

bool InputSystem::replace_binding(std::uint64_t binding, Binding replacement) {
    if (!valid_binding(replacement)) {
        return false;
    }
    for (auto& context_value : impl_->contexts) {
        const auto found = std::find_if(context_value.bindings.begin(), context_value.bindings.end(),
            [binding](const Binding& value) { return value.id == binding; });
        if (found == context_value.bindings.end()) {
            continue;
        }
        const auto action_definition = std::find_if(context_value.actions.begin(), context_value.actions.end(),
            [&replacement](const ActionDefinition& value) {
                return value.name == replacement.action && value.kind == replacement.kind;
            });
        if (action_definition == context_value.actions.end()) {
            return false;
        }
        replacement.id = binding;
        *found = std::move(replacement);
        impl_->runtime.at(binding) = {};
        return true;
    }
    return false;
}

bool InputSystem::rebind_control(std::uint64_t binding, std::size_t index, Control control) {
    if (!valid_control(control)) {
        return false;
    }
    for (auto& context_value : impl_->contexts) {
        for (auto& value : context_value.bindings) {
            if (value.id == binding && index < value.control_count) {
                value.controls[index] = control;
                impl_->runtime.at(binding) = {};
                return true;
            }
        }
    }
    return false;
}

bool InputSystem::begin_capture(CaptureOptions options) {
    if (impl_->replaying || !std::isfinite(options.axis_threshold) || options.axis_threshold <= 0.0 ||
        options.axis_threshold > 1.0 || !std::isfinite(options.timeout_seconds) ||
        options.timeout_seconds <= 0.0 || options.timeout_seconds > 120.0 ||
        options.cancel_key >= keyboard_button_count || (!options.keyboard && !options.mouse && !options.gamepad) ||
        (!options.buttons && !options.axes && !options.pointer && !options.wheel)) {
        return false;
    }
    impl_->capture_options = options;
    impl_->capture_result = {CaptureStatus::Waiting, std::nullopt, 1.0};
    impl_->capture_elapsed = 0.0;
    for (auto& slot : impl_->slots) {
        slot.capture_waiting_neutral.reset();
        for (std::size_t code = 0; code < button_count(slot.state.kind); ++code) {
            if (slot.state.buttons[code].down) {
                slot.capture_waiting_neutral.set(code);
            }
        }
        for (std::size_t code = 0; code < slot.state.axes.size(); ++code) {
            if (std::abs(slot.state.axes[code]) >= options.axis_threshold * 0.5) {
                slot.capture_waiting_neutral.set(keyboard_button_count + code);
            }
        }
    }
    return true;
}

void InputSystem::cancel_capture() {
    if (impl_->capture_result.status == CaptureStatus::Waiting) {
        impl_->capture_result = {CaptureStatus::Cancelled, std::nullopt, 1.0};
    }
}

const CaptureResult& InputSystem::capture() const noexcept {
    return impl_->capture_result;
}

bool InputSystem::apply_capture(std::uint64_t binding, std::size_t index) {
    if (impl_->capture_result.status != CaptureStatus::Captured || !impl_->capture_result.control) {
        return false;
    }
    if (!rebind_control(binding, index, *impl_->capture_result.control)) {
        return false;
    }
    impl_->capture_result = {};
    return true;
}

bool InputSystem::start_recording() {
    if (impl_->replaying) {
        return false;
    }
    impl_->recorded.clear();
    impl_->recorded_bytes = 0;
    impl_->recording = true;
    return true;
}

void InputSystem::stop_recording() noexcept {
    impl_->recording = false;
}

void InputSystem::clear_recording() {
    impl_->recorded.clear();
    impl_->recorded_bytes = 0;
}

std::span<const InputSnapshot> InputSystem::recording() const noexcept {
    return impl_->recorded;
}

bool InputSystem::start_replay(std::span<const InputSnapshot> frames, bool loop) {
    if (frames.empty() || frames.size() > impl_->limits.recording_frames) {
        return false;
    }
    std::size_t memory = 0;
    std::optional<std::uint64_t> previous;
    try {
        for (const auto& frame : frames) {
            validate_snapshot(frame, impl_->limits);
            const auto bytes = snapshot_bytes(frame);
            if (bytes > impl_->limits.recording_bytes - memory || (previous && frame.tick <= *previous)) {
                return false;
            }
            memory += bytes;
            previous = frame.tick;
        }
    } catch (const std::invalid_argument&) {
        return false;
    }
    std::vector<InputSnapshot> prepared(frames.begin(), frames.end());
    clear();
    impl_->recording = false;
    impl_->replay = std::move(prepared);
    impl_->replay_position = 0;
    impl_->replay_loop = loop;
    impl_->replaying = true;
    return true;
}

void InputSystem::stop_replay() {
    impl_->replaying = false;
    impl_->replay_position = 0;
    impl_->replay.clear();
    clear();
}

InputStatistics InputSystem::statistics() const {
    InputStatistics result;
    {
        std::scoped_lock lock(impl_->queue_mutex);
        result.accepted_events = impl_->accepted_events;
        result.rejected_events = impl_->rejected_events;
        result.queued_events = impl_->queue.size();
    }
    result.stale_events = impl_->stale_events;
    result.overflow_recoveries = impl_->overflow_recoveries;
    result.processed_events = impl_->processed_events;
    result.connected_devices = static_cast<std::size_t>(std::count_if(impl_->slots.begin(), impl_->slots.end(),
        [](const Impl::DeviceSlot& slot) { return slot.connected; }));
    result.contexts = impl_->contexts.size();
    result.bindings = impl_->binding_count();
    result.recorded_frames = impl_->recorded.size();
    result.replay_position = impl_->replay_position;
    result.recording = impl_->recording;
    result.replaying = impl_->replaying;
    return result;
}

void register_actions(ge::core::Kernel& kernel) {
    if (!kernel.register_module({"input.actions", "Bounded injected input, action maps and frame replay", 1})) {
        return;
    }
    auto& input = kernel.services().emplace<InputSystem>();
    const auto gameplay = input.create_context("gameplay", 0);
    const auto add_button = [&input, gameplay](std::string name, std::uint16_t code) {
        static_cast<void>(input.define_action(gameplay, {name, ActionKind::Button}));
        Binding binding;
        binding.action = std::move(name);
        binding.controls[0] = key(code);
        binding.response.deadzone = 0.0;
        static_cast<void>(input.bind(gameplay, std::move(binding)));
    };
    add_button("jump", 32);
    add_button("interact", 69);
    add_button("sprint", 16);
    add_button("pause", 27);
    static_cast<void>(input.define_action(gameplay, {"move", ActionKind::Vector2}));
    Binding move;
    move.action = "move";
    move.kind = ActionKind::Vector2;
    move.control_count = 4;
    move.controls = {key(65), key(68), key(83), key(87)};
    move.response.deadzone = 0.0;
    static_cast<void>(input.bind(gameplay, move));
    move.control_count = 2;
    move.controls[0] = gamepad_axis(0);
    move.controls[1] = gamepad_axis(1);
    move.response.deadzone = 0.15;
    move.scale.y = -1.0;
    static_cast<void>(input.bind(gameplay, move));
    static_cast<void>(input.define_action(gameplay, {"look", ActionKind::Vector2}));
    Binding look;
    look.action = "look";
    look.kind = ActionKind::Vector2;
    look.control_count = 2;
    look.controls[0] = {DeviceKind::Mouse, ControlKind::PointerX, 0, {}};
    look.controls[1] = {DeviceKind::Mouse, ControlKind::PointerY, 0, {}};
    look.response.deadzone = 0.0;
    look.normalize_vector = false;
    static_cast<void>(input.bind(gameplay, look));
    look.controls[0] = gamepad_axis(2);
    look.controls[1] = gamepad_axis(3);
    look.response.deadzone = 0.15;
    static_cast<void>(input.bind(gameplay, look));
    kernel.add_system({"input.actions", core::Phase::Input, -100, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& service = host.services().get<InputSystem>();
            for (auto& event : host.events().drain<InputEvent>()) {
                static_cast<void>(service.enqueue(std::move(event)));
            }
            service.update(frame.tick, frame.delta_seconds);
            const auto stats = service.statistics();
            host.metric("input.devices", static_cast<double>(stats.connected_devices));
            host.metric("input.events.processed", static_cast<double>(stats.processed_events));
            host.metric("input.events.rejected", static_cast<double>(stats.rejected_events));
            host.metric("input.events.stale", static_cast<double>(stats.stale_events));
            host.metric("input.overflow_recoveries", static_cast<double>(stats.overflow_recoveries));
            host.metric("input.actions", static_cast<double>(service.snapshot().actions.size()));
            host.metric("input.recorded_frames", static_cast<double>(stats.recorded_frames));
            host.metric("input.replaying", stats.replaying ? 1.0 : 0.0);
        });
    kernel.add_command("input.event", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& service = host.services().get<InputSystem>();
        if (args.empty()) {
            return {false, "usage: input.event key|mouse CODE down|up; pointer X Y DX DY; wheel X Y; focus on|off"};
        }
        InputEvent event;
        if ((args[0] == "key" || args[0] == "mouse") && args.size() == 3) {
            event.device = args[0] == "key" ? service.keyboard() : service.mouse();
            if (!parse_integer(args[1], event.code) || (args[2] != "down" && args[2] != "up")) {
                return {false, "invalid button code or transition"};
            }
            event.kind = args[2] == "down" ? EventKind::ButtonDown : EventKind::ButtonUp;
        } else if (args[0] == "pointer" && args.size() == 5) {
            event.device = service.mouse();
            event.kind = EventKind::Pointer;
            if (!parse_real(args[1], event.position.x) || !parse_real(args[2], event.position.y) ||
                !parse_real(args[3], event.delta.x) || !parse_real(args[4], event.delta.y)) {
                return {false, "invalid pointer coordinates"};
            }
        } else if (args[0] == "wheel" && args.size() == 3) {
            event.device = service.mouse();
            event.kind = EventKind::Wheel;
            if (!parse_real(args[1], event.delta.x) || !parse_real(args[2], event.delta.y)) {
                return {false, "invalid wheel delta"};
            }
        } else if (args[0] == "focus" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
            event.kind = args[1] == "on" ? EventKind::FocusGained : EventKind::FocusLost;
        } else {
            return {false, "unrecognized input event syntax"};
        }
        const bool accepted = service.enqueue(event);
        return {accepted, accepted ? "input event queued" : "input event rejected"};
    }, "Inject keyboard, mouse, pointer, wheel and focus events at the backend boundary");
    kernel.add_command("input.bind", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4) {
            return {false, "usage: input.bind CONTEXT ACTION key|mouse|gamepad|axis CODE"};
        }
        auto& service = host.services().get<InputSystem>();
        const auto context_value = service.context_id(args[0]);
        std::uint16_t code{};
        if (context_value == 0 || !parse_integer(args[3], code)) {
            return {false, "unknown context or invalid control code"};
        }
        Binding binding;
        binding.action = args[1];
        if (args[2] == "key") {
            binding.controls[0] = key(code);
        } else if (args[2] == "mouse") {
            binding.controls[0] = mouse_button(code);
        } else if (args[2] == "gamepad") {
            binding.controls[0] = gamepad_button(code);
        } else if (args[2] == "axis") {
            binding.controls[0] = gamepad_axis(code);
            binding.kind = ActionKind::Axis;
        } else {
            return {false, "unknown control kind"};
        }
        if (!valid_binding(binding)) {
            return {false, "invalid binding definition"};
        }
        const auto* existing = service.context(context_value);
        const bool defined = std::any_of(existing->actions.begin(), existing->actions.end(),
            [&binding](const ActionDefinition& definition) { return definition.name == binding.action; });
        if (!defined && !service.define_action(context_value, {binding.action, binding.kind})) {
            return {false, "could not define action"};
        }
        const auto id = service.bind(context_value, std::move(binding));
        return {id != 0, id == 0 ? "binding rejected" : "binding " + std::to_string(id) + " created"};
    }, "Add a validated physical control binding to an action context");
    kernel.add_command("input.context", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 3) {
            return {false, "usage: input.context create|enable|disable|block|unblock|remove NAME [PRIORITY]"};
        }
        auto& service = host.services().get<InputSystem>();
        if (args[0] == "create") {
            int priority = 0;
            if (args.size() == 3) {
                const auto parsed = std::from_chars(args[2].data(), args[2].data() + args[2].size(), priority);
                if (parsed.ec != std::errc{} || parsed.ptr != args[2].data() + args[2].size()) {
                    return {false, "invalid context priority"};
                }
            }
            const auto id = service.create_context(args[1], priority);
            return {id != 0, id == 0 ? "context rejected" : "context " + std::to_string(id) + " created"};
        }
        if (args.size() != 2) {
            return {false, "this context operation takes one name"};
        }
        const auto id = service.context_id(args[1]);
        bool changed = false;
        if (args[0] == "enable" || args[0] == "disable") {
            changed = service.set_context_enabled(id, args[0] == "enable");
        } else if (args[0] == "block" || args[0] == "unblock") {
            changed = service.set_context_blocking(id, args[0] == "block");
        } else if (args[0] == "remove") {
            changed = service.remove_context(id);
        }
        return {changed, changed ? "context updated" : "unknown context or operation"};
    }, "Create and control prioritized action contexts");
    kernel.add_command("input.record", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) {
            return {false, "usage: input.record start|stop|clear|replay|loop|stop-replay"};
        }
        auto& service = host.services().get<InputSystem>();
        if (args[0] == "start") {
            return {service.start_recording(), "recording request processed"};
        }
        if (args[0] == "stop") {
            service.stop_recording();
        } else if (args[0] == "clear") {
            service.clear_recording();
        } else if (args[0] == "replay" || args[0] == "loop") {
            return {service.start_replay(service.recording(), args[0] == "loop"), "replay request processed"};
        } else if (args[0] == "stop-replay") {
            service.stop_replay();
        } else {
            return {false, "unknown recording operation"};
        }
        return {true, "recording state updated"};
    }, "Record bounded input snapshots and replay them deterministically");
    kernel.add_shutdown("input.actions", [service = &input] { service->clear(); });
}

} // namespace ge::input
