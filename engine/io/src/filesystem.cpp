#include <ge/io/filesystem.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace ge::io {
namespace {

namespace fs = std::filesystem;

struct Metrics {
    std::atomic<std::uint64_t> reads{};
    std::atomic<std::uint64_t> writes{};
    std::atomic<std::uint64_t> bytes_read{};
    std::atomic<std::uint64_t> bytes_written{};
    std::atomic<std::uint64_t> cursors{};
    std::atomic<std::uint64_t> failures{};
    std::atomic<std::uint64_t> changes{};
    std::atomic<std::size_t> memory_reserved{};
};

struct MemoryBuffer {
    std::vector<std::byte> bytes;
    std::shared_ptr<Metrics> metrics;
    std::size_t charge{};

    ~MemoryBuffer() {
        if (metrics) {
            metrics->memory_reserved.fetch_sub(charge, std::memory_order_relaxed);
        }
    }
};

struct MemoryFile {
    std::shared_ptr<const MemoryBuffer> buffer;
    std::uint64_t revision{};
};

struct Mount {
    MountInfo info;
    std::map<std::string, MemoryFile, std::less<>> files;
};

struct Resolved {
    std::shared_ptr<Mount> mount;
    FileMetadata metadata;
    fs::path physical;
    std::shared_ptr<const MemoryBuffer> memory;
};

struct Watch {
    WatchInfo info;
    DirectorySnapshot snapshot;
};

[[noreturn]] void fail(IoError code, std::string_view path,
                       std::string_view message) {
    throw IoException(code, std::string(path), std::string(message));
}

void file_error(std::string_view virtual_path, const std::error_code& error) {
    if (error) {
        fail(IoError::filesystem_failure, virtual_path, error.message());
    }
}

bool absent(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory ||
           error == std::errc::not_a_directory;
}

bool physical_contains(const fs::path& parent, const fs::path& child) {
    auto left = parent.begin();
    auto right = child.begin();
    for (; left != parent.end(); ++left, ++right) {
        if (right == child.end() || *left != *right) {
            return false;
        }
    }
    return true;
}

std::string relative_path(std::string_view root, std::string_view path) {
    if (root == "/") {
        return std::string(path);
    }
    if (root == path) {
        return "/";
    }
    return std::string(path.substr(root.size()));
}

std::string map_path(std::string_view root, std::string_view relative) {
    if (relative == "/") {
        return std::string(root);
    }
    if (root == "/") {
        return std::string(relative);
    }
    return std::string(root) + std::string(relative);
}

fs::path native_utf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

fs::path native_relative(std::string_view relative) {
    const auto text = relative == "/" ? std::string_view{} : relative.substr(1);
    return native_utf8(text);
}

std::string generic_utf8(const fs::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

fs::path contained_path(const Mount& mount, std::string_view relative,
                        std::string_view virtual_path) {
    std::error_code error;
    auto path = fs::weakly_canonical(mount.info.physical_root / native_relative(relative), error);
    file_error(virtual_path, error);
    if (!physical_contains(mount.info.physical_root, path)) {
        fail(IoError::outside_mount, virtual_path, "resolved path escapes its mount");
    }
    return path;
}

std::int64_t modified_time(const fs::path& path, std::string_view virtual_path) {
    std::error_code error;
    const auto stamp = fs::last_write_time(path, error);
    file_error(virtual_path, error);
    const auto seconds = std::chrono::duration<long double>(stamp.time_since_epoch()).count();
    const auto nanos = seconds * 1000000000.0L;
    const auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (nanos <= minimum) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (nanos >= maximum) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(nanos);
}

std::size_t bounded_size(std::uint64_t count, std::size_t maximum,
                         std::string_view path) {
    if (count > maximum || count > std::numeric_limits<std::size_t>::max()) {
        fail(IoError::limit_exceeded, path, "file byte limit exceeded");
    }
    return static_cast<std::size_t>(count);
}

std::size_t operation_limit(std::size_t requested, std::size_t configured) {
    return requested == 0 ? configured : std::min(requested, configured);
}

bool same_metadata(const FileMetadata& left, const FileMetadata& right) {
    return left.path == right.path && left.kind == right.kind &&
           left.mount == right.mount && left.size == right.size &&
           left.modified_ns == right.modified_ns && left.revision == right.revision &&
           left.writable == right.writable;
}

void validate_limits(const IoLimits& limits) {
    if (limits.max_path_bytes == 0 || limits.max_path_bytes > 4096 ||
        limits.max_mounts == 0 || limits.max_mounts > 65536 ||
        limits.max_entries == 0 || limits.max_entries > 1000000 ||
        limits.max_watches > 65536 || limits.max_directory_depth > 4096 ||
        limits.max_read_bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) ||
        limits.max_write_bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::invalid_argument("invalid virtual filesystem limits");
    }
}

std::shared_ptr<const MemoryBuffer> allocate_memory(
    std::span<const std::byte> bytes, const std::shared_ptr<Metrics>& metrics,
    std::size_t maximum, std::string_view path) {
    const auto charged = metrics->memory_reserved.load(std::memory_order_relaxed);
    if (charged > maximum || bytes.size() > maximum - charged) {
        fail(IoError::limit_exceeded, path, "memory mount budget exhausted including retained cursors");
    }
    metrics->memory_reserved.fetch_add(bytes.size(), std::memory_order_relaxed);
    try {
        auto result = std::make_shared<MemoryBuffer>();
        result->bytes.assign(bytes.begin(), bytes.end());
        result->metrics = metrics;
        result->charge = bytes.size();
        return result;
    } catch (...) {
        metrics->memory_reserved.fetch_sub(bytes.size(), std::memory_order_relaxed);
        throw;
    }
}

std::uint64_t hash_native(const fs::path& path, std::uint64_t expected_size,
                          std::string_view virtual_path,
                          const std::shared_ptr<Metrics>& metrics) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(IoError::filesystem_failure, virtual_path, "cannot open file for hashing");
    }
    std::array<std::byte, 65536> buffer{};
    core::Fnv1a64 hash;
    std::uint64_t remaining = expected_size;
    while (remaining != 0) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count) || input.bad()) {
            metrics->failures.fetch_add(1, std::memory_order_relaxed);
            fail(IoError::short_read, virtual_path, "file changed or failed while hashing");
        }
        hash.update(std::span<const std::byte>(buffer.data(), count));
        remaining -= static_cast<std::uint64_t>(count);
        metrics->bytes_read.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
    }
    metrics->reads.fetch_add(1, std::memory_order_relaxed);
    return hash.value();
}

class StagingDirectory final {
public:
    explicit StagingDirectory(const fs::path& parent, std::string_view virtual_path) {
        static std::atomic<std::uint64_t> sequence{1};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
            path_ = parent / (".ge-stage-" + std::to_string(stamp) + "-" + std::to_string(serial));
            std::error_code error;
            const bool made = fs::create_directory(path_, error);
            if (made) {
                owned_ = true;
                return;
            }
            if (error && error != std::errc::file_exists) {
                file_error(virtual_path, error);
            }
        }
        fail(IoError::exhausted, virtual_path, "cannot reserve a unique staging directory");
    }

    ~StagingDirectory() {
        if (owned_) {
            std::error_code ignored;
            fs::remove(path_ / "payload", ignored);
            ignored.clear();
            fs::remove(path_, ignored);
        }
    }

    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    [[nodiscard]] fs::path payload() const { return path_ / "payload"; }

private:
    fs::path path_;
    bool owned_{};
};

void staged_write(const fs::path& destination, std::span<const std::byte> bytes,
                   std::string_view virtual_path) {
    StagingDirectory staging(destination.parent_path(), virtual_path);
    const auto payload = staging.payload();
    std::ofstream output(payload, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail(IoError::filesystem_failure, virtual_path, "cannot open staged output");
    }
    constexpr std::size_t chunk_limit = 1024U * 1024U;
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto count = std::min(chunk_limit, bytes.size() - offset);
        output.write(reinterpret_cast<const char*>(bytes.data() + offset),
                     static_cast<std::streamsize>(count));
        if (!output) {
            fail(IoError::short_write, virtual_path, "staged file write failed");
        }
        offset += count;
    }
    output.flush();
    if (!output) {
        fail(IoError::short_write, virtual_path, "staged file flush failed");
    }
    output.close();
    if (!output) {
        fail(IoError::short_write, virtual_path, "staged file close failed");
    }
    std::error_code error;
    fs::rename(payload, destination, error);
    file_error(virtual_path, error);
}

template<class Integer>
Integer parse_integer(std::string_view text, std::string_view label) {
    Integer value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid " + std::string(label));
    }
    return value;
}

} // namespace

IoException::IoException(IoError code, std::string path, std::string message)
    : std::runtime_error(message + (path.empty() ? "" : ": " + path)),
      code_(code), path_(std::move(path)) {}

IoError IoException::code() const noexcept { return code_; }
const std::string& IoException::path() const noexcept { return path_; }

std::string normalize_virtual_path(std::string_view path, std::size_t maximum_bytes) {
    if (path.size() > maximum_bytes || maximum_bytes == 0) {
        fail(IoError::limit_exceeded, path.substr(0, 128), "virtual path limit exceeded");
    }
    if (!core::valid_utf8(path)) {
        fail(IoError::invalid_path, {}, "virtual path is not UTF-8");
    }
    for (const char character : path) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || byte == 127 || character == ':' || character == '\\' ||
            character == '<' || character == '>' || character == '|' ||
            character == '?' || character == '*' || character == '"') {
            fail(IoError::invalid_path, path, "unsupported virtual path character");
        }
    }
    if (path.starts_with("//")) {
        fail(IoError::invalid_path, path, "network path syntax is not a virtual path");
    }
    std::string result{"/"};
    std::size_t cursor{};
    while (cursor < path.size()) {
        while (cursor < path.size() && path[cursor] == '/') {
            ++cursor;
        }
        const auto begin = cursor;
        while (cursor < path.size() && path[cursor] != '/') {
            ++cursor;
        }
        const auto component = path.substr(begin, cursor - begin);
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            fail(IoError::invalid_path, path, "parent traversal is not allowed");
        }
        if (component.ends_with(' ') || component.ends_with('.')) {
            fail(IoError::invalid_path, path, "ambiguous trailing path characters");
        }
        const auto dot = component.find('.');
        const auto stem = component.substr(0, dot);
        std::string upper;
        upper.reserve(stem.size());
        for (const char character : stem) {
            upper += character >= 'a' && character <= 'z'
                ? static_cast<char>(character - 'a' + 'A') : character;
        }
        if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
            (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
             upper[3] >= '1' && upper[3] <= '9')) {
            fail(IoError::invalid_path, path, "reserved device name is not a virtual file");
        }
        if (result.size() != 1) {
            result += '/';
        }
        result.append(component);
        if (result.size() > maximum_bytes) {
            fail(IoError::limit_exceeded, path, "normalized virtual path exceeds limit");
        }
    }
    return result;
}

std::string join_virtual_path(std::string_view base, std::string_view relative,
                              std::size_t maximum_bytes) {
    if (relative.starts_with('/')) {
        fail(IoError::invalid_path, relative, "join requires a relative child path");
    }
    const auto normalized = normalize_virtual_path(base, maximum_bytes);
    std::size_t size{};
    const auto separator = normalized == "/" ? std::size_t{0} : std::size_t{1};
    if (!core::checked_add(normalized.size(), relative.size(), size) ||
        !core::checked_add(size, separator, size) || size > maximum_bytes) {
        fail(IoError::limit_exceeded, base, "joined path exceeds limit");
    }
    return normalize_virtual_path(normalized + (separator == 0 ? "" : "/") +
                                  std::string(relative), maximum_bytes);
}

std::string virtual_parent(std::string_view path) {
    const auto normalized = normalize_virtual_path(path);
    const auto slash = normalized.find_last_of('/');
    return slash == 0 ? "/" : normalized.substr(0, slash);
}

std::string virtual_filename(std::string_view path) {
    const auto normalized = normalize_virtual_path(path);
    return normalized == "/" ? std::string{} : normalized.substr(normalized.find_last_of('/') + 1);
}

std::string virtual_extension(std::string_view path) {
    const auto name = virtual_filename(path);
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? std::string{} : name.substr(dot);
}

bool virtual_contains(std::string_view parent, std::string_view child) noexcept {
    return parent == "/" || parent == child ||
           (child.size() > parent.size() && child.starts_with(parent) && child[parent.size()] == '/');
}

std::uint64_t metadata_cache_key(const FileMetadata& metadata) {
    core::ByteWriter writer(8192);
    writer.write_string(metadata.path, 4096);
    writer.write_u8(static_cast<std::uint8_t>(metadata.kind));
    writer.write_u64(metadata.mount);
    writer.write_u64(metadata.size);
    writer.write_i64(metadata.modified_ns);
    writer.write_u64(metadata.revision);
    return core::fnv1a64(writer.bytes());
}

struct ReadCursor::State {
    FileMetadata metadata;
    std::ifstream input;
    std::shared_ptr<const MemoryBuffer> memory;
    std::shared_ptr<Metrics> metrics;
    std::size_t read_limit{};
    std::uint64_t position{};

    ~State() {
        if (metrics) {
            metrics->cursors.fetch_sub(1, std::memory_order_relaxed);
        }
    }
};

ReadCursor::ReadCursor() noexcept = default;
ReadCursor::ReadCursor(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
ReadCursor::~ReadCursor() = default;
ReadCursor::ReadCursor(ReadCursor&&) noexcept = default;
ReadCursor& ReadCursor::operator=(ReadCursor&&) noexcept = default;
bool ReadCursor::is_open() const noexcept { return static_cast<bool>(state_); }

const FileMetadata& ReadCursor::metadata() const {
    if (!state_) {
        fail(IoError::not_found, {}, "cursor is closed");
    }
    return state_->metadata;
}

std::uint64_t ReadCursor::size() const noexcept { return state_ ? state_->metadata.size : 0; }
std::uint64_t ReadCursor::position() const noexcept { return state_ ? state_->position : 0; }
std::uint64_t ReadCursor::remaining() const noexcept { return size() - position(); }
bool ReadCursor::eof() const noexcept { return remaining() == 0; }

void ReadCursor::seek(std::uint64_t position_value) {
    if (!state_ || position_value > state_->metadata.size ||
        position_value > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        fail(IoError::invalid_offset, state_ ? state_->metadata.path : "", "invalid cursor position");
    }
    if (!state_->memory) {
        state_->input.clear();
        state_->input.seekg(static_cast<std::streamoff>(position_value), std::ios::beg);
        if (!state_->input) {
            fail(IoError::filesystem_failure, state_->metadata.path, "file seek failed");
        }
    }
    state_->position = position_value;
}

void ReadCursor::skip(std::uint64_t count) {
    if (count > remaining()) {
        fail(IoError::invalid_offset, state_ ? state_->metadata.path : "", "cursor skip exceeds file");
    }
    seek(position() + count);
}

std::size_t ReadCursor::read(std::span<std::byte> destination) {
    if (!state_) {
        fail(IoError::not_found, {}, "cursor is closed");
    }
    if (destination.size() > state_->read_limit) {
        fail(IoError::limit_exceeded, state_->metadata.path, "cursor read exceeds operation limit");
    }
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining(), destination.size()));
    if (count == 0) {
        return 0;
    }
    if (state_->memory) {
        const auto offset = static_cast<std::size_t>(state_->position);
        std::memcpy(destination.data(), state_->memory->bytes.data() + offset, count);
    } else {
        state_->input.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(count));
        if (state_->input.gcount() != static_cast<std::streamsize>(count) || state_->input.bad()) {
            state_->metrics->failures.fetch_add(1, std::memory_order_relaxed);
            fail(IoError::short_read, state_->metadata.path, "file changed or read failed");
        }
    }
    state_->position += static_cast<std::uint64_t>(count);
    state_->metrics->reads.fetch_add(1, std::memory_order_relaxed);
    state_->metrics->bytes_read.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
    return count;
}

std::vector<std::byte> ReadCursor::read_exact(std::size_t count) {
    if (!state_ || count > remaining()) {
        fail(IoError::short_read, state_ ? state_->metadata.path : "", "requested range exceeds file");
    }
    if (count > state_->read_limit) {
        fail(IoError::limit_exceeded, state_->metadata.path, "cursor allocation exceeds limit");
    }
    std::vector<std::byte> result(count);
    (void)read(result);
    return result;
}

void ReadCursor::close() noexcept { state_.reset(); }

struct VirtualFileSystem::Impl {
    IoLimits limits;
    mutable std::mutex mutex;
    std::shared_ptr<Metrics> metrics{std::make_shared<Metrics>()};
    std::vector<std::shared_ptr<Mount>> mounts;
    std::map<WatchId, Watch> watches;
    MountId next_mount{1};
    WatchId next_watch{1};

    explicit Impl(IoLimits value) : limits(value) { validate_limits(limits); }

    void order_mounts() {
        std::sort(mounts.begin(), mounts.end(), [](const auto& left, const auto& right) {
            if (left->info.priority != right->info.priority) {
                return left->info.priority > right->info.priority;
            }
            return left->info.id > right->info.id;
        });
    }

    FileMetadata directory_metadata(const Mount& mount, std::string path) const {
        return {std::move(path), FileKind::directory, mount.info.id, 0, 0,
                mount.info.kind == MountKind::memory ? mount.info.revision : 1,
                mount.info.writable};
    }

    std::optional<Resolved> resolve_mount(const std::shared_ptr<Mount>& mount,
                                          const std::string& path) const {
        if (!virtual_contains(mount->info.virtual_root, path)) {
            if (virtual_contains(path, mount->info.virtual_root)) {
                return Resolved{mount, directory_metadata(*mount, path), {}, {}};
            }
            return std::nullopt;
        }
        const auto relative = relative_path(mount->info.virtual_root, path);
        if (mount->info.kind == MountKind::memory) {
            const auto found = mount->files.find(relative);
            if (found != mount->files.end()) {
                const auto revision = found->second.revision;
                const auto stamp = static_cast<std::int64_t>(std::min<std::uint64_t>(
                    revision, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
                FileMetadata metadata{path, FileKind::regular, mount->info.id,
                    static_cast<std::uint64_t>(found->second.buffer->bytes.size()), stamp,
                    revision, mount->info.writable};
                return Resolved{mount, std::move(metadata), {}, found->second.buffer};
            }
            const auto prefix = relative == "/" ? relative : relative + "/";
            const auto child = mount->files.lower_bound(prefix);
            if (relative == "/" || (child != mount->files.end() && child->first.starts_with(prefix))) {
                return Resolved{mount, directory_metadata(*mount, path), {}, {}};
            }
            return std::nullopt;
        }
        const auto physical = contained_path(*mount, relative, path);
        std::error_code error;
        const auto status = fs::status(physical, error);
        if (absent(error) || (!error && !fs::exists(status))) {
            return std::nullopt;
        }
        file_error(path, error);
        if (!fs::is_regular_file(status) && !fs::is_directory(status)) {
            fail(IoError::access_denied, path, "special filesystem nodes are not readable assets");
        }
        FileMetadata metadata = directory_metadata(*mount, path);
        metadata.modified_ns = modified_time(physical, path);
        if (fs::is_regular_file(status)) {
            metadata.kind = FileKind::regular;
            const auto bytes = fs::file_size(physical, error);
            file_error(path, error);
            if (bytes > limits.max_file_bytes) {
                fail(IoError::limit_exceeded, path, "native file exceeds configured limit");
            }
            metadata.size = static_cast<std::uint64_t>(bytes);
        }
        return Resolved{mount, std::move(metadata), physical, {}};
    }

    bool blocked_by_file(const std::shared_ptr<Mount>& mount, const std::string& path) const {
        if (!virtual_contains(mount->info.virtual_root, path)) {
            return false;
        }
        auto parent = virtual_parent(path);
        while (virtual_contains(mount->info.virtual_root, parent)) {
            const auto entry = resolve_mount(mount, parent);
            if (entry && entry->metadata.kind == FileKind::regular) {
                return true;
            }
            if (parent == mount->info.virtual_root || parent == "/") {
                break;
            }
            parent = virtual_parent(parent);
        }
        return false;
    }

    std::optional<Resolved> resolve(const std::string& path) const {
        for (const auto& mount : mounts) {
            const auto result = resolve_mount(mount, path);
            if (result) {
                return result;
            }
            if (blocked_by_file(mount, path)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::shared_ptr<Mount> write_mount(const std::string& path) const {
        for (const auto& mount : mounts) {
            if (virtual_contains(mount->info.virtual_root, path)) {
                if (!mount->info.writable) {
                    const auto visible = resolve_mount(mount, path);
                    if (visible || blocked_by_file(mount, path)) {
                        fail(IoError::access_denied, path, "visible entry belongs to a read-only mount");
                    }
                    continue;
                }
                return mount;
            }
        }
        fail(IoError::access_denied, path, "no writable mount covers this path");
    }

    std::vector<FileMetadata> list_locked(const std::string& path, ListOptions options) const;
    DirectorySnapshot snapshot_locked(const std::string& path, WatchOptions options) const;
};

std::vector<FileMetadata> VirtualFileSystem::Impl::list_locked(
    const std::string& path, ListOptions options) const {
    const auto root = resolve(path);
    if (!root) {
        fail(IoError::not_found, path, "directory does not exist");
    }
    if (root->metadata.kind != FileKind::directory) {
        fail(IoError::not_a_directory, path, "listing requires a directory");
    }
    const auto maximum = operation_limit(options.maximum_entries, limits.max_entries);
    std::set<std::string, std::less<>> candidates;
    std::size_t inspected{};
    auto insert = [&](std::string candidate) {
        if (candidate == path || !virtual_contains(path, candidate)) {
            return;
        }
        if (!options.recursive) {
            const auto offset = path == "/" ? std::size_t{1} : path.size() + 1;
            const auto slash = candidate.find('/', offset);
            if (slash != std::string::npos) {
                candidate.resize(slash);
            }
        }
        candidates.insert(std::move(candidate));
        if (candidates.size() > maximum) {
            fail(IoError::limit_exceeded, path, "directory listing exceeds entry limit");
        }
    };
    auto insert_ancestors = [&](std::string candidate) {
        while (candidate != path && virtual_contains(path, candidate)) {
            insert(candidate);
            if (candidate == "/") {
                break;
            }
            candidate = virtual_parent(candidate);
        }
    };
    for (const auto& mount : mounts) {
        const bool below = virtual_contains(path, mount->info.virtual_root);
        const bool above = virtual_contains(mount->info.virtual_root, path);
        if (!below && !above) {
            continue;
        }
        if (below) {
            insert_ancestors(mount->info.virtual_root);
        }
        if (mount->info.kind == MountKind::memory) {
            for (const auto& [relative, file] : mount->files) {
                (void)file;
                if (++inspected > limits.max_entries) {
                    fail(IoError::limit_exceeded, path, "directory scan work limit exceeded");
                }
                insert_ancestors(map_path(mount->info.virtual_root, relative));
            }
            continue;
        }
        if (!options.recursive && below && mount->info.virtual_root != path) {
            continue;
        }
        const auto start_path = above ? path : mount->info.virtual_root;
        const auto relative = relative_path(mount->info.virtual_root, start_path);
        const auto start = contained_path(*mount, relative, start_path);
        std::error_code error;
        const auto status = fs::status(start, error);
        if (absent(error) || (!error && !fs::is_directory(status))) {
            continue;
        }
        file_error(start_path, error);
        struct PendingDirectory {
            fs::path physical;
            std::string virtual_path;
            std::size_t depth{};
        };
        std::vector<PendingDirectory> pending;
        pending.push_back({start, start_path, 0});
        while (!pending.empty()) {
            auto directory = std::move(pending.back());
            pending.pop_back();
            fs::directory_iterator cursor(directory.physical, error);
            file_error(directory.virtual_path, error);
            const fs::directory_iterator end;
            while (cursor != end) {
                if (++inspected > limits.max_entries) {
                    fail(IoError::limit_exceeded, path, "directory scan work limit exceeded");
                }
                const auto entry = *cursor;
                const auto name = generic_utf8(entry.path().filename());
                if (name.starts_with(".ge-stage-")) {
                    cursor.increment(error);
                    file_error(directory.virtual_path, error);
                    continue;
                }
                const auto candidate = join_virtual_path(directory.virtual_path, name,
                                                         limits.max_path_bytes);
                const auto link_status = entry.symlink_status(error);
                file_error(candidate, error);
                const auto canonical = fs::weakly_canonical(entry.path(), error);
                file_error(candidate, error);
                if (!physical_contains(mount->info.physical_root, canonical)) {
                    cursor.increment(error);
                    file_error(directory.virtual_path, error);
                    continue;
                }
                const auto entry_status = entry.status(error);
                if (absent(error)) {
                    error.clear();
                } else {
                    file_error(candidate, error);
                    if (fs::is_regular_file(entry_status) || fs::is_directory(entry_status)) {
                        insert(candidate);
                    }
                    if (options.recursive && fs::is_directory(entry_status) &&
                        !fs::is_symlink(link_status)) {
                        if (directory.depth >= limits.max_directory_depth) {
                            fail(IoError::limit_exceeded, candidate, "directory depth limit exceeded");
                        }
                        pending.push_back({canonical, candidate, directory.depth + 1});
                        if (pending.size() > limits.max_entries) {
                            fail(IoError::limit_exceeded, path, "directory traversal queue exhausted");
                        }
                    }
                }
                cursor.increment(error);
                file_error(directory.virtual_path, error);
            }
        }
    }
    std::vector<FileMetadata> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const auto visible = resolve(candidate);
        if (!visible || (!options.include_directories &&
                         visible->metadata.kind == FileKind::directory)) {
            continue;
        }
        result.push_back(visible->metadata);
    }
    return result;
}

DirectorySnapshot VirtualFileSystem::Impl::snapshot_locked(
    const std::string& path, WatchOptions options) const {
    DirectorySnapshot snapshot;
    snapshot.root = path;
    snapshot.recursive = options.recursive;
    snapshot.hashes_contents = options.hash_contents;
    auto entries = list_locked(path, {options.recursive, true, options.maximum_entries});
    snapshot.entries.reserve(entries.size());
    for (auto& metadata : entries) {
        std::uint64_t hash{};
        if (options.hash_contents && metadata.kind == FileKind::regular) {
            const auto resolved = resolve(metadata.path);
            if (!resolved) {
                fail(IoError::not_found, metadata.path, "file disappeared while taking snapshot");
            }
            if (resolved->memory) {
                hash = core::fnv1a64(resolved->memory->bytes);
            } else {
                hash = hash_native(resolved->physical, metadata.size, metadata.path, metrics);
            }
            const auto after = resolve(metadata.path);
            if (!after || !same_metadata(metadata, after->metadata)) {
                fail(IoError::filesystem_failure, metadata.path, "file changed during snapshot");
            }
        }
        snapshot.entries.push_back({std::move(metadata), hash});
    }
    return snapshot;
}

VirtualFileSystem::VirtualFileSystem(IoLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
VirtualFileSystem::~VirtualFileSystem() = default;
const IoLimits& VirtualFileSystem::limits() const noexcept { return impl_->limits; }

MountId VirtualFileSystem::mount_directory(const fs::path& physical_root, MountOptions options) {
    const auto virtual_root = normalize_virtual_path(options.virtual_root, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    if (impl_->mounts.size() >= impl_->limits.max_mounts ||
        impl_->next_mount == std::numeric_limits<MountId>::max()) {
        fail(IoError::exhausted, virtual_root, "mount capacity exhausted");
    }
    std::error_code error;
    const auto canonical = fs::canonical(physical_root, error);
    file_error(virtual_root, error);
    const auto status = fs::status(canonical, error);
    file_error(virtual_root, error);
    if (!fs::is_directory(status)) {
        fail(IoError::not_a_directory, virtual_root, "native mount root must be an existing directory");
    }
    auto mount = std::make_shared<Mount>();
    mount->info = {impl_->next_mount, MountKind::directory, virtual_root,
                   canonical, options.priority, options.writable, 1, 0, 0};
    impl_->mounts.push_back(mount);
    ++impl_->next_mount;
    impl_->order_mounts();
    return mount->info.id;
}

MountId VirtualFileSystem::mount_memory(MountOptions options, std::span<const MemoryEntry> initial) {
    const auto virtual_root = normalize_virtual_path(options.virtual_root, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    if (impl_->mounts.size() >= impl_->limits.max_mounts ||
        impl_->next_mount == std::numeric_limits<MountId>::max()) {
        fail(IoError::exhausted, virtual_root, "mount capacity exhausted");
    }
    if (initial.size() > impl_->limits.max_entries) {
        fail(IoError::limit_exceeded, virtual_root, "initial memory file limit exceeded");
    }
    auto mount = std::make_shared<Mount>();
    mount->info = {impl_->next_mount, MountKind::memory, virtual_root,
                   {}, options.priority, options.writable, 1, 0, 0};
    for (const auto& entry : initial) {
        const auto relative = normalize_virtual_path(entry.path, impl_->limits.max_path_bytes);
        if (relative == "/" || entry.bytes.size() > impl_->limits.max_write_bytes ||
            entry.bytes.size() > impl_->limits.max_file_bytes) {
            fail(IoError::limit_exceeded, entry.path, "invalid initial memory file");
        }
        const auto virtual_path = map_path(virtual_root, relative);
        (void)normalize_virtual_path(virtual_path, impl_->limits.max_path_bytes);
        if (mount->files.contains(relative)) {
            fail(IoError::already_exists, virtual_path, "duplicate initial memory file");
        }
        auto parent = virtual_parent(relative);
        while (parent != "/") {
            if (mount->files.contains(parent)) {
                fail(IoError::not_a_directory, virtual_path, "memory file has a file parent");
            }
            parent = virtual_parent(parent);
        }
        const auto prefix = relative + "/";
        const auto child = mount->files.lower_bound(prefix);
        if (child != mount->files.end() && child->first.starts_with(prefix)) {
            fail(IoError::not_a_file, virtual_path, "memory file collides with an existing directory");
        }
        auto buffer = allocate_memory(entry.bytes, impl_->metrics, impl_->limits.max_memory_bytes,
                                      virtual_path);
        mount->files.emplace(relative, MemoryFile{std::move(buffer), 1});
        mount->info.memory_bytes += entry.bytes.size();
    }
    mount->info.memory_files = mount->files.size();
    impl_->mounts.push_back(mount);
    ++impl_->next_mount;
    impl_->order_mounts();
    return mount->info.id;
}

bool VirtualFileSystem::unmount(MountId mount_id) {
    std::scoped_lock lock(impl_->mutex);
    const auto it = std::find_if(impl_->mounts.begin(), impl_->mounts.end(),
        [mount_id](const auto& mount) { return mount->info.id == mount_id; });
    if (it == impl_->mounts.end()) {
        return false;
    }
    impl_->mounts.erase(it);
    return true;
}

std::vector<MountInfo> VirtualFileSystem::mounts() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<MountInfo> result;
    result.reserve(impl_->mounts.size());
    for (const auto& mount : impl_->mounts) {
        result.push_back(mount->info);
    }
    return result;
}

std::optional<FileMetadata> VirtualFileSystem::metadata(std::string_view path) const {
    const auto normalized = normalize_virtual_path(path, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    const auto result = impl_->resolve(normalized);
    return result ? std::optional<FileMetadata>{result->metadata} : std::nullopt;
}

bool VirtualFileSystem::exists(std::string_view path) const { return metadata(path).has_value(); }

std::uint64_t VirtualFileSystem::cache_key(std::string_view path) const {
    const auto info = metadata(path);
    if (!info) {
        fail(IoError::not_found, path, "cannot identify missing file");
    }
    return metadata_cache_key(*info);
}

std::uint64_t VirtualFileSystem::content_hash(std::string_view path) {
    auto cursor = open_read(path);
    core::Fnv1a64 hash;
    const auto chunk_size = std::min<std::size_t>(impl_->limits.max_read_bytes, 65536);
    if (chunk_size == 0 && cursor.remaining() != 0) {
        fail(IoError::limit_exceeded, path, "read limit prohibits hashing");
    }
    std::vector<std::byte> buffer(chunk_size);
    while (!cursor.eof()) {
        const auto count = cursor.read(buffer);
        hash.update(std::span<const std::byte>(buffer.data(), count));
    }
    return hash.value();
}

std::vector<FileMetadata> VirtualFileSystem::list(std::string_view path, ListOptions options) const {
    const auto normalized = normalize_virtual_path(path, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    return impl_->list_locked(normalized, options);
}

ReadCursor VirtualFileSystem::open_read(std::string_view path) {
    const auto normalized = normalize_virtual_path(path, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    const auto resolved = impl_->resolve(normalized);
    if (!resolved) {
        fail(IoError::not_found, normalized, "file does not exist");
    }
    if (resolved->metadata.kind != FileKind::regular) {
        fail(IoError::not_a_file, normalized, "read requires a regular file");
    }
    auto state = std::make_unique<ReadCursor::State>();
    state->metadata = resolved->metadata;
    state->read_limit = impl_->limits.max_read_bytes;
    state->memory = resolved->memory;
    if (!state->memory) {
        state->input.open(resolved->physical, std::ios::binary);
        if (!state->input) {
            impl_->metrics->failures.fetch_add(1, std::memory_order_relaxed);
            fail(IoError::filesystem_failure, normalized, "cannot open native file");
        }
    }
    state->metrics = impl_->metrics;
    impl_->metrics->cursors.fetch_add(1, std::memory_order_relaxed);
    return ReadCursor(std::move(state));
}

std::vector<std::byte> VirtualFileSystem::read(std::string_view path, std::size_t maximum_bytes) {
    auto cursor = open_read(path);
    const auto maximum = operation_limit(maximum_bytes, impl_->limits.max_read_bytes);
    return cursor.read_exact(bounded_size(cursor.size(), maximum, path));
}

std::vector<std::byte> VirtualFileSystem::read_range(std::string_view path,
                                                   std::uint64_t offset,
                                                   std::size_t length) {
    auto cursor = open_read(path);
    cursor.seek(offset);
    return cursor.read_exact(length);
}

std::string VirtualFileSystem::read_text(std::string_view path, std::size_t maximum_bytes) {
    const auto bytes = read(path, maximum_bytes);
    if (!core::valid_utf8(bytes)) {
        fail(IoError::invalid_path, path, "file text is not valid UTF-8");
    }
    if (bytes.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void VirtualFileSystem::write(std::string_view path, std::span<const std::byte> bytes,
                              WriteOptions options) {
    const auto normalized = normalize_virtual_path(path, impl_->limits.max_path_bytes);
    if (bytes.size() > impl_->limits.max_write_bytes || bytes.size() > impl_->limits.max_file_bytes) {
        fail(IoError::limit_exceeded, normalized, "write byte limit exceeded");
    }
    std::scoped_lock lock(impl_->mutex);
    auto mount = impl_->write_mount(normalized);
    const auto previous = impl_->resolve(normalized);
    if (previous && previous->metadata.kind == FileKind::directory) {
        fail(IoError::not_a_file, normalized, "cannot replace a directory with a file");
    }
    if (previous && !options.overwrite) {
        fail(IoError::already_exists, normalized, "destination already exists");
    }
    if (options.expected_cache_key && (!previous ||
        metadata_cache_key(previous->metadata) != *options.expected_cache_key)) {
        fail(IoError::filesystem_failure, normalized, "destination changed since expected revision");
    }
    if (mount->info.revision == std::numeric_limits<std::uint64_t>::max()) {
        fail(IoError::exhausted, normalized, "mount revision exhausted");
    }
    const auto relative = relative_path(mount->info.virtual_root, normalized);
    if (mount->info.kind == MountKind::memory) {
        if (impl_->blocked_by_file(mount, normalized)) {
            fail(IoError::not_a_directory, normalized, "memory file parent is not a directory");
        }
        const auto existing = mount->files.find(relative);
        if (existing == mount->files.end() && mount->files.size() >= impl_->limits.max_entries) {
            fail(IoError::limit_exceeded, normalized, "memory file count limit exceeded");
        }
        const auto old_size = existing == mount->files.end() ? std::size_t{0}
            : existing->second.buffer->bytes.size();
        auto buffer = allocate_memory(bytes, impl_->metrics, impl_->limits.max_memory_bytes, normalized);
        const auto revision = mount->info.revision + 1;
        mount->files.insert_or_assign(relative, MemoryFile{std::move(buffer), revision});
        mount->info.memory_bytes = mount->info.memory_bytes - old_size + bytes.size();
        mount->info.memory_files = mount->files.size();
        mount->info.revision = revision;
    } else {
        const auto destination = contained_path(*mount, relative, normalized);
        const auto parent = destination.parent_path();
        std::error_code error;
        if (options.create_parents) {
            fs::create_directories(parent, error);
            file_error(normalized, error);
        }
        const auto parent_status = fs::status(parent, error);
        file_error(normalized, error);
        if (!fs::is_directory(parent_status)) {
            fail(IoError::not_a_directory, normalized, "destination parent does not exist");
        }
        const auto revalidated = contained_path(*mount, relative, normalized);
        if (revalidated != destination) {
            fail(IoError::outside_mount, normalized, "destination changed during preparation");
        }
        staged_write(destination, bytes, normalized);
        ++mount->info.revision;
    }
    impl_->metrics->writes.fetch_add(1, std::memory_order_relaxed);
    impl_->metrics->bytes_written.fetch_add(static_cast<std::uint64_t>(bytes.size()),
                                           std::memory_order_relaxed);
}

void VirtualFileSystem::write_text(std::string_view path, std::string_view text, WriteOptions options) {
    if (!core::valid_utf8(text)) {
        fail(IoError::invalid_path, path, "text output is not valid UTF-8");
    }
    write(path, core::as_bytes(text), options);
}

bool VirtualFileSystem::remove(std::string_view path) {
    const auto normalized = normalize_virtual_path(path, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    const auto resolved = impl_->resolve(normalized);
    if (!resolved) {
        return false;
    }
    if (!resolved->metadata.writable || !resolved->mount) {
        fail(IoError::access_denied, normalized, "file is not writable");
    }
    if (resolved->metadata.kind != FileKind::regular) {
        fail(IoError::not_a_file, normalized, "remove only accepts regular files");
    }
    auto& mount = *resolved->mount;
    if (mount.info.revision == std::numeric_limits<std::uint64_t>::max()) {
        fail(IoError::exhausted, normalized, "mount revision exhausted");
    }
    if (mount.info.kind == MountKind::memory) {
        const auto relative = relative_path(mount.info.virtual_root, normalized);
        const auto it = mount.files.find(relative);
        mount.info.memory_bytes -= it->second.buffer->bytes.size();
        mount.files.erase(it);
        mount.info.memory_files = mount.files.size();
    } else {
        // Resolution validated containment; unlink the requested entry rather
        // than the canonical target of an in-mount file symlink.
        const auto relative = relative_path(mount.info.virtual_root, normalized);
        const auto entry = mount.info.physical_root / native_relative(relative);
        std::error_code error;
        const bool removed = fs::remove(entry, error);
        file_error(normalized, error);
        if (!removed) {
            return false;
        }
    }
    ++mount.info.revision;
    impl_->metrics->writes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void VirtualFileSystem::copy(std::string_view source, std::string_view destination,
                             WriteOptions options) {
    const auto bytes = read(source, impl_->limits.max_write_bytes);
    write(destination, bytes, options);
}

DirectorySnapshot VirtualFileSystem::snapshot(std::string_view path, WatchOptions options) {
    const auto normalized = normalize_virtual_path(path, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    return impl_->snapshot_locked(normalized, options);
}

WatchId VirtualFileSystem::watch(std::string_view path, WatchOptions options) {
    const auto normalized = normalize_virtual_path(path, impl_->limits.max_path_bytes);
    std::scoped_lock lock(impl_->mutex);
    if (impl_->watches.size() >= impl_->limits.max_watches ||
        impl_->next_watch == std::numeric_limits<WatchId>::max()) {
        fail(IoError::exhausted, normalized, "watch capacity exhausted");
    }
    auto snapshot = impl_->snapshot_locked(normalized, options);
    const auto id = impl_->next_watch;
    WatchInfo info{id, normalized, options, snapshot.entries.size()};
    impl_->watches.emplace(id, Watch{std::move(info), std::move(snapshot)});
    ++impl_->next_watch;
    return id;
}

bool VirtualFileSystem::unwatch(WatchId watch_id) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->watches.erase(watch_id) != 0;
}

std::vector<WatchInfo> VirtualFileSystem::watches() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<WatchInfo> result;
    result.reserve(impl_->watches.size());
    for (const auto& [id, watch] : impl_->watches) {
        (void)id;
        result.push_back(watch.info);
    }
    return result;
}

PollResult VirtualFileSystem::poll_changes(std::size_t maximum_changes) {
    std::scoped_lock lock(impl_->mutex);
    const auto maximum = operation_limit(maximum_changes, impl_->limits.max_changes);
    PollResult result;
    struct PendingSnapshot {
        Watch* watch;
        DirectorySnapshot snapshot;
    };
    std::vector<PendingSnapshot> pending;
    for (auto& [id, watch] : impl_->watches) {
        DirectorySnapshot next;
        const auto root = impl_->resolve(watch.info.root);
        if (root && root->metadata.kind == FileKind::directory) {
            next = impl_->snapshot_locked(watch.info.root, watch.info.options);
        } else {
            next.root = watch.info.root;
            next.recursive = watch.info.options.recursive;
            next.hashes_contents = watch.info.options.hash_contents;
        }
        std::vector<FileChange> changes;
        try {
            changes = compare_snapshots(watch.snapshot, next, id, maximum - result.changes.size());
        } catch (const IoException& error) {
            if (error.code() != IoError::limit_exceeded) {
                throw;
            }
            result.overflow = true;
            break;
        }
        result.changes.insert(result.changes.end(),
            std::make_move_iterator(changes.begin()), std::make_move_iterator(changes.end()));
        pending.push_back({&watch, std::move(next)});
        ++result.watches_polled;
    }
    // A later watch can fail while scanning or hashing. Publish only after the
    // complete result is ready so an exception cannot discard earlier events.
    for (auto& update : pending) {
        update.watch->snapshot = std::move(update.snapshot);
        update.watch->info.observed_entries = update.watch->snapshot.entries.size();
    }
    impl_->metrics->changes.fetch_add(static_cast<std::uint64_t>(result.changes.size()),
                                     std::memory_order_relaxed);
    return result;
}

IoStatistics VirtualFileSystem::statistics() const {
    std::scoped_lock lock(impl_->mutex);
    const auto& metrics = *impl_->metrics;
    return {metrics.reads.load(std::memory_order_relaxed),
            metrics.writes.load(std::memory_order_relaxed),
            metrics.bytes_read.load(std::memory_order_relaxed),
            metrics.bytes_written.load(std::memory_order_relaxed),
            metrics.cursors.load(std::memory_order_relaxed),
            metrics.failures.load(std::memory_order_relaxed),
            metrics.changes.load(std::memory_order_relaxed),
            impl_->mounts.size(), impl_->watches.size(),
            metrics.memory_reserved.load(std::memory_order_relaxed)};
}

namespace {

void validate_snapshot(const DirectorySnapshot& snapshot, const IoLimits& limits) {
    if (normalize_virtual_path(snapshot.root, limits.max_path_bytes) != snapshot.root ||
        snapshot.entries.size() > limits.max_entries) {
        fail(IoError::malformed_snapshot, snapshot.root, "invalid snapshot root or count");
    }
    std::string_view previous;
    for (const auto& entry : snapshot.entries) {
        const auto& metadata = entry.metadata;
        if (normalize_virtual_path(metadata.path, limits.max_path_bytes) != metadata.path ||
            !virtual_contains(snapshot.root, metadata.path) || metadata.path == snapshot.root ||
            (!previous.empty() && previous >= metadata.path) || metadata.mount == 0 ||
            metadata.revision == 0 || metadata.size > limits.max_file_bytes ||
            (metadata.kind != FileKind::regular && metadata.kind != FileKind::directory) ||
            (metadata.kind == FileKind::directory && (metadata.size != 0 || entry.content_hash != 0)) ||
            (!snapshot.hashes_contents && entry.content_hash != 0)) {
            fail(IoError::malformed_snapshot, metadata.path, "invalid snapshot entry");
        }
        if (!snapshot.recursive && virtual_parent(metadata.path) != snapshot.root) {
            fail(IoError::malformed_snapshot, metadata.path, "nonrecursive snapshot has a descendant");
        }
        previous = metadata.path;
    }
}

void write_metadata(core::ByteWriter& writer, const FileMetadata& metadata,
                    std::size_t maximum_path) {
    writer.write_string(metadata.path, maximum_path);
    writer.write_u8(static_cast<std::uint8_t>(metadata.kind));
    writer.write_bool(metadata.writable);
    writer.write_u16(0);
    writer.write_u64(metadata.mount);
    writer.write_u64(metadata.size);
    writer.write_i64(metadata.modified_ns);
    writer.write_u64(metadata.revision);
}

FileMetadata read_metadata(core::ByteReader& reader) {
    FileMetadata metadata;
    metadata.path = reader.read_string();
    const auto kind = reader.read_u8();
    if (kind > static_cast<std::uint8_t>(FileKind::directory)) {
        fail(IoError::malformed_snapshot, metadata.path, "unknown snapshot file kind");
    }
    metadata.kind = static_cast<FileKind>(kind);
    metadata.writable = reader.read_bool();
    reader.require_zero(2);
    metadata.mount = reader.read_u64();
    metadata.size = reader.read_u64();
    metadata.modified_ns = reader.read_i64();
    metadata.revision = reader.read_u64();
    return metadata;
}

struct PollState {
    double elapsed{};
    std::deque<FileChange> pending;
};

} // namespace

std::vector<FileChange> compare_snapshots(const DirectorySnapshot& before,
                                         const DirectorySnapshot& after,
                                         WatchId watch, std::size_t maximum_changes) {
    IoLimits validation_limits;
    validation_limits.max_entries = 1000000;
    validation_limits.max_file_bytes = std::numeric_limits<std::uint64_t>::max();
    validate_snapshot(before, validation_limits);
    validate_snapshot(after, validation_limits);
    if (before.root != after.root || before.recursive != after.recursive ||
        before.hashes_contents != after.hashes_contents) {
        fail(IoError::malformed_snapshot, after.root, "snapshot scopes do not match");
    }
    std::vector<FileChange> result;
    auto append = [&](FileChange change) {
        if (result.size() >= maximum_changes) {
            fail(IoError::limit_exceeded, after.root, "change event limit exceeded");
        }
        result.push_back(std::move(change));
    };
    std::size_t left{};
    std::size_t right{};
    while (left < before.entries.size() || right < after.entries.size()) {
        if (right == after.entries.size() || (left < before.entries.size() &&
            before.entries[left].metadata.path < after.entries[right].metadata.path)) {
            const auto& entry = before.entries[left++];
            append({watch, ChangeKind::removed, entry.metadata.path, entry.metadata, std::nullopt});
        } else if (left == before.entries.size() ||
                   after.entries[right].metadata.path < before.entries[left].metadata.path) {
            const auto& entry = after.entries[right++];
            append({watch, ChangeKind::added, entry.metadata.path, std::nullopt, entry.metadata});
        } else {
            const auto& old_entry = before.entries[left++];
            const auto& new_entry = after.entries[right++];
            if (!same_metadata(old_entry.metadata, new_entry.metadata) ||
                old_entry.content_hash != new_entry.content_hash) {
                append({watch, ChangeKind::modified, new_entry.metadata.path,
                        old_entry.metadata, new_entry.metadata});
            }
        }
    }
    return result;
}

std::vector<std::byte> encode_snapshot(const DirectorySnapshot& snapshot, IoLimits limits) {
    validate_limits(limits);
    validate_snapshot(snapshot, limits);
    if (snapshot.entries.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail(IoError::limit_exceeded, snapshot.root, "snapshot entry count cannot be encoded");
    }
    core::ByteWriter writer(limits.max_write_bytes);
    writer.write_bytes("GEVFS001");
    writer.write_u16(1);
    writer.write_bool(snapshot.recursive);
    writer.write_bool(snapshot.hashes_contents);
    writer.write_u32(static_cast<std::uint32_t>(snapshot.entries.size()));
    writer.write_string(snapshot.root, limits.max_path_bytes);
    for (const auto& entry : snapshot.entries) {
        write_metadata(writer, entry.metadata, limits.max_path_bytes);
        writer.write_u64(entry.content_hash);
    }
    const auto checksum = core::crc32(writer.bytes());
    writer.write_u32(checksum);
    return writer.take();
}

DirectorySnapshot decode_snapshot(std::span<const std::byte> bytes, IoLimits limits) {
    validate_limits(limits);
    if (bytes.size() < 21 || bytes.size() > limits.max_read_bytes) {
        fail(IoError::malformed_snapshot, {}, "snapshot length is invalid");
    }
    const auto payload = bytes.first(bytes.size() - 4);
    core::ByteReader checksum_reader(bytes.last(4));
    if (checksum_reader.read_u32() != core::crc32(payload)) {
        fail(IoError::malformed_snapshot, {}, "snapshot checksum mismatch");
    }
    core::BinaryLimits binary_limits;
    binary_limits.max_total_bytes = limits.max_read_bytes;
    binary_limits.max_string_bytes = limits.max_path_bytes;
    core::ByteReader reader(payload, core::ByteOrder::little, binary_limits);
    reader.expect("GEVFS001");
    if (reader.read_u16() != 1) {
        fail(IoError::malformed_snapshot, {}, "unsupported snapshot version");
    }
    DirectorySnapshot result;
    result.recursive = reader.read_bool();
    result.hashes_contents = reader.read_bool();
    const auto count = reader.read_u32();
    if (count > limits.max_entries || static_cast<std::uint64_t>(count) * 45ULL > reader.remaining()) {
        fail(IoError::malformed_snapshot, {}, "snapshot entry count exceeds payload");
    }
    result.root = reader.read_string();
    result.entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto metadata = read_metadata(reader);
        const auto hash = reader.read_u64();
        result.entries.push_back({std::move(metadata), hash});
    }
    reader.require_end();
    validate_snapshot(result, limits);
    return result;
}

void register_filesystem(core::Kernel& kernel) {
    if (kernel.services().contains<VirtualFileSystem>()) {
        return;
    }
    auto& filesystem = kernel.services().emplace<VirtualFileSystem>();
    auto& polling = kernel.services().emplace<PollState>();
    (void)polling;
    (void)kernel.register_module({"io.filesystem", "Bounded virtual mounts, file access and change polling", 1});
    const auto read_root = kernel.config().text("io.root");
    if (!read_root.empty()) {
        (void)filesystem.mount_directory(native_utf8(read_root), {"/assets", 0, false});
    }
    const auto writable_root = kernel.config().text("io.writable_root");
    if (!writable_root.empty()) {
        (void)filesystem.mount_directory(native_utf8(writable_root), {"/user", 0, true});
    }
    (void)filesystem.mount_memory({"/memory", 0, true});
    (void)kernel.add_command("io.mount", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 3) {
            return core::CommandResult{false, "usage: io.mount native_directory virtual_root [priority]"};
        }
        const int priority = args.size() == 3 ? parse_integer<int>(args[2], "mount priority") : 0;
        const auto id = host.services().get<VirtualFileSystem>().mount_directory(
            native_utf8(args[0]), {args[1], priority, false});
        return core::CommandResult{true, "mounted read-only directory " + std::to_string(id)};
    }, "Mount an existing native directory read-only; writable roots are startup configuration");
    (void)kernel.add_command("io.memory", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: io.memory virtual_root"};
        }
        const auto id = host.services().get<VirtualFileSystem>().mount_memory({args[0], 0, true});
        return core::CommandResult{true, "mounted memory directory " + std::to_string(id)};
    }, "Create a writable in-memory mount");
    (void)kernel.add_command("io.unmount", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: io.unmount mount_id"};
        }
        const bool removed = host.services().get<VirtualFileSystem>().unmount(
            parse_integer<MountId>(args[0], "mount id"));
        return core::CommandResult{removed, removed ? "unmounted" : "unknown mount"};
    }, "Remove a mount while preserving already-open memory versions");
    (void)kernel.add_command("io.mounts", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: io.mounts"};
        }
        std::ostringstream result;
        for (const auto& mount : host.services().get<VirtualFileSystem>().mounts()) {
            result << mount.id << ' ' << mount.virtual_root << ' '
                   << (mount.kind == MountKind::memory ? "memory" : "directory") << ' '
                   << (mount.writable ? "writable" : "read-only") << " priority=" << mount.priority << '\n';
        }
        return core::CommandResult{true, result.str()};
    }, "List mounts in overlay precedence order");
    (void)kernel.add_command("io.list", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() > 2 || (args.size() == 2 && args[1] != "recursive")) {
            return core::CommandResult{false, "usage: io.list [path] [recursive]"};
        }
        const auto path = args.empty() ? std::string_view{"/"} : std::string_view(args[0]);
        const auto entries = host.services().get<VirtualFileSystem>().list(path,
            {args.size() == 2, true, 4096});
        std::ostringstream result;
        for (const auto& entry : entries) {
            result << (entry.kind == FileKind::directory ? "dir " : "file ")
                   << entry.path << ' ' << entry.size << '\n';
        }
        return core::CommandResult{true, result.str()};
    }, "List visible entries in deterministic virtual-path order");
    (void)kernel.add_command("io.read", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: io.read path"};
        }
        return core::CommandResult{true,
            host.services().get<VirtualFileSystem>().read_text(args[0], 1024U * 1024U)};
    }, "Read a bounded UTF-8 text file");
    (void)kernel.add_command("io.write", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: io.write path text"};
        }
        host.services().get<VirtualFileSystem>().write_text(args[0], args[1], {true, true, {}});
        return core::CommandResult{true, "written " + std::to_string(args[1].size()) + " bytes"};
    }, "Write text to a configured writable directory or memory mount");
    (void)kernel.add_command("io.remove", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: io.remove path"};
        }
        const bool removed = host.services().get<VirtualFileSystem>().remove(args[0]);
        return core::CommandResult{removed, removed ? "removed" : "file not found"};
    }, "Remove a regular file from a writable mount");
    (void)kernel.add_command("io.copy", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: io.copy source destination"};
        }
        host.services().get<VirtualFileSystem>().copy(args[0], args[1], {true, true, {}});
        return core::CommandResult{true, "copied"};
    }, "Copy a bounded file through the virtual filesystem");
    (void)kernel.add_command("io.hash", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: io.hash path"};
        }
        return core::CommandResult{true,
            std::to_string(host.services().get<VirtualFileSystem>().content_hash(args[0]))};
    }, "Stream a file into a noncryptographic FNV content checksum");
    (void)kernel.add_command("io.watch", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "hash")) {
            return core::CommandResult{false, "usage: io.watch path [hash]"};
        }
        const auto id = host.services().get<VirtualFileSystem>().watch(args[0],
            {true, args.size() == 2, 8192});
        return core::CommandResult{true, "watch " + std::to_string(id)};
    }, "Observe a recursive snapshot, optionally comparing content checksums");
    (void)kernel.add_command("io.unwatch", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: io.unwatch watch_id"};
        }
        const bool removed = host.services().get<VirtualFileSystem>().unwatch(
            parse_integer<WatchId>(args[0], "watch id"));
        return core::CommandResult{removed, removed ? "watch removed" : "unknown watch"};
    }, "Stop observing a directory");
    (void)kernel.add_command("io.snapshot", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: io.snapshot root output_file"};
        }
        auto& vfs = host.services().get<VirtualFileSystem>();
        const auto snapshot = vfs.snapshot(args[0], {true, false, 8192});
        const auto bytes = encode_snapshot(snapshot, vfs.limits());
        vfs.write(args[1], bytes, {true, true, {}});
        return core::CommandResult{true, "saved " + std::to_string(snapshot.entries.size()) + " entries"};
    }, "Serialize a versioned, checksummed directory snapshot");
    (void)kernel.add_system({"io.filesystem", core::Phase::Input, -500, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& vfs = host.services().get<VirtualFileSystem>();
            auto& state = host.services().get<PollState>();
            state.elapsed += frame.delta_seconds;
            const auto interval = std::clamp(host.config().number("io.poll_interval", 0.25), 0.01, 60.0);
            while (!state.pending.empty()) {
                if (!host.events().publish(state.pending.front())) {
                    break;
                }
                state.pending.pop_front();
            }
            if (state.elapsed >= interval && state.pending.empty()) {
                state.elapsed = 0.0;
                try {
                    auto result = vfs.poll_changes();
                    for (auto& event : result.changes) {
                        state.pending.push_back(std::move(event));
                    }
                    while (!state.pending.empty()) {
                        if (!host.events().publish(state.pending.front())) {
                            break;
                        }
                        state.pending.pop_front();
                    }
                    if (result.overflow) {
                        host.diagnose(core::Diagnostic::Severity::Warning, "io.filesystem",
                                      "change batch exceeded limit; narrow the watch or raise its configured capacity");
                    }
                } catch (const IoException& error) {
                    host.diagnose(core::Diagnostic::Severity::Warning, "io.filesystem",
                                  std::string("snapshot retained after polling error: ") + error.what());
                }
            }
            const auto statistics = vfs.statistics();
            host.metric("io.mounts", static_cast<double>(statistics.mounts));
            host.metric("io.watches", static_cast<double>(statistics.watches));
            host.metric("io.bytes_read", static_cast<double>(statistics.bytes_read));
            host.metric("io.bytes_written", static_cast<double>(statistics.bytes_written));
            host.metric("io.memory_reserved", static_cast<double>(statistics.memory_bytes));
            host.metric("io.open_cursors", static_cast<double>(statistics.open_cursors));
            host.metric("io.pending_changes", static_cast<double>(state.pending.size()));
        });
}

} // namespace ge::io
