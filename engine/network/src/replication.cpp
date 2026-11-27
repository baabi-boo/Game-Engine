#include <ge/network/replication.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace ge::network {
namespace {
constexpr std::uint32_t replication_magic = 0x47455250U;
constexpr std::uint16_t replication_version = 1;
constexpr std::uint64_t maximum_sequence = std::numeric_limits<std::uint64_t>::max();
bool has(SnapshotMask mask, SnapshotField field) noexcept {
    return (mask & field_mask(field)) != 0;
}
bool same(math::Vec3 first, math::Vec3 second) noexcept {
    return first.x == second.x && first.y == second.y && first.z == second.z;
}
bool same_bits(double first, double second) noexcept {
    return std::bit_cast<std::uint64_t>(first) == std::bit_cast<std::uint64_t>(second);
}
bool same_bits(math::Vec3 first, math::Vec3 second) noexcept {
    return same_bits(first.x, second.x) && same_bits(first.y, second.y) && same_bits(first.z, second.z);
}
bool same_bits(math::Quat first, math::Quat second) noexcept {
    return same_bits(first.x, second.x) && same_bits(first.y, second.y) &&
        same_bits(first.z, second.z) && same_bits(first.w, second.w);
}
bool bounded(math::Vec3 value, double limit) noexcept {
    return math::finite(value) && std::abs(value.x) <= limit &&
        std::abs(value.y) <= limit && std::abs(value.z) <= limit;
}
bool valid_authority(ReplicationAuthority value) noexcept {
    return value == ReplicationAuthority::Server || value == ReplicationAuthority::Owner;
}
bool valid_frame(const SnapshotFrame& frame, std::size_t maximum) noexcept {
    if (frame.sequence == 0 || !std::isfinite(frame.server_seconds) ||
        frame.server_seconds < 0.0 || frame.entities.size() > maximum) return false;
    ge::PersistentId previous{};
    for (const auto& entity : frame.entities) {
        if (!valid_snapshot(entity) || entity.id.persistent <= previous) return false;
        previous = entity.id.persistent;
    }
    return true;
}
void write_vector(core::ByteWriter& writer, math::Vec3 value) {
    writer.write_f64(value.x);
    writer.write_f64(value.y);
    writer.write_f64(value.z);
}
math::Vec3 read_vector(core::ByteReader& reader) {
    const auto x = reader.read_f64();
    const auto y = reader.read_f64();
    const auto z = reader.read_f64();
    return {x, y, z};
}
void write_fields(core::ByteWriter& writer, const EntitySnapshot& value, SnapshotMask mask) {
    if (has(mask, SnapshotField::Position)) write_vector(writer, value.world.position);
    if (has(mask, SnapshotField::Rotation)) {
        writer.write_f64(value.world.rotation.x);
        writer.write_f64(value.world.rotation.y);
        writer.write_f64(value.world.rotation.z);
        writer.write_f64(value.world.rotation.w);
    }
    if (has(mask, SnapshotField::Scale)) write_vector(writer, value.world.scale);
    if (has(mask, SnapshotField::Velocity)) {
        writer.write_bool(value.has_velocity);
        if (value.has_velocity) {
            write_vector(writer, value.linear_velocity);
            write_vector(writer, value.angular_velocity);
        }
    }
    if (has(mask, SnapshotField::Enabled)) {
        writer.write_bool(value.enabled);
        writer.write_bool(value.physics_enabled);
    }
    if (has(mask, SnapshotField::Layers)) writer.write_u64(value.layers);
    if (has(mask, SnapshotField::Owner)) writer.write_u64(value.owner.value);
    if (has(mask, SnapshotField::Prefab)) writer.write_u64(value.prefab.value);
    if (has(mask, SnapshotField::Authority)) writer.write_u8(static_cast<std::uint8_t>(value.authority));
    if (has(mask, SnapshotField::InputAcknowledgement)) {
        writer.write_bool(value.acknowledged_input.has_value());
        if (value.acknowledged_input) writer.write_u64(*value.acknowledged_input);
    }
}
void read_fields(core::ByteReader& reader, EntitySnapshot& value, SnapshotMask mask) {
    if (has(mask, SnapshotField::Position)) value.world.position = read_vector(reader);
    if (has(mask, SnapshotField::Rotation)) {
        value.world.rotation.x = reader.read_f64();
        value.world.rotation.y = reader.read_f64();
        value.world.rotation.z = reader.read_f64();
        value.world.rotation.w = reader.read_f64();
    }
    if (has(mask, SnapshotField::Scale)) value.world.scale = read_vector(reader);
    if (has(mask, SnapshotField::Velocity)) {
        value.has_velocity = reader.read_bool();
        value.linear_velocity = value.has_velocity ? read_vector(reader) : math::Vec3{};
        value.angular_velocity = value.has_velocity ? read_vector(reader) : math::Vec3{};
    }
    if (has(mask, SnapshotField::Enabled)) {
        value.enabled = reader.read_bool();
        value.physics_enabled = reader.read_bool();
    }
    if (has(mask, SnapshotField::Layers)) value.layers = reader.read_u64();
    if (has(mask, SnapshotField::Owner)) value.owner.value = reader.read_u64();
    if (has(mask, SnapshotField::Prefab)) value.prefab.value = reader.read_u64();
    if (has(mask, SnapshotField::Authority)) value.authority = static_cast<ReplicationAuthority>(reader.read_u8());
    if (has(mask, SnapshotField::InputAcknowledgement)) {
        value.acknowledged_input = reader.read_bool() ? std::optional<std::uint64_t>{reader.read_u64()} : std::nullopt;
    }
}
void merge_fields(EntitySnapshot& destination, const EntitySnapshot& source, SnapshotMask mask) {
    if (has(mask, SnapshotField::Position)) destination.world.position = source.world.position;
    if (has(mask, SnapshotField::Rotation)) destination.world.rotation = source.world.rotation;
    if (has(mask, SnapshotField::Scale)) destination.world.scale = source.world.scale;
    if (has(mask, SnapshotField::Velocity)) {
        destination.has_velocity = source.has_velocity;
        destination.linear_velocity = source.linear_velocity;
        destination.angular_velocity = source.angular_velocity;
    }
    if (has(mask, SnapshotField::Enabled)) {
        destination.enabled = source.enabled;
        destination.physics_enabled = source.physics_enabled;
    }
    if (has(mask, SnapshotField::Layers)) destination.layers = source.layers;
    if (has(mask, SnapshotField::Owner)) destination.owner = source.owner;
    if (has(mask, SnapshotField::Prefab)) destination.prefab = source.prefab;
    if (has(mask, SnapshotField::Authority)) destination.authority = source.authority;
    if (has(mask, SnapshotField::InputAcknowledgement)) destination.acknowledged_input = source.acknowledged_input;
}
bool valid_packet(const ReplicationPacket& packet, const ReplicationConfig& config) noexcept {
    if (!packet.world || !packet.sender || !std::isfinite(packet.server_seconds) ||
        packet.server_seconds < 0.0 || packet.changes.size() > config.max_entities * 2) return false;
    const bool control = packet.kind == ReplicationMessage::Ack || packet.kind == ReplicationMessage::Resync;
    if (control) return packet.changes.empty() && packet.baseline == 0 && packet.baseline_hash == 0 &&
        packet.tick == 0 && packet.server_seconds == 0.0 &&
        (packet.kind == ReplicationMessage::Resync || packet.sequence != 0);
    if (packet.sequence == 0) return false;
    if (packet.kind == ReplicationMessage::Full) {
        if (packet.baseline != 0 || packet.baseline_hash != 0 || packet.changes.size() > config.max_entities) return false;
    } else if (packet.kind == ReplicationMessage::Delta) {
        if (packet.baseline == 0 || packet.baseline >= packet.sequence) return false;
    } else return false;
    NetworkEntityId previous{};
    for (const auto& change : packet.changes) {
        if (!change.id || change.id <= previous || (change.fields & ~all_snapshot_fields) != 0) return false;
        if (packet.kind == ReplicationMessage::Full && change.change != EntityChange::Spawn) return false;
        switch (change.change) {
        case EntityChange::Spawn:
            if (change.fields != all_snapshot_fields || change.value.id != change.id || !valid_snapshot(change.value)) return false;
            break;
        case EntityChange::Update:
            if (change.fields == 0 || change.value.id != change.id) return false;
            break;
        case EntityChange::Despawn:
            if (change.fields != 0) return false;
            break;
        default: return false;
        }
        previous = change.id;
    }
    return true;
}
const EntitySnapshot* lookup(const SnapshotFrame& frame, NetworkEntityId id) noexcept {
    const auto found = std::lower_bound(frame.entities.begin(), frame.entities.end(), id,
        [](const EntitySnapshot& value, NetworkEntityId key) { return value.id < key; });
    return found != frame.entities.end() && found->id == id ? &*found : nullptr;
}
struct BusyScope {
    bool& flag;
    explicit BusyScope(bool& value) : flag(value) { flag = true; }
    ~BusyScope() { flag = false; }
};
std::optional<std::uint64_t> parse_number(std::string_view text) noexcept {
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return result;
}
}

std::string_view replication_error_name(ReplicationError error) noexcept {
    switch (error) {
    case ReplicationError::None: return "none";
    case ReplicationError::InvalidConfiguration: return "invalid configuration";
    case ReplicationError::NotAttached: return "scene not attached";
    case ReplicationError::InvalidEntity: return "invalid scene root";
    case ReplicationError::InvalidIdentity: return "invalid identity";
    case ReplicationError::IdentityConflict: return "identity conflict";
    case ReplicationError::StaleGeneration: return "stale incarnation";
    case ReplicationError::Capacity: return "capacity exhausted";
    case ReplicationError::InvalidTime: return "invalid time";
    case ReplicationError::InvalidSnapshot: return "invalid snapshot";
    case ReplicationError::Malformed: return "malformed packet";
    case ReplicationError::UnsupportedVersion: return "unsupported version";
    case ReplicationError::BaselineMissing: return "baseline missing";
    case ReplicationError::BaselineMismatch: return "baseline mismatch";
    case ReplicationError::Unauthorized: return "unauthorized";
    case ReplicationError::UnknownPeer: return "unknown peer";
    case ReplicationError::TransportFailure: return "transport failure";
    case ReplicationError::FactoryRequired: return "resource factory required";
    case ReplicationError::ApplyFailed: return "world publication failed";
    case ReplicationError::Busy: return "service busy";
    case ReplicationError::SequenceExhausted: return "sequence exhausted";
    case ReplicationError::OutOfMemory: return "out of memory";
    }
    return "unknown";
}
ReplicationError validate_replication_config(const ReplicationConfig& config) noexcept {
    if (!config.world || !config.local_identity || !config.server_identity ||
        config.max_entities == 0 || config.max_entities > 16384 ||
        config.max_identities < config.max_entities || config.max_identities > 1048576 ||
        config.max_peers == 0 || config.max_peers > 256 ||
        config.max_message_bytes < 256 || config.max_message_bytes > 16U * 1024U * 1024U ||
        config.history_frames < 2 || config.history_frames > 128 ||
        config.max_history_states < config.max_entities * 2 || config.max_history_states > 4194304 ||
        config.interpolation_samples == 0 || config.interpolation_samples > 128 ||
        config.receive_messages_per_frame == 0 || config.receive_messages_per_frame > 4096 ||
        !std::isfinite(config.max_extrapolation_seconds) || config.max_extrapolation_seconds < 0.0 ||
        config.max_extrapolation_seconds > 1.0 || !std::isfinite(config.resync_interval_seconds) ||
        config.resync_interval_seconds < 0.01 || config.resync_interval_seconds > 60.0 ||
        config.send_every_ticks == 0) return ReplicationError::InvalidConfiguration;
    return ReplicationError::None;
}
bool valid_snapshot(const EntitySnapshot& value) noexcept {
    const auto scale = value.world.scale;
    const double rotation_length = math::length(value.world.rotation);
    return value.id && valid_authority(value.authority) &&
        (value.authority != ReplicationAuthority::Owner || value.owner.valid()) &&
        bounded(value.world.position, 1.0e12) && math::finite(value.world.rotation) &&
        std::isfinite(rotation_length) && std::abs(rotation_length - 1.0) <= 1.0e-6 &&
        bounded(scale, 1.0e6) && std::abs(scale.x) >= 1.0e-8 &&
        std::abs(scale.y) >= 1.0e-8 && std::abs(scale.z) >= 1.0e-8 &&
        bounded(value.linear_velocity, 1.0e6) && bounded(value.angular_velocity, 1.0e6) &&
        (value.has_velocity || (same(value.linear_velocity, {}) && same(value.angular_velocity, {})));
}
SnapshotMask changed_fields(const EntitySnapshot& first, const EntitySnapshot& second) noexcept {
    SnapshotMask result{};
    const auto add = [&result](SnapshotField field) { result = static_cast<SnapshotMask>(result | field_mask(field)); };
    if (!same_bits(first.world.position, second.world.position)) add(SnapshotField::Position);
    if (!same_bits(first.world.rotation, second.world.rotation)) add(SnapshotField::Rotation);
    if (!same_bits(first.world.scale, second.world.scale)) add(SnapshotField::Scale);
    if (first.has_velocity != second.has_velocity || !same_bits(first.linear_velocity, second.linear_velocity) ||
        !same_bits(first.angular_velocity, second.angular_velocity)) add(SnapshotField::Velocity);
    if (first.enabled != second.enabled || first.physics_enabled != second.physics_enabled) add(SnapshotField::Enabled);
    if (first.layers != second.layers) add(SnapshotField::Layers);
    if (first.owner != second.owner) add(SnapshotField::Owner);
    if (first.prefab != second.prefab) add(SnapshotField::Prefab);
    if (first.authority != second.authority) add(SnapshotField::Authority);
    if (first.acknowledged_input != second.acknowledged_input) add(SnapshotField::InputAcknowledgement);
    return result;
}
std::uint64_t snapshot_hash(const SnapshotFrame& frame) {
    core::ByteWriter writer(16U * 1024U * 1024U, core::ByteOrder::big);
    writer.write_u64(frame.sequence);
    writer.write_u64(frame.tick);
    writer.write_f64(frame.server_seconds);
    writer.write_u64(static_cast<std::uint64_t>(frame.entities.size()));
    for (const auto& value : frame.entities) {
        writer.write_u64(value.id.persistent.value);
        writer.write_u64(value.id.incarnation);
        write_fields(writer, value, all_snapshot_fields);
    }
    return core::fnv1a64(writer.bytes());
}
ReplicationResult encode_replication(const ReplicationPacket& packet, std::vector<std::byte>& output,
    const ReplicationConfig& limits) {
    if (validate_replication_config(limits) != ReplicationError::None) return {ReplicationError::InvalidConfiguration};
    if (!valid_packet(packet, limits)) return {ReplicationError::InvalidSnapshot};
    try {
        core::ByteWriter writer(limits.max_message_bytes, core::ByteOrder::big);
        writer.write_u32(replication_magic);
        writer.write_u16(replication_version);
        writer.write_u8(static_cast<std::uint8_t>(packet.kind));
        writer.write_u8(0);
        writer.write_u64(packet.world.value);
        writer.write_u64(packet.sender.value);
        writer.write_u64(packet.sequence);
        writer.write_u64(packet.baseline);
        writer.write_u64(packet.baseline_hash);
        writer.write_u64(packet.state_hash);
        writer.write_u64(packet.tick);
        writer.write_f64(packet.server_seconds);
        writer.write_u32(static_cast<std::uint32_t>(packet.changes.size()));
        for (const auto& change : packet.changes) {
            writer.write_u64(change.id.persistent.value);
            writer.write_u64(change.id.incarnation);
            writer.write_u8(static_cast<std::uint8_t>(change.change));
            writer.write_u16(change.fields);
            write_fields(writer, change.value, change.fields);
        }
        output = writer.take();
        return {ReplicationError::None, packet.changes.size(), packet.sequence};
    } catch (const core::BinaryException&) { return {ReplicationError::Capacity};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory}; }
}
ReplicationResult decode_replication(std::span<const std::byte> bytes, ReplicationPacket& output,
    const ReplicationConfig& limits) {
    if (validate_replication_config(limits) != ReplicationError::None) return {ReplicationError::InvalidConfiguration};
    if (bytes.size() > limits.max_message_bytes) return {ReplicationError::Capacity};
    try {
        core::BinaryLimits binary_limits;
        binary_limits.max_total_bytes = limits.max_message_bytes;
        core::ByteReader reader(bytes, core::ByteOrder::big, binary_limits);
        if (reader.read_u32() != replication_magic) return {ReplicationError::Malformed};
        if (reader.read_u16() != replication_version) return {ReplicationError::UnsupportedVersion};
        ReplicationPacket packet;
        packet.kind = static_cast<ReplicationMessage>(reader.read_u8());
        if (reader.read_u8() != 0) return {ReplicationError::Malformed};
        packet.world.value = reader.read_u64();
        packet.sender.value = reader.read_u64();
        packet.sequence = reader.read_u64();
        packet.baseline = reader.read_u64();
        packet.baseline_hash = reader.read_u64();
        packet.state_hash = reader.read_u64();
        packet.tick = reader.read_u64();
        packet.server_seconds = reader.read_f64();
        const auto count = reader.read_u32();
        if (count > limits.max_entities * 2 || count > reader.remaining() / 19) return {ReplicationError::Capacity};
        packet.changes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            EntityDelta change;
            change.id.persistent.value = reader.read_u64();
            change.id.incarnation = reader.read_u64();
            change.change = static_cast<EntityChange>(reader.read_u8());
            change.fields = reader.read_u16();
            if ((change.fields & ~all_snapshot_fields) != 0) return {ReplicationError::Malformed};
            change.value.id = change.id;
            read_fields(reader, change.value, change.fields);
            packet.changes.push_back(std::move(change));
        }
        reader.require_end();
        if (!valid_packet(packet, limits)) return {ReplicationError::Malformed};
        output = std::move(packet);
        return {ReplicationError::None, count, output.sequence};
    } catch (const core::BinaryException&) { return {ReplicationError::Malformed};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory}; }
}
ReplicationResult make_replication_delta(const SnapshotFrame& current, const SnapshotFrame* baseline,
    ReplicationPacket& output, const ReplicationConfig& config) {
    if (validate_replication_config(config) != ReplicationError::None) return {ReplicationError::InvalidConfiguration};
    if (!valid_frame(current, config.max_entities) || (baseline &&
        (!valid_frame(*baseline, config.max_entities) || baseline->sequence >= current.sequence ||
         baseline->tick > current.tick || baseline->server_seconds > current.server_seconds))) return {ReplicationError::InvalidSnapshot};
    try {
        ReplicationPacket packet;
        packet.kind = baseline ? ReplicationMessage::Delta : ReplicationMessage::Full;
        packet.world = config.world;
        packet.sender = config.local_identity;
        packet.sequence = current.sequence;
        packet.tick = current.tick;
        packet.server_seconds = current.server_seconds;
        packet.state_hash = snapshot_hash(current);
        packet.baseline = baseline ? baseline->sequence : 0;
        packet.baseline_hash = baseline ? snapshot_hash(*baseline) : 0;
        if (baseline) {
            for (const auto& old : baseline->entities) {
                if (!lookup(current, old.id)) packet.changes.push_back({EntityChange::Despawn, old.id, 0, {}});
            }
        }
        for (const auto& state : current.entities) {
            const auto* old = baseline ? lookup(*baseline, state.id) : nullptr;
            const auto fields = old ? changed_fields(*old, state) : all_snapshot_fields;
            if (fields != 0) packet.changes.push_back({old ? EntityChange::Update : EntityChange::Spawn, state.id, fields, state});
        }
        std::sort(packet.changes.begin(), packet.changes.end(),
            [](const EntityDelta& first, const EntityDelta& second) { return first.id < second.id; });
        output = std::move(packet);
        return {ReplicationError::None, output.changes.size(), output.sequence};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory};
    } catch (const core::BinaryException&) { return {ReplicationError::Capacity}; }
}
ReplicationResult reconstruct_snapshot(const ReplicationPacket& packet, const SnapshotFrame* baseline,
    SnapshotFrame& output, const ReplicationConfig& limits) {
    if (validate_replication_config(limits) != ReplicationError::None) return {ReplicationError::InvalidConfiguration};
    if (!valid_packet(packet, limits) || (packet.kind != ReplicationMessage::Full && packet.kind != ReplicationMessage::Delta)) return {ReplicationError::InvalidSnapshot};
    try {
        std::map<NetworkEntityId, EntitySnapshot> states;
        if (packet.kind == ReplicationMessage::Delta) {
            if (!baseline || baseline->sequence != packet.baseline) return {ReplicationError::BaselineMissing};
            if (!valid_frame(*baseline, limits.max_entities) || snapshot_hash(*baseline) != packet.baseline_hash) return {ReplicationError::BaselineMismatch};
            if (baseline->tick > packet.tick || baseline->server_seconds > packet.server_seconds) return {ReplicationError::InvalidTime};
            for (const auto& value : baseline->entities) states.emplace(value.id, value);
        }
        for (const auto& change : packet.changes) {
            const auto found = states.find(change.id);
            if (change.change == EntityChange::Spawn) {
                if (found != states.end()) return {ReplicationError::InvalidSnapshot};
                states.emplace(change.id, change.value);
            } else if (change.change == EntityChange::Despawn) {
                if (found == states.end()) return {ReplicationError::InvalidSnapshot};
                states.erase(found);
            } else {
                if (found == states.end()) return {ReplicationError::InvalidSnapshot};
                merge_fields(found->second, change.value, change.fields);
            }
        }
        if (states.size() > limits.max_entities) return {ReplicationError::Capacity};
        SnapshotFrame frame{packet.sequence, packet.tick, packet.server_seconds, {}};
        frame.entities.reserve(states.size());
        for (auto& [id, state] : states) {
            (void)id;
            frame.entities.push_back(std::move(state));
        }
        if (!valid_frame(frame, limits.max_entities)) return {ReplicationError::InvalidSnapshot};
        if (snapshot_hash(frame) != packet.state_hash) return {ReplicationError::BaselineMismatch};
        output = std::move(frame);
        return {ReplicationError::None, output.entities.size(), output.sequence};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory};
    } catch (const core::BinaryException&) { return {ReplicationError::Malformed}; }
}

struct ReplicationService::Impl {
    struct TimedState {
        double seconds{};
        std::uint64_t tick{};
        EntitySnapshot state;
    };
    struct BindingRecord {
        ReplicationBinding binding;
        std::deque<TimedState> samples;
        PeerHandle sample_peer{};
        ge::PersistentId source_identity{};
    };
    struct PeerRecord {
        ReplicationPeerState state;
        std::deque<SnapshotFrame> sent;
        std::deque<SnapshotFrame> received;
        std::uint64_t ack_sequence{};
        std::uint64_t ack_hash{};
        bool ack_pending{};
        double last_resync{-60.0};
    };
    ReplicationConfig config;
    scene::Scene* scene{};
    physics::PhysicsWorld* physics{};
    scene::WorldPartition* partition{};
    NetworkTransport* transport{};
    ReplicationFactory factory;
    std::map<NetworkEntityId, BindingRecord> entities;
    std::map<ge::PersistentId, std::uint64_t> floors;
    std::map<PeerHandle, PeerRecord> peers;
    SnapshotFrame captured;
    ReplicationStatistics statistics;
    std::uint64_t next_sequence{1};
    double last_receive_seconds{};
    std::size_t history_states{};
    std::size_t peer_cursor{};
    bool busy{};
    explicit Impl(ReplicationConfig value) : config(value) {}
    bool local_authority(const ReplicationBindingOptions& options) const noexcept {
        return config.local_identity == config.server_identity ||
            (options.authority == ReplicationAuthority::Owner && options.owner == config.local_identity);
    }
    std::optional<math::Vec3> origin() const noexcept {
        if (!partition) return math::Vec3{};
        const auto value = partition->origin();
        return value.consistent ? std::optional<math::Vec3>{value.absolute_origin} : std::nullopt;
    }
    BindingRecord* by_persistent(ge::PersistentId id) noexcept {
        const auto found = entities.lower_bound(NetworkEntityId{id, 0});
        return found != entities.end() && found->first.persistent == id ? &found->second : nullptr;
    }
    bool root_valid(ecs::Entity entity) const {
        if (!scene || !scene->registry().alive(entity) || !scene->contains(entity) ||
            scene->parent(entity) || !scene->local_transform(entity)) return false;
        return !scene->registry().has<physics::BodyComponent>(entity) || (physics && physics->get(entity));
    }
    const SnapshotFrame* history_find(const std::deque<SnapshotFrame>& frames, std::uint64_t sequence) const noexcept {
        const auto found = std::find_if(frames.begin(), frames.end(),
            [sequence](const SnapshotFrame& frame) { return frame.sequence == sequence; });
        return found == frames.end() ? nullptr : &*found;
    }
    void drop_front(std::deque<SnapshotFrame>& frames) noexcept {
        history_states -= frames.front().entities.size();
        frames.pop_front();
    }
    bool admit_history(std::deque<SnapshotFrame>& frames, std::size_t states) {
        while (frames.size() > 1 && frames.size() >= config.history_frames) drop_front(frames);
        while (states > config.max_history_states - history_states) {
            std::deque<SnapshotFrame>* oldest{};
            for (auto& [handle, peer] : peers) {
                (void)handle;
                for (auto* history : {&peer.sent, &peer.received}) {
                    if (history->size() > 1 && (!oldest || history->size() > oldest->size())) oldest = history;
                }
            }
            if (!oldest) break;
            drop_front(*oldest);
        }
        return states <= config.max_history_states - history_states;
    }
    bool retire(NetworkEntityId id) {
        const auto found = entities.find(id);
        if (found == entities.end()) return true;
        const auto binding = found->second.binding;
        if (scene->registry().alive(binding.entity) && scene->registry().persistent_id(binding.entity) != id.persistent) return false;
        if (!binding.options.destroy_on_despawn && scene->registry().alive(binding.entity)) {
            if (!scene->registry().set_enabled(binding.entity, false)) return false;
            if (physics && physics->find(binding.entity) && !physics->set_enabled(physics->find(binding.entity), false)) return false;
        }
        if (binding.options.destroy_on_despawn && scene->registry().alive(binding.entity)) {
            if (scene->registry().persistent_id(binding.entity) != id.persistent ||
                (scene->registry().has<physics::BodyComponent>(binding.entity) && (!physics || !physics->get(binding.entity))))
                return false;
            bool destroyed{};
            if (factory.despawn) destroyed = factory.despawn(*scene, physics, binding.entity);
            else {
                if (physics && physics->find(binding.entity)) (void)physics->remove(binding.entity);
                destroyed = static_cast<bool>(scene->destroy(binding.entity,
                    scene::DestroyPolicy::DetachChildren, scene::ReparentMode::KeepWorld));
            }
            if (!destroyed || scene->registry().alive(binding.entity)) return false;
        }
        entities.erase(found);
        ++statistics.despawned;
        return true;
    }
    ReplicationError preflight(const PeerRecord& peer, const SnapshotFrame& frame,
        std::vector<NetworkEntityId>& removals) {
        if (!scene) return ReplicationError::NotAttached;
        if (scene->registry().iterating() || (physics && physics->stepping())) return ReplicationError::Busy;
        const auto offset = origin();
        if (!offset) return ReplicationError::ApplyFailed;
        const bool server = peer.state.policy.identity == config.server_identity;
        if (!peer.state.policy.accept_snapshots) return ReplicationError::Unauthorized;
        std::size_t additions{};
        std::size_t new_identities{};
        for (const auto& [id, record] : entities) {
            if (record.binding.source_peer == peer.state.peer && !lookup(frame, id)) {
                if (!server) return ReplicationError::Unauthorized;
                removals.push_back(id);
            }
        }
        for (const auto& value : frame.entities) {
            if (!bounded(value.world.position - *offset, 1.0e12)) return ReplicationError::ApplyFailed;
            const auto exact = entities.find(value.id);
            if (exact == entities.end()) {
                if (!server || !peer.state.policy.allow_spawns) return ReplicationError::Unauthorized;
                const auto floor = floors.find(value.id.persistent);
                if (floor != floors.end() && value.id.incarnation <= floor->second) return ReplicationError::StaleGeneration;
                if (floor == floors.end()) ++new_identities;
                const auto* old = by_persistent(value.id.persistent);
                if (old && (!old->binding.options.destroy_on_despawn ||
                    old->binding.source_peer != peer.state.peer)) return ReplicationError::IdentityConflict;
                if (!old && scene->registry().find(value.id.persistent)) return ReplicationError::IdentityConflict;
                if (value.prefab && !factory.spawn) return ReplicationError::FactoryRequired;
                ++additions;
                continue;
            }
            const auto& binding = exact->second.binding;
            if (value.prefab != binding.options.prefab) return ReplicationError::FactoryRequired;
            if (!root_valid(binding.entity) || scene->registry().persistent_id(binding.entity) != value.id.persistent) {
                if (!scene->registry().alive(binding.entity) && binding.options.destroy_on_despawn && server &&
                    peer.state.policy.allow_spawns && !scene->registry().find(value.id.persistent)) {
                    if (value.prefab && !factory.spawn) return ReplicationError::FactoryRequired;
                    continue;
                }
                return ReplicationError::InvalidEntity;
            }
            if (!server && (binding.options.authority != ReplicationAuthority::Owner ||
                binding.options.owner != peer.state.policy.identity || value.owner != binding.options.owner ||
                value.authority != binding.options.authority || value.prefab != binding.options.prefab ||
                value.acknowledged_input.has_value()))
                return ReplicationError::Unauthorized;
            const bool predicted = binding.options.prediction_owned_pose && value.owner == config.local_identity;
            if (physics && !predicted) {
                const auto* body = physics->get(binding.entity);
                if (body && value.has_velocity) {
                    if (body->mode == physics::BodyMode::Static &&
                        (!same(value.linear_velocity, {}) || !same(value.angular_velocity, {}))) return ReplicationError::ApplyFailed;
                    if (math::length(value.linear_velocity) > physics->config().max_linear_speed ||
                        math::length(value.angular_velocity) > physics->config().max_angular_speed) return ReplicationError::ApplyFailed;
                }
            }
        }
        if (new_identities > config.max_identities - floors.size() ||
            additions > config.max_entities - (entities.size() - removals.size())) return ReplicationError::Capacity;
        return ReplicationError::None;
    }
    ReplicationError publish(BindingRecord& record, const EntitySnapshot& value) {
        auto& binding = record.binding;
        auto& registry = scene->registry();
        const auto offset = origin();
        if (!offset) return ReplicationError::ApplyFailed;
        auto runtime = value.world;
        runtime.position = runtime.position - *offset;
        if (!bounded(runtime.position, 1.0e12)) return ReplicationError::ApplyFailed;
        const auto body = physics ? physics->find(binding.entity) : physics::BodyHandle{};
        const bool predicted = binding.options.prediction_owned_pose && value.owner == config.local_identity;
        if (!predicted) {
            if (body) {
                const auto* state = physics->get(body);
                if (!state || (value.has_velocity &&
                    (math::length(value.linear_velocity) > physics->config().max_linear_speed ||
                     math::length(value.angular_velocity) > physics->config().max_angular_speed ||
                     (state->mode == physics::BodyMode::Static &&
                      (!same(value.linear_velocity, {}) || !same(value.angular_velocity, {}))))))
                    return ReplicationError::ApplyFailed;
                if (!physics->set_pose(body, runtime)) return ReplicationError::ApplyFailed;
                if (value.has_velocity && state && state->mode != physics::BodyMode::Static &&
                    !physics->set_velocity(body, value.linear_velocity, value.angular_velocity)) return ReplicationError::ApplyFailed;
                if (!value.has_velocity && state && state->mode != physics::BodyMode::Static &&
                    !physics->set_velocity(body, {}, {})) return ReplicationError::ApplyFailed;
            } else {
                if (!scene->set_world_transform(binding.entity, runtime)) return ReplicationError::ApplyFailed;
                if (value.has_velocity) registry.emplace_or_replace<ecs::VelocityComponent>(binding.entity,
                    ecs::VelocityComponent{value.linear_velocity, value.angular_velocity});
                else (void)registry.remove<ecs::VelocityComponent>(binding.entity);
            }
        }
        if (!registry.set_enabled(binding.entity, value.enabled) || !registry.set_layers(binding.entity, value.layers))
            return ReplicationError::ApplyFailed;
        if (body && !physics->set_enabled(body, value.physics_enabled)) return ReplicationError::ApplyFailed;
        binding.options.owner = value.owner;
        binding.options.authority = value.authority;
        binding.acknowledged_input = value.acknowledged_input;
        if (value.owner != config.local_identity) binding.options.prediction_owned_pose = false;
        return ReplicationError::None;
    }
    ReplicationError apply(PeerRecord& peer, const SnapshotFrame& frame) {
        std::vector<NetworkEntityId> removals;
        auto error = preflight(peer, frame, removals);
        if (error != ReplicationError::None) return error;
        for (const auto id : removals) if (!retire(id)) return ReplicationError::ApplyFailed;
        for (const auto& value : frame.entities) {
            auto found = entities.find(value.id);
            if (found != entities.end() && !scene->registry().alive(found->second.binding.entity)) {
                entities.erase(found);
                found = entities.end();
            }
            if (found == entities.end()) {
                const auto offset = origin();
                if (!offset) return ReplicationError::ApplyFailed;
                auto runtime = value.world;
                runtime.position = runtime.position - *offset;
                ReplicationBinding binding;
                binding.id = value.id;
                binding.options = {value.owner, value.prefab, value.authority, true, false};
                binding.source_peer = peer.state.peer;
                const auto floor = floors.try_emplace(value.id.persistent, 0);
                try {
                    found = entities.emplace(value.id, BindingRecord{binding, {}, {}, peer.state.policy.identity}).first;
                    const auto entity = factory.spawn ? factory.spawn(*scene, physics, value) :
                        scene->create("replicated", {}, runtime, value.id.persistent);
                    if (!entity || !root_valid(entity) || scene->registry().persistent_id(entity) != value.id.persistent) {
                        entities.erase(found);
                        if (floor.second) floors.erase(floor.first);
                        return ReplicationError::ApplyFailed;
                    }
                    found->second.binding.entity = entity;
                    floor.first->second = value.id.incarnation;
                } catch (...) {
                    if (found != entities.end()) entities.erase(found);
                    if (floor.second) floors.erase(floor.first);
                    throw;
                }
                ++statistics.spawned;
            }
            auto applied = value;
            if (peer.state.policy.identity != config.server_identity)
                applied.acknowledged_input = found->second.binding.acknowledged_input;
            error = publish(found->second, applied);
            if (error != ReplicationError::None) return error;
            if (peer.state.policy.identity == config.server_identity) {
                found->second.binding.source_peer = peer.state.peer;
                found->second.source_identity = peer.state.policy.identity;
            }
            auto& samples = found->second.samples;
            if (found->second.sample_peer != peer.state.peer) samples.clear();
            found->second.sample_peer = peer.state.peer;
            if (!samples.empty() && samples.back().seconds == frame.server_seconds) samples.pop_back();
            while (samples.size() >= config.interpolation_samples) samples.pop_front();
            samples.push_back({frame.server_seconds, frame.tick, value});
            ++statistics.updated;
        }
        return ReplicationError::None;
    }
    ReplicationError control(NetworkTransport& transport, PeerRecord& peer, double now) {
        ReplicationPacket packet;
        packet.world = config.world;
        packet.sender = config.local_identity;
        const bool resync = peer.state.resync_pending && now - peer.last_resync >= config.resync_interval_seconds;
        if (!resync && !peer.ack_pending) return ReplicationError::None;
        packet.kind = resync ? ReplicationMessage::Resync : ReplicationMessage::Ack;
        packet.sequence = resync ? peer.state.last_received : peer.ack_sequence;
        packet.state_hash = resync ? 0 : peer.ack_hash;
        std::vector<std::byte> wire;
        const auto encoded = encode_replication(packet, wire, config);
        if (!encoded) return encoded.error;
        if (!transport.send(peer.state.peer, config.channel, MessageKind::Replication, wire, DeliveryMode::ReliableOrdered)) {
            ++statistics.send_failures;
            return ReplicationError::TransportFailure;
        }
        if (resync) {
            peer.last_resync = now;
            ++statistics.resync_requests;
        } else peer.ack_pending = false;
        return ReplicationError::None;
    }
    ReplicationError consume(PeerRecord& peer, const ReceivedMessage& message) {
        if (message.delivery != DeliveryMode::ReliableOrdered) return ReplicationError::Unauthorized;
        ReplicationPacket packet;
        const auto decoded = decode_replication(message.payload, packet, config);
        if (!decoded) return decoded.error;
        if (packet.world != config.world || packet.sender != peer.state.policy.identity) return ReplicationError::Unauthorized;
        if (packet.kind == ReplicationMessage::Ack) {
            if (packet.sequence <= peer.state.acknowledged) return ReplicationError::None;
            const auto* frame = history_find(peer.sent, packet.sequence);
            if (!frame || snapshot_hash(*frame) != packet.state_hash) {
                peer.state.full_required = true;
                return ReplicationError::BaselineMismatch;
            }
            peer.state.acknowledged = packet.sequence;
            return ReplicationError::None;
        }
        if (packet.kind == ReplicationMessage::Resync) {
            peer.state.full_required = true;
            return ReplicationError::None;
        }
        if (!peer.state.policy.accept_snapshots) return ReplicationError::Unauthorized;
        const bool repeated_full = packet.kind == ReplicationMessage::Full && peer.state.resync_pending &&
            packet.sequence == peer.state.last_received && packet.state_hash == peer.ack_hash;
        if (packet.sequence <= peer.state.last_received && !repeated_full) {
            ++statistics.stale_messages;
            if (!peer.received.empty()) {
                peer.ack_sequence = peer.received.back().sequence;
                peer.ack_hash = snapshot_hash(peer.received.back());
                peer.ack_pending = true;
            }
            return ReplicationError::None;
        }
        if (!peer.received.empty() && (packet.tick < peer.received.back().tick ||
            packet.server_seconds < peer.received.back().server_seconds)) return ReplicationError::InvalidTime;
        const auto* baseline = history_find(peer.received, packet.baseline);
        SnapshotFrame frame;
        const auto reconstructed = reconstruct_snapshot(packet, baseline, frame, config);
        if (!reconstructed) return reconstructed.error;
        if (!admit_history(peer.received, repeated_full ? 0 : frame.entities.size())) return ReplicationError::Capacity;
        const auto error = apply(peer, frame);
        if (error != ReplicationError::None) return error;
        const auto count = frame.entities.size();
        if (repeated_full && !peer.received.empty() && peer.received.back().sequence == packet.sequence) {
            history_states -= peer.received.back().entities.size();
            peer.received.back() = std::move(frame);
        } else peer.received.push_back(std::move(frame));
        history_states += count;
        peer.state.last_received = packet.sequence;
        peer.state.resync_pending = false;
        peer.ack_sequence = packet.sequence;
        peer.ack_hash = packet.state_hash;
        peer.ack_pending = true;
        ++statistics.frames_received;
        return ReplicationError::None;
    }
};

ReplicationService::ReplicationService(ReplicationConfig configuration) {
    if (validate_replication_config(configuration) != ReplicationError::None)
        throw std::invalid_argument("invalid replication configuration");
    impl_ = std::make_unique<Impl>(configuration);
}
ReplicationService::~ReplicationService() = default;
const ReplicationConfig& ReplicationService::config() const noexcept { return impl_->config; }
ReplicationResult ReplicationService::attach(scene::Scene& scene, physics::PhysicsWorld* physics,
    scene::WorldPartition* partition) {
    if (impl_->busy || (impl_->scene && impl_->scene != &scene)) return {ReplicationError::Busy};
    if (physics && &physics->scene() != &scene) return {ReplicationError::InvalidConfiguration};
    if ((!impl_->entities.empty() || !impl_->peers.empty()) && impl_->scene &&
        (impl_->physics != physics || impl_->partition != partition)) return {ReplicationError::Busy};
    impl_->scene = &scene;
    impl_->physics = physics;
    impl_->partition = partition;
    return {};
}
ReplicationResult ReplicationService::set_factory(ReplicationFactory factory) {
    if (impl_->busy || !impl_->entities.empty()) return {ReplicationError::Busy};
    if (static_cast<bool>(factory.spawn) != static_cast<bool>(factory.despawn)) return {ReplicationError::InvalidConfiguration};
    impl_->factory = std::move(factory);
    return {};
}
ReplicationResult ReplicationService::bind(ecs::Entity entity, NetworkEntityId id, ReplicationBindingOptions options) {
    auto& state = *impl_;
    if (state.busy) return {ReplicationError::Busy};
    if (!state.scene) return {ReplicationError::NotAttached};
    if (!id || !valid_authority(options.authority) ||
        (options.authority == ReplicationAuthority::Owner && !options.owner)) return {ReplicationError::InvalidIdentity};
    if (!state.root_valid(entity) || state.scene->registry().iterating()) return {ReplicationError::InvalidEntity};
    if (state.by_persistent(id.persistent) || binding(entity)) return {ReplicationError::IdentityConflict};
    const auto old_id = state.scene->registry().persistent_id(entity);
    if (old_id && old_id != id.persistent) return {ReplicationError::IdentityConflict};
    if (state.scene->registry().find(id.persistent) && state.scene->registry().find(id.persistent) != entity)
        return {ReplicationError::IdentityConflict};
    const auto floor = state.floors.find(id.persistent);
    if (floor != state.floors.end() && id.incarnation <= floor->second) return {ReplicationError::StaleGeneration};
    if (state.entities.size() >= state.config.max_entities ||
        (floor == state.floors.end() && state.floors.size() >= state.config.max_identities)) return {ReplicationError::Capacity};
    if (options.prediction_owned_pose && options.owner != state.config.local_identity) return {ReplicationError::Unauthorized};
    try {
        state.entities.emplace(id, Impl::BindingRecord{ReplicationBinding{id, entity, options, {}, {}}, {}, {}, {}});
        try {
            state.floors.try_emplace(id.persistent, 0);
            if (!old_id && !state.scene->registry().set_persistent_id(entity, id.persistent)) {
                state.entities.erase(id);
                if (floor == state.floors.end()) state.floors.erase(id.persistent);
                return {ReplicationError::IdentityConflict};
            }
            state.floors.at(id.persistent) = id.incarnation;
        } catch (...) {
            state.entities.erase(id);
            if (floor == state.floors.end()) state.floors.erase(id.persistent);
            throw;
        }
        return {ReplicationError::None, 1};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory};
    } catch (const std::exception&) { return {ReplicationError::ApplyFailed}; }
}
ReplicationResult ReplicationService::unbind(NetworkEntityId id) {
    if (impl_->busy) return {ReplicationError::Busy};
    if (impl_->entities.erase(id) == 0) return {ReplicationError::InvalidIdentity};
    return {ReplicationError::None, 1};
}
ReplicationResult ReplicationService::set_owner(NetworkEntityId id, ge::PersistentId owner,
    ReplicationAuthority authority) {
    if (impl_->busy) return {ReplicationError::Busy};
    if (impl_->config.local_identity != impl_->config.server_identity) return {ReplicationError::Unauthorized};
    if (!valid_authority(authority) || (authority == ReplicationAuthority::Owner && !owner)) return {ReplicationError::InvalidIdentity};
    const auto found = impl_->entities.find(id);
    if (found == impl_->entities.end()) return {ReplicationError::InvalidIdentity};
    if (found->second.binding.options.owner != owner) found->second.binding.acknowledged_input.reset();
    found->second.binding.options.owner = owner;
    found->second.binding.options.authority = authority;
    if (owner != impl_->config.local_identity) found->second.binding.options.prediction_owned_pose = false;
    return {ReplicationError::None, 1};
}
ReplicationResult ReplicationService::set_prediction_owned(NetworkEntityId id, bool enabled) {
    if (impl_->busy) return {ReplicationError::Busy};
    const auto found = impl_->entities.find(id);
    if (found == impl_->entities.end()) return {ReplicationError::InvalidIdentity};
    if (enabled && found->second.binding.options.owner != impl_->config.local_identity) return {ReplicationError::Unauthorized};
    found->second.binding.options.prediction_owned_pose = enabled;
    return {ReplicationError::None, 1};
}
ReplicationResult ReplicationService::acknowledge_input(NetworkEntityId id,
    std::optional<std::uint64_t> input_sequence) {
    if (impl_->busy) return {ReplicationError::Busy};
    if (impl_->config.local_identity != impl_->config.server_identity) return {ReplicationError::Unauthorized};
    const auto found = impl_->entities.find(id);
    if (found == impl_->entities.end()) return {ReplicationError::InvalidIdentity};
    const auto previous = found->second.binding.acknowledged_input;
    if (previous && input_sequence && *input_sequence < *previous) return {ReplicationError::InvalidSnapshot};
    found->second.binding.acknowledged_input = input_sequence;
    return {ReplicationError::None, 1};
}
std::optional<ReplicationBinding> ReplicationService::binding(NetworkEntityId id) const {
    const auto found = impl_->entities.find(id);
    return found == impl_->entities.end() ? std::nullopt : std::optional<ReplicationBinding>{found->second.binding};
}
std::optional<ReplicationBinding> ReplicationService::binding(ecs::Entity entity) const {
    for (const auto& [id, record] : impl_->entities) {
        (void)id;
        if (record.binding.entity == entity) return record.binding;
    }
    return std::nullopt;
}
ecs::Entity ReplicationService::find(NetworkEntityId id) const noexcept {
    const auto found = impl_->entities.find(id);
    return found != impl_->entities.end() && impl_->scene && impl_->scene->registry().alive(found->second.binding.entity)
        && impl_->scene->registry().persistent_id(found->second.binding.entity) == id.persistent
        ? found->second.binding.entity : ecs::Entity{};
}
std::vector<ReplicationBinding> ReplicationService::bindings() const {
    std::vector<ReplicationBinding> result;
    result.reserve(impl_->entities.size());
    for (const auto& [id, record] : impl_->entities) {
        (void)id;
        result.push_back(record.binding);
    }
    return result;
}
ReplicationResult ReplicationService::add_peer(NetworkTransport& transport, PeerHandle peer, ReplicationPeerPolicy policy) {
    if (impl_->busy) return {ReplicationError::Busy};
    if (impl_->transport && impl_->transport != &transport) return {ReplicationError::InvalidConfiguration};
    if (!policy.identity || policy.identity == impl_->config.local_identity || !peer) return {ReplicationError::InvalidIdentity};
    const auto network_peer = transport.peer(peer);
    if (!network_peer || network_peer->failure != ProtocolError::None) return {ReplicationError::UnknownPeer};
    if (impl_->peers.contains(peer)) return {ReplicationError::IdentityConflict};
    for (const auto& [handle, record] : impl_->peers) {
        (void)handle;
        if (record.state.policy.identity == policy.identity) return {ReplicationError::IdentityConflict};
    }
    if (policy.allow_spawns && (!policy.accept_snapshots || policy.identity != impl_->config.server_identity))
        return {ReplicationError::Unauthorized};
    if (impl_->peers.size() >= impl_->config.max_peers) return {ReplicationError::Capacity};
    try {
        Impl::PeerRecord record;
        record.state.peer = peer;
        record.state.policy = policy;
        impl_->peers.emplace(peer, std::move(record));
        impl_->transport = &transport;
        if (policy.accept_snapshots) {
            for (auto& [id, binding] : impl_->entities) {
                (void)id;
                if (!binding.binding.source_peer && binding.source_identity == policy.identity)
                    binding.binding.source_peer = peer;
            }
        }
        return {ReplicationError::None, 1};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory}; }
}
ReplicationResult ReplicationService::remove_peer(PeerHandle peer, bool retire_entities) {
    if (impl_->busy) return {ReplicationError::Busy};
    const auto found = impl_->peers.find(peer);
    if (found == impl_->peers.end()) return {ReplicationError::UnknownPeer};
    if (impl_->scene && (impl_->scene->registry().iterating() || (impl_->physics && impl_->physics->stepping())))
        return {ReplicationError::Busy};
    BusyScope scope(impl_->busy);
    try {
        std::vector<NetworkEntityId> removals;
        for (const auto& [id, record] : impl_->entities) if (record.binding.source_peer == peer) removals.push_back(id);
        for (const auto id : removals) {
            if (retire_entities) {
                if (!impl_->retire(id)) return {ReplicationError::ApplyFailed};
            } else {
                auto& record = impl_->entities.at(id);
                record.binding.source_peer = {};
                record.sample_peer = {};
                record.samples.clear();
            }
        }
        while (!found->second.sent.empty()) impl_->drop_front(found->second.sent);
        while (!found->second.received.empty()) impl_->drop_front(found->second.received);
        impl_->peers.erase(found);
        return {ReplicationError::None, removals.size()};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory};
    } catch (const std::exception&) { return {ReplicationError::ApplyFailed}; }
}
std::optional<ReplicationPeerState> ReplicationService::peer(PeerHandle peer) const {
    const auto found = impl_->peers.find(peer);
    return found == impl_->peers.end() ? std::nullopt : std::optional<ReplicationPeerState>{found->second.state};
}
std::vector<ReplicationPeerState> ReplicationService::peers() const {
    std::vector<ReplicationPeerState> result;
    result.reserve(impl_->peers.size());
    for (const auto& [handle, record] : impl_->peers) {
        (void)handle;
        result.push_back(record.state);
    }
    return result;
}
ReplicationResult ReplicationService::capture(std::uint64_t tick, double server_seconds) {
    auto& state = *impl_;
    if (state.busy) return {ReplicationError::Busy};
    if (!state.scene) return {ReplicationError::NotAttached};
    if (!std::isfinite(server_seconds) || server_seconds < 0.0 ||
        (state.captured.sequence != 0 && (tick < state.captured.tick || server_seconds < state.captured.server_seconds)))
        return {ReplicationError::InvalidTime};
    if (state.next_sequence == maximum_sequence) return {ReplicationError::SequenceExhausted};
    if (state.scene->registry().iterating() || (state.physics && state.physics->stepping())) return {ReplicationError::Busy};
    const auto offset = state.origin();
    if (!offset) return {ReplicationError::ApplyFailed};
    BusyScope scope(state.busy);
    try {
        SnapshotFrame frame{state.next_sequence, tick, server_seconds, {}};
        frame.entities.reserve(state.entities.size());
        for (auto it = state.entities.begin(); it != state.entities.end();) {
            const auto& binding = it->second.binding;
            if (!state.scene->registry().alive(binding.entity)) {
                if (it->second.source_identity && state.config.local_identity != state.config.server_identity) {
                    ++it;
                    continue;
                }
                it = state.entities.erase(it);
                continue;
            }
            ++it;
            if (!state.local_authority(binding.options)) continue;
            if (!state.root_valid(binding.entity) || state.scene->registry().persistent_id(binding.entity) != binding.id.persistent)
                return {ReplicationError::InvalidEntity};
            const auto world = state.scene->local_transform(binding.entity);
            if (!world) return {ReplicationError::InvalidEntity};
            EntitySnapshot value;
            value.id = binding.id;
            value.owner = binding.options.owner;
            value.prefab = binding.options.prefab;
            value.authority = binding.options.authority;
            value.world = *world;
            value.enabled = state.scene->registry().enabled(binding.entity);
            value.layers = state.scene->registry().layers(binding.entity);
            if (state.config.local_identity == state.config.server_identity) value.acknowledged_input = binding.acknowledged_input;
            if (const auto* body = state.physics ? state.physics->get(binding.entity) : nullptr) {
                value.world = body->pose;
                value.has_velocity = true;
                value.linear_velocity = body->linear_velocity;
                value.angular_velocity = body->angular_velocity;
                value.physics_enabled = body->enabled;
            } else if (const auto* velocity = state.scene->registry().get<ecs::VelocityComponent>(binding.entity)) {
                value.has_velocity = true;
                value.linear_velocity = velocity->linear;
                value.angular_velocity = velocity->angular;
            }
            value.world.position = value.world.position + *offset;
            if (!valid_snapshot(value)) return {ReplicationError::InvalidSnapshot};
            frame.entities.push_back(value);
        }
        state.captured = std::move(frame);
        ++state.next_sequence;
        ++state.statistics.captures;
        return {ReplicationError::None, state.captured.entities.size(), state.captured.sequence};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory};
    } catch (const std::exception&) { return {ReplicationError::ApplyFailed}; }
}
const SnapshotFrame* ReplicationService::latest_capture() const noexcept {
    return impl_->captured.sequence == 0 ? nullptr : &impl_->captured;
}
ReplicationResult ReplicationService::send(NetworkTransport& transport, PeerHandle handle, bool force_full) {
    auto& state = *impl_;
    if (state.busy) return {ReplicationError::Busy};
    if (state.transport != &transport) return {ReplicationError::InvalidConfiguration};
    const auto found = state.peers.find(handle);
    if (found == state.peers.end() || !transport.peer(handle)) return {ReplicationError::UnknownPeer};
    auto& peer = found->second;
    if (!peer.state.policy.send_snapshots) return {ReplicationError::Unauthorized};
    if (state.captured.sequence == 0) return {ReplicationError::InvalidSnapshot};
    const bool resend = force_full || peer.state.full_required;
    if (state.captured.sequence < peer.state.last_sent || (state.captured.sequence == peer.state.last_sent && !resend))
        return {ReplicationError::None, 0, peer.state.last_sent};
    BusyScope scope(state.busy);
    try {
        const bool full = force_full || peer.state.full_required;
        const auto* baseline = full ? nullptr : state.history_find(peer.sent, peer.state.acknowledged);
        ReplicationPacket packet;
        const auto delta = make_replication_delta(state.captured, baseline, packet, state.config);
        if (!delta) return delta;
        std::vector<std::byte> wire;
        const auto encoded = encode_replication(packet, wire, state.config);
        if (!encoded) return encoded;
        const bool retain = !state.history_find(peer.sent, state.captured.sequence);
        if (retain) {
            if (!state.admit_history(peer.sent, state.captured.entities.size())) return {ReplicationError::Capacity};
            peer.sent.push_back(state.captured);
            state.history_states += state.captured.entities.size();
        }
        const auto sent = transport.send(handle, state.config.channel, MessageKind::Replication, wire, DeliveryMode::ReliableOrdered);
        if (!sent) {
            if (retain) {
                state.history_states -= peer.sent.back().entities.size();
                peer.sent.pop_back();
            }
            ++state.statistics.send_failures;
            return {ReplicationError::TransportFailure};
        }
        peer.state.last_sent = state.captured.sequence;
        peer.state.full_required = false;
        peer.state.last_error = ReplicationError::None;
        if (packet.kind == ReplicationMessage::Full) ++state.statistics.full_sent;
        else ++state.statistics.deltas_sent;
        return {ReplicationError::None, packet.changes.size(), packet.sequence};
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory};
    } catch (const std::exception&) { return {ReplicationError::ApplyFailed}; }
}
ReplicationResult ReplicationService::receive(NetworkTransport& transport, double now_seconds, std::size_t message_budget) {
    auto& state = *impl_;
    if (state.busy) return {ReplicationError::Busy};
    if (state.transport && state.transport != &transport) return {ReplicationError::InvalidConfiguration};
    if (!state.scene) return {ReplicationError::NotAttached};
    if (!std::isfinite(now_seconds) || now_seconds < state.last_receive_seconds || now_seconds < 0.0)
        return {ReplicationError::InvalidTime};
    if (state.scene->registry().iterating() || (state.physics && state.physics->stepping())) return {ReplicationError::Busy};
    state.last_receive_seconds = now_seconds;
    BusyScope scope(state.busy);
    ReplicationResult result;
    message_budget = std::min(message_budget, state.config.receive_messages_per_frame);
    try {
        std::vector<PeerHandle> order;
        order.reserve(state.peers.size());
        for (const auto& [handle, record] : state.peers) {
            (void)record;
            order.push_back(handle);
        }
        if (order.empty()) return result;
        const auto start = state.peer_cursor % order.size();
        state.peer_cursor = (start + 1) % order.size();
        for (std::size_t index = 0; index < order.size(); ++index) {
            const auto handle = order[(start + index) % order.size()];
            auto& peer = state.peers.at(handle);
            const auto network_peer = transport.peer(handle);
            if (!network_peer || network_peer->failure != ProtocolError::None) {
                peer.state.last_error = ReplicationError::TransportFailure;
                result.error = ReplicationError::TransportFailure;
                continue;
            }
            const auto messages = transport.take_messages(handle,
                MessageFilter{MessageKind::Replication, state.config.channel}, std::min<std::size_t>(message_budget, 4), 4096);
            message_budget -= messages.size();
            for (const auto& message : messages) {
                ReplicationError error;
                try { error = state.consume(peer, message); }
                catch (const std::bad_alloc&) { error = ReplicationError::OutOfMemory; }
                catch (const std::exception&) { error = ReplicationError::ApplyFailed; }
                peer.state.last_error = error;
                if (error != ReplicationError::None) {
                    ++state.statistics.rejected_messages;
                    result.error = error;
                    if (peer.state.policy.accept_snapshots && error != ReplicationError::Unauthorized &&
                        error != ReplicationError::Malformed && error != ReplicationError::UnsupportedVersion)
                        peer.state.resync_pending = true;
                } else ++result.affected;
            }
            const auto control_error = state.control(transport, peer, now_seconds);
            if (control_error != ReplicationError::None) result.error = control_error;
        }
        return result;
    } catch (const std::bad_alloc&) { return {ReplicationError::OutOfMemory, result.affected};
    } catch (const std::exception&) { return {ReplicationError::ApplyFailed, result.affected}; }
}
ReplicationResult ReplicationService::request_resync(PeerHandle handle) {
    if (impl_->busy) return {ReplicationError::Busy};
    const auto found = impl_->peers.find(handle);
    if (found == impl_->peers.end()) return {ReplicationError::UnknownPeer};
    if (!found->second.state.policy.accept_snapshots) return {ReplicationError::Unauthorized};
    found->second.state.resync_pending = true;
    return {};
}
const SnapshotFrame* ReplicationService::latest_received(PeerHandle handle) const noexcept {
    const auto found = impl_->peers.find(handle);
    return found == impl_->peers.end() || found->second.received.empty() ? nullptr : &found->second.received.back();
}
SnapshotHistory ReplicationService::received_since(PeerHandle handle, std::uint64_t sequence, std::size_t limit) const {
    SnapshotHistory result;
    const auto found = impl_->peers.find(handle);
    if (found == impl_->peers.end()) return result;
    result.latest_sequence = found->second.state.last_received;
    const auto& frames = found->second.received;
    result.history_lost = sequence != 0 && result.latest_sequence > sequence &&
        (frames.empty() || frames.front().sequence > sequence);
    limit = std::min(limit, impl_->config.history_frames);
    for (const auto& frame : frames) {
        if (result.frames.size() >= limit) break;
        if (frame.sequence > sequence) result.frames.push_back(frame);
    }
    return result;
}
std::optional<InterpolationSample> ReplicationService::sample(NetworkEntityId id, double remote_seconds) const {
    if (!std::isfinite(remote_seconds) || remote_seconds < 0.0) return std::nullopt;
    const auto found = impl_->entities.find(id);
    if (found == impl_->entities.end() || found->second.samples.empty()) return std::nullopt;
    const auto& samples = found->second.samples;
    const auto upper = std::lower_bound(samples.begin(), samples.end(), remote_seconds,
        [](const Impl::TimedState& value, double seconds) { return value.seconds < seconds; });
    InterpolationSample result;
    if (upper == samples.begin()) {
        result.state = upper->state;
        result.lower_tick = upper->tick;
        result.upper_tick = upper->tick;
        result.clamped = remote_seconds < upper->seconds;
        return result;
    }
    if (upper == samples.end()) {
        const auto& last = samples.back();
        result.state = last.state;
        result.lower_tick = last.tick;
        result.upper_tick = last.tick;
        const auto elapsed = remote_seconds - last.seconds;
        const auto used = std::min(elapsed, impl_->config.max_extrapolation_seconds);
        if (last.state.has_velocity && last.state.enabled && used > 0.0) {
            result.state.world.position = last.state.world.position + last.state.linear_velocity * used;
            result.extrapolated = true;
        }
        result.clamped = elapsed > impl_->config.max_extrapolation_seconds || !last.state.has_velocity;
        return valid_snapshot(result.state) ? std::optional<InterpolationSample>{result} : std::nullopt;
    }
    const auto lower = std::prev(upper);
    const auto span = upper->seconds - lower->seconds;
    result.fraction = span > 0.0 ? std::clamp((remote_seconds - lower->seconds) / span, 0.0, 1.0) : 1.0;
    result.lower_tick = lower->tick;
    result.upper_tick = upper->tick;
    result.state = result.fraction >= 1.0 ? upper->state : lower->state;
    result.state.world = math::interpolate(lower->state.world, upper->state.world, result.fraction);
    if (lower->state.has_velocity && upper->state.has_velocity) {
        result.state.linear_velocity = math::lerp(lower->state.linear_velocity, upper->state.linear_velocity, result.fraction);
        result.state.angular_velocity = math::lerp(lower->state.angular_velocity, upper->state.angular_velocity, result.fraction);
    }
    return valid_snapshot(result.state) ? std::optional<InterpolationSample>{result} : std::nullopt;
}
std::optional<math::Transform> ReplicationService::runtime_transform(const EntitySnapshot& state) const noexcept {
    if (!valid_snapshot(state)) return std::nullopt;
    const auto origin = impl_->origin();
    if (!origin) return std::nullopt;
    auto transform = state.world;
    transform.position = transform.position - *origin;
    return bounded(transform.position, 1.0e12) ? std::optional<math::Transform>{transform} : std::nullopt;
}
ReplicationStatistics ReplicationService::statistics() const noexcept {
    auto result = impl_->statistics;
    result.bindings = impl_->entities.size();
    result.retained_identities = impl_->floors.size();
    result.peers = impl_->peers.size();
    result.history_states = impl_->history_states;
    return result;
}

namespace {
ReplicationResult attach_services(core::Kernel& kernel) {
    auto* scene = kernel.services().find<scene::Scene>();
    if (!scene) return {ReplicationError::NotAttached};
    return kernel.services().get<ReplicationService>().attach(*scene,
        kernel.services().find<physics::PhysicsWorld>(), kernel.services().find<scene::WorldPartition>());
}
core::CommandResult command_result(ReplicationResult result) {
    return {static_cast<bool>(result), std::string(replication_error_name(result.error)) +
        "; affected=" + std::to_string(result.affected) + "; sequence=" + std::to_string(result.sequence)};
}
}
void register_replication(core::Kernel& kernel) {
    kernel.register_module({"network.replication", "Authoritative entity snapshots, acknowledged deltas and retained identity floors", 1});
    ReplicationConfig config;
    const auto local = kernel.config().integer("network.replication.local_identity", 1);
    const auto server = kernel.config().integer("network.replication.server_identity", 1);
    const auto world = kernel.config().integer("network.replication.world", 1);
    const auto channel = kernel.config().integer("network.replication.channel", 2);
    if (local <= 0 || server <= 0 || world <= 0 || channel < 0 || channel > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("replication identity or channel configuration is invalid");
    config.local_identity.value = static_cast<std::uint64_t>(local);
    config.server_identity.value = static_cast<std::uint64_t>(server);
    config.world.value = static_cast<std::uint64_t>(world);
    config.channel = static_cast<std::uint16_t>(channel);
    config.automatic_send = kernel.config().boolean("network.replication.automatic_send", true);
    kernel.services().emplace<ReplicationService>(config);
    kernel.add_system({"network.replication.receive", core::Phase::PreSimulation, -8400, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto* transport = host.services().find<NetworkTransport>();
            if (!transport || !attach_services(host)) return;
            auto& replication = host.services().get<ReplicationService>();
            const auto received = replication.receive(*transport, frame.elapsed_seconds, replication.config().receive_messages_per_frame);
            if (!received && received.error != ReplicationError::BaselineMissing && received.error != ReplicationError::BaselineMismatch)
                host.diagnose(core::Diagnostic::Severity::Warning, "network.replication", std::string(replication_error_name(received.error)));
            const auto stats = replication.statistics();
            host.metric("network.replication.entities", static_cast<double>(stats.bindings));
            host.metric("network.replication.history_states", static_cast<double>(stats.history_states));
            host.metric("network.replication.rejected", static_cast<double>(stats.rejected_messages));
        });
    kernel.add_system({"network.replication.capture", core::Phase::PostSimulation, 10000, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& replication = host.services().get<ReplicationService>();
            auto* transport = host.services().find<NetworkTransport>();
            if (!transport || !replication.config().automatic_send || frame.tick % replication.config().send_every_ticks != 0 ||
                !attach_services(host)) return;
            const auto peers = replication.peers();
            if (peers.empty()) return;
            const auto captured = replication.capture(frame.tick, frame.elapsed_seconds);
            if (!captured) return;
            for (const auto& peer : peers) {
                if (!peer.policy.send_snapshots) continue;
                const auto sent = replication.send(*transport, peer.peer);
                if (!sent && sent.error != ReplicationError::TransportFailure)
                    host.diagnose(core::Diagnostic::Severity::Warning, "network.replication", std::string(replication_error_name(sent.error)));
            }
        });
    kernel.add_command("network.replication.attach", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) return core::CommandResult{false, "usage: network.replication.attach"};
        return command_result(attach_services(host));
    }, "Attach the shared Scene, PhysicsWorld and absolute origin provider");
    kernel.add_command("network.replication.bind", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 3 || args.size() > 5) return core::CommandResult{false,
            "usage: network.replication.bind entity_index:generation persistent_id incarnation [owner_id] [server|owner]"};
        const auto entity = ecs::parse_entity(args[0]);
        const auto persistent = parse_number(args[1]);
        const auto incarnation = parse_number(args[2]);
        const auto owner = args.size() >= 4 ? parse_number(args[3]) : std::optional<std::uint64_t>{0};
        if (!entity || !persistent || !incarnation || !owner ||
            (args.size() == 5 && args[4] != "server" && args[4] != "owner")) return core::CommandResult{false, "invalid binding arguments"};
        const auto attached = attach_services(host);
        if (!attached) return command_result(attached);
        ReplicationBindingOptions options;
        options.owner.value = *owner;
        options.authority = args.size() == 5 && args[4] == "owner" ? ReplicationAuthority::Owner : ReplicationAuthority::Server;
        return command_result(host.services().get<ReplicationService>().bind(*entity, {{*persistent}, *incarnation}, options));
    }, "Bind a real Scene root to an authority-issued network incarnation");
    kernel.add_command("network.replication.peer", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 3) return core::CommandResult{false,
            "usage: network.replication.peer peer_index:generation identity send|server|owner"};
        const auto peer = ecs::parse_entity(args[0]);
        const auto identity = parse_number(args[1]);
        auto* transport = host.services().find<NetworkTransport>();
        if (!peer || !identity || !transport || (args[2] != "send" && args[2] != "server" && args[2] != "owner"))
            return core::CommandResult{false, "invalid peer policy or unavailable transport"};
        ReplicationPeerPolicy policy;
        policy.identity.value = *identity;
        policy.send_snapshots = args[2] != "server";
        policy.accept_snapshots = args[2] != "send";
        policy.allow_spawns = args[2] == "server";
        return command_result(host.services().get<ReplicationService>().add_peer(*transport, PeerHandle{*peer}, policy));
    }, "Explicitly authorize a connected peer identity; this performs no authentication");
    kernel.add_command("network.replication.capture", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) return core::CommandResult{false, "usage: network.replication.capture"};
        const auto attached = attach_services(host);
        if (!attached) return command_result(attached);
        return command_result(host.services().get<ReplicationService>().capture(host.frame().tick, host.frame().elapsed_seconds));
    }, "Capture real bound transforms, physics velocities and enablement");
    kernel.add_command("network.replication.send", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 2) return core::CommandResult{false,
            "usage: network.replication.send peer_index:generation [full]"};
        const auto peer = ecs::parse_entity(args[0]);
        auto* transport = host.services().find<NetworkTransport>();
        if (!peer || !transport || (args.size() == 2 && args[1] != "full")) return core::CommandResult{false, "invalid peer or unavailable transport"};
        return command_result(host.services().get<ReplicationService>().send(*transport, PeerHandle{*peer}, args.size() == 2));
    }, "Queue the captured frame as a full snapshot or acknowledged delta");
    kernel.add_command("network.replication.resync", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) return core::CommandResult{false, "usage: network.replication.resync peer_index:generation"};
        const auto peer = ecs::parse_entity(args[0]);
        if (!peer) return core::CommandResult{false, "invalid peer"};
        return command_result(host.services().get<ReplicationService>().request_resync(PeerHandle{*peer}));
    }, "Request a complete baseline from an authorized authority");
    kernel.add_command("network.replication.inspect", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) return core::CommandResult{false, "usage: network.replication.inspect"};
        const auto& replication = host.services().get<ReplicationService>();
        const auto stats = replication.statistics();
        std::string text = "bindings=" + std::to_string(stats.bindings) + " peers=" + std::to_string(stats.peers) +
            " retained_identities=" + std::to_string(stats.retained_identities) + " history_states=" + std::to_string(stats.history_states) +
            " received=" + std::to_string(stats.frames_received) + " rejected=" + std::to_string(stats.rejected_messages);
        for (const auto& binding : replication.bindings()) text += "\n" + std::to_string(binding.id.persistent.value) + ":" +
            std::to_string(binding.id.incarnation) + " -> " + ecs::format_entity(binding.entity) +
            " owner=" + std::to_string(binding.options.owner.value) + (binding.options.prediction_owned_pose ? " prediction" : " replication");
        return core::CommandResult{true, std::move(text)};
    }, "Inspect actual entity bindings, retained state and authority ownership");
}

} // namespace ge::network
