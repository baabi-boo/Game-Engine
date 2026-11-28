#pragma once

#include <ge/core/identity.hpp>
#include <ge/network/protocol.hpp>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::network {

enum class AddressFamily : std::uint8_t { IPv4, IPv6 };
struct Endpoint {
    AddressFamily family{AddressFamily::IPv4};
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port{};
    std::uint32_t scope_id{};
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool unspecified() const noexcept;
    [[nodiscard]] bool loopback() const noexcept;
    [[nodiscard]] bool multicast() const noexcept;
    [[nodiscard]] bool mapped_ipv4() const noexcept;
    [[nodiscard]] bool valid_remote() const noexcept;
    auto operator<=>(const Endpoint&) const = default;
};
// Numeric addresses only. IPv6 permits a numeric %scope, never a DNS name or
// interface lookup. The combined form requires brackets around IPv6 addresses.
[[nodiscard]] std::optional<Endpoint> parse_endpoint(
    std::string_view address, std::uint16_t port) noexcept;
[[nodiscard]] std::optional<Endpoint> parse_endpoint(std::string_view text) noexcept;
[[nodiscard]] std::string format_address(const Endpoint& endpoint);
[[nodiscard]] std::string format_endpoint(const Endpoint& endpoint);

struct SocketHandle {
    ge::RuntimeHandle value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value.valid(); }
    auto operator<=>(const SocketHandle&) const = default;
};
struct PeerHandle {
    ge::RuntimeHandle value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value.valid(); }
    auto operator<=>(const PeerHandle&) const = default;
};

enum class TransportError : std::uint8_t {
    None,
    InvalidConfiguration,
    InvalidEndpoint,
    InvalidHandle,
    WrongThread,
    InvalidTime,
    Capacity,
    QueueFull,
    MessageTooLarge,
    PermissionDenied,
    AddressInUse,
    AddressUnavailable,
    WouldBlock,
    Interrupted,
    NetworkUnavailable,
    SocketFailure,
    ProtocolFailure,
    DuplicatePeer,
    OutOfMemory
};
[[nodiscard]] std::string_view transport_error_name(TransportError error) noexcept;
struct TransportResult {
    TransportError error{TransportError::None};
    int native_error{};
    ProtocolError protocol_error{ProtocolError::None};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == TransportError::None;
    }
};
struct OpenResult : TransportResult {
    SocketHandle socket{};
    Endpoint local{};
};
struct ConnectResult : TransportResult {
    PeerHandle peer{};
};
struct SendResult : TransportResult {
    std::uint32_t message_sequence{};
    std::size_t fragment_count{};
};

enum class EndpointAccess : std::uint8_t { LoopbackOnly, Unicast };
struct SocketOptions {
    Endpoint local{};
    EndpointAccess access{EndpointAccess::LoopbackOnly};
    std::size_t receive_buffer_bytes{256U * 1024U};
    std::size_t send_buffer_bytes{256U * 1024U};
    bool reuse_address{};
    // Unrouted datagrams are untrusted opaque input and require explicit opt-in.
    // This flag admits raw input; it never admits a peer or authorizes commands.
    bool receive_unrouted{};
};
struct TransportLimits {
    std::size_t max_sockets{8};
    std::size_t max_peers{128};
    std::size_t max_queued_datagrams{2048};
    std::size_t max_queued_bytes{2U * 1024U * 1024U};
    std::size_t max_unrouted_datagrams{256};
    std::size_t max_unrouted_bytes{256U * max_wire_datagram_bytes};
    std::size_t max_error_events{128};
    // Each admitted peer reserves its configured send+receive byte ceilings.
    std::size_t max_protocol_reserved_bytes{64U * 1024U * 1024U};
    // Datagrams queued before the first valid pump begin aging at that pump.
    double queued_lifetime_seconds{2.0};
};
[[nodiscard]] TransportError validate_transport_limits(const TransportLimits& limits) noexcept;
// Datagram limits count receive attempts that consumed packets and send queue
// entries examined (including expiry). Native calls share io_operations; when
// output may be pending a share is reserved so a receive flood cannot starve it.
struct PumpBudget {
    std::size_t receive_datagrams{64};
    std::size_t send_datagrams{64};
    std::size_t protocol_datagrams{64};
    std::size_t protocol_peers{64};
    std::size_t io_operations{256};
    std::size_t receive_wire_bytes{64U * max_wire_datagram_bytes};
    std::size_t send_wire_bytes{64U * max_wire_datagram_bytes};
    std::size_t protocol_wire_bytes{64U * max_wire_datagram_bytes};
};
struct PumpResult : TransportResult {
    std::size_t received{};
    std::size_t sent{};
    std::size_t generated{};
    std::size_t received_bytes{};
    std::size_t sent_bytes{};
    std::size_t generated_bytes{};
    std::size_t io_operations{};
    std::size_t peers_pumped{};
    bool budget_exhausted{};
};
struct TransportStatistics {
    std::uint64_t datagrams_received{};
    std::uint64_t datagrams_sent{};
    std::uint64_t wire_bytes_received{};
    std::uint64_t wire_bytes_sent{};
    std::uint64_t truncated_datagrams{};
    std::uint64_t unrouted_datagrams{};
    std::uint64_t denied_datagrams{};
    std::uint64_t dropped_datagrams{};
    std::uint64_t protocol_rejections{};
    std::uint64_t send_backpressure{};
    std::uint64_t socket_errors{};
    std::uint64_t expired_datagrams{};
    std::uint64_t lost_error_events{};
    std::uint64_t budget_exhaustions{};
    std::size_t open_sockets{};
    std::size_t connected_peers{};
    std::size_t queued_datagrams{};
    std::size_t queued_bytes{};
    std::size_t unrouted_queue_size{};
    std::size_t unrouted_queue_bytes{};
    std::size_t protocol_reserved_bytes{};
};
struct SocketSnapshot {
    SocketHandle socket{};
    Endpoint local{};
    SocketOptions options{};
    std::size_t queued_datagrams{};
    std::size_t queued_bytes{};
    int actual_receive_buffer_bytes{};
    int actual_send_buffer_bytes{};
};
struct PeerSnapshot {
    PeerHandle peer{};
    SocketHandle socket{};
    Endpoint remote{};
    ge::PersistentId connection{};
    ProtocolError failure{ProtocolError::None};
    ProtocolStatistics protocol{};
};
struct InboundDatagram {
    SocketHandle socket{};
    Endpoint remote{};
    double received_seconds{};
    std::vector<std::byte> bytes;
};
struct TransportEvent {
    SocketHandle socket{};
    PeerHandle peer{};
    TransportResult result{};
    double seconds{};
};

// A single owner thread controls all methods, including queries and destruction.
// Handles are local to this service lifetime and never serialize on the wire.
// Construction/registration does not initialize Winsock, open a socket, resolve
// a name, bind a port, or contact a peer. open() is the native I/O opt-in.
// connect() installs an exact endpoint route, not authentication or a handshake.
// Endpoint/connection matches and CRC framing do not confer gameplay authority;
// session consumers must validate identity, permissions and message schemas.
class NetworkTransport {
public:
    explicit NetworkTransport(TransportLimits limits = {});
    ~NetworkTransport();
    NetworkTransport(const NetworkTransport&) = delete;
    NetworkTransport& operator=(const NetworkTransport&) = delete;
    NetworkTransport(NetworkTransport&&) = delete;
    NetworkTransport& operator=(NetworkTransport&&) = delete;
    [[nodiscard]] bool on_owner_thread() const noexcept;
    [[nodiscard]] const TransportLimits& limits() const noexcept;
    [[nodiscard]] TransportResult configure(const TransportLimits& limits);
    [[nodiscard]] OpenResult open(const SocketOptions& options);
    [[nodiscard]] TransportResult close(SocketHandle socket);
    void close_all() noexcept;
    [[nodiscard]] ConnectResult connect(SocketHandle socket, Endpoint remote,
        ge::PersistentId connection, ProtocolLimits limits = {});
    [[nodiscard]] TransportResult disconnect(PeerHandle peer);
    [[nodiscard]] SendResult send(PeerHandle peer, std::uint16_t channel,
        MessageKind kind, std::span<const std::byte> payload,
        DeliveryMode delivery = DeliveryMode::ReliableOrdered);
    // Raw datagrams use the same destination policy and queue caps. Callers own
    // framing; this is intended for explicitly authorized session negotiation.
    [[nodiscard]] TransportResult send_datagram(SocketHandle socket,
        Endpoint remote, std::span<const std::byte> bytes);
    // Socket errors are recorded in the bounded event queue; the returned error
    // reports a rejected pump call. Counters charge truncated input at 1200 bytes
    // because native truncation does not report its complete wire size uniformly.
    [[nodiscard]] PumpResult pump(double now_seconds, PumpBudget budget = {});
    [[nodiscard]] std::vector<ReceivedMessage> take_messages(
        PeerHandle peer, std::size_t limit = 64);
    [[nodiscard]] std::vector<ReceivedMessage> take_messages(
        PeerHandle peer, MessageFilter filter, std::size_t limit = 64,
        std::size_t scan_limit = 1024);
    [[nodiscard]] std::vector<InboundDatagram> take_unrouted(std::size_t limit = 64);
    [[nodiscard]] std::vector<TransportEvent> take_events(std::size_t limit = 64);
    [[nodiscard]] std::optional<SocketSnapshot> socket(SocketHandle handle) const;
    [[nodiscard]] std::optional<PeerSnapshot> peer(PeerHandle handle) const;
    [[nodiscard]] std::optional<PeerHandle> find_peer(
        SocketHandle socket, const Endpoint& endpoint) const noexcept;
    [[nodiscard]] std::vector<SocketHandle> sockets() const;
    [[nodiscard]] std::vector<PeerHandle> peers() const;
    [[nodiscard]] TransportStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_transport(ge::core::Kernel& kernel);

} // namespace ge::network
