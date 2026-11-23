#include <ge/world/state.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ge::world {
namespace {

constexpr std::array<std::uint8_t, 8> state_magic{'G', 'E', 'S', 'T', 'A', 'T', 'E', '1'};

[[noreturn]] void invalid(std::string_view reason) {
    throw std::runtime_error(std::string("persistent state: ") + std::string(reason));
}

[[noreturn]] void io_failure(std::string_view reason, const std::error_code& error) {
    invalid(std::string(reason) + ": " + error.message());
}

void validate_path(const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty() || path.filename() == "." ||
        path.filename() == ".." ||
        path.native().find(std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos) {
        invalid("invalid file path");
    }
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::span<const std::uint8_t> take(std::size_t size) {
        if (size > bytes_.size() - position_) {
            invalid("truncated record");
        }
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

    [[nodiscard]] std::uint32_t u32() {
        const auto bytes = take(4U);
        std::uint32_t result{};
        for (unsigned i = 0; i < 4U; ++i) {
            result |= static_cast<std::uint32_t>(bytes[i]) << (8U * i);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t u64() {
        const auto bytes = take(8U);
        std::uint64_t result{};
        for (unsigned i = 0; i < 8U; ++i) {
            result |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
        }
        return result;
    }

    [[nodiscard]] bool finished() const noexcept { return position_ == bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const auto mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    validate_path(path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        invalid("save must be a readable regular file");
    }
    if (error) {
        io_failure("cannot inspect save", error);
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        invalid("cannot open save");
    }
    const auto end = file.tellg();
    if (end == std::ifstream::pos_type(-1)) {
        invalid("cannot determine save size");
    }
    const auto length = static_cast<std::streamoff>(end);
    if (length < static_cast<std::streamoff>(state_header_bytes + state_checksum_bytes)) {
        invalid("truncated header or checksum");
    }
    if (length > static_cast<std::streamoff>(max_state_file_bytes)) {
        invalid("save exceeds 4 MiB limit");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0, std::ios::beg);
    if (!file) {
        invalid("cannot seek save");
    }
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size())) {
        invalid("truncated save or read failure");
    }
    char extra{};
    if (file.get(extra)) {
        invalid("save grew while being read");
    }
    if (!file.eof() || file.bad()) {
        invalid("save read failure");
    }
    return bytes;
}

// The x mode exclusively creates the sibling; a collision never truncates an
// existing file. The private Windows helper preserves native wide path names.
[[nodiscard]] int open_exclusive(const std::filesystem::path& path, std::FILE*& file) {
#ifdef _WIN32
    return static_cast<int>(_wfopen_s(&file, path.c_str(), L"wbx"));
#else
    errno = 0;
    file = std::fopen(path.c_str(), "wbx");
    return file != nullptr ? 0 : errno;
#endif
}

class TemporaryFile final {
public:
    explicit TemporaryFile(const std::filesystem::path& destination) {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto parent = destination.parent_path();
        for (unsigned attempt = 0; attempt < 64U; ++attempt) {
            path_ = parent / (".ge-state-" + std::to_string(stamp) + "-" +
                              std::to_string(attempt) + ".tmp");
            if (path_.filename() == destination.filename()) {
                continue;
            }
            const auto result = open_exclusive(path_, file_);
            if (result == 0 && file_ != nullptr) {
                owned_ = true;
                return;
            }
            if (result != EEXIST) {
                io_failure("cannot create sibling temporary file",
                           std::error_code(result, std::generic_category()));
            }
        }
        invalid("cannot reserve a sibling temporary filename");
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile() {
        if (file_ != nullptr) {
            (void)std::fclose(file_);
        }
        if (owned_) {
            std::error_code ignored;
            (void)std::filesystem::remove(path_, ignored);
        }
    }

    void write_and_close(std::span<const std::uint8_t> bytes) {
        if (std::fwrite(bytes.data(), 1U, bytes.size(), file_) != bytes.size()) {
            invalid("temporary save write failed");
        }
        if (std::fflush(file_) != 0) {
            invalid("temporary save flush failed");
        }
        auto* closing = std::exchange(file_, nullptr);
        if (std::fclose(closing) != 0) {
            invalid("temporary save close failed");
        }
    }

    void publish(const std::filesystem::path& destination) {
#ifdef _WIN32
        // Same-directory move only: never enable COPY_ALLOWED or delete the
        // destination first. This is checked replacement, not crash durability.
        // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw
        if (MoveFileExW(path_.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
            const auto error = static_cast<int>(GetLastError());
            io_failure("cannot publish save", std::error_code(error, std::system_category()));
        }
#else
        std::error_code error;
        std::filesystem::rename(path_, destination, error);
        if (error) {
            io_failure("cannot publish save", error);
        }
#endif
        owned_ = false;
    }

private:
    std::filesystem::path path_;
    std::FILE* file_{};
    bool owned_{};
};

} // namespace

StateSnapshot::StateSnapshot(std::uint64_t baseline_id, std::uint64_t sequence,
                             std::vector<StateEntry> entries) noexcept
    : baseline_id_(baseline_id), sequence_(sequence), entries_(std::move(entries)) {}

std::uint32_t StateSnapshot::version() const noexcept { return state_schema_version; }
std::uint64_t StateSnapshot::baseline_id() const noexcept { return baseline_id_; }
std::uint64_t StateSnapshot::sequence() const noexcept { return sequence_; }
std::span<const StateEntry> StateSnapshot::entries() const noexcept { return entries_; }

std::vector<std::uint8_t> encode_state(const StateSnapshot& snapshot) {
    if (snapshot.baseline_id() == 0U || snapshot.sequence() == 0U ||
        snapshot.entries().size() > max_state_entries) {
        invalid("invalid snapshot metadata");
    }
    const auto length = state_header_bytes + snapshot.entries().size() * state_entry_bytes +
                        state_checksum_bytes;
    if (length > max_state_file_bytes) {
        invalid("save exceeds 4 MiB limit");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(length);
    bytes.insert(bytes.end(), state_magic.begin(), state_magic.end());
    append_u32(bytes, snapshot.version());
    append_u32(bytes, 0U);
    append_u64(bytes, snapshot.baseline_id());
    append_u64(bytes, snapshot.sequence());
    append_u32(bytes, static_cast<std::uint32_t>(snapshot.entries().size()));
    append_u32(bytes, 0U);
    std::uint64_t previous_id{};
    for (const auto& entry : snapshot.entries()) {
        if (!entry.id.valid() || entry.id.value <= previous_id) {
            invalid("snapshot IDs must be nonzero, unique, and increasing");
        }
        append_u64(bytes, entry.id.value);
        append_u32(bytes, std::bit_cast<std::uint32_t>(entry.value));
        previous_id = entry.id.value;
    }
    append_u32(bytes, crc32(bytes));
    return bytes;
}

StateSnapshot decode_state(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < state_header_bytes + state_checksum_bytes) {
        invalid("truncated header or checksum");
    }
    if (bytes.size() > max_state_file_bytes) {
        invalid("save exceeds 4 MiB limit");
    }
    const auto payload = bytes.first(bytes.size() - state_checksum_bytes);
    Reader checksum(bytes.last(state_checksum_bytes));
    if (crc32(payload) != checksum.u32()) {
        invalid("CRC32 mismatch");
    }

    Reader reader(payload);
    const auto magic = reader.take(state_magic.size());
    if (!std::equal(state_magic.begin(), state_magic.end(), magic.begin())) {
        invalid("unknown magic");
    }
    if (reader.u32() != state_schema_version) {
        invalid("unsupported schema version");
    }
    if (reader.u32() != 0U) {
        invalid("nonzero reserved header field");
    }
    const auto baseline_id = reader.u64();
    const auto sequence = reader.u64();
    const auto count = reader.u32();
    if (baseline_id == 0U || sequence == 0U) {
        invalid("zero baseline or sequence");
    }
    if (count > max_state_entries) {
        invalid("entry count exceeds limit");
    }
    if (reader.u32() != 0U) {
        invalid("nonzero reserved header field");
    }
    const std::uint64_t expected_size = state_header_bytes +
        static_cast<std::uint64_t>(count) * state_entry_bytes + state_checksum_bytes;
    if (expected_size != bytes.size()) {
        invalid("file size does not match entry count");
    }

    std::vector<StateEntry> entries;
    entries.reserve(count);
    std::uint64_t previous_id{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto id = reader.u64();
        const auto value = std::bit_cast<std::int32_t>(reader.u32());
        if (id == 0U || id <= previous_id) {
            invalid("IDs must be nonzero, unique, and increasing");
        }
        entries.push_back({ge::PersistentId{id}, value});
        previous_id = id;
    }
    if (!reader.finished()) {
        invalid("trailing payload data");
    }
    return StateSnapshot(baseline_id, sequence, std::move(entries));
}

PersistentState::PersistentState(std::uint64_t baseline_id,
                                 std::span<const StateEntry> initial_values)
    : baseline_id_(baseline_id) {
    if (baseline_id_ == 0U || initial_values.size() > max_state_entries) {
        throw std::invalid_argument("persistent state: invalid baseline or entry count");
    }
    entries_.assign(initial_values.begin(), initial_values.end());
    std::sort(entries_.begin(), entries_.end(),
              [](const StateEntry& left, const StateEntry& right) { return left.id < right.id; });
    std::uint64_t previous_id{};
    for (const auto& entry : entries_) {
        if (!entry.id.valid() || entry.id.value <= previous_id) {
            throw std::invalid_argument("persistent state: zero or duplicate initial ID");
        }
        previous_id = entry.id.value;
    }
}

bool PersistentState::set(ge::PersistentId id, std::int32_t value) noexcept {
    const auto found = std::lower_bound(entries_.begin(), entries_.end(), id,
        [](const StateEntry& entry, ge::PersistentId wanted) { return entry.id < wanted; });
    if (found == entries_.end() || found->id != id) {
        return false;
    }
    found->value = value;
    return true;
}

std::optional<std::int32_t> PersistentState::get(ge::PersistentId id) const noexcept {
    const auto found = std::lower_bound(entries_.begin(), entries_.end(), id,
        [](const StateEntry& entry, ge::PersistentId wanted) { return entry.id < wanted; });
    if (found == entries_.end() || found->id != id) {
        return std::nullopt;
    }
    return found->value;
}

std::uint64_t PersistentState::baseline_id() const noexcept { return baseline_id_; }
std::uint64_t PersistentState::sequence() const noexcept { return sequence_; }
std::size_t PersistentState::size() const noexcept { return entries_.size(); }

void PersistentState::require_newer(std::uint64_t sequence) const {
    if (sequence == 0U || sequence <= sequence_) {
        invalid("snapshot sequence must be strictly newer");
    }
}

void PersistentState::require_compatible(const StateSnapshot& snapshot) const {
    if (snapshot.baseline_id() != baseline_id_) {
        invalid("baseline mismatch");
    }
    if (snapshot.entries().size() != entries_.size()) {
        invalid("snapshot does not contain every known ID exactly once");
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (snapshot.entries()[i].id != entries_[i].id) {
            invalid("snapshot contains an unknown or missing ID");
        }
    }
}

StateSnapshot PersistentState::capture(std::uint64_t sequence) {
    require_newer(sequence);
    StateSnapshot snapshot(baseline_id_, sequence, entries_);
    sequence_ = sequence;
    return snapshot;
}

void PersistentState::restore(const StateSnapshot& snapshot) {
    require_compatible(snapshot);
    if (snapshot.sequence() == 0U || snapshot.sequence() < sequence_) {
        invalid("snapshot sequence is older than live state");
    }
    // Copy before publication; allocation failure leaves the store unchanged.
    auto replacement = snapshot.entries_;
    entries_.swap(replacement);
    sequence_ = snapshot.sequence();
}

void PersistentState::restore(std::span<const std::uint8_t> bytes) {
    restore(decode_state(bytes));
}

void PersistentState::restore(const std::filesystem::path& path) {
    restore(read_bytes(path));
}

void PersistentState::save(const std::filesystem::path& path, std::uint64_t sequence) {
    require_newer(sequence);
    validate_path(path);

    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && status.type() != std::filesystem::file_type::not_found) {
        io_failure("cannot inspect destination", error);
    }
    if (std::filesystem::exists(status)) {
        if (!std::filesystem::is_regular_file(status)) {
            invalid("destination must be a regular file, not a directory or symlink");
        }
        const auto previous = decode_state(read_bytes(path));
        require_compatible(previous);
        if (sequence <= previous.sequence()) {
            invalid("snapshot sequence is not newer than destination");
        }
    }

    const StateSnapshot snapshot(baseline_id_, sequence, entries_);
    const auto bytes = encode_state(snapshot);
    TemporaryFile temporary(path);
    temporary.write_and_close(bytes);
    temporary.publish(path);
    sequence_ = sequence;
}

} // namespace ge::world
