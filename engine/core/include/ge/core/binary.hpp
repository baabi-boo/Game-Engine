#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core {

enum class ByteOrder { little, big };

enum class BinaryError {
    truncated,
    limit_exceeded,
    invalid_value,
    invalid_utf8,
    noncanonical,
    invalid_magic,
    unsupported_version,
    checksum_mismatch,
    duplicate_identifier,
    overlapping_range,
    trailing_data,
    decoder_failed
};

class BinaryException final : public std::runtime_error {
public:
    BinaryException(BinaryError error, std::size_t offset, std::string message);
    [[nodiscard]] BinaryError error() const noexcept;
    [[nodiscard]] std::size_t offset() const noexcept;

private:
    BinaryError error_;
    std::size_t offset_;
};

struct BinaryLimits {
    std::size_t max_total_bytes{64U * 1024U * 1024U};
    std::size_t max_string_bytes{1024U * 1024U};
    std::size_t max_chunks{4096};
    std::size_t max_chunk_bytes{16U * 1024U * 1024U};
    std::size_t max_frame_bytes{16U * 1024U * 1024U};
};

[[nodiscard]] bool checked_add(std::size_t a, std::size_t b,
                               std::size_t& result) noexcept;
[[nodiscard]] bool checked_multiply(std::size_t a, std::size_t b,
                                    std::size_t& result) noexcept;
[[nodiscard]] std::optional<std::size_t> aligned_size(std::size_t value,
                                                     std::size_t alignment) noexcept;
[[nodiscard]] bool valid_utf8(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] bool valid_utf8(std::string_view text) noexcept;
[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept;

// Readers borrow input. A successful read advances; a rejected scalar or string
// leaves its cursor unchanged. A subreader also borrows the original storage.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes,
                        ByteOrder order = ByteOrder::little,
                        BinaryLimits limits = {});

    [[nodiscard]] std::size_t position() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] ByteOrder order() const noexcept;
    [[nodiscard]] const BinaryLimits& limits() const noexcept;
    [[nodiscard]] std::span<const std::byte> source() const noexcept;
    [[nodiscard]] std::span<const std::byte> unread() const noexcept;
    void seek(std::size_t position);
    void skip(std::size_t count);
    void align(std::size_t alignment, bool require_zero = true);
    void require_end() const;
    void expect(std::span<const std::byte> expected);
    void expect(std::string_view expected);
    void require_zero(std::size_t count);

    [[nodiscard]] std::uint8_t read_u8();
    [[nodiscard]] std::uint16_t read_u16();
    [[nodiscard]] std::uint32_t read_u32();
    [[nodiscard]] std::uint64_t read_u64();
    [[nodiscard]] std::int8_t read_i8();
    [[nodiscard]] std::int16_t read_i16();
    [[nodiscard]] std::int32_t read_i32();
    [[nodiscard]] std::int64_t read_i64();
    [[nodiscard]] float read_f32(bool require_finite = true);
    [[nodiscard]] double read_f64(bool require_finite = true);
    [[nodiscard]] bool read_bool();
    [[nodiscard]] std::uint64_t read_varuint();
    [[nodiscard]] std::int64_t read_varsint();
    [[nodiscard]] std::span<const std::byte> read_bytes(std::size_t count);
    [[nodiscard]] std::span<const std::byte> peek_bytes(std::size_t count) const;
    [[nodiscard]] std::string_view read_string_view();
    [[nodiscard]] std::string read_string();
    [[nodiscard]] std::string_view read_cstring(std::size_t maximum_bytes);
    [[nodiscard]] ByteReader read_subreader(std::size_t count);
    [[nodiscard]] ByteReader read_sized_subreader();
    [[nodiscard]] std::vector<std::uint32_t> read_u32_array(std::size_t maximum_count);
    [[nodiscard]] std::vector<float> read_f32_array(std::size_t maximum_count,
                                                 bool require_finite = true);

private:
    void require(std::size_t count) const;
    [[nodiscard]] std::uint64_t integer(std::size_t width);
    std::span<const std::byte> bytes_;
    std::size_t position_{};
    ByteOrder order_;
    BinaryLimits limits_;
};

// Copy the reader for speculative parsing, then assign it back after success.
// Writers own their bytes and enforce the limit before allocation or mutation.
class ByteWriter {
public:
    explicit ByteWriter(std::size_t maximum_bytes = 64U * 1024U * 1024U,
                        ByteOrder order = ByteOrder::little);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t maximum_size() const noexcept;
    [[nodiscard]] std::size_t remaining_capacity() const noexcept;
    [[nodiscard]] ByteOrder order() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] const std::vector<std::byte>& storage() const noexcept;
    [[nodiscard]] std::vector<std::byte> take() noexcept;
    void clear() noexcept;
    void reserve(std::size_t capacity);
    void truncate(std::size_t size);
    void write_u8(std::uint8_t value);
    void write_u16(std::uint16_t value);
    void write_u32(std::uint32_t value);
    void write_u64(std::uint64_t value);
    void write_i8(std::int8_t value);
    void write_i16(std::int16_t value);
    void write_i32(std::int32_t value);
    void write_i64(std::int64_t value);
    void write_f32(float value, bool require_finite = true);
    void write_f64(double value, bool require_finite = true);
    void write_bool(bool value);
    void write_varuint(std::uint64_t value);
    void write_varsint(std::int64_t value);
    void write_bytes(std::span<const std::byte> bytes);
    void write_bytes(std::string_view bytes);
    void write_zeroes(std::size_t count);
    void write_string(std::string_view text, std::size_t maximum_bytes = 1024U * 1024U);
    void write_cstring(std::string_view text, std::size_t maximum_bytes = 1024U * 1024U);
    void write_sized_bytes(std::span<const std::byte> bytes);
    void write_u32_array(std::span<const std::uint32_t> values);
    void write_f32_array(std::span<const float> values, bool require_finite = true);
    void align(std::size_t alignment);
    void patch_u16(std::size_t offset, std::uint16_t value);
    void patch_u32(std::size_t offset, std::uint32_t value);
    void patch_u64(std::size_t offset, std::uint64_t value);
    void patch_bytes(std::size_t offset, std::span<const std::byte> bytes);

private:
    void prepare(std::size_t additional);
    void integer(std::uint64_t value, std::size_t width);
    void patch_integer(std::size_t offset, std::uint64_t value, std::size_t width);
    [[nodiscard]] bool overlaps(std::span<const std::byte> bytes) const noexcept;
    std::vector<std::byte> bytes_;
    std::size_t maximum_bytes_;
    ByteOrder order_;
};

// These noncryptographic checksums detect corruption or accelerate lookup. They
// do not authenticate files, peers, ownership, or executable code.
class Crc32 {
public:
    void update(std::span<const std::byte> bytes) noexcept;
    void update(std::string_view text) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint32_t value() const noexcept;

private:
    std::uint32_t state_{0xffffffffU};
};

class Fnv1a64 {
public:
    void update(std::span<const std::byte> bytes) noexcept;
    void update(std::string_view text) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint64_t value() const noexcept;

private:
    std::uint64_t state_{14695981039346656037ULL};
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint64_t fnv1a64(std::string_view text) noexcept;
[[nodiscard]] std::string encode_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> decode_hex(std::string_view text,
                                              std::size_t maximum_bytes);

struct BinaryChunk {
    std::uint32_t id{};
    std::uint16_t version{1};
    std::vector<std::byte> data;
};

struct ChunkArchive {
    std::uint32_t schema{};
    std::uint64_t revision{};
    std::vector<BinaryChunk> chunks;
    [[nodiscard]] const BinaryChunk* find(std::uint32_t id) const noexcept;
    [[nodiscard]] BinaryChunk* find(std::uint32_t id) noexcept;
};

// GECHNK01 is a separate extensible container, never an implicit replacement for
// GEPACK01 or GESTATE1. Encoding sorts chunk IDs; decoding requires that order,
// contiguous payloads, a valid archive CRC, and each chunk's own CRC.
[[nodiscard]] std::vector<std::byte> encode_archive(const ChunkArchive& archive,
                                                   BinaryLimits limits = {});
[[nodiscard]] ChunkArchive decode_archive(std::span<const std::byte> bytes,
                                          BinaryLimits limits = {});

struct BinaryFrame {
    std::uint32_t type{};
    std::uint64_t sequence{};
    std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> encode_frame(const BinaryFrame& frame,
                                                 BinaryLimits limits = {});

// Feed reports accepted bytes, so a transport can retain the unconsumed suffix.
// next() emits complete frames in arrival order and latches malformed-input
// failures until reset. has_complete_frame() is an observational header probe.
// Moving preserves the destination stream and resets the source with its limits.
// This class makes no peer authentication or sequence-order claim.
class FrameDecoder {
public:
    explicit FrameDecoder(BinaryLimits limits = {});
    FrameDecoder(const FrameDecoder&) = default;
    FrameDecoder& operator=(const FrameDecoder&) = default;
    FrameDecoder(FrameDecoder&& other) noexcept;
    FrameDecoder& operator=(FrameDecoder&& other) noexcept;
    [[nodiscard]] std::size_t feed(std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<BinaryFrame> next();
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    [[nodiscard]] std::size_t available_capacity() const noexcept;
    [[nodiscard]] std::uint64_t frames_decoded() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] bool has_complete_frame() const;
    void finish() const;
    void reset() noexcept;

private:
    void compact();
    [[nodiscard]] std::optional<std::size_t> frame_size() const;
    BinaryLimits limits_;
    std::vector<std::byte> buffer_;
    std::size_t begin_{};
    std::uint64_t frames_decoded_{};
    bool failed_{};
};

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> bytes);
    [[nodiscard]] std::size_t position() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;
    [[nodiscard]] std::uint64_t read(unsigned width);
    [[nodiscard]] std::int64_t read_signed(unsigned width);
    [[nodiscard]] bool read_bool();
    void skip(std::size_t bits);
    void align(bool require_zero = true);
    void require_end(bool allow_zero_padding = true);

private:
    std::span<const std::byte> bytes_;
    std::size_t bit_size_{};
    std::size_t position_{};
};

class BitWriter {
public:
    explicit BitWriter(std::size_t maximum_bytes = 64U * 1024U * 1024U);
    BitWriter(const BitWriter&) = default;
    BitWriter& operator=(const BitWriter&) = default;
    // A moved-from writer is empty and retains its capacity limit.
    BitWriter(BitWriter&& other) noexcept;
    BitWriter& operator=(BitWriter&& other) noexcept;
    [[nodiscard]] std::size_t bit_size() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    void write(std::uint64_t value, unsigned width);
    void write_signed(std::int64_t value, unsigned width);
    void write_bool(bool value);
    void align();
    void clear() noexcept;
    [[nodiscard]] std::vector<std::byte> take() noexcept;

private:
    std::size_t maximum_bytes_;
    std::size_t bit_size_{};
    std::vector<std::byte> bytes_;
};

[[nodiscard]] std::uint16_t float_to_half(float value) noexcept;
[[nodiscard]] float half_to_float(std::uint16_t value) noexcept;
[[nodiscard]] std::uint32_t quantize_unorm(double value, unsigned bits);
[[nodiscard]] double dequantize_unorm(std::uint32_t value, unsigned bits);
[[nodiscard]] std::int32_t quantize_snorm(double value, unsigned bits);
[[nodiscard]] double dequantize_snorm(std::int32_t value, unsigned bits);

// Packet RLE: high control bit means repeated byte; low seven bits encode count-1.
// Literal and repeat packets are bounded to 128 bytes. The output bound is always
// enforced before allocation; malformed/truncated input never publishes output.
[[nodiscard]] std::vector<std::byte> rle_encode(std::span<const std::byte> bytes,
                                              std::size_t maximum_output_bytes);
[[nodiscard]] std::vector<std::byte> rle_decode(std::span<const std::byte> bytes,
                                              std::size_t expected_output_bytes,
                                              std::size_t maximum_output_bytes);

} // namespace ge::core
