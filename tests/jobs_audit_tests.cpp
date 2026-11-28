#include <ge/jobs/executor.hpp>

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <thread>

namespace {

using namespace std::chrono_literals;

void cancellation_callback_cannot_shutdown_its_executor() {
    std::binary_semaphore registered{0};
    std::binary_semaphore callback_entered{0};
    std::atomic<bool> shutdown_rejected{};
    ge::jobs::Executor executor({.worker_count = 1, .capacity = 4});
    const auto job = executor.submit([&](const ge::jobs::JobContext& context) {
        std::stop_callback callback(context.stop_token(), [&] {
            callback_entered.release();
            shutdown_rejected.store(!executor.shutdown(), std::memory_order_release);
        });
        registered.release();
        callback_entered.acquire();
        // Returning destroys callback. Its destructor waits for the callback
        // above, so joining this worker inside that callback would deadlock.
    });
    CHECK(job);
    registered.acquire();
    CHECK(executor.cancel(*job));
    CHECK(shutdown_rejected.load(std::memory_order_acquire));
    CHECK(executor.statistics().accepting);
    CHECK(executor.wait(*job, 2s));
    CHECK(executor.inspect(*job)->state == ge::jobs::JobState::Cancelled);
    CHECK(executor.release(*job));

    const auto subsequent = executor.submit([](const ge::jobs::JobContext&) {});
    CHECK(subsequent);
    CHECK(executor.shutdown(ge::jobs::ShutdownMode::Drain));
    CHECK(executor.inspect(*subsequent)->state == ge::jobs::JobState::Succeeded);
    CHECK(executor.release(*subsequent));
    CHECK(!executor.statistics().accepting);
}

void nested_cancellation_keeps_both_executor_guards() {
    std::binary_semaphore inner_registered{0};
    std::binary_semaphore inner_callback_entered{0};
    std::binary_semaphore outer_registered{0};
    std::binary_semaphore outer_callback_entered{0};
    std::atomic<bool> outer_shutdown_rejected{};
    std::atomic<bool> inner_shutdown_rejected{};
    std::atomic<bool> inner_cancel_accepted{};
    ge::jobs::Executor outer({.worker_count = 1, .capacity = 1});
    ge::jobs::Executor inner({.worker_count = 1, .capacity = 1});
    struct ShutdownBoth {
        ge::jobs::Executor& outer;
        ge::jobs::Executor& inner;
        ~ShutdownBoth() {
            (void)outer.shutdown();
            (void)inner.shutdown();
        }
    } shutdown_both{outer, inner};

    const auto inner_job = inner.submit([&](const ge::jobs::JobContext& context) {
        std::stop_callback callback(context.stop_token(), [&] {
            inner_callback_entered.release();
            outer_shutdown_rejected.store(!outer.shutdown(), std::memory_order_release);
            inner_shutdown_rejected.store(!inner.shutdown(), std::memory_order_release);
        });
        inner_registered.release();
        inner_callback_entered.acquire();
    });
    CHECK(inner_job);
    inner_registered.acquire();
    const auto outer_job = outer.submit([&, inner_handle = *inner_job](const ge::jobs::JobContext& context) {
        std::stop_callback callback(context.stop_token(), [&] {
            outer_callback_entered.release();
            inner_cancel_accepted.store(inner.cancel(inner_handle), std::memory_order_release);
        });
        outer_registered.release();
        outer_callback_entered.acquire();
    });
    CHECK(outer_job);
    outer_registered.acquire();
    CHECK(outer.cancel(*outer_job));
    CHECK(inner_cancel_accepted.load(std::memory_order_acquire));
    CHECK(outer_shutdown_rejected.load(std::memory_order_acquire));
    CHECK(inner_shutdown_rejected.load(std::memory_order_acquire));
    CHECK(outer.shutdown());
    CHECK(inner.shutdown());
    CHECK(outer.release(*outer_job));
    CHECK(inner.release(*inner_job));
}

void owner_shutdown_joins_running_job_after_stop_callback() {
    std::binary_semaphore registered{0};
    std::binary_semaphore callback_entered{0};
    std::atomic<bool> shutdown_rejected{};
    std::atomic<bool> job_exited{};
    ge::jobs::Executor executor({.worker_count = 1, .capacity = 1});
    const auto job = executor.submit([&](const ge::jobs::JobContext& context) {
        {
            std::stop_callback callback(context.stop_token(), [&] {
                callback_entered.release();
                shutdown_rejected.store(!executor.shutdown(), std::memory_order_release);
            });
            registered.release();
            callback_entered.acquire();
        }
        job_exited.store(true, std::memory_order_release);
    });
    CHECK(job);
    registered.acquire();
    CHECK(executor.shutdown());
    CHECK(shutdown_rejected.load(std::memory_order_acquire));
    CHECK(job_exited.load(std::memory_order_acquire));
    CHECK(executor.statistics().running == 0);
    CHECK(executor.inspect(*job)->state == ge::jobs::JobState::Cancelled);
    CHECK(executor.release(*job));
    CHECK(executor.shutdown());
}

void cancelled_dependencies_preserve_graph_ownership_until_release() {
    std::atomic<unsigned> skipped_calls{};
    std::atomic<unsigned> cleanup_calls{};
    ge::jobs::Executor executor({.worker_count = 1, .capacity = 3});
    CHECK(executor.pause());
    ge::jobs::TaskGraph graph;
    const auto root = graph.add([&](const ge::jobs::JobContext&) { ++skipped_calls; });
    const auto dependent = graph.add([&](const ge::jobs::JobContext&) { ++skipped_calls; });
    ge::jobs::JobOptions cleanup_options;
    cleanup_options.run_after_failed_dependency = true;
    const auto cleanup = graph.add([&](const ge::jobs::JobContext&) { ++cleanup_calls; },
                                   cleanup_options);
    CHECK(root && dependent && cleanup);
    CHECK(graph.depends_on(*dependent, *root));
    CHECK(graph.depends_on(*cleanup, *dependent));
    const auto submitted = executor.submit(graph);
    CHECK(submitted);
    CHECK(executor.cancel(submitted->jobs[0]));
    CHECK(executor.inspect(submitted->jobs[0])->state == ge::jobs::JobState::Cancelled);
    CHECK(executor.inspect(submitted->jobs[1])->state == ge::jobs::JobState::Cancelled);
    CHECK(executor.inspect(submitted->jobs[2])->state == ge::jobs::JobState::Queued);
    CHECK(!executor.release(submitted->jobs[0]));
    CHECK(!executor.release(submitted->graph));
    CHECK(executor.poll().size() == 2);
    CHECK(executor.statistics().retained == 3);
    CHECK(!executor.submit([](const ge::jobs::JobContext&) {}));
    CHECK(executor.resume());
    CHECK(executor.wait(submitted->graph, 2s));
    CHECK(skipped_calls.load() == 0);
    CHECK(cleanup_calls.load() == 1);
    const auto finished = executor.inspect(submitted->graph);
    CHECK(finished && finished->finished());
    CHECK(finished->cancelled == 2 && finished->succeeded == 1);
    CHECK(executor.poll().size() == 1);
    CHECK(executor.statistics().retained == 3);
    CHECK(executor.release(submitted->graph));
    CHECK(executor.statistics().retained == 0);
    for (const auto handle : submitted->jobs) {
        CHECK(!executor.inspect(handle));
        CHECK(!executor.cancel(handle));
        CHECK(!executor.release(handle));
    }
    const auto replacement = executor.submit([](const ge::jobs::JobContext&) {});
    CHECK(replacement);
    for (const auto handle : submitted->jobs) {
        CHECK(*replacement != handle);
    }
    CHECK(executor.wait(*replacement, 2s));
    CHECK(executor.release(*replacement));
}

} // namespace

int main() {
    // Bound the process even when the regression blocks in shutdown or in a
    // stop_callback destructor, where an ordinary timed assertion cannot run.
    std::jthread watchdog([](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, stop, 10s, [] { return false; });
        if (!stop.stop_requested()) {
            std::fputs("[FAIL] jobs_audit: lifecycle regression exceeded 10 seconds\n", stderr);
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
    });
    return ge::test::run("jobs_audit", {
        {"cancellation_callback_cannot_shutdown_its_executor",
         cancellation_callback_cannot_shutdown_its_executor},
        {"nested_cancellation_keeps_both_executor_guards",
         nested_cancellation_keeps_both_executor_guards},
        {"owner_shutdown_joins_running_job_after_stop_callback",
         owner_shutdown_joins_running_job_after_stop_callback},
        {"cancelled_dependencies_preserve_graph_ownership_until_release",
         cancelled_dependencies_preserve_graph_ownership_until_release},
    });
}
