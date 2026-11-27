#pragma once

#include <ge/scene/serialization.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::io { class VirtualFileSystem; }
namespace ge::world { class StateSnapshot; class PersistentState; }

namespace ge::persistence {

using TransactionId = std::uint64_t;
inline constexpr std::uint16_t journal_format_version = 1;
inline constexpr std::uint32_t scene_value_type = 0x53434e45U;
inline constexpr std::uint32_t state_value_type = 0x53544154U;

enum class JournalError : std::uint8_t {
    None,
    InvalidArgument,
    Limit,
    NotOpen,
    NotFound,
    TransactionActive,
    NoTransaction,
    WrongTransaction,
    Conflict,
    Corrupt,
    UnsupportedVersion,
    Truncated,
    IoFailure,
    AllocationFailure,
    RequiresCheckpoint,
    DocumentFailure,
    StateFailure,
    Exhausted
};

[[nodiscard]] std::string_view describe(JournalError error) noexcept;

struct JournalResult {
    JournalError error{JournalError::None};
    std::size_t offset{};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == JournalError::None;
    }
};

struct JournalLimits {
    std::size_t max_journal_bytes{64U * 1024U * 1024U};
    std::size_t max_checkpoint_bytes{64U * 1024U * 1024U};
    std::size_t max_value_bytes{16U * 1024U * 1024U};
    std::size_t max_state_bytes{32U * 1024U * 1024U};
    std::size_t max_transaction_bytes{32U * 1024U * 1024U};
    std::size_t max_entries{4096};
    std::size_t max_operations{4096};
    std::size_t max_records{65536};
    std::size_t max_key_bytes{256};
    std::size_t max_label_bytes{256};
    std::size_t max_slots{256};
    std::size_t retained_generations{3};
    std::size_t generation_probes{128};
};

struct JournalValue {
    std::uint32_t type{};
    std::uint16_t version{1};
    std::vector<std::byte> bytes;
    bool operator==(const JournalValue&) const = default;
};

struct JournalEntryInfo {
    std::string key;
    std::uint32_t type{};
    std::uint16_t version{};
    std::size_t bytes{};
    std::uint32_t checksum{};
};

enum class RecoveryPolicy : std::uint8_t {
    Strict,
    // Stops at the first invalid record without resynchronizing. Every later
    // byte is discarded, even if it contains an apparently valid commit.
    CommittedPrefix,
    // Tries manifest generations newest first, accepting only a valid log.
    PreviousGeneration
};

struct RecoveryReport {
    JournalResult status;
    std::uint64_t generation{};
    std::uint64_t last_sequence{};
    std::uint64_t committed_sequence{};
    std::size_t records{};
    std::size_t committed_transactions{};
    std::size_t aborted_transactions{};
    std::size_t discarded_transactions{};
    // Byte counts describe the selected generation. Fallback may lose an
    // unquantifiable number of commits from rejected newer generations.
    std::size_t accepted_bytes{};
    std::size_t discarded_bytes{};
    std::size_t failure_offset{};
    JournalError suffix_error{JournalError::None};
    std::string suffix_message;
    bool used_previous_generation{};
    bool requires_checkpoint{};
};

struct JournalStatus {
    bool open{};
    bool requires_checkpoint{};
    bool memory_backed{};
    std::string root;
    std::string slot;
    std::uint64_t generation{};
    std::uint64_t last_sequence{};
    std::uint64_t committed_sequence{};
    TransactionId active_transaction{};
    std::size_t entries{};
    std::size_t state_bytes{};
    std::size_t journal_bytes{};
    std::size_t records{};
    std::size_t pending_operations{};
    std::size_t retained_generations{};
};

struct SaveSlotInfo {
    std::string name;
    std::uint64_t generation{};
    std::size_t retained_generations{};
    JournalResult status;
};

struct RetentionReport {
    std::size_t removed_files{};
    std::size_t retained_files{};
    std::size_t failed_files{};
    std::string first_failure;
};

// One externally serialized owner must hold exclusive ownership of the slot
// namespace and its VFS mounts. No interprocess lock is provided. VFS cache-key
// checks detect ordinary stale publication; they are not a hostile-writer lock.
// All reads expose committed values only. At most one transaction is active.
// Sequence and transaction IDs increase within the accepted history. An explicit
// rollback can discard larger IDs; identify restored history with its generation.
// Every record uses a bounded complete-file VFS publication: the VFS has no
// append primitive. Memory mounts remain volatile. Native flush/close/rename
// provide logical publication, not fsync or hardware crash durability.
// Checksums and digests detect accidental corruption; they do not authenticate.
// Borrowed VFS must outlive an open journal. Destruction performs no I/O.
class SaveJournal final {
public:
    explicit SaveJournal(JournalLimits limits = {});
    ~SaveJournal();
    SaveJournal(const SaveJournal&) = delete;
    SaveJournal& operator=(const SaveJournal&) = delete;
    SaveJournal(SaveJournal&&) = delete;
    SaveJournal& operator=(SaveJournal&&) = delete;

    [[nodiscard]] const JournalLimits& limits() const noexcept;
    [[nodiscard]] JournalStatus inspect() const;
    [[nodiscard]] std::vector<JournalEntryInfo> entries() const;
    // Returned pointers borrow the committed view and expire on commit/open/
    // recover/close. Pending edits never mutate a borrowed committed value.
    [[nodiscard]] const JournalValue* find(std::string_view key) const noexcept;
    [[nodiscard]] const RecoveryReport& last_recovery() const noexcept;

    // Failed open/recover preserve the current committed view and binding.
    // create_missing only creates when no manifest or generation file exists.
    JournalResult open(io::VirtualFileSystem& files, std::string_view root,
                       std::string_view slot, bool create_missing = false,
                       RecoveryPolicy policy = RecoveryPolicy::Strict,
                       RecoveryReport* report = nullptr);
    // Successful recovery deliberately abandons any locally unfinished draft.
    JournalResult recover(RecoveryPolicy policy, RecoveryReport& report);
    JournalResult close();
    JournalResult begin(TransactionId& transaction, std::string_view label = {});
    JournalResult put(TransactionId transaction, std::string_view key,
                      std::span<const std::byte> bytes, std::uint32_t type = 0,
                      std::uint16_t version = 1);
    JournalResult remove(TransactionId transaction, std::string_view key);
    JournalResult commit(TransactionId transaction);
    JournalResult abort(TransactionId transaction);

    // Checkpoint publishes a fresh snapshot/log generation before switching the
    // manifest. It also acknowledges explicit prefix/previous-generation repair.
    // Failure leaves the current generation selected; orphan cleanup is separate.
    JournalResult checkpoint();
    JournalResult prune(RetentionReport& report);
    JournalResult list_slots(io::VirtualFileSystem& files, std::string_view root,
                             std::vector<SaveSlotInfo>& destination) const;

    // A failed convenience save may leave a persisted unfinished transaction;
    // inspect/abort/recover explicitly resolve it. No live scene is replaced.
    JournalResult save_document(std::string_view key,
                                const scene::SceneDocumentStore& document);
    JournalResult capture_scene(std::string_view key, scene::Scene& source,
                                scene::SceneArchiveLimits limits = {});
    JournalResult load_document(std::string_view key,
                                scene::SceneDocumentStore& destination,
                                scene::SceneDecodeOptions options = {}) const;
    JournalResult put_state(TransactionId transaction, std::string_view key,
                            const world::StateSnapshot& snapshot);
    JournalResult restore_state(std::string_view key,
                                world::PersistentState& destination) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using JournalService = SaveJournal;
void register_journal(core::Kernel& kernel);

} // namespace ge::persistence
