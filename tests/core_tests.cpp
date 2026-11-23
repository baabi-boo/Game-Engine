#include "test_support.hpp"

#include <ge/core/clock.hpp>
#include <ge/core/identity.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace std::chrono_literals;
using Duration = ge::FixedClock::duration;

static_assert(!std::is_copy_constructible_v<ge::GenerationPool>);
static_assert(!std::is_move_constructible_v<ge::GenerationPool>);
static_assert(!std::is_copy_assignable_v<ge::GenerationPool>);
static_assert(!std::is_move_assignable_v<ge::GenerationPool>);

void expect_tick(
    ge::FixedClock& clock, Duration elapsed, std::uint32_t steps,
    Duration dropped, Duration remainder) {
    const auto tick = clock.advance(elapsed);
    CHECK(tick.steps == steps);
    CHECK(tick.dropped_time == dropped);
    CHECK(tick.remainder == remainder);
    CHECK(clock.remainder() == remainder);
    CHECK(tick.interpolation >= 0.0);
    CHECK(tick.interpolation < 1.0);
    const auto ratio = static_cast<double>(remainder.count()) /
                       static_cast<double>(clock.fixed_step().count());
    CHECK(std::abs(tick.interpolation - ratio) <=
          2.0 * std::numeric_limits<double>::epsilon());
}

void identity_values() {
    CHECK(!ge::PersistentId{}.valid());
    CHECK(ge::PersistentId{1}.valid());
    CHECK(static_cast<bool>(ge::PersistentId{std::numeric_limits<std::uint64_t>::max()}));
    CHECK(ge::PersistentId{7} == ge::PersistentId{7});
    CHECK(ge::PersistentId{7} != ge::PersistentId{8});
    CHECK(!ge::RuntimeHandle{}.valid());
    CHECK(!ge::RuntimeHandle{0, 1}.valid());
    CHECK(!ge::RuntimeHandle{1, 0}.valid());
    CHECK(static_cast<bool>(ge::RuntimeHandle{1, 1}));
    CHECK(ge::RuntimeHandle{1, 2} != ge::RuntimeHandle{2, 2});
    CHECK(ge::RuntimeHandle{1, 2} != ge::RuntimeHandle{1, 3});
}

void pool_capacity_and_invalid_releases() {
    ge::GenerationPool empty{0};
    CHECK(!empty.acquire());
    CHECK(empty.size() == 0);
    CHECK(empty.available_slots() == 0);
    CHECK(empty.allocated_slots() == 0);
    CHECK_THROWS_AS(ge::GenerationPool(2, 0), std::invalid_argument);

    ge::GenerationPool pool{2, 3};
    CHECK(pool.capacity() == 2);
    CHECK(pool.generation_limit() == 3);
    CHECK(pool.available_slots() == 2);
    const auto first = pool.acquire();
    const auto second = pool.acquire();
    CHECK(first);
    CHECK(second);
    CHECK(*first != *second);
    CHECK(pool.size() == 2);
    CHECK(pool.allocated_slots() == 2);
    CHECK(pool.available_slots() == 0);
    CHECK(!pool.acquire());

    const std::array invalid{
        ge::RuntimeHandle{}, ge::RuntimeHandle{0, 1}, ge::RuntimeHandle{1, 0},
        ge::RuntimeHandle{3, 1}, ge::RuntimeHandle{first->index, first->generation + 1}};
    for (const auto handle : invalid) {
        CHECK(!pool.alive(handle));
        CHECK(!pool.release(handle));
        CHECK(pool.alive(*first));
        CHECK(pool.alive(*second));
        CHECK(pool.size() == 2);
        CHECK(pool.available_slots() == 0);
        CHECK(pool.allocated_slots() == 2);
        CHECK(pool.retired_slots() == 0);
    }

    CHECK(pool.release(*first));
    CHECK(pool.available_slots() == 1);
    CHECK(pool.size() == 1);
    const auto reused = pool.acquire();
    CHECK(reused);
    CHECK(reused->index == first->index);
    CHECK(reused->generation == first->generation + 1);
    CHECK(!pool.release(*first));
    CHECK(pool.alive(*reused));
    CHECK(pool.alive(*second));
    CHECK(pool.release(*second));
    CHECK(pool.release(*reused));
    CHECK(pool.size() == 0);
    CHECK(pool.available_slots() == 2);
}

void pool_reuse_and_generation_exhaustion() {
    ge::GenerationPool pool{2, 2};
    std::array<ge::RuntimeHandle, 4> history{};
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto current = pool.acquire();
        CHECK(current);
        CHECK(current->index == index / 2 + 1);
        CHECK(current->generation == index % 2 + 1);
        history[index] = *current;
        for (std::size_t stale = 0; stale < index; ++stale) {
            CHECK(!pool.alive(history[stale]));
            CHECK(!pool.release(history[stale]));
        }
        CHECK(pool.alive(*current));
        CHECK(pool.size() == 1);
        CHECK(pool.release(*current));
        CHECK(!pool.alive(*current));
        CHECK(!pool.release(*current));
        CHECK(pool.size() == 0);
        CHECK(pool.retired_slots() == (index + 1) / 2);
        CHECK(pool.available_slots() == 2 - pool.retired_slots());
    }
    CHECK(pool.allocated_slots() == 2);
    CHECK(pool.retired_slots() == 2);
    CHECK(!pool.acquire());
    for (const auto stale : history) {
        CHECK(!pool.alive(stale));
        CHECK(!pool.release(stale));
    }

    ge::GenerationPool no_reuse{1, 1};
    const auto only = no_reuse.acquire();
    CHECK(only);
    CHECK(no_reuse.release(*only));
    CHECK(no_reuse.retired_slots() == 1);
    CHECK(!no_reuse.acquire());
}

void clock_configuration() {
    CHECK_THROWS_AS(ge::FixedClock(0ns), std::invalid_argument);
    CHECK_THROWS_AS(ge::FixedClock(-1ns), std::invalid_argument);
    CHECK_THROWS_AS(ge::FixedClock(Duration::min()), std::invalid_argument);
    CHECK_THROWS_AS(ge::FixedClock(1ns, 0), std::invalid_argument);
    ge::FixedClock clock{10ns};
    CHECK(clock.fixed_step() == 10ns);
    CHECK(clock.max_catch_up_steps() == 8);
    CHECK(clock.remainder() == 0ns);
}

void clock_normal_steps_and_remainder() {
    ge::FixedClock clock{10ns, 3};
    expect_tick(clock, 4ns, 0, 0ns, 4ns);
    expect_tick(clock, 6ns, 1, 0ns, 0ns);
    expect_tick(clock, 24ns, 2, 0ns, 4ns);
    expect_tick(clock, 6ns, 1, 0ns, 0ns);
    expect_tick(clock, 9ns, 0, 0ns, 9ns);
    expect_tick(clock, 1ns, 1, 0ns, 0ns);
}

void clock_pause_and_explicit_reset() {
    ge::FixedClock clock{10ns, 3};
    expect_tick(clock, 7ns, 0, 0ns, 7ns);
    expect_tick(clock, 0ns, 0, 0ns, 7ns);
    expect_tick(clock, 0ns, 0, 0ns, 7ns);
    clock.reset();
    CHECK(clock.remainder() == 0ns);
    CHECK(clock.fixed_step() == 10ns);
    CHECK(clock.max_catch_up_steps() == 3);
    expect_tick(clock, 10ns, 1, 0ns, 0ns);
}

void clock_catch_up_reports_per_call_drops() {
    ge::FixedClock clock{10ns, 3};
    expect_tick(clock, 7ns, 0, 0ns, 7ns);
    expect_tick(clock, 47ns, 3, 20ns, 4ns);
    expect_tick(clock, 6ns, 1, 0ns, 0ns);
    expect_tick(clock, 100ns, 3, 70ns, 0ns);
    expect_tick(clock, 39ns, 3, 0ns, 9ns);
    expect_tick(clock, 11ns, 2, 0ns, 0ns);
}

void clock_negative_elapsed_preserves_remainder() {
    ge::FixedClock clock{10ns, 3};
    expect_tick(clock, 7ns, 0, 0ns, 7ns);
    for (const auto elapsed : {-1ns, Duration::min()}) {
        CHECK_THROWS_AS(clock.advance(elapsed), std::invalid_argument);
        CHECK(clock.remainder() == 7ns);
        CHECK(clock.fixed_step() == 10ns);
        CHECK(clock.max_catch_up_steps() == 3);
    }
    expect_tick(clock, 3ns, 1, 0ns, 0ns);
}

void clock_extreme_durations_do_not_overflow() {
    const auto maximum = Duration::max().count();
    ge::FixedClock large_step{Duration::max(), 1};
    expect_tick(large_step, Duration{maximum - 1}, 0, 0ns, Duration{maximum - 1});
    // The mathematical sum of elapsed and retained time exceeds duration::max().
    expect_tick(large_step, Duration::max(), 1, 0ns, Duration{maximum - 1});
    expect_tick(large_step, 1ns, 1, 0ns, 0ns);

    ge::FixedClock carry_with_drop{2ns, 1};
    expect_tick(carry_with_drop, 1ns, 0, 0ns, 1ns);
    expect_tick(carry_with_drop, Duration::max(), 1, Duration{maximum - 1}, 0ns);

    const auto half_step = maximum / 2 + 1;
    ge::FixedClock large_drop{Duration{half_step}, 1};
    expect_tick(large_drop, Duration{half_step - 1}, 0, 0ns, Duration{half_step - 1});
    expect_tick(large_drop, Duration::max(), 1, Duration{half_step}, Duration{half_step - 2});

    constexpr auto max_steps = std::numeric_limits<std::uint32_t>::max();
    ge::FixedClock large_limit{1ns, max_steps};
    expect_tick(large_limit, Duration::max(), max_steps,
                Duration{maximum - static_cast<Duration::rep>(max_steps)}, 0ns);
}

} // namespace

int main() {
    return ge::test::run("core", {
        {"identity values", identity_values},
        {"pool capacity and rejected releases", pool_capacity_and_invalid_releases},
        {"stale handles and generation exhaustion", pool_reuse_and_generation_exhaustion},
        {"clock configuration", clock_configuration},
        {"clock normal steps and remainder", clock_normal_steps_and_remainder},
        {"clock pause and reset", clock_pause_and_explicit_reset},
        {"clock bounded catch-up", clock_catch_up_reports_per_call_drops},
        {"clock negative elapsed preserves state", clock_negative_elapsed_preserves_remainder},
        {"clock extreme durations", clock_extreme_durations_do_not_overflow},
    });
}
