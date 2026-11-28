#include <ge/core/binary.hpp>

#include "test_support.hpp"

#include <utility>

namespace {

using namespace ge::core;

void prime_decoder(FrameDecoder& decoder) {
    const auto first = encode_frame({1, 10, {std::byte{0x12}}});
    const auto second = encode_frame({2, 20, {std::byte{0x34}}});
    CHECK(decoder.feed(first) == first.size());
    CHECK(decoder.feed(second) == second.size());
    const auto decoded = decoder.next();
    CHECK(decoded && decoded->sequence == 10);
    CHECK(decoder.buffered_bytes() == second.size());
}

void check_moved_decoder(FrameDecoder& destination, FrameDecoder& source) {
    CHECK(destination.frames_decoded() == 1);
    const auto second = destination.next();
    CHECK(second && second->type == 2 && second->sequence == 20);
    CHECK(second->payload == std::vector<std::byte>{std::byte{0x34}});
    destination.finish();

    CHECK(source.buffered_bytes() == 0);
    CHECK(source.available_capacity() == 64);
    CHECK(source.frames_decoded() == 0);
    CHECK(!source.failed());
    CHECK(!source.next());
    source.finish();
    const auto next = encode_frame({3, 30, {}});
    CHECK(source.feed(next) == next.size());
    const auto reused = source.next();
    CHECK(reused && reused->sequence == 30);
}

void moving_partially_consumed_decoder_resets_source() {
    BinaryLimits limits;
    limits.max_total_bytes = 64;
    FrameDecoder source(limits);
    prime_decoder(source);
    FrameDecoder destination(std::move(source));
    check_moved_decoder(destination, source);
}

void assigning_partially_consumed_decoder_resets_source() {
    BinaryLimits limits;
    limits.max_total_bytes = 64;
    FrameDecoder source(limits);
    prime_decoder(source);
    FrameDecoder destination;
    const auto previous = encode_frame({4, 40, {}});
    CHECK(destination.feed(previous) == previous.size());
    destination = std::move(source);
    check_moved_decoder(destination, source);
    CHECK(destination.available_capacity() == 64);
}

void check_moved_bit_writer(BitWriter& destination, BitWriter& source) {
    CHECK(destination.bit_size() == 9);
    BitReader reader(destination.bytes());
    CHECK(reader.read(9) == 0x105);
    reader.require_end();
    destination.write(0x42, 7);
    CHECK_THROWS_AS(destination.write_bool(true), BinaryException);
    CHECK(destination.bit_size() == 16);
    BitReader completed(destination.bytes());
    CHECK(completed.read(9) == 0x105);
    CHECK(completed.read(7) == 0x42);
    completed.require_end();

    CHECK(source.bit_size() == 0);
    CHECK(source.bytes().empty());
    source.write_bool(true);
    CHECK(source.bit_size() == 1);
    CHECK(source.bytes().size() == 1);
    CHECK(source.bytes().front() == std::byte{1});
    CHECK_THROWS_AS(source.write(0, 16), BinaryException);
    CHECK(source.bit_size() == 1);
}

void moving_bit_writer_resets_source() {
    BitWriter source(2);
    source.write(0x105, 9);
    BitWriter destination(std::move(source));
    check_moved_bit_writer(destination, source);
}

void assigning_bit_writer_resets_source() {
    BitWriter source(2);
    source.write(0x105, 9);
    BitWriter destination(4);
    destination.write(0xff, 8);
    destination = std::move(source);
    check_moved_bit_writer(destination, source);
}

} // namespace

int main() {
    return ge::test::run("binary_audit", {
        {"move_partially_consumed_decoder", moving_partially_consumed_decoder_resets_source},
        {"assign_partially_consumed_decoder", assigning_partially_consumed_decoder_resets_source},
        {"move_bit_writer", moving_bit_writer_resets_source},
        {"assign_bit_writer", assigning_bit_writer_resets_source},
    });
}
