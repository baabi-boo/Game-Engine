#include <ge/core/binary.hpp>

#include "test_support.hpp"

#include <cstdlib>
#include <new>

namespace {

std::size_t rejected_allocation_size{};

struct RejectAllocation {
    explicit RejectAllocation(std::size_t size) { rejected_allocation_size = size; }
    ~RejectAllocation() { rejected_allocation_size = 0; }
};

} // namespace

void* operator new(std::size_t size) {
    if (rejected_allocation_size != 0 && size == rejected_allocation_size) {
        rejected_allocation_size = 0;
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) { return memory; }
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace ge::core;

void failed_bit_writer_copy_preserves_bits_and_capacity() {
    BitWriter source(3);
    source.write(0xabcdef, 24);
    BitWriter destination(1);
    destination.write_bool(true);
    {
        const RejectAllocation reject_copy(source.bytes().size());
        CHECK_THROWS_AS(destination = source, std::bad_alloc);
    }
    CHECK(destination.bit_size() == 1);
    CHECK(destination.bytes().size() == 1);
    CHECK(destination.bytes().front() == std::byte{1});
    destination.write(0x7f, 7);
    CHECK(destination.bit_size() == 8);
    CHECK(destination.bytes().front() == std::byte{0xff});
    CHECK_THROWS_AS(destination.write_bool(true), BinaryException);

    destination = source;
    CHECK(destination.bit_size() == 24);
    BitReader reader(destination.bytes());
    CHECK(reader.read(24) == 0xabcdef);
    reader.require_end();
    CHECK_THROWS_AS(destination.write_bool(true), BinaryException);
    CHECK(source.bit_size() == 24);
}

} // namespace

int main() {
    return ge::test::run("binary_limits_audit", {
        {"failed_bit_writer_copy", failed_bit_writer_copy_preserves_bits_and_capacity},
    });
}
