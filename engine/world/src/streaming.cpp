#include <ge/world/streaming.hpp>

#include <limits>

namespace ge::world {

StreamingPolicy::StreamingPolicy(std::uint64_t capacity_bytes) noexcept
    : capacity_bytes_(capacity_bytes) {}

std::optional<RequestToken> StreamingPolicy::begin(
    std::uint32_t region_id,
    std::uint64_t content_revision,
    std::uint64_t coordinate_epoch,
    std::uint32_t required_mask,
    std::uint64_t budget_bytes) {
    if (region_id == 0U || budget_bytes == 0U ||
        !valid_readiness_mask(required_mask) || next_generation_ == 0U ||
        regions_.contains(region_id)) {
        return std::nullopt;
    }

    // Subtraction avoids overflowing even when capacity is UINT64_MAX. Every
    // accepted transition preserves reserved_bytes_ <= capacity_bytes_.
    if (budget_bytes > capacity_bytes_ - reserved_bytes_) {
        return std::nullopt;
    }

    const RequestToken token{
        region_id, next_generation_, content_revision, coordinate_epoch};
    const RegionStatus initial{
        token, RegionState::Loading, required_mask, 0U, budget_bytes};

    // Allocate metadata before publishing accounting or consuming a generation.
    // std::map insertion leaves the map unchanged if allocation fails.
    const auto [position, inserted] = regions_.emplace(region_id, initial);
    (void)position;
    if (!inserted) {
        return std::nullopt;
    }

    pending_bytes_ += budget_bytes;
    reserved_bytes_ += budget_bytes;
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        next_generation_ = 0U;
    } else {
        ++next_generation_;
    }
    return token;
}

bool StreamingPolicy::mark_ready(
    const RequestToken& token, std::uint32_t ready_mask) noexcept {
    auto* region = matching(token);
    if (region == nullptr || region->state != RegionState::Loading ||
        !valid_readiness_mask(ready_mask)) {
        return false;
    }

    region->ready_mask |= ready_mask;
    if (region->fully_ready()) {
        region->state = RegionState::Ready;
    }
    return true;
}

bool StreamingPolicy::activate(const RequestToken& token) noexcept {
    auto* region = matching(token);
    if (region == nullptr || region->state != RegionState::Ready ||
        !region->fully_ready()) {
        return false;
    }

    pending_bytes_ -= region->budget_bytes;
    resident_bytes_ += region->budget_bytes;
    region->state = RegionState::Active;
    return true;
}

bool StreamingPolicy::cancel(const RequestToken& token) noexcept {
    auto* region = matching(token);
    if (region == nullptr ||
        (region->state != RegionState::Loading &&
         region->state != RegionState::Ready)) {
        return false;
    }

    pending_bytes_ -= region->budget_bytes;
    retiring_bytes_ += region->budget_bytes;
    region->state = RegionState::Retiring;
    return true;
}

bool StreamingPolicy::retire(const RequestToken& token) noexcept {
    auto* region = matching(token);
    if (region == nullptr || region->state != RegionState::Active) {
        return false;
    }

    resident_bytes_ -= region->budget_bytes;
    retiring_bytes_ += region->budget_bytes;
    region->state = RegionState::Retiring;
    return true;
}

bool StreamingPolicy::acknowledge_retirement(const RequestToken& token) noexcept {
    const auto position = regions_.find(token.region_id);
    if (position == regions_.end() || position->second.token != token ||
        position->second.state != RegionState::Retiring) {
        return false;
    }

    const auto budget_bytes = position->second.budget_bytes;
    retiring_bytes_ -= budget_bytes;
    reserved_bytes_ -= budget_bytes;
    regions_.erase(position);
    return true;
}

std::optional<RegionStatus> StreamingPolicy::status(
    std::uint32_t region_id) const noexcept {
    const auto position = regions_.find(region_id);
    if (position == regions_.end()) {
        return std::nullopt;
    }
    return position->second;
}

RegionState StreamingPolicy::state(std::uint32_t region_id) const noexcept {
    const auto position = regions_.find(region_id);
    return position == regions_.end() ? RegionState::Unloaded
                                      : position->second.state;
}

bool StreamingPolicy::is_current(const RequestToken& token) const noexcept {
    return matching(token) != nullptr;
}

MemoryUsage StreamingPolicy::memory_usage() const noexcept {
    return {capacity_bytes_, pending_bytes_, resident_bytes_, retiring_bytes_,
            reserved_bytes_, capacity_bytes_ - reserved_bytes_};
}

std::size_t StreamingPolicy::live_regions() const noexcept {
    return regions_.size();
}

bool StreamingPolicy::generations_exhausted() const noexcept {
    return next_generation_ == 0U;
}

RegionStatus* StreamingPolicy::matching(const RequestToken& token) noexcept {
    const auto position = regions_.find(token.region_id);
    return position != regions_.end() && position->second.token == token
               ? &position->second
               : nullptr;
}

const RegionStatus* StreamingPolicy::matching(
    const RequestToken& token) const noexcept {
    const auto position = regions_.find(token.region_id);
    return position != regions_.end() && position->second.token == token
               ? &position->second
               : nullptr;
}

} // namespace ge::world
