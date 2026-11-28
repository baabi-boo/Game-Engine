#include <ge/network/transport.hpp>

#include "test_support.hpp"

#include <array>

namespace {

void queued_datagrams_age_from_first_pump() {
    for (std::size_t first_send_budget = 0; first_send_budget < 2; ++first_send_budget) {
        ge::network::NetworkTransport transport;
        ge::network::SocketOptions options;
        const auto local = ge::network::parse_endpoint("127.0.0.1", 0);
        CHECK(local);
        options.local = *local;
        const auto opened = transport.open(options);
        CHECK(opened);

        const std::array payload{std::byte{1}};
        for (std::size_t i = 0; i <= first_send_budget; ++i) {
            CHECK(transport.send_datagram(opened.socket, opened.local, payload));
        }

        ge::network::PumpBudget budget;
        budget.receive_datagrams = 0;
        budget.protocol_peers = 0;
        budget.send_datagrams = first_send_budget;
        budget.io_operations = 1;
        const auto first = transport.pump(10.0, budget);
        CHECK(first);
        CHECK(first.sent == first_send_budget);
        CHECK(transport.statistics().queued_datagrams == 1);

        budget.send_datagrams = 1;
        const auto later = transport.pump(10.0 + transport.limits().queued_lifetime_seconds + 1.0, budget);
        CHECK(later);
        CHECK(later.sent == 0);
        const auto stats = transport.statistics();
        CHECK(stats.expired_datagrams == 1);
        CHECK(stats.dropped_datagrams == 1);
        CHECK(stats.queued_datagrams == 0);
        CHECK(stats.queued_bytes == 0);
        CHECK(stats.datagrams_sent == first_send_budget);
    }
}

} // namespace

int main() {
    return ge::test::run("network_transport_audit", {
        {"queued_datagrams_age_from_first_pump", queued_datagrams_age_from_first_pump},
    });
}
