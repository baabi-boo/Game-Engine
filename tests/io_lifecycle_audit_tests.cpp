#include <ge/io/filesystem.hpp>

#include "test_support.hpp"

namespace {

void failed_poll_retains_every_watch_snapshot() {
    ge::io::VirtualFileSystem filesystem;
    (void)filesystem.mount_memory({"/files", 0, true});
    filesystem.write_text("/files/first/original", "first");
    filesystem.write_text("/files/second/original", "second");
    const auto first = filesystem.watch("/files/first");
    const auto second = filesystem.watch("/files/second", {true, false, 1});
    filesystem.write_text("/files/first/added", "retained event");
    filesystem.write_text("/files/second/added", "exceeds watch entry limit");

    try {
        (void)filesystem.poll_changes();
        CHECK(false);
    } catch (const ge::io::IoException& error) {
        CHECK(error.code() == ge::io::IoError::limit_exceeded);
    }
    const auto watches = filesystem.watches();
    CHECK(watches.size() == 2);
    CHECK(watches[0].id == first);
    CHECK(watches[0].observed_entries == 1);
    CHECK(watches[1].id == second);
    CHECK(watches[1].observed_entries == 1);
    CHECK(filesystem.statistics().change_events == 0);

    CHECK(filesystem.remove("/files/second/added"));
    const auto recovered = filesystem.poll_changes();
    CHECK(!recovered.overflow);
    CHECK(recovered.watches_polled == 2);
    CHECK(recovered.changes.size() == 1);
    CHECK(recovered.changes[0].watch == first);
    CHECK(recovered.changes[0].kind == ge::io::ChangeKind::added);
    CHECK(recovered.changes[0].path == "/files/first/added");
    CHECK(filesystem.watches()[0].observed_entries == 2);
    CHECK(filesystem.statistics().change_events == 1);
    CHECK(filesystem.poll_changes().changes.empty());
}

void overflowing_poll_commits_only_reported_watches() {
    ge::io::VirtualFileSystem filesystem;
    (void)filesystem.mount_memory({"/files", 0, true});
    filesystem.write_text("/files/first/original", "first");
    filesystem.write_text("/files/second/original", "second");
    const auto first = filesystem.watch("/files/first");
    const auto second = filesystem.watch("/files/second");
    filesystem.write_text("/files/first/added", "first event");
    filesystem.write_text("/files/second/added", "second event");

    const auto partial = filesystem.poll_changes(1);
    CHECK(partial.overflow);
    CHECK(partial.watches_polled == 1);
    CHECK(partial.changes.size() == 1);
    CHECK(partial.changes[0].watch == first);
    const auto watches = filesystem.watches();
    CHECK(watches[0].observed_entries == 2);
    CHECK(watches[1].observed_entries == 1);

    const auto remaining = filesystem.poll_changes(1);
    CHECK(!remaining.overflow);
    CHECK(remaining.watches_polled == 2);
    CHECK(remaining.changes.size() == 1);
    CHECK(remaining.changes[0].watch == second);
    CHECK(remaining.changes[0].path == "/files/second/added");
    CHECK(filesystem.statistics().change_events == 2);
    CHECK(filesystem.poll_changes().changes.empty());
}

} // namespace

int main() {
    return ge::test::run("io_lifecycle_audit", {
        {"failed_poll_retains_every_watch_snapshot", failed_poll_retains_every_watch_snapshot},
        {"overflowing_poll_commits_only_reported_watches", overflowing_poll_commits_only_reported_watches},
    });
}
