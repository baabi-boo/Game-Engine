#include <ge/core/memory.hpp>

#include "test_support.hpp"

#include <memory>
#include <stdexcept>

namespace {

struct CopyState {
    bool fail_copy{};
    int copies{};
    int moves{};
    int live{};
};

struct ThrowingMove {
    CopyState& state;
    int value;

    ThrowingMove(CopyState& copy_state, int initial) : state(copy_state), value(initial) {
        ++state.live;
    }

    ThrowingMove(const ThrowingMove& other) : state(other.state), value(other.value) {
        ++state.copies;
        if (state.fail_copy) {
            throw std::runtime_error("copy failure");
        }
        ++state.live;
    }

    ThrowingMove(ThrowingMove&& other) : state(other.state), value(other.value) {
        ++state.moves;
        throw std::runtime_error("move failure");
    }

    ~ThrowingMove() {
        --state.live;
    }
};

void extraction_does_not_require_optional_return_elision() {
    // Compile this suite with -fno-elide-constructors on GCC/Clang as well:
    // a named optional return used to move after the source was removed.
    CopyState state;
    {
        ge::core::RingBuffer<ThrowingMove> queue(2);
        CHECK(queue.emplace_back(state, 17));
        CHECK(queue.emplace_back(state, 29));
        const auto value = queue.take_front();
        CHECK(value && value->value == 17);
        CHECK(state.copies == 1);
        CHECK(state.moves == 0);
        CHECK(state.live == 2);
        CHECK(queue.size() == 1);
        CHECK(queue.front()->value == 29);
    }
    CHECK(state.live == 0);
}

void failed_extraction_preserves_queue_and_lifetimes() {
    CopyState state;
    {
        ge::core::RingBuffer<ThrowingMove> queue(2);
        CHECK(queue.emplace_back(state, 41));
        CHECK(queue.emplace_back(state, 53));
        const auto* original = queue.front();
        state.fail_copy = true;
        CHECK_THROWS_AS(queue.take_front(), std::runtime_error);
        CHECK(queue.size() == 2);
        CHECK(queue.front() == original);
        CHECK(queue.front()->value == 41);
        CHECK(queue.back()->value == 53);
        CHECK(state.live == 2);
        CHECK(state.moves == 0);

        state.fail_copy = false;
        const auto value = queue.take_front();
        CHECK(value && value->value == 41);
        CHECK(queue.size() == 1);
        CHECK(queue.front()->value == 53);
        CHECK(state.live == 2);
    }
    CHECK(state.live == 0);
}

void move_only_extraction_and_unwinding() {
    ge::core::RingBuffer<std::unique_ptr<int>> queue(2);
    CHECK(queue.emplace_back(std::make_unique<int>(67)));
    CHECK(queue.emplace_back(std::make_unique<int>(71)));
    CHECK(queue.pop_front());
    CHECK(queue.emplace_back(std::make_unique<int>(73)));
    const auto first = queue.take_front();
    CHECK(first && **first == 71);
    const auto second = queue.take_front();
    CHECK(second && **second == 73);
    CHECK(queue.empty());
    CHECK(!queue.take_front());

    ge::core::RingBuffer<int> during_unwind(1);
    CHECK(during_unwind.emplace_back(79));
    int extracted = 0;
    struct ExtractOnUnwind {
        ge::core::RingBuffer<int>& source;
        int& destination;
        ~ExtractOnUnwind() {
            const auto value = source.take_front();
            destination = value.value_or(0);
        }
    };
    try {
        ExtractOnUnwind guard{during_unwind, extracted};
        throw std::runtime_error("unrelated exception");
    } catch (const std::runtime_error&) {
    }
    CHECK(extracted == 79);
    CHECK(during_unwind.empty());
}

} // namespace

int main() {
    return ge::test::run("memory_audit", {
        {"extraction_without_optional_elision", extraction_does_not_require_optional_return_elision},
        {"failed_extraction_preserves_queue", failed_extraction_preserves_queue_and_lifetimes},
        {"move_only_extraction_and_unwinding", move_only_extraction_and_unwinding},
    });
}
