#include "test_support.hpp"

#include <ge/world/streaming.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using namespace ge::world;

static_assert(!std::is_copy_constructible_v<StreamingPolicy>);
static_assert(!std::is_move_constructible_v<StreamingPolicy>);
static_assert(!std::is_copy_assignable_v<StreamingPolicy>);
static_assert(!std::is_move_assignable_v<StreamingPolicy>);

struct Snapshot {
    MemoryUsage memory;
    std::array<std::optional<RegionStatus>, 4> regions;
    std::size_t count;
    bool exhausted;
    bool operator==(const Snapshot&) const = default;
};

// Mutation tests use region IDs 1..4; count also detects unexpected metadata.
Snapshot snapshot(const StreamingPolicy& policy) {
    return {policy.memory_usage(),
            {policy.status(1), policy.status(2), policy.status(3), policy.status(4)},
            policy.live_regions(), policy.generations_exhausted()};
}

void expect_usage(
    const StreamingPolicy& policy, std::uint64_t pending,
    std::uint64_t resident, std::uint64_t retiring) {
    const auto usage = policy.memory_usage();
    CHECK(pending <= usage.capacity_bytes);
    CHECK(resident <= usage.capacity_bytes - pending);
    CHECK(retiring <= usage.capacity_bytes - pending - resident);
    const auto reserved = pending + resident + retiring;
    CHECK(usage == MemoryUsage{usage.capacity_bytes, pending, resident, retiring,
                              reserved, usage.capacity_bytes - reserved});
}

enum class Operation { MarkReady, Activate, Cancel, Retire, Acknowledge };

constexpr std::array operations{
    Operation::MarkReady, Operation::Activate, Operation::Cancel,
    Operation::Retire, Operation::Acknowledge};

std::string_view operation_name(Operation operation) {
    switch (operation) {
    case Operation::MarkReady: return "mark_ready";
    case Operation::Activate: return "activate";
    case Operation::Cancel: return "cancel";
    case Operation::Retire: return "retire";
    case Operation::Acknowledge: return "acknowledge_retirement";
    }
    return "unknown";
}

bool apply(StreamingPolicy& policy, Operation operation, RequestToken token) {
    switch (operation) {
    case Operation::MarkReady: return policy.mark_ready(token, readiness_all);
    case Operation::Activate: return policy.activate(token);
    case Operation::Cancel: return policy.cancel(token);
    case Operation::Retire: return policy.retire(token);
    case Operation::Acknowledge: return policy.acknowledge_retirement(token);
    }
    return false;
}

bool allowed(RegionState state, Operation operation) {
    switch (operation) {
    case Operation::MarkReady: return state == RegionState::Loading;
    case Operation::Activate: return state == RegionState::Ready;
    case Operation::Cancel:
        return state == RegionState::Loading || state == RegionState::Ready;
    case Operation::Retire: return state == RegionState::Active;
    case Operation::Acknowledge: return state == RegionState::Retiring;
    }
    return false;
}

void prepare(StreamingPolicy& policy, RequestToken token, RegionState state) {
    if (state == RegionState::Loading) {
        return;
    }
    CHECK(policy.mark_ready(token, readiness_all));
    if (state == RegionState::Ready) {
        return;
    }
    CHECK(policy.activate(token));
    if (state == RegionState::Active) {
        return;
    }
    CHECK(policy.retire(token));
    if (state == RegionState::Unloaded) {
        CHECK(policy.acknowledge_retirement(token));
    }
}

void readiness_and_token_values() {
    CHECK(!valid_readiness_mask(0));
    CHECK(valid_readiness_mask(readiness_visual));
    CHECK(valid_readiness_mask(readiness_collision));
    CHECK(valid_readiness_mask(readiness_navigation));
    CHECK(valid_readiness_mask(readiness_logic));
    CHECK(valid_readiness_mask(readiness_all));
    CHECK(!valid_readiness_mask(16));
    CHECK(!valid_readiness_mask(readiness_all | 16U));
    CHECK(!RequestToken{}.valid());
    CHECK(!RequestToken{0, 1, 1, 1}.valid());
    CHECK(!RequestToken{1, 0, 1, 1}.valid());
    CHECK(RequestToken{1, 1, 0, 0}.valid());
    CHECK(!RegionStatus{}.fully_ready());
    CHECK(state_name(RegionState::Unloaded) == "Unloaded");
    CHECK(state_name(RegionState::Loading) == "Loading");
    CHECK(state_name(RegionState::Ready) == "Ready");
    CHECK(state_name(RegionState::Active) == "Active");
    CHECK(state_name(RegionState::Retiring) == "Retiring");
}

void invalid_admission_preserves_state_and_generations() {
    StreamingPolicy policy{8};
    CHECK(policy.memory_usage().capacity_bytes == 8);
    CHECK(!policy.generations_exhausted());
    expect_usage(policy, 0, 0, 0);
    const auto initial = snapshot(policy);
    struct Request { std::uint32_t region; std::uint32_t mask; std::uint64_t bytes; };
    const std::array invalid{
        Request{0, readiness_all, 1}, Request{1, readiness_all, 0},
        Request{1, 0, 1}, Request{1, 16, 1}, Request{1, readiness_all | 16U, 1},
        Request{1, readiness_all, 9},
        Request{1, readiness_all, std::numeric_limits<std::uint64_t>::max()}};
    for (const auto request : invalid) {
        CHECK(!policy.begin(request.region, 0, 0, request.mask, request.bytes));
        CHECK(snapshot(policy) == initial);
    }
    const auto first = policy.begin(1, 0, 0, readiness_logic, 8);
    CHECK(first);
    CHECK(first->generation == 1);
    CHECK(first->content_revision == 0);
    CHECK(first->coordinate_epoch == 0);
    const auto full = snapshot(policy);
    CHECK(!policy.begin(1, 9, 9, readiness_all, 1));
    CHECK(snapshot(policy) == full);
    CHECK(!policy.begin(2, 0, 0, readiness_all, 1));
    CHECK(snapshot(policy) == full);
    CHECK(policy.cancel(*first));
    CHECK(policy.acknowledge_retirement(*first));
    const auto next = policy.begin(2, 0, 0, readiness_logic, 8);
    CHECK(next);
    CHECK(next->generation == first->generation + 1);

    StreamingPolicy zero_capacity{0};
    CHECK(!zero_capacity.begin(1, 0, 0, readiness_logic, 1));
    CHECK(zero_capacity.live_regions() == 0);
    expect_usage(zero_capacity, 0, 0, 0);
}

void readiness_requires_every_requested_bit() {
    StreamingPolicy policy{100};
    const auto token = policy.begin(1, 5, 7, readiness_all, 40);
    CHECK(token);
    CHECK(policy.is_current(*token));
    CHECK(policy.state(1) == RegionState::Loading);
    CHECK(policy.status(1)->ready_mask == 0);
    CHECK(!policy.status(1)->fully_ready());
    expect_usage(policy, 40, 0, 0);

    for (const auto bit : {readiness_visual, readiness_collision, readiness_logic}) {
        CHECK(policy.mark_ready(*token, bit));
        CHECK(policy.state(1) == RegionState::Loading);
        const auto before = snapshot(policy);
        CHECK(!policy.activate(*token));
        CHECK(snapshot(policy) == before);
    }
    const auto partial = snapshot(policy);
    CHECK(policy.mark_ready(*token, readiness_visual));
    CHECK(snapshot(policy) == partial);
    for (const auto invalid_mask : {0U, 16U, readiness_visual | 16U}) {
        CHECK(!policy.mark_ready(*token, invalid_mask));
        CHECK(snapshot(policy) == partial);
    }
    CHECK(policy.mark_ready(*token, readiness_navigation));
    CHECK(policy.state(1) == RegionState::Ready);
    CHECK(policy.status(1)->fully_ready());
    CHECK(policy.status(1)->ready_mask == readiness_all);
    expect_usage(policy, 40, 0, 0);
    CHECK(policy.activate(*token));
    CHECK(policy.state(1) == RegionState::Active);
    expect_usage(policy, 0, 40, 0);

    const auto subset = policy.begin(2, 0, 0, readiness_logic, 10);
    CHECK(subset);
    CHECK(policy.mark_ready(*subset, readiness_visual));
    CHECK(policy.state(2) == RegionState::Loading);
    CHECK(policy.mark_ready(*subset, readiness_logic));
    CHECK(policy.state(2) == RegionState::Ready);
    CHECK(policy.status(2)->ready_mask == (readiness_visual | readiness_logic));
}

void wrong_state_operations_never_mutate() {
    constexpr std::array states{
        RegionState::Loading, RegionState::Ready, RegionState::Active,
        RegionState::Retiring, RegionState::Unloaded};
    for (const auto state : states) {
        StreamingPolicy policy{20};
        const auto token = policy.begin(1, 3, 5, readiness_all, 10);
        CHECK(token);
        prepare(policy, *token, state);
        CHECK(policy.state(1) == state);
        const auto before = snapshot(policy);
        for (const auto operation : operations) {
            if (!allowed(state, operation)) {
                const auto context = std::string(state_name(state)) + "/" +
                                     std::string(operation_name(operation));
                CHECK_MSG(!apply(policy, operation, *token), context);
                CHECK_MSG(snapshot(policy) == before, context);
            }
            CHECK_MSG(!apply(policy, operation, RequestToken{}), operation_name(operation));
            CHECK(snapshot(policy) == before);
        }
        if (state != RegionState::Unloaded) {
            CHECK(!policy.begin(1, 9, 11, readiness_all, 1));
            CHECK(snapshot(policy) == before);
        }
        const auto next = policy.begin(2, 0, 0, readiness_logic, 1);
        CHECK(next);
        CHECK(next->generation == token->generation + 1);
    }
}

void every_operation_checks_all_four_token_fields() {
    constexpr std::array required_states{
        RegionState::Loading, RegionState::Ready, RegionState::Loading,
        RegionState::Active, RegionState::Retiring};
    constexpr std::array field_names{
        "region_id", "generation", "content_revision", "coordinate_epoch"};
    for (std::size_t operation_index = 0; operation_index < operations.size(); ++operation_index) {
        StreamingPolicy policy{100};
        const auto token = policy.begin(1, 7, 9, readiness_all, 20);
        const auto other = policy.begin(2, 7, 9, readiness_all, 10);
        CHECK(token);
        CHECK(other);
        prepare(policy, *token, required_states[operation_index]);
        prepare(policy, *other, required_states[operation_index]);
        const auto before = snapshot(policy);
        std::array<RequestToken, 4> mismatches{*token, *token, *token, *token};
        mismatches[0].region_id = other->region_id;
        ++mismatches[1].generation;
        ++mismatches[2].content_revision;
        ++mismatches[3].coordinate_epoch;
        for (std::size_t field = 0; field < mismatches.size(); ++field) {
            const auto context = std::string(operation_name(operations[operation_index])) +
                                 "/" + field_names[field];
            CHECK_MSG(!policy.is_current(mismatches[field]), context);
            CHECK_MSG(!apply(policy, operations[operation_index], mismatches[field]), context);
            CHECK_MSG(snapshot(policy) == before, context);
            CHECK(policy.is_current(*token));
        }
        CHECK(apply(policy, operations[operation_index], *token));
        const auto next = policy.begin(3, 0, 0, readiness_logic, 1);
        CHECK(next);
        CHECK(next->generation == other->generation + 1);
    }
}

void reservations_move_without_disappearing() {
    StreamingPolicy policy{100};
    const auto first = policy.begin(1, 0, 0, readiness_all, 30);
    CHECK(first);
    expect_usage(policy, 30, 0, 0);
    const auto second = policy.begin(2, 0, 0, readiness_all, 40);
    CHECK(second);
    expect_usage(policy, 70, 0, 0);
    const auto third = policy.begin(3, 0, 0, readiness_all, 20);
    CHECK(third);
    CHECK(policy.live_regions() == 3);
    expect_usage(policy, 90, 0, 0);
    CHECK(policy.mark_ready(*first, readiness_all));
    CHECK(policy.activate(*first));
    expect_usage(policy, 60, 30, 0);
    CHECK(policy.mark_ready(*second, readiness_all));
    expect_usage(policy, 60, 30, 0);
    CHECK(policy.cancel(*third));
    expect_usage(policy, 40, 30, 20);
    CHECK(policy.retire(*first));
    expect_usage(policy, 40, 0, 50);
    CHECK(policy.activate(*second));
    expect_usage(policy, 0, 40, 50);
    CHECK(policy.acknowledge_retirement(*third));
    expect_usage(policy, 0, 40, 30);
    CHECK(policy.retire(*second));
    expect_usage(policy, 0, 0, 70);
    CHECK(policy.acknowledge_retirement(*first));
    expect_usage(policy, 0, 0, 40);
    CHECK(policy.acknowledge_retirement(*second));
    expect_usage(policy, 0, 0, 0);
    CHECK(policy.live_regions() == 0);
}

void cancellation_keeps_capacity_until_acknowledged() {
    for (const auto state : {RegionState::Loading, RegionState::Ready}) {
        StreamingPolicy policy{8};
        const auto token = policy.begin(1, 0, 0, readiness_all, 8);
        CHECK(token);
        prepare(policy, *token, state);
        CHECK(policy.cancel(*token));
        CHECK(policy.state(1) == RegionState::Retiring);
        CHECK(policy.is_current(*token));
        expect_usage(policy, 0, 0, 8);
        const auto retiring = snapshot(policy);
        CHECK(!policy.begin(2, 0, 0, readiness_all, 1));
        CHECK(snapshot(policy) == retiring);
        CHECK(!policy.cancel(*token));
        CHECK(snapshot(policy) == retiring);
        CHECK(policy.acknowledge_retirement(*token));
        expect_usage(policy, 0, 0, 0);
        const auto next = policy.begin(2, 0, 0, readiness_all, 8);
        CHECK(next);
        CHECK(next->generation == token->generation + 1);
    }
}

void stale_and_duplicate_acknowledgements_are_rejected() {
    StreamingPolicy policy{10};
    const auto old = policy.begin(1, 11, 13, readiness_logic, 10);
    CHECK(old);
    CHECK(policy.cancel(*old));
    CHECK(policy.acknowledge_retirement(*old));
    CHECK(!policy.is_current(*old));
    const auto unloaded = snapshot(policy);
    CHECK(!policy.acknowledge_retirement(*old));
    CHECK(snapshot(policy) == unloaded);

    const auto current = policy.begin(1, 11, 13, readiness_logic, 10);
    CHECK(current);
    CHECK(current->generation > old->generation);
    CHECK(policy.cancel(*current));
    const auto retiring = snapshot(policy);
    for (const auto operation : operations) {
        CHECK_MSG(!apply(policy, operation, *old), operation_name(operation));
        CHECK(snapshot(policy) == retiring);
    }
    CHECK(policy.is_current(*current));
    expect_usage(policy, 0, 0, 10);
    CHECK(policy.acknowledge_retirement(*current));
    const auto finished = snapshot(policy);
    CHECK(!policy.acknowledge_retirement(*current));
    CHECK(snapshot(policy) == finished);
    expect_usage(policy, 0, 0, 0);
}

void unload_removes_status_metadata() {
    StreamingPolicy policy{1};
    std::uint64_t last_generation = 0;
    // Visit distinct IDs, then reuse an unloaded ID: none retains a status.
    for (const auto region : {1U, 2U, 3U, 4U, 1U}) {
        CHECK(policy.state(region) == RegionState::Unloaded);
        CHECK(!policy.status(region));
        const auto token = policy.begin(region, 0, 0, readiness_logic, 1);
        CHECK(token);
        CHECK(token->generation == last_generation + 1);
        last_generation = token->generation;
        CHECK(policy.live_regions() == 1);
        CHECK(policy.mark_ready(*token, readiness_logic));
        CHECK(policy.activate(*token));
        CHECK(policy.retire(*token));
        CHECK(policy.acknowledge_retirement(*token));
        CHECK(policy.live_regions() == 0);
        CHECK(!policy.status(region));
        CHECK(policy.state(region) == RegionState::Unloaded);
        CHECK(!policy.is_current(*token));
        expect_usage(policy, 0, 0, 0);
    }
}

void maximum_uint64_capacity_is_admitted_without_overflow() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    StreamingPolicy policy{maximum};
    CHECK(policy.memory_usage().capacity_bytes == maximum);
    const auto first = policy.begin(1, maximum, maximum, readiness_logic, maximum - 1);
    CHECK(first);
    expect_usage(policy, maximum - 1, 0, 0);
    const auto almost_full = snapshot(policy);
    CHECK(!policy.begin(2, 0, 0, readiness_logic, 2));
    CHECK(snapshot(policy) == almost_full);
    const auto second = policy.begin(2, 0, 0, readiness_logic, 1);
    CHECK(second);
    CHECK(second->generation == first->generation + 1);
    expect_usage(policy, maximum, 0, 0);
    const auto full = snapshot(policy);
    CHECK(!policy.begin(3, 0, 0, readiness_logic, 1));
    CHECK(snapshot(policy) == full);
    CHECK(policy.mark_ready(*first, readiness_logic));
    CHECK(policy.activate(*first));
    expect_usage(policy, 1, maximum - 1, 0);
    CHECK(policy.cancel(*second));
    expect_usage(policy, 0, maximum - 1, 1);
    CHECK(policy.retire(*first));
    expect_usage(policy, 0, 0, maximum);
    CHECK(policy.acknowledge_retirement(*first));
    expect_usage(policy, 0, 0, 1);
    const auto one_retiring = snapshot(policy);
    CHECK(!policy.begin(3, 0, 0, readiness_logic, maximum));
    CHECK(snapshot(policy) == one_retiring);
    CHECK(policy.acknowledge_retirement(*second));
    const auto whole_capacity = policy.begin(3, 0, 0, readiness_logic, maximum);
    CHECK(whole_capacity);
    CHECK(whole_capacity->generation == second->generation + 1);
    expect_usage(policy, maximum, 0, 0);
    CHECK(policy.cancel(*whole_capacity));
    expect_usage(policy, 0, 0, maximum);
    CHECK(policy.acknowledge_retirement(*whole_capacity));
    expect_usage(policy, 0, 0, 0);
}

} // namespace

int main() {
    return ge::test::run("streaming", {
        {"readiness and token values", readiness_and_token_values},
        {"invalid admission is transactional", invalid_admission_preserves_state_and_generations},
        {"all required readiness bits gate activation", readiness_requires_every_requested_bit},
        {"wrong state operations preserve state", wrong_state_operations_never_mutate},
        {"all operations validate all token fields", every_operation_checks_all_four_token_fields},
        {"reservation conservation", reservations_move_without_disappearing},
        {"cancellation stays charged until acknowledgement", cancellation_keeps_capacity_until_acknowledged},
        {"stale and duplicate acknowledgements", stale_and_duplicate_acknowledgements_are_rejected},
        {"unload removes metadata", unload_removes_status_metadata},
        {"uint64 maximum capacity", maximum_uint64_capacity_is_admitted_without_overflow},
    });
}
