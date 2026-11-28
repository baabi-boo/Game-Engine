#include "test_support.hpp"

#include <ge/network/prediction.hpp>
#include <ge/physics/collision.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <cmath>
#include <vector>

namespace {

void check_authority_handoff(ge::network::ReplicationAuthority authority, ge::PersistentId owner) {
    using namespace ge;
    using namespace ge::network;
    const NetworkEntityId id{PersistentId{42}, 1};
    ReplicationConfig configuration;
    configuration.local_identity = PersistentId{2};
    ecs::Registry registry;
    scene::Scene scene(registry);
    physics::CollisionWorld collision(registry);
    physics::PhysicsWorld physics(scene, collision);
    NetworkTransport transport;
    ReplicationService replication(configuration);
    PredictionService prediction;
    CHECK(replication.attach(scene, &physics));

    SocketOptions options;
    const auto endpoint = parse_endpoint("127.0.0.1", 0);
    CHECK(endpoint);
    options.local = *endpoint;
    const auto sender_socket = transport.open(options);
    const auto receiver_socket = transport.open(options);
    CHECK(sender_socket);
    CHECK(receiver_socket);
    const auto sender = transport.connect(sender_socket.socket, receiver_socket.local, PersistentId{77});
    const auto receiver = transport.connect(receiver_socket.socket, sender_socket.local, PersistentId{77});
    CHECK(sender);
    CHECK(receiver);
    CHECK(replication.add_peer(transport, receiver.peer, {PersistentId{1}, true, true, false}));

    const auto entity = scene.create("AuthorityHandoff");
    CHECK(entity);
    physics::BodyDescription description;
    description.mode = physics::BodyMode::Kinematic;
    const auto body = physics.add(entity, description);
    CHECK(body);
    ReplicationBindingOptions binding;
    binding.owner = configuration.local_identity;
    CHECK(replication.bind(entity, id, binding));
    CHECK(prediction.attach(replication, physics));
    PredictionBindingOptions predicted;
    predicted.peer = receiver.peer;
    CHECK(prediction.bind(id, predicted));

    double now{};
    const auto deliver = [&](const EntitySnapshot& state, std::uint64_t sequence) {
        const SnapshotFrame frame{sequence, sequence, static_cast<double>(sequence), {state}};
        ReplicationPacket packet;
        CHECK(make_replication_delta(frame, nullptr, packet));
        std::vector<std::byte> wire;
        CHECK(encode_replication(packet, wire));
        CHECK(transport.send(sender.peer, configuration.channel, MessageKind::Replication, wire));
        for (int attempt = 0; attempt < 8; ++attempt) {
            now += 0.001;
            CHECK(transport.pump(now));
            CHECK(replication.receive(transport, now));
            const auto* received = replication.latest_received(receiver.peer);
            if (received && received->sequence == sequence) return;
        }
        CHECK_MSG(false, "loopback authority snapshot was not received");
    };

    EntitySnapshot baseline;
    baseline.id = id;
    baseline.owner = configuration.local_identity;
    baseline.has_velocity = true;
    baseline.world.position.x = 1.0;
    deliver(baseline, 1);
    CHECK(prediction.poll_authority());
    CHECK(std::abs(physics.get(body)->pose.position.x - 1.0) < 1.0e-10);

    // The accepted transfer snapshot must become the body's starting state
    // even though either kind of authority handoff suspends prediction.
    auto handoff = baseline;
    handoff.authority = authority;
    handoff.owner = owner;
    handoff.world.position.x = 40.0;
    handoff.linear_velocity.x = 2.0;
    deliver(handoff, 2);
    const auto reconciled = prediction.poll_authority();
    CHECK(reconciled.error == PredictionError::Unauthorized);
    CHECK(prediction.state(id)->status == PredictionStatus::Suspended);
    CHECK(replication.binding(id)->options.authority == authority);
    CHECK(replication.binding(id)->options.owner == owner);
    CHECK(!replication.binding(id)->options.prediction_owned_pose);
    CHECK(std::abs(physics.get(body)->pose.position.x - handoff.world.position.x) < 1.0e-10);
    CHECK(std::abs(physics.get(body)->linear_velocity.x - handoff.linear_velocity.x) < 1.0e-10);
    const auto world = scene.world_transform(entity);
    CHECK(world);
    CHECK(std::abs(world->position.x - handoff.world.position.x) < 1.0e-10);

    CHECK(prediction.advance(0.0, 1));
    CHECK(prediction.finish());
    CHECK(replication.capture(2, 2.0));
    const auto* captured = replication.latest_capture();
    CHECK(captured);
    if (owner == configuration.local_identity) {
        CHECK(captured->entities.size() == 1);
        CHECK(std::abs(captured->entities.front().world.position.x - handoff.world.position.x) < 1.0e-10);
    } else {
        CHECK(captured->entities.empty());
    }
    CHECK(prediction.shutdown());
    CHECK(replication.remove_peer(receiver.peer));
    transport.close_all();
    CHECK(transport.statistics().open_sockets == 0);
    CHECK(transport.statistics().connected_peers == 0);
}

void authority_transfer_publishes_the_handoff_pose() {
    check_authority_handoff(ge::network::ReplicationAuthority::Owner, ge::PersistentId{2});
}

void owner_transfer_publishes_the_handoff_pose() {
    check_authority_handoff(ge::network::ReplicationAuthority::Server, ge::PersistentId{3});
}

} // namespace

int main() {
    return ge::test::run("network_integration_audit", {
        {"authority_transfer_handoff_pose", authority_transfer_publishes_the_handoff_pose},
        {"owner_transfer_handoff_pose", owner_transfer_publishes_the_handoff_pose},
    });
}
