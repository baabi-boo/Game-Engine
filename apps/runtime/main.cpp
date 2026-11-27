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

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        auto value = [&]() -> std::string_view {
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
            options.script = value();
        } else if (arg == "--report") {
            options.report = value();
        } else if (arg == "--ticks") {
            const auto text = value();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.ticks);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || options.ticks > 10000000) {
                throw std::invalid_argument("ticks must be an integer between zero and 10000000");
            }
        } else if (arg == "--step") {
            const auto text = value();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.step);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                !std::isfinite(options.step) || options.step <= 0.0 || options.step > 1.0) {
                throw std::invalid_argument("step must be finite and in (0,1] seconds");
            }
        } else if (arg == "--set") {
            const auto text = value();
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
        if (line == "tick") {
            success = kernel.tick(kernel.config().number("runtime.step", 1.0 / 60.0)) && success;
            continue;
        }
        const auto result = kernel.execute(line);
        if (!result.message.empty()) {
            (result.success ? std::cout : std::cerr) << result.message << '\n';
        }
        success = result.success && success;
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

int main(int argc, char** argv) {
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
        install_core_services(kernel);
        ge::application::install_modules(kernel);
        if (options.list_commands) {
            for (const auto& [name, help] : kernel.commands()) {
                std::cout << name << "\t" << help << '\n';
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
            success = kernel.tick(kernel.config().number("runtime.step", options.step)) && success;
        }
        if (options.interactive) {
            success = consume_commands(kernel, std::cin, true) && success;
        }
        const auto report = kernel.report_json();
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
        kernel.shutdown();
        return success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "ge_runtime: " << error.what() << '\n';
        return 2;
    }
}
