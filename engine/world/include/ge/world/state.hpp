#pragma once

#include <ge/core/identity.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ge::world {

inline constexpr std::uint32_t state_schema_version = 1U;
inline constexpr std::size_t state_header_bytes = 40U;
inline constexpr std::size_t state_entry_bytes = 12U;
inline constexpr std::size_t state_checksum_bytes = 4U;
inline constexpr std::size_t max_state_entries = 65536U;
inline constexpr std::size_t max_state_file_bytes = 4U * 1024U * 1024U;

struct StateEntry {
    ge::PersistentId id{};
    std::int32_t value{};

    constexpr bool operator==(const StateEntry&) const noexcept = default;
};

// An owned, read-only value snapshot. Its records contain stable IDs and scalar
// values only; it has no dependency on the originating store's lifetime.
// No mutable record access or assignment is exposed.
class StateSnapshot final {
public:
    StateSnapshot(const StateSnapshot&) = default;
    StateSnapshot(StateSnapshot&&) noexcept = default;
    StateSnapshot& operator=(const StateSnapshot&) = delete;
    StateSnapshot& operator=(StateSnapshot&&) = delete;

    [[nodiscard]] std::uint32_t version() const noexcept;
    [[nodiscard]] std::uint64_t baseline_id() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] std::span<const StateEntry> entries() const noexcept;

private:
    friend class PersistentState;
    friend StateSnapshot decode_state(std::span<const std::uint8_t> bytes);

    StateSnapshot(std::uint64_t baseline_id, std::uint64_t sequence,
                  std::vector<StateEntry> entries) noexcept;

    std::uint64_t baseline_id_;
    std::uint64_t sequence_;
    std::vector<StateEntry> entries_;
};

// GESTATE1 v1 is little-endian: 8-byte magic, u32 version, u32 reserved=0,
// u64 baseline, u64 sequence, u32 count, u32 reserved=0, count records of
// (u64 persistent ID, i32 value), then IEEE CRC32 of every preceding byte.
// IDs must be nonzero and strictly increasing. Empty snapshots are permitted.
// Baseline and sequence must be nonzero. No trailing bytes are accepted.
// CRC32 detects accidental corruption; it provides no authenticity guarantee.
// Invalid input throws std::runtime_error; allocation failures may also throw.
[[nodiscard]] std::vector<std::uint8_t> encode_state(const StateSnapshot& snapshot);
[[nodiscard]] StateSnapshot decode_state(std::span<const std::uint8_t> bytes);

// Synchronous scalar state for one baseline and an immutable set of known IDs.
// The caller serializes access to both this object and its save paths. This is
// neither a thread-safe store nor an interprocess publication protocol.
class PersistentState final {
public:
    // Copies and sorts initial values. A zero baseline, zero/duplicate IDs, or
    // more than max_state_entries throws std::invalid_argument. An empty known
    // ID set is valid. The initial sequence is zero (no snapshot accepted yet).
    explicit PersistentState(std::uint64_t baseline_id,
                             std::span<const StateEntry> initial_values);

    PersistentState(const PersistentState&) = delete;
    PersistentState& operator=(const PersistentState&) = delete;
    PersistentState(PersistentState&&) = delete;
    PersistentState& operator=(PersistentState&&) = delete;

    // Unknown/invalid IDs return false or nullopt without adding records.
    // Editing values does not advance the snapshot sequence.
    [[nodiscard]] bool set(ge::PersistentId id, std::int32_t value) noexcept;
    [[nodiscard]] std::optional<std::int32_t> get(ge::PersistentId id) const noexcept;
    [[nodiscard]] std::uint64_t baseline_id() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // Requires sequence > this->sequence() and sequence != 0. Only successful
    // capture advances this store's sequence; snapshots remain independent of
    // subsequent edits. Exhaustion at UINT64_MAX does not wrap to zero.
    [[nodiscard]] StateSnapshot capture(std::uint64_t sequence);

    // Validates the entire snapshot, matching baseline and the exact known ID
    // set before replacing any live value. An older sequence is rejected; an
    // equal sequence is accepted to permit restoring a save after local edits.
    // Failure preserves all live values and the previous sequence.
    void restore(const StateSnapshot& snapshot);
    void restore(std::span<const std::uint8_t> bytes);
    void restore(const std::filesystem::path& path);

    // Requires a strictly newer sequence than both the store and an existing
    // compatible destination. Invalid/incompatible existing files are never
    // overwritten. The caller chooses an existing parent directory; this API
    // does not create parents or impose a filesystem sandbox.
    //
    // Writes an exclusively created sibling temporary file, checks write,
    // flush and close, then checks publication (POSIX rename / Windows
    // MoveFileExW with REPLACE_EXISTING). It never deletes the old destination
    // before replacement. Failed operations preserve the old destination and
    // live sequence; temporary-file cleanup on failure is best effort.
    //
    // No fsync/directory-sync or power-loss/crash durability is promised.
    // Validation and I/O failures throw std::runtime_error; allocation errors
    // may also throw. Sequence advances only after successful publication.
    void save(const std::filesystem::path& path, std::uint64_t sequence);

private:
    void require_newer(std::uint64_t sequence) const;
    void require_compatible(const StateSnapshot& snapshot) const;

    const std::uint64_t baseline_id_;
    std::uint64_t sequence_{};
    std::vector<StateEntry> entries_;
};

} // namespace ge::world
