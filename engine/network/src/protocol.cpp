#include <ge/network/protocol.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <limits>
#include <list>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ge::network {
namespace {
constexpr std::uint32_t sequence_half_range = 0x80000000U;
constexpr std::size_t checksum_offset = 60;
constexpr std::size_t mode_count = 4;
constexpr std::uint32_t message_window = 64;

bool valid_mode(DeliveryMode mode) noexcept {
    return static_cast<std::uint8_t>(mode) < mode_count;
}

void increment(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    const auto available = std::numeric_limits<std::uint64_t>::max() - value;
    value += std::min(amount, available);
}

ProtocolError validate_header(const PacketHeader& header, std::size_t payload_size) noexcept {
    if (!header.connection.valid() || !valid_mode(header.delivery) ||
        (header.flags & static_cast<std::uint16_t>(~packet_ack_valid)) != 0 ||
        payload_size != header.payload_bytes) {
        return ProtocolError::InvalidHeader;
    }
    if ((header.flags & packet_ack_valid) == 0 &&
        (header.ack_sequence != 0 || header.ack_bits != 0)) {
        return ProtocolError::InvalidHeader;
    }
    if (header.packet_kind == PacketKind::Acknowledgement) {
        const bool empty = header.channel == 0 && header.packet_sequence == 0 &&
            header.message_sequence == 0 && header.message_bytes == 0 &&
            header.fragment_index == 0 && header.fragment_count == 0 &&
            header.payload_bytes == 0 && static_cast<std::uint16_t>(header.message_kind) == 0;
        return empty && header.delivery == DeliveryMode::Unreliable &&
            header.flags == packet_ack_valid ? ProtocolError::None : ProtocolError::InvalidHeader;
    }
    if (header.packet_kind != PacketKind::Data || header.fragment_count == 0 ||
        header.fragment_index >= header.fragment_count ||
        static_cast<std::uint16_t>(header.message_kind) == 0) {
        return ProtocolError::InvalidHeader;
    }
    const auto total = static_cast<std::uint64_t>(header.message_bytes);
    const auto count = std::max<std::uint64_t>(1,
        (total + max_fragment_payload_bytes - 1) / max_fragment_payload_bytes);
    if (count != header.fragment_count) {
        return ProtocolError::InvalidHeader;
    }
    const auto offset = static_cast<std::uint64_t>(header.fragment_index) *
        max_fragment_payload_bytes;
    if (offset > total) {
        return ProtocolError::InvalidHeader;
    }
    const auto expected = std::min<std::uint64_t>(max_fragment_payload_bytes, total - offset);
    return expected == payload_size ? ProtocolError::None : ProtocolError::InvalidHeader;
}

std::uint32_t packet_checksum(std::span<const std::byte> wire) noexcept {
    ge::core::Crc32 checksum;
    const std::array<std::byte, 4> zeroes{};
    checksum.update(wire.first(checksum_offset));
    checksum.update(zeroes);
    checksum.update(wire.subspan(packet_header_bytes));
    return checksum.value();
}

struct MessageKey {
    std::uint16_t channel{};
    DeliveryMode mode{};
    std::uint32_t sequence{};
    auto operator<=>(const MessageKey&) const = default;
};
MessageKey message_key(const PacketHeader& header) noexcept {
    return {header.channel, header.delivery, header.message_sequence};
}
struct OutgoingFragment {
    PacketHeader header;
    std::vector<std::byte> payload;
    std::optional<std::uint32_t> packet_sequence;
    std::uint32_t transmissions{};
    double first_sent{};
    double last_sent{};
    double retry_delay{};
};
struct Assembly {
    ReceivedMessage message;
    std::vector<std::uint8_t> fragments;
    std::size_t received_fragments{};
    double first_received{};
    [[nodiscard]] bool complete() const noexcept {
        return received_fragments == fragments.size();
    }
};
struct ModeState {
    std::uint32_t next_send{};
    std::uint32_t next_receive{};
    std::uint64_t unordered_completed{};
    AckWindow completed;
};
struct ChannelState {
    std::array<ModeState, mode_count> modes;
};
enum class MessageDisposition { Admit, Duplicate, Outside };
} // namespace

std::string_view protocol_error_name(ProtocolError error) noexcept {
    switch (error) {
    case ProtocolError::None: return "none";
    case ProtocolError::InvalidConfiguration: return "invalid-configuration";
    case ProtocolError::InvalidTime: return "invalid-time";
    case ProtocolError::InvalidArgument: return "invalid-argument";
    case ProtocolError::Truncated: return "truncated";
    case ProtocolError::Oversized: return "oversized";
    case ProtocolError::BadMagic: return "bad-magic";
    case ProtocolError::UnsupportedVersion: return "unsupported-version";
    case ProtocolError::InvalidHeader: return "invalid-header";
    case ProtocolError::ChecksumMismatch: return "checksum-mismatch";
    case ProtocolError::WrongConnection: return "wrong-connection";
    case ProtocolError::SendCapacity: return "send-capacity";
    case ProtocolError::ReceiveCapacity: return "receive-capacity";
    case ProtocolError::SequenceWindow: return "sequence-window";
    case ProtocolError::FragmentConflict: return "fragment-conflict";
    case ProtocolError::RetryExhausted: return "retry-exhausted";
    case ProtocolError::ReliableExpired: return "reliable-expired";
    case ProtocolError::Failed: return "failed";
    }
    return "unknown";
}
std::string_view delivery_mode_name(DeliveryMode mode) noexcept {
    switch (mode) {
    case DeliveryMode::Unreliable: return "unreliable";
    case DeliveryMode::Sequenced: return "sequenced";
    case DeliveryMode::ReliableUnordered: return "reliable-unordered";
    case DeliveryMode::ReliableOrdered: return "reliable-ordered";
    }
    return "invalid";
}
bool reliable(DeliveryMode mode) noexcept {
    return mode == DeliveryMode::ReliableUnordered || mode == DeliveryMode::ReliableOrdered;
}
bool sequence_newer(std::uint32_t a, std::uint32_t b) noexcept {
    const auto distance = a - b;
    return distance != 0 && distance < sequence_half_range;
}
bool sequence_ambiguous(std::uint32_t a, std::uint32_t b) noexcept {
    return a - b == sequence_half_range;
}
PacketDecodeResult::operator bool() const noexcept {
    return error == ProtocolError::None;
}
PacketEncodeResult::operator bool() const noexcept {
    return error == ProtocolError::None;
}
EnqueueResult::operator bool() const noexcept {
    return error == ProtocolError::None;
}
ReceiveResult::operator bool() const noexcept {
    return error == ProtocolError::None;
}

PacketEncodeResult encode_packet(PacketHeader header, std::span<const std::byte> payload) {
    if (payload.size() > max_fragment_payload_bytes) {
        return {ProtocolError::Oversized, {}};
    }
    header.payload_bytes = static_cast<std::uint16_t>(payload.size());
    const auto validation = validate_header(header, payload.size());
    if (validation != ProtocolError::None) {
        return {validation, {}};
    }
    ge::core::ByteWriter writer(max_wire_datagram_bytes, ge::core::ByteOrder::big);
    writer.reserve(packet_header_bytes + payload.size());
    writer.write_u32(protocol_magic);
    writer.write_u8(protocol_version);
    writer.write_u8(static_cast<std::uint8_t>(packet_header_bytes));
    writer.write_u8(static_cast<std::uint8_t>(header.packet_kind));
    writer.write_u8(static_cast<std::uint8_t>(header.delivery));
    writer.write_u16(header.flags);
    writer.write_u16(header.channel);
    writer.write_u64(header.connection.value);
    writer.write_u32(header.packet_sequence);
    writer.write_u32(header.ack_sequence);
    writer.write_u64(header.ack_bits);
    writer.write_u32(header.message_sequence);
    writer.write_u32(header.message_bytes);
    writer.write_u16(header.fragment_index);
    writer.write_u16(header.fragment_count);
    writer.write_u16(header.payload_bytes);
    writer.write_u16(static_cast<std::uint16_t>(header.message_kind));
    writer.write_u64(0);
    writer.write_u32(0);
    writer.write_bytes(payload);
    writer.patch_u32(checksum_offset, packet_checksum(writer.bytes()));
    return {ProtocolError::None, writer.take()};
}

PacketDecodeResult decode_packet(std::span<const std::byte> wire) {
    if (wire.size() < packet_header_bytes) {
        return {ProtocolError::Truncated, {}, {}};
    }
    if (wire.size() > max_wire_datagram_bytes) {
        return {ProtocolError::Oversized, {}, {}};
    }
    ge::core::BinaryLimits limits;
    limits.max_total_bytes = max_wire_datagram_bytes;
    ge::core::ByteReader reader(wire, ge::core::ByteOrder::big, limits);
    if (reader.read_u32() != protocol_magic) {
        return {ProtocolError::BadMagic, {}, {}};
    }
    if (reader.read_u8() != protocol_version) {
        return {ProtocolError::UnsupportedVersion, {}, {}};
    }
    if (reader.read_u8() != packet_header_bytes) {
        return {ProtocolError::InvalidHeader, {}, {}};
    }
    PacketHeader header;
    header.packet_kind = static_cast<PacketKind>(reader.read_u8());
    header.delivery = static_cast<DeliveryMode>(reader.read_u8());
    header.flags = reader.read_u16();
    header.channel = reader.read_u16();
    header.connection.value = reader.read_u64();
    header.packet_sequence = reader.read_u32();
    header.ack_sequence = reader.read_u32();
    header.ack_bits = reader.read_u64();
    header.message_sequence = reader.read_u32();
    header.message_bytes = reader.read_u32();
    header.fragment_index = reader.read_u16();
    header.fragment_count = reader.read_u16();
    header.payload_bytes = reader.read_u16();
    header.message_kind = static_cast<MessageKind>(reader.read_u16());
    if (reader.read_u64() != 0) {
        return {ProtocolError::InvalidHeader, {}, {}};
    }
    const auto checksum = reader.read_u32();
    const auto validation = validate_header(header, reader.remaining());
    if (validation != ProtocolError::None) {
        return {validation, {}, {}};
    }
    if (packet_checksum(wire) != checksum) {
        return {ProtocolError::ChecksumMismatch, {}, {}};
    }
    return {ProtocolError::None, header, reader.unread()};
}

AckWindow::Observation AckWindow::classify(std::uint32_t sequence) const noexcept {
    if (!initialized_) {
        return Observation::New;
    }
    if (sequence == newest_) {
        return Observation::Duplicate;
    }
    if (sequence_ambiguous(sequence, newest_)) {
        return Observation::Ambiguous;
    }
    if (sequence_newer(sequence, newest_)) {
        return Observation::New;
    }
    const auto distance = newest_ - sequence;
    if (distance > 64) {
        return Observation::TooOld;
    }
    return (bits_ & (std::uint64_t{1} << (distance - 1))) != 0 ?
        Observation::Duplicate : Observation::New;
}
AckWindow::Observation AckWindow::observe(std::uint32_t sequence) noexcept {
    const auto result = classify(sequence);
    if (result != Observation::New) {
        return result;
    }
    if (!initialized_) {
        initialized_ = true;
        newest_ = sequence;
        bits_ = 0;
    } else if (sequence_newer(sequence, newest_)) {
        const auto distance = sequence - newest_;
        if (distance > 64) {
            bits_ = 0;
        } else if (distance == 64) {
            bits_ = std::uint64_t{1} << 63;
        } else {
            bits_ = (bits_ << distance) | (std::uint64_t{1} << (distance - 1));
        }
        newest_ = sequence;
    } else {
        bits_ |= std::uint64_t{1} << (newest_ - sequence - 1);
    }
    return result;
}
bool AckWindow::contains(std::uint32_t sequence) const noexcept {
    return initialized_ && acknowledges(sequence, newest_, bits_);
}
bool AckWindow::initialized() const noexcept {
    return initialized_;
}
std::uint32_t AckWindow::newest() const noexcept {
    return newest_;
}
std::uint64_t AckWindow::bits() const noexcept {
    return bits_;
}
void AckWindow::clear() noexcept {
    initialized_ = false;
    newest_ = 0;
    bits_ = 0;
}
bool AckWindow::acknowledges(std::uint32_t sequence, std::uint32_t newest,
    std::uint64_t bits) noexcept {
    const auto distance = newest - sequence;
    if (distance == 0) {
        return true;
    }
    return distance <= 64 && (bits & (std::uint64_t{1} << (distance - 1))) != 0;
}

ProtocolError validate_protocol_limits(const ProtocolLimits& limits) noexcept {
    if (limits.max_channels == 0 || limits.max_channels > 256 ||
        limits.max_message_bytes == 0 || limits.max_message_bytes > 16U * 1024U * 1024U ||
        limits.max_pending_messages == 0 || limits.max_pending_messages > 4096 ||
        limits.max_pending_fragments == 0 || limits.max_pending_fragments > 65536 ||
        limits.max_receive_messages == 0 || limits.max_receive_messages > 4096 ||
        limits.max_send_bytes < limits.max_message_bytes ||
        limits.max_receive_bytes < limits.max_message_bytes ||
        limits.max_send_bytes > 64U * 1024U * 1024U ||
        limits.max_receive_bytes > 64U * 1024U * 1024U ||
        limits.max_inflight_fragments == 0 || limits.max_inflight_fragments > 64 ||
        limits.max_work_per_pump < 16 || limits.max_work_per_pump > 65536 ||
        limits.max_transmissions == 0 || limits.max_transmissions > 64) {
        return ProtocolError::InvalidConfiguration;
    }
    const std::array times{limits.initial_rto_seconds, limits.minimum_rto_seconds,
        limits.maximum_rto_seconds, limits.acknowledgement_delay_seconds,
        limits.reassembly_timeout_seconds, limits.reliable_lifetime_seconds};
    for (const double time : times) {
        if (!std::isfinite(time) || time < 0) {
            return ProtocolError::InvalidConfiguration;
        }
    }
    if (limits.minimum_rto_seconds <= 0 ||
        limits.initial_rto_seconds < limits.minimum_rto_seconds ||
        limits.initial_rto_seconds > limits.maximum_rto_seconds ||
        limits.maximum_rto_seconds > 60 ||
        limits.acknowledgement_delay_seconds > limits.maximum_rto_seconds ||
        limits.reassembly_timeout_seconds < limits.maximum_rto_seconds ||
        limits.reliable_lifetime_seconds < limits.reassembly_timeout_seconds ||
        limits.reliable_lifetime_seconds > 86400) {
        return ProtocolError::InvalidConfiguration;
    }
    return ProtocolError::None;
}

struct ProtocolPeer::Impl {
    ge::PersistentId connection;
    ProtocolLimits limits;
    ProtocolStatistics stats;
    ProtocolError failure{ProtocolError::None};
    std::vector<ChannelState> channels;
    std::map<std::uint64_t, OutgoingFragment> outgoing;
    std::map<MessageKey, std::size_t> send_messages;
    std::map<std::uint32_t, std::uint64_t> transmissions;
    std::map<MessageKey, Assembly> assemblies;
    std::list<ReceivedMessage> inbox;
    AckWindow received_packets;
    std::uint64_t next_fragment{1};
    std::uint64_t send_cursor{};
    std::optional<MessageKey> assembly_cursor;
    std::uint32_t next_packet{};
    std::size_t send_bytes{};
    std::size_t receive_bytes{};
    std::size_t receive_messages{};
    double now{};
    double ack_since{};
    bool clock_initialized{};
    bool ack_dirty{};
    bool have_rtt{};

    Impl(ge::PersistentId identity, ProtocolLimits bounds)
        : connection(identity), limits(bounds), channels(bounds.max_channels) {
        stats.retransmission_timeout_seconds = limits.initial_rto_seconds;
    }
    bool accept_time(double value) noexcept {
        if (!std::isfinite(value) || value < 0 || (clock_initialized && value < now)) {
            return false;
        }
        clock_initialized = true;
        now = value;
        return true;
    }
    ModeState& mode(const MessageKey& key) noexcept {
        return channels[key.channel].modes[static_cast<std::size_t>(key.mode)];
    }
    void mark_ack(DeliveryMode delivery) noexcept {
        if (reliable(delivery)) {
            if (!ack_dirty) {
                ack_since = now;
            }
            ack_dirty = true;
        }
    }
    void attach_ack(PacketHeader& header) const noexcept {
        if (received_packets.initialized()) {
            header.flags = packet_ack_valid;
            header.ack_sequence = received_packets.newest();
            header.ack_bits = received_packets.bits();
        }
    }
    void update_rtt(double sample) noexcept {
        sample = std::clamp(sample, 0.0, limits.reliable_lifetime_seconds);
        if (!have_rtt) {
            stats.smoothed_rtt_seconds = sample;
            stats.rtt_variation_seconds = sample / 2;
            have_rtt = true;
        } else {
            const double error = std::abs(stats.smoothed_rtt_seconds - sample);
            stats.rtt_variation_seconds += (error - stats.rtt_variation_seconds) / 4;
            stats.smoothed_rtt_seconds += (sample - stats.smoothed_rtt_seconds) / 8;
        }
        const double estimate = stats.smoothed_rtt_seconds +
            std::max(0.001, 4 * stats.rtt_variation_seconds);
        stats.retransmission_timeout_seconds = std::clamp(estimate,
            limits.minimum_rto_seconds, limits.maximum_rto_seconds);
    }
    void erase_outgoing(std::map<std::uint64_t, OutgoingFragment>::iterator it) noexcept {
        if (it->second.packet_sequence) {
            transmissions.erase(*it->second.packet_sequence);
        }
        send_bytes -= it->second.payload.size();
        const auto message = send_messages.find(message_key(it->second.header));
        if (message != send_messages.end() && --message->second == 0) {
            send_messages.erase(message);
        }
        outgoing.erase(it);
    }
    void acknowledge(const PacketHeader& header) noexcept {
        if ((header.flags & packet_ack_valid) == 0) {
            return;
        }
        for (unsigned offset = 0; offset <= 64; ++offset) {
            if (offset != 0 && (header.ack_bits & (std::uint64_t{1} << (offset - 1))) == 0) {
                continue;
            }
            const auto sequence = header.ack_sequence - offset;
            const auto sent = transmissions.find(sequence);
            if (sent == transmissions.end()) {
                continue;
            }
            const auto fragment = outgoing.find(sent->second);
            if (fragment != outgoing.end()) {
                if (fragment->second.transmissions == 1) {
                    update_rtt(now - fragment->second.last_sent);
                }
                increment(stats.acknowledged_fragments);
                erase_outgoing(fragment);
            }
        }
    }
    MessageDisposition disposition(const MessageKey& key) noexcept {
        auto& state = mode(key);
        if (reliable(key.mode)) {
            if (sequence_ambiguous(key.sequence, state.next_receive)) {
                return MessageDisposition::Outside;
            }
            if (sequence_newer(state.next_receive, key.sequence)) {
                return MessageDisposition::Duplicate;
            }
            const auto distance = key.sequence - state.next_receive;
            if (distance >= message_window) {
                return MessageDisposition::Outside;
            }
            if (key.mode == DeliveryMode::ReliableUnordered &&
                (state.unordered_completed & (std::uint64_t{1} << distance)) != 0) {
                return MessageDisposition::Duplicate;
            }
            return MessageDisposition::Admit;
        }
        if (key.mode == DeliveryMode::Sequenced && state.completed.initialized()) {
            if (sequence_ambiguous(key.sequence, state.completed.newest())) {
                return MessageDisposition::Outside;
            }
            return sequence_newer(key.sequence, state.completed.newest()) ?
                MessageDisposition::Admit : MessageDisposition::Duplicate;
        }
        const auto observed = state.completed.classify(key.sequence);
        if (observed == AckWindow::Observation::Ambiguous) {
            return MessageDisposition::Outside;
        }
        return observed == AckWindow::Observation::New ?
            MessageDisposition::Admit : MessageDisposition::Duplicate;
    }
    void erase_assembly(std::map<MessageKey, Assembly>::iterator it) noexcept {
        receive_bytes -= it->second.message.payload.size();
        --receive_messages;
        assemblies.erase(it);
    }
    bool publish(const MessageKey& key) {
        const auto found = assemblies.find(key);
        if (found == assemblies.end() || !found->second.complete()) {
            return false;
        }
        auto& state = mode(key);
        if (key.mode == DeliveryMode::ReliableOrdered && key.sequence != state.next_receive) {
            return false;
        }
        if (!reliable(key.mode) && disposition(key) != MessageDisposition::Admit) {
            increment(stats.stale_messages);
            erase_assembly(found);
            return true;
        }
        inbox.push_back(std::move(found->second.message));
        if (key.mode == DeliveryMode::ReliableOrdered) {
            ++state.next_receive;
        } else if (key.mode == DeliveryMode::ReliableUnordered) {
            const auto distance = key.sequence - state.next_receive;
            state.unordered_completed |= std::uint64_t{1} << distance;
            while ((state.unordered_completed & 1U) != 0) {
                state.unordered_completed >>= 1;
                ++state.next_receive;
            }
        } else {
            static_cast<void>(state.completed.observe(key.sequence));
        }
        assemblies.erase(found);
        increment(stats.completed_messages);
        return true;
    }
    void release_ordered(std::uint16_t channel) {
        MessageKey key{channel, DeliveryMode::ReliableOrdered, 0};
        for (std::uint32_t count = 0; count < message_window; ++count) {
            key.sequence = mode(key).next_receive;
            if (!publish(key)) {
                break;
            }
        }
    }
    void maintain_assemblies(std::size_t& work) {
        const auto count = std::min(assemblies.size(), std::max<std::size_t>(1, work / 4));
        for (std::size_t i = 0; i < count && !assemblies.empty() && work != 0; ++i) {
            --work;
            auto it = assembly_cursor ? assemblies.upper_bound(*assembly_cursor) : assemblies.begin();
            if (it == assemblies.end()) {
                it = assemblies.begin();
            }
            const auto key = it->first;
            assembly_cursor = key;
            if (!reliable(key.mode) && disposition(key) != MessageDisposition::Admit) {
                increment(stats.stale_messages);
                erase_assembly(it);
                continue;
            }
            const double age = now - it->second.first_received;
            const double lifetime = it->second.complete() ?
                limits.reliable_lifetime_seconds : limits.reassembly_timeout_seconds;
            if (age >= lifetime) {
                increment(stats.expired_messages);
                if (reliable(key.mode)) {
                    failure = ProtocolError::ReliableExpired;
                    return;
                }
                erase_assembly(it);
                continue;
            }
            static_cast<void>(publish(key));
        }
    }
    ProtocolStatistics snapshot() const noexcept {
        auto result = stats;
        result.pending_messages = send_messages.size();
        result.pending_fragments = outgoing.size();
        result.pending_send_bytes = send_bytes;
        result.retained_receive_messages = receive_messages;
        result.retained_receive_bytes = receive_bytes;
        result.inflight_fragments = transmissions.size();
        return result;
    }
};

ProtocolPeer::ProtocolPeer(ge::PersistentId connection, ProtocolLimits limits) {
    if (!connection.valid() || validate_protocol_limits(limits) != ProtocolError::None) {
        throw std::invalid_argument("invalid protocol connection or limits");
    }
    impl_ = std::make_unique<Impl>(connection, limits);
}
ProtocolPeer::~ProtocolPeer() = default;
ProtocolPeer::ProtocolPeer(ProtocolPeer&&) noexcept = default;
ProtocolPeer& ProtocolPeer::operator=(ProtocolPeer&&) noexcept = default;
ge::PersistentId ProtocolPeer::connection() const noexcept {
    return impl_ ? impl_->connection : ge::PersistentId{};
}
const ProtocolLimits& ProtocolPeer::limits() const noexcept {
    static const ProtocolLimits defaults;
    return impl_ ? impl_->limits : defaults;
}
ProtocolError ProtocolPeer::failure() const noexcept {
    return impl_ ? impl_->failure : ProtocolError::Failed;
}
ProtocolStatistics ProtocolPeer::statistics() const noexcept {
    return impl_ ? impl_->snapshot() : ProtocolStatistics{};
}

EnqueueResult ProtocolPeer::enqueue(std::uint16_t channel, MessageKind kind,
    std::span<const std::byte> payload, DeliveryMode delivery) {
    if (!impl_ || impl_->failure != ProtocolError::None) {
        return {failure(), 0, 0};
    }
    auto& state = *impl_;
    if (channel >= state.limits.max_channels || !valid_mode(delivery) ||
        static_cast<std::uint16_t>(kind) == 0) {
        return {ProtocolError::InvalidArgument, 0, 0};
    }
    if (payload.size() > state.limits.max_message_bytes) {
        return {ProtocolError::Oversized, 0, 0};
    }
    const auto count = std::max<std::size_t>(1,
        (payload.size() + max_fragment_payload_bytes - 1) / max_fragment_payload_bytes);
    if (state.send_messages.size() >= state.limits.max_pending_messages ||
        count > state.limits.max_pending_fragments - state.outgoing.size() ||
        payload.size() > state.limits.max_send_bytes - state.send_bytes ||
        count > std::numeric_limits<std::uint64_t>::max() - state.next_fragment) {
        return {ProtocolError::SendCapacity, 0, 0};
    }
    auto& channel_state = state.channels[channel].modes[static_cast<std::size_t>(delivery)];
    const MessageKey key{channel, delivery, channel_state.next_send};
    if (state.send_messages.contains(key)) {
        return {ProtocolError::SequenceWindow, 0, 0};
    }
    std::map<std::uint64_t, OutgoingFragment> staged;
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = index * max_fragment_payload_bytes;
        const auto length = std::min(max_fragment_payload_bytes, payload.size() - offset);
        const auto slice = payload.subspan(offset, length);
        OutgoingFragment fragment;
        fragment.header.channel = channel;
        fragment.header.connection = state.connection;
        fragment.header.delivery = delivery;
        fragment.header.message_kind = kind;
        fragment.header.message_sequence = key.sequence;
        fragment.header.message_bytes = static_cast<std::uint32_t>(payload.size());
        fragment.header.fragment_index = static_cast<std::uint16_t>(index);
        fragment.header.fragment_count = static_cast<std::uint16_t>(count);
        fragment.header.payload_bytes = static_cast<std::uint16_t>(length);
        fragment.payload.assign(slice.begin(), slice.end());
        staged.emplace(state.next_fragment + index, std::move(fragment));
    }
    state.send_messages.emplace(key, count);
    state.outgoing.merge(staged);
    state.next_fragment += count;
    state.send_bytes += payload.size();
    ++channel_state.next_send;
    return {ProtocolError::None, key.sequence, count};
}

ReceiveResult ProtocolPeer::receive(std::span<const std::byte> datagram, double now_seconds) {
    if (!impl_ || impl_->failure != ProtocolError::None) {
        return {failure(), false, false, 0};
    }
    auto& state = *impl_;
    if (!state.accept_time(now_seconds)) {
        return {ProtocolError::InvalidTime, false, false, 0};
    }
    increment(state.stats.datagrams_received);
    increment(state.stats.wire_bytes_received, static_cast<std::uint64_t>(datagram.size()));
    const auto decoded = decode_packet(datagram);
    if (!decoded) {
        increment(state.stats.malformed_datagrams);
        return {decoded.error, false, false, 0};
    }
    const auto& header = decoded.header;
    auto reject = [&state](ProtocolError error) {
        increment(state.stats.rejected_datagrams);
        return ReceiveResult{error, false, false, 0};
    };
    if (header.connection != state.connection) {
        return reject(ProtocolError::WrongConnection);
    }
    state.acknowledge(header);
    if (header.packet_kind == PacketKind::Acknowledgement) {
        return {ProtocolError::None, true, false, 0};
    }
    if (header.channel >= state.limits.max_channels ||
        header.message_bytes > state.limits.max_message_bytes) {
        return reject(ProtocolError::Oversized);
    }
    const auto observation = state.received_packets.classify(header.packet_sequence);
    if (observation == AckWindow::Observation::Duplicate) {
        state.mark_ack(header.delivery);
        increment(state.stats.duplicate_datagrams);
        return {ProtocolError::None, true, true, 0};
    }
    if (observation != AckWindow::Observation::New) {
        return reject(ProtocolError::SequenceWindow);
    }
    const auto key = message_key(header);
    const auto disposition = state.disposition(key);
    if (disposition == MessageDisposition::Outside) {
        return reject(ProtocolError::SequenceWindow);
    }
    if (disposition == MessageDisposition::Duplicate) {
        static_cast<void>(state.received_packets.observe(header.packet_sequence));
        state.mark_ack(header.delivery);
        increment(state.stats.stale_messages);
        return {ProtocolError::None, true, true, 0};
    }
    auto found = state.assemblies.find(key);
    if (found == state.assemblies.end()) {
        if (state.receive_messages >= state.limits.max_receive_messages ||
            header.message_bytes > state.limits.max_receive_bytes - state.receive_bytes) {
            return reject(ProtocolError::ReceiveCapacity);
        }
        Assembly assembly;
        assembly.message.channel = header.channel;
        assembly.message.kind = header.message_kind;
        assembly.message.delivery = header.delivery;
        assembly.message.sequence = header.message_sequence;
        assembly.message.payload.resize(header.message_bytes);
        assembly.fragments.resize(header.fragment_count);
        assembly.first_received = now_seconds;
        found = state.assemblies.emplace(key, std::move(assembly)).first;
        state.receive_bytes += header.message_bytes;
        ++state.receive_messages;
    }
    auto& assembly = found->second;
    if (assembly.message.kind != header.message_kind ||
        assembly.message.payload.size() != header.message_bytes ||
        assembly.fragments.size() != header.fragment_count) {
        return reject(ProtocolError::FragmentConflict);
    }
    const auto offset = static_cast<std::size_t>(header.fragment_index) * max_fragment_payload_bytes;
    auto target = std::span<std::byte>(assembly.message.payload).subspan(offset, decoded.payload.size());
    const bool duplicate = assembly.fragments[header.fragment_index] != 0;
    if (duplicate && !std::equal(target.begin(), target.end(), decoded.payload.begin())) {
        return reject(ProtocolError::FragmentConflict);
    }
    if (!duplicate) {
        std::copy(decoded.payload.begin(), decoded.payload.end(), target.begin());
        assembly.fragments[header.fragment_index] = 1;
        ++assembly.received_fragments;
    }
    static_cast<void>(state.received_packets.observe(header.packet_sequence));
    state.mark_ack(header.delivery);
    const auto completed_before = state.stats.completed_messages;
    static_cast<void>(state.publish(key));
    if (header.delivery == DeliveryMode::ReliableOrdered) {
        state.release_ordered(header.channel);
    }
    return {ProtocolError::None, true, duplicate,
        static_cast<std::size_t>(state.stats.completed_messages - completed_before)};
}

std::vector<WireDatagram> ProtocolPeer::pump(double now_seconds,
    std::size_t max_datagrams, std::size_t max_wire_bytes) {
    std::vector<WireDatagram> result;
    if (!impl_ || impl_->failure != ProtocolError::None) {
        return result;
    }
    auto& state = *impl_;
    if (!state.accept_time(now_seconds)) {
        state.failure = ProtocolError::InvalidTime;
        return result;
    }
    auto work = state.limits.max_work_per_pump;
    state.maintain_assemblies(work);
    if (state.failure != ProtocolError::None) {
        return result;
    }
    max_datagrams = std::min(max_datagrams, work);
    result.reserve(std::min(max_datagrams, max_wire_bytes / packet_header_bytes));
    std::size_t emitted_bytes = 0;
    const auto scan_count = std::min(work, state.outgoing.size());
    for (std::size_t scan = 0; scan < scan_count && !state.outgoing.empty(); ++scan) {
        auto it = state.outgoing.upper_bound(state.send_cursor);
        if (it == state.outgoing.end()) {
            it = state.outgoing.begin();
        }
        state.send_cursor = it->first;
        auto& fragment = it->second;
        const bool is_reliable = reliable(fragment.header.delivery);
        const bool retransmission = fragment.transmissions != 0;
        if (retransmission && now_seconds - fragment.first_sent >= state.limits.reliable_lifetime_seconds) {
            state.failure = ProtocolError::ReliableExpired;
            break;
        }
        if (retransmission && now_seconds - fragment.last_sent < fragment.retry_delay) {
            continue;
        }
        if (fragment.transmissions >= state.limits.max_transmissions) {
            state.failure = ProtocolError::RetryExhausted;
            break;
        }
        if (result.size() >= max_datagrams) {
            continue;
        }
        const auto wire_size = packet_header_bytes + fragment.payload.size();
        if (wire_size > max_wire_bytes - emitted_bytes ||
            (is_reliable && !retransmission &&
             state.transmissions.size() >= state.limits.max_inflight_fragments)) {
            continue;
        }
        if (state.transmissions.contains(state.next_packet)) {
            state.failure = ProtocolError::SequenceWindow;
            break;
        }
        auto header = fragment.header;
        header.packet_sequence = state.next_packet;
        state.attach_ack(header);
        auto encoded = encode_packet(header, fragment.payload);
        if (!encoded) {
            state.failure = encoded.error;
            break;
        }
        if (is_reliable) {
            state.transmissions.emplace(state.next_packet, it->first);
            if (fragment.packet_sequence) {
                state.transmissions.erase(*fragment.packet_sequence);
            }
            fragment.packet_sequence = state.next_packet;
            fragment.retry_delay = retransmission ?
                std::min(state.limits.maximum_rto_seconds, fragment.retry_delay * 2) :
                state.stats.retransmission_timeout_seconds;
            if (!retransmission) {
                fragment.first_sent = now_seconds;
            }
            fragment.last_sent = now_seconds;
            ++fragment.transmissions;
        }
        result.push_back({std::move(encoded.bytes), state.next_packet, retransmission});
        ++state.next_packet;
        emitted_bytes += wire_size;
        increment(state.stats.datagrams_sent);
        increment(state.stats.wire_bytes_sent, static_cast<std::uint64_t>(wire_size));
        if (retransmission) {
            increment(state.stats.retransmissions);
        }
        state.ack_dirty = false;
        if (!is_reliable) {
            state.erase_outgoing(it);
        }
    }
    if (state.failure == ProtocolError::None && state.ack_dirty &&
        now_seconds - state.ack_since >= state.limits.acknowledgement_delay_seconds &&
        result.size() < max_datagrams && packet_header_bytes <= max_wire_bytes - emitted_bytes) {
        PacketHeader header;
        header.packet_kind = PacketKind::Acknowledgement;
        header.delivery = DeliveryMode::Unreliable;
        header.connection = state.connection;
        header.fragment_count = 0;
        header.message_kind = static_cast<MessageKind>(0);
        state.attach_ack(header);
        auto encoded = encode_packet(header, {});
        if (encoded) {
            result.push_back({std::move(encoded.bytes), 0, false});
            increment(state.stats.datagrams_sent);
            increment(state.stats.wire_bytes_sent, packet_header_bytes);
            state.ack_dirty = false;
        }
    }
    return result;
}

std::vector<ReceivedMessage> ProtocolPeer::take_messages(std::size_t limit) {
    return take_messages(MessageFilter{}, limit, std::numeric_limits<std::size_t>::max());
}
std::vector<ReceivedMessage> ProtocolPeer::take_messages(MessageFilter filter,
    std::size_t limit, std::size_t scan_limit) {
    std::vector<ReceivedMessage> result;
    if (!impl_) {
        return result;
    }
    auto& state = *impl_;
    result.reserve(std::min({limit, scan_limit, state.inbox.size()}));
    auto it = state.inbox.begin();
    std::size_t scanned = 0;
    while (it != state.inbox.end() && result.size() < limit && scanned < scan_limit) {
        ++scanned;
        if ((filter.kind && *filter.kind != it->kind) ||
            (filter.channel && *filter.channel != it->channel)) {
            ++it;
            continue;
        }
        const auto bytes = it->payload.size();
        result.push_back(std::move(*it));
        it = state.inbox.erase(it);
        state.receive_bytes -= bytes;
        --state.receive_messages;
    }
    return result;
}
void ProtocolPeer::close() noexcept {
    if (!impl_) {
        return;
    }
    impl_->outgoing.clear();
    impl_->transmissions.clear();
    impl_->send_messages.clear();
    impl_->assemblies.clear();
    impl_->inbox.clear();
    impl_->send_bytes = 0;
    impl_->receive_bytes = 0;
    impl_->receive_messages = 0;
    impl_->failure = ProtocolError::Failed;
}
ProtocolService::ProtocolService() = default;
ProtocolPeer* ProtocolService::peer() noexcept {
    return peer_.get();
}
const ProtocolPeer* ProtocolService::peer() const noexcept {
    return peer_.get();
}
void ProtocolService::open(ge::PersistentId connection, ProtocolLimits limits) {
    auto replacement = std::make_unique<ProtocolPeer>(connection, limits);
    peer_ = std::move(replacement);
}
void ProtocolService::close() noexcept {
    peer_.reset();
}

namespace {
template<class T>
bool parse_number(std::string_view text, T& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
std::optional<DeliveryMode> parse_mode(std::string_view text) {
    for (std::uint8_t index = 0; index < mode_count; ++index) {
        const auto mode = static_cast<DeliveryMode>(index);
        if (text == delivery_mode_name(mode)) {
            return mode;
        }
    }
    std::uint32_t number{};
    if (parse_number(text, number) && number < mode_count) {
        return static_cast<DeliveryMode>(number);
    }
    return std::nullopt;
}
ge::core::CommandResult error_result(ProtocolError error) {
    return {false, std::string(protocol_error_name(error))};
}
ProtocolPeer& command_peer(ge::core::Kernel& kernel) {
    auto* peer = kernel.services().get<ProtocolService>().peer();
    if (peer == nullptr) {
        throw std::logic_error("open a protocol peer first");
    }
    return *peer;
}
std::string describe_packet(const PacketDecodeResult& packet) {
    const auto& header = packet.header;
    std::ostringstream out;
    out << "connection=" << header.connection.value << " packet=" << header.packet_sequence
        << " channel=" << header.channel << " mode=" << delivery_mode_name(header.delivery)
        << " kind=" << static_cast<std::uint16_t>(header.message_kind)
        << " message=" << header.message_sequence << " fragment=" << header.fragment_index
        << '/' << header.fragment_count << " total=" << header.message_bytes
        << " ack-valid=" << ((header.flags & packet_ack_valid) != 0)
        << " ack=" << header.ack_sequence << " ack-bits=" << header.ack_bits
        << " payload=" << ge::core::encode_hex(packet.payload);
    return out.str();
}
} // namespace

void register_protocol(ge::core::Kernel& kernel) {
    kernel.register_module({"network.protocol", "Bounded datagram codec and reliable channels", 1});
    auto& service = kernel.services().emplace<ProtocolService>();
    kernel.add_command("network.protocol.open", [](ge::core::Kernel& host, ge::core::CommandArgs args) {
        std::uint64_t identity{};
        if (args.size() != 1 || !parse_number(args[0], identity) || identity == 0) {
            return ge::core::CommandResult{false, "usage: network.protocol.open <nonzero-connection-id>"};
        }
        host.services().get<ProtocolService>().open(ge::PersistentId{identity});
        return ge::core::CommandResult{true, "protocol connection opened; CRC provides no authentication"};
    }, "Create an isolated command-facing protocol connection");
    kernel.add_command("network.protocol.close", [](ge::core::Kernel& host, ge::core::CommandArgs args) {
        if (!args.empty()) {
            return ge::core::CommandResult{false, "usage: network.protocol.close"};
        }
        host.services().get<ProtocolService>().close();
        return ge::core::CommandResult{true, "protocol connection closed"};
    }, "Discard the command-facing connection and its retained data");
    kernel.add_command("network.protocol.enqueue", [](ge::core::Kernel& host, ge::core::CommandArgs args) {
        std::uint16_t channel{};
        std::uint16_t kind{};
        const auto mode = args.size() == 4 ? parse_mode(args[1]) : std::nullopt;
        if (args.size() != 4 || !mode || !parse_number(args[0], channel) ||
            !parse_number(args[2], kind) || kind == 0) {
            return ge::core::CommandResult{false,
                "usage: network.protocol.enqueue <channel> <mode> <kind> <payload-hex>"};
        }
        auto& peer = command_peer(host);
        const auto payload = ge::core::decode_hex(args[3], peer.limits().max_message_bytes);
        const auto result = peer.enqueue(channel, static_cast<MessageKind>(kind), payload, *mode);
        if (!result) {
            return error_result(result.error);
        }
        return ge::core::CommandResult{true, "message=" + std::to_string(result.message_sequence) +
            " fragments=" + std::to_string(result.fragment_count)};
    }, "Queue an owning message; modes: unreliable, sequenced, reliable-unordered, reliable-ordered");
    kernel.add_command("network.protocol.pump", [](ge::core::Kernel& host, ge::core::CommandArgs args) {
        double now{};
        std::size_t count = 8;
        if (args.empty() || args.size() > 2 || !parse_number(args[0], now) ||
            (args.size() == 2 && !parse_number(args[1], count)) || count > 32) {
            return ge::core::CommandResult{false, "usage: network.protocol.pump <seconds> [count<=32]"};
        }
        auto& peer = command_peer(host);
        const auto datagrams = peer.pump(now, count, count * max_wire_datagram_bytes);
        std::ostringstream out;
        out << "datagrams=" << datagrams.size() << " failure=" << protocol_error_name(peer.failure());
        for (const auto& datagram : datagrams) {
            out << '\n' << ge::core::encode_hex(datagram.bytes);
        }
        return ge::core::CommandResult{peer.failure() == ProtocolError::None, out.str()};
    }, "Advance caller time, expire state, and emit complete wire datagrams as hex");
    kernel.add_command("network.protocol.receive", [](ge::core::Kernel& host, ge::core::CommandArgs args) {
        double now{};
        if (args.size() != 2 || !parse_number(args[0], now)) {
            return ge::core::CommandResult{false, "usage: network.protocol.receive <seconds> <datagram-hex>"};
        }
        const auto wire = ge::core::decode_hex(args[1], max_wire_datagram_bytes);
        const auto result = command_peer(host).receive(wire, now);
        if (!result) {
            return error_result(result.error);
        }
        return ge::core::CommandResult{true, "accepted=" + std::to_string(result.accepted) +
            " duplicate=" + std::to_string(result.duplicate) +
            " completed=" + std::to_string(result.completed_messages)};
    }, "Decode and admit one supplied datagram without opening a socket");
    kernel.add_command("network.protocol.messages", [](ge::core::Kernel& host, ge::core::CommandArgs args) {
        std::size_t count = 8;
        if (args.size() > 1 || (!args.empty() && !parse_number(args[0], count)) || count > 32) {
            return ge::core::CommandResult{false, "usage: network.protocol.messages [count<=32]"};
        }
        const auto messages = command_peer(host).take_messages(count);
        std::ostringstream out;
        out << "messages=" << messages.size();
        for (const auto& message : messages) {
            out << "\nchannel=" << message.channel << " kind=" << static_cast<std::uint16_t>(message.kind)
                << " mode=" << delivery_mode_name(message.delivery) << " sequence=" << message.sequence
                << " payload=" << ge::core::encode_hex(message.payload);
        }
        return ge::core::CommandResult{true, out.str()};
    }, "Consume completed messages and release their receive reservations");
    kernel.add_command("network.protocol.inspect", [](ge::core::Kernel& host, ge::core::CommandArgs args) {
        if (!args.empty()) {
            return ge::core::CommandResult{false, "usage: network.protocol.inspect"};
        }
        const auto& peer = command_peer(host);
        const auto stats = peer.statistics();
        std::ostringstream out;
        out << "connection=" << peer.connection().value << " failure=" << protocol_error_name(peer.failure())
            << " sent=" << stats.datagrams_sent << " received=" << stats.datagrams_received
            << " pending-messages=" << stats.pending_messages << " pending-fragments=" << stats.pending_fragments
            << " send-bytes=" << stats.pending_send_bytes << " receive-bytes=" << stats.retained_receive_bytes
            << " retained-messages=" << stats.retained_receive_messages << " inflight=" << stats.inflight_fragments
            << " retries=" << stats.retransmissions << " acknowledged=" << stats.acknowledged_fragments
            << " malformed=" << stats.malformed_datagrams << " rejected=" << stats.rejected_datagrams
            << " completed=" << stats.completed_messages << " srtt=" << stats.smoothed_rtt_seconds
            << " rto=" << stats.retransmission_timeout_seconds;
        return ge::core::CommandResult{true, out.str()};
    }, "Inspect bounded queue accounting, acknowledgements, retries, and caller-time RTT");
    kernel.add_command("network.packet.decode", [](ge::core::Kernel&, ge::core::CommandArgs args) {
        if (args.size() != 1) {
            return ge::core::CommandResult{false, "usage: network.packet.decode <datagram-hex>"};
        }
        const auto wire = ge::core::decode_hex(args[0], max_wire_datagram_bytes);
        const auto packet = decode_packet(wire);
        return packet ? ge::core::CommandResult{true, describe_packet(packet)} : error_result(packet.error);
    }, "Inspect a version-1 datagram and validate structural bounds and CRC");
    kernel.add_command("network.packet.encode", [](ge::core::Kernel&, ge::core::CommandArgs args) {
        PacketHeader header;
        std::uint16_t kind{};
        const auto mode = args.size() == 7 ? parse_mode(args[2]) : std::nullopt;
        if (args.size() != 7 || !mode || !parse_number(args[0], header.connection.value) ||
            !parse_number(args[1], header.channel) || !parse_number(args[3], kind) ||
            !parse_number(args[4], header.packet_sequence) || !parse_number(args[5], header.message_sequence)) {
            return ge::core::CommandResult{false,
                "usage: network.packet.encode <connection> <channel> <mode> <kind> <packet> <message> <hex>"};
        }
        const auto payload = ge::core::decode_hex(args[6], max_fragment_payload_bytes);
        header.delivery = *mode;
        header.message_kind = static_cast<MessageKind>(kind);
        header.message_bytes = static_cast<std::uint32_t>(payload.size());
        const auto packet = encode_packet(header, payload);
        return packet ? ge::core::CommandResult{true, ge::core::encode_hex(packet.bytes)} : error_result(packet.error);
    }, "Encode a single-fragment application packet without sending it");
    kernel.add_system({"network.protocol.metrics", ge::core::Phase::EndFrame, 950, {}},
        [](ge::core::Kernel& host, const ge::core::Frame&) {
            const auto* peer = host.services().get<ProtocolService>().peer();
            const auto stats = peer == nullptr ? ProtocolStatistics{} : peer->statistics();
            host.metric("network.protocol.send_bytes", static_cast<double>(stats.pending_send_bytes));
            host.metric("network.protocol.receive_bytes", static_cast<double>(stats.retained_receive_bytes));
            host.metric("network.protocol.rto_seconds", stats.retransmission_timeout_seconds);
            host.metric("network.protocol.retransmissions", static_cast<double>(stats.retransmissions));
        });
    kernel.add_shutdown("network.protocol.close", [service_ptr = &service] { service_ptr->close(); });
}

} // namespace ge::network
