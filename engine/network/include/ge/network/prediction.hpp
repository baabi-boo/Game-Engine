#pragma once

#include <ge/network/replication.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::input { class InputSystem; }
namespace ge::physics { class CharacterSystem; class VehicleSystem; }

namespace ge::network {

// Application/channel 3 is reserved for GEPRED01 input batches by default.
// Input sequences start at one, never wrap, and are independent of all ticks,
// packet ACKs, transport message sequences and snapshot sequence numbers.
inline constexpr std::uint16_t prediction_channel = 3;
inline constexpr std::uint16_t prediction_wire_version = 1;
enum class PredictionButton : std::uint16_t {
    Sprint = 1, Crouch = 2, JumpPressed = 4, JumpHeld = 8, Brake = 16
};
struct PredictionControl {
    math::Vec2 movement{};
    double turn{};
    double lift{};
    std::uint16_t buttons{};
    bool operator==(const PredictionControl&) const = default;
};
struct PredictionInput {
    std::uint64_t sequence{};
    std::uint64_t client_tick{};
    PredictionControl control{};
    bool operator==(const PredictionInput&) const = default;
};
enum class PredictionRole : std::uint8_t { Client, Server };
enum class PredictionStatus : std::uint8_t { Active, AwaitingAuthority, Suspended };
enum class PredictionError : std::uint8_t {
    None, InvalidConfiguration, NotAttached, InvalidIdentity, InvalidInput,
    InvalidTime, Capacity, UnknownBinding, AlreadyBound, RetiredIdentity,
    Unauthorized, AuthorityConflict, InvalidBody, UnknownPeer, WrongRole,
    Malformed, UnsupportedVersion, ModelMismatch, SequenceExhausted,
    SequenceWindow, DuplicateConflict, StaleAuthority, FutureAcknowledgement,
    MissingAcknowledgement, HistoryMissing, ReplayBudget, ModelFailed,
    PublishFailed, TransportFailure, Busy, OutOfMemory
};
[[nodiscard]] std::string_view prediction_error_name(PredictionError) noexcept;
struct PredictionResult {
    PredictionError error{PredictionError::None};
    std::size_t affected{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == PredictionError::None;
    }
};
struct PredictionConfig {
    std::uint16_t channel{prediction_channel};
    std::size_t max_bindings{128};
    std::size_t max_retired_identities{4096};
    std::size_t history_inputs{256};
    std::size_t server_queue_inputs{256};
    std::size_t batch_inputs{32};
    std::size_t replay_steps{256};
    std::size_t steps_per_frame{8};
    std::size_t messages_per_frame{32};
    std::size_t receive_scan_limit{1024};
    std::size_t snapshot_frames_per_poll{16};
    std::size_t max_message_bytes{8192};
    double fixed_seconds{1.0 / 60.0};
    double maximum_frame_seconds{0.25};
    double retry_seconds{0.25};
    double smoothing_half_life{0.08};
    double smoothing_distance_limit{4.0};
    double position_tolerance{0.001};
    double velocity_tolerance{0.001};
    double rotation_tolerance{0.001};
    double look_sensitivity{0.002};
};
struct KinematicPrediction {
    double speed{6.0};
    double acceleration{24.0};
    double braking{32.0};
    double turn_speed{3.0};
    double lift_speed{4.0};
    double sprint_multiplier{1.5};
    double crouch_multiplier{0.5};
};
[[nodiscard]] PredictionError validate_prediction_config(const PredictionConfig&) noexcept;
[[nodiscard]] bool valid_prediction_control(const PredictionControl&) noexcept;
[[nodiscard]] bool valid_kinematic_prediction(const KinematicPrediction&) noexcept;
// A collision-free free-flight model: +Y input means local -Z movement.
// Lift is world +Y. Jump flags are available to custom models and have no
// effect here. There is no gravity, ground state, sweep, or contact rollback.
[[nodiscard]] PredictionResult predict_kinematic(const EntitySnapshot& before,
    const PredictionInput& input, double fixed_seconds, const KinematicPrediction& model,
    EntitySnapshot& after) noexcept;
[[nodiscard]] std::uint64_t prediction_model_signature(
    const KinematicPrediction&, double fixed_seconds);

struct PredictionBatch {
    ge::PersistentId world{};
    ge::PersistentId sender{};
    NetworkEntityId entity{};
    std::uint64_t model_signature{};
    double fixed_seconds{};
    std::vector<PredictionInput> inputs;
};
[[nodiscard]] PredictionResult encode_prediction(const PredictionBatch&,
    std::vector<std::byte>& output, const PredictionConfig& limits = {});
[[nodiscard]] PredictionResult decode_prediction(std::span<const std::byte>,
    PredictionBatch& output, const PredictionConfig& limits = {});

// The callback is a pure, bounded state transition over the supplied snapshot.
// It must preserve identity, ownership, enablement, scale-independent metadata
// and acknowledged_input; all replay-relevant state must live in the snapshot.
// Do not mutate Scene, controllers, physics, or this service from the callback.
// Its agreed nonzero signature identifies the algorithm AND its tuning.
struct PredictionAdapter {
    std::uint64_t signature{};
    std::function<PredictionResult(const EntitySnapshot&, const PredictionInput&,
        double, EntitySnapshot&)> step;
};
struct PredictionBindingOptions {
    PredictionRole role{PredictionRole::Client};
    PeerHandle peer{};
    KinematicPrediction kinematic{};
    PredictionAdapter adapter{};
};
struct PredictionState {
    NetworkEntityId id{};
    ecs::Entity entity{};
    PeerHandle peer{};
    PredictionRole role{PredictionRole::Client};
    PredictionStatus status{PredictionStatus::Active};
    PredictionError last_error{PredictionError::None};
    EntitySnapshot predicted{};
    std::optional<std::uint64_t> acknowledged_input;
    std::uint64_t last_generated{};
    std::uint64_t last_applied{};
    std::uint64_t last_snapshot{};
    std::size_t pending_inputs{};
    std::size_t unsent_inputs{};
    std::size_t queued_inputs{};
    double position_error{};
    double velocity_error{};
    double rotation_error{};
    math::Vec3 visual_position_offset{};
    math::Quat visual_rotation_offset{};
};
struct PredictionHistoryEntry {
    PredictionInput input{};
    EntitySnapshot after{};
    bool sent{};
};
struct PredictionStatistics {
    std::uint64_t generated_inputs{};
    std::uint64_t applied_inputs{};
    std::uint64_t acknowledged_inputs{};
    std::uint64_t reconciliations{};
    std::uint64_t corrections{};
    std::uint64_t replayed_inputs{};
    std::uint64_t history_stalls{};
    std::uint64_t history_losses{};
    std::uint64_t resync_requests{};
    std::uint64_t rejected_messages{};
    std::uint64_t duplicate_inputs{};
    std::uint64_t sent_batches{};
    std::uint64_t send_failures{};
    std::uint64_t model_failures{};
    std::uint64_t publication_failures{};
    std::size_t bindings{};
    std::size_t retained_inputs{};
    std::size_t queued_inputs{};
    std::size_t awaiting_authority{};
    double dropped_seconds{};
    double maximum_position_error{};
};

// Single owner thread. Registry and replication retain entity identity ownership.
// Bind only existing, server-authoritative replication bindings with an existing
// kinematic body whose centre of mass is zero. Character/vehicle components are
// rejected, including Manual authority: their internal state is not replayable.
// No binding creates entities, opens sockets, grants peer trust, or advances the
// PhysicsWorld. Model poses go through PhysicsWorld; presentation smoothing is
// query-only and never modifies the authoritative collision pose.
//
// advance publishes fixed-step poses and a stationary kinematic target so normal
// dynamics cannot advance them twice. finish republishes model velocity and only
// then emits server application ACKs. Schedule advance before dynamics, finish
// after dynamics and before replication capture. Other world bodies/contacts are
// not rewound, and pure-model replay does not promise deterministic physics.
//
// Unacknowledged histories are never evicted. Capacity causes backpressure.
// Batches retry until application ACK, including inputs already transport-sent;
// transport reliability alone does not prove admission to the server input queue.
// Missing ACK/history pauses generation, preserves pending inputs for transport,
// requests a full replication baseline and retries newer authoritative frames.
// An unbound identity is retired for this service lifetime; use a new incarnation
// to start another stream. Call shutdown while borrowed services remain alive.
class PredictionService final {
public:
    explicit PredictionService(PredictionConfig configuration = {});
    ~PredictionService();
    PredictionService(const PredictionService&) = delete;
    PredictionService& operator=(const PredictionService&) = delete;
    [[nodiscard]] const PredictionConfig& config() const noexcept;
    [[nodiscard]] PredictionResult attach(ReplicationService&, physics::PhysicsWorld&,
        physics::CharacterSystem* characters = nullptr, physics::VehicleSystem* vehicles = nullptr);
    [[nodiscard]] PredictionResult bind(NetworkEntityId, PredictionBindingOptions = {});
    [[nodiscard]] PredictionResult unbind(NetworkEntityId);
    [[nodiscard]] PredictionResult shutdown();
    [[nodiscard]] PredictionResult select(NetworkEntityId);
    void release_control() noexcept;
    [[nodiscard]] std::optional<NetworkEntityId> controlled() const noexcept;
    [[nodiscard]] PredictionResult set_control(NetworkEntityId, PredictionControl);
    [[nodiscard]] PredictionResult sample_input(const input::InputSystem&);
    [[nodiscard]] PredictionResult enqueue(PeerHandle, const PredictionBatch&);
    [[nodiscard]] PredictionResult receive(NetworkTransport&);
    [[nodiscard]] PredictionResult send(NetworkTransport&);
    [[nodiscard]] PredictionResult reconcile(NetworkEntityId, const SnapshotFrame&);
    [[nodiscard]] PredictionResult poll_authority();
    [[nodiscard]] PredictionResult request_resync(NetworkEntityId);
    [[nodiscard]] PredictionResult advance(double delta_seconds, std::uint64_t tick);
    [[nodiscard]] PredictionResult finish();
    [[nodiscard]] std::optional<PredictionState> state(NetworkEntityId) const;
    [[nodiscard]] std::vector<PredictionState> states() const;
    [[nodiscard]] std::vector<PredictionHistoryEntry> history(NetworkEntityId,
        std::size_t limit = 256) const;
    [[nodiscard]] std::optional<math::Transform> presentation(NetworkEntityId) const;
    [[nodiscard]] PredictionStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_prediction(core::Kernel& kernel);

} // namespace ge::network
