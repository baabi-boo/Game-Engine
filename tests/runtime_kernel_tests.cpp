#include "test_support.hpp"

#include <ge/core/runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ge::core::CommandArgs;
using ge::core::CommandResult;
using ge::core::EventBus;
using ge::core::Frame;
using ge::core::Kernel;
using ge::core::Phase;

void shutdown_is_reentrant_and_idempotent() {
    std::vector<int> calls;
    unsigned recursive_calls{};
    Kernel kernel;
    CHECK(kernel.add_shutdown("first", [&] { calls.push_back(1); }));
    CHECK(kernel.add_shutdown("throws", [&] {
        calls.push_back(2);
        throw std::runtime_error("shutdown failure");
    }));
    CHECK(kernel.add_shutdown("recursive", [&] {
        calls.push_back(3);
        // Bound a defective implementation's recursion so the regression need
        // not rely on an eventual stack overflow to reject duplicate callbacks.
        if (++recursive_calls == 1) {
            kernel.shutdown();
        }
    }));
    kernel.shutdown();
    CHECK(calls == std::vector<int>({3, 2, 1}));
    CHECK(recursive_calls == 1);
    CHECK(kernel.stop_requested());
    CHECK(!kernel.tick(0.01));
    kernel.shutdown();
    CHECK(calls == std::vector<int>({3, 2, 1}));
}

void shutdown_rejects_registration_and_runs_all_handlers() {
    std::vector<int> calls;
    bool accepted = true;
    bool nested_tick = true;
    {
        Kernel kernel;
        CHECK(kernel.add_shutdown("first", [&] { calls.push_back(1); }));
        CHECK(kernel.add_shutdown("register", [&] {
            accepted = kernel.add_shutdown("late", [&] { calls.push_back(4); });
            nested_tick = kernel.tick(0.01);
            calls.push_back(2);
            throw 7;
        }));
        CHECK(kernel.add_shutdown("last", [&] { calls.push_back(3); }));
        kernel.shutdown();
        CHECK(!accepted);
        CHECK(!nested_tick);
        CHECK(calls == std::vector<int>({3, 2, 1}));
        CHECK(!kernel.add_shutdown("after", [] {}));
    }
    CHECK(calls == std::vector<int>({3, 2, 1}));
}

void command_drains_use_a_snapshot_and_reject_recursion() {
    std::vector<std::string> calls;
    std::size_t recursive_results = 1;
    bool queued_late{};
    Kernel kernel;
    CHECK(kernel.add_command("nested", [&](Kernel& host, CommandArgs) {
        calls.emplace_back("nested");
        queued_late = host.enqueue_command("late");
        recursive_results = host.drain_commands().size();
        return CommandResult{};
    }));
    for (const std::string name : {"tail", "deferred", "late"}) {
        CHECK(kernel.add_command(name, [&, name](Kernel&, CommandArgs) {
            calls.push_back(name);
            return CommandResult{};
        }));
    }
    CHECK(kernel.enqueue_command("nested"));
    CHECK(kernel.enqueue_command("tail"));
    CHECK(kernel.enqueue_command("deferred"));
    CHECK(kernel.drain_commands(0).empty());
    const auto first = kernel.drain_commands(2);
    CHECK(first.size() == 2);
    CHECK(first[0].success && first[1].success);
    CHECK(queued_late);
    CHECK(recursive_results == 0);
    CHECK(calls == std::vector<std::string>({"nested", "tail"}));
    const auto second = kernel.drain_commands();
    CHECK(second.size() == 2);
    CHECK(second[0].success && second[1].success);
    CHECK(calls == std::vector<std::string>({"nested", "tail", "deferred", "late"}));
    CHECK(kernel.drain_commands().empty());
}

void commands_and_systems_cannot_reenter_ticks() {
    unsigned command_calls{};
    unsigned system_calls{};
    bool command_tick = true;
    bool system_tick = true;
    Kernel kernel;
    CHECK(kernel.add_command("tick", [&](Kernel& host, CommandArgs) {
        ++command_calls;
        command_tick = host.tick(0.01);
        return CommandResult{};
    }));
    CHECK(kernel.add_system({"simulation", Phase::Simulation, 0, {}},
        [&](Kernel& host, const Frame&) {
            ++system_calls;
            system_tick = host.tick(0.01);
        }));
    CHECK(kernel.enqueue_command("tick"));
    CHECK(kernel.tick(0.01));
    CHECK(!command_tick);
    CHECK(!system_tick);
    CHECK(command_calls == 1);
    CHECK(system_calls == 1);
    CHECK(kernel.frame().tick == 1);
    CHECK(kernel.enqueue_command("tick"));
    const auto drained = kernel.drain_commands();
    CHECK(drained.size() == 1 && drained.front().success);
    CHECK(!command_tick);
    CHECK(command_calls == 2);
    CHECK(kernel.frame().tick == 1);
    CHECK(kernel.tick(0.01));
    CHECK(system_calls == 2);
}

void command_installed_systems_run_in_the_current_frame() {
    std::vector<std::string> calls;
    bool installed{};
    Kernel kernel;
    CHECK(kernel.add_system({"first", Phase::Simulation, 0, {}},
        [&](Kernel&, const Frame&) { calls.emplace_back("first"); }));
    CHECK(kernel.execution_order() == std::vector<std::string>({"first"}));
    CHECK(kernel.add_command("install", [&](Kernel& host, CommandArgs) {
        installed = host.add_system({"new", Phase::Simulation, 0, {"first"}},
            [&](Kernel&, const Frame&) { calls.emplace_back("new"); });
        return CommandResult{installed, {}};
    }));
    CHECK(kernel.enqueue_command("install"));
    CHECK(kernel.tick(0.01));
    CHECK(installed);
    CHECK(calls == std::vector<std::string>({"first", "new"}));
}

void queued_command_failures_are_reported_without_skipping_the_frame() {
    unsigned calls{};
    bool subsequent_command{};
    Kernel kernel;
    CHECK(kernel.add_command("reject", [](Kernel&, CommandArgs) {
        return CommandResult{false, "deliberate rejection"};
    }));
    CHECK(kernel.add_command("continue", [&](Kernel&, CommandArgs) {
        subsequent_command = true;
        return CommandResult{};
    }));
    CHECK(kernel.add_system({"simulation", Phase::Simulation, 0, {}},
        [&](Kernel&, const Frame&) { ++calls; }));
    CHECK(kernel.enqueue_command("reject"));
    CHECK(kernel.enqueue_command("continue"));
    CHECK(!kernel.tick(0.01));
    CHECK(subsequent_command);
    CHECK(calls == 1);
    CHECK(kernel.frame().tick == 1);
    CHECK(!kernel.diagnostics().empty());
    CHECK(kernel.diagnostics().back().severity == ge::core::Diagnostic::Severity::Error);
    CHECK(kernel.diagnostics().back().subsystem == "kernel");
    CHECK(kernel.diagnostics().back().message == "queued command failed: deliberate rejection");
    CHECK(kernel.tick(0.01));
    CHECK(calls == 2);
}

void command_order_changes_are_validated_before_system_execution() {
    std::vector<std::string> calls;
    Kernel kernel;
    for (const std::string name : {"a", "b"}) {
        CHECK(kernel.add_system({name, Phase::Simulation, 0, {}},
            [&, name](Kernel&, const Frame&) { calls.push_back(name); }));
    }
    CHECK(kernel.execution_order() == std::vector<std::string>({"a", "b"}));
    CHECK(kernel.add_command("reorder", [](Kernel& host, CommandArgs) {
        return CommandResult{host.order_after("a", "b"), {}};
    }));
    CHECK(kernel.add_command("cycle", [](Kernel& host, CommandArgs) {
        return CommandResult{host.order_after("b", "a"), {}};
    }));
    CHECK(kernel.enqueue_command("reorder"));
    CHECK(kernel.tick(0.01));
    CHECK(calls == std::vector<std::string>({"b", "a"}));
    CHECK(kernel.enqueue_command("cycle"));
    CHECK(!kernel.tick(0.01));
    CHECK(kernel.frame().tick == 1);
    CHECK(calls == std::vector<std::string>({"b", "a"}));
    CHECK(!kernel.diagnostics().empty());
    CHECK(kernel.diagnostics().back().message == "system dependency cycle");
}

void command_installed_invalid_dependencies_prevent_the_frame() {
    unsigned calls{};
    bool installed{};
    Kernel kernel;
    CHECK(kernel.add_system({"valid", Phase::Simulation, 0, {}},
        [&](Kernel&, const Frame&) { ++calls; }));
    CHECK(kernel.execution_order() == std::vector<std::string>({"valid"}));
    CHECK(kernel.add_command("invalid", [&](Kernel& host, CommandArgs) {
        installed = host.add_system({"invalid", Phase::Simulation, 0, {"missing"}},
            [&](Kernel&, const Frame&) { ++calls; });
        return CommandResult{installed, {}};
    }));
    CHECK(kernel.enqueue_command("invalid"));
    CHECK(!kernel.tick(0.01));
    CHECK(installed);
    CHECK(calls == 0);
    CHECK(kernel.frame().tick == 0);
    CHECK(!kernel.diagnostics().empty());
    CHECK(kernel.diagnostics().back().message ==
          "invalid system dependency: invalid after missing");
    CHECK(kernel.add_command("repair", [&](Kernel& host, CommandArgs) {
        return CommandResult{host.add_system({"missing", Phase::Simulation, 0, {}},
            [&](Kernel&, const Frame&) { ++calls; }), {}};
    }));
    CHECK(kernel.enqueue_command("repair"));
    CHECK(kernel.tick(0.01));
    CHECK(calls == 3);
    CHECK(kernel.frame().tick == 1);
}

void standalone_bus_retention_and_capacity_are_unchanged() {
    EventBus events{3, 2};
    CHECK(events.publish(1));
    CHECK(events.publish(2));
    CHECK(!events.publish(3));
    CHECK(events.publish(std::string("message")));
    CHECK(!events.publish(1.0));
    CHECK(events.dropped() == 2);
    CHECK(events.retired() == 0);
    CHECK(events.pending() == 3);
    CHECK(events.drain<double>().empty());
    CHECK(events.drain<int>(0).empty());
    CHECK(events.pending<int>() == 2);
    CHECK(events.drain<int>(1) == std::vector<int>({1}));
    CHECK(events.publish(3));
    CHECK(events.drain<int>() == std::vector<int>({2, 3}));
    CHECK(events.pending() == 1);
    CHECK(events.drain<std::string>() == std::vector<std::string>({"message"}));
    CHECK(events.pending() == 0);
    CHECK(events.retired() == 0);
}

void frame_retirement_preserves_new_events_after_partial_drains() {
    EventBus events{8, 8};
    CHECK(events.publish(1));
    CHECK(events.publish(2));
    CHECK(events.publish(3));
    events.retire_frame_events();
    CHECK(events.pending() == 3);
    CHECK(events.retired() == 0);
    CHECK(events.drain<int>(1) == std::vector<int>({1}));
    CHECK(events.publish(4));
    CHECK(events.publish(5));
    events.retire_frame_events();
    CHECK(events.retired() == 2);
    CHECK(events.pending<int>() == 2);
    CHECK(events.drain<int>() == std::vector<int>({4, 5}));
    CHECK(events.publish(6));
    events.retire_frame_events();
    CHECK(events.pending<int>() == 1);
    events.retire_frame_events();
    CHECK(events.pending() == 0);
    CHECK(events.retired() == 3);
    CHECK(events.dropped() == 0);
}

void channel_recreation_and_clear_reset_retirement_boundaries() {
    EventBus events;
    CHECK(events.publish(1));
    events.retire_frame_events();
    CHECK(events.drain<int>() == std::vector<int>({1}));
    CHECK(events.publish(2));
    events.retire_frame_events();
    CHECK(events.pending<int>() == 1);
    CHECK(events.retired() == 0);
    events.clear();
    CHECK(events.pending() == 0);
    CHECK(events.publish(3));
    events.retire_frame_events();
    CHECK(events.pending<int>() == 1);
    events.retire_frame_events();
    CHECK(events.pending() == 0);
    CHECK(events.retired() == 1);
}

void late_events_reach_the_following_frame_consumer() {
    std::vector<int> received;
    bool published = true;
    Kernel kernel;
    CHECK(kernel.add_system({"consumer", Phase::Input, 0, {}},
        [&](Kernel& host, const Frame&) {
            const auto events = host.events().drain<int>();
            received.insert(received.end(), events.begin(), events.end());
        }));
    CHECK(kernel.add_system({"late.publisher", Phase::EndFrame, 0, {}},
        [&](Kernel& host, const Frame& frame) {
            published = host.events().publish(static_cast<int>(frame.tick)) && published;
        }));
    CHECK(kernel.tick(0.01));
    CHECK(received.empty());
    CHECK(kernel.events().pending<int>() == 1);
    CHECK(kernel.events().publish(100));
    CHECK(kernel.tick(0.01));
    CHECK(received == std::vector<int>({1, 100}));
    CHECK(kernel.tick(0.01));
    CHECK(received == std::vector<int>({1, 100, 2}));
    CHECK(published);
    CHECK(kernel.events().pending<int>() == 1);
    CHECK(kernel.events().retired() == 0);
}

template<std::size_t Channel>
struct OrphanNotification {
    std::uint64_t tick;
};

template<std::size_t... Channels>
void publish_orphans(EventBus& events, std::uint64_t tick, std::size_t& rejected,
                     std::index_sequence<Channels...>) {
    ((rejected += static_cast<std::size_t>(
        !events.publish(OrphanNotification<Channels>{tick}))), ...);
}

void unconsumed_channels_do_not_permanently_starve_live_events() {
    unsigned consumed{};
    std::size_t rejected{};
    Kernel kernel;
    CHECK(kernel.add_system({"consumer", Phase::Input, 0, {}},
        [&](Kernel& host, const Frame&) {
            consumed += static_cast<unsigned>(host.events().drain<int>().size());
        }));
    CHECK(kernel.add_system({"notifications", Phase::Simulation, 0, {}},
        [&](Kernel& host, const Frame& frame) {
            for (unsigned i = 0; i < 1024; ++i) {
                publish_orphans(host.events(), frame.tick, rejected,
                                std::make_index_sequence<8>{});
            }
        }));
    // Without retirement, these eight orphan channels fill the global 65536
    // allowance after eight ticks and prevent the ninth live event admission.
    for (unsigned tick = 1; tick <= 12; ++tick) {
        CHECK(kernel.events().publish(1));
        CHECK(kernel.tick(0.01));
        CHECK(consumed == tick);
        CHECK(rejected == 0);
        CHECK(kernel.events().pending() == 8192);
        CHECK(kernel.events().retired() == (tick - 1) * std::uint64_t{8192});
        CHECK(kernel.metric_value("kernel.events.retired") ==
              static_cast<double>((tick - 1) * std::uint64_t{8192}));
    }
    CHECK(kernel.events().dropped() == 0);
}

void rejected_and_failed_ticks_preserve_lifetime_and_release_guards() {
    unsigned calls{};
    Kernel kernel;
    CHECK(kernel.events().publish(1));
    CHECK(!kernel.tick(std::numeric_limits<double>::infinity()));
    CHECK(kernel.events().pending<int>() == 1);
    CHECK(kernel.add_system({"failing", Phase::Simulation, 0, {}},
        [&](Kernel&, const Frame&) {
            ++calls;
            throw std::runtime_error("system failed");
        }));
    CHECK(!kernel.tick(0.01));
    CHECK(kernel.frame().tick == 1);
    CHECK(kernel.events().pending<int>() == 1);
    CHECK(kernel.tick(0.01));
    CHECK(kernel.frame().tick == 2);
    CHECK(calls == 1);
    CHECK(kernel.events().pending() == 0);
    CHECK(kernel.events().retired() == 1);
    CHECK(kernel.system_statistics().front().failures == 1);
    CHECK(!kernel.system_statistics().front().enabled);
}

} // namespace

int main() {
    return ge::test::run("runtime kernel", {
        {"reentrant and idempotent shutdown", shutdown_is_reentrant_and_idempotent},
        {"shutdown registration rejection", shutdown_rejects_registration_and_runs_all_handlers},
        {"bounded recursive command drains", command_drains_use_a_snapshot_and_reject_recursion},
        {"recursive tick rejection", commands_and_systems_cannot_reenter_ticks},
        {"command-installed systems", command_installed_systems_run_in_the_current_frame},
        {"queued command failures", queued_command_failures_are_reported_without_skipping_the_frame},
        {"command dependency changes", command_order_changes_are_validated_before_system_execution},
        {"invalid command-installed dependencies", command_installed_invalid_dependencies_prevent_the_frame},
        {"standalone event retention", standalone_bus_retention_and_capacity_are_unchanged},
        {"partial drains and frame retirement", frame_retirement_preserves_new_events_after_partial_drains},
        {"event channel recreation", channel_recreation_and_clear_reset_retirement_boundaries},
        {"late event delivery", late_events_reach_the_following_frame_consumer},
        {"orphan event channel starvation", unconsumed_channels_do_not_permanently_starve_live_events},
        {"failed tick guards and lifetime", rejected_and_failed_ticks_preserve_lifetime_and_release_guards},
    });
}
