#include <ge/jobs/executor.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <utility>

namespace ge::jobs {

namespace {
using Clock = std::chrono::steady_clock;
thread_local const void* executing_executor{};

// request_stop invokes user callbacks on the caller's thread. Keep every
// nested dispatch visible: a callback may cancel a job on another executor
// before attempting to shut down the original executor.
class CancellationDispatch final {
public:
    explicit CancellationDispatch(const void* executor) noexcept
        : executor_(executor), previous_(active_) {
        active_ = this;
    }

    ~CancellationDispatch() { active_ = previous_; }
    CancellationDispatch(const CancellationDispatch&) = delete;
    CancellationDispatch& operator=(const CancellationDispatch&) = delete;

    [[nodiscard]] static bool contains(const void* executor) noexcept {
        for (auto* dispatch = active_; dispatch; dispatch = dispatch->previous_) {
            if (dispatch->executor_ == executor) {
                return true;
            }
        }
        return false;
    }

private:
    static thread_local const CancellationDispatch* active_;
    const void* executor_;
    const CancellationDispatch* previous_;
};

thread_local const CancellationDispatch* CancellationDispatch::active_{};

bool valid_priority(Priority priority) noexcept {
    return static_cast<unsigned>(priority) <= static_cast<unsigned>(Priority::High);
}

bool valid_options(const JobOptions& options) noexcept {
    return options.name.size() <= 128 && valid_priority(options.priority);
}

double seconds_between(Clock::time_point start, Clock::time_point finish) noexcept {
    return std::max(0.0, std::chrono::duration<double>(finish - start).count());
}

std::string bounded_exception_message(const std::exception& error) {
    const auto* message = error.what();
    if (!message) {
        return "job threw an exception without a message";
    }
    std::size_t size{};
    while (size < 1024 && message[size] != '\0') {
        ++size;
    }
    return std::string(message, size);
}

template<class Integer>
bool parse_integer(std::string_view text, Integer& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
}

namespace detail {
struct JobControl {
    JobHandle handle;
    JobOptions options;
    JobFunction function;
    std::stop_source stop;
    std::atomic<bool> cancellation_requested{};
    std::atomic<double> progress{};
    JobState state{JobState::Waiting};
    GraphHandle graph;
    std::size_t pending_dependencies{};
    bool dependency_failed{};
    std::vector<JobHandle> dependents;
    std::uint64_t sequence{};
    Clock::time_point submitted{};
    Clock::time_point started{};
    Clock::time_point finished{};
    std::string error;
    std::exception_ptr exception;
};
}

bool terminal(JobState state) noexcept {
    return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled;
}

std::string_view state_name(JobState state) noexcept {
    switch (state) {
    case JobState::Waiting: return "waiting";
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* JobCancelled::what() const noexcept {
    return "job cancellation requested";
}

JobContext::JobContext(std::shared_ptr<detail::JobControl> control,
                       std::size_t worker) noexcept
    : control_(std::move(control)), worker_(worker) {}

JobHandle JobContext::handle() const noexcept {
    return control_->handle;
}

std::stop_token JobContext::stop_token() const noexcept {
    return control_->stop.get_token();
}

bool JobContext::stop_requested() const noexcept {
    return control_->cancellation_requested.load(std::memory_order_acquire) ||
           control_->stop.stop_requested();
}

std::size_t JobContext::worker_index() const noexcept {
    return worker_;
}

std::uint64_t JobContext::tag() const noexcept {
    return control_->options.tag;
}

void JobContext::throw_if_stopped() const {
    if (stop_requested()) {
        throw JobCancelled{};
    }
}

bool JobContext::report_progress(double progress) const noexcept {
    if (!std::isfinite(progress)) {
        return false;
    }
    const double next = std::clamp(progress, 0.0, 1.0);
    auto current = control_->progress.load(std::memory_order_relaxed);
    while (current < next) {
        if (control_->progress.compare_exchange_weak(current, next,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            break;
        }
    }
    return true;
}

TaskGraph::TaskGraph(std::size_t max_nodes, std::size_t max_edges)
    : max_nodes_(max_nodes), max_edges_(max_edges) {
    if (max_nodes_ == 0 || max_nodes_ > 1048576 || max_edges_ > 16777216) {
        throw std::invalid_argument("task graph bounds exceed supported limits");
    }
}

bool TaskGraph::valid_id(NodeId node) const noexcept {
    return node != 0 && static_cast<std::size_t>(node) <= nodes_.size();
}

std::optional<NodeId> TaskGraph::add(JobFunction function, JobOptions options) {
    if (!function || !valid_options(options) || nodes_.size() >= max_nodes_) {
        return std::nullopt;
    }
    const auto next = static_cast<NodeId>(nodes_.size() + 1);
    if (options.name.empty()) {
        options.name = "node-" + std::to_string(next);
    }
    nodes_.push_back(Node{std::move(function), std::move(options), {}});
    return next;
}

std::optional<NodeId> TaskGraph::barrier(std::string name,
                                        std::span<const NodeId> prerequisites) {
    if (name.size() > 128 || nodes_.size() >= max_nodes_ ||
        prerequisites.size() > max_edges_ - edge_count_) {
        return std::nullopt;
    }
    std::vector<NodeId> dependencies(prerequisites.begin(), prerequisites.end());
    std::sort(dependencies.begin(), dependencies.end());
    if (std::adjacent_find(dependencies.begin(), dependencies.end()) != dependencies.end()) {
        return std::nullopt;
    }
    for (const auto node : dependencies) {
        if (!valid_id(node)) {
            return std::nullopt;
        }
    }
    JobOptions options;
    options.name = std::move(name);
    const auto id = add([](const JobContext& context) { context.throw_if_stopped(); },
                        std::move(options));
    if (!id) {
        return std::nullopt;
    }
    nodes_.back().prerequisites = std::move(dependencies);
    edge_count_ += nodes_.back().prerequisites.size();
    return id;
}

bool TaskGraph::depends_on(NodeId node, NodeId prerequisite) {
    if (!valid_id(node) || !valid_id(prerequisite) || node == prerequisite ||
        edge_count_ >= max_edges_) {
        return false;
    }
    auto& dependencies = nodes_[static_cast<std::size_t>(node - 1)].prerequisites;
    const auto position = std::lower_bound(dependencies.begin(), dependencies.end(), prerequisite);
    if (position != dependencies.end() && *position == prerequisite) {
        return false;
    }
    if (reachable(prerequisite, node)) {
        return false;
    }
    dependencies.insert(position, prerequisite);
    ++edge_count_;
    return true;
}

bool TaskGraph::remove_dependency(NodeId node, NodeId prerequisite) {
    if (!valid_id(node)) {
        return false;
    }
    auto& dependencies = nodes_[static_cast<std::size_t>(node - 1)].prerequisites;
    const auto position = std::lower_bound(dependencies.begin(), dependencies.end(), prerequisite);
    if (position == dependencies.end() || *position != prerequisite) {
        return false;
    }
    dependencies.erase(position);
    --edge_count_;
    return true;
}

bool TaskGraph::set_options(NodeId node, JobOptions options) {
    if (!valid_id(node) || !valid_options(options)) {
        return false;
    }
    nodes_[static_cast<std::size_t>(node - 1)].options = std::move(options);
    return true;
}

std::optional<JobOptions> TaskGraph::options(NodeId node) const {
    if (!valid_id(node)) {
        return std::nullopt;
    }
    return nodes_[static_cast<std::size_t>(node - 1)].options;
}

std::vector<NodeId> TaskGraph::prerequisites(NodeId node) const {
    if (!valid_id(node)) {
        return {};
    }
    return nodes_[static_cast<std::size_t>(node - 1)].prerequisites;
}

std::vector<NodeId> TaskGraph::dependents(NodeId node) const {
    std::vector<NodeId> result;
    if (!valid_id(node)) {
        return result;
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto& dependencies = nodes_[i].prerequisites;
        if (std::binary_search(dependencies.begin(), dependencies.end(), node)) {
            result.push_back(static_cast<NodeId>(i + 1));
        }
    }
    return result;
}

std::vector<NodeId> TaskGraph::roots() const {
    std::vector<NodeId> result;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].prerequisites.empty()) {
            result.push_back(static_cast<NodeId>(i + 1));
        }
    }
    return result;
}

std::vector<NodeId> TaskGraph::leaves() const {
    std::vector<bool> has_dependents(nodes_.size(), false);
    for (const auto& node : nodes_) {
        for (const auto prerequisite : node.prerequisites) {
            has_dependents[static_cast<std::size_t>(prerequisite - 1)] = true;
        }
    }
    std::vector<NodeId> result;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!has_dependents[i]) {
            result.push_back(static_cast<NodeId>(i + 1));
        }
    }
    return result;
}

bool TaskGraph::reachable(NodeId from, NodeId to) const {
    if (!valid_id(from) || !valid_id(to)) {
        return false;
    }
    std::vector<bool> visited(nodes_.size(), false);
    std::vector<NodeId> pending;
    pending.reserve(nodes_.size());
    pending.push_back(from);
    visited[static_cast<std::size_t>(from - 1)] = true;
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (current == to) {
            return true;
        }
        for (const auto next : nodes_[static_cast<std::size_t>(current - 1)].prerequisites) {
            const auto index = static_cast<std::size_t>(next - 1);
            if (!visited[index]) {
                visited[index] = true;
                pending.push_back(next);
            }
        }
    }
    return false;
}

GraphValidation TaskGraph::validate() const {
    GraphValidation result;
    result.edges = edge_count_;
    if (nodes_.empty()) {
        result.error = "cannot submit an empty task graph";
        return result;
    }
    if (nodes_.size() > max_nodes_ || edge_count_ > max_edges_) {
        result.error = "graph exceeds configured bounds";
        return result;
    }
    std::vector<std::size_t> remaining(nodes_.size());
    std::vector<std::vector<NodeId>> children(nodes_.size());
    std::vector<NodeId> ready;
    ready.reserve(nodes_.size());
    result.topological_order.reserve(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto& node = nodes_[i];
        if (!node.function || !valid_options(node.options)) {
            result.error = "graph contains an invalid callable or options";
            return result;
        }
        remaining[i] = node.prerequisites.size();
        if (remaining[i] == 0) {
            ready.push_back(static_cast<NodeId>(i + 1));
        }
        for (const auto prerequisite : node.prerequisites) {
            if (!valid_id(prerequisite) || prerequisite == i + 1) {
                result.error = "graph contains an invalid dependency";
                return result;
            }
            children[static_cast<std::size_t>(prerequisite - 1)].push_back(static_cast<NodeId>(i + 1));
        }
    }
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end());
        result.maximum_parallel_width = std::max(result.maximum_parallel_width, ready.size());
        result.levels.push_back(ready);
        std::vector<NodeId> next;
        next.reserve(nodes_.size());
        for (const auto current : ready) {
            result.topological_order.push_back(current);
            for (const auto child : children[static_cast<std::size_t>(current - 1)]) {
                auto& count = remaining[static_cast<std::size_t>(child - 1)];
                if (--count == 0) {
                    next.push_back(child);
                }
            }
        }
        ready = std::move(next);
    }
    if (result.topological_order.size() != nodes_.size()) {
        result.error = "task dependency cycle";
        result.topological_order.clear();
        result.levels.clear();
        return result;
    }
    result.valid = true;
    return result;
}

std::vector<NodeId> TaskGraph::parallel_for(std::size_t count, std::size_t grain,
                                           RangeFunction function, JobOptions options) {
    if (count == 0 || grain == 0 || !function || !valid_options(options)) {
        return {};
    }
    const auto chunks = count / grain + (count % grain == 0 ? 0U : 1U);
    if (chunks > max_nodes_ - nodes_.size()) {
        return {};
    }
    auto next = nodes_;
    next.reserve(nodes_.size() + chunks);
    std::vector<NodeId> result;
    result.reserve(chunks);
    for (std::size_t begin = 0; begin < count;) {
        const auto length = std::min(grain, count - begin);
        const auto end = begin + length;
        auto node_options = options;
        if (!node_options.name.empty()) {
            const auto suffix = "/" + std::to_string(result.size());
            if (node_options.name.size() + suffix.size() > 128) {
                return {};
            }
            node_options.name += suffix;
        }
        JobFunction task = [function, begin, end](const JobContext& context) {
            context.throw_if_stopped();
            function(context, begin, end);
        };
        next.push_back(Node{std::move(task), std::move(node_options), {}});
        result.push_back(static_cast<NodeId>(next.size()));
        begin = end;
    }
    nodes_.swap(next);
    return result;
}

std::vector<NodeId> TaskGraph::append(const TaskGraph& other, std::string_view name_prefix) {
    if (other.size() > max_nodes_ - nodes_.size() ||
        other.edge_count() > max_edges_ - edge_count_ || name_prefix.size() > 128) {
        return {};
    }
    auto next = nodes_;
    next.reserve(nodes_.size() + other.size());
    const auto offset = static_cast<NodeId>(nodes_.size());
    std::vector<NodeId> mapping;
    mapping.reserve(other.size());
    for (const auto& node : other.nodes_) {
        auto copy = node;
        if (name_prefix.size() + copy.options.name.size() > 128) {
            return {};
        }
        copy.options.name.insert(0, name_prefix);
        for (auto& prerequisite : copy.prerequisites) {
            prerequisite += offset;
        }
        next.push_back(std::move(copy));
        mapping.push_back(static_cast<NodeId>(next.size()));
    }
    const auto next_edge_count = edge_count_ + other.edge_count_;
    nodes_.swap(next);
    edge_count_ = next_edge_count;
    return mapping;
}

std::size_t TaskGraph::size() const noexcept {
    return nodes_.size();
}

std::size_t TaskGraph::edge_count() const noexcept {
    return edge_count_;
}

bool TaskGraph::empty() const noexcept {
    return nodes_.empty();
}

void TaskGraph::clear() noexcept {
    nodes_.clear();
    edge_count_ = 0;
}

bool GraphSnapshot::finished() const noexcept {
    return total == succeeded + failed + cancelled;
}

bool GraphSnapshot::successful() const noexcept {
    return total != 0 && succeeded == total;
}

struct Executor::Impl {
    struct GraphRecord {
        GraphHandle handle;
        std::vector<JobHandle> jobs;
    };

    explicit Impl(ExecutorOptions configured)
        : options(configured), handles(configured.capacity, configured.generation_limit),
          slots(configured.capacity), notifications(configured.capacity) {
        propagation.reserve(configured.capacity);
        workers.reserve(configured.worker_count);
    }

    [[nodiscard]] std::shared_ptr<detail::JobControl> find(JobHandle handle) const {
        if (!handles.alive(handle)) {
            return {};
        }
        const auto& result = slots[static_cast<std::size_t>(handle.index - 1)];
        return result && result->handle == handle ? result : nullptr;
    }

    [[nodiscard]] JobSnapshot snapshot(const detail::JobControl& job) const {
        const auto now = Clock::now();
        const bool started = job.started != Clock::time_point{};
        JobSnapshot result;
        result.handle = job.handle;
        result.state = job.state;
        result.priority = job.options.priority;
        result.name = job.options.name;
        result.tag = job.options.tag;
        result.progress = job.progress.load(std::memory_order_relaxed);
        result.queue_seconds = seconds_between(job.submitted, started ? job.started :
            (terminal(job.state) ? job.finished : now));
        result.execution_seconds = started ? seconds_between(job.started,
            terminal(job.state) ? job.finished : now) : 0.0;
        result.pending_dependencies = job.pending_dependencies;
        result.cancellation_requested = job.cancellation_requested.load(std::memory_order_acquire);
        result.error = job.error;
        result.exception = job.exception;
        if (result.error.empty() && job.state == JobState::Cancelled) {
            result.error = job.dependency_failed ? "prerequisite did not succeed" : "job cancelled";
        }
        return result;
    }

    [[nodiscard]] JobCompletion completion(const detail::JobControl& job) const {
        const auto observed = snapshot(job);
        return {observed.handle, observed.state, observed.name, observed.tag,
                observed.queue_seconds, observed.execution_seconds,
                observed.error, observed.exception};
    }

    [[nodiscard]] GraphSnapshot graph_snapshot(const GraphRecord& graph) const {
        GraphSnapshot result;
        result.handle = graph.handle;
        result.total = graph.jobs.size();
        for (const auto handle : graph.jobs) {
            const auto job = find(handle);
            if (!job) {
                ++result.failed;
                continue;
            }
            switch (job->state) {
            case JobState::Waiting: ++result.waiting; break;
            case JobState::Queued: ++result.queued; break;
            case JobState::Running: ++result.running; break;
            case JobState::Succeeded: ++result.succeeded; break;
            case JobState::Failed: ++result.failed; break;
            case JobState::Cancelled: ++result.cancelled; break;
            }
        }
        return result;
    }

    void notify_completion(const std::shared_ptr<detail::JobControl>& job) noexcept {
        if (notification_count == notifications.size()) {
            ++dropped_notifications;
            return;
        }
        const auto position = (notification_head + notification_count) % notifications.size();
        notifications[position] = job;
        ++notification_count;
    }

    void remove_notification(JobHandle handle) noexcept {
        std::size_t output{};
        const auto previous_count = notification_count;
        for (std::size_t i = 0; i < previous_count; ++i) {
            const auto index = (notification_head + i) % notifications.size();
            if (notifications[index] && notifications[index]->handle == handle) {
                notifications[index].reset();
                continue;
            }
            const auto destination = (notification_head + output) % notifications.size();
            if (destination != index) {
                notifications[destination] = std::move(notifications[index]);
            }
            ++output;
        }
        notification_count = output;
    }

    void record_terminal(const std::shared_ptr<detail::JobControl>& job,
                         JobState result, Clock::time_point now) noexcept {
        if (job->state == JobState::Queued) {
            --queued;
        } else if (job->state == JobState::Running) {
            --running;
        }
        job->state = result;
        job->finished = now;
        --unfinished;
        switch (result) {
        case JobState::Succeeded:
            ++succeeded;
            job->progress.store(1.0, std::memory_order_relaxed);
            break;
        case JobState::Failed:
            ++failed;
            break;
        case JobState::Cancelled:
            ++cancelled;
            job->cancellation_requested.store(true, std::memory_order_release);
            break;
        default:
            break;
        }
        notify_completion(job);
        propagation.push_back(job);
    }

    // Every node reaches this transition once. Pre-reserved propagation storage
    // bounds graph failure fan-out without recursive calls or allocations.
    void finish(const std::shared_ptr<detail::JobControl>& job, JobState result,
                std::string error = {}, std::exception_ptr exception = {}) noexcept {
        if (terminal(job->state)) {
            return;
        }
        propagation.clear();
        job->error = std::move(error);
        job->exception = std::move(exception);
        const auto now = Clock::now();
        record_terminal(job, result, now);
        for (std::size_t position = 0; position < propagation.size(); ++position) {
            const auto completed = propagation[position];
            for (const auto child_handle : completed->dependents) {
                const auto child = find(child_handle);
                if (!child || terminal(child->state) || child->pending_dependencies == 0) {
                    continue;
                }
                --child->pending_dependencies;
                child->dependency_failed = child->dependency_failed ||
                    completed->state != JobState::Succeeded;
                if (child->pending_dependencies != 0) {
                    continue;
                }
                if (child->dependency_failed && !child->options.run_after_failed_dependency) {
                    record_terminal(child, JobState::Cancelled, now);
                } else {
                    child->state = JobState::Queued;
                    ++queued;
                }
            }
        }
        propagation.clear();
    }

    [[nodiscard]] std::shared_ptr<detail::JobControl> choose_ready() const {
        std::shared_ptr<detail::JobControl> best;
        unsigned best_priority{};
        const auto now = Clock::now();
        for (const auto& job : slots) {
            if (!job || job->state != JobState::Queued) {
                continue;
            }
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - job->submitted);
            const auto promotion = std::clamp<std::int64_t>(
                age.count() / options.aging_interval.count(), 0, 3);
            const auto effective = std::min(3U, static_cast<unsigned>(job->options.priority) +
                static_cast<unsigned>(promotion));
            if (!best || effective > best_priority ||
                (effective == best_priority && job->sequence < best->sequence)) {
                best = job;
                best_priority = effective;
            }
        }
        return best;
    }

    ExecutorOptions options;
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    ge::GenerationPool handles;
    std::vector<std::shared_ptr<detail::JobControl>> slots;
    std::map<std::uint64_t, GraphRecord> graphs;
    std::vector<std::shared_ptr<detail::JobControl>> notifications;
    std::vector<std::shared_ptr<detail::JobControl>> propagation;
    std::size_t notification_head{};
    std::size_t notification_count{};
    std::size_t queued{};
    std::size_t running{};
    std::size_t unfinished{};
    std::size_t high_watermark{};
    std::uint64_t sequence{};
    std::uint64_t next_graph{1};
    std::uint64_t submitted{};
    std::uint64_t succeeded{};
    std::uint64_t failed{};
    std::uint64_t cancelled{};
    std::uint64_t rejected{};
    std::uint64_t dropped_notifications{};
    bool accepting{true};
    bool paused{};
    bool closing{};
    bool joining{};
    bool joined{};
    std::thread::id join_owner;
    std::vector<std::jthread> workers;
};

Executor::Executor(ExecutorOptions options) {
    if (options.worker_count == 0 || options.worker_count > 64 || options.capacity == 0 ||
        options.capacity > 1048576 || options.graph_capacity > 65536 ||
        options.max_graph_edges > 16777216 || options.generation_limit == 0 ||
        options.aging_interval.count() <= 0) {
        throw std::invalid_argument("invalid bounded executor configuration");
    }
    impl_ = std::make_unique<Impl>(options);
    try {
        for (std::size_t i = 0; i < options.worker_count; ++i) {
            impl_->workers.emplace_back([implementation = impl_.get(), i](std::stop_token stop) {
                run_worker(implementation, stop, i);
            });
        }
    } catch (...) {
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->accepting = false;
            impl_->closing = true;
        }
        impl_->changed.notify_all();
        for (auto& worker : impl_->workers) {
            worker.request_stop();
        }
        for (auto& worker : impl_->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

Executor::~Executor() {
    // Destroying the owner from its own callback cannot preserve worker lifetime.
    if (!shutdown(ShutdownMode::Cancel)) {
        std::terminate();
    }
}

void Executor::run_worker(Impl* implementation, std::stop_token stop,
                          std::size_t worker_index) {
    while (!stop.stop_requested()) {
        std::shared_ptr<detail::JobControl> job;
        {
            std::unique_lock lock(implementation->mutex);
            implementation->changed.wait(lock, stop, [&] {
                return (implementation->closing && implementation->unfinished == 0) ||
                    (!implementation->paused && implementation->queued != 0);
            });
            if (stop.stop_requested() ||
                (implementation->closing && implementation->unfinished == 0)) {
                return;
            }
            job = implementation->choose_ready();
            if (!job) {
                continue;
            }
            job->state = JobState::Running;
            job->started = Clock::now();
            --implementation->queued;
            ++implementation->running;
        }
        JobState outcome = JobState::Succeeded;
        std::string error;
        std::exception_ptr exception;
        const auto* previous_executor = executing_executor;
        executing_executor = implementation;
        try {
            const JobContext context(job, worker_index);
            context.throw_if_stopped();
            job->function(context);
            context.throw_if_stopped();
        } catch (const JobCancelled&) {
            outcome = JobState::Cancelled;
        } catch (const std::exception& failure) {
            outcome = JobState::Failed;
            exception = std::current_exception();
            try {
                error = bounded_exception_message(failure);
            } catch (...) {
                error.clear();
            }
        } catch (...) {
            outcome = JobState::Failed;
            exception = std::current_exception();
            try {
                error = "non-standard job exception";
            } catch (...) {
                error.clear();
            }
        }
        executing_executor = previous_executor;
        {
            std::scoped_lock lock(implementation->mutex);
            if (outcome == JobState::Succeeded &&
                job->cancellation_requested.load(std::memory_order_acquire)) {
                outcome = JobState::Cancelled;
            }
            implementation->finish(job, outcome, std::move(error), std::move(exception));
        }
        implementation->changed.notify_all();
    }
}

std::optional<JobHandle> Executor::submit(JobFunction function, JobOptions options) {
    if (!function || !valid_options(options)) {
        std::scoped_lock lock(impl_->mutex);
        ++impl_->rejected;
        return std::nullopt;
    }
    auto job = std::make_shared<detail::JobControl>();
    job->function = std::move(function);
    job->options = std::move(options);
    if (job->options.name.empty()) {
        job->options.name = "job";
    }
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->accepting || impl_->handles.available_slots() == 0 ||
            impl_->sequence == std::numeric_limits<std::uint64_t>::max()) {
            ++impl_->rejected;
            return std::nullopt;
        }
        const auto handle = impl_->handles.acquire();
        if (!handle) {
            ++impl_->rejected;
            return std::nullopt;
        }
        job->handle = *handle;
        job->state = JobState::Queued;
        job->sequence = ++impl_->sequence;
        job->submitted = Clock::now();
        impl_->slots[static_cast<std::size_t>(handle->index - 1)] = job;
        ++impl_->queued;
        ++impl_->unfinished;
        ++impl_->submitted;
        impl_->high_watermark = std::max(impl_->high_watermark,
            static_cast<std::size_t>(impl_->handles.size()));
    }
    impl_->changed.notify_one();
    return job->handle;
}

std::optional<GraphSubmission> Executor::submit(const TaskGraph& graph) {
    const auto validation = graph.validate();
    if (!validation.valid || graph.size() > impl_->options.capacity ||
        graph.edge_count() > impl_->options.max_graph_edges) {
        std::scoped_lock lock(impl_->mutex);
        ++impl_->rejected;
        return std::nullopt;
    }
    std::vector<std::shared_ptr<detail::JobControl>> jobs;
    jobs.reserve(graph.size());
    std::vector<std::size_t> children(graph.size(), 0);
    for (const auto& node : graph.nodes_) {
        for (const auto prerequisite : node.prerequisites) {
            ++children[static_cast<std::size_t>(prerequisite - 1)];
        }
    }
    for (std::size_t i = 0; i < graph.size(); ++i) {
        auto job = std::make_shared<detail::JobControl>();
        job->function = graph.nodes_[i].function;
        job->options = graph.nodes_[i].options;
        job->pending_dependencies = graph.nodes_[i].prerequisites.size();
        job->dependents.reserve(children[i]);
        jobs.push_back(std::move(job));
    }
    GraphSubmission submission;
    submission.jobs.reserve(graph.size());
    Impl::GraphRecord record;
    record.jobs.reserve(graph.size());
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->accepting || graph.size() > impl_->handles.available_slots() ||
            impl_->graphs.size() >= impl_->options.graph_capacity ||
            impl_->next_graph == std::numeric_limits<std::uint64_t>::max() ||
            graph.size() > std::numeric_limits<std::uint64_t>::max() - impl_->sequence) {
            ++impl_->rejected;
            return std::nullopt;
        }
        submission.graph = GraphHandle{impl_->next_graph};
        record.handle = submission.graph;
        try {
            for (std::size_t i = 0; i < graph.size(); ++i) {
                const auto handle = impl_->handles.acquire();
                if (!handle) {
                    throw std::length_error("executor handle admission changed unexpectedly");
                }
                submission.jobs.push_back(*handle);
                record.jobs.push_back(*handle);
                jobs[i]->handle = *handle;
                jobs[i]->graph = submission.graph;
            }
            for (std::size_t i = 0; i < graph.size(); ++i) {
                for (const auto prerequisite : graph.nodes_[i].prerequisites) {
                    jobs[static_cast<std::size_t>(prerequisite - 1)]->dependents.push_back(jobs[i]->handle);
                }
            }
            impl_->graphs.emplace(record.handle.value, std::move(record));
        } catch (...) {
            for (const auto handle : submission.jobs) {
                (void)impl_->handles.release(handle);
            }
            throw;
        }
        const auto now = Clock::now();
        for (const auto& job : jobs) {
            job->submitted = now;
            job->sequence = ++impl_->sequence;
            if (job->pending_dependencies == 0) {
                job->state = JobState::Queued;
                ++impl_->queued;
            }
            impl_->slots[static_cast<std::size_t>(job->handle.index - 1)] = job;
        }
        ++impl_->next_graph;
        impl_->unfinished += graph.size();
        impl_->submitted += static_cast<std::uint64_t>(graph.size());
        impl_->high_watermark = std::max(impl_->high_watermark,
            static_cast<std::size_t>(impl_->handles.size()));
    }
    impl_->changed.notify_all();
    return submission;
}

std::optional<GraphSubmission> Executor::parallel_for(std::size_t count,
    std::size_t grain, RangeFunction function, JobOptions options) {
    TaskGraph graph(impl_->options.capacity, impl_->options.max_graph_edges);
    if (graph.parallel_for(count, grain, std::move(function), std::move(options)).empty()) {
        return std::nullopt;
    }
    return submit(graph);
}

bool Executor::cancel(JobHandle handle) {
    std::shared_ptr<detail::JobControl> job;
    {
        std::scoped_lock lock(impl_->mutex);
        job = impl_->find(handle);
        if (!job || terminal(job->state) ||
            job->cancellation_requested.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        if (job->state != JobState::Running) {
            impl_->finish(job, JobState::Cancelled);
        }
    }
    // A stop callback must not join a worker whose stop_callback destructor
    // is waiting for this callback to return. Restore the dispatch afterward.
    {
        const CancellationDispatch dispatch(impl_.get());
        (void)job->stop.request_stop();
    }
    impl_->changed.notify_all();
    return true;
}

std::size_t Executor::cancel(GraphHandle graph) {
    std::vector<JobHandle> jobs;
    {
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->graphs.find(graph.value);
        if (found == impl_->graphs.end()) {
            return 0;
        }
        jobs = found->second.jobs;
    }
    std::size_t count{};
    for (const auto handle : jobs) {
        if (cancel(handle)) {
            ++count;
        }
    }
    return count;
}

std::size_t Executor::cancel_tag(std::uint64_t tag) {
    std::vector<JobHandle> jobs;
    {
        std::scoped_lock lock(impl_->mutex);
        jobs.reserve(impl_->handles.size());
        for (const auto& job : impl_->slots) {
            if (job && job->options.tag == tag && !terminal(job->state)) {
                jobs.push_back(job->handle);
            }
        }
    }
    std::size_t count{};
    for (const auto handle : jobs) {
        if (cancel(handle)) {
            ++count;
        }
    }
    return count;
}

std::optional<JobSnapshot> Executor::inspect(JobHandle handle) const {
    std::scoped_lock lock(impl_->mutex);
    const auto job = impl_->find(handle);
    return job ? std::optional<JobSnapshot>(impl_->snapshot(*job)) : std::nullopt;
}

std::optional<GraphSnapshot> Executor::inspect(GraphHandle handle) const {
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->graphs.find(handle.value);
    return found == impl_->graphs.end() ? std::nullopt :
        std::optional<GraphSnapshot>(impl_->graph_snapshot(found->second));
}

std::vector<JobSnapshot> Executor::inspect_all(std::size_t limit) const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<JobSnapshot> result;
    result.reserve(std::min(limit, static_cast<std::size_t>(impl_->handles.size())));
    for (const auto& job : impl_->slots) {
        if (result.size() == limit) {
            break;
        }
        if (job) {
            result.push_back(impl_->snapshot(*job));
        }
    }
    return result;
}

std::vector<JobCompletion> Executor::poll(std::size_t limit) {
    std::scoped_lock lock(impl_->mutex);
    std::vector<JobCompletion> result;
    const auto count = std::min(limit, impl_->notification_count);
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& job = impl_->notifications[impl_->notification_head];
        // Build the copy before consuming its notification, so allocation failure
        // preserves the current item and the retained job remains queryable.
        result.push_back(impl_->completion(*job));
        impl_->notifications[impl_->notification_head].reset();
        impl_->notification_head = (impl_->notification_head + 1) % impl_->notifications.size();
        --impl_->notification_count;
    }
    return result;
}

bool Executor::release(JobHandle handle) {
    std::shared_ptr<detail::JobControl> released;
    {
        std::scoped_lock lock(impl_->mutex);
        const auto job = impl_->find(handle);
        if (!job || !terminal(job->state) || job->graph) {
            return false;
        }
        released = std::move(impl_->slots[static_cast<std::size_t>(handle.index - 1)]);
        impl_->remove_notification(handle);
        (void)impl_->handles.release(handle);
    }
    // Callable captures and exception objects retire after unlocking.
    released.reset();
    impl_->changed.notify_all();
    return true;
}

bool Executor::release(GraphHandle handle) {
    std::vector<std::shared_ptr<detail::JobControl>> released;
    {
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->graphs.find(handle.value);
        if (found == impl_->graphs.end() || !impl_->graph_snapshot(found->second).finished()) {
            return false;
        }
        released.reserve(found->second.jobs.size());
        for (const auto job_handle : found->second.jobs) {
            released.push_back(std::move(impl_->slots[static_cast<std::size_t>(job_handle.index - 1)]));
            impl_->remove_notification(job_handle);
            (void)impl_->handles.release(job_handle);
        }
        impl_->graphs.erase(found);
    }
    released.clear();
    impl_->changed.notify_all();
    return true;
}

std::size_t Executor::release_completed(std::uint64_t tag) {
    std::vector<JobHandle> jobs;
    {
        std::scoped_lock lock(impl_->mutex);
        jobs.reserve(impl_->handles.size());
        for (const auto& job : impl_->slots) {
            if (job && !job->graph && job->options.tag == tag && terminal(job->state)) {
                jobs.push_back(job->handle);
            }
        }
    }
    std::size_t count{};
    for (const auto handle : jobs) {
        if (release(handle)) {
            ++count;
        }
    }
    return count;
}

bool Executor::wait(JobHandle handle, std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    const auto initial = impl_->find(handle);
    if (!initial || timeout.count() < 0) {
        return false;
    }
    if (terminal(initial->state)) {
        return true;
    }
    if (executing_executor == impl_.get()) {
        return false;
    }
    const bool finished = impl_->changed.wait_for(lock, timeout, [&] {
        const auto job = impl_->find(handle);
        return !job || terminal(job->state);
    });
    const auto job = impl_->find(handle);
    return finished && job && terminal(job->state);
}

bool Executor::wait(GraphHandle handle, std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    auto finished = [&] {
        const auto found = impl_->graphs.find(handle.value);
        return found != impl_->graphs.end() && impl_->graph_snapshot(found->second).finished();
    };
    if (!impl_->graphs.contains(handle.value) || timeout.count() < 0) {
        return false;
    }
    if (finished()) {
        return true;
    }
    if (executing_executor == impl_.get()) {
        return false;
    }
    const bool observed = impl_->changed.wait_for(lock, timeout, [&] {
        return !impl_->graphs.contains(handle.value) || finished();
    });
    return observed && finished();
}

bool Executor::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    if (impl_->unfinished == 0) {
        return true;
    }
    if (executing_executor == impl_.get() || timeout.count() < 0) {
        return false;
    }
    return impl_->changed.wait_for(lock, timeout, [&] { return impl_->unfinished == 0; });
}

ExecutorStatistics Executor::statistics() const {
    std::scoped_lock lock(impl_->mutex);
    ExecutorStatistics result;
    result.workers = impl_->options.worker_count;
    result.capacity = impl_->options.capacity;
    result.retained = impl_->handles.size();
    result.queued = impl_->queued;
    result.running = impl_->running;
    result.waiting = impl_->unfinished - impl_->queued - impl_->running;
    result.terminal_jobs = result.retained - impl_->unfinished;
    result.graph_count = impl_->graphs.size();
    result.high_watermark = impl_->high_watermark;
    result.pending_notifications = impl_->notification_count;
    result.submitted = impl_->submitted;
    result.succeeded = impl_->succeeded;
    result.failed = impl_->failed;
    result.cancelled = impl_->cancelled;
    result.rejected = impl_->rejected;
    result.dropped_notifications = impl_->dropped_notifications;
    result.accepting = impl_->accepting;
    result.paused = impl_->paused;
    return result;
}

bool Executor::pause() {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->accepting) {
        return false;
    }
    impl_->paused = true;
    return true;
}

bool Executor::resume() {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->accepting) {
            return false;
        }
        impl_->paused = false;
    }
    impl_->changed.notify_all();
    return true;
}

bool Executor::shutdown(ShutdownMode mode) {
    if (!impl_) {
        return true;
    }
    if (executing_executor == impl_.get() || CancellationDispatch::contains(impl_.get())) {
        return false;
    }
    {
        std::unique_lock lock(impl_->mutex);
        if (impl_->joined) {
            return true;
        }
        if (impl_->joining) {
            if (impl_->join_owner == std::this_thread::get_id()) {
                return false;
            }
            impl_->changed.wait(lock, [&] { return impl_->joined; });
            return true;
        }
        impl_->joining = true;
        impl_->join_owner = std::this_thread::get_id();
        impl_->accepting = false;
        impl_->closing = true;
        impl_->paused = false;
        if (mode == ShutdownMode::Cancel) {
            for (const auto& job : impl_->slots) {
                if (!job || terminal(job->state)) {
                    continue;
                }
                job->cancellation_requested.store(true, std::memory_order_release);
                if (job->state != JobState::Running) {
                    impl_->finish(job, JobState::Cancelled);
                }
            }
        }
    }
    if (mode == ShutdownMode::Cancel) {
        // Take one shared owner at a time so shutdown needs no allocation and
        // caller stop callbacks execute without either state or join locks held.
        const CancellationDispatch dispatch(impl_.get());
        for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
            std::shared_ptr<detail::JobControl> job;
            {
                std::scoped_lock lock(impl_->mutex);
                job = impl_->slots[i];
            }
            if (job && job->cancellation_requested.load(std::memory_order_acquire)) {
                (void)job->stop.request_stop();
            }
        }
    }
    impl_->changed.notify_all();
    for (auto& worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->joined = true;
        impl_->joining = false;
    }
    impl_->changed.notify_all();
    return true;
}

namespace {

std::string format_statistics(const ExecutorStatistics& statistics) {
    std::ostringstream output;
    output << "workers=" << statistics.workers
           << " retained=" << statistics.retained << '/' << statistics.capacity
           << " queued=" << statistics.queued
           << " waiting=" << statistics.waiting
           << " running=" << statistics.running
           << " terminal=" << statistics.terminal_jobs
           << " graphs=" << statistics.graph_count
           << " succeeded=" << statistics.succeeded
           << " failed=" << statistics.failed
           << " cancelled=" << statistics.cancelled
           << " rejected=" << statistics.rejected
           << " accepting=" << (statistics.accepting ? "yes" : "no")
           << " paused=" << (statistics.paused ? "yes" : "no");
    return output.str();
}

std::string format_snapshot(const JobSnapshot& snapshot) {
    std::ostringstream output;
    output << snapshot.handle.index << ':' << snapshot.handle.generation
           << ' ' << state_name(snapshot.state)
           << " name=" << snapshot.name
           << " tag=" << snapshot.tag
           << " progress=" << snapshot.progress
           << " queued_seconds=" << snapshot.queue_seconds
           << " execution_seconds=" << snapshot.execution_seconds
           << " dependencies=" << snapshot.pending_dependencies;
    if (!snapshot.error.empty()) {
        output << " error=" << snapshot.error;
    }
    return output.str();
}

std::optional<JobHandle> parse_handle(core::CommandArgs args) {
    if (args.size() != 2) {
        return std::nullopt;
    }
    JobHandle result;
    if (!parse_integer(args[0], result.index) || !parse_integer(args[1], result.generation) ||
        !result.valid()) {
        return std::nullopt;
    }
    return result;
}

}

void register_executor(ge::core::Kernel& kernel) {
    if (kernel.services().contains<Executor>()) {
        return;
    }
    const auto hardware = std::thread::hardware_concurrency();
    const auto default_workers = std::clamp<std::int64_t>(
        hardware == 0 ? 2 : static_cast<std::int64_t>(hardware) / 2, 1, 8);
    ExecutorOptions options;
    options.worker_count = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("jobs.workers", default_workers), 1, 64));
    options.capacity = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        kernel.config().integer("jobs.capacity", 4096), 1, 1048576));
    options.graph_capacity = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("jobs.graph_capacity", 128), 0, 65536));
    options.aging_interval = std::chrono::milliseconds(std::clamp<std::int64_t>(
        kernel.config().integer("jobs.aging_ms", 100), 1, 60000));
    auto& executor = kernel.services().emplace<Executor>(options);
    if (!kernel.register_module({"jobs.executor",
            "Bounded cooperative workers, dependency graphs and retained completion ownership", 1})) {
        (void)executor.shutdown();
        throw std::logic_error("jobs module registration conflict");
    }
    if (!kernel.add_shutdown("jobs.executor", [service = &executor] {
            (void)service->shutdown(ShutdownMode::Cancel);
        })) {
        (void)executor.shutdown();
        throw std::logic_error("jobs shutdown registration conflict");
    }
    kernel.add_system({"jobs.completions", core::Phase::Input, -1000, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& service = host.services().get<Executor>();
            const auto configured = std::clamp<std::int64_t>(
                host.config().integer("jobs.completions_per_frame", 256), 1, 65536);
            auto completions = service.poll(static_cast<std::size_t>(configured));
            for (auto& completion : completions) {
                if (completion.state == JobState::Failed) {
                    host.diagnose(core::Diagnostic::Severity::Error, "jobs",
                        completion.name + ": " + (completion.error.empty()
                            ? "job threw an exception" : completion.error));
                }
                (void)host.events().publish(JobCompletedEvent{std::move(completion)});
            }
            const auto statistics = service.statistics();
            host.metric("jobs.queued", static_cast<double>(statistics.queued));
            host.metric("jobs.waiting", static_cast<double>(statistics.waiting));
            host.metric("jobs.running", static_cast<double>(statistics.running));
            host.metric("jobs.retained", static_cast<double>(statistics.retained));
            host.metric("jobs.succeeded", static_cast<double>(statistics.succeeded));
            host.metric("jobs.failed", static_cast<double>(statistics.failed));
            host.metric("jobs.cancelled", static_cast<double>(statistics.cancelled));
            host.metric("jobs.rejected", static_cast<double>(statistics.rejected));
            host.metric("jobs.high_watermark", static_cast<double>(statistics.high_watermark));
        });
    kernel.add_command("jobs.stats", [](core::Kernel& host, core::CommandArgs) {
        return core::CommandResult{true, format_statistics(host.services().get<Executor>().statistics())};
    }, "Show worker, admission, ownership and completion counters");
    kernel.add_command("jobs.list", [](core::Kernel& host, core::CommandArgs args) {
        std::size_t limit{128};
        if (args.size() > 1 || (!args.empty() && !parse_integer(args[0], limit)) || limit > 4096) {
            return core::CommandResult{false, "usage: jobs.list [limit:0..4096]"};
        }
        std::ostringstream output;
        for (const auto& snapshot : host.services().get<Executor>().inspect_all(limit)) {
            output << format_snapshot(snapshot) << '\n';
        }
        return core::CommandResult{true, output.str()};
    }, "List retained jobs with full generation handles");
    kernel.add_command("jobs.inspect", [](core::Kernel& host, core::CommandArgs args) {
        const auto handle = parse_handle(args);
        if (!handle) {
            return core::CommandResult{false, "usage: jobs.inspect index generation"};
        }
        const auto snapshot = host.services().get<Executor>().inspect(*handle);
        return core::CommandResult{snapshot.has_value(), snapshot ? format_snapshot(*snapshot) : "stale job handle"};
    }, "Inspect one current generation of a job");
    kernel.add_command("jobs.cancel", [](core::Kernel& host, core::CommandArgs args) {
        const auto handle = parse_handle(args);
        if (!handle) {
            return core::CommandResult{false, "usage: jobs.cancel index generation"};
        }
        const bool accepted = host.services().get<Executor>().cancel(*handle);
        return core::CommandResult{accepted, accepted ? "cancellation requested" : "stale, terminal or already cancelling job"};
    }, "Request cooperative cancellation without releasing ownership");
    kernel.add_command("jobs.release", [](core::Kernel& host, core::CommandArgs args) {
        const auto handle = parse_handle(args);
        if (!handle) {
            return core::CommandResult{false, "usage: jobs.release index generation"};
        }
        const bool released = host.services().get<Executor>().release(*handle);
        return core::CommandResult{released, released ? "released" : "job is stale, active or owned by a graph"};
    }, "Release a terminal standalone job after consuming its result");
    kernel.add_command("jobs.cancel_tag", [](core::Kernel& host, core::CommandArgs args) {
        std::uint64_t tag{};
        if (args.size() != 1 || !parse_integer(args[0], tag)) {
            return core::CommandResult{false, "usage: jobs.cancel_tag tag"};
        }
        return core::CommandResult{true, std::to_string(host.services().get<Executor>().cancel_tag(tag))};
    }, "Cancel pending/running jobs for one caller-defined ownership tag");
    kernel.add_command("jobs.release_tag", [](core::Kernel& host, core::CommandArgs args) {
        std::uint64_t tag{};
        if (args.size() != 1 || !parse_integer(args[0], tag)) {
            return core::CommandResult{false, "usage: jobs.release_tag tag"};
        }
        return core::CommandResult{true, std::to_string(host.services().get<Executor>().release_completed(tag))};
    }, "Release terminal standalone jobs for an ownership tag");
    kernel.add_command("jobs.graph", [](core::Kernel& host, core::CommandArgs args) {
        GraphHandle handle;
        if (args.size() != 1 || !parse_integer(args[0], handle.value) || !handle) {
            return core::CommandResult{false, "usage: jobs.graph graph_id"};
        }
        const auto snapshot = host.services().get<Executor>().inspect(handle);
        if (!snapshot) {
            return core::CommandResult{false, "unknown graph"};
        }
        std::ostringstream output;
        output << "total=" << snapshot->total << " queued=" << snapshot->queued
               << " waiting=" << snapshot->waiting << " running=" << snapshot->running
               << " succeeded=" << snapshot->succeeded << " failed=" << snapshot->failed
               << " cancelled=" << snapshot->cancelled;
        return core::CommandResult{true, output.str()};
    }, "Inspect aggregate graph progress");
    kernel.add_command("jobs.cancel_graph", [](core::Kernel& host, core::CommandArgs args) {
        GraphHandle handle;
        if (args.size() != 1 || !parse_integer(args[0], handle.value) || !handle) {
            return core::CommandResult{false, "usage: jobs.cancel_graph graph_id"};
        }
        return core::CommandResult{true, std::to_string(host.services().get<Executor>().cancel(handle))};
    }, "Cancel every unfinished graph node");
    kernel.add_command("jobs.release_graph", [](core::Kernel& host, core::CommandArgs args) {
        GraphHandle handle;
        if (args.size() != 1 || !parse_integer(args[0], handle.value) || !handle) {
            return core::CommandResult{false, "usage: jobs.release_graph graph_id"};
        }
        const bool released = host.services().get<Executor>().release(handle);
        return core::CommandResult{released, released ? "released" : "graph is unknown or still active"};
    }, "Release all terminal nodes and graph ownership together");
    kernel.add_command("jobs.pause", [](core::Kernel& host, core::CommandArgs) {
        const bool paused = host.services().get<Executor>().pause();
        return core::CommandResult{paused, paused ? "dispatch paused; active jobs continue" : "executor is closing"};
    }, "Pause dispatch while preserving accepted jobs and dependencies");
    kernel.add_command("jobs.resume", [](core::Kernel& host, core::CommandArgs) {
        const bool resumed = host.services().get<Executor>().resume();
        return core::CommandResult{resumed, resumed ? "dispatch resumed" : "executor is closing"};
    }, "Resume paused job dispatch");
}

} // namespace ge::jobs
