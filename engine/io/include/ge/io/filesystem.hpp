#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::io {

using MountId = std::uint64_t;
using WatchId = std::uint64_t;

enum class IoError : std::uint8_t {
    invalid_path,
    not_found,
    not_a_file,
    not_a_directory,
    access_denied,
    outside_mount,
    limit_exceeded,
    invalid_offset,
    filesystem_failure,
    short_read,
    short_write,
    already_exists,
    exhausted,
    malformed_snapshot
};

class IoException final : public std::runtime_error {
public:
    IoException(IoError code, std::string path, std::string message);
    [[nodiscard]] IoError code() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;

private:
    IoError code_;
    std::string path_;
};

struct IoLimits {
    std::size_t max_path_bytes{4096};
    std::size_t max_mounts{64};
    std::size_t max_entries{65536};
    std::size_t max_read_bytes{64U * 1024U * 1024U};
    std::size_t max_write_bytes{64U * 1024U * 1024U};
    std::size_t max_memory_bytes{256U * 1024U * 1024U};
    std::size_t max_watches{64};
    std::size_t max_changes{8192};
    std::uint64_t max_file_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t max_directory_depth{128};
};

struct MountOptions {
    std::string virtual_root{"/"};
    int priority{};
    bool writable{};
};

enum class MountKind : std::uint8_t { directory, memory };
enum class FileKind : std::uint8_t { regular, directory };

struct MountInfo {
    MountId id{};
    MountKind kind{};
    std::string virtual_root;
    std::filesystem::path physical_root;
    int priority{};
    bool writable{};
    std::uint64_t revision{};
    std::size_t memory_bytes{};
    std::size_t memory_files{};
};

struct FileMetadata {
    std::string path;
    FileKind kind{FileKind::regular};
    MountId mount{};
    std::uint64_t size{};
    std::int64_t modified_ns{};
    std::uint64_t revision{};
    bool writable{};
};

struct MemoryEntry {
    std::string path;
    std::vector<std::byte> bytes;
};

struct ListOptions {
    bool recursive{};
    bool include_directories{true};
    std::size_t maximum_entries{};
};

struct WriteOptions {
    bool overwrite{true};
    bool create_parents{};
    std::optional<std::uint64_t> expected_cache_key;
};

struct IoStatistics {
    std::uint64_t read_operations{};
    std::uint64_t write_operations{};
    std::uint64_t bytes_read{};
    std::uint64_t bytes_written{};
    std::uint64_t open_cursors{};
    std::uint64_t read_failures{};
    std::uint64_t change_events{};
    std::size_t mounts{};
    std::size_t watches{};
    std::size_t memory_bytes{};
};

// Normalized paths use a leading slash. Parent traversal, drive/UNC syntax,
// control bytes and invalid UTF-8 are rejected rather than silently repaired.
[[nodiscard]] std::string normalize_virtual_path(std::string_view path,
                                                std::size_t maximum_bytes = 4096);
[[nodiscard]] std::string join_virtual_path(std::string_view base,
                                          std::string_view relative,
                                          std::size_t maximum_bytes = 4096);
[[nodiscard]] std::string virtual_parent(std::string_view path);
[[nodiscard]] std::string virtual_filename(std::string_view path);
[[nodiscard]] std::string virtual_extension(std::string_view path);
[[nodiscard]] bool virtual_contains(std::string_view parent,
                                    std::string_view child) noexcept;
[[nodiscard]] std::uint64_t metadata_cache_key(const FileMetadata& metadata);

class ReadCursor final {
public:
    ReadCursor() noexcept;
    ~ReadCursor();
    ReadCursor(ReadCursor&&) noexcept;
    ReadCursor& operator=(ReadCursor&&) noexcept;
    ReadCursor(const ReadCursor&) = delete;
    ReadCursor& operator=(const ReadCursor&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const FileMetadata& metadata() const;
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] std::uint64_t position() const noexcept;
    [[nodiscard]] std::uint64_t remaining() const noexcept;
    [[nodiscard]] bool eof() const noexcept;
    void seek(std::uint64_t position);
    void skip(std::uint64_t count);
    [[nodiscard]] std::size_t read(std::span<std::byte> destination);
    [[nodiscard]] std::vector<std::byte> read_exact(std::size_t count);
    void close() noexcept;

private:
    friend class VirtualFileSystem;
    struct State;
    explicit ReadCursor(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

struct SnapshotEntry {
    FileMetadata metadata;
    std::uint64_t content_hash{};
};

struct DirectorySnapshot {
    std::string root;
    bool recursive{};
    bool hashes_contents{};
    std::vector<SnapshotEntry> entries;
};

enum class ChangeKind : std::uint8_t { added, modified, removed };

struct FileChange {
    WatchId watch{};
    ChangeKind kind{};
    std::string path;
    std::optional<FileMetadata> before;
    std::optional<FileMetadata> after;
};

struct WatchOptions {
    bool recursive{true};
    bool hash_contents{};
    std::size_t maximum_entries{};
};

struct WatchInfo {
    WatchId id{};
    std::string root;
    WatchOptions options;
    std::size_t observed_entries{};
};

struct PollResult {
    std::vector<FileChange> changes;
    std::size_t watches_polled{};
    bool overflow{};
};

[[nodiscard]] std::vector<FileChange> compare_snapshots(
    const DirectorySnapshot& before, const DirectorySnapshot& after,
    WatchId watch = 0, std::size_t maximum_changes = 8192);
[[nodiscard]] std::vector<std::byte> encode_snapshot(
    const DirectorySnapshot& snapshot, IoLimits limits = {});
[[nodiscard]] DirectorySnapshot decode_snapshot(
    std::span<const std::byte> bytes, IoLimits limits = {});

// Filesystem operations are serialized internally. Memory cursors retain an
// immutable file version. Native cursors retain an open file, not an OS snapshot.
// Canonical containment is validated before I/O. Hostile concurrent replacement
// of filesystem ancestors requires stronger native directory-handle APIs.
class VirtualFileSystem final {
public:
    explicit VirtualFileSystem(IoLimits limits = {});
    ~VirtualFileSystem();
    VirtualFileSystem(const VirtualFileSystem&) = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;
    VirtualFileSystem(VirtualFileSystem&&) = delete;
    VirtualFileSystem& operator=(VirtualFileSystem&&) = delete;

    [[nodiscard]] const IoLimits& limits() const noexcept;
    [[nodiscard]] MountId mount_directory(const std::filesystem::path& physical_root,
                                          MountOptions options = {});
    [[nodiscard]] MountId mount_memory(MountOptions options = {},
                                       std::span<const MemoryEntry> initial = {});
    bool unmount(MountId mount);
    [[nodiscard]] std::vector<MountInfo> mounts() const;
    [[nodiscard]] std::optional<FileMetadata> metadata(std::string_view path) const;
    [[nodiscard]] bool exists(std::string_view path) const;
    [[nodiscard]] std::uint64_t cache_key(std::string_view path) const;
    [[nodiscard]] std::uint64_t content_hash(std::string_view path);
    [[nodiscard]] std::vector<FileMetadata> list(std::string_view path = "/",
                                               ListOptions options = {}) const;
    [[nodiscard]] ReadCursor open_read(std::string_view path);
    [[nodiscard]] std::vector<std::byte> read(std::string_view path,
                                            std::size_t maximum_bytes = 0);
    [[nodiscard]] std::vector<std::byte> read_range(std::string_view path,
                                                  std::uint64_t offset,
                                                  std::size_t length);
    [[nodiscard]] std::string read_text(std::string_view path,
                                       std::size_t maximum_bytes = 0);
    void write(std::string_view path, std::span<const std::byte> bytes,
               WriteOptions options = {});
    void write_text(std::string_view path, std::string_view text,
                    WriteOptions options = {});
    // Removing an in-mount file symlink unlinks the entry and preserves its target.
    bool remove(std::string_view path);
    void copy(std::string_view source, std::string_view destination,
              WriteOptions options = {});
    [[nodiscard]] DirectorySnapshot snapshot(std::string_view path,
                                             WatchOptions options = {});
    [[nodiscard]] WatchId watch(std::string_view path, WatchOptions options = {});
    bool unwatch(WatchId watch);
    [[nodiscard]] std::vector<WatchInfo> watches() const;
    [[nodiscard]] PollResult poll_changes(std::size_t maximum_changes = 0);
    [[nodiscard]] IoStatistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using Vfs = VirtualFileSystem;

void register_filesystem(core::Kernel& kernel);

} // namespace ge::io
