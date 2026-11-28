#include "test_support.hpp"

#include <ge/core/signals.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

using ge::core::SignalExceptionPolicy;
using ge::core::SignalHub;
using ge::core::SignalListenerOptions;
using ge::core::SignalReceipt;
using ge::core::SignalStatus;
using ge::core::SignalTraceKind;

void close_during_a_throwing_final_callback_cancels_the_event() {
    for (const bool borrowed : {false, true}) {
        SignalHub hub;
        const auto signal = hub.channel<int>();
        hub.set_exception_policy(SignalExceptionPolicy::Rethrow);
        CHECK(hub.subscribe(signal, [&](const int&) {
            hub.close();
            throw std::runtime_error("closed from callback");
        }));
        if (borrowed) {
            CHECK_THROWS_AS(hub.emit_borrowed(signal, 7), std::runtime_error);
        } else {
            CHECK(hub.enqueue(signal, 7));
            CHECK_THROWS_AS(hub.flush(), std::runtime_error);
        }
        const auto stats = hub.statistics();
        CHECK(stats.closed);
        CHECK(!stats.dispatching);
        CHECK(stats.pending == 0);
        CHECK(stats.pending_accounted_bytes == 0);
        CHECK(stats.callbacks == 1);
        CHECK(stats.exceptions == 1);
        CHECK(stats.completed == 0);
        CHECK(stats.cancelled == 1);
        CHECK(hub.history().back().kind == SignalTraceKind::Cancelled);
    }
}

struct OnDestroy {
    explicit OnDestroy(std::function<void()> value) : action(std::move(value)) {}
    ~OnDestroy() { action(); }
    std::function<void()> action;
};

void deferred_capture_destruction_is_included_in_the_dispatch_report() {
    SignalReceipt queued{SignalStatus::Closed};
    bool disconnected{};
    SignalHub hub;
    const auto signal = hub.channel<int>("event");
    const auto other = hub.channel<int>("other");
    auto enqueue_on_destroy = std::make_shared<OnDestroy>([&] {
        queued = hub.enqueue(signal, 2);
    });
    const auto deferred = hub.subscribe(other,
        [keep = enqueue_on_destroy](const int&) { static_cast<void>(keep); });
    enqueue_on_destroy.reset();

    auto disconnect_on_destroy = std::make_shared<OnDestroy>([&, deferred] {
        disconnected = hub.disconnect(deferred);
    });
    SignalListenerOptions once;
    once.once = true;
    const auto first = hub.subscribe(signal,
        [keep = disconnect_on_destroy](const int&) { static_cast<void>(keep); }, once);
    disconnect_on_destroy.reset();
    CHECK(first);
    CHECK(deferred);
    CHECK(hub.enqueue(signal, 1));

    // Reclaiming the once-listener releases its capture, which disconnects the
    // other listener. That second capture is destroyed by the dispatch guard.
    const auto report = hub.flush(1);
    CHECK(disconnected);
    CHECK(queued);
    CHECK(report.completed == 1);
    CHECK(report.callbacks == 1);
    CHECK(report.remaining == 1);
    CHECK(report.budget_exhausted);
    CHECK(hub.statistics().pending == report.remaining);
    CHECK(!hub.statistics().dispatching);
    CHECK(hub.flush().completed == 1);
    CHECK(hub.statistics().pending == 0);
    CHECK(hub.statistics().pending_accounted_bytes == 0);
}

} // namespace

int main() {
    return ge::test::run("signals_lifecycle_audit", {
        {"close while rethrowing cancels", close_during_a_throwing_final_callback_cancels_the_event},
        {"deferred cleanup report", deferred_capture_destruction_is_included_in_the_dispatch_report},
    });
}
