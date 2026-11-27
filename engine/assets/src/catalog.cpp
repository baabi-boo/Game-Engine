#include <ge/assets/catalog.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <utility>

namespace ge::assets {
namespace {

[[noreturn]] void reject(AssetError code, std::string message) {
    throw AssetException(code, std::move(message));
}

void require_text(std::string_view text, std::size_t maximum, bool empty = false) {
    if ((!empty && text.empty()) || text.size() > maximum || !core::valid_utf8(text)) {
        reject(AssetError::invalid_argument, "invalid or oversized asset text");
    }
    for (const char character : text) {
        if (static_cast<unsigned char>(character) < 32U || character == '\x7f') {
            reject(AssetError::invalid_argument, "asset text contains a control byte");
        }
    }
}

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

void canonical_tags(std::vector<std::string>& tags, const CatalogLimits& limits) {
    if (tags.size() > limits.max_tags_per_asset) {
        reject(AssetError::limit_exceeded, "too many asset tags");
    }
    for (auto& tag : tags) {
        require_text(tag, limits.max_tag_bytes);
        tag = lower_ascii(tag);
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
}

std::uint64_t take_counter(std::uint64_t& counter, std::string_view description) {
    if (counter == 0) {
        reject(AssetError::exhausted, std::string(description) + " exhausted");
    }
    const auto result = counter;
    counter = counter == std::numeric_limits<std::uint64_t>::max() ? 0 : counter + 1;
    return result;
}

void increment_stat(std::uint64_t& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) {
        ++counter;
    }
}

std::uint64_t parse_unsigned(std::string_view text) {
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        reject(AssetError::invalid_argument, "expected an unsigned decimal integer");
    }
    return value;
}

std::size_t bounded_size(std::uint64_t value, std::size_t maximum) {
    if (value > maximum) {
        reject(AssetError::limit_exceeded, "asset count or byte size exceeds its limit");
    }
    return static_cast<std::size_t>(value);
}

std::string status_name(ImportStatus status) {
    switch (status) {
    case ImportStatus::registered: return "registered";
    case ImportStatus::stale: return "stale";
    case ImportStatus::importing: return "importing";
    case ImportStatus::ready: return "ready";
    case ImportStatus::failed: return "failed";
    case ImportStatus::missing: return "missing";
    }
    return "unknown";
}

std::string handle_text(ResourceHandle handle) {
    return std::to_string(handle.slot) + ":" + std::to_string(handle.generation) +
           ":" + std::to_string(handle.type);
}

ResourceHandle parse_handle(std::string_view text) {
    const auto first = text.find(':');
    const auto second = first == text.npos ? text.npos : text.find(':', first + 1);
    if (first == text.npos || second == text.npos) {
        reject(AssetError::invalid_argument, "expected slot:generation:type handle");
    }
    const auto slot = parse_unsigned(text.substr(0, first));
    const auto generation = parse_unsigned(text.substr(first + 1, second - first - 1));
    const auto type = parse_unsigned(text.substr(second + 1));
    if (slot >= std::numeric_limits<std::uint32_t>::max() || generation == 0 ||
        type == 0 || type > std::numeric_limits<AssetTypeId>::max()) {
        reject(AssetError::invalid_handle, "resource handle fields are out of range");
    }
    return {static_cast<std::uint32_t>(slot), generation, static_cast<AssetTypeId>(type)};
}

class PinSet final {
public:
    PinSet(ResourceCache& cache, std::size_t asset_count) : cache_(cache) {
        std::size_t capacity{};
        if (!core::checked_multiply(asset_count, 2, capacity)) {
            reject(AssetError::limit_exceeded, "import pin set exceeds its allocation limit");
        }
        handles_.reserve(capacity);
    }
    ~PinSet() {
        for (const auto handle : handles_) {
            (void)cache_.unpin(handle);
        }
    }
    void add(ResourceHandle handle) {
        if (std::find(handles_.begin(), handles_.end(), handle) != handles_.end()) {
            return;
        }
        handles_.push_back(handle);
        if (!cache_.pin(handle)) {
            handles_.pop_back();
            reject(AssetError::pinned, "resource could not be pinned for import");
        }
    }
private:
    ResourceCache& cache_;
    std::vector<ResourceHandle> handles_;
};

} // namespace

AssetException::AssetException(AssetError code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

AssetError AssetException::code() const noexcept { return code_; }

ImportProduct import_source(io::VirtualFileSystem& filesystem,
                            const ImportRequest& request) {
    if (!request.ticket.asset || !request.ticket.revision || request.ticket.generation == 0 ||
        request.ticket.type == 0 || request.ticket.type != request.descriptor.id ||
        request.descriptor.maximum_source_bytes == 0 ||
        request.descriptor.maximum_resource_bytes == 0) {
        reject(AssetError::invalid_argument, "invalid import request");
    }
    const auto before = filesystem.metadata(request.source_path);
    if (!before || before->kind != io::FileKind::regular) {
        reject(AssetError::source_missing, "asset source is not a regular file");
    }
    (void)bounded_size(before->size, request.descriptor.maximum_source_bytes);
    auto source = filesystem.read(request.source_path, request.descriptor.maximum_source_bytes);
    const auto source_hash = filesystem.content_hash(request.source_path);
    const auto after = filesystem.metadata(request.source_path);
    const auto source_key = io::metadata_cache_key(*before);
    if (!after || source.size() != before->size ||
        source_key != io::metadata_cache_key(*after) || core::fnv1a64(source) != source_hash) {
        reject(AssetError::source_changed, "asset source changed while it was read");
    }
    ResourcePayload payload;
    if (request.descriptor.decode) {
        payload = request.descriptor.decode({request.ticket.asset, request.ticket.revision,
            request.source_path, source, request.dependencies,
            request.descriptor.maximum_resource_bytes});
    } else {
        payload.bytes = std::move(source);
        payload.format = request.descriptor.name;
    }
    if (payload.bytes.capacity() > request.descriptor.maximum_resource_bytes) {
        reject(AssetError::limit_exceeded, "imported byte allocation exceeds its type limit");
    }
    require_text(payload.format, 256, true);
    payload.checksum = core::fnv1a64(payload.bytes);
    return {request.ticket, *after, source_key, source_hash,
            request.dependencies, std::move(payload)};
}

struct ResourceCache::Impl {
    struct Slot {
        ResourceTicket ticket;
        ResourceState state{ResourceState::pending};
        std::size_t reserved{};
        std::uint32_t pins{};
        std::uint64_t access{};
        std::uint64_t generation{1};
        ResourcePayload payload;
        bool occupied{};
        bool exhausted{};
    };

    explicit Impl(CacheLimits requested) : limits(requested) {
        if (limits.max_resources == 0 ||
            limits.max_resources >= std::numeric_limits<std::uint32_t>::max() ||
            limits.max_resource_bytes == 0) {
            reject(AssetError::invalid_argument, "invalid resource cache limits");
        }
    }

    Slot* locate(ResourceHandle handle) noexcept {
        if (handle.slot >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[handle.slot];
        return slot.occupied && slot.ticket.handle == handle ? &slot : nullptr;
    }

    const Slot* locate(ResourceHandle handle) const noexcept {
        if (handle.slot >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[handle.slot];
        return slot.occupied && slot.ticket.handle == handle ? &slot : nullptr;
    }

    std::size_t reserved() const noexcept {
        std::size_t result{};
        for (const auto& slot : slots) {
            if (slot.occupied) {
                result += slot.reserved;
            }
        }
        return result;
    }

    void touch(Slot& slot) noexcept {
        if (clock == std::numeric_limits<std::uint64_t>::max()) {
            for (auto& entry : slots) {
                entry.access = 0;
            }
            clock = 0;
        }
        slot.access = ++clock;
    }

    static ResourceInfo describe(const Slot& slot) {
        return {slot.ticket, slot.state, slot.reserved, slot.payload.bytes.size(),
                slot.pins, slot.access, slot.payload.checksum, slot.payload.format};
    }

    void release_slot(Slot& slot) noexcept {
        slot.payload = {};
        slot.reserved = 0;
        slot.pins = 0;
        slot.occupied = false;
        if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
            slot.exhausted = true;
        } else {
            ++slot.generation;
        }
    }

    Slot* eviction_candidate(const Slot* excluded = nullptr) noexcept {
        Slot* candidate = nullptr;
        for (auto& slot : slots) {
            if (&slot != excluded && slot.occupied && slot.pins == 0 &&
                slot.state == ResourceState::resident &&
                (candidate == nullptr || slot.access < candidate->access)) {
                candidate = &slot;
            }
        }
        return candidate;
    }

    bool make_room(std::size_t additional, const Slot* excluded = nullptr) {
        const auto used = reserved();
        if (additional <= limits.budget_bytes - used) {
            return true;
        }
        const auto needed = additional - (limits.budget_bytes - used);
        std::size_t reclaimable{};
        for (const auto& slot : slots) {
            if (&slot != excluded && slot.occupied && slot.pins == 0 &&
                slot.state == ResourceState::resident) {
                reclaimable += slot.reserved;
            }
        }
        if (needed > reclaimable) {
            return false;
        }
        std::size_t reclaimed{};
        while (reclaimed < needed) {
            auto* candidate = eviction_candidate(excluded);
            if (candidate == nullptr) {
                return false;
            }
            reclaimed += candidate->reserved;
            candidate->state = ResourceState::retiring;
            release_slot(*candidate);
            increment_stat(evictions);
        }
        return true;
    }

    CacheLimits limits;
    std::vector<Slot> slots;
    std::uint64_t next_request{1};
    std::uint64_t clock{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t evictions{};
};

ResourceCache::ResourceCache(CacheLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
ResourceCache::~ResourceCache() = default;

ResourceAdmission ResourceCache::admit(AssetId asset, AssetRevision revision,
                                       AssetTypeId type, std::size_t bytes_count) {
    if (!asset || !revision || type == 0 || bytes_count > impl_->limits.max_resource_bytes) {
        reject(AssetError::invalid_argument, "invalid resource admission");
    }
    for (auto& slot : impl_->slots) {
        if (slot.occupied && slot.ticket.asset == asset && slot.ticket.revision == revision &&
            slot.ticket.handle.type == type) {
            if (slot.state == ResourceState::resident) {
                impl_->touch(slot);
                increment_stat(impl_->hits);
                return {slot.ticket, true};
            }
            reject(AssetError::busy, "this resource version already has a live reservation");
        }
    }
    if (impl_->next_request == 0) {
        reject(AssetError::exhausted, "resource request generations exhausted");
    }
    auto index = impl_->slots.size();
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        if (!impl_->slots[i].occupied && !impl_->slots[i].exhausted) {
            index = i;
            break;
        }
    }
    if (index == impl_->slots.size()) {
        if (index >= impl_->limits.max_resources) {
            auto* candidate = impl_->eviction_candidate();
            if (candidate == nullptr || candidate->generation == std::numeric_limits<std::uint64_t>::max()) {
                reject(AssetError::limit_exceeded, "resource slots exhausted");
            }
            index = static_cast<std::size_t>(candidate - impl_->slots.data());
            if (!impl_->make_room(bytes_count > candidate->reserved ? bytes_count - candidate->reserved : 0,
                                  candidate)) {
                reject(AssetError::budget_exceeded, "resource admission exceeds available budget");
            }
            impl_->release_slot(impl_->slots[index]);
            increment_stat(impl_->evictions);
        } else {
            impl_->slots.emplace_back();
        }
    }
    auto& slot = impl_->slots[index];
    if (!impl_->make_room(bytes_count, &slot)) {
        reject(AssetError::budget_exceeded, "resource admission exceeds available budget");
    }
    slot.ticket = {{static_cast<std::uint32_t>(index), slot.generation, type}, asset, revision,
                   take_counter(impl_->next_request, "resource requests")};
    slot.state = ResourceState::pending;
    slot.reserved = bytes_count;
    slot.pins = 0;
    slot.occupied = true;
    impl_->touch(slot);
    increment_stat(impl_->misses);
    return {slot.ticket, false};
}

bool ResourceCache::publish(const ResourceTicket& ticket, ResourcePayload payload) {
    auto* slot = impl_->locate(ticket.handle);
    if (slot == nullptr || slot->ticket != ticket || slot->state == ResourceState::retiring) {
        return false;
    }
    if (payload.bytes.capacity() > impl_->limits.max_resource_bytes ||
        payload.bytes.capacity() > slot->reserved || payload.format.size() > 256 ||
        !core::valid_utf8(payload.format)) {
        return false;
    }
    if (slot->state == ResourceState::resident) {
        return payload.bytes == slot->payload.bytes && payload.format == slot->payload.format;
    }
    payload.checksum = core::fnv1a64(payload.bytes);
    slot->reserved = payload.bytes.capacity();
    slot->payload = std::move(payload);
    slot->state = ResourceState::resident;
    impl_->touch(*slot);
    return true;
}

bool ResourceCache::cancel(const ResourceTicket& ticket) {
    auto* slot = impl_->locate(ticket.handle);
    if (slot == nullptr || slot->ticket != ticket || slot->state != ResourceState::pending) {
        return false;
    }
    slot->state = ResourceState::retiring;
    return true;
}

std::optional<ResourceTicket> ResourceCache::retire(ResourceHandle handle) {
    auto* slot = impl_->locate(handle);
    if (slot == nullptr || slot->pins != 0 || slot->state == ResourceState::retiring) {
        return std::nullopt;
    }
    slot->state = ResourceState::retiring;
    return slot->ticket;
}

bool ResourceCache::acknowledge_retirement(const ResourceTicket& ticket) {
    auto* slot = impl_->locate(ticket.handle);
    if (slot == nullptr || slot->ticket != ticket ||
        slot->state != ResourceState::retiring || slot->pins != 0) {
        return false;
    }
    impl_->release_slot(*slot);
    return true;
}

bool ResourceCache::pin(ResourceHandle handle) {
    auto* slot = impl_->locate(handle);
    if (slot == nullptr || slot->state != ResourceState::resident ||
        slot->pins == std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    ++slot->pins;
    impl_->touch(*slot);
    return true;
}

bool ResourceCache::unpin(ResourceHandle handle) {
    auto* slot = impl_->locate(handle);
    if (slot == nullptr || slot->pins == 0) {
        return false;
    }
    --slot->pins;
    return true;
}

std::optional<ResourceHandle> ResourceCache::find(AssetId asset, AssetRevision revision,
                                                 AssetTypeId type) {
    for (auto& slot : impl_->slots) {
        if (slot.occupied && slot.state == ResourceState::resident &&
            slot.ticket.asset == asset && slot.ticket.revision == revision &&
            slot.ticket.handle.type == type) {
            impl_->touch(slot);
            increment_stat(impl_->hits);
            return slot.ticket.handle;
        }
    }
    increment_stat(impl_->misses);
    return std::nullopt;
}

std::optional<ResourceInfo> ResourceCache::inspect(ResourceHandle handle) const {
    const auto* slot = impl_->locate(handle);
    return slot == nullptr ? std::nullopt : std::optional<ResourceInfo>{Impl::describe(*slot)};
}

std::vector<ResourceInfo> ResourceCache::entries() const {
    std::vector<ResourceInfo> result;
    for (const auto& slot : impl_->slots) {
        if (slot.occupied) {
            result.push_back(Impl::describe(slot));
        }
    }
    return result;
}

std::span<const std::byte> ResourceCache::bytes(ResourceHandle handle) {
    auto* slot = impl_->locate(handle);
    if (slot == nullptr || slot->state != ResourceState::resident) {
        reject(AssetError::invalid_handle, "resource handle is stale or not resident");
    }
    impl_->touch(*slot);
    return slot->payload.bytes;
}

CacheStatistics ResourceCache::statistics() const noexcept {
    CacheStatistics result;
    result.budget_bytes = impl_->limits.budget_bytes;
    result.hits = impl_->hits;
    result.misses = impl_->misses;
    result.evictions = impl_->evictions;
    for (const auto& slot : impl_->slots) {
        if (!slot.occupied) {
            continue;
        }
        ++result.resources;
        result.pinned_resources += slot.pins != 0 ? 1U : 0U;
        switch (slot.state) {
        case ResourceState::pending: result.pending_bytes += slot.reserved; break;
        case ResourceState::resident: result.resident_bytes += slot.reserved; break;
        case ResourceState::retiring: result.retiring_bytes += slot.reserved; break;
        }
    }
    return result;
}

bool ResourceCache::set_budget(std::size_t bytes_count) {
    std::size_t fixed{};
    for (const auto& slot : impl_->slots) {
        if (slot.occupied && (slot.pins != 0 || slot.state != ResourceState::resident)) {
            fixed += slot.reserved;
        }
    }
    if (fixed > bytes_count) {
        return false;
    }
    (void)trim(bytes_count);
    impl_->limits.budget_bytes = bytes_count;
    return true;
}

std::size_t ResourceCache::trim(std::size_t target_reserved_bytes) {
    std::size_t reclaimed{};
    auto used = impl_->reserved();
    while (used > target_reserved_bytes) {
        auto* candidate = impl_->eviction_candidate();
        if (candidate == nullptr) {
            break;
        }
        used -= candidate->reserved;
        reclaimed += candidate->reserved;
        candidate->state = ResourceState::retiring;
        impl_->release_slot(*candidate);
        increment_stat(impl_->evictions);
    }
    return reclaimed;
}

struct AssetCatalog::Impl {
    using Records = std::map<AssetId, AssetRecord>;

    Impl(io::VirtualFileSystem& source, CatalogLimits requested)
        : filesystem(source), limits(requested) {
        if (limits.max_assets == 0 || limits.max_assets > std::numeric_limits<std::uint32_t>::max() ||
            limits.max_types == 0 || limits.max_path_bytes == 0 || limits.max_name_bytes == 0 ||
            limits.max_source_bytes == 0 || limits.max_resource_bytes == 0 ||
            limits.max_catalog_bytes < 44 || limits.max_error_bytes == 0 ||
            limits.change_history == 0 || limits.max_tags_per_asset > std::numeric_limits<std::uint32_t>::max() ||
            limits.max_dependencies_per_asset > std::numeric_limits<std::uint32_t>::max()) {
            reject(AssetError::invalid_argument, "invalid asset catalog limits");
        }
        history.reserve(limits.change_history);
    }

    AssetRecord& get(AssetId asset) {
        const auto found = records.find(asset);
        if (found == records.end()) {
            reject(AssetError::unknown_asset, "unknown asset " + std::to_string(asset.value));
        }
        return found->second;
    }

    const AssetRecord& get(AssetId asset) const {
        const auto found = records.find(asset);
        if (found == records.end()) {
            reject(AssetError::unknown_asset, "unknown asset " + std::to_string(asset.value));
        }
        return found->second;
    }

    void ensure_sequences(std::size_t count, bool revisions = false) const {
        if (count > std::numeric_limits<std::uint64_t>::max() - sequence ||
            (revisions && (next_revision == 0 ||
             count > std::numeric_limits<std::uint64_t>::max() - next_revision + 1))) {
            reject(AssetError::exhausted, "catalog revision or change sequence exhausted");
        }
    }

    void emit(AssetId asset, AssetRevision revision, AssetChangeKind kind) noexcept {
        const AssetChange change{++sequence, asset, revision, kind};
        if (history.size() < limits.change_history) {
            history.push_back(change);
        } else {
            history[history_next] = change;
            history_next = (history_next + 1) % history.size();
        }
    }

    void normalize(AssetDefinition& definition) const {
        if (types.find(definition.type) == types.end()) {
            reject(AssetError::unknown_type, "asset type is not registered");
        }
        definition.source_path = io::normalize_virtual_path(definition.source_path, limits.max_path_bytes);
        if (definition.source_path == "/") {
            reject(AssetError::invalid_argument, "an asset source cannot be the virtual root");
        }
        if (definition.name.empty()) {
            definition.name = io::virtual_filename(definition.source_path);
        }
        require_text(definition.name, limits.max_name_bytes);
        canonical_tags(definition.tags, limits);
        if (definition.dependencies.size() > limits.max_dependencies_per_asset) {
            reject(AssetError::limit_exceeded, "too many asset dependencies");
        }
        std::sort(definition.dependencies.begin(), definition.dependencies.end());
        if (std::adjacent_find(definition.dependencies.begin(), definition.dependencies.end()) !=
            definition.dependencies.end()) {
            reject(AssetError::duplicate, "duplicate asset dependency");
        }
        const auto& extensions = types.at(definition.type).extensions;
        const auto extension = lower_ascii(io::virtual_extension(definition.source_path));
        if (!extensions.empty() && std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
            reject(AssetError::invalid_argument, "asset source extension does not match its type");
        }
    }

    void validate_links(const AssetDefinition& definition, const Records& source) const {
        std::size_t edges = definition.dependencies.size();
        for (const auto& [id, record] : source) {
            if (id != definition.id) {
                if (record.definition.source_path == definition.source_path) {
                    reject(AssetError::duplicate, "source path already belongs to an asset");
                }
                if (record.definition.dependencies.size() > limits.max_dependency_edges -
                    std::min(edges, limits.max_dependency_edges)) {
                    reject(AssetError::limit_exceeded, "catalog dependency edge budget exceeded");
                }
                edges += record.definition.dependencies.size();
            }
        }
        if (edges > limits.max_dependency_edges) {
            reject(AssetError::limit_exceeded, "catalog dependency edge budget exceeded");
        }
        std::set<AssetId> visited;
        std::vector<AssetId> pending(definition.dependencies);
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (current == definition.id) {
                reject(AssetError::dependency_cycle, "asset dependency would create a cycle");
            }
            if (!visited.insert(current).second) {
                continue;
            }
            const auto found = source.find(current);
            if (!current || found == source.end()) {
                reject(AssetError::unknown_asset, "dependency refers to an unknown asset");
            }
            for (const auto dependency : found->second.definition.dependencies) {
                pending.push_back(dependency);
            }
        }
    }

    std::vector<AssetId> closure(std::span<const AssetId> roots) const {
        std::map<AssetId, std::vector<AssetId>> reverse;
        for (const auto& [id, record] : records) {
            for (const auto dependency : record.definition.dependencies) {
                reverse[dependency].push_back(id);
            }
        }
        std::set<AssetId> seen;
        std::vector<AssetId> result;
        for (const auto root : roots) {
            (void)get(root);
            if (seen.insert(root).second) {
                result.push_back(root);
            }
        }
        for (std::size_t index = 0; index < result.size(); ++index) {
            const auto found = reverse.find(result[index]);
            if (found == reverse.end()) {
                continue;
            }
            for (const auto dependent : found->second) {
                if (seen.insert(dependent).second) {
                    result.push_back(dependent);
                }
            }
        }
        return result;
    }

    void invalidate_records(std::span<const AssetId> affected) {
        ensure_sequences(affected.size(), true);
        for (const auto id : affected) {
            auto& record = get(id);
            record.revision = {take_counter(next_revision, "asset revisions")};
            record.status = ImportStatus::stale;
            record.request_generation = 0;
            emit(id, record.revision, AssetChangeKind::invalidated);
        }
    }

    bool matches(const ImportTicket& ticket) const {
        const auto found = records.find(ticket.asset);
        return ticket.generation != 0 && found != records.end() &&
               found->second.revision == ticket.revision &&
               found->second.definition.type == ticket.type &&
               found->second.request_generation == ticket.generation &&
               found->second.status == ImportStatus::importing;
    }

    io::VirtualFileSystem& filesystem;
    CatalogLimits limits;
    Records records;
    std::map<AssetTypeId, AssetTypeDescriptor> types;
    std::vector<AssetChange> history;
    std::size_t history_next{};
    std::uint64_t next_id{1};
    std::uint64_t next_revision{1};
    std::uint64_t next_import{1};
    std::uint64_t sequence{};
};

AssetCatalog::AssetCatalog(io::VirtualFileSystem& filesystem, CatalogLimits limits)
    : impl_(std::make_unique<Impl>(filesystem, limits)) {
    register_type({binary_asset_type, "binary", {}, limits.max_source_bytes,
                   limits.max_resource_bytes, {}});
}

AssetCatalog::~AssetCatalog() = default;
const CatalogLimits& AssetCatalog::limits() const noexcept { return impl_->limits; }

void AssetCatalog::register_type(AssetTypeDescriptor descriptor) {
    if (descriptor.id == 0 || descriptor.maximum_source_bytes == 0 ||
        descriptor.maximum_source_bytes > impl_->limits.max_source_bytes ||
        descriptor.maximum_resource_bytes == 0 ||
        descriptor.maximum_resource_bytes > impl_->limits.max_resource_bytes ||
        descriptor.extensions.size() > 64) {
        reject(AssetError::invalid_argument, "invalid asset type descriptor");
    }
    require_text(descriptor.name, impl_->limits.max_name_bytes);
    if (impl_->types.size() >= impl_->limits.max_types) {
        reject(AssetError::limit_exceeded, "asset type limit exceeded");
    }
    for (const auto& [id, existing] : impl_->types) {
        if (id == descriptor.id || lower_ascii(existing.name) == lower_ascii(descriptor.name)) {
            reject(AssetError::duplicate, "asset type id or name is already registered");
        }
    }
    for (auto& extension : descriptor.extensions) {
        require_text(extension, 32);
        extension = lower_ascii(extension);
        if (extension.front() != '.' || extension.find_first_of("/\\:") != extension.npos) {
            reject(AssetError::invalid_argument, "asset extension must begin with a dot");
        }
    }
    std::sort(descriptor.extensions.begin(), descriptor.extensions.end());
    descriptor.extensions.erase(std::unique(descriptor.extensions.begin(), descriptor.extensions.end()),
                                descriptor.extensions.end());
    impl_->types.emplace(descriptor.id, std::move(descriptor));
}

std::vector<AssetTypeDescriptor> AssetCatalog::types() const {
    std::vector<AssetTypeDescriptor> result;
    result.reserve(impl_->types.size());
    for (const auto& [id, descriptor] : impl_->types) {
        (void)id;
        result.push_back(descriptor);
    }
    return result;
}

AssetId AssetCatalog::add(AssetDefinition definition) {
    if (impl_->records.size() >= impl_->limits.max_assets) {
        reject(AssetError::limit_exceeded, "asset catalog is full");
    }
    impl_->normalize(definition);
    if (!definition.id) {
        if (impl_->next_id == 0) {
            reject(AssetError::exhausted, "asset identities exhausted");
        }
        definition.id = {impl_->next_id};
    }
    if (impl_->records.contains(definition.id)) {
        reject(AssetError::duplicate, "asset identity is already registered");
    }
    impl_->validate_links(definition, impl_->records);
    impl_->ensure_sequences(1, true);
    const auto id = definition.id;
    AssetRecord record;
    record.definition = std::move(definition);
    record.revision = {impl_->next_revision};
    impl_->records.emplace(id, std::move(record));
    (void)take_counter(impl_->next_revision, "asset revisions");
    if (impl_->next_id != 0 && id.value >= impl_->next_id) {
        impl_->next_id = id.value == std::numeric_limits<std::uint64_t>::max() ? 0 : id.value + 1;
    }
    impl_->emit(id, impl_->get(id).revision, AssetChangeKind::registered);
    return id;
}

bool AssetCatalog::erase(AssetId asset) {
    const auto found = impl_->records.find(asset);
    if (found == impl_->records.end()) {
        return false;
    }
    for (const auto& [id, record] : impl_->records) {
        (void)id;
        if (std::binary_search(record.definition.dependencies.begin(), record.definition.dependencies.end(), asset)) {
            reject(AssetError::dependency_in_use, "asset is referenced by another asset");
        }
    }
    impl_->ensure_sequences(1);
    const auto revision = found->second.revision;
    impl_->records.erase(found);
    impl_->emit(asset, revision, AssetChangeKind::erased);
    return true;
}

void AssetCatalog::update(AssetDefinition definition) {
    auto& record = impl_->get(definition.id);
    impl_->normalize(definition);
    impl_->validate_links(definition, impl_->records);
    const auto affected = impl_->closure(std::span<const AssetId>(&definition.id, 1));
    std::size_t event_count{};
    if (!core::checked_add(affected.size(), 1, event_count)) {
        reject(AssetError::exhausted, "asset update change count overflowed");
    }
    impl_->ensure_sequences(affected.size(), true);
    impl_->ensure_sequences(event_count);
    record.definition = std::move(definition);
    impl_->invalidate_records(affected);
    impl_->emit(record.definition.id, record.revision, AssetChangeKind::updated);
}

std::optional<AssetRecord> AssetCatalog::inspect(AssetId asset) const {
    const auto found = impl_->records.find(asset);
    return found == impl_->records.end() ? std::nullopt : std::optional<AssetRecord>{found->second};
}

std::optional<AssetId> AssetCatalog::find_path(std::string_view path) const {
    const auto canonical = io::normalize_virtual_path(path, impl_->limits.max_path_bytes);
    for (const auto& [id, record] : impl_->records) {
        if (record.definition.source_path == canonical) {
            return id;
        }
    }
    return std::nullopt;
}

std::vector<AssetRecord> AssetCatalog::search(const AssetQuery& query) const {
    require_text(query.text, impl_->limits.max_path_bytes, true);
    auto tags = query.tags;
    canonical_tags(tags, impl_->limits);
    const auto text = lower_ascii(query.text);
    const auto maximum = std::min(query.limit, impl_->limits.max_assets);
    std::vector<AssetRecord> result;
    std::size_t skipped{};
    for (const auto& [id, record] : impl_->records) {
        (void)id;
        if (result.size() >= maximum) {
            break;
        }
        if ((query.type && record.definition.type != *query.type) ||
            (query.status && record.status != *query.status) ||
            !std::includes(record.definition.tags.begin(), record.definition.tags.end(), tags.begin(), tags.end())) {
            continue;
        }
        if (!text.empty() && lower_ascii(record.definition.name).find(text) == std::string::npos &&
            lower_ascii(record.definition.source_path).find(text) == std::string::npos) {
            continue;
        }
        if (skipped < query.offset) {
            ++skipped;
        } else {
            result.push_back(record);
        }
    }
    return result;
}

std::vector<AssetId> AssetCatalog::dependency_order(AssetId asset) const {
    (void)impl_->get(asset);
    std::map<AssetId, std::uint8_t> colors;
    std::vector<std::pair<AssetId, std::size_t>> stack{{asset, 0}};
    std::vector<AssetId> result;
    colors[asset] = 1;
    while (!stack.empty()) {
        auto& frame = stack.back();
        const auto& dependencies = impl_->get(frame.first).definition.dependencies;
        if (frame.second == dependencies.size()) {
            colors[frame.first] = 2;
            result.push_back(frame.first);
            stack.pop_back();
            continue;
        }
        const auto dependency = dependencies[frame.second++];
        const auto color = colors[dependency];
        if (color == 1) {
            reject(AssetError::dependency_cycle, "asset catalog contains a dependency cycle");
        }
        if (color == 0) {
            colors[dependency] = 1;
            stack.emplace_back(dependency, 0);
        }
    }
    return result;
}

std::vector<AssetId> AssetCatalog::dependents(AssetId asset, bool transitive) const {
    (void)impl_->get(asset);
    if (transitive) {
        auto result = impl_->closure(std::span<const AssetId>(&asset, 1));
        result.erase(result.begin());
        return result;
    }
    std::vector<AssetId> result;
    for (const auto& [id, record] : impl_->records) {
        if (std::binary_search(record.definition.dependencies.begin(), record.definition.dependencies.end(), asset)) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<AssetId> AssetCatalog::invalidate(AssetId asset) {
    auto result = impl_->closure(std::span<const AssetId>(&asset, 1));
    impl_->invalidate_records(result);
    return result;
}

std::size_t AssetCatalog::apply_file_changes(std::span<const io::FileChange> changes) {
    if (changes.size() > impl_->limits.change_history) {
        reject(AssetError::limit_exceeded, "asset file-change batch exceeds its bound");
    }
    std::set<AssetId> unique;
    std::set<AssetId> removed;
    for (const auto& change : changes) {
        const auto canonical = io::normalize_virtual_path(change.path, impl_->limits.max_path_bytes);
        for (const auto& [id, record] : impl_->records) {
            if (io::virtual_contains(canonical, record.definition.source_path)) {
                unique.insert(id);
                if (change.kind == io::ChangeKind::removed) {
                    removed.insert(id);
                } else {
                    removed.erase(id);
                }
            }
        }
    }
    const std::vector<AssetId> roots(unique.begin(), unique.end());
    const auto affected = impl_->closure(roots);
    impl_->invalidate_records(affected);
    for (const auto asset : removed) {
        impl_->get(asset).status = ImportStatus::missing;
    }
    return affected.size();
}

ImportRequest AssetCatalog::begin_import(AssetId asset) {
    auto& record = impl_->get(asset);
    if (record.status == ImportStatus::importing) {
        reject(AssetError::busy, "asset already has an active import");
    }
    ImportRequest request;
    request.source_path = record.definition.source_path;
    request.descriptor = impl_->types.at(record.definition.type);
    for (const auto dependency : record.definition.dependencies) {
        const auto& source = impl_->get(dependency);
        if (source.status != ImportStatus::ready || source.last_good_revision != source.revision) {
            reject(AssetError::busy, "asset dependency has no current imported version");
        }
        request.dependencies.push_back({dependency, source.revision});
    }
    impl_->ensure_sequences(1);
    const auto generation = take_counter(impl_->next_import, "asset import generations");
    request.ticket = {asset, record.revision, record.definition.type, generation};
    record.request_generation = generation;
    record.status = ImportStatus::importing;
    impl_->emit(asset, record.revision, AssetChangeKind::updated);
    return request;
}

bool AssetCatalog::fail_import(const ImportTicket& ticket, std::string error) {
    if (!impl_->matches(ticket)) {
        return false;
    }
    if (!core::valid_utf8(error)) {
        error = "import failed with an invalid UTF-8 diagnostic";
    }
    if (error.size() > impl_->limits.max_error_bytes) {
        error.resize(impl_->limits.max_error_bytes);
        while (!error.empty() && !core::valid_utf8(error)) {
            error.pop_back();
        }
    }
    for (char& character : error) {
        if (static_cast<unsigned char>(character) < 32U || character == '\x7f') {
            character = ' ';
        }
    }
    impl_->ensure_sequences(1);
    auto& record = impl_->get(ticket.asset);
    record.last_error = std::move(error);
    record.status = ImportStatus::failed;
    impl_->emit(ticket.asset, record.revision, AssetChangeKind::failed);
    return true;
}

ResourceHandle AssetCatalog::publish_import(ImportProduct product, ResourceCache& cache,
                                            const ResourceTicket& admission) {
    if (!impl_->matches(product.ticket) || admission.asset != product.ticket.asset ||
        admission.revision != product.ticket.revision || admission.handle.type != product.ticket.type) {
        reject(AssetError::stale_request, "asset publication token no longer matches");
    }
    auto& record = impl_->get(product.ticket.asset);
    const auto& descriptor = impl_->types.at(record.definition.type);
    if (product.dependencies.size() != record.definition.dependencies.size()) {
        reject(AssetError::stale_request, "asset dependency set changed before publication");
    }
    for (std::size_t i = 0; i < product.dependencies.size(); ++i) {
        const auto& expected = impl_->get(record.definition.dependencies[i]);
        if (product.dependencies[i].asset != expected.definition.id ||
            product.dependencies[i].revision != expected.revision ||
            expected.status != ImportStatus::ready || expected.last_good_revision != expected.revision) {
            reject(AssetError::stale_request, "asset dependency revision changed before publication");
        }
    }
    if (product.source.path != record.definition.source_path || product.source.kind != io::FileKind::regular ||
        product.source.size > descriptor.maximum_source_bytes ||
        product.payload.bytes.capacity() > descriptor.maximum_resource_bytes ||
        product.source_cache_key != io::metadata_cache_key(product.source) ||
        impl_->filesystem.cache_key(record.definition.source_path) != product.source_cache_key) {
        reject(AssetError::source_changed, "asset source metadata no longer matches imported data");
    }
    if (record.last_good_revision == record.revision && record.source_hash != product.source_hash) {
        reject(AssetError::source_changed, "changed source requires a new asset revision");
    }
    impl_->ensure_sequences(1);
    const auto resource_size = product.payload.bytes.size();
    if (!cache.publish(admission, std::move(product.payload))) {
        reject(AssetError::budget_exceeded, "resource publication rejected its token or reserved allocation");
    }
    record.source_cache_key = product.source_cache_key;
    record.source_hash = product.source_hash;
    record.source_bytes = product.source.size;
    record.resource_bytes = static_cast<std::uint64_t>(resource_size);
    record.last_good_revision = record.revision;
    record.last_error.clear();
    record.status = ImportStatus::ready;
    impl_->emit(record.definition.id, record.revision, AssetChangeKind::imported);
    return admission.handle;
}

ResourceHandle AssetCatalog::load(AssetId asset, ResourceCache& cache) {
    const auto order = dependency_order(asset);
    PinSet retained(cache, order.size());
    for (const auto id : order) {
        const auto& record = impl_->get(id);
        if (record.last_good_revision) {
            const auto old = cache.find(id, record.last_good_revision, record.definition.type);
            if (old) {
                retained.add(*old);
            }
        }
    }
    ResourceHandle result;
    for (const auto id : order) {
        auto& record = impl_->get(id);
        std::optional<io::FileMetadata> metadata;
        try {
            metadata = impl_->filesystem.metadata(record.definition.source_path);
            if (record.last_good_revision == record.revision &&
                (!metadata || metadata->kind != io::FileKind::regular ||
                 io::metadata_cache_key(*metadata) != record.source_cache_key ||
                 impl_->filesystem.content_hash(record.definition.source_path) != record.source_hash)) {
                (void)invalidate(id);
            }
        } catch (const std::exception& error) {
            const auto failed = begin_import(id);
            (void)fail_import(failed.ticket, error.what());
            throw;
        }
        if (record.status == ImportStatus::ready && record.last_good_revision == record.revision) {
            const auto resident = cache.find(id, record.revision, record.definition.type);
            if (resident) {
                result = *resident;
                retained.add(result);
                continue;
            }
        }
        const auto request = begin_import(id);
        std::optional<ResourceAdmission> admission;
        try {
            if (!metadata || metadata->kind != io::FileKind::regular) {
                reject(AssetError::source_missing, "asset source is missing");
            }
            const auto reservation = request.descriptor.decode ? request.descriptor.maximum_resource_bytes :
                bounded_size(metadata->size, request.descriptor.maximum_resource_bytes);
            admission = cache.admit(id, request.ticket.revision, request.ticket.type, reservation);
            auto product = import_source(impl_->filesystem, request);
            result = publish_import(std::move(product), cache, admission->ticket);
            retained.add(result);
        } catch (const std::exception& error) {
            if (admission && !admission->already_resident && cache.cancel(admission->ticket)) {
                (void)cache.acknowledge_retirement(admission->ticket);
            }
            (void)fail_import(request.ticket, error.what());
            throw;
        } catch (...) {
            if (admission && !admission->already_resident && cache.cancel(admission->ticket)) {
                (void)cache.acknowledge_retirement(admission->ticket);
            }
            (void)fail_import(request.ticket, "asset decoder raised an unknown exception");
            throw;
        }
    }
    return result;
}

AssetChangeBatch AssetCatalog::changes_since(std::uint64_t sequence, std::size_t limit) const {
    AssetChangeBatch result;
    result.latest_sequence = impl_->sequence;
    if (impl_->history.empty()) {
        result.history_lost = sequence > impl_->sequence;
        return result;
    }
    const auto begin = impl_->history.size() == impl_->limits.change_history ? impl_->history_next : 0;
    const auto earliest = impl_->history[begin].sequence;
    result.history_lost = sequence > impl_->sequence || (sequence < earliest && earliest - sequence > 1);
    const auto maximum = std::min(limit, impl_->limits.change_history);
    for (std::size_t i = 0; i < impl_->history.size() && result.changes.size() < maximum; ++i) {
        const auto& change = impl_->history[(begin + i) % impl_->history.size()];
        if (change.sequence > sequence) {
            result.changes.push_back(change);
        }
    }
    return result;
}

std::vector<std::byte> AssetCatalog::encode() const {
    core::ByteWriter writer(impl_->limits.max_catalog_bytes);
    writer.write_bytes("GECAT001");
    writer.write_u32(1);
    writer.write_u32(0);
    writer.write_u64(impl_->next_id);
    writer.write_u64(impl_->next_revision);
    writer.write_u32(static_cast<std::uint32_t>(impl_->records.size()));
    writer.write_u32(0);
    for (const auto& [id, record] : impl_->records) {
        writer.write_u64(id.value);
        writer.write_u32(record.definition.type);
        writer.write_u64(record.revision.value);
        writer.write_u64(record.last_good_revision.value);
        writer.write_u64(record.source_cache_key);
        writer.write_u64(record.source_hash);
        writer.write_u64(record.source_bytes);
        writer.write_u64(record.resource_bytes);
        const auto status = record.status == ImportStatus::importing ? ImportStatus::stale : record.status;
        writer.write_u8(static_cast<std::uint8_t>(status));
        writer.write_string(record.definition.source_path, impl_->limits.max_path_bytes);
        writer.write_string(record.definition.name, impl_->limits.max_name_bytes);
        writer.write_string(record.last_error, impl_->limits.max_error_bytes);
        writer.write_u32(static_cast<std::uint32_t>(record.definition.tags.size()));
        for (const auto& tag : record.definition.tags) {
            writer.write_string(tag, impl_->limits.max_tag_bytes);
        }
        writer.write_u32(static_cast<std::uint32_t>(record.definition.dependencies.size()));
        for (const auto dependency : record.definition.dependencies) {
            writer.write_u64(dependency.value);
        }
    }
    writer.write_u32(core::crc32(writer.bytes()));
    return writer.take();
}

void AssetCatalog::restore(std::span<const std::byte> bytes) {
    if (bytes.size() < 44 || bytes.size() > impl_->limits.max_catalog_bytes) {
        reject(AssetError::malformed_catalog, "catalog byte length is invalid");
    }
    core::BinaryLimits binary_limits;
    binary_limits.max_total_bytes = impl_->limits.max_catalog_bytes;
    binary_limits.max_string_bytes = std::max({impl_->limits.max_path_bytes, impl_->limits.max_name_bytes,
                                              impl_->limits.max_tag_bytes, impl_->limits.max_error_bytes});
    core::ByteReader checksum_reader(bytes.last(4), core::ByteOrder::little, binary_limits);
    if (checksum_reader.read_u32() != core::crc32(bytes.first(bytes.size() - 4))) {
        reject(AssetError::malformed_catalog, "catalog checksum mismatch");
    }
    core::ByteReader reader(bytes.first(bytes.size() - 4), core::ByteOrder::little, binary_limits);
    reader.expect("GECAT001");
    if (reader.read_u32() != 1 || reader.read_u32() != 0) {
        reject(AssetError::malformed_catalog, "unsupported catalog version or flags");
    }
    const auto saved_next_id = reader.read_u64();
    const auto saved_next_revision = reader.read_u64();
    const auto count = bounded_size(reader.read_u32(), impl_->limits.max_assets);
    reader.require_zero(4);
    Impl::Records candidate;
    std::uint64_t previous_id{};
    std::uint64_t greatest_revision{};
    std::size_t total_edges{};
    for (std::size_t i = 0; i < count; ++i) {
        AssetRecord record;
        record.definition.id = {reader.read_u64()};
        record.definition.type = reader.read_u32();
        record.revision = {reader.read_u64()};
        record.last_good_revision = {reader.read_u64()};
        record.source_cache_key = reader.read_u64();
        record.source_hash = reader.read_u64();
        record.source_bytes = reader.read_u64();
        record.resource_bytes = reader.read_u64();
        const auto status = reader.read_u8();
        if (record.definition.id.value <= previous_id || !record.revision ||
            record.last_good_revision > record.revision || status > static_cast<std::uint8_t>(ImportStatus::missing) ||
            status == static_cast<std::uint8_t>(ImportStatus::importing)) {
            reject(AssetError::malformed_catalog, "invalid catalog identity, revision, or status");
        }
        record.status = static_cast<ImportStatus>(status);
        record.definition.source_path = reader.read_string();
        record.definition.name = reader.read_string();
        record.last_error = reader.read_string();
        require_text(record.last_error, impl_->limits.max_error_bytes, true);
        const auto tags = bounded_size(reader.read_u32(), impl_->limits.max_tags_per_asset);
        for (std::size_t tag = 0; tag < tags; ++tag) {
            record.definition.tags.push_back(reader.read_string());
        }
        const auto edges = bounded_size(reader.read_u32(), impl_->limits.max_dependencies_per_asset);
        if (edges > impl_->limits.max_dependency_edges - total_edges) {
            reject(AssetError::limit_exceeded, "catalog dependency edge limit exceeded");
        }
        total_edges += edges;
        for (std::size_t edge = 0; edge < edges; ++edge) {
            record.definition.dependencies.push_back({reader.read_u64()});
        }
        auto normalized = record.definition;
        impl_->normalize(normalized);
        if (normalized.source_path != record.definition.source_path || normalized.name != record.definition.name ||
            normalized.tags != record.definition.tags || normalized.dependencies != record.definition.dependencies ||
            record.source_bytes > impl_->limits.max_source_bytes ||
            record.resource_bytes > impl_->limits.max_resource_bytes) {
            reject(AssetError::malformed_catalog, "catalog record is not canonical or exceeds limits");
        }
        previous_id = record.definition.id.value;
        greatest_revision = std::max(greatest_revision, record.revision.value);
        const auto id = record.definition.id;
        candidate.emplace(id, std::move(record));
    }
    reader.require_end();
    if ((saved_next_id != 0 && saved_next_id <= previous_id) ||
        (saved_next_revision != 0 && saved_next_revision <= greatest_revision)) {
        reject(AssetError::malformed_catalog, "catalog sequence counters precede stored records");
    }
    for (const auto& [id, record] : candidate) {
        (void)id;
        impl_->validate_links(record.definition, candidate);
    }
    if (impl_->next_revision == 0 || saved_next_revision == 0) {
        reject(AssetError::exhausted, "restored catalog cannot advance exhausted revisions");
    }
    auto revision = std::max(impl_->next_revision, saved_next_revision);
    if (count > std::numeric_limits<std::uint64_t>::max() - revision + 1) {
        reject(AssetError::exhausted, "catalog restore would exhaust revision identity");
    }
    impl_->ensure_sequences(1);
    for (auto& [id, record] : candidate) {
        (void)id;
        record.revision = {take_counter(revision, "restored revisions")};
        record.last_good_revision = {};
        record.request_generation = 0;
        if (record.status != ImportStatus::failed && record.status != ImportStatus::missing) {
            record.status = ImportStatus::stale;
        }
    }
    impl_->records.swap(candidate);
    impl_->next_revision = revision;
    impl_->next_id = impl_->next_id == 0 || saved_next_id == 0 ? 0 : std::max(impl_->next_id, saved_next_id);
    impl_->emit({}, {}, AssetChangeKind::restored);
}

void AssetCatalog::save(std::string_view virtual_path) const {
    const auto bytes = encode();
    impl_->filesystem.write(virtual_path, bytes, {true, true, {}});
}

void AssetCatalog::open(std::string_view virtual_path) {
    const auto bytes = impl_->filesystem.read(virtual_path, impl_->limits.max_catalog_bytes);
    restore(bytes);
}

namespace {
struct CatalogRuntime {
    std::uint64_t published_sequence{};
};
} // namespace

void register_catalog(core::Kernel& kernel) {
    if (kernel.services().contains<CatalogRuntime>()) {
        return;
    }
    auto* filesystem = kernel.services().find<io::VirtualFileSystem>();
    if (filesystem == nullptr) {
        throw std::logic_error("assets.catalog requires the virtual filesystem service");
    }
    (void)kernel.register_module({"assets.catalog", "Asset identities, imports and bounded byte residency", 1});
    (void)kernel.services().emplace<AssetCatalog>(*filesystem);
    CacheLimits cache_limits;
    const auto configured = kernel.config().integer("assets.budget_bytes", 256LL * 1024LL * 1024LL);
    if (configured >= 0 && static_cast<std::uint64_t>(configured) <= std::numeric_limits<std::size_t>::max()) {
        cache_limits.budget_bytes = static_cast<std::size_t>(configured);
    }
    (void)kernel.services().emplace<ResourceCache>(cache_limits);
    (void)kernel.services().emplace<CatalogRuntime>();
    (void)kernel.add_command("asset.register", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 3) {
            return core::CommandResult{false, "usage: asset.register path [type_id] [asset_id]"};
        }
        AssetDefinition definition;
        definition.source_path = args[0];
        if (args.size() >= 2) {
            const auto type = parse_unsigned(args[1]);
            if (type > std::numeric_limits<AssetTypeId>::max()) {
                reject(AssetError::invalid_argument, "asset type id is out of range");
            }
            definition.type = static_cast<AssetTypeId>(type);
        }
        if (args.size() == 3) {
            definition.id = {parse_unsigned(args[2])};
        }
        const auto id = host.services().get<AssetCatalog>().add(std::move(definition));
        return core::CommandResult{true, "asset " + std::to_string(id.value)};
    }, "Register a canonical virtual source with a persistent asset identity");
    (void)kernel.add_command("asset.list", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() > 1) {
            return core::CommandResult{false, "usage: asset.list [search_text]"};
        }
        AssetQuery query;
        if (!args.empty()) {
            query.text = args[0];
        }
        std::string output;
        for (const auto& record : host.services().get<AssetCatalog>().search(query)) {
            output += std::to_string(record.definition.id.value) + " " + record.definition.source_path +
                      " revision=" + std::to_string(record.revision.value) + " " + status_name(record.status) + "\n";
        }
        return core::CommandResult{true, output.empty() ? "no matching assets" : output};
    }, "Search up to 256 catalog records by name or source path");
    (void)kernel.add_command("asset.load", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: asset.load asset_id"};
        }
        auto& cache = host.services().get<ResourceCache>();
        const auto handle = host.services().get<AssetCatalog>().load({parse_unsigned(args[0])}, cache);
        if (!cache.pin(handle)) {
            return core::CommandResult{false, "loaded resource could not be pinned"};
        }
        return core::CommandResult{true, "pinned " + handle_text(handle) + " bytes=" +
            std::to_string(cache.bytes(handle).size())};
    }, "Synchronously import dependencies and source bytes, then pin the returned handle");
    (void)kernel.add_command("asset.release", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: asset.release slot:generation:type"};
        }
        const bool released = host.services().get<ResourceCache>().unpin(parse_handle(args[0]));
        return core::CommandResult{released, released ? "resource unpinned" : "stale handle or no pin to release"};
    }, "Release one resource pin while retaining eligible cached bytes");
    (void)kernel.add_command("asset.inspect", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: asset.inspect asset_id"};
        }
        const auto record = host.services().get<AssetCatalog>().inspect({parse_unsigned(args[0])});
        if (!record) {
            return core::CommandResult{false, "unknown asset"};
        }
        std::string output = "id=" + std::to_string(record->definition.id.value) +
            " type=" + std::to_string(record->definition.type) + " source=" + record->definition.source_path +
            " revision=" + std::to_string(record->revision.value) + " last_good=" +
            std::to_string(record->last_good_revision.value) + " status=" + status_name(record->status) +
            " source_bytes=" + std::to_string(record->source_bytes) +
            " resource_bytes=" + std::to_string(record->resource_bytes) +
            " source_hash=" + std::to_string(record->source_hash);
        if (!record->last_error.empty()) {
            output += " error=" + record->last_error;
        }
        return core::CommandResult{true, std::move(output)};
    }, "Inspect import status, revisions, source checksum and retained errors");
    (void)kernel.add_command("asset.invalidate", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: asset.invalidate asset_id"};
        }
        const auto affected = host.services().get<AssetCatalog>().invalidate({parse_unsigned(args[0])});
        return core::CommandResult{true, "invalidated " + std::to_string(affected.size()) + " assets"};
    }, "Invalidate an asset and every transitive dependent without destroying valid cached bytes");
    (void)kernel.add_command("asset.save", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: asset.save virtual_path"};
        }
        host.services().get<AssetCatalog>().save(args[0]);
        return core::CommandResult{true, "catalog saved"};
    }, "Write a versioned, checksummed catalog through the virtual filesystem");
    (void)kernel.add_command("asset.open", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: asset.open virtual_path"};
        }
        host.services().get<AssetCatalog>().open(args[0]);
        return core::CommandResult{true, "catalog restored; resources require import"};
    }, "Validate a complete catalog before replacing metadata and advancing revisions");
    (void)kernel.add_command("asset.trim", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: asset.trim target_reserved_bytes"};
        }
        const auto target = bounded_size(parse_unsigned(args[0]), std::numeric_limits<std::size_t>::max());
        const auto reclaimed = host.services().get<ResourceCache>().trim(target);
        return core::CommandResult{true, "reclaimed " + std::to_string(reclaimed) + " payload bytes"};
    }, "Evict least recently used unpinned CPU resources to a target reservation");
    (void)kernel.add_system({"assets.catalog", core::Phase::Input, -400, {"io.filesystem"}},
        [](core::Kernel& host, const core::Frame&) {
            auto& catalog = host.services().get<AssetCatalog>();
            auto& state = host.services().get<CatalogRuntime>();
            const auto file_changes = host.events().drain<io::FileChange>(catalog.limits().change_history);
            if (!file_changes.empty()) {
                (void)catalog.apply_file_changes(file_changes);
            }
            const auto changes = catalog.changes_since(state.published_sequence);
            if (changes.history_lost) {
                host.diagnose(core::Diagnostic::Severity::Warning, "assets.catalog",
                              "asset change history overflowed; downstream readers must rescan metadata");
            }
            for (const auto& change : changes.changes) {
                if (!host.events().publish(change)) {
                    break;
                }
                state.published_sequence = change.sequence;
            }
            const auto statistics = host.services().get<ResourceCache>().statistics();
            host.metric("assets.pending_bytes", static_cast<double>(statistics.pending_bytes));
            host.metric("assets.resident_bytes", static_cast<double>(statistics.resident_bytes));
            host.metric("assets.retiring_bytes", static_cast<double>(statistics.retiring_bytes));
            host.metric("assets.resources", static_cast<double>(statistics.resources));
            host.metric("assets.pinned", static_cast<double>(statistics.pinned_resources));
            host.metric("assets.evictions", static_cast<double>(statistics.evictions));
        });
}

} // namespace ge::assets
