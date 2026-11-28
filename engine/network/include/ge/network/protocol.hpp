#pragma once

#include <ge/core/identity.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::network {

// Version 1 uses network byte order, a fixed header, and a CRC over header plus
// payload with the CRC field zeroed. CRC detects corruption, NOT authentication.
inline constexpr std::uint32_t protocol_magic = 0x47454447U;
inline constexpr std::uint8_t protocol_version = 1;
inline constexpr std::size_t packet_header_bytes = 64;
inline constexpr std::size_t max_wire_datagram_bytes = 1200;
inline constexpr std::size_t max_fragment_payload_bytes =
    max_wire_datagram_bytes - packet_header_bytes;
inline constexpr std::uint16_t packet_ack_valid = 1;

enum class PacketKind : std::uint8_t { Data = 1, Acknowledgement = 2 };
enum class DeliveryMode : std::uint8_t {
    Unreliable = 0,
    Sequenced = 1,
    ReliableUnordered = 2,
    ReliableOrdered = 3
};
enum class MessageKind : std::uint16_t {
    Application = 1,
    Replication = 2,
    RemoteCall = 3,
    Session = 4
};
enum class ProtocolError {
    None,
    InvalidConfiguration,
    InvalidTime,
    InvalidArgument,
    Truncated,
    Oversized,
    BadMagic,
    UnsupportedVersion,
    InvalidHeader,
    ChecksumMismatch,
    WrongConnection,
    SendCapacity,
    ReceiveCapacity,
    SequenceWindow,
    FragmentConflict,
    RetryExhausted,
    ReliableExpired,
    Failed
};
[[nodiscard]] std::string_view protocol_error_name(ProtocolError error) noexcept;
[[nodiscard]] std::string_view delivery_mode_name(DeliveryMode mode) noexcept;
[[nodiscard]] bool reliable(DeliveryMode mode) noexcept;
[[nodiscard]] bool sequence_newer(std::uint32_t a, std::uint32_t b) noexcept;
[[nodiscard]] bool sequence_ambiguous(std::uint32_t a, std::uint32_t b) noexcept;

struct PacketHeader {
    PacketKind packet_kind{PacketKind::Data};
    DeliveryMode delivery{DeliveryMode::ReliableOrdered};
    std::uint16_t flags{};
    std::uint16_t channel{};
    ge::PersistentId connection{};
    std::uint32_t packet_sequence{};
    std::uint32_t ack_sequence{};
    std::uint64_t ack_bits{};
    std::uint32_t message_sequence{};
    std::uint32_t message_bytes{};
    std::uint16_t fragment_index{};
    std::uint16_t fragment_count{1};
    std::uint16_t payload_bytes{};
    MessageKind message_kind{MessageKind::Application};
};
struct PacketDecodeResult {
    ProtocolError error{ProtocolError::None};
    PacketHeader header{};
    // Borrows exactly the caller's input until that storage changes.
    std::span<const std::byte> payload{};
    [[nodiscard]] explicit operator bool() const noexcept;
};
struct PacketEncodeResult {
    ProtocolError error{ProtocolError::None};
    std::vector<std::byte> bytes;
    [[nodiscard]] explicit operator bool() const noexcept;
};
// Encoder derives payload_bytes; other fragment fields must be canonical.
[[nodiscard]] PacketEncodeResult encode_packet(
    PacketHeader header, std::span<const std::byte> payload);
[[nodiscard]] PacketDecodeResult decode_packet(std::span<const std::byte> wire);

// The newest sequence is implicit; bits 0..63 acknowledge newest-1..newest-64.
// Half-range differences are deliberately unordered and never admitted.
class AckWindow {
public:
    enum class Observation { New, Duplicate, TooOld, Ambiguous };
    [[nodiscard]] Observation classify(std::uint32_t sequence) const noexcept;
    [[nodiscard]] Observation observe(std::uint32_t sequence) noexcept;
    [[nodiscard]] bool contains(std::uint32_t sequence) const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint32_t newest() const noexcept;
    [[nodiscard]] std::uint64_t bits() const noexcept;
    void clear() noexcept;
    [[nodiscard]] static bool acknowledges(std::uint32_t sequence,
        std::uint32_t newest, std::uint64_t bits) noexcept;
private:
    bool initialized_{};
    std::uint32_t newest_{};
    std::uint64_t bits_{};
};

struct ProtocolLimits {
    std::size_t max_channels{16};
    std::size_t max_message_bytes{256U * 1024U};
    std::size_t max_pending_messages{128};
    std::size_t max_pending_fragments{1024};
    std::size_t max_send_bytes{2U * 1024U * 1024U};
    std::size_t max_receive_messages{128};
    std::size_t max_receive_bytes{2U * 1024U * 1024U};
    std::size_t max_inflight_fragments{64};
    std::size_t max_work_per_pump{2048};
    std::uint32_t max_transmissions{10};
    double initial_rto_seconds{0.25};
    double minimum_rto_seconds{0.05};
    double maximum_rto_seconds{4.0};
    double acknowledgement_delay_seconds{0.01};
    double reassembly_timeout_seconds{15.0};
    double reliable_lifetime_seconds{60.0};
};
[[nodiscard]] ProtocolError validate_protocol_limits(const ProtocolLimits& limits) noexcept;
struct ProtocolStatistics {
    std::uint64_t datagrams_sent{};
    std::uint64_t datagrams_received{};
    std::uint64_t wire_bytes_sent{};
    std::uint64_t wire_bytes_received{};
    std::uint64_t malformed_datagrams{};
    std::uint64_t duplicate_datagrams{};
    std::uint64_t rejected_datagrams{};
    std::uint64_t retransmissions{};
    std::uint64_t acknowledged_fragments{};
    std::uint64_t completed_messages{};
    std::uint64_t expired_messages{};
    std::uint64_t stale_messages{};
    std::size_t pending_messages{};
    std::size_t pending_fragments{};
    std::size_t pending_send_bytes{};
    std::size_t retained_receive_messages{};
    std::size_t retained_receive_bytes{};
    std::size_t inflight_fragments{};
    double smoothed_rtt_seconds{};
    double rtt_variation_seconds{};
    double retransmission_timeout_seconds{};
};
struct EnqueueResult {
    ProtocolError error{ProtocolError::None};
    std::uint32_t message_sequence{};
    std::size_t fragment_count{};
    [[nodiscard]] explicit operator bool() const noexcept;
};
struct ReceiveResult {
    ProtocolError error{ProtocolError::None};
    bool accepted{};
    bool duplicate{};
    std::size_t completed_messages{};
    [[nodiscard]] explicit operator bool() const noexcept;
};
struct WireDatagram {
    // Owns a complete datagram, always <= max_wire_datagram_bytes. Hand this
    // unchanged to UDP. A transport owns it after pump returns and may queue it.
    std::vector<std::byte> bytes;
    std::uint32_t packet_sequence{};
    bool retransmission{};
};
struct MessageFilter {
    std::optional<MessageKind> kind;
    std::optional<std::uint16_t> channel;
};
struct ReceivedMessage {
    std::uint16_t channel{};
    MessageKind kind{MessageKind::Application};
    DeliveryMode delivery{DeliveryMode::ReliableOrdered};
    std::uint32_t sequence{};
    std::vector<std::byte> payload;
};

// One connection, one owner thread, no sockets, wall clock, authentication,
// congestion-control guarantee, or automatic peer discovery. Both endpoints
// must use the same nonzero connection identity and compatible channel limits.
// All time inputs are finite nonnegative monotonic seconds from the caller.
// Reliable reassembly expiry is terminal: acknowledged fragments are never
// silently forgotten while the connection continues. Drain messages to release
// receive reservations. Mode sequences are independent within each channel.
// Reliable modes admit at most 64 message positions beyond the next contiguous
// completion; ordered delivery starts at sequence zero. Unreliable and sequenced
// messages may be dropped. Retransmits use a fresh packet sequence.
// ACKs for any retained attempt complete a fragment; each inflight fragment
// retains at most limits.max_transmissions packet identities until acknowledged.
// receive processes at most 65 ACK entries and 64 ordered publications per call.
// Allocation failures propagate; all allocations have configured size bounds.
class ProtocolPeer {
public:
    explicit ProtocolPeer(ge::PersistentId connection, ProtocolLimits limits = {});
    ~ProtocolPeer();
    ProtocolPeer(const ProtocolPeer&) = delete;
    ProtocolPeer& operator=(const ProtocolPeer&) = delete;
    ProtocolPeer(ProtocolPeer&&) noexcept;
    ProtocolPeer& operator=(ProtocolPeer&&) noexcept;
    [[nodiscard]] ge::PersistentId connection() const noexcept;
    [[nodiscard]] const ProtocolLimits& limits() const noexcept;
    [[nodiscard]] ProtocolError failure() const noexcept;
    [[nodiscard]] ProtocolStatistics statistics() const noexcept;
    [[nodiscard]] EnqueueResult enqueue(std::uint16_t channel, MessageKind kind,
        std::span<const std::byte> payload,
        DeliveryMode delivery = DeliveryMode::ReliableOrdered);
    [[nodiscard]] ReceiveResult receive(std::span<const std::byte> datagram,
        double now_seconds);
    // Emission is treated as transmission. Transport errors behave as loss;
    // reliable data stays retained until acknowledged or terminal failure.
    // Invalid pump time latches InvalidTime; invalid receive time only rejects.
    // Pump regularly even when no messages are queued to emit ACKs and expire state.
    [[nodiscard]] std::vector<WireDatagram> pump(double now_seconds,
        std::size_t max_datagrams = 64,
        std::size_t max_wire_bytes = 64U * max_wire_datagram_bytes);
    [[nodiscard]] std::vector<ReceivedMessage> take_messages(std::size_t limit = 64);
    // Unmatched messages retain their relative order and reservations. Each
    // filtered call scans from the inbox front; use scan_limit >=
    // limits().max_receive_messages to cover every retained message in one poll.
    [[nodiscard]] std::vector<ReceivedMessage> take_messages(MessageFilter filter,
        std::size_t limit = 64, std::size_t scan_limit = 1024);
    // Explicitly abandons connection state and all retained data. Identity is
    // never silently reused; callers create a fresh peer for a new session.
    void close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A command-facing peer instance. Transports create separate ProtocolPeers and
// retain ownership; this service does not consume any transport's messages.
class ProtocolService {
public:
    ProtocolService();
    [[nodiscard]] ProtocolPeer* peer() noexcept;
    [[nodiscard]] const ProtocolPeer* peer() const noexcept;
    void open(ge::PersistentId connection, ProtocolLimits limits = {});
    void close() noexcept;
private:
    std::unique_ptr<ProtocolPeer> peer_;
};
void register_protocol(ge::core::Kernel& kernel);

} // namespace ge::network
