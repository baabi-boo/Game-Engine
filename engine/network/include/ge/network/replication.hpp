#pragma once

#include <ge/core/identity.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>
#include <ge/network/transport.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; class WorldPartition; }
namespace ge::physics { class PhysicsWorld; }

namespace ge::network {

// Incarnation is assigned by the world/session authority, never copied from a
// Registry generation. Persistent identity remains the Registry's identity.
struct NetworkEntityId {
    ge::PersistentId persistent{};
    std::uint64_t incarnation{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return persistent.valid() && incarnation != 0;
    }
    auto operator<=>(const NetworkEntityId&) const = default;
};
enum class ReplicationAuthority : std::uint8_t { Server, Owner };
enum class SnapshotField : std::uint16_t {
    Position = 1U << 0U,
    Rotation = 1U << 1U,
    Scale = 1U << 2U,
    Velocity = 1U << 3U,
    Enabled = 1U << 4U,
    Layers = 1U << 5U,
    Owner = 1U << 6U,
    Prefab = 1U << 7U,
    Authority = 1U << 8U,
    InputAcknowledgement = 1U << 9U
};
using SnapshotMask = std::uint16_t;
inline constexpr SnapshotMask all_snapshot_fields = 0x03ffU;
[[nodiscard]] constexpr SnapshotMask field_mask(SnapshotField field) noexcept {
    return static_cast<SnapshotMask>(field);
}
struct EntitySnapshot {
    NetworkEntityId id{};
    ge::PersistentId owner{};
    // Optional portable content identity. No local resource handle is encoded.
    ge::PersistentId prefab{};
    ReplicationAuthority authority{ReplicationAuthority::Server};
    // Absolute world position = Scene position + WorldPartition origin.
    math::Transform world{};
    math::Vec3 linear_velocity{};
    math::Vec3 angular_velocity{};
    std::uint64_t layers{1};
    bool has_velocity{};
    bool enabled{true};
    bool physics_enabled{true};
    // Explicit client input sequence processed by the authority. Snapshot ticks
    // are never inferred to acknowledge inputs. Missing means no input ACK.
    std::optional<std::uint64_t> acknowledged_input;
};
struct SnapshotFrame {
    std::uint64_t sequence{};
    std::uint64_t tick{};
    double server_seconds{};
    std::vector<EntitySnapshot> entities;
};
enum class ReplicationMessage : std::uint8_t { Full = 1, Delta = 2, Ack = 3, Resync = 4 };
enum class EntityChange : std::uint8_t { Spawn = 1, Update = 2, Despawn = 3 };
struct EntityDelta {
    EntityChange change{EntityChange::Update};
    NetworkEntityId id{};
    SnapshotMask fields{};
    EntitySnapshot value{};
};
struct ReplicationPacket {
    ReplicationMessage kind{ReplicationMessage::Full};
    ge::PersistentId world{};
    ge::PersistentId sender{};
    std::uint64_t sequence{};
    std::uint64_t baseline{};
    std::uint64_t baseline_hash{};
    std::uint64_t state_hash{};
    std::uint64_t tick{};
    double server_seconds{};
    std::vector<EntityDelta> changes;
};
enum class ReplicationError : std::uint8_t {
    None, InvalidConfiguration, NotAttached, InvalidEntity, InvalidIdentity,
    IdentityConflict, StaleGeneration, Capacity, InvalidTime, InvalidSnapshot,
    Malformed, UnsupportedVersion, BaselineMissing, BaselineMismatch,
    Unauthorized, UnknownPeer, TransportFailure, FactoryRequired,
    ApplyFailed, Busy, SequenceExhausted, OutOfMemory
};
[[nodiscard]] std::string_view replication_error_name(ReplicationError error) noexcept;
struct ReplicationResult {
    ReplicationError error{ReplicationError::None};
    std::size_t affected{};
    std::uint64_t sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ReplicationError::None;
    }
};
struct ReplicationConfig {
    ge::PersistentId world{1};
    ge::PersistentId local_identity{1};
    ge::PersistentId server_identity{1};
    std::uint16_t channel{2};
    std::size_t max_entities{1024};
    std::size_t max_identities{8192};
    std::size_t max_peers{32};
    std::size_t max_message_bytes{256U * 1024U};
    std::size_t history_frames{16};
    // Counts retained entity states across both directions and every peer.
    // Latest frames are preserved under pressure; insufficient room rejects.
    std::size_t max_history_states{65536};
    std::size_t interpolation_samples{16};
    std::size_t receive_messages_per_frame{32};
    double max_extrapolation_seconds{0.1};
    double resync_interval_seconds{0.25};
    std::uint64_t send_every_ticks{1};
    bool automatic_send{true};
};
[[nodiscard]] ReplicationError validate_replication_config(const ReplicationConfig&) noexcept;
[[nodiscard]] bool valid_snapshot(const EntitySnapshot&) noexcept;
[[nodiscard]] SnapshotMask changed_fields(const EntitySnapshot&, const EntitySnapshot&) noexcept;
[[nodiscard]] ReplicationResult encode_replication(const ReplicationPacket& packet,
    std::vector<std::byte>& output, const ReplicationConfig& limits = {});
[[nodiscard]] ReplicationResult decode_replication(std::span<const std::byte> bytes,
    ReplicationPacket& output, const ReplicationConfig& limits = {});
[[nodiscard]] ReplicationResult make_replication_delta(const SnapshotFrame& current,
    const SnapshotFrame* baseline, ReplicationPacket& output,
    const ReplicationConfig& configuration = {});
[[nodiscard]] ReplicationResult reconstruct_snapshot(const ReplicationPacket& packet,
    const SnapshotFrame* baseline, SnapshotFrame& output,
    const ReplicationConfig& limits = {});
[[nodiscard]] std::uint64_t snapshot_hash(const SnapshotFrame& frame);

struct ReplicationBindingOptions {
    ge::PersistentId owner{};
    ge::PersistentId prefab{};
    ReplicationAuthority authority{ReplicationAuthority::Server};
    bool destroy_on_despawn{};
    // Prediction consumes received_since and owns pose/velocity reconciliation
    // for locally owned, server-authoritative snapshots. A change of owner or
    // authority publishes the handoff pose and releases prediction ownership.
    // Enablement, ownership and lifecycle still follow the authoritative stream.
    bool prediction_owned_pose{};
};
struct ReplicationBinding {
    NetworkEntityId id{};
    ecs::Entity entity{};
    ReplicationBindingOptions options{};
    PeerHandle source_peer{};
    std::optional<std::uint64_t> acknowledged_input;
};
struct ReplicationPeerPolicy {
    ge::PersistentId identity{};
    bool send_snapshots{true};
    bool accept_snapshots{};
    bool allow_spawns{};
};
struct ReplicationPeerState {
    PeerHandle peer{};
    ReplicationPeerPolicy policy{};
    std::uint64_t last_sent{};
    std::uint64_t acknowledged{};
    std::uint64_t last_received{};
    bool full_required{true};
    bool resync_pending{};
    ReplicationError last_error{ReplicationError::None};
};
struct SnapshotHistory {
    std::vector<SnapshotFrame> frames;
    bool history_lost{};
    std::uint64_t latest_sequence{};
};
struct InterpolationSample {
    EntitySnapshot state{};
    std::uint64_t lower_tick{};
    std::uint64_t upper_tick{};
    double fraction{};
    bool extrapolated{};
    bool clamped{};
};
struct ReplicationStatistics {
    std::uint64_t captures{};
    std::uint64_t full_sent{};
    std::uint64_t deltas_sent{};
    std::uint64_t frames_received{};
    std::uint64_t spawned{};
    std::uint64_t updated{};
    std::uint64_t despawned{};
    std::uint64_t rejected_messages{};
    std::uint64_t stale_messages{};
    std::uint64_t resync_requests{};
    std::uint64_t send_failures{};
    std::size_t bindings{};
    std::size_t retained_identities{};
    std::size_t peers{};
    std::size_t history_states{};
};
// Hooks must not reenter this service. The spawn hook returns a newly owned
// Scene root carrying the requested persistent ID. Prefab IDs require a hook.
// Despawn must retire owned physics/resources before destroying the entity.
// Adapter failure may leave partial world changes; the frame is not acknowledged
// and a full resync is requested. Destructors never call hooks or dependencies.
struct ReplicationFactory {
    std::function<ecs::Entity(scene::Scene&, physics::PhysicsWorld*, const EntitySnapshot&)> spawn;
    std::function<bool(scene::Scene&, physics::PhysicsWorld*, ecs::Entity)> despawn;
};

// One owner thread. Explicitly trusted peer identities grant gameplay authority;
// transport endpoint routing is not authentication. Both peers configure the
// same world and server identities. Only root entities with exact TRS are bound.
// Owner authority can update existing state, but only the server can spawn,
// despawn, transfer ownership or publish input acknowledgements. Resource type
// changes require a new incarnation. Client captures omit input ACK metadata.
// Tombstone floors remain charged until this service is destroyed. Rebinding a
// retired identity requires a strictly larger authority-issued incarnation.
// All peers belong to one borrowed NetworkTransport for this service lifetime.
// Local bindings are borrowed unless destroy_on_despawn is explicitly enabled;
// authoritative despawn disables borrowed entities and releases their binding.
class ReplicationService final {
public:
    explicit ReplicationService(ReplicationConfig configuration = {});
    ~ReplicationService();
    ReplicationService(const ReplicationService&) = delete;
    ReplicationService& operator=(const ReplicationService&) = delete;
    [[nodiscard]] const ReplicationConfig& config() const noexcept;
    [[nodiscard]] ReplicationResult attach(scene::Scene& scene,
        physics::PhysicsWorld* physics = nullptr, scene::WorldPartition* partition = nullptr);
    [[nodiscard]] ReplicationResult set_factory(ReplicationFactory factory);
    [[nodiscard]] ReplicationResult bind(ecs::Entity entity, NetworkEntityId id,
        ReplicationBindingOptions options = {});
    [[nodiscard]] ReplicationResult unbind(NetworkEntityId id);
    [[nodiscard]] ReplicationResult set_owner(NetworkEntityId id, ge::PersistentId owner,
        ReplicationAuthority authority);
    [[nodiscard]] ReplicationResult set_prediction_owned(NetworkEntityId id, bool enabled);
    // Call only after actually applying that input. Owner transfer resets the
    // acknowledgement; nullopt explicitly clears it for a new input baseline.
    [[nodiscard]] ReplicationResult acknowledge_input(NetworkEntityId id,
        std::optional<std::uint64_t> input_sequence);
    [[nodiscard]] std::optional<ReplicationBinding> binding(NetworkEntityId id) const;
    [[nodiscard]] std::optional<ReplicationBinding> binding(ecs::Entity entity) const;
    [[nodiscard]] ecs::Entity find(NetworkEntityId id) const noexcept;
    [[nodiscard]] std::vector<ReplicationBinding> bindings() const;
    [[nodiscard]] ReplicationResult add_peer(NetworkTransport& transport, PeerHandle peer,
        ReplicationPeerPolicy policy);
    // A disconnected stream stays dormant for identity-verified reconnection.
    // Explicit retirement consumes its incarnations and optionally destroys
    // owned entities; the authority must issue newer incarnations afterward.
    [[nodiscard]] ReplicationResult remove_peer(PeerHandle peer, bool retire_entities = false);
    [[nodiscard]] std::optional<ReplicationPeerState> peer(PeerHandle peer) const;
    [[nodiscard]] std::vector<ReplicationPeerState> peers() const;
    [[nodiscard]] ReplicationResult capture(std::uint64_t tick, double server_seconds);
    [[nodiscard]] const SnapshotFrame* latest_capture() const noexcept;
    [[nodiscard]] ReplicationResult send(NetworkTransport& transport, PeerHandle peer,
        bool force_full = false);
    [[nodiscard]] ReplicationResult receive(NetworkTransport& transport,
        double now_seconds, std::size_t message_budget = 32);
    [[nodiscard]] ReplicationResult request_resync(PeerHandle peer);
    [[nodiscard]] const SnapshotFrame* latest_received(PeerHandle peer) const noexcept;
    // History is lost only when a received frame newer than the nonzero cursor
    // was evicted. Evicting the cursor itself does not lose unread snapshots.
    [[nodiscard]] SnapshotHistory received_since(PeerHandle peer, std::uint64_t sequence,
        std::size_t limit = 16) const;
    // Samples retain absolute coordinates and never overwrite a physics pose.
    [[nodiscard]] std::optional<InterpolationSample> sample(NetworkEntityId id,
        double remote_seconds) const;
    [[nodiscard]] std::optional<math::Transform> runtime_transform(
        const EntitySnapshot& state) const noexcept;
    [[nodiscard]] ReplicationStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_replication(core::Kernel& kernel);

} // namespace ge::network
