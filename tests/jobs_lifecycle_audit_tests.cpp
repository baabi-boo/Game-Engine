#include <ge/jobs/executor.hpp>
#include <ge/core/runtime.hpp>

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <thread>

namespace {

using namespace std::chrono_literals;

void released_waited_job_destroys_captures_without_the_state_lock() {
    struct Capture {
        ge::jobs::Executor& executor;
        std::atomic<bool>& destroyed;

        ~Capture() {
            (void)executor.statistics();
            destroyed.store(true, std::memory_order_release);
        }
    };

    ge::jobs::Executor executor({.worker_count = 1, .capacity = 1});
    CHECK(executor.pause());
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        std::atomic<bool> destroyed{};
        std::binary_semaphore entering_wait{0};
        auto capture = std::make_shared<Capture>(executor, destroyed);
        const auto job = executor.submit([capture](const ge::jobs::JobContext&) {});
        CHECK(job);
        capture.reset();
        std::jthread waiter([&] {
            entering_wait.release();
            (void)executor.wait(*job, 2s);
        });
        entering_wait.acquire();
        // Give the waiter an opportunity to retain the active job before its
        // cancellation and release race with the completion wake-up.
        std::this_thread::sleep_for(1ms);
        CHECK(executor.cancel(*job));
        CHECK(executor.release(*job));
        waiter.join();
        CHECK(destroyed.load(std::memory_order_acquire));
        CHECK(executor.statistics().retained == 0);
    }
}

void cancellation_callback_cannot_wait_for_its_executor() {
    std::binary_semaphore registered{0};
    std::binary_semaphore callback_entered{0};
    std::atomic<bool> job_wait_rejected{};
    std::atomic<bool> graph_wait_rejected{};
    std::atomic<bool> idle_wait_rejected{};
    ge::jobs::Executor executor({.worker_count = 1, .capacity = 1});
    CHECK(executor.pause());
    ge::jobs::TaskGraph graph;
    ge::jobs::GraphHandle graph_handle;
    CHECK(graph.add([&](const ge::jobs::JobContext& context) {
        std::stop_callback callback(context.stop_token(), [&] {
            callback_entered.release();
            job_wait_rejected.store(!executor.wait(context.handle(), 1h),
                                    std::memory_order_release);
            graph_wait_rejected.store(!executor.wait(graph_handle, 1h),
                                      std::memory_order_release);
            idle_wait_rejected.store(!executor.wait_idle(1h), std::memory_order_release);
        });
        registered.release();
        callback_entered.acquire();
        // Destroying callback must wait for the cancellation dispatch above;
        // none of those waits can therefore make progress inside the callback.
    }));
    const auto submitted = executor.submit(graph);
    CHECK(submitted);
    graph_handle = submitted->graph;
    CHECK(executor.resume());
    registered.acquire();
    CHECK(executor.cancel(submitted->jobs.front()));
    CHECK(job_wait_rejected.load(std::memory_order_acquire));
    CHECK(graph_wait_rejected.load(std::memory_order_acquire));
    CHECK(idle_wait_rejected.load(std::memory_order_acquire));
    CHECK(executor.wait(graph_handle, 2s));
    CHECK(executor.inspect(graph_handle)->cancelled == 1);
    CHECK(executor.release(graph_handle));
}

void kernel_reports_refused_executor_cleanup_from_an_external_stop_callback() {
    ge::core::Kernel kernel;
    CHECK(kernel.config().set("jobs.workers", "1"));
    ge::jobs::register_executor(kernel);
    auto& executor = kernel.services().get<ge::jobs::Executor>();
    std::binary_semaphore registered{0};
    std::binary_semaphore observed{0};
    bool callback_saw_open_executor{};
    std::size_t callback_failures{};
    const auto job = executor.submit([&](const ge::jobs::JobContext& context) {
        std::stop_callback callback(context.stop_token(), [&] {
            // Direct owner-thread cancellation is outside a Kernel command or
            // tick. This callback cannot synchronously join its own worker.
            kernel.shutdown();
            callback_saw_open_executor = executor.statistics().accepting;
            callback_failures = kernel.shutdown_failures();
            observed.release();
        });
        registered.release();
        observed.acquire();
    });
    CHECK(job);
    registered.acquire();
    CHECK(executor.cancel(*job));
    CHECK(callback_saw_open_executor);
    CHECK(callback_failures == 1);
    CHECK(kernel.shutdown_failures() == 1);
    CHECK(kernel.services().find<ge::jobs::Executor>() == &executor);
    CHECK(std::any_of(kernel.diagnostics().begin(), kernel.diagnostics().end(),
        [](const ge::core::Diagnostic& diagnostic) {
            return diagnostic.severity == ge::core::Diagnostic::Severity::Error &&
                diagnostic.subsystem == "jobs.executor";
        }));
    CHECK(executor.wait(*job, 2s));
    kernel.shutdown();
    CHECK(kernel.shutdown_failures() == 1);
    // The owner can explicitly finish cleanup after cancellation dispatch has
    // unwound. Reporting the refused callback must not destroy the service.
    CHECK(executor.shutdown());
    CHECK(!executor.statistics().accepting);
    CHECK(executor.statistics().running == 0);
    CHECK(executor.release(*job));
}

} // namespace

int main() {
    std::jthread watchdog([](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, stop, 10s, [] { return false; });
        if (!stop.stop_requested()) {
            std::fputs("[FAIL] jobs_lifecycle_audit: lifecycle regression exceeded 10 seconds\n", stderr);
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
    });
    return ge::test::run("jobs_lifecycle_audit", {
        {"released_waited_job_destroys_captures_without_the_state_lock",
         released_waited_job_destroys_captures_without_the_state_lock},
        {"cancellation_callback_cannot_wait_for_its_executor",
         cancellation_callback_cannot_wait_for_its_executor},
        {"kernel_reports_refused_executor_cleanup_from_an_external_stop_callback",
         kernel_reports_refused_executor_cleanup_from_an_external_stop_callback},
    });
}
