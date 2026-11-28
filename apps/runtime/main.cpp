#include "modules.hpp"

#include <ge/core/binary.hpp>
#include <ge/core/memory.hpp>
#include <ge/core/runtime.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using CommandCharacter = std::filesystem::path::value_type;
using CommandArgument = std::basic_string_view<CommandCharacter>;

std::string argument_text(CommandArgument argument) {
#ifdef _WIN32
    // Native paths stay UTF-16; only option text is converted to UTF-8.
    const auto utf8 = std::filesystem::path{argument}.u8string();
    return std::string{utf8.begin(), utf8.end()};
#else
    return std::string{argument};
#endif
}

struct Options {
    std::uint64_t ticks{1};
    double step{1.0 / 60.0};
    std::filesystem::path script;
    std::filesystem::path report;
    std::vector<std::pair<std::string, std::string>> settings;
    bool interactive{};
    bool help{};
    bool list_commands{};
};

double parse_step(std::string_view text) {
    double step{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), step);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(step) || step <= 0.0 || step > 1.0) {
        throw std::invalid_argument("runtime.step must be finite and in (0,1] seconds");
    }
    return step;
}

std::string format_step(double step) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), step,
                                      std::chars_format::general,
                                      std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("cannot encode runtime timestep");
    }
    return std::string(buffer.data(), result.ptr);
}

Options parse_options(int argc, CommandCharacter* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const auto arg = argument_text(argv[i]);
        auto value = [&]() -> CommandArgument {
            if (++i >= argc) {
                throw std::invalid_argument("missing value for " + std::string(arg));
            }
            return argv[i];
        };
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--interactive") {
            options.interactive = true;
        } else if (arg == "--commands") {
            options.list_commands = true;
        } else if (arg == "--script") {
            options.script = std::filesystem::path{value()};
        } else if (arg == "--report") {
            options.report = std::filesystem::path{value()};
        } else if (arg == "--ticks") {
            const auto text = argument_text(value());
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.ticks);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || options.ticks > 10000000) {
                throw std::invalid_argument("ticks must be an integer between zero and 10000000");
            }
        } else if (arg == "--step") {
            options.step = parse_step(argument_text(value()));
        } else if (arg == "--set") {
            const auto text = argument_text(value());
            const auto separator = text.find('=');
            if (separator == std::string_view::npos || separator == 0 || options.settings.size() >= 256) {
                throw std::invalid_argument("settings require key=value; maximum 256 overrides");
            }
            options.settings.emplace_back(text.substr(0, separator), text.substr(separator + 1));
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
    }
    return options;
}

bool read_command_line(std::istream& input, std::string& line) {
    line.clear();
    char character{};
    while (input.get(character)) {
        if (character == '\n') {
            return true;
        }
        if (line.size() >= 65536) {
            throw std::length_error("command line exceeds 65536 bytes");
        }
        line += character;
    }
    if (input.bad()) {
        throw std::runtime_error("command stream read failure");
    }
    return !line.empty();
}

ge::core::CommandResult advance_frame(ge::core::Kernel& kernel) {
    try {
        const bool success = kernel.tick(parse_step(kernel.config().text("runtime.step")));
        return {success, success ? "" : "runtime tick failed"};
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}

ge::core::CommandResult execute_command(ge::core::Kernel& kernel, std::string_view line) {
    try {
        const auto args = ge::core::tokenize_command(line);
        if (args.empty()) {
            return {true, {}};
        }
        if (args.front() == "tick") {
            if (args.size() != 1) {
                return {false, "usage: tick"};
            }
            return advance_frame(kernel);
        }
        if (args.front() == "engine.set" && args.size() == 3 && args[1] == "runtime.step") {
            // Reject invalid updates before replacing the last valid timestep.
            (void)parse_step(args[2]);
        }
        return kernel.command(args.front(), ge::core::CommandArgs(args).subspan(1));
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}

bool record_result(ge::core::Kernel& kernel, const ge::core::CommandResult& result) {
    if (result.success) {
        if (!result.message.empty()) {
            std::cout << result.message << '\n';
        }
        return true;
    }
    const auto message = result.message.empty() ? "runtime command failed" : result.message;
    std::cerr << message << '\n';
    const auto& diagnostics = kernel.diagnostics();
    if (diagnostics.empty() || diagnostics.back().severity != ge::core::Diagnostic::Severity::Error ||
        diagnostics.back().tick != kernel.frame().tick || diagnostics.back().message != message) {
        kernel.diagnose(ge::core::Diagnostic::Severity::Error, "runtime.command", message);
    }
    return false;
}

bool consume_commands(ge::core::Kernel& kernel, std::istream& input, bool interactive) {
    bool success = true;
    std::string line;
    std::size_t lines{};
    while (!kernel.stop_requested()) {
        if (interactive) {
            std::cout << "ge> " << std::flush;
        }
        if (!read_command_line(input, line)) {
            break;
        }
        if (++lines > 1000000) {
            throw std::length_error("command stream exceeds one million lines");
        }
        success = record_result(kernel, execute_command(kernel, line)) && success;
    }
    return success;
}

void install_core_services(ge::core::Kernel& kernel) {
    kernel.services().emplace<ge::core::LinearArena>(8U * 1024U * 1024U);
    kernel.services().emplace<ge::core::BudgetResource>(256U * 1024U * 1024U);
    kernel.add_command("content.hash", [](ge::core::Kernel&, ge::core::CommandArgs args) {
        if (args.size() != 1) {
            return ge::core::CommandResult{false, "usage: content.hash text"};
        }
        return ge::core::CommandResult{true, std::to_string(ge::core::fnv1a64(args.front()))};
    }, "Compute a stable non-cryptographic content key");
    kernel.add_system({"core.scratch", ge::core::Phase::EndFrame, 10000, {}},
        [](ge::core::Kernel& host, const ge::core::Frame&) {
            auto& arena = host.services().get<ge::core::LinearArena>();
            arena.reset();
        });
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    try {
        const auto options = parse_options(argc, argv);
        if (options.help) {
            std::cout << "ge_runtime [--ticks N] [--step seconds] [--script path]\n"
                         "           [--set key=value] [--interactive] [--commands] [--report path]\n"
                         "Commands operate on installed engine services. Type tick in a script\n"
                         "or console to advance one simulation frame.\n";
            return 0;
        }
        ge::core::Kernel kernel;
        (void)kernel.config().set("runtime.step", format_step(options.step));
        for (const auto& [key, value] : options.settings) {
            if (!kernel.config().set(key, value)) {
                throw std::invalid_argument("configuration bounds exceeded");
            }
        }
        (void)parse_step(kernel.config().text("runtime.step"));
        install_core_services(kernel);
        ge::application::install_modules(kernel);
        if (options.list_commands) {
            for (const auto& [name, help] : kernel.commands()) {
                std::cout << name << "\t" << help << '\n';
            }
            std::cout << "tick\tAdvance one simulation frame\n";
            kernel.shutdown();
            if (kernel.shutdown_failures() != 0) {
                std::cerr << "runtime shutdown failed: " << kernel.shutdown_failures()
                          << " callback(s) failed\n";
                return 1;
            }
            return 0;
        }
        bool success = true;
        if (!options.script.empty()) {
            std::ifstream input(options.script);
            if (!input) {
                throw std::runtime_error("cannot open command script");
            }
            success = consume_commands(kernel, input, false);
        }
        for (std::uint64_t i = 0; i < options.ticks && !kernel.stop_requested(); ++i) {
            if (!record_result(kernel, advance_frame(kernel))) {
                success = false;
                break;
            }
        }
        if (options.interactive) {
            success = consume_commands(kernel, std::cin, true) && success;
        }
        // Cleanup failures belong to this execution's report and exit status.
        // Services remain alive until Kernel destruction, so reporting is safe.
        kernel.shutdown();
        success = kernel.shutdown_failures() == 0 && success;
        for (const auto& diagnostic : kernel.diagnostics()) {
            if (diagnostic.severity == ge::core::Diagnostic::Severity::Error) {
                success = false;
            }
        }
        auto report = kernel.report_json();
        report.insert(1, success ? "\"success\":true," : "\"success\":false,");
        if (options.report.empty()) {
            std::cout << report << '\n';
        } else {
            std::ofstream output(options.report, std::ios::binary | std::ios::trunc);
            output << report << '\n';
            output.close();
            if (!output) {
                throw std::runtime_error("cannot write runtime report");
            }
        }
        std::cout.flush();
        if (!std::cout) {
            throw std::runtime_error("cannot write runtime output");
        }
        return success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "ge_runtime: " << error.what() << '\n';
        return 2;
    }
}
