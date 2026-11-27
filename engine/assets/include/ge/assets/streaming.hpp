#pragma once

#include <ge/assets/catalog.hpp>
#include <ge/jobs/executor.hpp>

#include <cstddef>
#include <cstdint>
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::assets {

struct StreamConsumer {
    std::uint64_t id{};
    std::uint64_t epoch{};
    [[nodiscard]] explicit operator bool() const noexcept { return id != 0 && epoch != 0; }
    auto operator<=>(const StreamConsumer&) const = default;
};

struct StreamTicket {
    std::uint64_t generation{};
    AssetId asset;
    StreamConsumer consumer;
    [[nodiscard]] explicit operator bool() const noexcept { return generation != 0; }
    auto operator<=>(const StreamTicket&) const = default;
};

enum class StreamState : std::uint8_t {
    queued, waiting_dependencies, waiting_budget, loading,
    ready, retrying, failed, cancelled
};

[[nodiscard]] std::string_view stream_state_name(StreamState state) noexcept;

struct StreamLimits {
    std::size_t max_requests{4096};
    std::size_t max_assets{8192};
    std::size_t max_consumers{1024};
    std::size_t max_pins{8192};
    std::size_t max_versions{16384};
    std::size_t max_dependency_references{65536};
    std::size_t max_jobs{8};
    std::size_t max_source_bytes{192U * 1024U * 1024U};
    std::size_t max_decoded_bytes{128U * 1024U * 1024U};
    std::size_t max_inflight_bytes{320U * 1024U * 1024U};
    std::size_t dispatches_per_pump{8};
    std::size_t completions_per_pump{64};
    std::size_t changes_per_poll{512};
    std::size_t max_error_bytes{1024};
    std::uint32_t maximum_attempts{8};
    std::uint32_t retry_initial_ms{100};
    std::uint32_t retry_maximum_ms{5000};
    std::uint32_t priority_aging_ms{250};
};

struct StreamRequestOptions {
    jobs::Priority priority{jobs::Priority::Normal};
    // Includes the first attempt. A revision change starts a fresh attempt count.
    // Shared asset work uses the largest allowance among its live requests.
    std::uint32_t attempts{3};
};

struct StreamSnapshot {
    StreamTicket ticket;
    StreamState state{StreamState::queued};
    AssetRevision desired_revision;
    AssetRevision available_revision;
    ResourceHandle available;
    std::size_t dependencies{};
    std::size_t ready_dependencies{};
    std::uint32_t attempts{};
    double retry_after_seconds{};
    bool last_good{};
    std::string error;
};

// An explicit pin keeps this exact version valid across hot reload and request
// cancellation. Release it, or advance/destroy its consumer, before shutdown.
struct StreamPin {
    std::uint64_t generation{};
    StreamTicket request;
    ResourceHandle resource;
    AssetRevision revision;
    [[nodiscard]] explicit operator bool() const noexcept { return generation != 0; }
    auto operator<=>(const StreamPin&) const = default;
};

struct StreamStatistics {
    std::size_t consumers{};
    std::size_t requests{};
    std::size_t tracked_assets{};
    std::size_t dependency_references{};
    std::size_t jobs{};
    std::size_t queued{};
    std::size_t ready{};
    std::size_t failed{};
    std::size_t explicit_pins{};
    std::size_t source_reserved_bytes{};
    std::size_t decoded_reserved_bytes{};
    std::size_t inflight_reserved_bytes{};
    std::size_t peak_inflight_bytes{};
    std::uint64_t submitted{};
    std::uint64_t published{};
    std::uint64_t cancelled{};
    std::uint64_t stale_results{};
    std::uint64_t failures{};
    std::uint64_t retries{};
    std::uint64_t admission_denials{};
    std::uint64_t history_rescans{};
    bool accepting{};
};

struct StreamPumpResult {
    std::size_t completed{};
    std::size_t submitted{};
    std::size_t published{};
    std::size_t discarded{};
};

// Single owner thread: requests, metadata, publication, pins and eviction are
// serialized on the constructing thread. Jobs touch only their owning request,
// shared result state and the thread-safe VFS. Decoders must be thread-safe and
// obey their advertised output cap; arbitrary decoder scratch is not measured.
// Requests retain their dependency closure and last good versions until released.
// Historical dependency graphs are preserved for versions published here. An
// external stale cache version with dependencies needs its original manifest and
// is not adopted as a fallback when that version was never tracked here.
// Source reservations include the VFS hashing buffer as well as source bytes.
// Explicit shutdown is required while all borrowed services remain alive. The
// destructor only destroys owned state and never calls borrowed services.
class AssetStreamer final {
public:
    AssetStreamer(jobs::Executor& executor, io::VirtualFileSystem& filesystem,
                  AssetCatalog& catalog, ResourceCache& cache, StreamLimits requested_limits = {});
    ~AssetStreamer();
    AssetStreamer(const AssetStreamer&) = delete;
    AssetStreamer& operator=(const AssetStreamer&) = delete;
    AssetStreamer(AssetStreamer&&) = delete;
    AssetStreamer& operator=(AssetStreamer&&) = delete;

    [[nodiscard]] const StreamLimits& limits() const noexcept;
    [[nodiscard]] StreamConsumer create_consumer();
    // Invalidates all old request/pin tickets, cancelling their work only when
    // no other live consumer still demands the same asset version.
    [[nodiscard]] StreamConsumer advance_consumer(StreamConsumer consumer);
    [[nodiscard]] bool destroy_consumer(StreamConsumer consumer);
    [[nodiscard]] bool valid(StreamConsumer consumer) const;

    [[nodiscard]] StreamTicket request(AssetId asset, StreamConsumer consumer,
                                        StreamRequestOptions options = {});
    [[nodiscard]] bool cancel(StreamTicket ticket);
    [[nodiscard]] bool release(StreamTicket ticket);
    [[nodiscard]] bool reprioritize(StreamTicket ticket, jobs::Priority priority);
    [[nodiscard]] bool retry(StreamTicket ticket);
    [[nodiscard]] std::optional<StreamSnapshot> inspect(StreamTicket ticket) const;
    [[nodiscard]] std::vector<StreamSnapshot> requests(StreamConsumer consumer = {}) const;

    [[nodiscard]] std::optional<StreamPin> pin(StreamTicket ticket, bool allow_last_good = true);
    [[nodiscard]] bool release(StreamPin ticket);
    [[nodiscard]] std::vector<StreamPin> pins(StreamConsumer consumer = {}) const;
    [[nodiscard]] std::span<const std::byte> bytes(StreamPin ticket);
    [[nodiscard]] std::size_t trim(std::size_t target_reserved_bytes);

    // Independent catalog cursor; never consumes io::FileChange or job events.
    [[nodiscard]] std::size_t poll_catalog_changes();
    [[nodiscard]] StreamPumpResult pump();
    [[nodiscard]] StreamStatistics statistics() const;
    // Stops admission, requests cancellation, waits for every owned job to become
    // terminal, then releases reservations, resource pins and executor handles.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_streaming(core::Kernel& kernel);

} // namespace ge::assets
