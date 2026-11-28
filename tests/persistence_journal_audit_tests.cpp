#include <ge/core/binary.hpp>
#include <ge/io/filesystem.hpp>
#include <ge/persistence/journal.hpp>

#include "test_support.hpp"

#include <cstdint>
#include <span>

namespace {

using namespace ge::persistence;

void overwrite_little_endian(std::span<std::byte> destination, std::uint64_t value) {
    for (auto& byte : destination) {
        byte = static_cast<std::byte>(value & 0xffU);
        value >>= 8U;
    }
}

void transaction_ids_cannot_exceed_their_record_sequence() {
    ge::io::VirtualFileSystem files;
    (void)files.mount_memory({"/saves", 0, true});
    SaveJournal journal;
    CHECK(journal.open(files, "/saves", "slot", true));
    TransactionId transaction{};
    CHECK(journal.begin(transaction));
    CHECK(journal.put(transaction, "value", ge::core::as_bytes("committed")));
    CHECK(journal.commit(transaction));
    const auto committed = *journal.find("value");
    const auto prefix = journal.inspect();
    CHECK(journal.begin(transaction));
    const auto pending = journal.inspect();

    // Keep all checksums valid while making the unfinished Begin impossible:
    // a transaction cannot be allocated before its own sequence is reached.
    auto bytes = files.read("/saves/slot/g1.journal");
    auto frame = std::span(bytes).subspan(prefix.journal_bytes);
    overwrite_little_endian(frame.subspan(20, 8), pending.last_sequence + 1);
    overwrite_little_endian(frame.subspan(40, 4), ge::core::crc32(frame.first(40)));
    overwrite_little_endian(frame.last(4), ge::core::crc32(frame.first(frame.size() - 4)));
    files.write("/saves/slot/g1.journal", bytes);

    RecoveryReport report;
    CHECK(journal.recover(RecoveryPolicy::Strict, report).error == JournalError::Corrupt);
    CHECK(report.failure_offset == prefix.journal_bytes);
    CHECK(journal.inspect().active_transaction == transaction);
    CHECK(journal.inspect().last_sequence == pending.last_sequence);
    CHECK(*journal.find("value") == committed);
    SaveJournal unopened;
    CHECK(unopened.open(files, "/saves", "slot").error == JournalError::Corrupt);
    CHECK(!unopened.inspect().open);

    CHECK(journal.recover(RecoveryPolicy::CommittedPrefix, report));
    CHECK(report.suffix_error == JournalError::Corrupt);
    CHECK(report.failure_offset == prefix.journal_bytes);
    CHECK(report.accepted_bytes == prefix.journal_bytes);
    CHECK(report.discarded_bytes == frame.size());
    CHECK(report.last_sequence == prefix.last_sequence);
    CHECK(report.committed_sequence == prefix.committed_sequence);
    CHECK(report.requires_checkpoint);
    CHECK(journal.inspect().active_transaction == 0);
    CHECK(*journal.find("value") == committed);
    CHECK(journal.begin(transaction).error == JournalError::RequiresCheckpoint);

    // Repair must publish a manifest whose counters can be read again.
    CHECK(journal.checkpoint());
    CHECK(journal.close());
    CHECK(journal.open(files, "/saves", "slot"));
    CHECK(!journal.inspect().requires_checkpoint);
    CHECK(*journal.find("value") == committed);
    CHECK(journal.begin(transaction));
    CHECK(transaction == 2);
    CHECK(journal.commit(transaction));
}

} // namespace

int main() {
    return ge::test::run("persistence_journal_audit", {
        {"transaction_sequence_validation", transaction_ids_cannot_exceed_their_record_sequence},
    });
}
