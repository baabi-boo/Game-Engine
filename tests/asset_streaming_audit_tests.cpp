#include <ge/assets/streaming.hpp>

#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace ge::assets;

StreamLimits limits_for(std::size_t assets, std::size_t changes) {
    StreamLimits limits;
    limits.max_assets = assets;
    limits.max_jobs = 1;
    limits.dispatches_per_pump = 1;
    limits.changes_per_poll = changes;
    return limits;
}

struct Fixture {
    ge::io::VirtualFileSystem filesystem;
    ge::jobs::Executor executor{{.worker_count = 1, .capacity = 8}};
    AssetCatalog catalog{filesystem, {.max_source_bytes = 64, .max_resource_bytes = 64}};
    ResourceCache cache{{.budget_bytes = 4096, .max_resources = 32, .max_resource_bytes = 64}};
    AssetStreamer streamer;
    StreamConsumer consumer;

    explicit Fixture(std::size_t assets = 8, std::size_t changes = 512)
        : streamer(executor, filesystem, catalog, cache, limits_for(assets, changes)),
          consumer(streamer.create_consumer()) {
        (void)filesystem.mount_memory({"/assets", 0, true});
    }

    ~Fixture() {
        streamer.shutdown();
        (void)executor.shutdown();
    }

    AssetId add(std::string name, std::vector<AssetId> dependencies = {}) {
        AssetDefinition definition;
        definition.source_path = "/assets/" + name;
        definition.name = std::move(name);
        definition.dependencies = std::move(dependencies);
        filesystem.write_text(definition.source_path, definition.name);
        return catalog.add(std::move(definition));
    }

    void replace_dependency(AssetId asset, AssetId dependency) {
        auto definition = catalog.inspect(asset)->definition;
        definition.dependencies = {dependency};
        catalog.update(std::move(definition));
    }

    StreamSnapshot await_ready(StreamTicket ticket) {
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            (void)streamer.pump();
            CHECK(executor.wait_idle(2s));
            const auto snapshot = streamer.inspect(ticket);
            CHECK(snapshot);
            if (snapshot->state == StreamState::ready) {
                return *snapshot;
            }
        }
        const auto snapshot = streamer.inspect(ticket);
        CHECK_MSG(snapshot && snapshot->state == StreamState::ready,
                  snapshot ? snapshot->error : "request disappeared");
        return *snapshot;
    }

    void drain_changes() {
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            if (streamer.poll_catalog_changes() == 0) {
                return;
            }
        }
        CHECK_MSG(false, "catalog history did not drain");
    }
};

void revision_observed_before_event_refreshes_closure_and_preserves_pins() {
    Fixture fixture;
    const auto b = fixture.add("b");
    const auto c = fixture.add("c");
    const auto a = fixture.add("a", {b});
    const auto ticket = fixture.streamer.request(a, fixture.consumer);
    const auto original = fixture.await_ready(ticket);
    const auto pin = fixture.streamer.pin(ticket);
    CHECK(pin);
    const auto old_dependency = fixture.cache.find(
        b, fixture.catalog.inspect(b)->revision, binary_asset_type);
    CHECK(old_dependency);
    fixture.drain_changes();

    fixture.replace_dependency(a, c);
    CHECK(fixture.streamer.reprioritize(ticket, ge::jobs::Priority::Normal));
    const auto replacement = fixture.await_ready(ticket);
    CHECK(replacement.desired_revision == fixture.catalog.inspect(a)->revision);
    CHECK(replacement.available_revision != original.available_revision);
    CHECK(replacement.dependencies == 1 && replacement.ready_dependencies == 1);
    CHECK(fixture.catalog.inspect(c)->status == ImportStatus::ready);
    CHECK(fixture.streamer.statistics().tracked_assets == 2);
    CHECK(fixture.streamer.statistics().dependency_references == 2);
    CHECK(fixture.streamer.bytes(*pin).front() == std::byte{'a'});
    (void)fixture.streamer.trim(0);
    CHECK(fixture.cache.inspect(old_dependency.value()));
    CHECK(fixture.cache.inspect(old_dependency.value())->pins != 0);
    CHECK(fixture.streamer.release(*pin));
    (void)fixture.streamer.trim(0);
    CHECK(!fixture.cache.inspect(old_dependency.value()));
}

void revision_observed_behind_catalog_backlog_refreshes_closure() {
    Fixture fixture(8, 1);
    const auto b = fixture.add("b");
    const auto c = fixture.add("c");
    const auto a = fixture.add("a", {b});
    const auto ticket = fixture.streamer.request(a, fixture.consumer);
    const auto original = fixture.await_ready(ticket);
    fixture.drain_changes();
    for (unsigned i = 0; i < 8; ++i) {
        (void)fixture.add("unrelated" + std::to_string(i));
    }

    fixture.replace_dependency(a, c);
    const auto replacement = fixture.await_ready(ticket);
    CHECK(replacement.available_revision != original.available_revision);
    CHECK(replacement.available_revision == fixture.catalog.inspect(a)->revision);
    CHECK(replacement.ready_dependencies == 1);
    CHECK(fixture.catalog.inspect(c)->status == ImportStatus::ready);
    CHECK(fixture.streamer.statistics().tracked_assets == 2);
    CHECK(fixture.streamer.statistics().dependency_references == 2);
}

void shared_request_closures_replace_obsolete_nodes_at_capacity() {
    Fixture fixture(2);
    const auto b = fixture.add("b");
    const auto c = fixture.add("c");
    const auto a = fixture.add("a", {b});
    const auto first = fixture.streamer.request(a, fixture.consumer);
    const auto second_consumer = fixture.streamer.create_consumer();
    const auto second = fixture.streamer.request(a, second_consumer);
    CHECK(fixture.streamer.statistics().jobs == 0);
    fixture.replace_dependency(a, c);

    CHECK(fixture.streamer.retry(first));
    CHECK(fixture.streamer.statistics().tracked_assets == 2);
    CHECK(fixture.streamer.statistics().dependency_references == 4);
    const auto replacement = fixture.await_ready(first);
    CHECK(replacement.available_revision == fixture.catalog.inspect(a)->revision);
    CHECK(fixture.streamer.inspect(second)->state == StreamState::ready);
    CHECK(fixture.catalog.inspect(c)->status == ImportStatus::ready);
    CHECK(fixture.catalog.inspect(b)->status == ImportStatus::registered);
    CHECK(fixture.streamer.statistics().tracked_assets == 2);
}

void shared_dependency_and_pending_job_keep_their_capacity_until_retirement() {
    Fixture fixture(2);
    const auto b = fixture.add("b");
    const auto c = fixture.add("c");
    const auto a = fixture.add("a", {b});
    const auto ticket = fixture.streamer.request(a, fixture.consumer);
    const auto other_consumer = fixture.streamer.create_consumer();
    const auto other = fixture.streamer.request(b, other_consumer);
    CHECK(fixture.executor.pause());
    CHECK(fixture.streamer.pump().submitted == 1);
    const auto charged = fixture.streamer.statistics();
    const auto pending_bytes = fixture.cache.statistics().pending_bytes;
    CHECK(charged.jobs == 1 && charged.inflight_reserved_bytes != 0);
    CHECK(pending_bytes != 0);

    fixture.replace_dependency(a, c);
    CHECK(!fixture.streamer.retry(ticket));
    CHECK(fixture.streamer.inspect(other)->state == StreamState::loading);
    CHECK(fixture.streamer.statistics().cancelled == 0);
    CHECK(fixture.streamer.statistics().tracked_assets == 2);
    CHECK(fixture.streamer.statistics().inflight_reserved_bytes == charged.inflight_reserved_bytes);
    CHECK(fixture.cache.statistics().pending_bytes == pending_bytes);

    CHECK(fixture.streamer.release(other));
    CHECK(fixture.streamer.statistics().cancelled == 1);
    CHECK(fixture.streamer.statistics().jobs == 1);
    CHECK(fixture.streamer.statistics().tracked_assets == 2);
    CHECK(fixture.streamer.statistics().inflight_reserved_bytes == charged.inflight_reserved_bytes);
    CHECK(fixture.cache.statistics().pending_bytes == pending_bytes);
    CHECK(fixture.executor.resume());
    const auto replacement = fixture.await_ready(ticket);
    CHECK(replacement.available_revision == fixture.catalog.inspect(a)->revision);
    CHECK(fixture.catalog.inspect(c)->status == ImportStatus::ready);
    CHECK(!fixture.catalog.inspect(b)->last_good_revision);
    CHECK(fixture.streamer.statistics().tracked_assets == 2);
    CHECK(fixture.streamer.statistics().dependency_references == 2);
    CHECK(fixture.streamer.statistics().jobs == 0);
    CHECK(fixture.streamer.statistics().inflight_reserved_bytes == 0);
    CHECK(fixture.cache.statistics().pending_bytes == 0);
}

} // namespace

int main() {
    return ge::test::run("asset_streaming_audit", {
        {"revision_observed_before_event_refreshes_closure_and_preserves_pins",
         revision_observed_before_event_refreshes_closure_and_preserves_pins},
        {"revision_observed_behind_catalog_backlog_refreshes_closure",
         revision_observed_behind_catalog_backlog_refreshes_closure},
        {"shared_request_closures_replace_obsolete_nodes_at_capacity",
         shared_request_closures_replace_obsolete_nodes_at_capacity},
        {"shared_dependency_and_pending_job_keep_their_capacity_until_retirement",
         shared_dependency_and_pending_job_keep_their_capacity_until_retirement},
    });
}
