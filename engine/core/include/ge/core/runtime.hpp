#pragma once

#include <algorithm>
#include <any>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ge::core {

class Kernel;

// Services have one owner in the host. Registration is serialized before ticks.
// Borrowed pointers remain valid until the service store is destroyed.
class ServiceStore final {
public:
    explicit ServiceStore(std::size_t capacity = 512) : capacity_(capacity) {}

    template<class T, class... Args>
    T& emplace(Args&&... args) {
        const auto key = std::type_index(typeid(T));
        if (const auto it = services_.find(key); it != services_.end()) {
            return *static_cast<T*>(it->second.get());
        }
        if (services_.size() >= capacity_) {
            throw std::length_error("service capacity exhausted");
        }
        auto value = std::make_shared<T>(std::forward<Args>(args)...);
        auto* result = value.get();
        services_.emplace(key, std::move(value));
        return *result;
    }

    template<class T>
    [[nodiscard]] T* find() noexcept {
        const auto it = services_.find(std::type_index(typeid(T)));
        return it == services_.end() ? nullptr : static_cast<T*>(it->second.get());
    }

    template<class T>
    [[nodiscard]] const T* find() const noexcept {
        const auto it = services_.find(std::type_index(typeid(T)));
        return it == services_.end() ? nullptr : static_cast<const T*>(it->second.get());
    }

    template<class T>
    [[nodiscard]] T& get() {
        auto* value = find<T>();
        if (!value) {
            throw std::logic_error("requested engine service is not registered");
        }
        return *value;
    }

    template<class T>
    [[nodiscard]] const T& get() const {
        const auto* value = find<T>();
        if (!value) {
            throw std::logic_error("requested engine service is not registered");
        }
        return *value;
    }

    template<class T>
    [[nodiscard]] bool contains() const noexcept { return find<T>() != nullptr; }

    [[nodiscard]] std::size_t size() const noexcept { return services_.size(); }

private:
    std::size_t capacity_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> services_;
};

// Queue limits are global as well as per event type. Producers may be worker
// threads; events are copied out before consumers run any callbacks. Standalone
// buses retain events until drained or cleared. Kernel invokes explicit frame
// maintenance, giving its events a transient lifetime (see retire_frame_events).
class EventBus final {
public:
    explicit EventBus(std::size_t capacity = 65536,
                      std::size_t channel_capacity = 8192)
        : capacity_(capacity), channel_capacity_(channel_capacity) {}

    template<class T>
    bool publish(T value) {
        std::scoped_lock lock(mutex_);
        const auto key = std::type_index(typeid(T));
        auto it = queues_.find(key);
        if (count_ >= capacity_ || channel_capacity_ == 0 ||
            (it != queues_.end() && it->second.events.size() >= channel_capacity_)) {
            ++dropped_;
            return false;
        }
        if (it == queues_.end()) {
            it = queues_.emplace(key, Channel{}).first;
        }
        it->second.events.emplace_back(std::move(value));
        ++count_;
        return true;
    }

    template<class T>
    [[nodiscard]] std::vector<T> drain(std::size_t limit = 8192) {
        std::scoped_lock lock(mutex_);
        const auto it = queues_.find(std::type_index(typeid(T)));
        if (it == queues_.end()) {
            return {};
        }
        auto& channel = it->second;
        auto& queue = channel.events;
        const auto size = std::min(limit, queue.size());
        std::vector<T> result;
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            result.push_back(std::any_cast<T>(std::move(queue.front())));
            queue.pop_front();
            if (channel.retained != 0) {
                --channel.retained;
            }
            --count_;
        }
        if (queue.empty()) {
            queues_.erase(it);
        }
        return result;
    }

    template<class T>
    [[nodiscard]] std::size_t pending() const {
        std::scoped_lock lock(mutex_);
        const auto it = queues_.find(std::type_index(typeid(T)));
        return it == queues_.end() ? 0 : it->second.events.size();
    }

    [[nodiscard]] std::size_t pending() const;
    [[nodiscard]] std::uint64_t dropped() const;
    [[nodiscard]] std::uint64_t retired() const;
    void clear();

    // Call once at the end of a frame to opt into transient event retention.
    // Events published since the previous call survive this call and remain
    // available throughout the next frame; older undrained events are retired.
    // Partial drains do not extend that lifetime. Durable work queues must be
    // retained separately. Retirement is counted separately from capacity drops.
    void retire_frame_events();

private:
    struct Channel {
        std::deque<std::any> events;
        // Surviving prefix observed at the previous frame boundary.
        std::size_t retained{};
    };
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::size_t channel_capacity_;
    std::size_t count_{};
    std::uint64_t dropped_{};
    std::uint64_t retired_{};
    std::unordered_map<std::type_index, Channel> queues_;
};

class Configuration final {
public:
    bool set(std::string key, std::string value);
    bool erase(std::string_view key);
    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] std::string text(std::string_view key,
                                   std::string_view fallback = {}) const;
    [[nodiscard]] double number(std::string_view key, double fallback) const;
    [[nodiscard]] std::int64_t integer(std::string_view key,
                                      std::int64_t fallback) const;
    [[nodiscard]] bool boolean(std::string_view key, bool fallback) const;
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>& values() const noexcept;

private:
    std::map<std::string, std::string, std::less<>> values_;
};

enum class Phase : std::uint8_t {
    Input,
    PreSimulation,
    Simulation,
    PostSimulation,
    Animation,
    Visibility,
    Render,
    Audio,
    EndFrame
};

struct Frame {
    std::uint64_t tick{};
    double delta_seconds{};
    double elapsed_seconds{};
};

struct SystemSpec {
    std::string name;
    Phase phase{Phase::Simulation};
    int order{};
    std::vector<std::string> after;
};

struct CommandResult {
    bool success{true};
    std::string message;
};

using CommandArgs = std::span<const std::string>;
using CommandFunction = std::function<CommandResult(Kernel&, CommandArgs)>;
using SystemFunction = std::function<void(Kernel&, const Frame&)>;

struct Diagnostic {
    enum class Severity : std::uint8_t { Info, Warning, Error };
    Severity severity{Severity::Info};
    std::string subsystem;
    std::string message;
    std::uint64_t tick{};
};

struct SystemStatistics {
    std::string name;
    Phase phase{};
    std::uint64_t invocations{};
    std::uint64_t failures{};
    double last_seconds{};
    double total_seconds{};
    bool enabled{true};
};

struct ModuleDescription {
    std::string name;
    std::string description;
    std::uint32_t version{1};
};

// The kernel composes concrete subsystems. It owns no renderer, scene or game
// type; modules add their services and processing functions to the host.
// Host APIs are serialized. Shutdown permanently rejects new command dispatch,
// queued work and module/system/command/shutdown registrations or schedule edits.
// Reports, diagnostics and already-owned service objects remain available.
class Kernel final {
public:
    explicit Kernel(std::size_t service_capacity = 512);
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    Kernel(Kernel&&) = delete;
    Kernel& operator=(Kernel&&) = delete;

    [[nodiscard]] ServiceStore& services() noexcept { return services_; }
    [[nodiscard]] const ServiceStore& services() const noexcept { return services_; }
    [[nodiscard]] EventBus& events() noexcept { return events_; }
    [[nodiscard]] const EventBus& events() const noexcept { return events_; }
    [[nodiscard]] Configuration& config() noexcept { return configuration_; }
    [[nodiscard]] const Configuration& config() const noexcept { return configuration_; }
    [[nodiscard]] const Frame& frame() const noexcept { return frame_; }

    bool register_module(ModuleDescription module);
    [[nodiscard]] const std::vector<ModuleDescription>& modules() const noexcept;
    bool add_system(SystemSpec spec, SystemFunction function);
    bool order_after(std::string_view system, std::string_view prerequisite);
    bool set_system_enabled(std::string_view name, bool enabled);
    [[nodiscard]] std::vector<SystemStatistics> system_statistics() const;
    [[nodiscard]] std::vector<std::string> execution_order();
    bool add_command(std::string name, CommandFunction function,
                     std::string help = {});
    [[nodiscard]] CommandResult command(std::string_view name, CommandArgs args = {});
    [[nodiscard]] CommandResult execute(std::string_view line);
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> commands() const;
    bool enqueue_command(std::string line);
    // Execute a bounded snapshot in FIFO order. Recursive drains do no work;
    // commands enqueued by callbacks remain queued for a later drain. Shutdown
    // discards unexecuted commands; results cover only commands actually invoked.
    [[nodiscard]] std::vector<CommandResult> drain_commands(std::size_t limit = 256);

    // A tick is synchronous. Individual registered services may own workers.
    // Recursive tick entry, including from queued commands, is rejected. Queued
    // commands may change systems before the current frame's schedule is checked.
    // Command failures are diagnosed and make the tick return false, while the
    // remaining commands and frame systems still run if admission remains valid.
    // A failed system is diagnosed and disabled until explicitly re-enabled.
    // Undrained events expire after a complete following frame (see EventBus).
    // Shutdown aborts remaining systems and returns false. An already-started
    // frame retains its tick/time but does not perform frame event retirement.
    bool tick(double delta_seconds);
    // Stop admits no further ticks but lets the current frame finish. Commands
    // and registrations remain available until shutdown is requested.
    void request_stop() noexcept { stop_requested_ = true; }
    [[nodiscard]] bool stop_requested() const noexcept { return stop_requested_; }
    bool add_shutdown(std::string name, std::function<void()> function);
    // Shutdown becomes terminal immediately. Reverse-order callbacks run once,
    // after active ticks/drains/commands unwind so their callbacks retain usable
    // services until they return. Idle shutdown completes before this call
    // returns; recursive shutdown is a no-op. Failures are counted even if
    // diagnostic allocation fails, and remaining shutdown callbacks still run.
    void shutdown() noexcept;
    [[nodiscard]] std::size_t shutdown_failures() const noexcept { return shutdown_failures_; }

    void metric(std::string name, double value);
    [[nodiscard]] double metric_value(std::string_view name, double fallback = 0.0) const;
    [[nodiscard]] const std::map<std::string, double, std::less<>>& metrics() const noexcept;
    void diagnose(Diagnostic::Severity severity, std::string subsystem,
                  std::string message);
    [[nodiscard]] const std::deque<Diagnostic>& diagnostics() const noexcept;
    [[nodiscard]] std::string report_json() const;

private:
    struct SystemEntry {
        SystemSpec spec;
        SystemFunction function;
        SystemStatistics statistics;
    };
    struct CommandEntry {
        CommandFunction function;
        std::string help;
    };
    bool rebuild_schedule();
    ServiceStore services_;
    EventBus events_;
    Configuration configuration_;
    Frame frame_;
    std::vector<ModuleDescription> modules_;
    std::vector<SystemEntry> systems_;
    std::vector<std::size_t> schedule_;
    std::map<std::string, CommandEntry, std::less<>> commands_;
    std::deque<std::string> queued_commands_;
    std::vector<std::pair<std::string, std::function<void()>>> shutdown_;
    std::map<std::string, double, std::less<>> metrics_;
    std::deque<Diagnostic> diagnostics_;
    std::size_t shutdown_failures_{};
    std::size_t command_depth_{};
    bool schedule_dirty_{true};
    bool tick_active_{};
    bool ticking_{};
    bool draining_commands_{};
    bool stop_requested_{};
    bool shutdown_requested_{};
    bool shutting_down_{};
    bool shutdown_complete_{};
};

[[nodiscard]] std::vector<std::string> tokenize_command(std::string_view line);
[[nodiscard]] std::string json_escape(std::string_view text);

} // namespace ge::core
