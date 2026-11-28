#include <ge/network/protocol.hpp>

#include "test_support.hpp"

#include <array>

namespace {

using namespace ge::network;
constexpr ge::PersistentId connection{47};
constexpr std::array payload{std::byte{0x47}};

void delayed_acknowledgement_completes_retransmitted_fragment() {
    ProtocolLimits limits;
    limits.max_transmissions = 2;
    ProtocolPeer sender(connection, limits);
    ProtocolPeer receiver(connection, limits);
    CHECK(sender.enqueue(0, MessageKind::Application, payload));
    const auto first = sender.pump(0.0);
    CHECK(first.size() == 1);
    CHECK(receiver.receive(first.front().bytes, 0.0));
    const auto delayed_ack = receiver.pump(0.02);
    CHECK(delayed_ack.size() == 1);

    const auto retry = sender.pump(0.25);
    CHECK(retry.size() == 1);
    CHECK(retry.front().retransmission);
    CHECK(retry.front().packet_sequence != first.front().packet_sequence);
    CHECK(sender.receive(delayed_ack.front().bytes, 0.30));
    CHECK(sender.statistics().pending_fragments == 0);
    CHECK(sender.statistics().pending_send_bytes == 0);
    CHECK(sender.statistics().inflight_fragments == 0);
    CHECK(sender.statistics().acknowledged_fragments == 1);
    CHECK(sender.pump(1.0).empty());
    CHECK(sender.failure() == ProtocolError::None);
    CHECK(receiver.take_messages().size() == 1);
}

void retry_history_preserves_inflight_fragment_capacity() {
    ProtocolLimits limits;
    limits.max_inflight_fragments = 1;
    ProtocolPeer sender(connection, limits);
    ProtocolPeer receiver(connection, limits);
    CHECK(sender.enqueue(0, MessageKind::Application, payload));
    CHECK(sender.enqueue(0, MessageKind::Application, payload));
    const auto first = sender.pump(0.0);
    CHECK(first.size() == 1);
    CHECK(receiver.receive(first.front().bytes, 0.0));
    const auto original_ack = receiver.pump(0.02);
    CHECK(original_ack.size() == 1);

    const auto retry = sender.pump(0.25);
    CHECK(retry.size() == 1);
    CHECK(retry.front().retransmission);
    CHECK(sender.statistics().inflight_fragments == 1);
    CHECK(receiver.receive(retry.front().bytes, 0.25));
    const auto retry_ack = receiver.pump(0.27);
    CHECK(retry_ack.size() == 1);
    CHECK(sender.receive(retry_ack.front().bytes, 0.30));
    CHECK(sender.statistics().inflight_fragments == 0);
    CHECK(sender.statistics().pending_fragments == 1);

    const auto second = sender.pump(0.30);
    CHECK(second.size() == 1);
    CHECK(!second.front().retransmission);
    CHECK(sender.statistics().inflight_fragments == 1);
    CHECK(sender.receive(original_ack.front().bytes, 0.31));
    CHECK(sender.statistics().pending_fragments == 1);
    CHECK(sender.statistics().acknowledged_fragments == 1);
    CHECK(receiver.receive(second.front().bytes, 0.31));
    const auto second_ack = receiver.pump(0.33);
    CHECK(second_ack.size() == 1);
    CHECK(sender.receive(second_ack.front().bytes, 0.34));
    CHECK(sender.statistics().inflight_fragments == 0);
    CHECK(sender.statistics().pending_fragments == 0);
    CHECK(sender.statistics().acknowledged_fragments == 2);
    CHECK(receiver.take_messages().size() == 2);
}

} // namespace

int main() {
    return ge::test::run("network_protocol_audit", {
        {"delayed_ack_after_retransmission", delayed_acknowledgement_completes_retransmitted_fragment},
        {"retry_history_and_inflight_capacity", retry_history_preserves_inflight_fragment_capacity},
    });
}
