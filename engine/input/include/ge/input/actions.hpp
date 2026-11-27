#pragma once

#include <ge/math/math.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core {
class Kernel;
}

namespace ge::input {

inline constexpr std::size_t keyboard_button_count = 512;
inline constexpr std::size_t mouse_button_count = 8;
inline constexpr std::size_t gamepad_button_count = 32;
inline constexpr std::size_t gamepad_axis_count = 8;

enum class DeviceKind : std::uint8_t { Keyboard, Mouse, Gamepad };
enum class EventKind : std::uint8_t {
    ButtonDown,
    ButtonUp,
    Axis,
    Pointer,
    Wheel,
    Text,
    FocusGained,
    FocusLost,
    Disconnected
};
enum class ControlKind : std::uint8_t {
    Button,
    Axis,
    PointerX,
    PointerY,
    WheelX,
    WheelY
};
enum class ActionKind : std::uint8_t { Button, Axis, Vector2 };
enum class Interaction : std::uint8_t { Immediate, Tap, Hold, DoubleTap };
enum class CaptureStatus : std::uint8_t { Idle, Waiting, Captured, Cancelled, TimedOut };

struct DeviceId {
    std::uint16_t slot{std::numeric_limits<std::uint16_t>::max()};
    std::uint32_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation != 0 && slot != std::numeric_limits<std::uint16_t>::max();
    }
    constexpr bool operator==(const DeviceId&) const = default;
};

struct InputEvent {
    EventKind kind{EventKind::ButtonDown};
    DeviceId device{};
    std::uint16_t code{};
    double value{};
    ge::math::Vec2 position{};
    ge::math::Vec2 delta{};
    char32_t text{};
    bool repeat{};
};

struct ButtonState {
    bool down{};
    bool pressed{};
    bool released{};
    std::uint16_t repeats{};
};

struct DeviceSnapshot {
    DeviceId id{};
    DeviceKind kind{DeviceKind::Keyboard};
    std::string name;
    std::array<ButtonState, keyboard_button_count> buttons{};
    std::array<double, gamepad_axis_count> axes{};
    ge::math::Vec2 pointer{};
    ge::math::Vec2 pointer_delta{};
    ge::math::Vec2 wheel{};
};

struct ActionState {
    std::string name;
    ActionKind kind{ActionKind::Button};
    double value{};
    ge::math::Vec2 vector{};
    bool down{};
    bool pressed{};
    bool released{};
    bool triggered{};
    bool consumed{};
    double held_seconds{};
    std::uint64_t source_binding{};
};

struct InputSnapshot {
    std::uint64_t tick{};
    double delta_seconds{};
    bool focused{true};
    bool overflow_recovered{};
    std::vector<DeviceSnapshot> devices;
    std::vector<ActionState> actions;
    std::u32string text;
    [[nodiscard]] const ActionState* find(std::string_view name) const noexcept;
    [[nodiscard]] const DeviceSnapshot* find(DeviceId id) const noexcept;
};

struct Control {
    DeviceKind device{DeviceKind::Keyboard};
    ControlKind kind{ControlKind::Button};
    std::uint16_t code{};
    DeviceId instance{};
    constexpr bool operator==(const Control&) const = default;
};

struct ResponseCurve {
    double deadzone{0.15};
    double saturation{1.0};
    double exponent{1.0};
    double sensitivity{1.0};
    bool invert{};
    bool radial{true};
};

struct ActionDefinition {
    std::string name;
    ActionKind kind{ActionKind::Button};
};

struct Binding {
    std::uint64_t id{};
    std::string action;
    ActionKind kind{ActionKind::Button};
    std::array<Control, 4> controls{};
    std::uint8_t control_count{1};
    std::vector<Control> chord;
    ResponseCurve response{};
    ge::math::Vec2 scale{1.0, 1.0};
    Interaction interaction{Interaction::Immediate};
    double threshold{0.5};
    double tap_seconds{0.25};
    double hold_seconds{0.4};
    double double_tap_seconds{0.3};
    bool consume{true};
    bool normalize_vector{true};
};

struct ContextDescription {
    std::uint64_t id{};
    std::string name;
    int priority{};
    bool enabled{true};
    bool block_lower{};
    std::vector<ActionDefinition> actions;
    std::vector<Binding> bindings;
};

struct InputLimits {
    std::size_t devices{16};
    std::size_t queued_events{4096};
    std::size_t events_per_device{512};
    std::size_t contexts{32};
    std::size_t actions{256};
    std::size_t bindings{1024};
    std::size_t text_codepoints{256};
    std::size_t recording_frames{3600};
    std::size_t recording_bytes{64U * 1024U * 1024U};
};

struct InputStatistics {
    std::uint64_t accepted_events{};
    std::uint64_t rejected_events{};
    std::uint64_t stale_events{};
    std::uint64_t overflow_recoveries{};
    std::uint64_t processed_events{};
    std::size_t queued_events{};
    std::size_t connected_devices{};
    std::size_t contexts{};
    std::size_t bindings{};
    std::size_t recorded_frames{};
    std::size_t replay_position{};
    bool recording{};
    bool replaying{};
};

struct CaptureOptions {
    bool keyboard{true};
    bool mouse{true};
    bool gamepad{true};
    bool buttons{true};
    bool axes{true};
    bool pointer{};
    bool wheel{true};
    bool bind_device_instance{};
    double axis_threshold{0.65};
    double timeout_seconds{10.0};
    std::uint16_t cancel_key{27};
};

struct CaptureResult {
    CaptureStatus status{CaptureStatus::Idle};
    std::optional<Control> control;
    double direction{1.0};
};

[[nodiscard]] double apply_response(double value, const ResponseCurve& curve) noexcept;
[[nodiscard]] ge::math::Vec2 apply_response(ge::math::Vec2 value,
                                          const ResponseCurve& curve) noexcept;
[[nodiscard]] Control key(std::uint16_t code) noexcept;
[[nodiscard]] Control mouse_button(std::uint16_t code) noexcept;
[[nodiscard]] Control gamepad_button(std::uint16_t code, DeviceId id = {}) noexcept;
[[nodiscard]] Control gamepad_axis(std::uint16_t code, DeviceId id = {}) noexcept;
[[nodiscard]] std::vector<std::byte> encode_recording(std::span<const InputSnapshot> frames,
                                                    InputLimits limits = {});
[[nodiscard]] std::vector<InputSnapshot> decode_recording(std::span<const std::byte> bytes,
                                                        InputLimits limits = {});

// Only enqueue() is producer-thread safe. Configuration, frame advancement and
// reads belong to the kernel thread. Snapshot references expire at the next update.
class InputSystem final {
public:
    explicit InputSystem(InputLimits limits = {});
    ~InputSystem();
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;
    InputSystem(InputSystem&&) = delete;
    InputSystem& operator=(InputSystem&&) = delete;

    [[nodiscard]] const InputLimits& limits() const noexcept;
    [[nodiscard]] DeviceId keyboard() const noexcept;
    [[nodiscard]] DeviceId mouse() const noexcept;
    [[nodiscard]] DeviceId connect_device(DeviceKind kind, std::string name);
    bool disconnect_device(DeviceId id);
    [[nodiscard]] bool connected(DeviceId id) const noexcept;
    [[nodiscard]] std::vector<DeviceId> devices(DeviceKind kind) const;
    bool enqueue(InputEvent event);
    void update(std::uint64_t tick, double delta_seconds);
    void clear();
    [[nodiscard]] const InputSnapshot& snapshot() const noexcept;
    [[nodiscard]] const ActionState* action(std::string_view name) const noexcept;
    [[nodiscard]] bool down(std::string_view name) const noexcept;
    [[nodiscard]] bool pressed(std::string_view name) const noexcept;
    [[nodiscard]] bool released(std::string_view name) const noexcept;
    [[nodiscard]] bool triggered(std::string_view name) const noexcept;
    [[nodiscard]] double axis(std::string_view name) const noexcept;
    [[nodiscard]] ge::math::Vec2 vector2(std::string_view name) const noexcept;
    bool consume(std::string_view name);
    [[nodiscard]] const ButtonState* button(DeviceId id, std::uint16_t code) const noexcept;

    [[nodiscard]] std::uint64_t create_context(std::string name, int priority = 0);
    bool remove_context(std::uint64_t id);
    bool set_context_enabled(std::uint64_t id, bool enabled);
    bool set_context_priority(std::uint64_t id, int priority);
    bool set_context_blocking(std::uint64_t id, bool blocking);
    [[nodiscard]] const ContextDescription* context(std::uint64_t id) const noexcept;
    [[nodiscard]] std::uint64_t context_id(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<ContextDescription> contexts() const;
    bool define_action(std::uint64_t context, ActionDefinition action);
    [[nodiscard]] std::uint64_t bind(std::uint64_t context, Binding binding);
    bool unbind(std::uint64_t binding);
    bool replace_binding(std::uint64_t binding, Binding replacement);
    bool rebind_control(std::uint64_t binding, std::size_t index, Control control);

    bool begin_capture(CaptureOptions options = {});
    void cancel_capture();
    [[nodiscard]] const CaptureResult& capture() const noexcept;
    bool apply_capture(std::uint64_t binding, std::size_t index = 0);
    bool start_recording();
    void stop_recording() noexcept;
    void clear_recording();
    [[nodiscard]] std::span<const InputSnapshot> recording() const noexcept;
    // Replay publishes recorded ticks and action states; live lifecycle events still apply.
    // Interaction durations use frame deltas; injected events have no subframe timestamps.
    bool start_replay(std::span<const InputSnapshot> frames, bool loop = false);
    void stop_replay();
    [[nodiscard]] InputStatistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_actions(ge::core::Kernel& kernel);

} // namespace ge::input
