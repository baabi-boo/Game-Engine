#pragma once

#include <ge/core/identity.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ge::core {

namespace memory_detail {

[[nodiscard]] constexpr bool valid_alignment(std::size_t alignment) noexcept {
    return alignment != 0 && std::has_single_bit(alignment);
}

[[nodiscard]] constexpr std::size_t checked_product(std::size_t count,
                                                    std::size_t stride) {
    if (stride != 0 && count > std::numeric_limits<std::size_t>::max() / stride) {
        throw std::length_error("allocation size overflow");
    }
    return count * stride;
}

inline void increment_saturated(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

} // namespace memory_detail

struct ArenaStatistics {
    std::size_t capacity{};
    std::size_t used{};
    std::size_t high_water{};
    std::uint64_t allocations{};
    std::uint64_t failed_allocations{};
};

// Single-writer scratch storage. Rewinding does not run object destructors.
// Owners must end every object's lifetime before reclaiming its storage.
class LinearArena final {
public:
    struct Marker {
        const LinearArena* owner{};
        std::uint64_t epoch{};
        std::size_t offset{};
    };

    explicit LinearArena(std::size_t capacity)
        : owned_(capacity == 0 ? nullptr : std::make_unique<std::byte[]>(capacity)),
          storage_(owned_.get(), capacity) {
        statistics_.capacity = capacity;
    }

    explicit LinearArena(std::span<std::byte> storage) noexcept : storage_(storage) {
        statistics_.capacity = storage_.size();
    }

    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;
    LinearArena(LinearArena&&) = delete;
    LinearArena& operator=(LinearArena&&) = delete;

    [[nodiscard]] void* try_allocate(std::size_t bytes,
                                     std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (!memory_detail::valid_alignment(alignment) || bytes == 0) {
            memory_detail::increment_saturated(statistics_.failed_allocations);
            return nullptr;
        }
        if (bytes > remaining()) {
            memory_detail::increment_saturated(statistics_.failed_allocations);
            return nullptr;
        }
        void* pointer = storage_.data() + statistics_.used;
        std::size_t space = remaining();
        if (std::align(alignment, bytes, pointer, space) == nullptr) {
            memory_detail::increment_saturated(statistics_.failed_allocations);
            return nullptr;
        }
        const auto padding = remaining() - space;
        statistics_.used += padding + bytes;
        statistics_.high_water = std::max(statistics_.high_water, statistics_.used);
        memory_detail::increment_saturated(statistics_.allocations);
        return pointer;
    }

    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) {
        if (!memory_detail::valid_alignment(alignment)) {
            throw std::invalid_argument("arena alignment must be a nonzero power of two");
        }
        if (bytes == 0) {
            return nullptr;
        }
        if (void* pointer = try_allocate(bytes, alignment)) {
            return pointer;
        }
        throw std::bad_alloc();
    }

    template <class T>
    [[nodiscard]] T* allocate_array(std::size_t count) {
        static_assert(!std::is_void_v<T>);
        return static_cast<T*>(allocate(memory_detail::checked_product(count, sizeof(T)),
                                        alignof(T)));
    }

    template <class T, class... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        const auto saved = mark();
        T* pointer = allocate_array<T>(1);
        try {
            return std::construct_at(pointer, std::forward<Args>(args)...);
        } catch (...) {
            static_cast<void>(rewind(saved));
            throw;
        }
    }

    [[nodiscard]] Marker mark() const noexcept {
        return Marker{this, epoch_, statistics_.used};
    }

    // Markers name offsets within one reset epoch, not individual allocations.
    [[nodiscard]] bool rewind(Marker marker) noexcept {
        if (marker.owner != this || marker.epoch != epoch_ ||
            marker.offset > statistics_.used) {
            return false;
        }
        statistics_.used = marker.offset;
        return true;
    }

    void reset() {
        if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("arena marker epoch exhausted");
        }
        statistics_.used = 0;
        ++epoch_;
    }

    void reset_statistics() noexcept {
        statistics_.high_water = statistics_.used;
        statistics_.allocations = 0;
        statistics_.failed_allocations = 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return statistics_.capacity;
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return statistics_.used;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return statistics_.capacity - statistics_.used;
    }

    [[nodiscard]] ArenaStatistics statistics() const noexcept {
        return statistics_;
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        if (pointer == nullptr || storage_.empty()) {
            return false;
        }
        const auto begin = reinterpret_cast<std::uintptr_t>(storage_.data());
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= begin && address - begin < storage_.size();
    }

private:
    std::unique_ptr<std::byte[]> owned_;
    std::span<std::byte> storage_;
    ArenaStatistics statistics_;
    std::uint64_t epoch_{1};
};

class ScratchScope final {
public:
    explicit ScratchScope(LinearArena& arena) noexcept : arena_(&arena), mark_(arena.mark()) {}

    ~ScratchScope() {
        if (arena_ != nullptr) {
            static_cast<void>(arena_->rewind(mark_));
        }
    }

    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;

    ScratchScope(ScratchScope&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)), mark_(other.mark_) {}

    ScratchScope& operator=(ScratchScope&&) = delete;

    void commit() noexcept {
        arena_ = nullptr;
    }

    [[nodiscard]] bool rewind() noexcept {
        if (arena_ == nullptr) {
            return false;
        }
        const bool result = arena_->rewind(mark_);
        if (result) {
            arena_ = nullptr;
        }
        return result;
    }

    [[nodiscard]] LinearArena::Marker marker() const noexcept {
        return mark_;
    }

private:
    LinearArena* arena_;
    LinearArena::Marker mark_;
};

// Destroy PMR containers before their arena is rewound. Deallocation is a no-op.
class ArenaResource final : public std::pmr::memory_resource {
public:
    explicit ArenaResource(LinearArena& arena) noexcept : arena_(&arena) {}

    [[nodiscard]] LinearArena& arena() noexcept {
        return *arena_;
    }

    [[nodiscard]] const LinearArena& arena() const noexcept {
        return *arena_;
    }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return arena_->allocate(std::max(bytes, std::size_t{1}), alignment);
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {}

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* resource = dynamic_cast<const ArenaResource*>(&other);
        return resource != nullptr && resource->arena_ == arena_;
    }

private:
    LinearArena* arena_;
};

struct ResourceStatistics {
    std::size_t limit{};
    std::size_t reserved{};
    std::size_t pending{};
    std::size_t resident{};
    std::size_t retiring{};
    std::size_t high_water{};
    std::size_t live_allocations{};
    std::uint64_t allocations{};
    std::uint64_t failed_allocations{};
};

// The budget charges requested payload bytes, including in-flight upstream calls.
// Upstream allocator metadata and alignment padding are outside that byte contract.
// Allocation/deallocation may overlap; upstream must itself support that usage.
class BudgetResource final : public std::pmr::memory_resource {
public:
    explicit BudgetResource(std::size_t limit,
                            std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(upstream) {
        if (upstream_ == nullptr) {
            throw std::invalid_argument("budget resource requires an upstream allocator");
        }
        statistics_.limit = limit;
    }

    BudgetResource(const BudgetResource&) = delete;
    BudgetResource& operator=(const BudgetResource&) = delete;

    [[nodiscard]] ResourceStatistics statistics() const {
        std::lock_guard lock(mutex_);
        return statistics_;
    }

    [[nodiscard]] std::size_t remaining() const {
        std::lock_guard lock(mutex_);
        return statistics_.limit - statistics_.reserved;
    }

    [[nodiscard]] bool set_limit(std::size_t limit) {
        std::lock_guard lock(mutex_);
        if (limit < statistics_.reserved) {
            return false;
        }
        statistics_.limit = limit;
        return true;
    }

    void reset_statistics() {
        std::lock_guard lock(mutex_);
        statistics_.high_water = statistics_.reserved;
        statistics_.allocations = 0;
        statistics_.failed_allocations = 0;
    }

    [[nodiscard]] std::pmr::memory_resource* upstream_resource() const noexcept {
        return upstream_;
    }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!memory_detail::valid_alignment(alignment)) {
            throw std::bad_alloc();
        }
        const auto charge = std::max(bytes, std::size_t{1});
        {
            std::lock_guard lock(mutex_);
            if (charge > statistics_.limit - statistics_.reserved ||
                statistics_.live_allocations == std::numeric_limits<std::size_t>::max()) {
                memory_detail::increment_saturated(statistics_.failed_allocations);
                throw std::bad_alloc();
            }
            statistics_.reserved += charge;
            statistics_.pending += charge;
            statistics_.high_water = std::max(statistics_.high_water, statistics_.reserved);
        }
        void* pointer = nullptr;
        try {
            pointer = upstream_->allocate(charge, alignment);
        } catch (...) {
            std::lock_guard lock(mutex_);
            statistics_.pending -= charge;
            statistics_.reserved -= charge;
            memory_detail::increment_saturated(statistics_.failed_allocations);
            throw;
        }
        {
            std::lock_guard lock(mutex_);
            statistics_.pending -= charge;
            statistics_.resident += charge;
            ++statistics_.live_allocations;
            memory_detail::increment_saturated(statistics_.allocations);
        }
        return pointer;
    }

    // As for every PMR resource, callers must supply the original size/alignment.
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        const auto charge = std::max(bytes, std::size_t{1});
        {
            std::lock_guard lock(mutex_);
            if (pointer == nullptr || charge > statistics_.resident ||
                statistics_.live_allocations == 0) {
                std::terminate();
            }
            statistics_.resident -= charge;
            statistics_.retiring += charge;
        }
        upstream_->deallocate(pointer, charge, alignment);
        {
            std::lock_guard lock(mutex_);
            statistics_.retiring -= charge;
            statistics_.reserved -= charge;
            --statistics_.live_allocations;
        }
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    std::pmr::memory_resource* upstream_;
    mutable std::mutex mutex_;
    ResourceStatistics statistics_;
};

// Stable object addresses, lazy allocation, and generation-checked pool-local IDs.
// All mutation is single-writer; iteration callbacks must not mutate the pool.
template <class T, std::size_t PageSize = 256>
class ObjectPool final {
    static_assert(PageSize > 0);
    static_assert(std::is_nothrow_destructible_v<T>);

    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
        ge::RuntimeHandle handle{};

        [[nodiscard]] T* object() noexcept {
            return std::launder(reinterpret_cast<T*>(storage));
        }

        [[nodiscard]] const T* object() const noexcept {
            return std::launder(reinterpret_cast<const T*>(storage));
        }
    };

    using Page = std::array<Slot, PageSize>;

public:
    explicit ObjectPool(std::uint32_t capacity,
                        std::uint64_t generation_limit = std::numeric_limits<std::uint64_t>::max())
        : handles_(capacity, generation_limit),
          pages_(static_cast<std::size_t>(capacity) / PageSize +
                 (static_cast<std::size_t>(capacity) % PageSize != 0 ? 1U : 0U)) {}

    ~ObjectPool() {
        clear();
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    template <class... Args>
    [[nodiscard]] std::optional<ge::RuntimeHandle> emplace(Args&&... args) {
        auto handle = handles_.acquire();
        if (!handle) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(handle->index - 1);
        try {
            auto& page = pages_[index / PageSize];
            if (!page) {
                page = std::make_unique<Page>();
            }
            auto& slot = (*page)[index % PageSize];
            std::construct_at(reinterpret_cast<T*>(slot.storage), std::forward<Args>(args)...);
            slot.handle = *handle;
        } catch (...) {
            static_cast<void>(handles_.release(*handle));
            throw;
        }
        return handle;
    }

    [[nodiscard]] bool contains(ge::RuntimeHandle handle) const noexcept {
        return handles_.alive(handle);
    }

    [[nodiscard]] T* get(ge::RuntimeHandle handle) noexcept {
        if (!handles_.alive(handle)) {
            return nullptr;
        }
        return slot_for(handle.index).object();
    }

    [[nodiscard]] const T* get(ge::RuntimeHandle handle) const noexcept {
        if (!handles_.alive(handle)) {
            return nullptr;
        }
        return slot_for(handle.index).object();
    }

    [[nodiscard]] T& at(ge::RuntimeHandle handle) {
        if (auto* object = get(handle)) {
            return *object;
        }
        throw std::out_of_range("object pool handle is stale or invalid");
    }

    [[nodiscard]] const T& at(ge::RuntimeHandle handle) const {
        if (const auto* object = get(handle)) {
            return *object;
        }
        throw std::out_of_range("object pool handle is stale or invalid");
    }

    [[nodiscard]] bool erase(ge::RuntimeHandle handle) noexcept {
        if (!handles_.alive(handle)) {
            return false;
        }
        auto& slot = slot_for(handle.index);
        std::destroy_at(slot.object());
        slot.handle = {};
        return handles_.release(handle);
    }

    void clear() noexcept {
        for (auto& page : pages_) {
            if (!page) {
                continue;
            }
            for (auto& slot : *page) {
                if (slot.handle.valid()) {
                    const auto handle = slot.handle;
                    std::destroy_at(slot.object());
                    slot.handle = {};
                    static_cast<void>(handles_.release(handle));
                }
            }
        }
    }

    template <class Function>
    void for_each(Function&& function) {
        for (auto& page : pages_) {
            if (!page) {
                continue;
            }
            for (auto& slot : *page) {
                if (slot.handle.valid()) {
                    std::invoke(function, slot.handle, *slot.object());
                }
            }
        }
    }

    template <class Function>
    void for_each(Function&& function) const {
        for (const auto& page : pages_) {
            if (!page) {
                continue;
            }
            for (const auto& slot : *page) {
                if (slot.handle.valid()) {
                    std::invoke(function, slot.handle, *slot.object());
                }
            }
        }
    }

    [[nodiscard]] std::size_t release_empty_pages() noexcept {
        std::size_t released = 0;
        for (auto& page : pages_) {
            if (page && std::none_of(page->begin(), page->end(),
                                    [](const Slot& slot) { return slot.handle.valid(); })) {
                page.reset();
                ++released;
            }
        }
        return released;
    }

    [[nodiscard]] std::uint32_t size() const noexcept {
        return handles_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return handles_.size() == 0;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return handles_.capacity();
    }

    [[nodiscard]] std::uint32_t available() const noexcept {
        return handles_.available_slots();
    }

    [[nodiscard]] std::uint32_t retired_slots() const noexcept {
        return handles_.retired_slots();
    }

    [[nodiscard]] std::size_t allocated_pages() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            pages_.begin(), pages_.end(), [](const auto& page) { return page != nullptr; }));
    }

private:
    [[nodiscard]] Slot& slot_for(std::uint32_t one_based_index) noexcept {
        const auto index = static_cast<std::size_t>(one_based_index - 1);
        return (*pages_[index / PageSize])[index % PageSize];
    }

    [[nodiscard]] const Slot& slot_for(std::uint32_t one_based_index) const noexcept {
        const auto index = static_cast<std::size_t>(one_based_index - 1);
        return (*pages_[index / PageSize])[index % PageSize];
    }

    ge::GenerationPool handles_;
    std::vector<std::unique_ptr<Page>> pages_;
};

template <class T>
class RingBuffer final {
    static_assert(std::is_nothrow_destructible_v<T>);

public:
    explicit RingBuffer(std::size_t capacity) : slots_(capacity) {}

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    template <class... Args>
    [[nodiscard]] T* emplace_back(Args&&... args) {
        if (full()) {
            return nullptr;
        }
        const auto index = physical_index(size_);
        auto& value = slots_[index].emplace(std::forward<Args>(args)...);
        ++size_;
        return std::addressof(value);
    }

    [[nodiscard]] bool push_back(const T& value) requires std::is_copy_constructible_v<T> {
        return emplace_back(value) != nullptr;
    }

    [[nodiscard]] bool push_back(T&& value) requires std::is_move_constructible_v<T> {
        return emplace_back(std::move(value)) != nullptr;
    }

    // The argument is constructed before an old element can be discarded.
    [[nodiscard]] bool push_overwrite(T value) requires std::is_nothrow_move_constructible_v<T> {
        if (capacity() == 0) {
            return false;
        }
        if (full()) {
            slots_[head_].reset();
            slots_[head_].emplace(std::move(value));
            advance_head();
            memory_detail::increment_saturated(overwritten_);
            return true;
        }
        static_cast<void>(emplace_back(std::move(value)));
        return true;
    }

    [[nodiscard]] bool pop_front() noexcept {
        if (empty()) {
            return false;
        }
        slots_[head_].reset();
        advance_head();
        --size_;
        if (empty()) {
            head_ = 0;
        }
        return true;
    }

    [[nodiscard]] bool pop_back() noexcept {
        if (empty()) {
            return false;
        }
        slots_[physical_index(size_ - 1)].reset();
        --size_;
        if (empty()) {
            head_ = 0;
        }
        return true;
    }

    [[nodiscard]] std::optional<T> take_front()
        requires (std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>) {
        if (empty()) {
            return std::nullopt;
        }
        // Return directly into the caller's result: moving a named optional
        // can throw even when the first construction safely copied T.
        struct PopOnSuccess {
            RingBuffer& queue;
            int exceptions{std::uncaught_exceptions()};

            ~PopOnSuccess() {
                if (std::uncaught_exceptions() == exceptions) {
                    static_cast<void>(queue.pop_front());
                }
            }
        } consume{*this};
        return std::optional<T>(std::in_place, std::move_if_noexcept(*slots_[head_]));
    }

    [[nodiscard]] T* front() noexcept {
        return empty() ? nullptr : std::addressof(*slots_[head_]);
    }

    [[nodiscard]] const T* front() const noexcept {
        return empty() ? nullptr : std::addressof(*slots_[head_]);
    }

    [[nodiscard]] T* back() noexcept {
        return empty() ? nullptr : std::addressof(*slots_[physical_index(size_ - 1)]);
    }

    [[nodiscard]] const T* back() const noexcept {
        return empty() ? nullptr : std::addressof(*slots_[physical_index(size_ - 1)]);
    }

    [[nodiscard]] T& at(std::size_t index) {
        if (index >= size_) {
            throw std::out_of_range("ring buffer index is outside the live range");
        }
        return *slots_[physical_index(index)];
    }

    [[nodiscard]] const T& at(std::size_t index) const {
        if (index >= size_) {
            throw std::out_of_range("ring buffer index is outside the live range");
        }
        return *slots_[physical_index(index)];
    }

    template <class Function>
    void for_each(Function&& function) {
        for (std::size_t index = 0; index < size_; ++index) {
            std::invoke(function, *slots_[physical_index(index)]);
        }
    }

    template <class Function>
    void for_each(Function&& function) const {
        for (std::size_t index = 0; index < size_; ++index) {
            std::invoke(function, *slots_[physical_index(index)]);
        }
    }

    template <class Predicate>
    [[nodiscard]] std::size_t discard_front_while(Predicate&& predicate) {
        std::size_t discarded = 0;
        while (!empty() && std::invoke(predicate, std::as_const(*front()))) {
            static_cast<void>(pop_front());
            ++discarded;
        }
        return discarded;
    }

    void clear() noexcept {
        while (pop_front()) {}
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return slots_.size();
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return capacity() - size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return size_ == capacity();
    }

    [[nodiscard]] std::uint64_t overwritten() const noexcept {
        return overwritten_;
    }

private:
    [[nodiscard]] std::size_t physical_index(std::size_t logical) const noexcept {
        const auto tail_space = capacity() - head_;
        return logical < tail_space ? head_ + logical : logical - tail_space;
    }

    void advance_head() noexcept {
        ++head_;
        if (head_ == capacity()) {
            head_ = 0;
        }
    }

    std::vector<std::optional<T>> slots_;
    std::size_t head_{};
    std::size_t size_{};
    std::uint64_t overwritten_{};
};

// Dense iteration and constant-time lookup/removal for bounded unsigned keys.
// Keys are local numeric indices, not generation-checked object identities.
template <class T>
class SparseSet final {
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_swappable_v<T>);

public:
    struct Entry {
        std::uint32_t key;
        T value;

        template <class... Args>
        explicit Entry(std::uint32_t entry_key, Args&&... args)
            : key(entry_key), value(std::forward<Args>(args)...) {}
    };

    explicit SparseSet(std::uint32_t key_capacity, std::size_t capacity)
        : sparse_(key_capacity, missing), capacity_(std::min(capacity,
                                                          static_cast<std::size_t>(key_capacity))) {
        dense_.reserve(capacity_);
    }

    SparseSet(const SparseSet&) = delete;
    SparseSet& operator=(const SparseSet&) = delete;
    SparseSet(SparseSet&&) = delete;
    SparseSet& operator=(SparseSet&&) = delete;

    template <class... Args>
    [[nodiscard]] T* emplace(std::uint32_t key, Args&&... args) {
        if (key >= sparse_.size()) {
            return nullptr;
        }
        if (sparse_[key] != missing) {
            return std::addressof(dense_[sparse_[key]].value);
        }
        if (dense_.size() == capacity_) {
            return nullptr;
        }
        dense_.emplace_back(key, std::forward<Args>(args)...);
        sparse_[key] = dense_.size() - 1;
        return std::addressof(dense_.back().value);
    }

    template <class... Args>
    [[nodiscard]] T* insert_or_assign(std::uint32_t key, Args&&... args) {
        if (T* existing = find(key)) {
            T replacement(std::forward<Args>(args)...);
            using std::swap;
            swap(*existing, replacement);
            return existing;
        }
        return emplace(key, std::forward<Args>(args)...);
    }

    [[nodiscard]] T* find(std::uint32_t key) noexcept {
        if (!contains(key)) {
            return nullptr;
        }
        return std::addressof(dense_[sparse_[key]].value);
    }

    [[nodiscard]] const T* find(std::uint32_t key) const noexcept {
        if (!contains(key)) {
            return nullptr;
        }
        return std::addressof(dense_[sparse_[key]].value);
    }

    [[nodiscard]] T& at(std::uint32_t key) {
        if (T* value = find(key)) {
            return *value;
        }
        throw std::out_of_range("sparse set key is absent");
    }

    [[nodiscard]] const T& at(std::uint32_t key) const {
        if (const T* value = find(key)) {
            return *value;
        }
        throw std::out_of_range("sparse set key is absent");
    }

    [[nodiscard]] bool contains(std::uint32_t key) const noexcept {
        return key < sparse_.size() && sparse_[key] != missing;
    }

    [[nodiscard]] bool erase(std::uint32_t key) noexcept {
        if (!contains(key)) {
            return false;
        }
        const auto removed = sparse_[key];
        const auto last = dense_.size() - 1;
        if (removed != last) {
            using std::swap;
            swap(dense_[removed].value, dense_[last].value);
            dense_[removed].key = dense_[last].key;
            sparse_[dense_[removed].key] = removed;
        }
        dense_.pop_back();
        sparse_[key] = missing;
        return true;
    }

    template <class Predicate>
    [[nodiscard]] std::size_t erase_if(Predicate&& predicate) {
        std::size_t removed = 0;
        std::size_t index = 0;
        while (index < dense_.size()) {
            const auto key = dense_[index].key;
            if (std::invoke(predicate, key, std::as_const(dense_[index].value))) {
                static_cast<void>(erase(key));
                ++removed;
            } else {
                ++index;
            }
        }
        return removed;
    }

    void clear() noexcept {
        for (const auto& entry : dense_) {
            sparse_[entry.key] = missing;
        }
        dense_.clear();
    }

    void sort_by_key() requires std::is_nothrow_move_assignable_v<T> {
        std::sort(dense_.begin(), dense_.end(),
                  [](const Entry& left, const Entry& right) noexcept { return left.key < right.key; });
        for (std::size_t index = 0; index < dense_.size(); ++index) {
            sparse_[dense_[index].key] = index;
        }
    }

    template <class Function>
    void for_each(Function&& function) {
        for (auto& entry : dense_) {
            std::invoke(function, entry.key, entry.value);
        }
    }

    template <class Function>
    void for_each(Function&& function) const {
        for (const auto& entry : dense_) {
            std::invoke(function, entry.key, entry.value);
        }
    }

    [[nodiscard]] std::span<const Entry> entries() const noexcept {
        return dense_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return dense_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return dense_.empty();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] std::size_t key_capacity() const noexcept {
        return sparse_.size();
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return capacity_ - dense_.size();
    }

private:
    static constexpr std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> sparse_;
    std::vector<Entry> dense_;
    std::size_t capacity_;
};

class DynamicBitSet final {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t word_bits = 64;

    explicit DynamicBitSet(std::size_t bits = 0,
                           std::size_t maximum_bits = std::numeric_limits<std::size_t>::max())
        : maximum_bits_(maximum_bits) {
        resize(bits);
    }

    DynamicBitSet(const DynamicBitSet&) = default;
    DynamicBitSet& operator=(const DynamicBitSet&) = default;

    DynamicBitSet(DynamicBitSet&& other) noexcept
        : words_(std::move(other.words_)),
          bit_count_(std::exchange(other.bit_count_, 0)),
          maximum_bits_(other.maximum_bits_) {
        other.words_.clear();
    }

    DynamicBitSet& operator=(DynamicBitSet&& other) noexcept {
        if (this != &other) {
            words_ = std::move(other.words_);
            bit_count_ = std::exchange(other.bit_count_, 0);
            maximum_bits_ = other.maximum_bits_;
            other.words_.clear();
        }
        return *this;
    }

    void resize(std::size_t bits, bool value = false) {
        if (bits > maximum_bits_) {
            throw std::length_error("bit set exceeds its configured bound");
        }
        const auto previous = bit_count_;
        words_.resize(word_count(bits), value ? ~std::uint64_t{0} : std::uint64_t{0});
        bit_count_ = bits;
        if (bits > previous) {
            static_cast<void>(set_range(previous, bits - previous, value));
        }
        trim_padding();
    }

    [[nodiscard]] bool test(std::size_t bit) const noexcept {
        return bit < bit_count_ && (words_[bit / word_bits] & bit_mask(bit)) != 0;
    }

    [[nodiscard]] bool at(std::size_t bit) const {
        if (bit >= bit_count_) {
            throw std::out_of_range("bit set index is outside its live range");
        }
        return test(bit);
    }

    [[nodiscard]] bool set(std::size_t bit, bool value = true) noexcept {
        if (bit >= bit_count_) {
            return false;
        }
        auto& word = words_[bit / word_bits];
        if (value) {
            word |= bit_mask(bit);
        } else {
            word &= ~bit_mask(bit);
        }
        return true;
    }

    [[nodiscard]] bool reset(std::size_t bit) noexcept {
        return set(bit, false);
    }

    [[nodiscard]] bool flip(std::size_t bit) noexcept {
        if (bit >= bit_count_) {
            return false;
        }
        words_[bit / word_bits] ^= bit_mask(bit);
        return true;
    }

    void reset() noexcept {
        std::fill(words_.begin(), words_.end(), std::uint64_t{0});
    }

    void set_all() noexcept {
        std::fill(words_.begin(), words_.end(), ~std::uint64_t{0});
        trim_padding();
    }

    void flip_all() noexcept {
        for (auto& word : words_) {
            word = ~word;
        }
        trim_padding();
    }

    [[nodiscard]] bool set_range(std::size_t first, std::size_t count, bool value = true) noexcept {
        if (first > bit_count_ || count > bit_count_ - first) {
            return false;
        }
        while (count != 0) {
            const auto offset = first % word_bits;
            const auto amount = std::min(count, word_bits - offset);
            const auto mask = low_mask(amount) << offset;
            auto& word = words_[first / word_bits];
            if (value) {
                word |= mask;
            } else {
                word &= ~mask;
            }
            first += amount;
            count -= amount;
        }
        return true;
    }

    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t result = 0;
        for (auto word : words_) {
            result += static_cast<std::size_t>(std::popcount(word));
        }
        return result;
    }

    [[nodiscard]] std::size_t count_range(std::size_t first, std::size_t length) const noexcept {
        if (first >= bit_count_) {
            return 0;
        }
        length = std::min(length, bit_count_ - first);
        std::size_t result = 0;
        while (length != 0) {
            const auto offset = first % word_bits;
            const auto amount = std::min(length, word_bits - offset);
            const auto word = (words_[first / word_bits] >> offset) & low_mask(amount);
            result += static_cast<std::size_t>(std::popcount(word));
            first += amount;
            length -= amount;
        }
        return result;
    }

    [[nodiscard]] bool any() const noexcept {
        return std::any_of(words_.begin(), words_.end(), [](auto word) { return word != 0; });
    }

    [[nodiscard]] bool none() const noexcept {
        return !any();
    }

    [[nodiscard]] bool all() const noexcept {
        if (words_.empty()) {
            return true;
        }
        for (std::size_t index = 0; index + 1 < words_.size(); ++index) {
            if (words_[index] != ~std::uint64_t{0}) {
                return false;
            }
        }
        const auto tail_bits = bit_count_ % word_bits;
        return words_.back() == low_mask(tail_bits == 0 ? word_bits : tail_bits);
    }

    [[nodiscard]] std::size_t find_first() const noexcept {
        return find_from(0, true);
    }

    [[nodiscard]] std::size_t find_next(std::size_t previous) const noexcept {
        if (previous == npos || previous >= bit_count_) {
            return npos;
        }
        return find_from(previous + 1, true);
    }

    [[nodiscard]] std::size_t find_first_clear() const noexcept {
        return find_from(0, false);
    }

    [[nodiscard]] std::size_t find_next_clear(std::size_t previous) const noexcept {
        if (previous == npos || previous >= bit_count_) {
            return npos;
        }
        return find_from(previous + 1, false);
    }

    [[nodiscard]] std::size_t find_from(std::size_t first, bool value = true) const noexcept {
        if (first >= bit_count_) {
            return npos;
        }
        auto index = first / word_bits;
        auto word = value ? words_[index] : ~words_[index];
        word &= ~low_mask(first % word_bits);
        for (;;) {
            if (word != 0) {
                const auto found = index * word_bits +
                                   static_cast<std::size_t>(std::countr_zero(word));
                return found < bit_count_ ? found : npos;
            }
            ++index;
            if (index == words_.size()) {
                return npos;
            }
            word = value ? words_[index] : ~words_[index];
        }
    }

    [[nodiscard]] std::size_t find_run(std::size_t length, bool value = true,
                                      std::size_t first = 0) const noexcept {
        if (first > bit_count_ || length > bit_count_ - first) {
            return npos;
        }
        if (length == 0) {
            return first;
        }
        auto candidate = find_from(first, value);
        while (candidate != npos && length <= bit_count_ - candidate) {
            const auto interruption = find_from(candidate, !value);
            if (interruption == npos || interruption - candidate >= length) {
                return candidate;
            }
            candidate = find_from(interruption + 1, value);
        }
        return npos;
    }

    [[nodiscard]] std::size_t select(std::size_t rank) const noexcept {
        for (std::size_t index = 0; index < words_.size(); ++index) {
            auto word = words_[index];
            const auto present = static_cast<std::size_t>(std::popcount(word));
            if (rank >= present) {
                rank -= present;
                continue;
            }
            while (rank != 0) {
                word &= word - 1;
                --rank;
            }
            return index * word_bits + static_cast<std::size_t>(std::countr_zero(word));
        }
        return npos;
    }

    void intersect_with(const DynamicBitSet& other) {
        require_compatible(other);
        for (std::size_t index = 0; index < words_.size(); ++index) {
            words_[index] &= other.words_[index];
        }
    }

    void union_with(const DynamicBitSet& other) {
        require_compatible(other);
        for (std::size_t index = 0; index < words_.size(); ++index) {
            words_[index] |= other.words_[index];
        }
    }

    void difference_with(const DynamicBitSet& other) {
        require_compatible(other);
        for (std::size_t index = 0; index < words_.size(); ++index) {
            words_[index] &= ~other.words_[index];
        }
    }

    void symmetric_difference_with(const DynamicBitSet& other) {
        require_compatible(other);
        for (std::size_t index = 0; index < words_.size(); ++index) {
            words_[index] ^= other.words_[index];
        }
    }

    [[nodiscard]] bool intersects(const DynamicBitSet& other) const {
        require_compatible(other);
        for (std::size_t index = 0; index < words_.size(); ++index) {
            if ((words_[index] & other.words_[index]) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_subset_of(const DynamicBitSet& other) const {
        require_compatible(other);
        for (std::size_t index = 0; index < words_.size(); ++index) {
            if ((words_[index] & ~other.words_[index]) != 0) {
                return false;
            }
        }
        return true;
    }

    void shift_left(std::size_t amount) noexcept {
        if (amount >= bit_count_) {
            reset();
            return;
        }
        if (amount == 0) {
            return;
        }
        const auto whole = amount / word_bits;
        const auto partial = amount % word_bits;
        for (auto destination = words_.size(); destination != 0;) {
            --destination;
            std::uint64_t result = 0;
            if (destination >= whole) {
                const auto source = destination - whole;
                result = words_[source] << partial;
                if (partial != 0 && source != 0) {
                    result |= words_[source - 1] >> (word_bits - partial);
                }
            }
            words_[destination] = result;
        }
        trim_padding();
    }

    void shift_right(std::size_t amount) noexcept {
        if (amount >= bit_count_) {
            reset();
            return;
        }
        if (amount == 0) {
            return;
        }
        const auto whole = amount / word_bits;
        const auto partial = amount % word_bits;
        for (std::size_t destination = 0; destination < words_.size(); ++destination) {
            std::uint64_t result = 0;
            if (whole < words_.size() - destination) {
                const auto source = destination + whole;
                result = words_[source] >> partial;
                if (partial != 0 && source + 1 < words_.size()) {
                    result |= words_[source + 1] << (word_bits - partial);
                }
            }
            words_[destination] = result;
        }
        trim_padding();
    }

    [[nodiscard]] bool assign_words(std::span<const std::uint64_t> words) {
        if (words.size() != words_.size()) {
            return false;
        }
        if (!words.empty() && bit_count_ % word_bits != 0 &&
            (words.back() & ~low_mask(bit_count_ % word_bits)) != 0) {
            return false;
        }
        std::vector<std::uint64_t> replacement(words.begin(), words.end());
        words_.swap(replacement);
        return true;
    }

    template <class Function>
    void for_each_set(Function&& function) const {
        for (std::size_t index = 0; index < words_.size(); ++index) {
            auto word = words_[index];
            while (word != 0) {
                const auto bit = static_cast<std::size_t>(std::countr_zero(word));
                std::invoke(function, index * word_bits + bit);
                word &= word - 1;
            }
        }
    }

    [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
        return words_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return bit_count_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return bit_count_ == 0;
    }

    [[nodiscard]] std::size_t maximum_size() const noexcept {
        return maximum_bits_;
    }

    [[nodiscard]] bool operator==(const DynamicBitSet& other) const noexcept {
        return bit_count_ == other.bit_count_ && words_ == other.words_;
    }

private:
    [[nodiscard]] static constexpr std::size_t word_count(std::size_t bits) noexcept {
        return bits / word_bits + (bits % word_bits != 0 ? 1U : 0U);
    }

    [[nodiscard]] static constexpr std::uint64_t low_mask(std::size_t bits) noexcept {
        return bits == word_bits ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    }

    [[nodiscard]] static constexpr std::uint64_t bit_mask(std::size_t bit) noexcept {
        return std::uint64_t{1} << (bit % word_bits);
    }

    void trim_padding() noexcept {
        if (!words_.empty() && bit_count_ % word_bits != 0) {
            words_.back() &= low_mask(bit_count_ % word_bits);
        }
    }

    void require_compatible(const DynamicBitSet& other) const {
        if (bit_count_ != other.bit_count_) {
            throw std::invalid_argument("bit set lengths must match");
        }
    }

    std::vector<std::uint64_t> words_;
    std::size_t bit_count_{};
    std::size_t maximum_bits_;
};

// Append-only indexed storage with stable addresses until pop/truncate/clear.
// Page table growth never moves existing T objects; page capacity is explicit.
template <class T, std::size_t PageSize = 256>
class PagedVector final {
    static_assert(PageSize > 0);
    static_assert(std::is_nothrow_destructible_v<T>);

    struct Slot {
        alignas(T) std::byte bytes[sizeof(T)];

        [[nodiscard]] T* object() noexcept {
            return std::launder(reinterpret_cast<T*>(bytes));
        }

        [[nodiscard]] const T* object() const noexcept {
            return std::launder(reinterpret_cast<const T*>(bytes));
        }
    };

    using Page = std::array<Slot, PageSize>;

public:
    explicit PagedVector(std::size_t maximum_size) : maximum_size_(maximum_size) {}

    ~PagedVector() {
        clear();
    }

    PagedVector(const PagedVector&) = delete;
    PagedVector& operator=(const PagedVector&) = delete;
    PagedVector(PagedVector&&) = delete;
    PagedVector& operator=(PagedVector&&) = delete;

    template <class... Args>
    [[nodiscard]] T* emplace_back(Args&&... args) {
        if (size_ == maximum_size_) {
            return nullptr;
        }
        const auto page_index = size_ / PageSize;
        if (page_index == pages_.size()) {
            auto page = std::make_unique<Page>();
            pages_.push_back(std::move(page));
        }
        auto& slot = (*pages_[page_index])[size_ % PageSize];
        T* value = std::construct_at(reinterpret_cast<T*>(slot.bytes), std::forward<Args>(args)...);
        ++size_;
        return value;
    }

    [[nodiscard]] bool reserve(std::size_t count) {
        if (count > maximum_size_) {
            return false;
        }
        const auto required = count / PageSize + (count % PageSize != 0 ? 1U : 0U);
        pages_.reserve(required);
        while (pages_.size() < required) {
            pages_.push_back(std::make_unique<Page>());
        }
        return true;
    }

    [[nodiscard]] bool pop_back() noexcept {
        if (size_ == 0) {
            return false;
        }
        --size_;
        std::destroy_at(slot_for(size_).object());
        return true;
    }

    [[nodiscard]] bool truncate(std::size_t count) noexcept {
        if (count > size_) {
            return false;
        }
        while (size_ != count) {
            static_cast<void>(pop_back());
        }
        return true;
    }

    template <class Factory>
    [[nodiscard]] bool resize_with(std::size_t count, Factory&& factory) {
        if (count > maximum_size_) {
            return false;
        }
        if (count <= size_) {
            return truncate(count);
        }
        const auto previous = size_;
        try {
            while (size_ < count) {
                static_cast<void>(emplace_back(std::invoke(factory, size_)));
            }
        } catch (...) {
            static_cast<void>(truncate(previous));
            throw;
        }
        return true;
    }

    [[nodiscard]] bool append(std::span<const T> values) requires std::is_copy_constructible_v<T> {
        if (values.size() > maximum_size_ - size_) {
            return false;
        }
        const auto previous = size_;
        try {
            for (const auto& value : values) {
                static_cast<void>(emplace_back(value));
            }
        } catch (...) {
            static_cast<void>(truncate(previous));
            throw;
        }
        return true;
    }

    [[nodiscard]] T* get(std::size_t index) noexcept {
        return index < size_ ? slot_for(index).object() : nullptr;
    }

    [[nodiscard]] const T* get(std::size_t index) const noexcept {
        return index < size_ ? slot_for(index).object() : nullptr;
    }

    [[nodiscard]] T& at(std::size_t index) {
        if (T* value = get(index)) {
            return *value;
        }
        throw std::out_of_range("paged vector index is outside the live range");
    }

    [[nodiscard]] const T& at(std::size_t index) const {
        if (const T* value = get(index)) {
            return *value;
        }
        throw std::out_of_range("paged vector index is outside the live range");
    }

    [[nodiscard]] T* front() noexcept {
        return get(0);
    }

    [[nodiscard]] const T* front() const noexcept {
        return get(0);
    }

    [[nodiscard]] T* back() noexcept {
        return size_ == 0 ? nullptr : get(size_ - 1);
    }

    [[nodiscard]] const T* back() const noexcept {
        return size_ == 0 ? nullptr : get(size_ - 1);
    }

    template <class Function>
    void for_each(Function&& function) {
        for (std::size_t index = 0; index < size_; ++index) {
            std::invoke(function, *slot_for(index).object());
        }
    }

    template <class Function>
    void for_each(Function&& function) const {
        for (std::size_t index = 0; index < size_; ++index) {
            std::invoke(function, *slot_for(index).object());
        }
    }

    template <class Function>
    void for_each_indexed(Function&& function) {
        for (std::size_t index = 0; index < size_; ++index) {
            std::invoke(function, index, *slot_for(index).object());
        }
    }

    void clear() noexcept {
        static_cast<void>(truncate(0));
    }

    void release_unused_pages() noexcept {
        const auto required = size_ / PageSize + (size_ % PageSize != 0 ? 1U : 0U);
        while (pages_.size() > required) {
            pages_.pop_back();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] std::size_t maximum_size() const noexcept {
        return maximum_size_;
    }

    [[nodiscard]] std::size_t allocated_pages() const noexcept {
        return pages_.size();
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return maximum_size_ - size_;
    }

private:
    [[nodiscard]] Slot& slot_for(std::size_t index) noexcept {
        return (*pages_[index / PageSize])[index % PageSize];
    }

    [[nodiscard]] const Slot& slot_for(std::size_t index) const noexcept {
        return (*pages_[index / PageSize])[index % PageSize];
    }

    std::vector<std::unique_ptr<Page>> pages_;
    std::size_t size_{};
    std::size_t maximum_size_;
};

} // namespace ge::core
