#include <ge/assets/streaming.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <utility>

namespace ge::assets {
namespace {
using Clock = std::chrono::steady_clock;

[[noreturn]] void stream_error(AssetError code, std::string message) {
    throw AssetException(code, std::move(message));
}

std::uint64_t next_identity(std::uint64_t& counter) {
    if (counter == 0) {
        stream_error(AssetError::exhausted, "streaming identity space exhausted");
    }
    const auto result = counter;
    counter = counter == std::numeric_limits<std::uint64_t>::max() ? 0 : counter + 1;
    return result;
}

void count_event(std::uint64_t& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) {
        ++counter;
    }
}

bool fits(std::size_t used, std::size_t additional, std::size_t limit) noexcept {
    return used <= limit && additional <= limit - used;
}

bool valid_priority(jobs::Priority priority) noexcept {
    return static_cast<unsigned>(priority) <= static_cast<unsigned>(jobs::Priority::High);
}

std::string diagnostic(const char* message, std::size_t maximum) {
    if (message == nullptr) {
        return "stream operation failed without a diagnostic";
    }
    std::size_t length{};
    while (length < maximum && message[length] != '\0') {
        ++length;
    }
    return std::string(message, length);
}

std::uint64_t number(std::string_view value) {
    std::uint64_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        stream_error(AssetError::invalid_argument, "expected an unsigned decimal integer");
    }
    return result;
}

std::chrono::milliseconds backoff(const StreamLimits& limits, std::uint32_t attempt) {
    std::uint64_t duration = limits.retry_initial_ms;
    for (std::uint32_t i = 1; i < attempt && duration < limits.retry_maximum_ms; ++i) {
        duration = std::min<std::uint64_t>(duration * 2, limits.retry_maximum_ms);
    }
    return std::chrono::milliseconds(static_cast<std::int64_t>(duration));
}
} // namespace

std::string_view stream_state_name(StreamState state) noexcept {
    switch (state) {
    case StreamState::queued: return "queued";
    case StreamState::waiting_dependencies: return "waiting_dependencies";
    case StreamState::waiting_budget: return "waiting_budget";
    case StreamState::loading: return "loading";
    case StreamState::ready: return "ready";
    case StreamState::retrying: return "retrying";
    case StreamState::failed: return "failed";
    case StreamState::cancelled: return "cancelled";
    }
    return "unknown";
}

struct AssetStreamer::Impl {
    struct Version {
        ResourceHandle handle;
        AssetId asset;
        AssetRevision revision;
        std::vector<std::shared_ptr<Version>> dependencies;
    };

    struct Request {
        StreamTicket ticket;
        StreamRequestOptions options;
        std::vector<AssetId> closure;
        std::string error;
        bool cancelled{};
    };

    struct Node {
        AssetId asset;
        AssetRevision revision;
        AssetTypeId type{};
        std::uint64_t generation{};
        std::size_t demand{};
        std::size_t wanted{};
        std::uint32_t attempts{};
        std::uint32_t admission_waits{};
        std::uint32_t allowed_attempts{1};
        jobs::Priority priority{jobs::Priority::Background};
        StreamState state{StreamState::queued};
        Clock::time_point queued_at{Clock::now()};
        Clock::time_point retry_at{};
        std::shared_ptr<Version> retained;
        std::string error;
        bool active{};
        bool exhausted{};
    };

    struct WorkerResult {
        ImportRequest request;
        std::mutex mutex;
        std::optional<ImportProduct> product;
    };

    struct Flight {
        std::shared_ptr<WorkerResult> result;
        ResourceAdmission admission;
        jobs::JobHandle job;
        std::uint64_t node_generation{};
        std::size_t source_charge{};
        std::size_t decoded_charge{};
        std::shared_ptr<Version> previous;
        std::vector<std::shared_ptr<Version>> dependencies;
        bool cancelled{};
    };

    struct Pin {
        StreamPin ticket;
        std::shared_ptr<Version> version;
    };

    Impl(jobs::Executor& jobs_service, io::VirtualFileSystem& file_service,
         AssetCatalog& catalog_service, ResourceCache& cache_service, StreamLimits requested)
        : executor(jobs_service), filesystem(file_service), catalog(catalog_service),
          cache(cache_service), limits(requested), owner(std::this_thread::get_id()) {
        if (limits.max_requests == 0 || limits.max_assets == 0 || limits.max_consumers == 0 ||
            limits.max_pins == 0 || limits.max_versions == 0 || limits.max_jobs == 0 ||
            limits.max_dependency_references == 0 || limits.max_source_bytes == 0 ||
            limits.max_decoded_bytes == 0 || limits.max_inflight_bytes == 0 ||
            limits.dispatches_per_pump == 0 || limits.completions_per_pump == 0 ||
            limits.changes_per_poll == 0 || limits.maximum_attempts == 0 ||
            limits.maximum_attempts > 1024 || limits.retry_initial_ms == 0 ||
            limits.retry_initial_ms > limits.retry_maximum_ms || limits.priority_aging_ms == 0 ||
            limits.max_error_bytes < 64 || limits.max_error_bytes > 65536 ||
            limits.max_jobs > limits.max_assets || limits.max_versions < limits.max_jobs) {
            stream_error(AssetError::invalid_argument, "invalid streaming limits");
        }
        flights.reserve(limits.max_jobs);
    }

    void require_owner() const {
        if (std::this_thread::get_id() != owner) {
            stream_error(AssetError::invalid_argument, "streaming operation requires its owner thread");
        }
    }

    void require_open() const {
        require_owner();
        if (!accepting) {
            stream_error(AssetError::busy, "asset streamer is shut down");
        }
    }

    bool valid_consumer(StreamConsumer consumer) const {
        const auto found = consumers.find(consumer.id);
        return consumer && found != consumers.end() && found->second == consumer.epoch;
    }

    Request* locate(StreamTicket ticket) {
        const auto found = requests.find(ticket.generation);
        return found != requests.end() && found->second.ticket == ticket &&
            valid_consumer(ticket.consumer) ? &found->second : nullptr;
    }

    const Request* locate(StreamTicket ticket) const {
        const auto found = requests.find(ticket.generation);
        return found != requests.end() && found->second.ticket == ticket &&
            valid_consumer(ticket.consumer) ? &found->second : nullptr;
    }

    void collect_versions() {
        std::vector<ResourceHandle> unused;
        unused.reserve(versions.size());
        for (const auto& [handle, version] : versions) {
            if (version.use_count() == 1) {
                unused.push_back(handle);
            }
        }
        while (!unused.empty()) {
            const auto handle = unused.back();
            unused.pop_back();
            const auto found = versions.find(handle);
            if (found == versions.end() || found->second.use_count() != 1) {
                continue;
            }
            auto version = std::move(found->second);
            versions.erase(found);
            (void)cache.unpin(handle);
            for (auto& dependency : version->dependencies) {
                const auto child = dependency->handle;
                dependency.reset();
                const auto held = versions.find(child);
                if (held != versions.end() && held->second.use_count() == 1) {
                    unused.push_back(child);
                }
            }
        }
    }

    std::shared_ptr<Version> hold(ResourceHandle handle, AssetId asset, AssetRevision revision,
                                  std::vector<std::shared_ptr<Version>> dependencies) {
        const auto found = versions.find(handle);
        if (found != versions.end()) {
            return found->second;
        }
        collect_versions();
        if (versions.size() >= limits.max_versions) {
            stream_error(AssetError::limit_exceeded, "retained streaming version limit reached");
        }
        auto version = std::make_shared<Version>();
        version->handle = handle;
        version->asset = asset;
        version->revision = revision;
        version->dependencies = std::move(dependencies);
        if (!cache.pin(handle)) {
            stream_error(AssetError::pinned, "resource could not be retained for streaming");
        }
        try {
            versions.emplace(handle, version);
        } catch (...) {
            (void)cache.unpin(handle);
            throw;
        }
        return version;
    }

    void abandon(Node& node) {
        for (const auto& flight : flights) {
            if (flight->result->request.ticket.asset == node.asset && !flight->cancelled) {
                flight->cancelled = true;
                (void)executor.cancel(flight->job);
                count_event(counters.cancelled);
            }
        }
    }

    void prune_nodes() {
        for (auto it = nodes.begin(); it != nodes.end();) {
            auto& node = it->second;
            if (node.demand == 0) {
                abandon(node);
                node.retained.reset();
                if (!node.active) {
                    it = nodes.erase(it);
                    continue;
                }
            }
            ++it;
        }
        collect_versions();
    }

    void set_closure(Request& request, std::vector<AssetId> closure) {
        const auto remaining = dependency_references - request.closure.size();
        if (!fits(remaining, closure.size(), limits.max_dependency_references)) {
            stream_error(AssetError::limit_exceeded, "streaming dependency reference limit reached");
        }
        std::size_t additions{};
        for (const auto asset : closure) {
            additions += nodes.contains(asset) ? 0U : 1U;
        }
        if (!fits(nodes.size(), additions, limits.max_assets)) {
            stream_error(AssetError::limit_exceeded, "streaming asset tracking limit reached");
        }
        for (const auto asset : closure) {
            if (!nodes.contains(asset)) {
                Node node;
                node.asset = asset;
                node.generation = next_identity(next_node);
                nodes.emplace(asset, std::move(node));
            }
        }
        request.closure = std::move(closure);
        dependency_references = remaining + request.closure.size();
        request.error.clear();
    }

    void reconcile() {
        if (catalog_dirty) {
            for (auto& [generation, request] : requests) {
                (void)generation;
                if (!request.cancelled) {
                    try {
                        set_closure(request, catalog.dependency_order(request.ticket.asset));
                    } catch (const std::exception& error) {
                        request.error = diagnostic(error.what(), limits.max_error_bytes);
                    }
                }
            }
            catalog_dirty = false;
        }
        for (auto& [asset, node] : nodes) {
            (void)asset;
            node.demand = 0;
            node.wanted = 0;
            node.priority = jobs::Priority::Background;
            node.allowed_attempts = 1;
        }
        for (const auto& [generation, request] : requests) {
            (void)generation;
            if (request.cancelled || !valid_consumer(request.ticket.consumer)) {
                continue;
            }
            for (const auto asset : request.closure) {
                auto& node = nodes.at(asset);
                ++node.demand;
                if (request.error.empty()) {
                    ++node.wanted;
                    node.priority = std::max(node.priority, request.options.priority);
                    node.allowed_attempts = std::max(node.allowed_attempts, request.options.attempts);
                }
            }
        }
        for (auto& [asset, node] : nodes) {
            if (node.demand == 0) {
                continue;
            }
            const auto record = catalog.inspect(asset);
            if (!record) {
                abandon(node);
                node.wanted = 0;
                node.state = StreamState::failed;
                node.error = "asset was removed from the catalog";
                continue;
            }
            if (record->revision != node.revision || record->definition.type != node.type) {
                abandon(node);
                node.generation = next_identity(next_node);
                node.revision = record->revision;
                node.type = record->definition.type;
                reset_failure(node);
            }
            if (node.wanted == 0) {
                abandon(node);
            }
            if (!node.active && record->last_good_revision &&
                (!node.retained || node.retained->revision != record->last_good_revision ||
                 node.retained->handle.type != record->definition.type)) {
                const auto handle = cache.find(asset, record->last_good_revision, record->definition.type);
                if (handle) {
                    const auto known = versions.find(*handle);
                    std::vector<std::shared_ptr<Version>> dependencies;
                    bool complete = record->last_good_revision == record->revision ||
                        record->definition.dependencies.empty();
                    for (const auto dependency : record->definition.dependencies) {
                        const auto child = nodes.find(dependency);
                        const auto metadata = catalog.inspect(dependency);
                        if (child == nodes.end() || !metadata || !child->second.retained ||
                            child->second.retained->revision != metadata->revision ||
                            metadata->status != ImportStatus::ready ||
                            metadata->last_good_revision != metadata->revision) {
                            complete = false;
                            break;
                        }
                        dependencies.push_back(child->second.retained);
                    }
                    try {
                        if (known != versions.end()) {
                            node.retained = known->second;
                        } else if (complete) {
                            node.retained = hold(*handle, asset, record->last_good_revision, std::move(dependencies));
                        }
                    } catch (const AssetException& error) {
                        if (!node.exhausted) {
                            fail(node, diagnostic(error.what(), limits.max_error_bytes), false, Clock::now());
                        }
                    }
                }
            }
            if (!node.active && node.retained && node.retained->revision == node.revision &&
                record->status == ImportStatus::ready && record->last_good_revision == node.revision) {
                node.state = StreamState::ready;
                node.error.clear();
                node.exhausted = false;
            } else if (!node.active && node.state == StreamState::ready) {
                node.state = StreamState::queued;
            }
        }
        prune_nodes();
    }

    void reset_failure(Node& node) {
        node.attempts = 0;
        node.admission_waits = 0;
        node.exhausted = false;
        node.state = StreamState::queued;
        node.error.clear();
        node.retry_at = {};
        node.queued_at = Clock::now();
    }

    void fail(Node& node, std::string message, bool retryable, Clock::time_point now) {
        node.error = std::move(message);
        count_event(counters.failures);
        if (retryable && node.attempts < node.allowed_attempts) {
            node.state = StreamState::retrying;
            node.retry_at = now + backoff(limits, std::max(1U, node.attempts));
            count_event(counters.retries);
        } else {
            node.state = StreamState::failed;
            node.exhausted = true;
        }
    }

    void denied(Node& node, std::string message, Clock::time_point now, bool permanent = false) {
        count_event(counters.admission_denials);
        ++node.admission_waits;
        if (permanent || node.admission_waits >= node.allowed_attempts) {
            fail(node, std::move(message), false, now);
        } else {
            node.error = std::move(message);
            node.state = StreamState::waiting_budget;
            node.retry_at = now + backoff(limits, node.admission_waits);
        }
    }

    bool dependencies_ready(Node& node, const AssetRecord& record,
                            std::vector<std::shared_ptr<Version>>& result) {
        result.reserve(record.definition.dependencies.size());
        for (const auto asset : record.definition.dependencies) {
            const auto found = nodes.find(asset);
            const auto metadata = catalog.inspect(asset);
            if (found == nodes.end() || !metadata || !found->second.retained ||
                found->second.retained->revision != metadata->revision ||
                metadata->status != ImportStatus::ready || metadata->last_good_revision != metadata->revision) {
                const bool failed = found != nodes.end() && found->second.state == StreamState::failed;
                node.state = failed ? StreamState::failed : StreamState::waiting_dependencies;
                node.error = failed ? "dependency " + std::to_string(asset.value) + " failed" : std::string{};
                return false;
            }
            result.push_back(found->second.retained);
        }
        return true;
    }

    bool transition_fits(const Flight& flight) const {
        const auto budget = cache.statistics().budget_bytes;
        std::size_t required = flight.decoded_charge;
        if (required > budget) {
            return false;
        }
        std::set<ResourceHandle> visited;
        std::vector<std::shared_ptr<Version>> pending = flight.dependencies;
        if (flight.previous) {
            pending.push_back(flight.previous);
        }
        while (!pending.empty()) {
            auto version = std::move(pending.back());
            pending.pop_back();
            if (!visited.insert(version->handle).second) {
                continue;
            }
            const auto info = cache.inspect(version->handle);
            if (!info || !fits(required, info->reserved_bytes, budget)) {
                return false;
            }
            required += info->reserved_bytes;
            for (const auto& dependency : version->dependencies) {
                pending.push_back(dependency);
            }
        }
        return true;
    }

    void discard_admission(Flight& flight) {
        if (flight.admission.ticket.handle && !flight.admission.already_resident) {
            (void)cache.cancel(flight.admission.ticket);
            (void)cache.acknowledge_retirement(flight.admission.ticket);
        }
    }

    void report_import_failure(const ImportTicket& ticket, std::string_view message) noexcept {
        try {
            (void)catalog.fail_import(ticket, std::string(message));
        } catch (...) {
            // Retirement must still finish if catalog history or allocation fails.
        }
    }

    bool launch(Node& node, const AssetRecord& record, Clock::time_point now) {
        auto flight = std::make_unique<Flight>();
        if (!dependencies_ready(node, record, flight->dependencies)) {
            return false;
        }
        flight->previous = node.retained;
        flight->node_generation = node.generation;
        const auto descriptors = catalog.types();
        const auto descriptor = std::find_if(descriptors.begin(), descriptors.end(),
            [&](const AssetTypeDescriptor& type) { return type.id == node.type; });
        if (descriptor == descriptors.end()) {
            fail(node, "asset type is no longer registered", false, now);
            return false;
        }
        const auto hash_scratch = std::min<std::size_t>(filesystem.limits().max_read_bytes, 65536);
        if (!fits(descriptor->maximum_source_bytes, hash_scratch, limits.max_source_bytes) ||
            descriptor->maximum_resource_bytes > limits.max_decoded_bytes ||
            !fits(descriptor->maximum_source_bytes + hash_scratch,
                  descriptor->maximum_resource_bytes, limits.max_inflight_bytes)) {
            denied(node, "asset type bounds exceed stream budgets; increase limits or lower type bounds", now, true);
            return false;
        }
        flight->source_charge = descriptor->maximum_source_bytes + hash_scratch;
        flight->decoded_charge = descriptor->maximum_resource_bytes;
        const auto additional = flight->source_charge + flight->decoded_charge;
        if (!fits(source_reserved, flight->source_charge, limits.max_source_bytes) ||
            !fits(decoded_reserved, flight->decoded_charge, limits.max_decoded_bytes) ||
            !fits(source_reserved + decoded_reserved, additional, limits.max_inflight_bytes)) {
            denied(node, "in-flight byte budget is occupied; release demand or retry after jobs retire", now);
            return false;
        }
        if (!transition_fits(*flight)) {
            denied(node, "last good version, dependencies and replacement cannot coexist in cache budget", now, true);
            return false;
        }
        collect_versions();
        if (versions.size() >= limits.max_versions ||
            flights.size() >= limits.max_versions - versions.size()) {
            denied(node, "retained version capacity is occupied; release pins or requests", now);
            return false;
        }
        flight->result = std::make_shared<WorkerResult>();
        try {
            flight->result->request = catalog.begin_import(node.asset);
            flight->admission = cache.admit(node.asset, node.revision, node.type, flight->decoded_charge);
            jobs::JobOptions options;
            options.name = "stream.asset." + std::to_string(node.asset.value);
            options.priority = node.priority;
            options.tag = 0x73747265616dULL;
            const auto shared = flight->result;
            auto* const files = &filesystem;
            const auto handle = executor.submit([shared, files](const jobs::JobContext& context) {
                context.throw_if_stopped();
                auto product = import_source(*files, shared->request);
                context.throw_if_stopped();
                std::scoped_lock lock(shared->mutex);
                shared->product.emplace(std::move(product));
            }, std::move(options));
            if (!handle) {
                discard_admission(*flight);
                report_import_failure(shared->request.ticket, "executor declined streaming admission");
                denied(node, "executor capacity is occupied or admission is stopped", now);
                return false;
            }
            flight->job = *handle;
            source_reserved += flight->source_charge;
            decoded_reserved += flight->decoded_charge;
            counters.peak_inflight_bytes = std::max(counters.peak_inflight_bytes,
                                                    source_reserved + decoded_reserved);
            // Constructor-reserved capacity makes ownership transfer non-allocating after submit.
            flights.push_back(std::move(flight));
            node.active = true;
            node.state = StreamState::loading;
            node.error.clear();
            node.admission_waits = 0;
            ++node.attempts;
            count_event(counters.submitted);
            return true;
        } catch (const AssetException& error) {
            discard_admission(*flight);
            report_import_failure(flight->result->request.ticket, diagnostic(error.what(), limits.max_error_bytes));
            const bool temporary = error.code() == AssetError::busy || error.code() == AssetError::budget_exceeded;
            denied(node, diagnostic(error.what(), limits.max_error_bytes), now, !temporary);
        } catch (const std::exception& error) {
            discard_admission(*flight);
            report_import_failure(flight->result->request.ticket, diagnostic(error.what(), limits.max_error_bytes));
            fail(node, diagnostic(error.what(), limits.max_error_bytes), false, now);
        }
        return false;
    }

    void retire(Flight& flight) {
        {
            std::scoped_lock lock(flight.result->mutex);
            flight.result->product.reset();
        }
        discard_admission(flight);
        (void)executor.release(flight.job);
        flight.dependencies.clear();
        flight.previous.reset();
        source_reserved -= flight.source_charge;
        decoded_reserved -= flight.decoded_charge;
    }

    void complete(Flight& flight, const std::optional<jobs::JobSnapshot>& job,
                  StreamPumpResult& result, Clock::time_point now) {
        const auto ticket = flight.result->request.ticket;
        const auto found = nodes.find(ticket.asset);
        Node* node = found == nodes.end() ? nullptr : &found->second;
        const bool current = accepting && !flight.cancelled && node != nullptr &&
            node->wanted != 0 && node->generation == flight.node_generation &&
            node->revision == ticket.revision;
        if (node != nullptr) {
            node->active = false;
        }
        if (!current) {
            ++result.discarded;
            count_event(counters.stale_results);
            report_import_failure(ticket, "stream import was cancelled or superseded");
            if (node != nullptr && node->generation == flight.node_generation && node->wanted != 0) {
                node->state = StreamState::queued;
                if (node->attempts != 0) {
                    --node->attempts;
                }
            }
            return;
        }
        try {
            if (!job) {
                stream_error(AssetError::stale_request, "owned executor handle was released outside the streamer");
            }
            if (job && job->exception) {
                std::rethrow_exception(job->exception);
            }
            if (job && job->state != jobs::JobState::Succeeded) {
                stream_error(AssetError::import_failed,
                    job->error.empty() ? "stream job was cancelled before completion" : job->error);
            }
            std::optional<ImportProduct> product;
            {
                std::scoped_lock lock(flight.result->mutex);
                product = std::move(flight.result->product);
                flight.result->product.reset();
            }
            if (!product || product->ticket != ticket) {
                stream_error(AssetError::stale_request, "stream result does not carry the submitted ticket");
            }
            const auto handle = catalog.publish_import(std::move(*product), cache, flight.admission.ticket);
            flight.admission.already_resident = true;
            node->retained = hold(handle, ticket.asset, ticket.revision, flight.dependencies);
            node->state = StreamState::ready;
            node->error.clear();
            node->exhausted = false;
            ++result.published;
            count_event(counters.published);
        } catch (const AssetException& error) {
            const auto message = diagnostic(error.what(), limits.max_error_bytes);
            report_import_failure(ticket, message);
            if (error.code() == AssetError::stale_request) {
                ++result.discarded;
                count_event(counters.stale_results);
                catalog_dirty = true;
            }
            const bool retryable = error.code() != AssetError::invalid_argument &&
                error.code() != AssetError::limit_exceeded && error.code() != AssetError::exhausted;
            fail(*node, message, retryable, now);
        } catch (const std::exception& error) {
            const auto message = diagnostic(error.what(), limits.max_error_bytes);
            report_import_failure(ticket, message);
            fail(*node, message, true, now);
        } catch (...) {
            report_import_failure(ticket, "stream decoder raised a non-standard exception");
            fail(*node, "stream decoder raised a non-standard exception", true, now);
        }
    }

    unsigned rank(const Node& node, Clock::time_point now) const {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - node.queued_at).count();
        const auto steps = age <= 0 ? 0ULL : static_cast<std::uint64_t>(age) / limits.priority_aging_ms;
        const auto base = static_cast<unsigned>(node.priority);
        return base + static_cast<unsigned>(std::min<std::uint64_t>(steps, 3U - base));
    }

    StreamSnapshot snapshot(const Request& request) const {
        StreamSnapshot result;
        result.ticket = request.ticket;
        result.dependencies = request.closure.empty() ? 0 : request.closure.size() - 1;
        const auto found = nodes.find(request.ticket.asset);
        if (found != nodes.end()) {
            const auto& node = found->second;
            result.state = node.state;
            result.desired_revision = node.revision;
            result.attempts = node.attempts;
            result.error = node.error;
            result.retry_after_seconds = std::max(0.0,
                std::chrono::duration<double>(node.retry_at - Clock::now()).count());
            if (node.retained) {
                result.available = node.retained->handle;
                result.available_revision = node.retained->revision;
                result.last_good = node.retained->revision != node.revision || node.state != StreamState::ready;
            }
        }
        for (const auto asset : request.closure) {
            const auto child = nodes.find(asset);
            if (asset != request.ticket.asset && child != nodes.end() &&
                child->second.state == StreamState::ready) {
                ++result.ready_dependencies;
            }
        }
        if (!request.error.empty()) {
            result.state = StreamState::failed;
            result.error = request.error;
            result.last_good = static_cast<bool>(result.available);
        }
        if (request.cancelled) {
            result.state = StreamState::cancelled;
            result.available = {};
            result.available_revision = {};
            result.last_good = false;
        }
        return result;
    }

    void remove_requests(StreamConsumer consumer) {
        for (auto it = requests.begin(); it != requests.end();) {
            if (it->second.ticket.consumer == consumer) {
                dependency_references -= it->second.closure.size();
                it = requests.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pins.begin(); it != pins.end();) {
            it = it->second.ticket.request.consumer == consumer ? pins.erase(it) : std::next(it);
        }
        reconcile();
    }

    jobs::Executor& executor;
    io::VirtualFileSystem& filesystem;
    AssetCatalog& catalog;
    ResourceCache& cache;
    StreamLimits limits;
    std::thread::id owner;
    std::map<std::uint64_t, std::uint64_t> consumers;
    std::map<std::uint64_t, Request> requests;
    std::map<AssetId, Node> nodes;
    std::map<ResourceHandle, std::shared_ptr<Version>> versions;
    std::map<std::uint64_t, Pin> pins;
    std::vector<std::unique_ptr<Flight>> flights;
    StreamStatistics counters;
    std::size_t dependency_references{};
    std::size_t source_reserved{};
    std::size_t decoded_reserved{};
    std::uint64_t next_consumer{1};
    std::uint64_t next_request{1};
    std::uint64_t next_pin{1};
    std::uint64_t next_node{1};
    std::uint64_t change_cursor{};
    bool catalog_dirty{};
    bool accepting{true};
    bool closed{};
};

AssetStreamer::AssetStreamer(jobs::Executor& executor, io::VirtualFileSystem& filesystem,
                             AssetCatalog& catalog, ResourceCache& cache, StreamLimits requested_limits)
    : impl_(std::make_unique<Impl>(executor, filesystem, catalog, cache, requested_limits)) {}

AssetStreamer::~AssetStreamer() = default;
const StreamLimits& AssetStreamer::limits() const noexcept { return impl_->limits; }

StreamConsumer AssetStreamer::create_consumer() {
    impl_->require_open();
    if (impl_->consumers.size() >= impl_->limits.max_consumers) {
        stream_error(AssetError::limit_exceeded, "stream consumer limit reached");
    }
    const StreamConsumer result{next_identity(impl_->next_consumer), 1};
    impl_->consumers.emplace(result.id, result.epoch);
    return result;
}

StreamConsumer AssetStreamer::advance_consumer(StreamConsumer consumer) {
    impl_->require_open();
    if (!impl_->valid_consumer(consumer)) {
        stream_error(AssetError::stale_request, "stream consumer epoch is stale");
    }
    if (consumer.epoch == std::numeric_limits<std::uint64_t>::max()) {
        stream_error(AssetError::exhausted, "stream consumer epoch exhausted");
    }
    impl_->remove_requests(consumer);
    ++consumer.epoch;
    impl_->consumers.at(consumer.id) = consumer.epoch;
    return consumer;
}

bool AssetStreamer::destroy_consumer(StreamConsumer consumer) {
    impl_->require_owner();
    if (!impl_->valid_consumer(consumer)) {
        return false;
    }
    impl_->remove_requests(consumer);
    impl_->consumers.erase(consumer.id);
    return true;
}

bool AssetStreamer::valid(StreamConsumer consumer) const {
    impl_->require_owner();
    return impl_->valid_consumer(consumer);
}

StreamTicket AssetStreamer::request(AssetId asset, StreamConsumer consumer, StreamRequestOptions options) {
    impl_->require_open();
    if (!impl_->valid_consumer(consumer) || !valid_priority(options.priority) ||
        options.attempts == 0 || options.attempts > impl_->limits.maximum_attempts) {
        stream_error(AssetError::invalid_argument, "invalid stream consumer or request options");
    }
    if (impl_->requests.size() >= impl_->limits.max_requests) {
        stream_error(AssetError::limit_exceeded, "stream request limit reached; release completed requests");
    }
    Impl::Request record;
    record.ticket = {next_identity(impl_->next_request), asset, consumer};
    record.options = options;
    const auto ticket = record.ticket;
    impl_->set_closure(record, impl_->catalog.dependency_order(asset));
    const auto references = record.closure.size();
    try {
        impl_->requests.emplace(ticket.generation, std::move(record));
    } catch (...) {
        impl_->dependency_references -= references;
        impl_->prune_nodes();
        throw;
    }
    try {
        impl_->reconcile();
    } catch (...) {
        const auto found = impl_->requests.find(ticket.generation);
        impl_->dependency_references -= found->second.closure.size();
        impl_->requests.erase(found);
        try {
            impl_->reconcile();
        } catch (...) {
        }
        throw;
    }
    return ticket;
}

bool AssetStreamer::cancel(StreamTicket ticket) {
    impl_->require_owner();
    auto* record = impl_->locate(ticket);
    if (record == nullptr || record->cancelled) {
        return false;
    }
    record->cancelled = true;
    impl_->dependency_references -= record->closure.size();
    record->closure.clear();
    impl_->reconcile();
    return true;
}

bool AssetStreamer::release(StreamTicket ticket) {
    impl_->require_owner();
    const auto* record = impl_->locate(ticket);
    if (record == nullptr) {
        return false;
    }
    impl_->dependency_references -= record->closure.size();
    impl_->requests.erase(ticket.generation);
    impl_->reconcile();
    return true;
}

bool AssetStreamer::reprioritize(StreamTicket ticket, jobs::Priority priority) {
    impl_->require_open();
    auto* record = impl_->locate(ticket);
    if (record == nullptr || record->cancelled || !valid_priority(priority)) {
        return false;
    }
    record->options.priority = priority;
    impl_->reconcile();
    return true;
}

bool AssetStreamer::retry(StreamTicket ticket) {
    impl_->require_open();
    auto* record = impl_->locate(ticket);
    if (record == nullptr || record->cancelled) {
        return false;
    }
    impl_->catalog_dirty = true;
    impl_->reconcile();
    for (const auto asset : record->closure) {
        auto& node = impl_->nodes.at(asset);
        if (!node.active && node.state != StreamState::ready) {
            impl_->reset_failure(node);
        }
    }
    return record->error.empty();
}

std::optional<StreamSnapshot> AssetStreamer::inspect(StreamTicket ticket) const {
    impl_->require_owner();
    const auto* record = impl_->locate(ticket);
    return record == nullptr ? std::nullopt : std::optional<StreamSnapshot>{impl_->snapshot(*record)};
}

std::vector<StreamSnapshot> AssetStreamer::requests(StreamConsumer consumer) const {
    impl_->require_owner();
    std::vector<StreamSnapshot> result;
    for (const auto& [generation, record] : impl_->requests) {
        (void)generation;
        if (!consumer || record.ticket.consumer == consumer) {
            result.push_back(impl_->snapshot(record));
        }
    }
    return result;
}

std::optional<StreamPin> AssetStreamer::pin(StreamTicket ticket, bool allow_last_good) {
    impl_->require_open();
    const auto* record = impl_->locate(ticket);
    const auto node = impl_->nodes.find(ticket.asset);
    if (record == nullptr || record->cancelled || node == impl_->nodes.end() ||
        !node->second.retained || (!allow_last_good &&
        (node->second.state != StreamState::ready || !record->error.empty()))) {
        return std::nullopt;
    }
    if (impl_->pins.size() >= impl_->limits.max_pins) {
        stream_error(AssetError::limit_exceeded, "stream explicit pin limit reached");
    }
    const auto version = node->second.retained;
    const StreamPin result{next_identity(impl_->next_pin), ticket, version->handle, version->revision};
    impl_->pins.emplace(result.generation, Impl::Pin{result, version});
    return result;
}

bool AssetStreamer::release(StreamPin ticket) {
    impl_->require_owner();
    const auto found = impl_->pins.find(ticket.generation);
    if (found == impl_->pins.end() || found->second.ticket != ticket ||
        !impl_->valid_consumer(ticket.request.consumer)) {
        return false;
    }
    impl_->pins.erase(found);
    impl_->collect_versions();
    return true;
}

std::vector<StreamPin> AssetStreamer::pins(StreamConsumer consumer) const {
    impl_->require_owner();
    std::vector<StreamPin> result;
    for (const auto& [generation, record] : impl_->pins) {
        (void)generation;
        if (!consumer || record.ticket.request.consumer == consumer) {
            result.push_back(record.ticket);
        }
    }
    return result;
}

std::span<const std::byte> AssetStreamer::bytes(StreamPin ticket) {
    impl_->require_owner();
    const auto found = impl_->pins.find(ticket.generation);
    if (found == impl_->pins.end() || found->second.ticket != ticket ||
        !impl_->valid_consumer(ticket.request.consumer)) {
        stream_error(AssetError::invalid_handle, "stream pin is stale or belongs to an old consumer epoch");
    }
    return impl_->cache.bytes(ticket.resource);
}

std::size_t AssetStreamer::trim(std::size_t target_reserved_bytes) {
    impl_->require_open();
    impl_->prune_nodes();
    return impl_->cache.trim(target_reserved_bytes);
}

std::size_t AssetStreamer::poll_catalog_changes() {
    impl_->require_owner();
    if (!impl_->accepting) {
        return 0;
    }
    const auto batch = impl_->catalog.changes_since(impl_->change_cursor, impl_->limits.changes_per_poll);
    if (batch.history_lost) {
        impl_->catalog_dirty = true;
        count_event(impl_->counters.history_rescans);
    }
    for (const auto& change : batch.changes) {
        const auto node = impl_->nodes.find(change.asset);
        if (change.kind == AssetChangeKind::restored || change.kind == AssetChangeKind::erased ||
            (node != impl_->nodes.end() && node->second.revision != change.revision)) {
            impl_->catalog_dirty = true;
        }
        impl_->change_cursor = change.sequence;
    }
    if (batch.changes.empty() && batch.history_lost) {
        impl_->change_cursor = batch.latest_sequence;
    }
    return batch.changes.size();
}

StreamPumpResult AssetStreamer::pump() {
    impl_->require_owner();
    StreamPumpResult result;
    if (!impl_->accepting) {
        return result;
    }
    (void)poll_catalog_changes();
    impl_->reconcile();
    const auto now = Clock::now();
    for (auto it = impl_->flights.begin(); it != impl_->flights.end() &&
         result.completed < impl_->limits.completions_per_pump;) {
        const auto job = impl_->executor.inspect((*it)->job);
        if (job && !jobs::terminal(job->state)) {
            ++it;
            continue;
        }
        impl_->complete(**it, job, result, now);
        impl_->retire(**it);
        it = impl_->flights.erase(it);
        ++result.completed;
    }
    impl_->reconcile();
    std::vector<Impl::Node*> candidates;
    candidates.reserve(impl_->nodes.size());
    for (auto& [asset, node] : impl_->nodes) {
        (void)asset;
        if (node.wanted != 0 && !node.active && !node.exhausted &&
            node.state != StreamState::ready && node.retry_at <= now) {
            candidates.push_back(&node);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Impl::Node* left, const Impl::Node* right) {
        const auto first = impl_->rank(*left, now);
        const auto second = impl_->rank(*right, now);
        if (first != second) {
            return first > second;
        }
        return left->queued_at != right->queued_at ? left->queued_at < right->queued_at : left->asset < right->asset;
    });
    for (auto* node : candidates) {
        if (result.submitted >= impl_->limits.dispatches_per_pump ||
            impl_->flights.size() >= impl_->limits.max_jobs) {
            break;
        }
        const auto record = impl_->catalog.inspect(node->asset);
        if (record && impl_->launch(*node, *record, now)) {
            ++result.submitted;
        }
    }
    return result;
}

StreamStatistics AssetStreamer::statistics() const {
    impl_->require_owner();
    auto result = impl_->counters;
    result.consumers = impl_->consumers.size();
    result.requests = impl_->requests.size();
    result.tracked_assets = impl_->nodes.size();
    result.dependency_references = impl_->dependency_references;
    result.jobs = impl_->flights.size();
    result.explicit_pins = impl_->pins.size();
    result.source_reserved_bytes = impl_->source_reserved;
    result.decoded_reserved_bytes = impl_->decoded_reserved;
    result.inflight_reserved_bytes = impl_->source_reserved + impl_->decoded_reserved;
    result.accepting = impl_->accepting;
    for (const auto& [asset, node] : impl_->nodes) {
        (void)asset;
        result.ready += node.state == StreamState::ready ? 1U : 0U;
        result.failed += node.state == StreamState::failed ? 1U : 0U;
        result.queued += node.wanted != 0 && !node.active && !node.exhausted &&
            node.state != StreamState::ready ? 1U : 0U;
    }
    return result;
}

void AssetStreamer::shutdown() {
    impl_->require_owner();
    if (impl_->closed) {
        return;
    }
    impl_->accepting = false;
    for (const auto& flight : impl_->flights) {
        flight->cancelled = true;
        (void)impl_->executor.cancel(flight->job);
    }
    for (const auto& flight : impl_->flights) {
        while (!impl_->executor.wait(flight->job, std::chrono::milliseconds(1000))) {
            try {
                if (!impl_->executor.inspect(flight->job)) {
                    break;
                }
            } catch (const std::bad_alloc&) {
                // Allocation failure in a diagnostic snapshot must not abandon a live worker.
            }
        }
        impl_->report_import_failure(flight->result->request.ticket, "asset streamer shut down");
        impl_->retire(*flight);
    }
    impl_->flights.clear();
    impl_->requests.clear();
    impl_->pins.clear();
    impl_->nodes.clear();
    for (const auto& [handle, version] : impl_->versions) {
        (void)version;
        (void)impl_->cache.unpin(handle);
    }
    impl_->versions.clear();
    impl_->consumers.clear();
    impl_->dependency_references = 0;
    impl_->closed = true;
}

namespace {
struct StreamingRuntime {
    StreamConsumer consumer;
};

StreamTicket command_ticket(AssetStreamer& streamer, StreamConsumer consumer, std::string_view value) {
    const auto generation = number(value);
    for (const auto& request : streamer.requests(consumer)) {
        if (request.ticket.generation == generation) {
            return request.ticket;
        }
    }
    stream_error(AssetError::stale_request, "unknown stream request generation");
}

std::size_t configured_size(core::Kernel& kernel, std::string_view key, std::size_t fallback) {
    const auto value = kernel.config().integer(key, static_cast<std::int64_t>(fallback));
    return value > 0 && static_cast<std::uint64_t>(value) <= std::numeric_limits<std::size_t>::max()
        ? static_cast<std::size_t>(value) : fallback;
}
} // namespace

void register_streaming(core::Kernel& kernel) {
    if (kernel.services().contains<AssetStreamer>()) {
        return;
    }
    auto* executor = kernel.services().find<jobs::Executor>();
    auto* filesystem = kernel.services().find<io::VirtualFileSystem>();
    auto* catalog = kernel.services().find<AssetCatalog>();
    auto* cache = kernel.services().find<ResourceCache>();
    if (executor == nullptr || filesystem == nullptr || catalog == nullptr || cache == nullptr) {
        throw std::logic_error("assets.streaming requires jobs.executor, io.filesystem and assets.catalog");
    }
    StreamLimits limits;
    limits.max_requests = configured_size(kernel, "stream.requests", limits.max_requests);
    limits.max_jobs = std::min(configured_size(kernel, "stream.jobs", limits.max_jobs), limits.max_assets);
    limits.max_source_bytes = configured_size(kernel, "stream.source_bytes", limits.max_source_bytes);
    limits.max_decoded_bytes = configured_size(kernel, "stream.decoded_bytes", limits.max_decoded_bytes);
    limits.max_inflight_bytes = configured_size(kernel, "stream.inflight_bytes", limits.max_inflight_bytes);
    auto& service = kernel.services().emplace<AssetStreamer>(*executor, *filesystem, *catalog, *cache, limits);
    if (!kernel.register_module({"assets.streaming", "Bounded asynchronous imports and retained version ownership", 1}) ||
        !kernel.add_shutdown("assets.streaming", [streamer = &service] { streamer->shutdown(); })) {
        service.shutdown();
        throw std::logic_error("streaming module or shutdown registration conflict");
    }
    kernel.services().emplace<StreamingRuntime>().consumer = service.create_consumer();
    (void)kernel.add_system({"assets.streaming.changes", core::Phase::Input, -300, {"assets.catalog"}},
        [](core::Kernel& host, const core::Frame&) {
            (void)host.services().get<AssetStreamer>().poll_catalog_changes();
        });
    (void)kernel.add_system({"assets.streaming", core::Phase::PreSimulation, -900, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& streamer = host.services().get<AssetStreamer>();
            (void)streamer.pump();
            const auto stats = streamer.statistics();
            host.metric("stream.jobs", static_cast<double>(stats.jobs));
            host.metric("stream.queued", static_cast<double>(stats.queued));
            host.metric("stream.ready", static_cast<double>(stats.ready));
            host.metric("stream.failed", static_cast<double>(stats.failed));
            host.metric("stream.source_bytes", static_cast<double>(stats.source_reserved_bytes));
            host.metric("stream.decoded_bytes", static_cast<double>(stats.decoded_reserved_bytes));
            host.metric("stream.published", static_cast<double>(stats.published));
            host.metric("stream.stale", static_cast<double>(stats.stale_results));
        });
    (void)kernel.add_command("stream.request", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 3) {
            return core::CommandResult{false, "usage: stream.request asset_id [priority_0_to_3] [attempts]"};
        }
        StreamRequestOptions options;
        if (args.size() >= 2) {
            const auto priority = number(args[1]);
            if (priority > 3) {
                return core::CommandResult{false, "priority must be between 0 and 3"};
            }
            options.priority = static_cast<jobs::Priority>(priority);
        }
        if (args.size() == 3) {
            const auto attempts = number(args[2]);
            if (attempts > std::numeric_limits<std::uint32_t>::max()) {
                return core::CommandResult{false, "attempt count exceeds its integer range"};
            }
            options.attempts = static_cast<std::uint32_t>(attempts);
        }
        const auto ticket = host.services().get<AssetStreamer>().request({number(args[0])},
            host.services().get<StreamingRuntime>().consumer, options);
        return core::CommandResult{true, "stream request " + std::to_string(ticket.generation)};
    }, "Queue an owning asynchronous asset request, including its dependency closure");
    (void)kernel.add_command("stream.cancel", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: stream.cancel request_generation"};
        }
        auto& streamer = host.services().get<AssetStreamer>();
        const auto ticket = command_ticket(streamer, host.services().get<StreamingRuntime>().consumer, args[0]);
        const bool cancelled = streamer.cancel(ticket);
        return core::CommandResult{cancelled, cancelled ? "request cancelled; active jobs retain reservations until terminal" : "request already cancelled"};
    }, "Cancel request demand without retiring still-running worker memory");
    (void)kernel.add_command("stream.pin", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: stream.pin request_generation"};
        }
        auto& streamer = host.services().get<AssetStreamer>();
        const auto pin = streamer.pin(command_ticket(streamer,
            host.services().get<StreamingRuntime>().consumer, args[0]));
        return core::CommandResult{pin.has_value(), pin ? "stream pin " + std::to_string(pin->generation) +
            " revision=" + std::to_string(pin->revision.value) : "no published or last good version is available"};
    }, "Pin the available exact version across reload, cancellation and request release");
    (void)kernel.add_command("stream.release", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2 || (args[0] != "request" && args[0] != "pin")) {
            return core::CommandResult{false, "usage: stream.release request|pin generation"};
        }
        auto& streamer = host.services().get<AssetStreamer>();
        const auto consumer = host.services().get<StreamingRuntime>().consumer;
        bool released{};
        if (args[0] == "request") {
            released = streamer.release(command_ticket(streamer, consumer, args[1]));
        } else {
            const auto generation = number(args[1]);
            for (const auto& pin : streamer.pins(consumer)) {
                if (pin.generation == generation) {
                    released = streamer.release(pin);
                    break;
                }
            }
        }
        return core::CommandResult{released, released ? "stream ownership released" : "unknown or stale ownership ticket"};
    }, "Release a request or exact-version pin and permit unused cached versions to be evicted");
    (void)kernel.add_command("stream.retry", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: stream.retry request_generation"};
        }
        auto& streamer = host.services().get<AssetStreamer>();
        const bool accepted = streamer.retry(command_ticket(streamer,
            host.services().get<StreamingRuntime>().consumer, args[0]));
        return core::CommandResult{accepted, accepted ? "retry queued" : "request cannot currently resolve its dependencies"};
    }, "Restart bounded retry policy for an existing request and failed dependencies");
    (void)kernel.add_command("stream.inspect", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: stream.inspect request_generation"};
        }
        auto& streamer = host.services().get<AssetStreamer>();
        const auto state = streamer.inspect(command_ticket(streamer,
            host.services().get<StreamingRuntime>().consumer, args[0]));
        if (!state) {
            return core::CommandResult{false, "request ticket is stale"};
        }
        return core::CommandResult{true, std::string(stream_state_name(state->state)) +
            " desired=" + std::to_string(state->desired_revision.value) +
            " available=" + std::to_string(state->available_revision.value) +
            " attempts=" + std::to_string(state->attempts) +
            " retry_seconds=" + std::to_string(state->retry_after_seconds) +
            (state->error.empty() ? std::string{} : " error=" + state->error)};
    }, "Inspect current revision, retained fallback, retry timing and actionable failure diagnostics");
    (void)kernel.add_command("stream.stats", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: stream.stats"};
        }
        const auto stats = host.services().get<AssetStreamer>().statistics();
        return core::CommandResult{true, "requests=" + std::to_string(stats.requests) +
            " jobs=" + std::to_string(stats.jobs) + " queued=" + std::to_string(stats.queued) +
            " ready=" + std::to_string(stats.ready) + " failed=" + std::to_string(stats.failed) +
            " source_reserved=" + std::to_string(stats.source_reserved_bytes) +
            " decoded_reserved=" + std::to_string(stats.decoded_reserved_bytes) +
            " peak_inflight=" + std::to_string(stats.peak_inflight_bytes) +
            " published=" + std::to_string(stats.published) + " stale=" + std::to_string(stats.stale_results) +
            " denials=" + std::to_string(stats.admission_denials)};
    }, "Inspect streaming demand, terminal failures and pending byte reservations");
}

} // namespace ge::assets
