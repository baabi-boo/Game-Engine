#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ge {

// A deterministic, single-threaded fixed-step accumulator. The caller supplies
// elapsed time; this class does not read wall time, sleep, or execute callbacks.
// All accounting is in integer nanoseconds. Only the interpolation ratio uses
// floating point, and it is for presentation rather than simulation scheduling.
class FixedClock {
public:
    using duration = std::chrono::nanoseconds;

    struct Tick {
        std::uint32_t steps{0};
        duration dropped_time{0};
        duration remainder{0};
        double interpolation{0.0};
    };

    // A positive step and a nonzero catch-up limit are required. The limit
    // bounds scheduled work per advance(), including after a long pause.
    explicit FixedClock(duration fixed_step, std::uint32_t max_catch_up_steps = 8)
        : fixed_step_(fixed_step), max_catch_up_steps_(max_catch_up_steps) {
        if (fixed_step_.count() <= 0) {
            throw std::invalid_argument("FixedClock step must be positive");
        }
        if (max_catch_up_steps_ == 0) {
            throw std::invalid_argument("FixedClock catch-up limit must be positive");
        }
    }

    // Consumes elapsed time plus the previous fractional remainder. Schedules
    // at most max_catch_up_steps complete steps, discards any additional whole
    // steps, and retains the fractional remainder. dropped_time reports exactly
    // the discarded whole-step time for THIS call, not a cumulative total.
    //
    // In mathematical (unbounded integer) arithmetic:
    // old_remainder + elapsed = steps * fixed_step + dropped_time + remainder.
    // Returned remainder is in [0, fixed_step); interpolation is in [0, 1).
    // Negative elapsed time throws std::invalid_argument without changing state.
    // Every nonnegative representable nanosecond duration is accepted, even if
    // adding it directly to the old remainder would overflow duration::rep.
    [[nodiscard]] Tick advance(duration elapsed) {
        if (elapsed.count() < 0) {
            throw std::invalid_argument("FixedClock elapsed time must not be negative");
        }

        const auto step = fixed_step_.count();
        const auto incoming_whole = elapsed.count() / step;
        const auto incoming_fraction = elapsed.count() % step;
        const auto until_next_step = step - remainder_.count();
        const bool carry = incoming_fraction >= until_next_step;

        // The branch avoids forming remainder + incoming_fraction when that
        // sum could overflow. A carry requires step >= 2, which also guarantees
        // that incoming_whole + 1 fits duration::rep.
        const auto fraction = carry
            ? incoming_fraction - until_next_step
            : remainder_.count() + incoming_fraction;
        const auto due = incoming_whole + (carry ? 1 : 0);
        const auto limit = static_cast<duration::rep>(max_catch_up_steps_);
        const auto scheduled = due < limit ? due : limit;
        const auto discarded = due - scheduled;

        // The catch-up limit is at least one. If there are discarded steps,
        // subtracting at least one whole step keeps this product representable,
        // even when elapsed + old_remainder would overflow duration::rep.
        const duration dropped{discarded * step};
        remainder_ = duration{fraction};

        auto interpolation = static_cast<double>(fraction) / static_cast<double>(step);
        if (interpolation >= 1.0) {
            // Large adjacent integers can round to the same double. Keep the
            // documented half-open range without changing exact time accounting.
            interpolation = std::nextafter(1.0, 0.0);
        }

        return Tick{static_cast<std::uint32_t>(scheduled), dropped, remainder_, interpolation};
    }

    // Drops only the retained fraction and preserves the clock configuration.
    // The caller explicitly chooses this reset; it is not reported as overload.
    void reset() noexcept { remainder_ = duration::zero(); }

    [[nodiscard]] duration fixed_step() const noexcept { return fixed_step_; }
    [[nodiscard]] std::uint32_t max_catch_up_steps() const noexcept {
        return max_catch_up_steps_;
    }
    [[nodiscard]] duration remainder() const noexcept { return remainder_; }

private:
    const duration fixed_step_;
    const std::uint32_t max_catch_up_steps_;
    duration remainder_{0};
};

} // namespace ge
