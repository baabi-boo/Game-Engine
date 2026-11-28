#pragma once

#include <ge/io/filesystem.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::assets {

struct AssetId {
    std::uint64_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const AssetId&) const = default;
};

struct AssetRevision {
    std::uint64_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    auto operator<=>(const AssetRevision&) const = default;
};

using AssetTypeId = std::uint32_t;
inline constexpr AssetTypeId binary_asset_type = 1;

enum class AssetError : std::uint8_t {
    invalid_argument, unknown_asset, unknown_type, duplicate, dependency_cycle,
    dependency_in_use, limit_exceeded, exhausted, stale_request, busy,
    source_missing, source_changed, import_failed, budget_exceeded,
    invalid_handle, pinned, malformed_catalog
};

class AssetException final : public std::runtime_error {
public:
    AssetException(AssetError code, std::string message);
    [[nodiscard]] AssetError code() const noexcept;
private:
    AssetError code_;
};

struct CatalogLimits {
    std::size_t max_assets{8192};
    std::size_t max_types{128};
    std::size_t max_path_bytes{4096};
    std::size_t max_name_bytes{256};
    std::size_t max_tags_per_asset{32};
    std::size_t max_tag_bytes{64};
    std::size_t max_dependencies_per_asset{128};
    std::size_t max_dependency_edges{65536};
    std::size_t max_source_bytes{64U * 1024U * 1024U};
    std::size_t max_resource_bytes{64U * 1024U * 1024U};
    std::size_t max_catalog_bytes{64U * 1024U * 1024U};
    std::size_t max_error_bytes{2048};
    std::size_t change_history{8192};
};

struct DependencyVersion {
    AssetId asset;
    AssetRevision revision;
    auto operator<=>(const DependencyVersion&) const = default;
};

struct ImportContext {
    AssetId asset;
    AssetRevision revision;
    std::string_view source_path;
    std::span<const std::byte> source;
    std::span<const DependencyVersion> dependencies;
    std::size_t maximum_output_bytes{};
};

struct ResourcePayload {
    std::vector<std::byte> bytes;
    std::string format;
    std::uint64_t checksum{};
};

struct AssetTypeDescriptor {
    AssetTypeId id{};
    std::string name;
    std::vector<std::string> extensions;
    std::size_t maximum_source_bytes{64U * 1024U * 1024U};
    std::size_t maximum_resource_bytes{64U * 1024U * 1024U};
    // An absent decoder moves the source bytes directly into the resource.
    std::function<ResourcePayload(const ImportContext&)> decode;
};

enum class ImportStatus : std::uint8_t {
    registered, stale, importing, ready, failed, missing
};

struct AssetDefinition {
    AssetId id;
    AssetTypeId type{binary_asset_type};
    std::string source_path;
    std::string name;
    std::vector<std::string> tags;
    std::vector<AssetId> dependencies;
};

struct AssetRecord {
    AssetDefinition definition;
    AssetRevision revision{1};
    AssetRevision last_good_revision;
    // Paired with last_good_revision even when update changes definition.type.
    // Restoring a catalog clears both until a resource is imported again.
    AssetTypeId last_good_type{};
    ImportStatus status{ImportStatus::registered};
    std::uint64_t request_generation{};
    std::uint64_t source_cache_key{};
    std::uint64_t source_hash{};
    std::uint64_t source_bytes{};
    std::uint64_t resource_bytes{};
    std::string last_error;
};

struct AssetQuery {
    std::string text;
    std::optional<AssetTypeId> type;
    std::optional<ImportStatus> status;
    std::vector<std::string> tags;
    std::size_t offset{};
    std::size_t limit{256};
};

struct ImportTicket {
    AssetId asset;
    AssetRevision revision;
    AssetTypeId type{};
    std::uint64_t generation{};
    auto operator<=>(const ImportTicket&) const = default;
};

struct ImportRequest {
    ImportTicket ticket;
    std::string source_path;
    AssetTypeDescriptor descriptor;
    std::vector<DependencyVersion> dependencies;
};

struct ImportProduct {
    ImportTicket ticket;
    io::FileMetadata source;
    std::uint64_t source_cache_key{};
    std::uint64_t source_hash{};
    std::vector<DependencyVersion> dependencies;
    ResourcePayload payload;
};

// No jobs are launched. Requests own their inputs; decoder thread safety is the
// registering application's responsibility if a later loader runs them on jobs.
[[nodiscard]] ImportProduct import_source(io::VirtualFileSystem& filesystem,
                                          const ImportRequest& request);

struct ResourceHandle {
    std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
    std::uint64_t generation{};
    AssetTypeId type{};
    [[nodiscard]] explicit operator bool() const noexcept { return generation != 0; }
    auto operator<=>(const ResourceHandle&) const = default;
};

template<AssetTypeId Type>
struct TypedResourceHandle {
    ResourceHandle handle;
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle) && handle.type == Type;
    }
    auto operator<=>(const TypedResourceHandle&) const = default;
};

struct ResourceTicket {
    ResourceHandle handle;
    AssetId asset;
    AssetRevision revision;
    std::uint64_t request_generation{};
    auto operator<=>(const ResourceTicket&) const = default;
};

enum class ResourceState : std::uint8_t { pending, resident, retiring };

struct ResourceInfo {
    ResourceTicket ticket;
    ResourceState state{ResourceState::pending};
    std::size_t reserved_bytes{};
    std::size_t payload_bytes{};
    std::uint32_t pins{};
    std::uint64_t last_access{};
    std::uint64_t checksum{};
    std::string format;
};

struct CacheLimits {
    std::size_t budget_bytes{256U * 1024U * 1024U};
    std::size_t max_resources{16384};
    std::size_t max_resource_bytes{64U * 1024U * 1024U};
};

struct CacheStatistics {
    std::size_t budget_bytes{};
    std::size_t pending_bytes{};
    std::size_t resident_bytes{};
    std::size_t retiring_bytes{};
    std::size_t resources{};
    std::size_t pinned_resources{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t evictions{};
    [[nodiscard]] std::size_t reserved_bytes() const noexcept {
        return pending_bytes + resident_bytes + retiring_bytes;
    }
};

struct ResourceAdmission {
    ResourceTicket ticket;
    bool already_resident{};
};

// Mutations are serialized by the caller. Retained byte views require a pin.
// Explicit retirement keeps its reservation and payload until acknowledgement.
// LRU eviction acknowledges immediately because this cache owns only CPU bytes.
class ResourceCache final {
public:
    explicit ResourceCache(CacheLimits limits = {});
    ~ResourceCache();
    ResourceCache(const ResourceCache&) = delete;
    ResourceCache& operator=(const ResourceCache&) = delete;
    [[nodiscard]] ResourceAdmission admit(AssetId asset, AssetRevision revision,
                                          AssetTypeId type, std::size_t bytes);
    [[nodiscard]] bool publish(const ResourceTicket& ticket, ResourcePayload payload);
    [[nodiscard]] bool cancel(const ResourceTicket& ticket);
    [[nodiscard]] std::optional<ResourceTicket> retire(ResourceHandle handle);
    [[nodiscard]] bool acknowledge_retirement(const ResourceTicket& ticket);
    [[nodiscard]] bool pin(ResourceHandle handle);
    [[nodiscard]] bool unpin(ResourceHandle handle);
    [[nodiscard]] std::optional<ResourceHandle> find(AssetId asset,
                                                    AssetRevision revision,
                                                    AssetTypeId type);
    [[nodiscard]] std::optional<ResourceInfo> inspect(ResourceHandle handle) const;
    [[nodiscard]] std::vector<ResourceInfo> entries() const;
    [[nodiscard]] std::span<const std::byte> bytes(ResourceHandle handle);
    [[nodiscard]] CacheStatistics statistics() const noexcept;
    [[nodiscard]] bool set_budget(std::size_t bytes);
    [[nodiscard]] std::size_t trim(std::size_t target_reserved_bytes);

    template<AssetTypeId Type>
    [[nodiscard]] TypedResourceHandle<Type> typed(ResourceHandle handle) const {
        const auto info = inspect(handle);
        if (!info || info->state != ResourceState::resident || handle.type != Type) {
            return {};
        }
        return {handle};
    }

    template<AssetTypeId Type>
    [[nodiscard]] std::span<const std::byte> bytes(TypedResourceHandle<Type> handle) {
        return handle ? bytes(handle.handle) : std::span<const std::byte>{};
    }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class AssetChangeKind : std::uint8_t {
    registered, updated, invalidated, imported, failed, erased, restored
};

struct AssetChange {
    std::uint64_t sequence{};
    AssetId asset;
    AssetRevision revision;
    AssetChangeKind kind{};
};

struct AssetChangeBatch {
    std::vector<AssetChange> changes;
    // A bounded reader advances to its last returned change, not this upper bound.
    std::uint64_t latest_sequence{};
    bool history_lost{};
};

class AssetCatalog final {
public:
    explicit AssetCatalog(io::VirtualFileSystem& filesystem, CatalogLimits limits = {});
    ~AssetCatalog();
    AssetCatalog(const AssetCatalog&) = delete;
    AssetCatalog& operator=(const AssetCatalog&) = delete;
    [[nodiscard]] const CatalogLimits& limits() const noexcept;
    void register_type(AssetTypeDescriptor descriptor);
    [[nodiscard]] std::vector<AssetTypeDescriptor> types() const;
    [[nodiscard]] AssetId add(AssetDefinition definition);
    [[nodiscard]] bool erase(AssetId asset);
    void update(AssetDefinition definition);
    [[nodiscard]] std::optional<AssetRecord> inspect(AssetId asset) const;
    [[nodiscard]] std::optional<AssetId> find_path(std::string_view path) const;
    [[nodiscard]] std::vector<AssetRecord> search(const AssetQuery& query = {}) const;
    [[nodiscard]] std::vector<AssetId> dependency_order(AssetId asset) const;
    [[nodiscard]] std::vector<AssetId> dependents(AssetId asset, bool transitive = true) const;
    [[nodiscard]] std::vector<AssetId> invalidate(AssetId asset);
    [[nodiscard]] std::size_t apply_file_changes(std::span<const io::FileChange> changes);
    // Manual import drivers pin previous/dependency resources and admit the
    // descriptor's maximum output before decoding. On failure they cancel the
    // reservation, acknowledge actual retirement, and call fail_import.
    [[nodiscard]] ImportRequest begin_import(AssetId asset);
    [[nodiscard]] bool fail_import(const ImportTicket& ticket, std::string error);
    [[nodiscard]] ResourceHandle publish_import(ImportProduct product,
                                               ResourceCache& cache,
                                               const ResourceTicket& admission);
    [[nodiscard]] ResourceHandle load(AssetId asset, ResourceCache& cache);
    [[nodiscard]] AssetChangeBatch changes_since(std::uint64_t sequence,
                                                std::size_t limit = 8192) const;
    [[nodiscard]] std::vector<std::byte> encode() const;
    void restore(std::span<const std::byte> bytes);
    void save(std::string_view virtual_path) const;
    void open(std::string_view virtual_path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// This module is the sole consumer of io::FileChange in the kernel. Downstream
// consumers independently poll changes_since, or designate one AssetChange
// event consumer; draining either kernel event channel is not a broadcast.
void register_catalog(core::Kernel& kernel);

} // namespace ge::assets
