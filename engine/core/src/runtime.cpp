#include <ge/core/runtime.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ge::core {

std::size_t EventBus::pending() const {
    std::scoped_lock lock(mutex_);
    return count_;
}

std::uint64_t EventBus::dropped() const {
    std::scoped_lock lock(mutex_);
    return dropped_;
}

void EventBus::clear() {
    std::scoped_lock lock(mutex_);
    queues_.clear();
    count_ = 0;
}

bool Configuration::set(std::string key, std::string value) {
    if (key.empty() || key.size() > 256 || value.size() > 65536) {
        return false;
    }
    if (values_.size() >= 4096 && !values_.contains(key)) {
        return false;
    }
    values_.insert_or_assign(std::move(key), std::move(value));
    return true;
}

bool Configuration::erase(std::string_view key) {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return false;
    }
    values_.erase(it);
    return true;
}

bool Configuration::contains(std::string_view key) const {
    return values_.find(key) != values_.end();
}

std::string Configuration::text(std::string_view key, std::string_view fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? std::string(fallback) : it->second;
}

double Configuration::number(std::string_view key, double fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    double result{};
    const auto* first = it->second.data();
    const auto* last = first + it->second.size();
    const auto parsed = std::from_chars(first, last, result);
    return parsed.ec == std::errc{} && parsed.ptr == last && std::isfinite(result)
        ? result : fallback;
}

std::int64_t Configuration::integer(std::string_view key, std::int64_t fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    std::int64_t result{};
    const auto* first = it->second.data();
    const auto* last = first + it->second.size();
    const auto parsed = std::from_chars(first, last, result);
    return parsed.ec == std::errc{} && parsed.ptr == last ? result : fallback;
}

bool Configuration::boolean(std::string_view key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    if (it->second == "true" || it->second == "1" || it->second == "on") {
        return true;
    }
    if (it->second == "false" || it->second == "0" || it->second == "off") {
        return false;
    }
    return fallback;
}

const std::map<std::string, std::string, std::less<>>& Configuration::values() const noexcept {
    return values_;
}

Kernel::Kernel(std::size_t service_capacity) : services_(service_capacity) {
    add_command("engine.stop", [](Kernel& kernel, CommandArgs) {
        kernel.request_stop();
        return CommandResult{true, "stop requested"};
    }, "Stop the runtime after the current frame");
    add_command("engine.set", [](Kernel& kernel, CommandArgs args) {
        if (args.size() != 2) {
            return CommandResult{false, "usage: engine.set key value"};
        }
        const bool accepted = kernel.config().set(args[0], args[1]);
        return CommandResult{accepted, accepted ? "updated" : "configuration limit exceeded"};
    }, "Set a runtime configuration value");
    add_command("engine.system", [](Kernel& kernel, CommandArgs args) {
        if (args.size() != 2 || (args[1] != "on" && args[1] != "off")) {
            return CommandResult{false, "usage: engine.system name on|off"};
        }
        const bool accepted = kernel.set_system_enabled(args[0], args[1] == "on");
        return CommandResult{accepted, accepted ? "updated" : "unknown system"};
    }, "Enable or disable a registered system");
    add_command("engine.report", [](Kernel& kernel, CommandArgs) {
        return CommandResult{true, kernel.report_json()};
    }, "Report runtime modules, metrics and system timing");
}

Kernel::~Kernel() { shutdown(); }

bool Kernel::register_module(ModuleDescription module) {
    if (ticking_ || module.name.empty() || module.name.size() > 128 ||
        modules_.size() >= 512 || module.version == 0) {
        return false;
    }
    const auto duplicate = std::find_if(modules_.begin(), modules_.end(),
        [&](const auto& item) { return item.name == module.name; });
    if (duplicate != modules_.end()) {
        return false;
    }
    modules_.push_back(std::move(module));
    return true;
}

const std::vector<ModuleDescription>& Kernel::modules() const noexcept { return modules_; }

bool Kernel::add_system(SystemSpec spec, SystemFunction function) {
    if (ticking_ || shutdown_complete_ || !function || spec.name.empty() ||
        spec.name.size() > 128 || systems_.size() >= 2048 || spec.after.size() > 256) {
        return false;
    }
    for (const auto& system : systems_) {
        if (system.spec.name == spec.name) {
            return false;
        }
    }
    SystemEntry entry;
    entry.statistics.name = spec.name;
    entry.statistics.phase = spec.phase;
    entry.spec = std::move(spec);
    entry.function = std::move(function);
    systems_.push_back(std::move(entry));
    schedule_dirty_ = true;
    return true;
}

bool Kernel::set_system_enabled(std::string_view name, bool enabled) {
    for (auto& system : systems_) {
        if (system.spec.name == name) {
            system.statistics.enabled = enabled;
            return true;
        }
    }
    return false;
}

std::vector<SystemStatistics> Kernel::system_statistics() const {
    std::vector<SystemStatistics> result;
    result.reserve(systems_.size());
    for (const auto& system : systems_) {
        result.push_back(system.statistics);
    }
    return result;
}

bool Kernel::rebuild_schedule() {
    if (!schedule_dirty_) {
        return true;
    }
    const auto count = systems_.size();
    std::vector<std::vector<std::size_t>> outgoing(count);
    std::vector<std::size_t> incoming(count, 0);
    std::map<std::string, std::size_t, std::less<>> indices;
    for (std::size_t i = 0; i < count; ++i) {
        indices.emplace(systems_[i].spec.name, i);
    }
    for (std::size_t i = 0; i < count; ++i) {
        for (const auto& dependency : systems_[i].spec.after) {
            const auto it = indices.find(dependency);
            if (it == indices.end() || it->second == i ||
                systems_[it->second].spec.phase > systems_[i].spec.phase) {
                diagnose(Diagnostic::Severity::Error, "kernel",
                         "invalid system dependency: " + systems_[i].spec.name + " after " + dependency);
                return false;
            }
            outgoing[it->second].push_back(i);
            ++incoming[i];
        }
    }
    auto earlier = [&](std::size_t a, std::size_t b) {
        const auto& left = systems_[a].spec;
        const auto& right = systems_[b].spec;
        if (left.phase != right.phase) {
            return left.phase < right.phase;
        }
        if (left.order != right.order) {
            return left.order < right.order;
        }
        return left.name < right.name;
    };
    std::vector<std::size_t> ready;
    std::vector<std::size_t> result;
    ready.reserve(count);
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (incoming[i] == 0) {
            ready.push_back(i);
        }
    }
    while (!ready.empty()) {
        const auto next = std::min_element(ready.begin(), ready.end(), earlier);
        const auto index = *next;
        ready.erase(next);
        result.push_back(index);
        for (const auto child : outgoing[index]) {
            if (--incoming[child] == 0) {
                ready.push_back(child);
            }
        }
    }
    if (result.size() != count) {
        diagnose(Diagnostic::Severity::Error, "kernel", "system dependency cycle");
        return false;
    }
    schedule_ = std::move(result);
    schedule_dirty_ = false;
    return true;
}

std::vector<std::string> Kernel::execution_order() {
    if (!rebuild_schedule()) {
        return {};
    }
    std::vector<std::string> result;
    result.reserve(schedule_.size());
    for (const auto index : schedule_) {
        result.push_back(systems_[index].spec.name);
    }
    return result;
}

bool Kernel::add_command(std::string name, CommandFunction function, std::string help) {
    if (ticking_ || name.empty() || name.size() > 128 || !function || commands_.size() >= 8192) {
        return false;
    }
    return commands_.emplace(std::move(name), CommandEntry{std::move(function), std::move(help)}).second;
}

CommandResult Kernel::command(std::string_view name, CommandArgs args) {
    const auto it = commands_.find(name);
    if (it == commands_.end()) {
        return {false, "unknown command: " + std::string(name)};
    }
    try {
        return it->second.function(*this, args);
    } catch (const std::exception& error) {
        diagnose(Diagnostic::Severity::Error, std::string(name), error.what());
        return {false, error.what()};
    } catch (...) {
        diagnose(Diagnostic::Severity::Error, std::string(name), "non-standard command exception");
        return {false, "non-standard command exception"};
    }
}

CommandResult Kernel::execute(std::string_view line) {
    try {
        auto args = tokenize_command(line);
        if (args.empty()) {
            return {true, {}};
        }
        return command(args.front(), CommandArgs(args).subspan(1));
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}

std::vector<std::pair<std::string, std::string>> Kernel::commands() const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(commands_.size());
    for (const auto& [name, entry] : commands_) {
        result.emplace_back(name, entry.help);
    }
    return result;
}

bool Kernel::enqueue_command(std::string line) {
    if (line.size() > 65536 || queued_commands_.size() >= 4096) {
        return false;
    }
    queued_commands_.push_back(std::move(line));
    return true;
}

std::vector<CommandResult> Kernel::drain_commands(std::size_t limit) {
    std::vector<CommandResult> results;
    const auto count = std::min(limit, queued_commands_.size());
    results.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto line = std::move(queued_commands_.front());
        queued_commands_.pop_front();
        results.push_back(execute(line));
    }
    return results;
}

bool Kernel::tick(double delta_seconds) {
    if (ticking_ || shutdown_complete_ || stop_requested_ ||
        !std::isfinite(delta_seconds) || delta_seconds < 0.0 || delta_seconds > 1.0 ||
        frame_.tick == std::numeric_limits<std::uint64_t>::max() || !rebuild_schedule()) {
        return false;
    }
    (void)drain_commands();
    if (stop_requested_) {
        return false;
    }
    frame_.delta_seconds = delta_seconds;
    frame_.elapsed_seconds += delta_seconds;
    ++frame_.tick;
    ticking_ = true;
    bool success = true;
    for (const auto index : schedule_) {
        auto& system = systems_[index];
        if (!system.statistics.enabled) {
            continue;
        }
        const auto begin = std::chrono::steady_clock::now();
        try {
            system.function(*this, frame_);
            ++system.statistics.invocations;
        } catch (const std::exception& error) {
            ++system.statistics.failures;
            system.statistics.enabled = false;
            diagnose(Diagnostic::Severity::Error, system.spec.name, error.what());
            success = false;
        } catch (...) {
            ++system.statistics.failures;
            system.statistics.enabled = false;
            diagnose(Diagnostic::Severity::Error, system.spec.name, "non-standard system exception");
            success = false;
        }
        system.statistics.last_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        system.statistics.total_seconds += system.statistics.last_seconds;
    }
    ticking_ = false;
    metric("kernel.events.pending", static_cast<double>(events_.pending()));
    metric("kernel.events.dropped", static_cast<double>(events_.dropped()));
    metric("kernel.services", static_cast<double>(services_.size()));
    return success;
}

bool Kernel::add_shutdown(std::string name, std::function<void()> function) {
    if (ticking_ || shutdown_complete_ || name.empty() || !function || shutdown_.size() >= 512) {
        return false;
    }
    for (const auto& item : shutdown_) {
        if (item.first == name) {
            return false;
        }
    }
    shutdown_.emplace_back(std::move(name), std::move(function));
    return true;
}

void Kernel::shutdown() noexcept {
    if (shutdown_complete_) {
        return;
    }
    stop_requested_ = true;
    for (auto it = shutdown_.rbegin(); it != shutdown_.rend(); ++it) {
        try {
            it->second();
        } catch (...) {
            // Destructors must still release the remaining services.
        }
    }
    shutdown_.clear();
    shutdown_complete_ = true;
}

void Kernel::metric(std::string name, double value) {
    if (name.size() <= 256 && std::isfinite(value) &&
        (metrics_.size() < 16384 || metrics_.contains(name))) {
        metrics_.insert_or_assign(std::move(name), value);
    }
}

double Kernel::metric_value(std::string_view name, double fallback) const {
    const auto it = metrics_.find(name);
    return it == metrics_.end() ? fallback : it->second;
}

const std::map<std::string, double, std::less<>>& Kernel::metrics() const noexcept { return metrics_; }

void Kernel::diagnose(Diagnostic::Severity severity, std::string subsystem, std::string message) {
    if (diagnostics_.size() >= 1024) {
        diagnostics_.pop_front();
    }
    diagnostics_.push_back({severity, std::move(subsystem), std::move(message), frame_.tick});
}

const std::deque<Diagnostic>& Kernel::diagnostics() const noexcept { return diagnostics_; }

std::string json_escape(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            result += '\\';
            result += character;
        } else if (byte < 0x20) {
            result += "\\u00";
            result += hex[byte >> 4U];
            result += hex[byte & 15U];
        } else {
            result += character;
        }
    }
    return result;
}

std::string Kernel::report_json() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12) << "{\"tick\":" << frame_.tick
        << ",\"elapsed_seconds\":" << frame_.elapsed_seconds << ",\"modules\":[";
    bool first = true;
    for (const auto& module : modules_) {
        if (!first) { out << ','; }
        first = false;
        out << "{\"name\":\"" << json_escape(module.name) << "\",\"version\":"
            << module.version << ",\"description\":\"" << json_escape(module.description) << "\"}";
    }
    out << "],\"metrics\":{";
    first = true;
    for (const auto& [name, value] : metrics_) {
        if (!first) { out << ','; }
        first = false;
        out << '"' << json_escape(name) << "\":" << value;
    }
    out << "},\"systems\":[";
    first = true;
    for (const auto& system : systems_) {
        if (!first) { out << ','; }
        first = false;
        out << "{\"name\":\"" << json_escape(system.spec.name)
            << "\",\"calls\":" << system.statistics.invocations
            << ",\"failures\":" << system.statistics.failures
            << ",\"last_seconds\":" << system.statistics.last_seconds
            << ",\"enabled\":" << (system.statistics.enabled ? "true" : "false") << '}';
    }
    out << "],\"diagnostics\":[";
    first = true;
    for (const auto& diagnostic : diagnostics_) {
        if (!first) { out << ','; }
        first = false;
        out << "{\"subsystem\":\"" << json_escape(diagnostic.subsystem)
            << "\",\"message\":\"" << json_escape(diagnostic.message)
            << "\",\"tick\":" << diagnostic.tick << '}';
    }
    out << "]}";
    return out.str();
}

std::vector<std::string> tokenize_command(std::string_view line) {
    if (line.size() > 65536) {
        throw std::length_error("command exceeds 65536 bytes");
    }
    std::vector<std::string> result;
    std::string token;
    char quote{};
    bool escaped{};
    bool started{};
    auto finish = [&] {
        if (started) {
            if (result.size() >= 256) {
                throw std::length_error("command has too many arguments");
            }
            result.push_back(std::move(token));
            token.clear();
            started = false;
        }
    };
    for (const char character : line) {
        if (escaped) {
            token += character;
            escaped = false;
            started = true;
        } else if (character == '\\') {
            escaped = true;
            started = true;
        } else if (quote != 0) {
            if (character == quote) { quote = 0; }
            else { token += character; }
        } else if (character == '"' || character == '\'') {
            quote = character;
            started = true;
        } else if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            finish();
        } else if (character == '#' && !started) {
            break;
        } else {
            token += character;
            started = true;
        }
    }
    if (escaped || quote != 0) {
        throw std::invalid_argument("unterminated command quote or escape");
    }
    finish();
    return result;
}

} // namespace ge::core
