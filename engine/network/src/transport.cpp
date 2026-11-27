#include <ge/network/transport.hpp>
#include <ge/core/runtime.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace ge::network {
namespace {
constexpr std::size_t socket_capacity = 128;
constexpr std::size_t peer_capacity = 4096;
constexpr std::size_t maximum_queue_entries = 65536;
constexpr std::size_t maximum_queue_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_protocol_bytes = 1024U * 1024U * 1024U;

template<class T>
bool parse_number(std::string_view text, T& value, int base = 10) noexcept {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_ipv4(std::string_view text, std::array<std::uint8_t, 4>& bytes) noexcept {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto separator = text.find('.');
        if ((i != 3 && separator == std::string_view::npos) ||
            (i == 3 && separator != std::string_view::npos)) return false;
        const auto field = text.substr(0, separator);
        unsigned value{};
        if (field.size() > 3 || (field.size() > 1 && field.front() == '0') ||
            !parse_number(field, value) || value > 255) return false;
        bytes[i] = static_cast<std::uint8_t>(value);
        if (i != 3) text.remove_prefix(separator + 1);
    }
    return true;
}

bool parse_groups(std::string_view text, std::array<std::uint16_t, 8>& groups,
                  std::size_t& count, bool dotted_tail) noexcept {
    count = 0;
    while (!text.empty()) {
        const auto end = text.find(':');
        const auto field = text.substr(0, end);
        if (field.find('.') != std::string_view::npos) {
            std::array<std::uint8_t, 4> bytes{};
            if (!dotted_tail || end != std::string_view::npos || count > 6 ||
                !parse_ipv4(field, bytes)) return false;
            groups[count++] = static_cast<std::uint16_t>(
                (static_cast<unsigned>(bytes[0]) << 8U) | bytes[1]);
            groups[count++] = static_cast<std::uint16_t>(
                (static_cast<unsigned>(bytes[2]) << 8U) | bytes[3]);
        } else {
            unsigned value{};
            if (count >= 8 || field.size() > 4 || !parse_number(field, value, 16) ||
                value > 65535) return false;
            groups[count++] = static_cast<std::uint16_t>(value);
        }
        if (end == std::string_view::npos) return true;
        text.remove_prefix(end + 1);
        if (text.empty()) return false;
    }
    return true;
}

void increment(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}

bool room(std::size_t used, std::size_t amount, std::size_t capacity) noexcept {
    return used <= capacity && amount <= capacity - used;
}

bool endpoint_allowed(const SocketOptions& options, const Endpoint& remote) noexcept {
    return remote.valid_remote() && remote.family == options.local.family &&
        (options.access == EndpointAccess::Unicast || remote.loopback());
}

#ifdef _WIN32
using NativeSocket = SOCKET;
using AddressLength = int;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
int last_socket_error() noexcept { return WSAGetLastError(); }
void close_native(NativeSocket socket) noexcept { static_cast<void>(closesocket(socket)); }
#else
using NativeSocket = int;
using AddressLength = socklen_t;
constexpr NativeSocket invalid_socket = -1;
int last_socket_error() noexcept { return errno; }
void close_native(NativeSocket socket) noexcept { static_cast<void>(::close(socket)); }
#endif

TransportError classify_error(int error) noexcept {
#ifdef _WIN32
    switch (error) {
        case WSAEWOULDBLOCK: return TransportError::WouldBlock;
        case WSAEINTR: return TransportError::Interrupted;
        case WSAEACCES: return TransportError::PermissionDenied;
        case WSAEADDRINUSE: return TransportError::AddressInUse;
        case WSAEADDRNOTAVAIL: return TransportError::AddressUnavailable;
        case WSAEMSGSIZE: return TransportError::MessageTooLarge;
        case WSAENOBUFS: return TransportError::WouldBlock;
        case WSAENETDOWN:
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAECONNRESET: return TransportError::NetworkUnavailable;
        default: return TransportError::SocketFailure;
    }
#else
    if (error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS)
        return TransportError::WouldBlock;
    switch (error) {
        case EINTR: return TransportError::Interrupted;
        case EACCES:
        case EPERM: return TransportError::PermissionDenied;
        case EADDRINUSE: return TransportError::AddressInUse;
        case EADDRNOTAVAIL: return TransportError::AddressUnavailable;
        case EMSGSIZE: return TransportError::MessageTooLarge;
        case ENETDOWN:
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ECONNREFUSED: return TransportError::NetworkUnavailable;
        default: return TransportError::SocketFailure;
    }
#endif
}

NativeSocket create_native(int family) noexcept {
#ifdef _WIN32
    return WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#else
    int type = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif
    return ::socket(family, type, IPPROTO_UDP);
#endif
}

TransportResult native_failure() noexcept {
    const int code = last_socket_error();
    return {classify_error(code), code, ProtocolError::None};
}

struct NativeAddress {
    sockaddr_storage storage{};
    AddressLength size{};
};
NativeAddress native_address(const Endpoint& endpoint) noexcept {
    NativeAddress result;
    if (endpoint.family == AddressFamily::IPv4) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port);
        std::memcpy(&address.sin_addr, endpoint.address.data(), 4);
        std::memcpy(&result.storage, &address, sizeof(address));
        result.size = static_cast<AddressLength>(sizeof(address));
    } else {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(endpoint.port);
        address.sin6_scope_id = endpoint.scope_id;
        std::memcpy(&address.sin6_addr, endpoint.address.data(), 16);
        std::memcpy(&result.storage, &address, sizeof(address));
        result.size = static_cast<AddressLength>(sizeof(address));
    }
    return result;
}

std::optional<Endpoint> endpoint_from_native(const sockaddr_storage& storage,
                                            AddressLength size) noexcept {
    Endpoint result;
    if (storage.ss_family == AF_INET && size >= static_cast<AddressLength>(sizeof(sockaddr_in))) {
        sockaddr_in address{};
        std::memcpy(&address, &storage, sizeof(address));
        result.port = ntohs(address.sin_port);
        std::memcpy(result.address.data(), &address.sin_addr, 4);
    } else if (storage.ss_family == AF_INET6 &&
               size >= static_cast<AddressLength>(sizeof(sockaddr_in6))) {
        sockaddr_in6 address{};
        std::memcpy(&address, &storage, sizeof(address));
        result.family = AddressFamily::IPv6;
        result.port = ntohs(address.sin6_port);
        result.scope_id = address.sin6_scope_id;
        std::memcpy(result.address.data(), &address.sin6_addr, 16);
    } else return std::nullopt;
    return result.valid() ? std::optional<Endpoint>{result} : std::nullopt;
}

bool set_socket_integer(NativeSocket socket, int level, int option, int value) noexcept {
#ifdef _WIN32
    return setsockopt(socket, level, option, reinterpret_cast<const char*>(&value),
                      static_cast<int>(sizeof(value))) == 0;
#else
    return setsockopt(socket, level, option, &value,
                      static_cast<socklen_t>(sizeof(value))) == 0;
#endif
}

int socket_integer(NativeSocket socket, int option) noexcept {
    int value{};
    AddressLength size = static_cast<AddressLength>(sizeof(value));
#ifdef _WIN32
    const int status = getsockopt(socket, SOL_SOCKET, option,
                                  reinterpret_cast<char*>(&value), &size);
#else
    const int status = getsockopt(socket, SOL_SOCKET, option, &value, &size);
#endif
    return status == 0 ? value : 0;
}

bool set_nonblocking(NativeSocket socket) noexcept {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    const int descriptor_flags = fcntl(socket, F_GETFD, 0);
    return descriptor_flags >= 0 &&
        fcntl(socket, F_SETFD, descriptor_flags | FD_CLOEXEC) >= 0;
#endif
}

TransportResult transmit(NativeSocket socket, const Endpoint& destination,
                         std::span<const std::byte> bytes) noexcept {
    const auto address = native_address(destination);
#ifdef _WIN32
    const int sent = sendto(socket, reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()), 0,
        reinterpret_cast<const sockaddr*>(&address.storage), address.size);
    if (sent == SOCKET_ERROR) return native_failure();
#else
    const auto sent = sendto(socket, bytes.data(), bytes.size(), 0,
        reinterpret_cast<const sockaddr*>(&address.storage), address.size);
    if (sent < 0) return native_failure();
#endif
    if (static_cast<std::size_t>(sent) != bytes.size()) return {TransportError::SocketFailure};
    return {};
}

struct NativeReceive {
    TransportResult result{};
    Endpoint remote{};
    std::size_t size{};
    bool truncated{};
};
NativeReceive receive_native(NativeSocket socket, std::span<std::byte> bytes) noexcept {
    NativeReceive result;
    sockaddr_storage source{};
    AddressLength source_size = static_cast<AddressLength>(sizeof(source));
#ifdef _WIN32
    const int received = recvfrom(socket, reinterpret_cast<char*>(bytes.data()),
        static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&source), &source_size);
    if (received == SOCKET_ERROR) {
        result.result = native_failure();
        result.truncated = result.result.error == TransportError::MessageTooLarge;
        return result;
    }
#else
    iovec buffer{};
    buffer.iov_base = bytes.data();
    buffer.iov_len = bytes.size();
    msghdr message{};
    message.msg_name = &source;
    message.msg_namelen = source_size;
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    const auto received = recvmsg(socket, &message, 0);
    if (received < 0) {
        result.result = native_failure();
        return result;
    }
    source_size = message.msg_namelen;
    result.truncated = (message.msg_flags & MSG_TRUNC) != 0;
#endif
    result.size = static_cast<std::size_t>(received);
    const auto endpoint = endpoint_from_native(source, source_size);
    if (!endpoint) result.result.error = TransportError::InvalidEndpoint;
    else result.remote = *endpoint;
    return result;
}

std::string handle_text(ge::RuntimeHandle handle) {
    return std::to_string(handle.index) + ":" + std::to_string(handle.generation);
}
bool parse_handle(std::string_view text, ge::RuntimeHandle& result) noexcept {
    const auto separator = text.find(':');
    return separator != std::string_view::npos &&
        parse_number(text.substr(0, separator), result.index) &&
        parse_number(text.substr(separator + 1), result.generation) && result.valid();
}
core::CommandResult command_result(TransportResult result, std::string success) {
    if (result) return {true, std::move(success)};
    std::string message{transport_error_name(result.error)};
    if (result.native_error != 0) message += " native=" + std::to_string(result.native_error);
    if (result.protocol_error != ProtocolError::None)
        message += " protocol=" + std::string{protocol_error_name(result.protocol_error)};
    return {false, std::move(message)};
}
} // namespace

bool Endpoint::valid() const noexcept {
    if (family == AddressFamily::IPv4)
        return scope_id == 0 && std::all_of(address.begin() + 4, address.end(),
                                          [](std::uint8_t byte) { return byte == 0; });
    return family == AddressFamily::IPv6 && !mapped_ipv4();
}
bool Endpoint::unspecified() const noexcept {
    return std::all_of(address.begin(), address.end(), [](std::uint8_t byte) { return byte == 0; });
}
bool Endpoint::loopback() const noexcept {
    if (family == AddressFamily::IPv4) return address[0] == 127;
    return address[15] == 1 && std::all_of(address.begin(), address.end() - 1,
                                         [](std::uint8_t byte) { return byte == 0; });
}
bool Endpoint::multicast() const noexcept {
    return family == AddressFamily::IPv4 ? address[0] >= 224 && address[0] <= 239
                                         : address[0] == 255;
}
bool Endpoint::mapped_ipv4() const noexcept {
    return family == AddressFamily::IPv6 && address[10] == 255 && address[11] == 255 &&
        std::all_of(address.begin(), address.begin() + 10, [](std::uint8_t byte) { return byte == 0; });
}
bool Endpoint::valid_remote() const noexcept {
    const bool broadcast = family == AddressFamily::IPv4 &&
        std::all_of(address.begin(), address.begin() + 4, [](std::uint8_t byte) { return byte == 255; });
    return valid() && port != 0 && !unspecified() && !multicast() && !broadcast;
}

std::optional<Endpoint> parse_endpoint(std::string_view address, std::uint16_t port) noexcept {
    if (address.empty() || address.size() > 80) return std::nullopt;
    Endpoint result;
    result.port = port;
    if (address.front() == '[') {
        if (address.back() != ']') return std::nullopt;
        address = address.substr(1, address.size() - 2);
    }
    if (address.find(':') == std::string_view::npos) {
        std::array<std::uint8_t, 4> bytes{};
        if (!parse_ipv4(address, bytes)) return std::nullopt;
        std::copy(bytes.begin(), bytes.end(), result.address.begin());
        return result;
    }
    result.family = AddressFamily::IPv6;
    const auto scope = address.find('%');
    if (scope != std::string_view::npos) {
        if (!parse_number(address.substr(scope + 1), result.scope_id)) return std::nullopt;
        address = address.substr(0, scope);
    }
    std::array<std::uint16_t, 8> left{}, right{}, groups{};
    std::size_t left_count{}, right_count{};
    const auto compression = address.find("::");
    if (compression == std::string_view::npos) {
        if (!parse_groups(address, groups, left_count, true) || left_count != 8) return std::nullopt;
    } else {
        if (address.find("::", compression + 2) != std::string_view::npos ||
            !parse_groups(address.substr(0, compression), left, left_count, false) ||
            !parse_groups(address.substr(compression + 2), right, right_count, true) ||
            left_count + right_count >= 8) return std::nullopt;
        std::copy_n(left.begin(), left_count, groups.begin());
        std::copy_n(right.begin(), right_count, groups.end() - static_cast<std::ptrdiff_t>(right_count));
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        result.address[i * 2] = static_cast<std::uint8_t>(groups[i] >> 8U);
        result.address[i * 2 + 1] = static_cast<std::uint8_t>(groups[i] & 255U);
    }
    if (result.mapped_ipv4()) {
        if (result.scope_id != 0) return std::nullopt;
        std::copy_n(result.address.begin() + 12, 4, result.address.begin());
        std::fill(result.address.begin() + 4, result.address.end(), 0);
        result.family = AddressFamily::IPv4;
    }
    return result;
}

std::optional<Endpoint> parse_endpoint(std::string_view text) noexcept {
    std::string_view address;
    std::string_view port_text;
    if (!text.empty() && text.front() == '[') {
        const auto bracket = text.find(']');
        if (bracket == std::string_view::npos || bracket + 1 >= text.size() ||
            text[bracket + 1] != ':') return std::nullopt;
        address = text.substr(1, bracket - 1);
        port_text = text.substr(bracket + 2);
    } else {
        const auto separator = text.find(':');
        if (separator == std::string_view::npos) return std::nullopt;
        address = text.substr(0, separator);
        port_text = text.substr(separator + 1);
    }
    std::uint16_t port{};
    return parse_number(port_text, port) ? parse_endpoint(address, port) : std::nullopt;
}

std::string format_address(const Endpoint& endpoint) {
    if (!endpoint.valid()) return {};
    std::string result;
    if (endpoint.family == AddressFamily::IPv4) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (i != 0) result += '.';
            result += std::to_string(endpoint.address[i]);
        }
    } else {
        for (std::size_t i = 0; i < 8; ++i) {
            if (i != 0) result += ':';
            const unsigned word = (static_cast<unsigned>(endpoint.address[i * 2]) << 8U) |
                endpoint.address[i * 2 + 1];
            std::array<char, 4> digits{};
            const auto encoded = std::to_chars(digits.data(), digits.data() + digits.size(), word, 16);
            result.append(digits.data(), encoded.ptr);
        }
        if (endpoint.scope_id != 0) result += '%' + std::to_string(endpoint.scope_id);
    }
    return result;
}
std::string format_endpoint(const Endpoint& endpoint) {
    const auto address = format_address(endpoint);
    if (address.empty()) return {};
    return (endpoint.family == AddressFamily::IPv6 ? "[" + address + "]" : address) +
        ":" + std::to_string(endpoint.port);
}

std::string_view transport_error_name(TransportError error) noexcept {
    switch (error) {
        case TransportError::None: return "none";
        case TransportError::InvalidConfiguration: return "invalid_configuration";
        case TransportError::InvalidEndpoint: return "invalid_endpoint";
        case TransportError::InvalidHandle: return "invalid_handle";
        case TransportError::WrongThread: return "wrong_thread";
        case TransportError::InvalidTime: return "invalid_time";
        case TransportError::Capacity: return "capacity";
        case TransportError::QueueFull: return "queue_full";
        case TransportError::MessageTooLarge: return "message_too_large";
        case TransportError::PermissionDenied: return "permission_denied";
        case TransportError::AddressInUse: return "address_in_use";
        case TransportError::AddressUnavailable: return "address_unavailable";
        case TransportError::WouldBlock: return "would_block";
        case TransportError::Interrupted: return "interrupted";
        case TransportError::NetworkUnavailable: return "network_unavailable";
        case TransportError::SocketFailure: return "socket_failure";
        case TransportError::ProtocolFailure: return "protocol_failure";
        case TransportError::DuplicatePeer: return "duplicate_peer";
        case TransportError::OutOfMemory: return "out_of_memory";
    }
    return "unknown";
}
TransportError validate_transport_limits(const TransportLimits& limits) noexcept {
    if (limits.max_sockets == 0 || limits.max_sockets > socket_capacity ||
        limits.max_peers == 0 || limits.max_peers > peer_capacity ||
        limits.max_queued_datagrams == 0 || limits.max_queued_datagrams > maximum_queue_entries ||
        limits.max_queued_bytes < max_wire_datagram_bytes || limits.max_queued_bytes > maximum_queue_bytes ||
        limits.max_unrouted_datagrams > maximum_queue_entries ||
        limits.max_unrouted_bytes > maximum_queue_bytes ||
        limits.max_error_events > maximum_queue_entries ||
        limits.max_protocol_reserved_bytes > maximum_protocol_bytes ||
        !std::isfinite(limits.queued_lifetime_seconds) || limits.queued_lifetime_seconds <= 0.0 ||
        limits.queued_lifetime_seconds > 3600.0) return TransportError::InvalidConfiguration;
    return TransportError::None;
}

struct NetworkTransport::Impl {
    struct Outbound {
        PeerHandle peer{};
        Endpoint remote{};
        std::vector<std::byte> bytes;
        double queued_seconds{};
    };
    struct SocketState {
        NativeSocket native{invalid_socket};
        SocketHandle handle{};
        Endpoint local{};
        SocketOptions options{};
        std::deque<Outbound> outgoing;
        std::size_t queued_bytes{};
        int receive_buffer{};
        int send_buffer{};
        ~SocketState() { if (native != invalid_socket) close_native(native); }
    };
    struct PeerState {
        PeerHandle handle{};
        SocketHandle socket{};
        Endpoint remote{};
        std::unique_ptr<ProtocolPeer> protocol;
        std::size_t reserved_bytes{};
        bool failure_reported{};
    };
    TransportLimits limits;
    const std::thread::id owner{std::this_thread::get_id()};
    ge::GenerationPool socket_ids{static_cast<std::uint32_t>(socket_capacity)};
    ge::GenerationPool peer_ids{static_cast<std::uint32_t>(peer_capacity)};
    std::vector<std::unique_ptr<SocketState>> sockets{socket_capacity};
    std::vector<std::unique_ptr<PeerState>> peers{peer_capacity};
    std::map<std::pair<SocketHandle, Endpoint>, PeerHandle> routes;
    std::deque<InboundDatagram> unrouted;
    std::deque<TransportEvent> events;
    TransportStatistics stats{};
    std::size_t receive_cursor{};
    std::size_t send_cursor{};
    std::size_t peer_cursor{};
    double now{};
    bool has_time{};
    bool prioritize_send{};
#ifdef _WIN32
    bool winsock_started{};
#endif
    explicit Impl(TransportLimits value) : limits(value) {}
    ~Impl() {
        sockets.clear();
#ifdef _WIN32
        if (winsock_started) static_cast<void>(WSACleanup());
#endif
    }
    SocketState* get(SocketHandle handle) const noexcept {
        return socket_ids.alive(handle.value) ? sockets[handle.value.index - 1U].get() : nullptr;
    }
    PeerState* get(PeerHandle handle) const noexcept {
        return peer_ids.alive(handle.value) ? peers[handle.value.index - 1U].get() : nullptr;
    }
    TransportResult startup() noexcept {
#ifdef _WIN32
        if (!winsock_started) {
            WSADATA data{};
            const int status = WSAStartup(MAKEWORD(2, 2), &data);
            if (status != 0) return {TransportError::SocketFailure, status};
            if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
                static_cast<void>(WSACleanup());
                return {TransportError::SocketFailure, WSAVERNOTSUPPORTED};
            }
            winsock_started = true;
        }
#endif
        return {};
    }
    void report(SocketHandle socket, PeerHandle peer, TransportResult result) noexcept {
        if (events.size() >= limits.max_error_events) {
            increment(stats.lost_error_events);
            return;
        }
        try { events.push_back({socket, peer, result, now}); }
        catch (const std::bad_alloc&) { increment(stats.lost_error_events); }
        catch (const std::length_error&) { increment(stats.lost_error_events); }
    }
    TransportResult admit(SocketState& socket, Outbound packet) {
        if (stats.queued_datagrams >= limits.max_queued_datagrams ||
            !room(stats.queued_bytes, packet.bytes.size(), limits.max_queued_bytes))
            return {TransportError::QueueFull};
        const auto size = packet.bytes.size();
        socket.outgoing.push_back(std::move(packet));
        socket.queued_bytes += size;
        stats.queued_bytes += size;
        ++stats.queued_datagrams;
        return {};
    }
    void pop(SocketState& socket) noexcept {
        const auto size = socket.outgoing.front().bytes.size();
        socket.queued_bytes -= size;
        stats.queued_bytes -= size;
        --stats.queued_datagrams;
        socket.outgoing.pop_front();
    }
    void remove_peer(PeerHandle handle) noexcept {
        auto* peer = get(handle);
        if (!peer) return;
        if (auto* socket = get(peer->socket)) {
            for (auto it = socket->outgoing.begin(); it != socket->outgoing.end();) {
                if (it->peer != handle) { ++it; continue; }
                socket->queued_bytes -= it->bytes.size();
                stats.queued_bytes -= it->bytes.size();
                --stats.queued_datagrams;
                increment(stats.dropped_datagrams);
                it = socket->outgoing.erase(it);
            }
        }
        routes.erase({peer->socket, peer->remote});
        stats.protocol_reserved_bytes -= peer->reserved_bytes;
        peer->protocol->close();
        peers[handle.value.index - 1U].reset();
        static_cast<void>(peer_ids.release(handle.value));
        --stats.connected_peers;
    }
    void remove_socket(SocketHandle handle) noexcept {
        auto* socket = get(handle);
        if (!socket) return;
        for (const auto& peer : peers)
            if (peer && peer->socket == handle) remove_peer(peer->handle);
        while (!socket->outgoing.empty()) {
            pop(*socket);
            increment(stats.dropped_datagrams);
        }
        for (auto it = unrouted.begin(); it != unrouted.end();) {
            if (it->socket != handle) { ++it; continue; }
            stats.unrouted_queue_bytes -= it->bytes.size();
            --stats.unrouted_queue_size;
            it = unrouted.erase(it);
            increment(stats.dropped_datagrams);
        }
        sockets[handle.value.index - 1U].reset();
        static_cast<void>(socket_ids.release(handle.value));
        --stats.open_sockets;
    }
};

NetworkTransport::NetworkTransport(TransportLimits limits) {
    if (validate_transport_limits(limits) != TransportError::None)
        throw std::invalid_argument("invalid network transport limits");
    impl_ = std::make_unique<Impl>(limits);
}
NetworkTransport::~NetworkTransport() = default;
bool NetworkTransport::on_owner_thread() const noexcept {
    return impl_->owner == std::this_thread::get_id();
}
const TransportLimits& NetworkTransport::limits() const noexcept { return impl_->limits; }
TransportResult NetworkTransport::configure(const TransportLimits& limits) {
    if (!on_owner_thread()) return {TransportError::WrongThread};
    if (validate_transport_limits(limits) != TransportError::None)
        return {TransportError::InvalidConfiguration};
    if (impl_->stats.open_sockets != 0 || impl_->stats.connected_peers != 0)
        return {TransportError::Capacity};
    if (impl_->events.size() > limits.max_error_events) return {TransportError::Capacity};
    impl_->limits = limits;
    return {};
}

OpenResult NetworkTransport::open(const SocketOptions& options) {
    OpenResult result;
    if (!on_owner_thread()) { result.error = TransportError::WrongThread; return result; }
    if (!options.local.valid() || options.local.multicast() ||
        (options.access != EndpointAccess::LoopbackOnly && options.access != EndpointAccess::Unicast)) {
        result.error = TransportError::InvalidEndpoint;
        return result;
    }
    if (options.access == EndpointAccess::LoopbackOnly && !options.local.loopback()) {
        result.error = TransportError::PermissionDenied;
        return result;
    }
    if (options.receive_buffer_bytes == 0 || options.send_buffer_bytes == 0 ||
        options.receive_buffer_bytes > maximum_queue_bytes || options.send_buffer_bytes > maximum_queue_bytes) {
        result.error = TransportError::InvalidConfiguration;
        return result;
    }
    if (impl_->stats.open_sockets >= impl_->limits.max_sockets) {
        result.error = TransportError::Capacity;
        return result;
    }
    try {
        auto state = std::make_unique<Impl::SocketState>();
        static_cast<TransportResult&>(result) = impl_->startup();
        if (!result) return result;
        const int family = options.local.family == AddressFamily::IPv4 ? AF_INET : AF_INET6;
        state->native = create_native(family);
        if (state->native == invalid_socket) {
            static_cast<TransportResult&>(result) = native_failure();
            return result;
        }
        if (!set_nonblocking(state->native) ||
            !set_socket_integer(state->native, SOL_SOCKET, SO_RCVBUF, static_cast<int>(options.receive_buffer_bytes)) ||
            !set_socket_integer(state->native, SOL_SOCKET, SO_SNDBUF, static_cast<int>(options.send_buffer_bytes)) ||
            (options.reuse_address && !set_socket_integer(state->native, SOL_SOCKET, SO_REUSEADDR, 1)) ||
            (options.local.family == AddressFamily::IPv6 &&
             !set_socket_integer(state->native, IPPROTO_IPV6, IPV6_V6ONLY, 1))) {
            static_cast<TransportResult&>(result) = native_failure();
            return result;
        }
#ifdef _WIN32
        if (!options.reuse_address && !set_socket_integer(state->native, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1)) {
            static_cast<TransportResult&>(result) = native_failure();
            return result;
        }
#endif
        const auto address = native_address(options.local);
        if (::bind(state->native, reinterpret_cast<const sockaddr*>(&address.storage), address.size) != 0) {
            static_cast<TransportResult&>(result) = native_failure();
            return result;
        }
        sockaddr_storage actual{};
        AddressLength actual_size = static_cast<AddressLength>(sizeof(actual));
        if (getsockname(state->native, reinterpret_cast<sockaddr*>(&actual), &actual_size) != 0) {
            static_cast<TransportResult&>(result) = native_failure();
            return result;
        }
        const auto local = endpoint_from_native(actual, actual_size);
        if (!local) { result.error = TransportError::InvalidEndpoint; return result; }
        const auto identity = impl_->socket_ids.acquire();
        if (!identity) { result.error = TransportError::Capacity; return result; }
        state->handle = SocketHandle{*identity};
        state->local = *local;
        state->options = options;
        state->receive_buffer = socket_integer(state->native, SO_RCVBUF);
        state->send_buffer = socket_integer(state->native, SO_SNDBUF);
        result.socket = state->handle;
        result.local = state->local;
        impl_->sockets[identity->index - 1U] = std::move(state);
        ++impl_->stats.open_sockets;
    } catch (const std::bad_alloc&) { result.error = TransportError::OutOfMemory; }
      catch (const std::length_error&) { result.error = TransportError::OutOfMemory; }
    return result;
}
TransportResult NetworkTransport::close(SocketHandle socket) {
    if (!on_owner_thread()) return {TransportError::WrongThread};
    if (!impl_->get(socket)) return {TransportError::InvalidHandle};
    impl_->remove_socket(socket);
    return {};
}
void NetworkTransport::close_all() noexcept {
    if (!on_owner_thread()) return;
    for (const auto& socket : impl_->sockets)
        if (socket) impl_->remove_socket(socket->handle);
}

ConnectResult NetworkTransport::connect(SocketHandle socket, Endpoint remote,
                                       ge::PersistentId connection, ProtocolLimits limits) {
    ConnectResult result;
    if (!on_owner_thread()) { result.error = TransportError::WrongThread; return result; }
    const auto* local = impl_->get(socket);
    if (!local) { result.error = TransportError::InvalidHandle; return result; }
    if (!remote.valid_remote() || remote.family != local->local.family) {
        result.error = TransportError::InvalidEndpoint;
        return result;
    }
    if (!endpoint_allowed(local->options, remote)) { result.error = TransportError::PermissionDenied; return result; }
    if (!connection || validate_protocol_limits(limits) != ProtocolError::None) {
        result.error = TransportError::InvalidConfiguration;
        return result;
    }
    if (impl_->routes.contains({socket, remote})) { result.error = TransportError::DuplicatePeer; return result; }
    if (impl_->stats.connected_peers >= impl_->limits.max_peers ||
        !room(limits.max_send_bytes, limits.max_receive_bytes, impl_->limits.max_protocol_reserved_bytes)) {
        result.error = TransportError::Capacity;
        return result;
    }
    const auto reservation = limits.max_send_bytes + limits.max_receive_bytes;
    if (!room(impl_->stats.protocol_reserved_bytes, reservation, impl_->limits.max_protocol_reserved_bytes)) {
        result.error = TransportError::Capacity;
        return result;
    }
    std::optional<ge::RuntimeHandle> identity;
    try {
        auto state = std::make_unique<Impl::PeerState>();
        state->socket = socket;
        state->remote = remote;
        state->reserved_bytes = reservation;
        state->protocol = std::make_unique<ProtocolPeer>(connection, limits);
        identity = impl_->peer_ids.acquire();
        if (!identity) { result.error = TransportError::Capacity; return result; }
        state->handle = PeerHandle{*identity};
        impl_->routes.emplace(std::make_pair(socket, remote), state->handle);
        result.peer = state->handle;
        impl_->peers[identity->index - 1U] = std::move(state);
        ++impl_->stats.connected_peers;
        impl_->stats.protocol_reserved_bytes += reservation;
    } catch (const std::bad_alloc&) { result.error = TransportError::OutOfMemory; }
      catch (const std::length_error&) { result.error = TransportError::OutOfMemory; }
    if (!result && identity) static_cast<void>(impl_->peer_ids.release(*identity));
    return result;
}
TransportResult NetworkTransport::disconnect(PeerHandle peer) {
    if (!on_owner_thread()) return {TransportError::WrongThread};
    if (!impl_->get(peer)) return {TransportError::InvalidHandle};
    impl_->remove_peer(peer);
    return {};
}
SendResult NetworkTransport::send(PeerHandle peer, std::uint16_t channel, MessageKind kind,
                                  std::span<const std::byte> payload, DeliveryMode delivery) {
    SendResult result;
    if (!on_owner_thread()) { result.error = TransportError::WrongThread; return result; }
    auto* state = impl_->get(peer);
    if (!state) { result.error = TransportError::InvalidHandle; return result; }
    try {
        const auto queued = state->protocol->enqueue(channel, kind, payload, delivery);
        result.protocol_error = queued.error;
        result.message_sequence = queued.message_sequence;
        result.fragment_count = queued.fragment_count;
        if (!queued) result.error = queued.error == ProtocolError::SendCapacity
            ? TransportError::QueueFull : TransportError::ProtocolFailure;
    } catch (const std::bad_alloc&) { result.error = TransportError::OutOfMemory; }
      catch (const std::length_error&) { result.error = TransportError::OutOfMemory; }
    return result;
}
TransportResult NetworkTransport::send_datagram(SocketHandle socket, Endpoint remote,
                                               std::span<const std::byte> bytes) {
    if (!on_owner_thread()) return {TransportError::WrongThread};
    auto* state = impl_->get(socket);
    if (!state) return {TransportError::InvalidHandle};
    if (!remote.valid_remote() || remote.family != state->local.family) return {TransportError::InvalidEndpoint};
    if (!endpoint_allowed(state->options, remote)) return {TransportError::PermissionDenied};
    if (bytes.size() > max_wire_datagram_bytes) return {TransportError::MessageTooLarge};
    if (impl_->stats.queued_datagrams >= impl_->limits.max_queued_datagrams ||
        !room(impl_->stats.queued_bytes, bytes.size(), impl_->limits.max_queued_bytes)) return {TransportError::QueueFull};
    try {
        Impl::Outbound packet{{}, remote, {bytes.begin(), bytes.end()}, impl_->has_time ? impl_->now : -1.0};
        return impl_->admit(*state, std::move(packet));
    } catch (const std::bad_alloc&) { return {TransportError::OutOfMemory}; }
      catch (const std::length_error&) { return {TransportError::OutOfMemory}; }
}

PumpResult NetworkTransport::pump(double now_seconds, PumpBudget budget) {
    PumpResult result;
    if (!on_owner_thread()) { result.error = TransportError::WrongThread; return result; }
    if (!std::isfinite(now_seconds) || now_seconds < 0.0 ||
        (impl_->has_time && now_seconds < impl_->now)) {
        result.error = TransportError::InvalidTime;
        return result;
    }
    impl_->now = now_seconds;
    impl_->has_time = true;
    if (impl_->stats.open_sockets == 0) return result;
    budget.receive_datagrams = std::min(budget.receive_datagrams, maximum_queue_entries);
    budget.send_datagrams = std::min(budget.send_datagrams, maximum_queue_entries);
    budget.protocol_datagrams = std::min(budget.protocol_datagrams, maximum_queue_entries);
    budget.protocol_peers = std::min(budget.protocol_peers, peer_capacity);
    budget.io_operations = std::min(budget.io_operations, maximum_queue_entries * 4);
    budget.receive_wire_bytes = std::min(budget.receive_wire_bytes, maximum_queue_bytes);
    budget.send_wire_bytes = std::min(budget.send_wire_bytes, maximum_queue_bytes);
    budget.protocol_wire_bytes = std::min(budget.protocol_wire_bytes, maximum_queue_bytes);
    const bool may_send = budget.send_datagrams != 0 &&
        (impl_->stats.queued_datagrams != 0 || impl_->stats.connected_peers != 0);
    const auto send_reservation = may_send ? std::min(budget.send_datagrams,
        budget.io_operations / 2 + (impl_->prioritize_send ? budget.io_operations % 2 : 0)) : 0;
    const auto receive_io_limit = budget.io_operations - send_reservation;
    impl_->prioritize_send = !impl_->prioritize_send;
    std::array<bool, socket_capacity> receive_blocked{};
    std::array<std::byte, max_wire_datagram_bytes> buffer{};
    std::size_t misses{};
    while (result.received < budget.receive_datagrams && result.io_operations < receive_io_limit &&
           room(result.received_bytes, max_wire_datagram_bytes, budget.receive_wire_bytes)) {
        const auto index = impl_->receive_cursor;
        impl_->receive_cursor = (index + 1) % socket_capacity;
        auto* state = impl_->sockets[index].get();
        if (!state || receive_blocked[index]) {
            if (++misses >= socket_capacity) break;
            continue;
        }
        misses = 0;
        ++result.io_operations;
        const auto incoming = receive_native(state->native, buffer);
        if (incoming.truncated) {
            ++result.received;
            result.received_bytes += max_wire_datagram_bytes;
            increment(impl_->stats.datagrams_received);
            increment(impl_->stats.wire_bytes_received, max_wire_datagram_bytes);
            increment(impl_->stats.truncated_datagrams);
            increment(impl_->stats.dropped_datagrams);
            continue;
        }
        if (incoming.result.error == TransportError::InvalidEndpoint) {
            ++result.received;
            result.received_bytes += incoming.size;
            increment(impl_->stats.datagrams_received);
            increment(impl_->stats.wire_bytes_received, static_cast<std::uint64_t>(incoming.size));
            increment(impl_->stats.denied_datagrams);
            increment(impl_->stats.dropped_datagrams);
            continue;
        }
        if (!incoming.result) {
            receive_blocked[index] = true;
            if (incoming.result.error != TransportError::WouldBlock &&
                incoming.result.error != TransportError::Interrupted) {
                increment(impl_->stats.socket_errors);
                impl_->report(state->handle, {}, incoming.result);
            }
            continue;
        }
        ++result.received;
        result.received_bytes += incoming.size;
        increment(impl_->stats.datagrams_received);
        increment(impl_->stats.wire_bytes_received, static_cast<std::uint64_t>(incoming.size));
        if (!endpoint_allowed(state->options, incoming.remote)) {
            increment(impl_->stats.denied_datagrams);
            increment(impl_->stats.dropped_datagrams);
            continue;
        }
        const auto wire = std::span<const std::byte>{buffer}.first(incoming.size);
        const auto route = impl_->routes.find({state->handle, incoming.remote});
        try {
            if (route != impl_->routes.end()) {
                auto* peer = impl_->get(route->second);
                if (!peer) { increment(impl_->stats.dropped_datagrams); continue; }
                const auto received = peer->protocol->receive(wire, now_seconds);
                if (!received) {
                    increment(impl_->stats.protocol_rejections);
                    impl_->report(state->handle, peer->handle,
                        {TransportError::ProtocolFailure, 0, received.error});
                }
            } else {
                increment(impl_->stats.unrouted_datagrams);
                if (!state->options.receive_unrouted ||
                    impl_->unrouted.size() >= impl_->limits.max_unrouted_datagrams ||
                    !room(impl_->stats.unrouted_queue_bytes, wire.size(), impl_->limits.max_unrouted_bytes)) {
                    increment(impl_->stats.dropped_datagrams);
                    continue;
                }
                InboundDatagram packet{state->handle, incoming.remote, now_seconds,
                                        {wire.begin(), wire.end()}};
                impl_->unrouted.push_back(std::move(packet));
                ++impl_->stats.unrouted_queue_size;
                impl_->stats.unrouted_queue_bytes += wire.size();
            }
        } catch (const std::bad_alloc&) {
            increment(impl_->stats.dropped_datagrams);
            impl_->report(state->handle, {}, {TransportError::OutOfMemory});
        } catch (const std::length_error&) {
            increment(impl_->stats.dropped_datagrams);
            impl_->report(state->handle, {}, {TransportError::OutOfMemory});
        }
    }
    for (std::size_t scanned = 0; scanned < peer_capacity &&
         result.peers_pumped < budget.protocol_peers; ++scanned) {
        if (result.generated != 0 && (result.generated >= budget.protocol_datagrams ||
            !room(result.generated_bytes, packet_header_bytes, budget.protocol_wire_bytes))) break;
        const auto index = impl_->peer_cursor;
        impl_->peer_cursor = (index + 1) % peer_capacity;
        auto* peer = impl_->peers[index].get();
        if (!peer) continue;
        auto* socket = impl_->get(peer->socket);
        if (!socket) continue;
        ++result.peers_pumped;
        const auto packet_room = impl_->limits.max_queued_datagrams - impl_->stats.queued_datagrams;
        const auto byte_room = impl_->limits.max_queued_bytes - impl_->stats.queued_bytes;
        const auto count = std::min({packet_room, byte_room / max_wire_datagram_bytes,
            budget.protocol_datagrams - result.generated});
        const auto bytes = std::min(byte_room, budget.protocol_wire_bytes - result.generated_bytes);
        try {
            auto packets = peer->protocol->pump(now_seconds, count, bytes);
            for (auto& packet : packets) {
                ++result.generated;
                result.generated_bytes += packet.bytes.size();
                Impl::Outbound outgoing{peer->handle, peer->remote, std::move(packet.bytes), now_seconds};
                try {
                    const auto admission = impl_->admit(*socket, std::move(outgoing));
                    if (!admission) {
                        increment(impl_->stats.dropped_datagrams);
                        impl_->report(socket->handle, peer->handle, admission);
                    }
                } catch (const std::bad_alloc&) {
                    increment(impl_->stats.dropped_datagrams);
                    impl_->report(socket->handle, peer->handle, {TransportError::OutOfMemory});
                } catch (const std::length_error&) {
                    increment(impl_->stats.dropped_datagrams);
                    impl_->report(socket->handle, peer->handle, {TransportError::OutOfMemory});
                }
            }
            const auto failure = peer->protocol->failure();
            if (failure != ProtocolError::None && !peer->failure_reported) {
                peer->failure_reported = true;
                impl_->report(socket->handle, peer->handle, {TransportError::ProtocolFailure, 0, failure});
            }
        } catch (const std::bad_alloc&) {
            impl_->report(socket->handle, peer->handle, {TransportError::OutOfMemory});
        } catch (const std::length_error&) {
            impl_->report(socket->handle, peer->handle, {TransportError::OutOfMemory});
        }
    }
    std::array<bool, socket_capacity> send_blocked{};
    std::size_t attempted{};
    misses = 0;
    while (attempted < budget.send_datagrams && result.io_operations < budget.io_operations) {
        const auto index = impl_->send_cursor;
        impl_->send_cursor = (index + 1) % socket_capacity;
        auto* socket = impl_->sockets[index].get();
        if (!socket || socket->outgoing.empty() || send_blocked[index]) {
            if (++misses >= socket_capacity) break;
            continue;
        }
        misses = 0;
        ++attempted;
        auto& packet = socket->outgoing.front();
        if (packet.queued_seconds < 0.0) packet.queued_seconds = now_seconds;
        const auto* peer = packet.peer ? impl_->get(packet.peer) : nullptr;
        const bool stale = packet.peer && (!peer || peer->protocol->failure() != ProtocolError::None);
        if (stale || now_seconds - packet.queued_seconds > impl_->limits.queued_lifetime_seconds) {
            if (!stale) increment(impl_->stats.expired_datagrams);
            increment(impl_->stats.dropped_datagrams);
            impl_->pop(*socket);
            continue;
        }
        if (!room(result.sent_bytes, packet.bytes.size(), budget.send_wire_bytes)) {
            send_blocked[index] = true;
            result.budget_exhausted = true;
            continue;
        }
        ++result.io_operations;
        const auto sent = transmit(socket->native, packet.remote, packet.bytes);
        if (sent.error == TransportError::WouldBlock || sent.error == TransportError::Interrupted) {
            send_blocked[index] = true;
            increment(impl_->stats.send_backpressure);
            continue;
        }
        if (!sent) {
            increment(impl_->stats.socket_errors);
            increment(impl_->stats.dropped_datagrams);
            impl_->report(socket->handle, packet.peer, sent);
        } else {
            ++result.sent;
            result.sent_bytes += packet.bytes.size();
            increment(impl_->stats.datagrams_sent);
            increment(impl_->stats.wire_bytes_sent, static_cast<std::uint64_t>(packet.bytes.size()));
        }
        impl_->pop(*socket);
    }
    if ((impl_->stats.open_sockets != 0 &&
         (result.io_operations >= budget.io_operations ||
          result.received >= budget.receive_datagrams ||
          !room(result.received_bytes, max_wire_datagram_bytes, budget.receive_wire_bytes))) ||
        (impl_->stats.queued_datagrams != 0 && attempted >= budget.send_datagrams) ||
        impl_->stats.connected_peers > result.peers_pumped)
        result.budget_exhausted = true;
    if (result.budget_exhausted) increment(impl_->stats.budget_exhaustions);
    return result;
}

std::vector<ReceivedMessage> NetworkTransport::take_messages(PeerHandle peer, std::size_t limit) {
    return take_messages(peer, MessageFilter{}, limit, maximum_queue_entries);
}
std::vector<ReceivedMessage> NetworkTransport::take_messages(
    PeerHandle peer, MessageFilter filter, std::size_t limit, std::size_t scan_limit) {
    if (!on_owner_thread()) return {};
    auto* state = impl_->get(peer);
    if (!state) return {};
    return state->protocol->take_messages(filter, std::min(limit, maximum_queue_entries),
                                          std::min(scan_limit, maximum_queue_entries));
}
std::vector<InboundDatagram> NetworkTransport::take_unrouted(std::size_t limit) {
    if (!on_owner_thread()) return {};
    std::vector<InboundDatagram> result;
    const auto count = std::min(limit, impl_->unrouted.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto bytes = impl_->unrouted.front().bytes.size();
        result.push_back(std::move(impl_->unrouted.front()));
        impl_->unrouted.pop_front();
        --impl_->stats.unrouted_queue_size;
        impl_->stats.unrouted_queue_bytes -= bytes;
    }
    return result;
}
std::vector<TransportEvent> NetworkTransport::take_events(std::size_t limit) {
    if (!on_owner_thread()) return {};
    std::vector<TransportEvent> result;
    const auto count = std::min(limit, impl_->events.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(impl_->events.front());
        impl_->events.pop_front();
    }
    return result;
}
std::optional<SocketSnapshot> NetworkTransport::socket(SocketHandle handle) const {
    if (!on_owner_thread()) return std::nullopt;
    const auto* state = impl_->get(handle);
    if (!state) return std::nullopt;
    return SocketSnapshot{handle, state->local, state->options, state->outgoing.size(),
                           state->queued_bytes, state->receive_buffer, state->send_buffer};
}
std::optional<PeerSnapshot> NetworkTransport::peer(PeerHandle handle) const {
    if (!on_owner_thread()) return std::nullopt;
    const auto* state = impl_->get(handle);
    if (!state) return std::nullopt;
    return PeerSnapshot{handle, state->socket, state->remote, state->protocol->connection(),
                         state->protocol->failure(), state->protocol->statistics()};
}
std::optional<PeerHandle> NetworkTransport::find_peer(SocketHandle socket, const Endpoint& endpoint) const noexcept {
    if (!on_owner_thread() || !impl_->get(socket)) return std::nullopt;
    const auto found = impl_->routes.find({socket, endpoint});
    return found == impl_->routes.end() ? std::nullopt : std::optional<PeerHandle>{found->second};
}
std::vector<SocketHandle> NetworkTransport::sockets() const {
    if (!on_owner_thread()) return {};
    std::vector<SocketHandle> result;
    result.reserve(impl_->stats.open_sockets);
    for (const auto& state : impl_->sockets) if (state) result.push_back(state->handle);
    return result;
}
std::vector<PeerHandle> NetworkTransport::peers() const {
    if (!on_owner_thread()) return {};
    std::vector<PeerHandle> result;
    result.reserve(impl_->stats.connected_peers);
    for (const auto& state : impl_->peers) if (state) result.push_back(state->handle);
    return result;
}
TransportStatistics NetworkTransport::statistics() const noexcept {
    return on_owner_thread() ? impl_->stats : TransportStatistics{};
}

void register_transport(core::Kernel& kernel) {
    kernel.register_module({"network.transport", "Explicit native UDP sockets and bounded protocol routing", 1});
    auto& service = kernel.services().emplace<NetworkTransport>();
    kernel.add_shutdown("network.transport.close", [pointer = &service] { pointer->close_all(); });
    kernel.add_system({"network.transport.pump", core::Phase::Input, -10000, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& transport = host.services().get<NetworkTransport>();
            const auto result = transport.pump(frame.elapsed_seconds);
            if (!result) host.diagnose(core::Diagnostic::Severity::Warning,
                "network.transport", std::string{transport_error_name(result.error)});
            const auto stats = transport.statistics();
            host.metric("network.sockets", static_cast<double>(stats.open_sockets));
            host.metric("network.peers", static_cast<double>(stats.connected_peers));
            host.metric("network.queued_bytes", static_cast<double>(stats.queued_bytes));
            host.metric("network.dropped_datagrams", static_cast<double>(stats.dropped_datagrams));
            host.metric("network.sent_bytes", static_cast<double>(stats.wire_bytes_sent));
            host.metric("network.received_bytes", static_cast<double>(stats.wire_bytes_received));
        });
    kernel.add_command("network.transport.configure", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 5) return core::CommandResult{false,
            "usage: network.transport.configure sockets peers queued_datagrams queued_bytes protocol_reserved_bytes"};
        auto& transport = host.services().get<NetworkTransport>();
        auto limits = transport.limits();
        if (!parse_number(args[0], limits.max_sockets) || !parse_number(args[1], limits.max_peers) ||
            !parse_number(args[2], limits.max_queued_datagrams) || !parse_number(args[3], limits.max_queued_bytes) ||
            !parse_number(args[4], limits.max_protocol_reserved_bytes))
            return core::CommandResult{false, "invalid numeric limits"};
        return command_result(transport.configure(limits), "transport limits configured");
    }, "Configure bounded resources while all sockets are closed");
    kernel.add_command("network.transport.open", [](core::Kernel& host, core::CommandArgs args) {
        if (args.empty() || args.size() > 3) return core::CommandResult{false,
            "usage: network.transport.open numeric_endpoint [unicast] [unrouted]"};
        const auto local = parse_endpoint(args[0]);
        if (!local) return core::CommandResult{false, "invalid numeric endpoint; use [IPv6]:port"};
        SocketOptions options;
        options.local = *local;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "unicast") options.access = EndpointAccess::Unicast;
            else if (args[i] == "unrouted") options.receive_unrouted = true;
            else return core::CommandResult{false, "unknown socket option"};
        }
        const auto opened = host.services().get<NetworkTransport>().open(options);
        return command_result(opened, "socket=" + handle_text(opened.socket.value) +
            " local=" + format_endpoint(opened.local));
    }, "Explicitly bind a nonblocking UDP socket; default policy permits loopback only");
    kernel.add_command("network.transport.connect", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 3) return core::CommandResult{false,
            "usage: network.transport.connect socket_index:generation numeric_endpoint connection_id"};
        SocketHandle socket;
        ge::PersistentId connection;
        const auto remote = parse_endpoint(args[1]);
        if (!parse_handle(args[0], socket.value) || !remote || !parse_number(args[2], connection.value))
            return core::CommandResult{false, "invalid socket, endpoint or connection identity"};
        const auto connected = host.services().get<NetworkTransport>().connect(socket, *remote, connection);
        return command_result(connected, "peer=" + handle_text(connected.peer.value) +
            " route configured; session authorization remains with its consumer");
    }, "Configure one protocol peer at an exact endpoint; does not authenticate a session");
    kernel.add_command("network.transport.send", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 5) return core::CommandResult{false,
            "usage: network.transport.send peer channel kind unreliable|sequenced|unordered|ordered text"};
        PeerHandle peer;
        std::uint16_t channel{}, kind{};
        if (!parse_handle(args[0], peer.value) || !parse_number(args[1], channel) || !parse_number(args[2], kind))
            return core::CommandResult{false, "invalid peer, channel or kind"};
        DeliveryMode mode;
        if (args[3] == "unreliable") mode = DeliveryMode::Unreliable;
        else if (args[3] == "sequenced") mode = DeliveryMode::Sequenced;
        else if (args[3] == "unordered") mode = DeliveryMode::ReliableUnordered;
        else if (args[3] == "ordered") mode = DeliveryMode::ReliableOrdered;
        else return core::CommandResult{false, "unknown delivery mode"};
        const auto bytes = std::as_bytes(std::span<const char>{args[4].data(), args[4].size()});
        const auto sent = host.services().get<NetworkTransport>().send(
            peer, channel, static_cast<MessageKind>(kind), bytes, mode);
        return command_result(sent, "queued message=" + std::to_string(sent.message_sequence) +
            " fragments=" + std::to_string(sent.fragment_count));
    }, "Queue an explicitly requested framed message; admission is not remote delivery");
    kernel.add_command("network.transport.disconnect", [](core::Kernel& host, core::CommandArgs args) {
        PeerHandle peer;
        if (args.size() != 1 || !parse_handle(args[0], peer.value))
            return core::CommandResult{false, "usage: network.transport.disconnect peer_index:generation"};
        return command_result(host.services().get<NetworkTransport>().disconnect(peer), "peer disconnected");
    }, "Abandon a peer, its protocol state and queued output");
    kernel.add_command("network.transport.close", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) return core::CommandResult{false, "usage: network.transport.close socket|all"};
        auto& transport = host.services().get<NetworkTransport>();
        if (args[0] == "all") { transport.close_all(); return core::CommandResult{true, "all sockets closed"}; }
        SocketHandle socket;
        if (!parse_handle(args[0], socket.value)) return core::CommandResult{false, "invalid socket handle"};
        return command_result(transport.close(socket), "socket closed");
    }, "Close native sockets and invalidate all associated generation handles");
    kernel.add_command("network.transport.status", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) return core::CommandResult{false, "usage: network.transport.status"};
        const auto& transport = host.services().get<NetworkTransport>();
        const auto stats = transport.statistics();
        std::string status = "sockets=" + std::to_string(stats.open_sockets) +
            " peers=" + std::to_string(stats.connected_peers) +
            " queued=" + std::to_string(stats.queued_datagrams) +
            " queued_bytes=" + std::to_string(stats.queued_bytes) +
            " sent=" + std::to_string(stats.datagrams_sent) +
            " received=" + std::to_string(stats.datagrams_received) +
            " dropped=" + std::to_string(stats.dropped_datagrams) +
            " errors=" + std::to_string(stats.socket_errors);
        for (const auto handle : transport.sockets()) {
            const auto socket = transport.socket(handle);
            if (socket) status += "\nsocket " + handle_text(handle.value) + " " + format_endpoint(socket->local);
        }
        for (const auto handle : transport.peers()) {
            const auto peer = transport.peer(handle);
            if (peer) status += "\npeer " + handle_text(handle.value) + " " + format_endpoint(peer->remote) +
                " connection=" + std::to_string(peer->connection.value) +
                " failure=" + std::string{protocol_error_name(peer->failure)};
        }
        return core::CommandResult{true, std::move(status)};
    }, "Inspect sockets, exact peer routes, queues and cumulative transport counters");
}

} // namespace ge::network
