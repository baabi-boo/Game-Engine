#pragma once

#include <ge/core/identity.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::jobs {

namespace detail { struct JobControl; }

using JobHandle = ge::RuntimeHandle;
using NodeId = std::uint32_t;

enum class Priority : std::uint8_t { Background, Low, Normal, High };
enum class JobState : std::uint8_t { Waiting, Queued, Running, Succeeded, Failed, Cancelled };
enum class ShutdownMode : std::uint8_t { Drain, Cancel };

[[nodiscard]] bool terminal(JobState state) noexcept;
[[nodiscard]] std::string_view state_name(JobState state) noexcept;

class JobCancelled final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override;
};

// A context belongs to one invocation. It must not escape the job callback.
class JobContext final {
public:
    [[nodiscard]] JobHandle handle() const noexcept;
    [[nodiscard]] std::stop_token stop_token() const noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;
    [[nodiscard]] std::size_t worker_index() const noexcept;
    [[nodiscard]] std::uint64_t tag() const noexcept;
    void throw_if_stopped() const;
    // Nonfinite values are rejected; valid progress is clamped and monotonic.
    bool report_progress(double progress) const noexcept;

private:
    friend class Executor;
    JobContext(std::shared_ptr<detail::JobControl> control, std::size_t worker) noexcept;
    std::shared_ptr<detail::JobControl> control_;
    std::size_t worker_{};
};

using JobFunction = std::function<void(const JobContext&)>;
using RangeFunction = std::function<void(const JobContext&, std::size_t, std::size_t)>;

struct JobOptions {
    std::string name;
    Priority priority{Priority::Normal};
    std::uint64_t tag{};
    // Graph nodes normally skip execution after a failed/cancelled prerequisite.
    bool run_after_failed_dependency{};
};

struct ExecutorOptions {
    std::size_t worker_count{2};
    std::uint32_t capacity{4096};
    std::size_t graph_capacity{128};
    std::size_t max_graph_edges{65536};
    std::chrono::milliseconds aging_interval{100};
    std::uint64_t generation_limit{UINT64_MAX};
};

struct JobSnapshot {
    JobHandle handle;
    JobState state{JobState::Waiting};
    Priority priority{Priority::Normal};
    std::string name;
    std::uint64_t tag{};
    double progress{};
    double queue_seconds{};
    double execution_seconds{};
    std::size_t pending_dependencies{};
    bool cancellation_requested{};
    std::string error;
    std::exception_ptr exception;
};

struct JobCompletion {
    JobHandle handle;
    JobState state{JobState::Succeeded};
    std::string name;
    std::uint64_t tag{};
    double queue_seconds{};
    double execution_seconds{};
    std::string error;
    std::exception_ptr exception;
};

struct ExecutorStatistics {
    std::size_t workers{};
    std::size_t capacity{};
    std::size_t retained{};
    std::size_t queued{};
    std::size_t waiting{};
    std::size_t running{};
    std::size_t terminal_jobs{};
    std::size_t graph_count{};
    std::size_t high_watermark{};
    std::size_t pending_notifications{};
    std::uint64_t submitted{};
    std::uint64_t succeeded{};
    std::uint64_t failed{};
    std::uint64_t cancelled{};
    std::uint64_t rejected{};
    std::uint64_t dropped_notifications{};
    bool accepting{};
    bool paused{};
};

struct GraphHandle {
    std::uint64_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const GraphHandle&) const = default;
};

struct GraphValidation {
    bool valid{};
    std::string error;
    std::vector<NodeId> topological_order;
    std::vector<std::vector<NodeId>> levels;
    std::size_t edges{};
    std::size_t maximum_parallel_width{};
};

// Single-writer graph authoring. Dependencies are expressed as node -> prerequisite.
// Submission copies callable state, so subsequent authoring cannot alter live work.
class TaskGraph final {
public:
    explicit TaskGraph(std::size_t max_nodes = 4096, std::size_t max_edges = 65536);
    [[nodiscard]] std::optional<NodeId> add(JobFunction function, JobOptions options = {});
    [[nodiscard]] std::optional<NodeId> barrier(std::string name,
                                               std::span<const NodeId> prerequisites);
    bool depends_on(NodeId node, NodeId prerequisite);
    bool remove_dependency(NodeId node, NodeId prerequisite);
    bool set_options(NodeId node, JobOptions options);
    [[nodiscard]] std::optional<JobOptions> options(NodeId node) const;
    [[nodiscard]] std::vector<NodeId> prerequisites(NodeId node) const;
    [[nodiscard]] std::vector<NodeId> dependents(NodeId node) const;
    [[nodiscard]] std::vector<NodeId> roots() const;
    [[nodiscard]] std::vector<NodeId> leaves() const;
    [[nodiscard]] bool reachable(NodeId from, NodeId to) const;
    [[nodiscard]] GraphValidation validate() const;
    // Adds disjoint [first,last) jobs. Failure preserves the original graph.
    [[nodiscard]] std::vector<NodeId> parallel_for(std::size_t count,
                                                  std::size_t grain,
                                                  RangeFunction function,
                                                  JobOptions options = {});
    // Copies another graph and returns its old-ID -> new-ID mapping in node order.
    [[nodiscard]] std::vector<NodeId> append(const TaskGraph& other,
                                            std::string_view name_prefix = {});
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t edge_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    void clear() noexcept;

private:
    friend class Executor;
    struct Node {
        JobFunction function;
        JobOptions options;
        std::vector<NodeId> prerequisites;
    };
    [[nodiscard]] bool valid_id(NodeId node) const noexcept;
    std::size_t max_nodes_;
    std::size_t max_edges_;
    std::size_t edge_count_{};
    std::vector<Node> nodes_;
};

struct GraphSubmission {
    GraphHandle graph;
    // Entry i corresponds to source NodeId i+1.
    std::vector<JobHandle> jobs;
};

struct GraphSnapshot {
    GraphHandle handle;
    std::size_t total{};
    std::size_t queued{};
    std::size_t waiting{};
    std::size_t running{};
    std::size_t succeeded{};
    std::size_t failed{};
    std::size_t cancelled{};
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] bool successful() const noexcept;
};

class Executor final {
public:
    explicit Executor(ExecutorOptions options = {});
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    [[nodiscard]] std::optional<JobHandle> submit(JobFunction function,
                                                 JobOptions options = {});
    [[nodiscard]] std::optional<GraphSubmission> submit(const TaskGraph& graph);
    [[nodiscard]] std::optional<GraphSubmission> parallel_for(std::size_t count,
                                                             std::size_t grain,
                                                             RangeFunction function,
                                                             JobOptions options = {});
    bool cancel(JobHandle handle);
    std::size_t cancel(GraphHandle graph);
    std::size_t cancel_tag(std::uint64_t tag);
    [[nodiscard]] std::optional<JobSnapshot> inspect(JobHandle handle) const;
    [[nodiscard]] std::optional<GraphSnapshot> inspect(GraphHandle handle) const;
    [[nodiscard]] std::vector<JobSnapshot> inspect_all(std::size_t limit = 4096) const;
    [[nodiscard]] std::vector<JobCompletion> poll(std::size_t limit = 256);

    // Completion notifications do not release slots. Ownership remains charged
    // through terminal state until release; graph jobs release as one graph.
    bool release(JobHandle handle);
    bool release(GraphHandle handle);
    std::size_t release_completed(std::uint64_t tag);
    [[nodiscard]] bool wait(JobHandle handle, std::chrono::milliseconds timeout);
    [[nodiscard]] bool wait(GraphHandle handle, std::chrono::milliseconds timeout);
    [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);
    [[nodiscard]] ExecutorStatistics statistics() const;
    bool pause();
    bool resume();
    // Owner-thread operation. Calls from this executor's workers return false.
    // Concurrent callers are serialized; no job/stop callback runs under a lock.
    bool shutdown(ShutdownMode mode = ShutdownMode::Cancel);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static void run_worker(Impl* implementation, std::stop_token stop,
                           std::size_t worker_index);
};

// Published by the kernel pump. inspect(handle) remains authoritative until release.
struct JobCompletedEvent { JobCompletion completion; };

void register_executor(ge::core::Kernel& kernel);

} // namespace ge::jobs
