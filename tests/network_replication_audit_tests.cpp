#include "test_support.hpp"

#include <ge/network/replication.hpp>
#include <ge/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

void check_received_history(std::size_t history_frames, std::size_t history_states) {
    ge::network::ReplicationConfig configuration;
    configuration.local_identity = {2};
    configuration.max_entities = 1;
    configuration.history_frames = history_frames;
    configuration.max_history_states = history_states;
    ge::ecs::Registry registry;
    ge::scene::Scene scene(registry);
    ge::network::ReplicationService replication(configuration);
    CHECK(replication.attach(scene));

    ge::network::NetworkTransport transport;
    ge::network::SocketOptions options;
    const auto endpoint = ge::network::parse_endpoint("127.0.0.1", 0);
    CHECK(endpoint);
    options.local = *endpoint;
    const auto server_socket = transport.open(options);
    const auto client_socket = transport.open(options);
    CHECK(server_socket);
    CHECK(client_socket);
    const auto server = transport.connect(server_socket.socket, client_socket.local, {77});
    const auto client = transport.connect(client_socket.socket, server_socket.local, {77});
    CHECK(server);
    CHECK(client);
    CHECK(replication.add_peer(transport, client.peer, {{1}, false, true, true}));

    double now{};
    const auto deliver = [&](std::uint64_t sequence) {
        ge::network::EntitySnapshot entity;
        entity.id = {{100}, 1};
        entity.world.position.x = static_cast<double>(sequence);
        const ge::network::SnapshotFrame frame{sequence, sequence,
            static_cast<double>(sequence), {entity}};
        ge::network::ReplicationPacket packet;
        CHECK(ge::network::make_replication_delta(frame, nullptr, packet));
        std::vector<std::byte> wire;
        CHECK(ge::network::encode_replication(packet, wire));
        CHECK(transport.send(server.peer, configuration.channel,
            ge::network::MessageKind::Replication, wire));
        for (int attempt = 0; attempt < 8; ++attempt) {
            now += 0.001;
            CHECK(transport.pump(now));
            CHECK(replication.receive(transport, now));
            const auto* received = replication.latest_received(client.peer);
            if (received && received->sequence == sequence) return;
        }
        CHECK_MSG(false, "loopback snapshot was not received");
    };

    // Sequence gaps are valid: an authority need not send every capture.
    deliver(10);
    deliver(20);
    deliver(30);
    const auto consumed = replication.received_since(client.peer, 10);
    CHECK(consumed.latest_sequence == 30);
    CHECK(consumed.frames.size() == 2);
    CHECK(consumed.frames[0].sequence == 20);
    CHECK(consumed.frames[1].sequence == 30);
    CHECK(!consumed.history_lost);
    CHECK(!replication.received_since(client.peer, 15).history_lost);
    CHECK(replication.received_since(client.peer, 9).history_lost);
    CHECK(!replication.received_since(client.peer, 0).history_lost);

    deliver(40);
    CHECK(replication.received_since(client.peer, 10).history_lost);
    CHECK(!replication.received_since(client.peer, 20).history_lost);
}

void consumed_frame_eviction_preserves_history() {
    check_received_history(2, 64);
}

void history_budget_eviction_tracks_actual_losses() {
    check_received_history(16, 2);
}

} // namespace

int main() {
    return ge::test::run("network_replication_audit", {
        {"consumed_frame_eviction_preserves_history", consumed_frame_eviction_preserves_history},
        {"history_budget_eviction_tracks_actual_losses", history_budget_eviction_tracks_actual_losses},
    });
}
