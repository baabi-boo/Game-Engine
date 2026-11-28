#include "test_support.hpp"

#include <ge/network/prediction.hpp>
#include <ge/physics/collision.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/scene.hpp>

#include <cmath>

namespace {

void server_models_use_current_authoritative_layers() {
    using namespace ge;
    using namespace ge::network;
    constexpr double step_seconds = 1.0 / 60.0;
    constexpr std::uint64_t signature = 17;
    const NetworkEntityId id{PersistentId{42}, 1};
    ecs::Registry registry;
    scene::Scene scene(registry);
    physics::CollisionWorld collision(registry);
    physics::PhysicsWorld physics(scene, collision);
    NetworkTransport transport;
    ReplicationService replication;
    PredictionService prediction;

    // A local route supplies a trusted peer handle; this test sends no packets.
    SocketOptions socket_options;
    const auto endpoint = parse_endpoint("127.0.0.1", 0);
    CHECK(endpoint);
    socket_options.local = *endpoint;
    const auto socket = transport.open(socket_options);
    CHECK(socket);
    const auto connection = transport.connect(socket.socket, socket.local, PersistentId{7});
    CHECK(connection);
    ReplicationPeerPolicy policy;
    policy.identity = PersistentId{2};
    CHECK(replication.add_peer(transport, connection.peer, policy));
    CHECK(replication.attach(scene, &physics));

    const auto entity = scene.create("PredictedBody");
    CHECK(entity);
    physics::BodyDescription description;
    description.mode = physics::BodyMode::Kinematic;
    const auto body = physics.add(entity, description);
    CHECK(body);
    ReplicationBindingOptions replication_options;
    replication_options.owner = policy.identity;
    CHECK(replication.bind(entity, id, replication_options));
    CHECK(prediction.attach(replication, physics));
    PredictionBindingOptions prediction_options;
    prediction_options.role = PredictionRole::Server;
    prediction_options.peer = connection.peer;
    prediction_options.adapter.signature = signature;
    prediction_options.adapter.step = [](const EntitySnapshot& before,
        const PredictionInput& input, double seconds, EntitySnapshot& after) {
        after = before;
        after.has_velocity = true;
        after.linear_velocity = {static_cast<double>(before.layers), 0.0, 0.0};
        after.world.position += after.linear_velocity * seconds;
        return PredictionResult{PredictionError::None, 1, input.sequence};
    };
    CHECK(prediction.bind(id, prediction_options));

    PredictionBatch batch;
    batch.world = replication.config().world;
    batch.sender = policy.identity;
    batch.entity = id;
    batch.model_signature = signature;
    batch.fixed_seconds = step_seconds;
    batch.inputs = {{1, 1, {}}, {2, 2, {}}};
    CHECK(prediction.enqueue(connection.peer, batch));

    double expected_position{};
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        const auto layers = sequence * 4;
        CHECK(registry.set_layers(entity, layers));
        CHECK(prediction.advance(step_seconds, sequence));
        const auto state = prediction.state(id);
        CHECK(state);
        CHECK(state->last_applied == sequence);
        CHECK(state->predicted.layers == layers);
        expected_position += static_cast<double>(layers) * step_seconds;
        CHECK(std::abs(state->predicted.world.position.x - expected_position) < 1.0e-10);
        CHECK(replication.binding(id)->acknowledged_input.value_or(0) == sequence - 1);

        const auto dynamics = physics.step(step_seconds);
        CHECK(!dynamics.busy && !dynamics.invalid_delta);
        CHECK(std::abs(physics.get(body)->pose.position.x - expected_position) < 1.0e-10);
        CHECK(prediction.finish());
        CHECK(replication.binding(id)->acknowledged_input == sequence);
        CHECK(physics.get(body)->linear_velocity.x == static_cast<double>(layers));
        const auto pose = scene.world_transform(entity);
        CHECK(pose);
        CHECK(std::abs(pose->position.x - expected_position) < 1.0e-10);
        CHECK(replication.capture(sequence, static_cast<double>(sequence) * step_seconds));
        const auto* captured = replication.latest_capture();
        CHECK(captured && captured->entities.size() == 1);
        CHECK(captured->entities.front().layers == state->predicted.layers);
        CHECK(captured->entities.front().acknowledged_input == sequence);
    }
    CHECK(prediction.shutdown());
}

} // namespace

int main() {
    return ge::test::run("network_prediction_audit", {
        {"current_authoritative_layers", server_models_use_current_authoritative_layers},
    });
}
