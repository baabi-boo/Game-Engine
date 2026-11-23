#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ge {

// Stable identity is assigned by the content/state owner. This value type does
// not allocate identities or enforce uniqueness; zero is always invalid.
struct PersistentId {
    std::uint64_t value{0};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

    auto operator<=>(const PersistentId&) const = default;
};

// A transient reference scoped to one GenerationPool's lifetime. Both fields
// are one-based: the default value is invalid, as is either field being zero.
// A token does not identify its originating pool, so callers must not exchange
// handles between pools or persist them in place of PersistentId values.
struct RuntimeHandle {
    std::uint32_t index{0};
    std::uint64_t generation{0};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

    auto operator<=>(const RuntimeHandle&) const = default;
};

// Single-threaded slot allocator; it owns no entity or component data.
//
// Slots are allocated lazily, then reused through an allocation-free free list.
// Every successful release invalidates the exact handle immediately. Generation
// numbers never wrap: a released slot at generation_limit is retired forever.
// A pool is deliberately neither copyable nor movable, and has no reset/clear
// operation that could make a stale handle valid again during its lifetime.
class GenerationPool {
public:
    // A capacity of zero is a valid permanently exhausted pool. A generation
    // limit of zero is invalid. Lower limits can bound reuse explicitly and make
    // generation exhaustion practical to exercise without billions of cycles.
    explicit GenerationPool(
        std::uint32_t capacity = std::numeric_limits<std::uint32_t>::max(),
        std::uint64_t generation_limit = std::numeric_limits<std::uint64_t>::max())
        : capacity_(capacity), generation_limit_(generation_limit) {
        if (generation_limit_ == 0) {
            throw std::invalid_argument("GenerationPool generation limit must be positive");
        }
    }

    GenerationPool(const GenerationPool&) = delete;
    GenerationPool& operator=(const GenerationPool&) = delete;
    GenerationPool(GenerationPool&&) = delete;
    GenerationPool& operator=(GenerationPool&&) = delete;

    // Returns no handle when every permitted slot is live or retired. Growing
    // the slot storage can throw std::bad_alloc/std::length_error; such failures
    // leave the pool unchanged. Reusing a free slot performs no allocation.
    [[nodiscard]] std::optional<RuntimeHandle> acquire() {
        if (free_head_ != 0) {
            const auto index = free_head_;
            auto& slot = slots_[static_cast<std::size_t>(index - 1)];
            free_head_ = slot.next_free;
            slot.next_free = 0;
            slot.live = true;
            ++live_count_;
            return RuntimeHandle{index, slot.generation};
        }

        if (slots_.size() >= capacity_) {
            return std::nullopt;
        }

        slots_.push_back(Slot{});
        ++live_count_;
        return RuntimeHandle{static_cast<std::uint32_t>(slots_.size()), 1};
    }

    [[nodiscard]] bool alive(RuntimeHandle handle) const noexcept {
        if (!handle.valid() || handle.index > slots_.size()) {
            return false;
        }
        const auto& slot = slots_[static_cast<std::size_t>(handle.index - 1)];
        return slot.live && slot.generation == handle.generation;
    }

    // False for invalid, stale, already released, or out-of-range tokens, with
    // no mutation. A release never allocates and never throws.
    [[nodiscard]] bool release(RuntimeHandle handle) noexcept {
        if (!alive(handle)) {
            return false;
        }

        auto& slot = slots_[static_cast<std::size_t>(handle.index - 1)];
        slot.live = false;
        --live_count_;
        if (slot.generation == generation_limit_) {
            ++retired_count_;
        } else {
            ++slot.generation;
            slot.next_free = free_head_;
            free_head_ = handle.index;
        }
        return true;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint64_t generation_limit() const noexcept {
        return generation_limit_;
    }
    [[nodiscard]] std::uint32_t size() const noexcept { return live_count_; }
    [[nodiscard]] std::uint32_t allocated_slots() const noexcept {
        return static_cast<std::uint32_t>(slots_.size());
    }
    [[nodiscard]] std::uint32_t retired_slots() const noexcept { return retired_count_; }

    // Includes reusable free slots and slots that have not yet been allocated.
    // This is a logical capacity bound, not a guarantee of available memory.
    [[nodiscard]] std::uint32_t available_slots() const noexcept {
        return capacity_ - live_count_ - retired_count_;
    }

private:
    struct Slot {
        std::uint64_t generation{1};
        std::uint32_t next_free{0};
        bool live{true};
    };

    const std::uint32_t capacity_;
    const std::uint64_t generation_limit_;
    std::vector<Slot> slots_;
    std::uint32_t free_head_{0};
    std::uint32_t live_count_{0};
    std::uint32_t retired_count_{0};
};

} // namespace ge
