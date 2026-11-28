#include <ge/network/prediction.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/input/actions.hpp>
#include <ge/physics/character.hpp>
#include <ge/physics/vehicle.hpp>
#include <ge/scene/partition.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ge::network {
namespace {
constexpr std::size_t batch_header_bytes = 60;
constexpr std::size_t input_wire_bytes = 52;
constexpr std::uint16_t valid_buttons = 31;
constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint16_t button(PredictionButton value) noexcept {
    return static_cast<std::uint16_t>(value);
}
bool pressed(const PredictionControl& value, PredictionButton flag) noexcept {
    return (value.buttons & button(flag)) != 0;
}
bool finite_range(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}
bool same_metadata(const EntitySnapshot& a, const EntitySnapshot& b) noexcept {
    return a.id == b.id && a.owner == b.owner && a.prefab == b.prefab &&
        a.authority == b.authority && a.layers == b.layers &&
        a.enabled == b.enabled && a.physics_enabled == b.physics_enabled &&
        a.acknowledged_input == b.acknowledged_input;
}
PredictionError valid_batch(const PredictionBatch& batch, const PredictionConfig& config) noexcept {
    if (!batch.world || !batch.sender || !batch.entity || batch.model_signature == 0 ||
        !finite_range(batch.fixed_seconds, 0.001, 0.1) ||
        std::abs(batch.fixed_seconds - config.fixed_seconds) > 1.0e-12)
        return PredictionError::ModelMismatch;
    if (batch.inputs.empty() || batch.inputs.size() > config.batch_inputs)
        return PredictionError::Capacity;
    std::uint64_t previous{};
    for (const auto& input : batch.inputs) {
        if (input.sequence == 0 || !valid_prediction_control(input.control))
            return PredictionError::InvalidInput;
        if (previous != 0 && (previous == maximum_sequence || input.sequence != previous + 1))
            return PredictionError::SequenceWindow;
        previous = input.sequence;
    }
    return PredictionError::None;
}
template<class Number>
std::optional<Number> parse(std::string_view text) {
    Number value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}
std::optional<NetworkEntityId> parse_id(core::CommandArgs args) {
    if (args.size() < 2) return std::nullopt;
    const auto persistent = parse<std::uint64_t>(args[0]);
    const auto incarnation = parse<std::uint64_t>(args[1]);
    if (!persistent || !incarnation || *persistent == 0 || *incarnation == 0) return std::nullopt;
    return NetworkEntityId{ge::PersistentId{*persistent}, *incarnation};
}
core::CommandResult command_result(PredictionResult result) {
    return {static_cast<bool>(result), std::string(prediction_error_name(result.error)) +
        "; affected=" + std::to_string(result.affected) + "; sequence=" + std::to_string(result.sequence)};
}
struct BusyGuard {
    bool& flag;
    explicit BusyGuard(bool& target) : flag(target) { flag = true; }
    ~BusyGuard() { flag = false; }
};
}

std::string_view prediction_error_name(PredictionError error) noexcept {
    switch (error) {
    case PredictionError::None: return "none";
    case PredictionError::InvalidConfiguration: return "invalid configuration";
    case PredictionError::NotAttached: return "services not attached";
    case PredictionError::InvalidIdentity: return "invalid entity identity";
    case PredictionError::InvalidInput: return "invalid input";
    case PredictionError::InvalidTime: return "invalid time or duplicate frame tick";
    case PredictionError::Capacity: return "bounded input capacity exhausted";
    case PredictionError::UnknownBinding: return "unknown prediction binding";
    case PredictionError::AlreadyBound: return "identity already bound";
    case PredictionError::RetiredIdentity: return "input stream identity retired";
    case PredictionError::Unauthorized: return "input authority rejected";
    case PredictionError::AuthorityConflict: return "character or vehicle owns control";
    case PredictionError::InvalidBody: return "requires a kinematic body with zero centre of mass";
    case PredictionError::UnknownPeer: return "unknown trusted replication peer";
    case PredictionError::WrongRole: return "operation does not match client or server role";
    case PredictionError::Malformed: return "malformed input batch";
    case PredictionError::UnsupportedVersion: return "unsupported prediction schema";
    case PredictionError::ModelMismatch: return "fixed step or model signature differs";
    case PredictionError::SequenceExhausted: return "input sequence exhausted";
    case PredictionError::SequenceWindow: return "input outside bounded sequence window";
    case PredictionError::DuplicateConflict: return "conflicting duplicate input";
    case PredictionError::StaleAuthority: return "stale authoritative snapshot or ACK";
    case PredictionError::FutureAcknowledgement: return "authority acknowledged ungenerated input";
    case PredictionError::MissingAcknowledgement: return "authority removed an established input ACK";
    case PredictionError::HistoryMissing: return "acknowledged input state missing";
    case PredictionError::ReplayBudget: return "replay budget exhausted; awaiting newer authority";
    case PredictionError::ModelFailed: return "prediction model failed";
    case PredictionError::PublishFailed: return "body publication failed";
    case PredictionError::TransportFailure: return "transport did not admit input batch";
    case PredictionError::Busy: return "prediction callback reentry rejected";
    case PredictionError::OutOfMemory: return "prediction allocation failed";
    }
    return "unknown prediction error";
}

PredictionError validate_prediction_config(const PredictionConfig& value) noexcept {
    if (value.max_bindings == 0 || value.max_bindings > 1024 ||
        value.max_retired_identities < value.max_bindings || value.max_retired_identities > 65536 ||
        value.history_inputs == 0 || value.history_inputs > 4096 ||
        value.server_queue_inputs == 0 || value.server_queue_inputs > 4096 ||
        value.max_bindings > 262144 / value.history_inputs ||
        value.max_bindings > 262144 / value.server_queue_inputs ||
        value.batch_inputs == 0 || value.batch_inputs > 128 ||
        value.batch_inputs > value.server_queue_inputs || value.batch_inputs > value.history_inputs ||
        value.replay_steps == 0 || value.replay_steps > value.history_inputs ||
        value.steps_per_frame == 0 || value.steps_per_frame > 128 ||
        value.messages_per_frame == 0 || value.messages_per_frame > 1024 ||
        value.receive_scan_limit == 0 || value.receive_scan_limit > 65536 ||
        value.snapshot_frames_per_poll == 0 || value.snapshot_frames_per_poll > 256 ||
        value.max_message_bytes < batch_header_bytes + value.batch_inputs * input_wire_bytes ||
        value.max_message_bytes > 1024U * 1024U)
        return PredictionError::InvalidConfiguration;
    if (!finite_range(value.fixed_seconds, 0.001, 0.1) ||
        !finite_range(value.maximum_frame_seconds, value.fixed_seconds, 1.0) ||
        !finite_range(value.retry_seconds, 0.01, 60.0) ||
        !finite_range(value.smoothing_half_life, 0.0, 10.0) ||
        !finite_range(value.smoothing_distance_limit, 0.0, 10000.0) ||
        !finite_range(value.position_tolerance, 0.0, 1.0) ||
        !finite_range(value.velocity_tolerance, 0.0, 1.0) ||
        !finite_range(value.rotation_tolerance, 0.0, math::pi) ||
        !finite_range(value.look_sensitivity, 0.0, 1.0))
        return PredictionError::InvalidConfiguration;
    return PredictionError::None;
}
bool valid_prediction_control(const PredictionControl& value) noexcept {
    return math::finite(value.movement) && math::length_squared(value.movement) <= 1.000001 &&
        finite_range(value.turn, -1.0, 1.0) && finite_range(value.lift, -1.0, 1.0) &&
        (value.buttons & static_cast<std::uint16_t>(~valid_buttons)) == 0;
}
bool valid_kinematic_prediction(const KinematicPrediction& value) noexcept {
    return finite_range(value.speed, 0.0, 1000.0) &&
        finite_range(value.acceleration, 0.0, 10000.0) &&
        finite_range(value.braking, 0.0, 10000.0) &&
        finite_range(value.turn_speed, 0.0, 100.0) &&
        finite_range(value.lift_speed, 0.0, 1000.0) &&
        finite_range(value.sprint_multiplier, 1.0, 10.0) &&
        finite_range(value.crouch_multiplier, 0.0, 1.0);
}
PredictionResult predict_kinematic(const EntitySnapshot& before, const PredictionInput& input,
    double fixed_seconds, const KinematicPrediction& model, EntitySnapshot& after) noexcept {
    if (!valid_snapshot(before) || input.sequence == 0 || !valid_prediction_control(input.control))
        return {PredictionError::InvalidInput};
    if (!valid_kinematic_prediction(model) || !finite_range(fixed_seconds, 0.001, 0.1))
        return {PredictionError::InvalidConfiguration};
    auto next = before;
    next.has_velocity = true;
    if (!before.enabled || !before.physics_enabled) {
        next.linear_velocity = {};
        next.angular_velocity = {};
        after = next;
        return {PredictionError::None, 1, input.sequence};
    }
    const auto& controls = input.control;
    double speed = model.speed;
    if (pressed(controls, PredictionButton::Sprint)) speed *= model.sprint_multiplier;
    if (pressed(controls, PredictionButton::Crouch)) speed *= model.crouch_multiplier;
    const double yaw_speed = -controls.turn * model.turn_speed;
    next.angular_velocity = {0.0, yaw_speed, 0.0};
    next.world.rotation = math::normalized(math::from_axis_angle({0.0, 1.0, 0.0},
        yaw_speed * fixed_seconds) * before.world.rotation);
    auto direction = math::rotate(next.world.rotation, {controls.movement.x, 0.0, -controls.movement.y});
    direction.y = 0.0;
    direction = math::clamp_length(direction, 1.0);
    auto desired = direction * speed + math::Vec3{0.0, controls.lift * model.lift_speed, 0.0};
    if (pressed(controls, PredictionButton::Brake)) desired = {};
    const double acceleration = math::length_squared(desired) > math::epsilon ? model.acceleration : model.braking;
    next.linear_velocity = math::move_towards(before.linear_velocity, desired, acceleration * fixed_seconds);
    next.world.position += next.linear_velocity * fixed_seconds;
    if (!valid_snapshot(next)) return {PredictionError::ModelFailed};
    after = next;
    return {PredictionError::None, 1, input.sequence};
}
std::uint64_t prediction_model_signature(const KinematicPrediction& model, double fixed_seconds) {
    core::ByteWriter writer(128, core::ByteOrder::big);
    writer.write_bytes("GEKINEMATIC01");
    writer.write_f64(fixed_seconds);
    writer.write_f64(model.speed);
    writer.write_f64(model.acceleration);
    writer.write_f64(model.braking);
    writer.write_f64(model.turn_speed);
    writer.write_f64(model.lift_speed);
    writer.write_f64(model.sprint_multiplier);
    writer.write_f64(model.crouch_multiplier);
    const auto signature = core::fnv1a64(writer.bytes());
    return signature == 0 ? 1 : signature;
}

PredictionResult encode_prediction(const PredictionBatch& batch,
    std::vector<std::byte>& output, const PredictionConfig& limits) {
    if (const auto error = validate_prediction_config(limits); error != PredictionError::None) return {error};
    if (const auto error = valid_batch(batch, limits); error != PredictionError::None) return {error};
    try {
        core::ByteWriter writer(limits.max_message_bytes, core::ByteOrder::big);
        writer.write_bytes("GEPRED01");
        writer.write_u16(prediction_wire_version);
        writer.write_u16(static_cast<std::uint16_t>(batch.inputs.size()));
        writer.write_u64(batch.world.value);
        writer.write_u64(batch.sender.value);
        writer.write_u64(batch.entity.persistent.value);
        writer.write_u64(batch.entity.incarnation);
        writer.write_u64(batch.model_signature);
        writer.write_f64(batch.fixed_seconds);
        for (const auto& input : batch.inputs) {
            writer.write_u64(input.sequence);
            writer.write_u64(input.client_tick);
            writer.write_f64(input.control.movement.x);
            writer.write_f64(input.control.movement.y);
            writer.write_f64(input.control.turn);
            writer.write_f64(input.control.lift);
            writer.write_u16(input.control.buttons);
            writer.write_u16(0);
        }
        output = writer.take();
        return {PredictionError::None, batch.inputs.size(), batch.inputs.back().sequence};
    } catch (const std::bad_alloc&) {
        return {PredictionError::OutOfMemory};
    } catch (const core::BinaryException&) {
        return {PredictionError::Malformed};
    }
}
PredictionResult decode_prediction(std::span<const std::byte> bytes,
    PredictionBatch& output, const PredictionConfig& limits) {
    if (const auto error = validate_prediction_config(limits); error != PredictionError::None) return {error};
    if (bytes.size() > limits.max_message_bytes || bytes.size() < batch_header_bytes)
        return {PredictionError::Malformed};
    try {
        core::BinaryLimits binary_limits;
        binary_limits.max_total_bytes = limits.max_message_bytes;
        core::ByteReader reader(bytes, core::ByteOrder::big, binary_limits);
        reader.expect("GEPRED01");
        if (reader.read_u16() != prediction_wire_version) return {PredictionError::UnsupportedVersion};
        const auto count = static_cast<std::size_t>(reader.read_u16());
        if (count == 0 || count > limits.batch_inputs || bytes.size() != batch_header_bytes + count * input_wire_bytes)
            return {PredictionError::Malformed};
        PredictionBatch next;
        next.world.value = reader.read_u64();
        next.sender.value = reader.read_u64();
        next.entity.persistent.value = reader.read_u64();
        next.entity.incarnation = reader.read_u64();
        next.model_signature = reader.read_u64();
        next.fixed_seconds = reader.read_f64();
        next.inputs.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            PredictionInput input;
            input.sequence = reader.read_u64();
            input.client_tick = reader.read_u64();
            input.control.movement.x = reader.read_f64();
            input.control.movement.y = reader.read_f64();
            input.control.turn = reader.read_f64();
            input.control.lift = reader.read_f64();
            input.control.buttons = reader.read_u16();
            if (reader.read_u16() != 0) return {PredictionError::Malformed};
            next.inputs.push_back(input);
        }
        reader.require_end();
        if (const auto error = valid_batch(next, limits); error != PredictionError::None) return {error};
        output = std::move(next);
        return {PredictionError::None, count, output.inputs.back().sequence};
    } catch (const std::bad_alloc&) {
        return {PredictionError::OutOfMemory};
    } catch (const core::BinaryException&) {
        return {PredictionError::Malformed};
    }
}

struct PredictionService::Impl {
    struct Record {
        PredictionState state;
        PredictionBindingOptions options;
        physics::BodyHandle body{};
        std::uint64_t signature{};
        EntitySnapshot base{};
        PredictionControl control{};
        std::deque<PredictionHistoryEntry> history;
        std::map<std::uint64_t, PredictionInput> queued;
        std::optional<std::uint64_t> pending_ack;
        double last_retry_seconds{};
        bool previous_pose_owner{};
    };
    PredictionConfig config;
    ReplicationService* replication{};
    physics::PhysicsWorld* physics{};
    physics::CharacterSystem* characters{};
    physics::VehicleSystem* vehicles{};
    std::map<NetworkEntityId, Record> records;
    std::set<NetworkEntityId> used_identities;
    std::optional<NetworkEntityId> selected;
    PredictionStatistics stats;
    std::optional<std::uint64_t> frame_tick;
    double accumulator{};
    double elapsed_seconds{};
    std::size_t receive_cursor{};
    std::size_t send_cursor{};
    bool busy{};

    explicit Impl(PredictionConfig value) : config(value) {}
    PredictionError permission(const Record& record) const {
        if (!replication || !physics) return PredictionError::NotAttached;
        if (physics->stepping()) return PredictionError::Busy;
        const auto binding = replication->binding(record.state.id);
        if (!binding || binding->entity != record.state.entity || !physics->registry().alive(binding->entity) ||
            physics->registry().persistent_id(binding->entity) != record.state.id.persistent)
            return PredictionError::InvalidIdentity;
        if (physics->registry().has<physics::CharacterComponent>(binding->entity) ||
            physics->registry().has<physics::VehicleComponent>(binding->entity))
            return PredictionError::AuthorityConflict;
        const auto* body = physics->get(record.body);
        if (!body || body->entity != binding->entity || body->mode != physics::BodyMode::Kinematic ||
            math::length_squared(body->mass.center_of_mass) > 1.0e-16)
            return PredictionError::InvalidBody;
        if (binding->options.authority != ReplicationAuthority::Server ||
            binding->options.owner != record.state.predicted.owner)
            return PredictionError::Unauthorized;
        const auto peer = replication->peer(record.state.peer);
        if (!peer) return PredictionError::UnknownPeer;
        const auto& identity = replication->config();
        if (record.state.role == PredictionRole::Client) {
            if (identity.local_identity == identity.server_identity ||
                binding->options.owner != identity.local_identity ||
                peer->policy.identity != identity.server_identity || !peer->policy.accept_snapshots ||
                (binding->source_peer && binding->source_peer != record.state.peer) ||
                !binding->options.prediction_owned_pose)
                return PredictionError::Unauthorized;
        } else if (identity.local_identity != identity.server_identity ||
            peer->policy.identity != binding->options.owner || !peer->policy.send_snapshots) {
            return PredictionError::Unauthorized;
        }
        return PredictionError::None;
    }
    PredictionResult publish(Record& record, const EntitySnapshot& state, bool hold) {
        if (const auto error = permission(record); error != PredictionError::None) return {error};
        const auto transform = replication->runtime_transform(state);
        if (!transform || !valid_snapshot(state) ||
            math::length(state.linear_velocity) > physics->config().max_linear_speed ||
            math::length(state.angular_velocity) > physics->config().max_angular_speed) {
            ++stats.publication_failures;
            return {PredictionError::PublishFailed};
        }
        const auto previous = physics->state(record.body);
        if (!previous) return {PredictionError::InvalidBody};
        if (!physics->set_pose(record.body, *transform) ||
            !physics->set_velocity(record.body, state.linear_velocity, state.angular_velocity) ||
            (hold && !physics->set_kinematic_target(record.body, *transform))) {
            (void)physics->set_pose(record.body, previous->pose);
            (void)physics->set_velocity(record.body, previous->linear_velocity, previous->angular_velocity);
            ++stats.publication_failures;
            return {PredictionError::PublishFailed};
        }
        return {PredictionError::None, 1};
    }
    PredictionResult simulate(const Record& record, const EntitySnapshot& before,
        const PredictionInput& input, EntitySnapshot& after) {
        PredictionResult result;
        try {
            result = record.options.adapter.step ?
                record.options.adapter.step(before, input, config.fixed_seconds, after) :
                predict_kinematic(before, input, config.fixed_seconds, record.options.kinematic, after);
        } catch (...) {
            result.error = PredictionError::ModelFailed;
        }
        if (!result || !valid_snapshot(after) || !same_metadata(before, after) ||
            !math::almost_equal(before.world.scale, after.world.scale)) {
            ++stats.model_failures;
            return {PredictionError::ModelFailed};
        }
        return {PredictionError::None, 1, input.sequence};
    }
    PredictionResult pause(Record& record, PredictionError error) {
        record.state.last_error = error;
        if (record.state.status != PredictionStatus::AwaitingAuthority) {
            record.state.status = PredictionStatus::AwaitingAuthority;
            if (record.state.role == PredictionRole::Client && replication->request_resync(record.state.peer))
                ++stats.resync_requests;
        }
        return {error};
    }
    void suspend(Record& record, PredictionError error) {
        record.state.status = PredictionStatus::Suspended;
        record.state.last_error = error;
        record.control = {};
        if (record.state.role == PredictionRole::Client)
            (void)replication->set_prediction_owned(record.state.id, record.previous_pose_owner);
        if (selected == record.state.id) selected.reset();
    }
    PredictionState snapshot(const Record& record) const {
        auto result = record.state;
        result.pending_inputs = record.history.size();
        result.queued_inputs = record.queued.size();
        result.unsent_inputs = static_cast<std::size_t>(std::count_if(record.history.begin(), record.history.end(),
            [](const PredictionHistoryEntry& entry) { return !entry.sent; }));
        return result;
    }
};

PredictionService::PredictionService(PredictionConfig configuration) {
    if (validate_prediction_config(configuration) != PredictionError::None)
        throw std::invalid_argument("invalid prediction configuration");
    impl_ = std::make_unique<Impl>(configuration);
}
PredictionService::~PredictionService() = default;
const PredictionConfig& PredictionService::config() const noexcept { return impl_->config; }
PredictionResult PredictionService::attach(ReplicationService& replication, physics::PhysicsWorld& world,
    physics::CharacterSystem* characters, physics::VehicleSystem* vehicles) {
    if (impl_->busy || world.stepping()) return {PredictionError::Busy};
    if ((!impl_->records.empty() && (impl_->replication != &replication || impl_->physics != &world)) ||
        impl_->config.channel == replication.config().channel)
        return {PredictionError::InvalidConfiguration};
    impl_->replication = &replication;
    impl_->physics = &world;
    impl_->characters = characters;
    impl_->vehicles = vehicles;
    return {};
}
PredictionResult PredictionService::bind(NetworkEntityId id, PredictionBindingOptions options) {
    if (impl_->busy) return {PredictionError::Busy};
    if (!impl_->replication || !impl_->physics) return {PredictionError::NotAttached};
    if (!id || !options.peer) return {PredictionError::InvalidIdentity};
    if (impl_->records.contains(id)) return {PredictionError::AlreadyBound};
    if (impl_->used_identities.contains(id)) return {PredictionError::RetiredIdentity};
    if (impl_->records.size() >= impl_->config.max_bindings ||
        impl_->used_identities.size() >= impl_->config.max_retired_identities)
        return {PredictionError::Capacity};
    if ((options.role != PredictionRole::Client && options.role != PredictionRole::Server) ||
        !valid_kinematic_prediction(options.kinematic) ||
        (static_cast<bool>(options.adapter.step) != (options.adapter.signature != 0)))
        return {PredictionError::InvalidConfiguration};
    const auto binding = impl_->replication->binding(id);
    if (!binding) return {PredictionError::UnknownBinding};
    const auto* body = impl_->physics->get(binding->entity);
    if (!body) return {PredictionError::InvalidBody};
    Impl::Record record;
    record.state.id = id;
    record.state.entity = binding->entity;
    record.state.peer = options.peer;
    record.state.role = options.role;
    record.body = body->handle;
    record.previous_pose_owner = binding->options.prediction_owned_pose;
    auto& initial = record.state.predicted;
    initial.id = id;
    initial.owner = binding->options.owner;
    initial.prefab = binding->options.prefab;
    initial.authority = binding->options.authority;
    initial.layers = impl_->physics->registry().layers(binding->entity);
    initial.enabled = impl_->physics->registry().enabled(binding->entity);
    initial.physics_enabled = body->enabled;
    initial.acknowledged_input = binding->acknowledged_input;
    // The zero-position probe uses replication's own consistent origin provider.
    const auto origin_probe = impl_->replication->runtime_transform(initial);
    if (!origin_probe) return {PredictionError::NotAttached};
    initial.world = body->pose;
    initial.world.position -= origin_probe->position;
    initial.has_velocity = true;
    initial.linear_velocity = body->linear_velocity;
    initial.angular_velocity = body->angular_velocity;
    if (!valid_snapshot(initial)) return {PredictionError::InvalidBody};
    record.base = initial;
    record.state.acknowledged_input = initial.acknowledged_input;
    record.state.last_generated = initial.acknowledged_input.value_or(0);
    record.state.last_applied = record.state.last_generated;
    try {
        record.signature = options.adapter.step ? options.adapter.signature :
            prediction_model_signature(options.kinematic, impl_->config.fixed_seconds);
        record.options = std::move(options);
        if (record.state.role == PredictionRole::Client &&
            !impl_->replication->set_prediction_owned(id, true)) return {PredictionError::Unauthorized};
        if (const auto error = impl_->permission(record); error != PredictionError::None) {
            if (record.state.role == PredictionRole::Client)
                (void)impl_->replication->set_prediction_owned(id, record.previous_pose_owner);
            return {error};
        }
        impl_->used_identities.insert(id);
        try {
            impl_->records.emplace(id, std::move(record));
        } catch (...) {
            impl_->used_identities.erase(id);
            throw;
        }
        return {PredictionError::None, 1};
    } catch (const std::bad_alloc&) {
        if (binding->options.owner == impl_->replication->config().local_identity)
            (void)impl_->replication->set_prediction_owned(id, binding->options.prediction_owned_pose);
        return {PredictionError::OutOfMemory};
    }
}
PredictionResult PredictionService::unbind(NetworkEntityId id) {
    if (impl_->busy) return {PredictionError::Busy};
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end()) return {PredictionError::UnknownBinding};
    if (found->second.state.role == PredictionRole::Client)
        (void)impl_->replication->set_prediction_owned(id, found->second.previous_pose_owner);
    if (impl_->selected == id) impl_->selected.reset();
    impl_->records.erase(found);
    return {PredictionError::None, 1};
}
PredictionResult PredictionService::shutdown() {
    if (impl_->busy) return {PredictionError::Busy};
    const auto count = impl_->records.size();
    while (!impl_->records.empty()) (void)unbind(impl_->records.begin()->first);
    impl_->selected.reset();
    impl_->replication = nullptr;
    impl_->physics = nullptr;
    impl_->characters = nullptr;
    impl_->vehicles = nullptr;
    return {PredictionError::None, count};
}
PredictionResult PredictionService::select(NetworkEntityId id) {
    if (impl_->busy) return {PredictionError::Busy};
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end()) return {PredictionError::UnknownBinding};
    if (found->second.state.role != PredictionRole::Client) return {PredictionError::WrongRole};
    if ((impl_->characters && impl_->characters->player()) || (impl_->vehicles && impl_->vehicles->player()))
        return {PredictionError::AuthorityConflict};
    if (const auto error = impl_->permission(found->second); error != PredictionError::None) return {error};
    release_control();
    impl_->selected = id;
    return {PredictionError::None, 1};
}
void PredictionService::release_control() noexcept {
    if (impl_->busy) return;
    if (impl_->selected) {
        const auto found = impl_->records.find(*impl_->selected);
        if (found != impl_->records.end()) found->second.control = {};
    }
    impl_->selected.reset();
}
std::optional<NetworkEntityId> PredictionService::controlled() const noexcept { return impl_->selected; }
PredictionResult PredictionService::set_control(NetworkEntityId id, PredictionControl control) {
    if (impl_->busy) return {PredictionError::Busy};
    if (!valid_prediction_control(control)) return {PredictionError::InvalidInput};
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end()) return {PredictionError::UnknownBinding};
    auto& record = found->second;
    if (record.state.role != PredictionRole::Client) return {PredictionError::WrongRole};
    if (record.state.status == PredictionStatus::Suspended) return {record.state.last_error};
    control.buttons = static_cast<std::uint16_t>(control.buttons |
        (record.control.buttons & button(PredictionButton::JumpPressed)));
    record.control = control;
    return {PredictionError::None, 1};
}
PredictionResult PredictionService::sample_input(const input::InputSystem& input) {
    if (impl_->busy) return {PredictionError::Busy};
    if (!impl_->selected) return {};
    if ((impl_->characters && impl_->characters->player()) || (impl_->vehicles && impl_->vehicles->player())) {
        release_control();
        return {PredictionError::AuthorityConflict};
    }
    if (!input.snapshot().focused) {
        const auto found = impl_->records.find(*impl_->selected);
        if (found == impl_->records.end()) return {PredictionError::UnknownBinding};
        found->second.control = {};
        return {PredictionError::None, 1};
    }
    PredictionControl control;
    if (input.snapshot().focused) {
        const auto movement = input.vector2("move");
        const double length = math::length(movement);
        control.movement = length > 1.0 ? movement / length : movement;
        control.turn = std::clamp(input.axis("turn") + input.vector2("look").x * impl_->config.look_sensitivity, -1.0, 1.0);
        control.lift = std::clamp(input.axis("lift"), -1.0, 1.0);
        if (input.down("sprint")) control.buttons |= button(PredictionButton::Sprint);
        if (input.down("crouch")) control.buttons |= button(PredictionButton::Crouch);
        if (input.pressed("jump")) control.buttons |= button(PredictionButton::JumpPressed);
        if (input.down("jump")) control.buttons |= button(PredictionButton::JumpHeld);
        if (input.down("brake")) control.buttons |= button(PredictionButton::Brake);
    }
    return set_control(*impl_->selected, control);
}

PredictionResult PredictionService::enqueue(PeerHandle peer, const PredictionBatch& batch) {
    if (impl_->busy) return {PredictionError::Busy};
    if (!impl_->replication) return {PredictionError::NotAttached};
    if (const auto error = valid_batch(batch, impl_->config); error != PredictionError::None) return {error};
    const auto found = impl_->records.find(batch.entity);
    if (found == impl_->records.end()) return {PredictionError::UnknownBinding};
    auto& record = found->second;
    if (record.state.role != PredictionRole::Server) return {PredictionError::WrongRole};
    if (const auto error = impl_->permission(record); error != PredictionError::None) return {error};
    if (record.state.status == PredictionStatus::Suspended || peer != record.state.peer ||
        batch.world != impl_->replication->config().world || batch.sender != record.state.predicted.owner)
        return {PredictionError::Unauthorized};
    if (batch.model_signature != record.signature) return {PredictionError::ModelMismatch};
    try {
        auto queued = record.queued;
        std::size_t added{};
        std::size_t duplicates{};
        for (const auto& input : batch.inputs) {
            if (input.sequence <= record.state.last_applied) {
                ++duplicates;
                continue;
            }
            if (input.sequence - record.state.last_applied > impl_->config.server_queue_inputs)
                return {PredictionError::SequenceWindow};
            const auto existing = queued.find(input.sequence);
            if (existing != queued.end()) {
                if (existing->second != input) return {PredictionError::DuplicateConflict};
                ++duplicates;
                continue;
            }
            if (queued.size() >= impl_->config.server_queue_inputs) return {PredictionError::Capacity};
            queued.emplace(input.sequence, input);
            ++added;
        }
        record.queued.swap(queued);
        impl_->stats.duplicate_inputs += static_cast<std::uint64_t>(duplicates);
        return {PredictionError::None, added, batch.inputs.back().sequence};
    } catch (const std::bad_alloc&) {
        return {PredictionError::OutOfMemory};
    }
}
PredictionResult PredictionService::receive(NetworkTransport& transport) {
    if (impl_->busy) return {PredictionError::Busy};
    std::set<PeerHandle> unique;
    for (const auto& [id, record] : impl_->records) {
        (void)id;
        if (record.state.role == PredictionRole::Server && record.state.status != PredictionStatus::Suspended)
            unique.insert(record.state.peer);
    }
    if (unique.empty()) return {};
    const std::vector<PeerHandle> peers(unique.begin(), unique.end());
    impl_->receive_cursor %= peers.size();
    std::size_t remaining = impl_->config.messages_per_frame;
    PredictionResult result;
    const auto share = std::max<std::size_t>(1, remaining / peers.size());
    for (std::size_t offset = 0; offset < peers.size() && remaining != 0; ++offset) {
        const auto peer = peers[(impl_->receive_cursor + offset) % peers.size()];
        const auto messages = transport.take_messages(peer, MessageFilter{MessageKind::Application, impl_->config.channel},
            std::min(share, remaining), impl_->config.receive_scan_limit);
        remaining -= messages.size();
        for (const auto& message : messages) {
            PredictionBatch batch;
            auto accepted = message.delivery == DeliveryMode::ReliableOrdered ?
                decode_prediction(message.payload, batch, impl_->config) : PredictionResult{PredictionError::Malformed};
            if (accepted) accepted = enqueue(peer, batch);
            if (!accepted) {
                ++impl_->stats.rejected_messages;
                result.error = accepted.error;
            } else {
                result.affected += accepted.affected;
            }
        }
    }
    impl_->receive_cursor = (impl_->receive_cursor + 1) % peers.size();
    return result;
}
PredictionResult PredictionService::send(NetworkTransport& transport) {
    if (impl_->busy) return {PredictionError::Busy};
    if (!impl_->replication) return {PredictionError::NotAttached};
    std::vector<Impl::Record*> clients;
    for (auto& [id, record] : impl_->records) {
        (void)id;
        if (record.state.role == PredictionRole::Client && record.state.status != PredictionStatus::Suspended)
            clients.push_back(&record);
    }
    if (clients.empty()) return {};
    impl_->send_cursor %= clients.size();
    PredictionResult result;
    std::size_t batches{};
    for (std::size_t offset = 0; offset < clients.size() && batches < impl_->config.messages_per_frame; ++offset) {
        auto& record = *clients[(impl_->send_cursor + offset) % clients.size()];
        if (const auto error = impl_->permission(record); error != PredictionError::None) {
            impl_->suspend(record, error);
            result.error = error;
            continue;
        }
        auto first = std::find_if(record.history.begin(), record.history.end(),
            [](const PredictionHistoryEntry& entry) { return !entry.sent; });
        const bool retry = !record.history.empty() && record.history.front().sent &&
            impl_->elapsed_seconds - record.last_retry_seconds >= impl_->config.retry_seconds;
        if (retry) {
            first = record.history.begin();
        }
        if (first == record.history.end()) continue;
        PredictionBatch batch;
        batch.world = impl_->replication->config().world;
        batch.sender = impl_->replication->config().local_identity;
        batch.entity = record.state.id;
        batch.model_signature = record.signature;
        batch.fixed_seconds = impl_->config.fixed_seconds;
        for (auto entry = first; entry != record.history.end() && batch.inputs.size() < impl_->config.batch_inputs; ++entry) {
            if (entry->sent && !retry) break;
            batch.inputs.push_back(entry->input);
        }
        std::vector<std::byte> wire;
        const auto encoded = encode_prediction(batch, wire, impl_->config);
        ++batches;
        if (!encoded) {
            result.error = encoded.error;
            continue;
        }
        const auto sent = transport.send(record.state.peer, impl_->config.channel, MessageKind::Application,
            wire, DeliveryMode::ReliableOrdered);
        if (!sent) {
            ++impl_->stats.send_failures;
            result.error = PredictionError::TransportFailure;
            continue;
        }
        auto marked = first;
        for (std::size_t index = 0; index < batch.inputs.size(); ++index, ++marked) marked->sent = true;
        if (retry) record.last_retry_seconds = impl_->elapsed_seconds;
        ++impl_->stats.sent_batches;
        result.affected += batch.inputs.size();
        result.sequence = batch.inputs.back().sequence;
    }
    impl_->send_cursor = (impl_->send_cursor + 1) % clients.size();
    return result;
}

PredictionResult PredictionService::reconcile(NetworkEntityId id, const SnapshotFrame& frame) {
    if (impl_->busy) return {PredictionError::Busy};
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end()) return {PredictionError::UnknownBinding};
    auto& record = found->second;
    if (record.state.role != PredictionRole::Client) return {PredictionError::WrongRole};
    if (record.state.status == PredictionStatus::Suspended) return {record.state.last_error};
    if (frame.sequence == 0 || frame.sequence <= record.state.last_snapshot)
        return {PredictionError::StaleAuthority};
    if (!std::isfinite(frame.server_seconds) || frame.server_seconds < 0.0 ||
        frame.entities.size() > impl_->replication->config().max_entities)
        return {PredictionError::Malformed};
    record.state.last_snapshot = frame.sequence;
    if (const auto error = impl_->permission(record); error != PredictionError::None) {
        impl_->suspend(record, error);
        return {error};
    }
    const auto authoritative = std::find_if(frame.entities.begin(), frame.entities.end(),
        [id](const EntitySnapshot& state) { return state.id == id; });
    if (authoritative == frame.entities.end()) return impl_->pause(record, PredictionError::HistoryMissing);
    if (!valid_snapshot(*authoritative) || authoritative->owner != record.state.predicted.owner ||
        authoritative->authority != ReplicationAuthority::Server)
        return impl_->pause(record, PredictionError::Unauthorized);
    const auto ack = authoritative->acknowledged_input;
    const auto previous_ack = record.state.acknowledged_input.value_or(0);
    if (!ack && previous_ack != 0) return impl_->pause(record, PredictionError::MissingAcknowledgement);
    if (ack && *ack < previous_ack) return {PredictionError::StaleAuthority};
    if (ack && *ack > record.state.last_generated)
        return impl_->pause(record, PredictionError::FutureAcknowledgement);
    auto first_pending = record.history.begin();
    EntitySnapshot expected = record.base;
    if (ack && *ack != previous_ack) {
        const auto acknowledged = std::find_if(record.history.begin(), record.history.end(),
            [ack](const PredictionHistoryEntry& entry) { return entry.input.sequence == *ack; });
        if (acknowledged == record.history.end()) return impl_->pause(record, PredictionError::HistoryMissing);
        if (!acknowledged->sent) return impl_->pause(record, PredictionError::FutureAcknowledgement);
        expected = acknowledged->after;
        first_pending = std::next(acknowledged);
    }
    const auto replay_count = static_cast<std::size_t>(std::distance(first_pending, record.history.end()));
    if (replay_count > impl_->config.replay_steps) return impl_->pause(record, PredictionError::ReplayBudget);
    BusyGuard guard(impl_->busy);
    try {
        std::deque<PredictionHistoryEntry> replay(first_pending, record.history.end());
        auto candidate = *authoritative;
        for (auto& entry : replay) {
            EntitySnapshot after;
            const auto step = impl_->simulate(record, candidate, entry.input, after);
            if (!step) return impl_->pause(record, step.error);
            candidate = after;
            entry.after = after;
        }
        const auto published = impl_->publish(record, candidate, true);
        if (!published) return impl_->pause(record, published.error);
        record.state.position_error = math::distance(expected.world.position, authoritative->world.position);
        record.state.velocity_error = math::distance(expected.linear_velocity, authoritative->linear_velocity);
        record.state.rotation_error = math::angular_distance(expected.world.rotation, authoritative->world.rotation);
        impl_->stats.maximum_position_error = std::max(impl_->stats.maximum_position_error, record.state.position_error);
        if (record.state.position_error > impl_->config.position_tolerance ||
            record.state.velocity_error > impl_->config.velocity_tolerance ||
            record.state.rotation_error > impl_->config.rotation_tolerance)
            ++impl_->stats.corrections;
        const auto old_visual_position = record.state.predicted.world.position + record.state.visual_position_offset;
        const auto old_visual_rotation = record.state.visual_rotation_offset * record.state.predicted.world.rotation;
        record.state.visual_position_offset = old_visual_position - candidate.world.position;
        record.state.visual_rotation_offset = math::normalized(old_visual_rotation * math::conjugate(candidate.world.rotation));
        if (impl_->config.smoothing_half_life == 0.0 ||
            math::length(record.state.visual_position_offset) > impl_->config.smoothing_distance_limit) {
            record.state.visual_position_offset = {};
            record.state.visual_rotation_offset = {};
        }
        record.history.swap(replay);
        record.base = *authoritative;
        record.state.predicted = candidate;
        record.state.acknowledged_input = ack;
        record.state.status = PredictionStatus::Active;
        record.state.last_error = PredictionError::None;
        ++impl_->stats.reconciliations;
        impl_->stats.replayed_inputs += static_cast<std::uint64_t>(replay_count);
        impl_->stats.acknowledged_inputs += ack.value_or(0) - previous_ack;
        return {PredictionError::None, replay_count, ack.value_or(0)};
    } catch (const std::bad_alloc&) {
        return impl_->pause(record, PredictionError::OutOfMemory);
    }
}
PredictionResult PredictionService::poll_authority() {
    if (impl_->busy) return {PredictionError::Busy};
    if (!impl_->replication) return {PredictionError::NotAttached};
    std::map<PeerHandle, std::uint64_t> cursors;
    for (const auto& [id, record] : impl_->records) {
        (void)id;
        if (record.state.role != PredictionRole::Client || record.state.status == PredictionStatus::Suspended) continue;
        const auto [cursor, added] = cursors.emplace(record.state.peer, record.state.last_snapshot);
        if (!added) cursor->second = std::min(cursor->second, record.state.last_snapshot);
    }
    PredictionResult result;
    for (const auto& [peer, cursor] : cursors) {
        const auto history = impl_->replication->received_since(peer, cursor, impl_->config.snapshot_frames_per_poll);
        if (history.history_lost) {
            ++impl_->stats.history_losses;
            if (impl_->replication->request_resync(peer)) ++impl_->stats.resync_requests;
        }
        for (const auto& frame : history.frames) {
            for (auto& [id, record] : impl_->records) {
                if (record.state.peer != peer || record.state.role != PredictionRole::Client ||
                    record.state.status == PredictionStatus::Suspended || frame.sequence <= record.state.last_snapshot) continue;
                const auto reconciled = reconcile(id, frame);
                if (!reconciled && reconciled.error != PredictionError::StaleAuthority) result.error = reconciled.error;
                result.affected += reconciled.affected;
            }
        }
    }
    return result;
}
PredictionResult PredictionService::request_resync(NetworkEntityId id) {
    if (impl_->busy) return {PredictionError::Busy};
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end()) return {PredictionError::UnknownBinding};
    auto& record = found->second;
    if (record.state.role != PredictionRole::Client) return {PredictionError::WrongRole};
    if (record.state.status == PredictionStatus::Suspended) return {record.state.last_error};
    if (!impl_->replication->request_resync(record.state.peer)) return {PredictionError::UnknownPeer};
    record.state.status = PredictionStatus::AwaitingAuthority;
    ++impl_->stats.resync_requests;
    return {PredictionError::None, 1};
}

PredictionResult PredictionService::advance(double delta_seconds, std::uint64_t tick) {
    if (impl_->busy) return {PredictionError::Busy};
    if (!impl_->replication || !impl_->physics) return {PredictionError::NotAttached};
    if (!finite_range(delta_seconds, 0.0, 60.0) || (impl_->frame_tick && tick <= *impl_->frame_tick))
        return {PredictionError::InvalidTime};
    BusyGuard guard(impl_->busy);
    impl_->frame_tick = tick;
    impl_->elapsed_seconds += delta_seconds;
    const double admitted = std::min(delta_seconds, impl_->config.maximum_frame_seconds);
    impl_->stats.dropped_seconds += delta_seconds - admitted;
    impl_->accumulator += admitted;
    const auto available = static_cast<std::size_t>(std::floor((impl_->accumulator + 1.0e-12) / impl_->config.fixed_seconds));
    const auto steps = std::min(available, impl_->config.steps_per_frame);
    impl_->accumulator = std::max(0.0, impl_->accumulator - static_cast<double>(available) * impl_->config.fixed_seconds);
    impl_->stats.dropped_seconds += static_cast<double>(available - steps) * impl_->config.fixed_seconds;
    const double decay = impl_->config.smoothing_half_life > 0.0 ?
        std::exp2(-admitted / impl_->config.smoothing_half_life) : 0.0;
    PredictionResult result;
    for (auto& [id, record] : impl_->records) {
        (void)id;
        record.state.visual_position_offset *= decay;
        record.state.visual_rotation_offset = math::slerp({}, record.state.visual_rotation_offset, decay);
        if (record.state.status == PredictionStatus::Suspended) continue;
        if (const auto error = impl_->permission(record); error != PredictionError::None) {
            impl_->suspend(record, error);
            result.error = error;
            continue;
        }
        if (record.state.role == PredictionRole::Server) {
            record.state.predicted.layers = impl_->physics->registry().layers(record.state.entity);
            record.state.predicted.enabled = impl_->physics->registry().enabled(record.state.entity);
            record.state.predicted.physics_enabled = impl_->physics->get(record.body)->enabled;
        }
        for (std::size_t step = 0; step < steps && record.state.status == PredictionStatus::Active; ++step) {
            PredictionInput input;
            if (record.state.role == PredictionRole::Client) {
                if (record.history.size() >= impl_->config.history_inputs) {
                    ++impl_->stats.history_stalls;
                    record.state.last_error = PredictionError::Capacity;
                    break;
                }
                if (record.state.last_generated == maximum_sequence) {
                    result.error = PredictionError::SequenceExhausted;
                    (void)impl_->pause(record, PredictionError::SequenceExhausted);
                    break;
                }
                input = {record.state.last_generated + 1, tick, record.control};
            } else {
                if (record.state.last_applied == maximum_sequence || record.queued.empty()) break;
                const auto next = record.queued.find(record.state.last_applied + 1);
                if (next == record.queued.end()) break;
                input = next->second;
            }
            EntitySnapshot after;
            const auto simulated = impl_->simulate(record, record.state.predicted, input, after);
            if (!simulated) {
                result.error = simulated.error;
                (void)impl_->pause(record, simulated.error);
                break;
            }
            try {
                if (record.state.role == PredictionRole::Client) record.history.push_back({input, after, false});
            } catch (const std::bad_alloc&) {
                result.error = PredictionError::OutOfMemory;
                (void)impl_->pause(record, result.error);
                break;
            }
            const auto published = impl_->publish(record, after, true);
            if (!published) {
                if (record.state.role == PredictionRole::Client) record.history.pop_back();
                result.error = published.error;
                (void)impl_->pause(record, published.error);
                break;
            }
            record.state.predicted = after;
            record.state.last_error = PredictionError::None;
            if (record.state.role == PredictionRole::Client) {
                record.state.last_generated = input.sequence;
                record.control.buttons = static_cast<std::uint16_t>(record.control.buttons &
                    static_cast<std::uint16_t>(~button(PredictionButton::JumpPressed)));
                ++impl_->stats.generated_inputs;
            } else {
                record.queued.erase(input.sequence);
                record.state.last_applied = input.sequence;
                record.pending_ack = input.sequence;
                ++impl_->stats.applied_inputs;
            }
            ++result.affected;
            result.sequence = input.sequence;
        }
        // A zero-step or stalled frame must still stop ordinary integration.
        const auto held = impl_->publish(record, record.state.predicted, true);
        if (!held) {
            result.error = held.error;
            (void)impl_->pause(record, held.error);
        }
    }
    return result;
}
PredictionResult PredictionService::finish() {
    if (impl_->busy) return {PredictionError::Busy};
    if (!impl_->replication || !impl_->physics) return {PredictionError::NotAttached};
    BusyGuard guard(impl_->busy);
    PredictionResult result;
    for (auto& [id, record] : impl_->records) {
        if (record.state.status == PredictionStatus::Suspended) continue;
        const auto published = impl_->publish(record, record.state.predicted, false);
        if (!published) {
            result.error = published.error;
            (void)impl_->pause(record, published.error);
            continue;
        }
        if (record.state.role == PredictionRole::Server &&
            record.state.status == PredictionStatus::AwaitingAuthority &&
            (record.state.last_error == PredictionError::PublishFailed ||
             record.state.last_error == PredictionError::OutOfMemory || record.state.last_error == PredictionError::Busy)) {
            record.state.status = PredictionStatus::Active;
            record.state.last_error = PredictionError::None;
        }
        if (record.state.role != PredictionRole::Server || !record.pending_ack) continue;
        const auto acknowledged = impl_->replication->acknowledge_input(id, record.pending_ack);
        if (!acknowledged) {
            result.error = PredictionError::PublishFailed;
            record.state.last_error = result.error;
            continue;
        }
        impl_->stats.acknowledged_inputs += *record.pending_ack - record.state.acknowledged_input.value_or(0);
        record.state.acknowledged_input = record.pending_ack;
        record.state.predicted.acknowledged_input = record.pending_ack;
        result.sequence = *record.pending_ack;
        record.pending_ack.reset();
        ++result.affected;
    }
    return result;
}

std::optional<PredictionState> PredictionService::state(NetworkEntityId id) const {
    const auto found = impl_->records.find(id);
    return found == impl_->records.end() ? std::nullopt : std::optional{impl_->snapshot(found->second)};
}
std::vector<PredictionState> PredictionService::states() const {
    std::vector<PredictionState> result;
    result.reserve(impl_->records.size());
    for (const auto& [id, record] : impl_->records) {
        (void)id;
        result.push_back(impl_->snapshot(record));
    }
    return result;
}
std::vector<PredictionHistoryEntry> PredictionService::history(NetworkEntityId id, std::size_t limit) const {
    std::vector<PredictionHistoryEntry> result;
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end()) return result;
    limit = std::min(limit, found->second.history.size());
    result.reserve(limit);
    for (const auto& entry : found->second.history) {
        if (result.size() >= limit) break;
        result.push_back(entry);
    }
    return result;
}
std::optional<math::Transform> PredictionService::presentation(NetworkEntityId id) const {
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end() || !impl_->replication) return std::nullopt;
    const auto& record = found->second;
    auto visual = record.state.predicted;
    visual.world.position += record.state.visual_position_offset;
    visual.world.rotation = math::normalized(record.state.visual_rotation_offset * visual.world.rotation);
    return impl_->replication->runtime_transform(visual);
}
PredictionStatistics PredictionService::statistics() const noexcept {
    auto result = impl_->stats;
    result.bindings = impl_->records.size();
    for (const auto& [id, record] : impl_->records) {
        (void)id;
        result.retained_inputs += record.history.size();
        result.queued_inputs += record.queued.size();
        if (record.state.status == PredictionStatus::AwaitingAuthority) ++result.awaiting_authority;
    }
    return result;
}

namespace {
PredictionResult attach_prediction_services(core::Kernel& host) {
    auto* replication = host.services().find<ReplicationService>();
    auto* world = host.services().find<physics::PhysicsWorld>();
    if (!replication || !world) return {PredictionError::NotAttached};
    if (!replication->attach(world->scene(), world, host.services().find<scene::WorldPartition>()))
        return {PredictionError::NotAttached};
    return host.services().get<PredictionService>().attach(*replication, *world,
        host.services().find<physics::CharacterSystem>(), host.services().find<physics::VehicleSystem>());
}
std::string state_text(const PredictionState& state) {
    return std::to_string(state.id.persistent.value) + ":" + std::to_string(state.id.incarnation) +
        " entity=" + ecs::format_entity(state.entity) +
        (state.role == PredictionRole::Client ? " client" : " server") +
        " status=" + std::to_string(static_cast<unsigned>(state.status)) +
        " generated=" + std::to_string(state.last_generated) +
        " applied=" + std::to_string(state.last_applied) +
        " input_ack=" + (state.acknowledged_input ? std::to_string(*state.acknowledged_input) : "absent") +
        " pending=" + std::to_string(state.pending_inputs) +
        " unsent=" + std::to_string(state.unsent_inputs) +
        " queued=" + std::to_string(state.queued_inputs) +
        " position_error=" + std::to_string(state.position_error) +
        " velocity_error=" + std::to_string(state.velocity_error) +
        " error=" + std::string(prediction_error_name(state.last_error));
}
}
void register_prediction(core::Kernel& kernel) {
    if (kernel.services().find<PredictionService>()) return;
    PredictionConfig configuration;
    configuration.fixed_seconds = kernel.config().number("network.prediction.fixed_seconds", configuration.fixed_seconds);
    configuration.smoothing_half_life = kernel.config().number("network.prediction.smoothing_half_life", configuration.smoothing_half_life);
    configuration.retry_seconds = kernel.config().number("network.prediction.retry_seconds", configuration.retry_seconds);
    const auto channel = kernel.config().integer("network.prediction.channel", prediction_channel);
    if (channel < 0 || channel > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("prediction channel is outside uint16 range");
    configuration.channel = static_cast<std::uint16_t>(channel);
    auto& service = kernel.services().emplace<PredictionService>(configuration);
    (void)kernel.register_module({"network.prediction", "Bounded kinematic input prediction and application-ACK reconciliation", 1});
    (void)kernel.add_shutdown("network.prediction", [prediction = &service] { (void)prediction->shutdown(); });
    (void)kernel.add_system({"network.prediction.advance", core::Phase::PreSimulation, -8300, {"network.replication.receive"}},
        [](core::Kernel& host, const core::Frame& frame) {
            if (!attach_prediction_services(host)) return;
            auto& prediction = host.services().get<PredictionService>();
            auto* transport = host.services().find<NetworkTransport>();
            if (transport) (void)prediction.receive(*transport);
            (void)prediction.poll_authority();
            if (const auto* input = host.services().find<input::InputSystem>()) (void)prediction.sample_input(*input);
            const auto advanced = prediction.advance(frame.delta_seconds, frame.tick);
            if (!advanced) host.diagnose(core::Diagnostic::Severity::Warning, "network.prediction",
                std::string(prediction_error_name(advanced.error)));
            if (transport) (void)prediction.send(*transport);
        });
    (void)kernel.add_system({"network.prediction.finish", core::Phase::PostSimulation, 9500,
        {"physics.dynamics.step", "scene.transforms.post"}}, [](core::Kernel& host, const core::Frame&) {
            auto& prediction = host.services().get<PredictionService>();
            const auto finished = prediction.finish();
            if (!finished && finished.error != PredictionError::NotAttached)
                host.diagnose(core::Diagnostic::Severity::Warning, "network.prediction", std::string(prediction_error_name(finished.error)));
            const auto stats = prediction.statistics();
            host.metric("network.prediction.bindings", static_cast<double>(stats.bindings));
            host.metric("network.prediction.pending_inputs", static_cast<double>(stats.retained_inputs));
            host.metric("network.prediction.server_queue", static_cast<double>(stats.queued_inputs));
            host.metric("network.prediction.corrections", static_cast<double>(stats.corrections));
            host.metric("network.prediction.replayed_inputs", static_cast<double>(stats.replayed_inputs));
            host.metric("network.prediction.history_stalls", static_cast<double>(stats.history_stalls));
            host.metric("network.prediction.awaiting_authority", static_cast<double>(stats.awaiting_authority));
            host.metric("network.prediction.maximum_position_error", stats.maximum_position_error);
            host.metric("network.prediction.dropped_seconds", stats.dropped_seconds);
        });
    (void)kernel.add_command("network.prediction.bind", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 4) return core::CommandResult{false,
            "usage: network.prediction.bind persistent_id incarnation client|server peer_index:generation"};
        const auto id = parse_id(args);
        const auto peer = ecs::parse_entity(args[3]);
        if (!id || !peer || (args[2] != "client" && args[2] != "server"))
            return core::CommandResult{false, "invalid identity, role or peer handle"};
        const auto attached = attach_prediction_services(host);
        if (!attached) return command_result(attached);
        PredictionBindingOptions options;
        options.peer = PeerHandle{*peer};
        options.role = args[2] == "client" ? PredictionRole::Client : PredictionRole::Server;
        return command_result(host.services().get<PredictionService>().bind(*id, std::move(options)));
    }, "Bind an existing replicated kinematic body to a trusted input stream");
    (void)kernel.add_command("network.prediction.unbind", [](core::Kernel& host, core::CommandArgs args) {
        const auto id = parse_id(args);
        if (args.size() != 2 || !id) return core::CommandResult{false, "usage: network.prediction.unbind persistent_id incarnation"};
        return command_result(host.services().get<PredictionService>().unbind(*id));
    }, "Retire a prediction stream and restore its previous replication pose policy");
    (void)kernel.add_command("network.prediction.control", [](core::Kernel& host, core::CommandArgs args) {
        auto& prediction = host.services().get<PredictionService>();
        if (args.size() == 1 && args[0] == "off") {
            prediction.release_control();
            return core::CommandResult{true, "automatic input sampling released"};
        }
        const auto id = parse_id(args);
        if (args.size() != 2 || !id) return core::CommandResult{false, "usage: network.prediction.control persistent_id incarnation | off"};
        return command_result(prediction.select(*id));
    }, "Sample move/look/turn/lift and button actions without stealing character or vehicle control");
    (void)kernel.add_command("network.prediction.input", [](core::Kernel& host, core::CommandArgs args) {
        const auto id = parse_id(args);
        if (!id || args.size() < 4 || args.size() > 7) return core::CommandResult{false,
            "usage: network.prediction.input persistent_id incarnation move_x move_y [turn] [lift] [button_bits]"};
        const auto x = parse<double>(args[2]);
        const auto y = parse<double>(args[3]);
        const auto turn = args.size() > 4 ? parse<double>(args[4]) : std::optional{0.0};
        const auto lift = args.size() > 5 ? parse<double>(args[5]) : std::optional{0.0};
        const auto bits = args.size() > 6 ? parse<std::uint16_t>(args[6]) : std::optional<std::uint16_t>{0};
        if (!x || !y || !turn || !lift || !bits) return core::CommandResult{false, "input values did not parse"};
        return command_result(host.services().get<PredictionService>().set_control(*id, {{*x, *y}, *turn, *lift, *bits}));
    }, "Set bounded client controls; +Y movement maps to local -Z");
    (void)kernel.add_command("network.prediction.reconcile", [](core::Kernel& host, core::CommandArgs args) {
        const auto id = parse_id(args);
        if (args.size() != 2 || !id) return core::CommandResult{false, "usage: network.prediction.reconcile persistent_id incarnation"};
        auto& prediction = host.services().get<PredictionService>();
        const auto state = prediction.state(*id);
        const auto* replication = host.services().find<ReplicationService>();
        const auto* latest = state && replication ? replication->latest_received(state->peer) : nullptr;
        if (!latest) return core::CommandResult{false, "no received authoritative snapshot for this binding"};
        return command_result(prediction.reconcile(*id, *latest));
    }, "Reconcile against the actual received authoritative frame and its explicit input ACK");
    (void)kernel.add_command("network.prediction.resync", [](core::Kernel& host, core::CommandArgs args) {
        const auto id = parse_id(args);
        if (args.size() != 2 || !id) return core::CommandResult{false, "usage: network.prediction.resync persistent_id incarnation"};
        return command_result(host.services().get<PredictionService>().request_resync(*id));
    }, "Pause new prediction, retain input history, and request a full authority baseline");
    (void)kernel.add_command("network.prediction.state", [](core::Kernel& host, core::CommandArgs args) {
        const auto& prediction = host.services().get<PredictionService>();
        if (args.empty()) {
            std::string text;
            for (const auto& state : prediction.states()) text += state_text(state) + "\n";
            return core::CommandResult{true, text.empty() ? "no prediction bindings" : std::move(text)};
        }
        const auto id = parse_id(args);
        if (args.size() != 2 || !id) return core::CommandResult{false, "usage: network.prediction.state [persistent_id incarnation]"};
        const auto state = prediction.state(*id);
        return core::CommandResult{state.has_value(), state ? state_text(*state) : "unknown prediction binding"};
    }, "Inspect prediction history, application ACKs, replay errors and control state");
}

} // namespace ge::network
