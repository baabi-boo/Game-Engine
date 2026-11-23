#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>

namespace ge::world {

inline constexpr std::uint32_t readiness_visual = 1U;
inline constexpr std::uint32_t readiness_collision = 2U;
inline constexpr std::uint32_t readiness_navigation = 4U;
inline constexpr std::uint32_t readiness_logic = 8U;
inline constexpr std::uint32_t readiness_all = 15U;

[[nodiscard]] constexpr bool valid_readiness_mask(std::uint32_t mask) noexcept {
    return mask != 0U && (mask & ~readiness_all) == 0U;
}

// Revisions and epochs are caller-supplied labels; zero is permitted for both.
// A token is meaningful only to the policy instance that issued it.
struct RequestToken {
    std::uint32_t region_id{};
    std::uint64_t generation{};
    std::uint64_t content_revision{};
    std::uint64_t coordinate_epoch{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return region_id != 0U && generation != 0U;
    }

    constexpr bool operator==(const RequestToken&) const noexcept = default;
};

enum class RegionState {
    Unloaded,
    Loading,
    Ready,
    Active,
    Retiring,
};

[[nodiscard]] constexpr std::string_view state_name(RegionState state) noexcept {
    switch (state) {
    case RegionState::Unloaded:
        return "Unloaded";
    case RegionState::Loading:
        return "Loading";
    case RegionState::Ready:
        return "Ready";
    case RegionState::Active:
        return "Active";
    case RegionState::Retiring:
        return "Retiring";
    }
    return "Unknown";
}

struct RegionStatus {
    RequestToken token{};
    RegionState state{RegionState::Unloaded};
    std::uint32_t required_mask{};
    std::uint32_t ready_mask{};
    std::uint64_t budget_bytes{};

    [[nodiscard]] constexpr bool fully_ready() const noexcept {
        return required_mask != 0U &&
               (ready_mask & required_mask) == required_mask;
    }

    constexpr bool operator==(const RegionStatus&) const noexcept = default;
};

struct MemoryUsage {
    std::uint64_t capacity_bytes{};
    std::uint64_t pending_bytes{};
    std::uint64_t resident_bytes{};
    std::uint64_t retiring_bytes{};
    std::uint64_t reserved_bytes{};
    std::uint64_t available_bytes{};

    constexpr bool operator==(const MemoryUsage&) const noexcept = default;
};

// Single-threaded, synchronous reservation policy. Readiness is symbolic: this
// class does not perform I/O, allocate resources, or establish physical safety.
// Byte counts are caller-provided resource reservations, not metadata usage.
//
// Loading and Ready count as pending; Active counts as resident; Retiring stays
// charged until acknowledge_retirement accepts the same complete token.
// Rejected operations leave all policy state, counters, and generations intact.
class StreamingPolicy final {
public:
    explicit StreamingPolicy(std::uint64_t capacity_bytes) noexcept;

    // The owner remains stable for the lifetime of its issued tokens.
    StreamingPolicy(const StreamingPolicy&) = delete;
    StreamingPolicy& operator=(const StreamingPolicy&) = delete;
    StreamingPolicy(StreamingPolicy&&) = delete;
    StreamingPolicy& operator=(StreamingPolicy&&) = delete;

    // Requires a nonzero region, nonzero budget, valid required readiness mask,
    // an Unloaded region, and sufficient unreserved capacity. Revision and epoch
    // are immutable for this request. Every successful begin issues a unique
    // generation within this policy; generation exhaustion permanently rejects
    // further begins. Metadata allocation failure may throw, with no changes.
    [[nodiscard]] std::optional<RequestToken> begin(
        std::uint32_t region_id,
        std::uint64_t content_revision,
        std::uint64_t coordinate_epoch,
        std::uint32_t required_mask,
        std::uint64_t budget_bytes);

    // Accepts a nonzero mask of known bits only while Loading. Bits accumulate;
    // duplicate bits while Loading are harmless. Completing the required mask
    // changes Loading to Ready. Known but unrequired bits may also be recorded.
    [[nodiscard]] bool mark_ready(
        const RequestToken& token, std::uint32_t ready_mask) noexcept;

    // Ready -> Active. Loading cannot be activated, even partially ready.
    [[nodiscard]] bool activate(const RequestToken& token) noexcept;

    // Loading or Ready -> Retiring. Cancellation does not release capacity.
    [[nodiscard]] bool cancel(const RequestToken& token) noexcept;

    // Active -> Retiring. Repeated retirement requests are rejected.
    [[nodiscard]] bool retire(const RequestToken& token) noexcept;

    // Retiring -> Unloaded. Only this operation releases a live reservation.
    [[nodiscard]] bool acknowledge_retirement(const RequestToken& token) noexcept;

    // Unloaded regions have no stored status. state() reports Unloaded for
    // absent regions, including after a successful retirement acknowledgement.
    [[nodiscard]] std::optional<RegionStatus> status(
        std::uint32_t region_id) const noexcept;
    [[nodiscard]] RegionState state(std::uint32_t region_id) const noexcept;
    [[nodiscard]] bool is_current(const RequestToken& token) const noexcept;
    [[nodiscard]] MemoryUsage memory_usage() const noexcept;
    [[nodiscard]] std::size_t live_regions() const noexcept;
    [[nodiscard]] bool generations_exhausted() const noexcept;

private:
    [[nodiscard]] RegionStatus* matching(const RequestToken& token) noexcept;
    [[nodiscard]] const RegionStatus* matching(
        const RequestToken& token) const noexcept;

    const std::uint64_t capacity_bytes_;
    std::uint64_t pending_bytes_{};
    std::uint64_t resident_bytes_{};
    std::uint64_t retiring_bytes_{};
    std::uint64_t reserved_bytes_{};
    // Zero means exhausted; it is never issued and never reset.
    std::uint64_t next_generation_{1U};
    std::map<std::uint32_t, RegionStatus> regions_;
};

} // namespace ge::world
