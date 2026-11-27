#include <ge/core/binary.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace ge::core {
namespace {

[[noreturn]] void fail(BinaryError error, std::size_t offset,
                       const char* message) {
    throw BinaryException(error, offset, message);
}

constexpr std::size_t archive_header_size = 40;
constexpr std::size_t archive_entry_size = 32;
constexpr std::size_t archive_checksum_size = 4;
constexpr std::size_t frame_header_size = 28;
constexpr std::uint16_t archive_version = 1;
constexpr std::uint16_t frame_version = 1;

std::uint8_t byte_value(std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

std::size_t bounded_size(std::uint64_t value, std::size_t bound,
                         std::size_t offset) {
    if (value > static_cast<std::uint64_t>(bound)) {
        fail(BinaryError::limit_exceeded, offset, "binary size exceeds its limit");
    }
    return static_cast<std::size_t>(value);
}

int hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

void require_normalized_bits(unsigned bits, unsigned minimum) {
    if (bits < minimum || bits > 32U) {
        throw std::invalid_argument("normalized integer width is unsupported");
    }
}

} // namespace

BinaryException::BinaryException(BinaryError error, std::size_t offset,
                                 std::string message)
    : std::runtime_error(std::move(message)), error_(error), offset_(offset) {}

BinaryError BinaryException::error() const noexcept {
    return error_;
}

std::size_t BinaryException::offset() const noexcept {
    return offset_;
}

bool checked_add(std::size_t a, std::size_t b, std::size_t& result) noexcept {
    if (b > std::numeric_limits<std::size_t>::max() - a) {
        return false;
    }
    result = a + b;
    return true;
}

bool checked_multiply(std::size_t a, std::size_t b,
                      std::size_t& result) noexcept {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        return false;
    }
    result = a * b;
    return true;
}

std::optional<std::size_t> aligned_size(std::size_t value,
                                       std::size_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return std::nullopt;
    }
    const auto remainder = value & (alignment - 1);
    if (remainder == 0) {
        return value;
    }
    std::size_t result{};
    if (!checked_add(value, alignment - remainder, result)) {
        return std::nullopt;
    }
    return result;
}

std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

bool valid_utf8(std::span<const std::byte> bytes) noexcept {
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto first = byte_value(bytes[cursor++]);
        if (first <= 0x7fU) {
            continue;
        }
        unsigned additional{};
        std::uint32_t codepoint{};
        std::uint32_t minimum{};
        if (first >= 0xc2U && first <= 0xdfU) {
            additional = 1;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            additional = 2;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            additional = 3;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (additional > bytes.size() - cursor) {
            return false;
        }
        for (unsigned index = 0; index < additional; ++index) {
            const auto continuation = byte_value(bytes[cursor++]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

bool valid_utf8(std::string_view text) noexcept {
    return valid_utf8(as_bytes(text));
}

ByteReader::ByteReader(std::span<const std::byte> bytes, ByteOrder order,
                       BinaryLimits limits)
    : bytes_(bytes), order_(order), limits_(limits) {
    if (bytes_.size() > limits_.max_total_bytes) {
        fail(BinaryError::limit_exceeded, 0, "reader input exceeds total byte limit");
    }
}

std::size_t ByteReader::position() const noexcept { return position_; }
std::size_t ByteReader::size() const noexcept { return bytes_.size(); }
std::size_t ByteReader::remaining() const noexcept { return bytes_.size() - position_; }
bool ByteReader::empty() const noexcept { return position_ == bytes_.size(); }
ByteOrder ByteReader::order() const noexcept { return order_; }
const BinaryLimits& ByteReader::limits() const noexcept { return limits_; }
std::span<const std::byte> ByteReader::source() const noexcept { return bytes_; }
std::span<const std::byte> ByteReader::unread() const noexcept {
    return bytes_.subspan(position_);
}

void ByteReader::require(std::size_t count) const {
    if (count > remaining()) {
        fail(BinaryError::truncated, position_, "binary input is truncated");
    }
}

void ByteReader::seek(std::size_t position) {
    if (position > bytes_.size()) {
        fail(BinaryError::truncated, position_, "reader seek lies outside input");
    }
    position_ = position;
}

void ByteReader::skip(std::size_t count) {
    require(count);
    position_ += count;
}

void ByteReader::align(std::size_t alignment, bool require_zero_padding) {
    const auto destination = aligned_size(position_, alignment);
    if (!destination) {
        fail(BinaryError::invalid_value, position_, "invalid reader alignment");
    }
    const auto padding = *destination - position_;
    if (require_zero_padding) {
        require_zero(padding);
    } else {
        skip(padding);
    }
}

void ByteReader::require_end() const {
    if (!empty()) {
        fail(BinaryError::trailing_data, position_, "unexpected trailing bytes");
    }
}

void ByteReader::expect(std::span<const std::byte> expected) {
    const auto actual = peek_bytes(expected.size());
    if (!std::equal(actual.begin(), actual.end(), expected.begin())) {
        fail(BinaryError::invalid_magic, position_, "binary signature mismatch");
    }
    position_ += expected.size();
}

void ByteReader::expect(std::string_view expected) {
    expect(as_bytes(expected));
}

void ByteReader::require_zero(std::size_t count) {
    const auto data = peek_bytes(count);
    if (std::any_of(data.begin(), data.end(),
                    [](std::byte value) { return value != std::byte{}; })) {
        fail(BinaryError::invalid_value, position_, "reserved bytes must be zero");
    }
    position_ += count;
}

std::uint64_t ByteReader::integer(std::size_t width) {
    require(width);
    std::uint64_t value{};
    for (std::size_t index = 0; index < width; ++index) {
        const auto shift_index = order_ == ByteOrder::little ? index : width - 1 - index;
        const auto octet = static_cast<std::uint64_t>(byte_value(bytes_[position_ + index]));
        value |= octet << (shift_index * 8U);
    }
    position_ += width;
    return value;
}

std::uint8_t ByteReader::read_u8() { return static_cast<std::uint8_t>(integer(1)); }
std::uint16_t ByteReader::read_u16() { return static_cast<std::uint16_t>(integer(2)); }
std::uint32_t ByteReader::read_u32() { return static_cast<std::uint32_t>(integer(4)); }
std::uint64_t ByteReader::read_u64() { return integer(8); }
std::int8_t ByteReader::read_i8() { return std::bit_cast<std::int8_t>(read_u8()); }
std::int16_t ByteReader::read_i16() { return std::bit_cast<std::int16_t>(read_u16()); }
std::int32_t ByteReader::read_i32() { return std::bit_cast<std::int32_t>(read_u32()); }
std::int64_t ByteReader::read_i64() { return std::bit_cast<std::int64_t>(read_u64()); }

float ByteReader::read_f32(bool require_finite) {
    auto candidate = *this;
    const auto value = std::bit_cast<float>(candidate.read_u32());
    if (require_finite && !std::isfinite(value)) {
        fail(BinaryError::invalid_value, position_, "nonfinite binary float");
    }
    position_ = candidate.position_;
    return value;
}

double ByteReader::read_f64(bool require_finite) {
    auto candidate = *this;
    const auto value = std::bit_cast<double>(candidate.read_u64());
    if (require_finite && !std::isfinite(value)) {
        fail(BinaryError::invalid_value, position_, "nonfinite binary double");
    }
    position_ = candidate.position_;
    return value;
}

bool ByteReader::read_bool() {
    require(1);
    const auto value = byte_value(bytes_[position_]);
    if (value > 1U) {
        fail(BinaryError::invalid_value, position_, "boolean must be zero or one");
    }
    ++position_;
    return value != 0;
}

std::uint64_t ByteReader::read_varuint() {
    auto candidate = *this;
    std::uint64_t value{};
    for (unsigned index = 0; index < 10U; ++index) {
        const auto octet = candidate.read_u8();
        const auto payload = static_cast<std::uint64_t>(octet & 0x7fU);
        if (index == 9U && octet > 1U) {
            fail(BinaryError::invalid_value, position_, "varuint overflows 64 bits");
        }
        value |= payload << (index * 7U);
        if ((octet & 0x80U) == 0) {
            if (index != 0 && payload == 0) {
                fail(BinaryError::noncanonical, position_, "redundant varuint encoding");
            }
            position_ = candidate.position_;
            return value;
        }
    }
    fail(BinaryError::invalid_value, position_, "unterminated varuint");
}

std::int64_t ByteReader::read_varsint() {
    const auto encoded = read_varuint();
    const auto magnitude = static_cast<std::int64_t>(encoded >> 1U);
    return (encoded & 1U) == 0 ? magnitude : -magnitude - 1;
}

std::span<const std::byte> ByteReader::peek_bytes(std::size_t count) const {
    require(count);
    return bytes_.subspan(position_, count);
}

std::span<const std::byte> ByteReader::read_bytes(std::size_t count) {
    const auto result = peek_bytes(count);
    position_ += count;
    return result;
}

std::string_view ByteReader::read_string_view() {
    auto candidate = *this;
    const auto count = static_cast<std::size_t>(candidate.read_u32());
    if (count > limits_.max_string_bytes) {
        fail(BinaryError::limit_exceeded, position_, "string exceeds byte limit");
    }
    const auto data = candidate.read_bytes(count);
    if (!valid_utf8(data)) {
        fail(BinaryError::invalid_utf8, position_, "string contains invalid UTF-8");
    }
    position_ = candidate.position_;
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

std::string ByteReader::read_string() {
    auto candidate = *this;
    std::string result(candidate.read_string_view());
    position_ = candidate.position_;
    return result;
}

std::string_view ByteReader::read_cstring(std::size_t maximum_bytes) {
    const auto bound = std::min(maximum_bytes, limits_.max_string_bytes);
    const auto search_count = bound < remaining() ? bound + 1 : remaining();
    const auto data = unread().first(search_count);
    const auto end = std::find(data.begin(), data.end(), std::byte{});
    if (end == data.end()) {
        const auto error = remaining() <= bound ? BinaryError::truncated
                                                : BinaryError::limit_exceeded;
        fail(error, position_, "unterminated or oversized C string");
    }
    const auto count = static_cast<std::size_t>(end - data.begin());
    const auto text = data.first(count);
    if (!valid_utf8(text)) {
        fail(BinaryError::invalid_utf8, position_, "C string contains invalid UTF-8");
    }
    position_ += count + 1;
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

ByteReader ByteReader::read_subreader(std::size_t count) {
    const auto data = peek_bytes(count);
    ByteReader result(data, order_, limits_);
    position_ += count;
    return result;
}

ByteReader ByteReader::read_sized_subreader() {
    auto candidate = *this;
    const auto count = static_cast<std::size_t>(candidate.read_u32());
    auto result = candidate.read_subreader(count);
    position_ = candidate.position_;
    return result;
}

std::vector<std::uint32_t> ByteReader::read_u32_array(std::size_t maximum_count) {
    auto candidate = *this;
    const auto count = static_cast<std::size_t>(candidate.read_u32());
    std::size_t required_bytes{};
    if (count > maximum_count || !checked_multiply(count, 4, required_bytes) ||
        required_bytes > limits_.max_total_bytes) {
        fail(BinaryError::limit_exceeded, position_, "integer array exceeds limit");
    }
    candidate.require(required_bytes);
    std::vector<std::uint32_t> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(candidate.read_u32());
    }
    position_ = candidate.position_;
    return result;
}

std::vector<float> ByteReader::read_f32_array(std::size_t maximum_count,
                                            bool require_finite) {
    auto candidate = *this;
    const auto count = static_cast<std::size_t>(candidate.read_u32());
    std::size_t required_bytes{};
    if (count > maximum_count || !checked_multiply(count, 4, required_bytes) ||
        required_bytes > limits_.max_total_bytes) {
        fail(BinaryError::limit_exceeded, position_, "float array exceeds limit");
    }
    candidate.require(required_bytes);
    std::vector<float> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(candidate.read_f32(require_finite));
    }
    position_ = candidate.position_;
    return result;
}

ByteWriter::ByteWriter(std::size_t maximum_bytes, ByteOrder order)
    : maximum_bytes_(maximum_bytes), order_(order) {}

std::size_t ByteWriter::size() const noexcept { return bytes_.size(); }
bool ByteWriter::empty() const noexcept { return bytes_.empty(); }
std::size_t ByteWriter::maximum_size() const noexcept { return maximum_bytes_; }
std::size_t ByteWriter::remaining_capacity() const noexcept {
    return maximum_bytes_ - bytes_.size();
}
ByteOrder ByteWriter::order() const noexcept { return order_; }
std::span<const std::byte> ByteWriter::bytes() const noexcept { return bytes_; }
const std::vector<std::byte>& ByteWriter::storage() const noexcept { return bytes_; }

std::vector<std::byte> ByteWriter::take() noexcept {
    std::vector<std::byte> result;
    result.swap(bytes_);
    return result;
}

void ByteWriter::clear() noexcept { bytes_.clear(); }

void ByteWriter::reserve(std::size_t capacity) {
    if (capacity > maximum_bytes_) {
        fail(BinaryError::limit_exceeded, size(), "writer reservation exceeds limit");
    }
    bytes_.reserve(capacity);
}

void ByteWriter::truncate(std::size_t count) {
    if (count > size()) {
        fail(BinaryError::invalid_value, size(), "writer cannot truncate beyond its end");
    }
    bytes_.resize(count);
}

void ByteWriter::prepare(std::size_t additional) {
    if (additional > remaining_capacity()) {
        fail(BinaryError::limit_exceeded, size(), "writer output exceeds byte limit");
    }
    const auto required = size() + additional;
    if (required <= bytes_.capacity()) {
        return;
    }
    const auto growth = bytes_.capacity() <= maximum_bytes_ / 2
        ? bytes_.capacity() * 2 : maximum_bytes_;
    bytes_.reserve(std::max(required, growth));
}

void ByteWriter::integer(std::uint64_t value, std::size_t width) {
    prepare(width);
    for (std::size_t index = 0; index < width; ++index) {
        const auto shift_index = order_ == ByteOrder::little ? index : width - 1 - index;
        bytes_.push_back(static_cast<std::byte>((value >> (shift_index * 8U)) & 0xffU));
    }
}

void ByteWriter::write_u8(std::uint8_t value) { integer(value, 1); }
void ByteWriter::write_u16(std::uint16_t value) { integer(value, 2); }
void ByteWriter::write_u32(std::uint32_t value) { integer(value, 4); }
void ByteWriter::write_u64(std::uint64_t value) { integer(value, 8); }
void ByteWriter::write_i8(std::int8_t value) { write_u8(std::bit_cast<std::uint8_t>(value)); }
void ByteWriter::write_i16(std::int16_t value) { write_u16(std::bit_cast<std::uint16_t>(value)); }
void ByteWriter::write_i32(std::int32_t value) { write_u32(std::bit_cast<std::uint32_t>(value)); }
void ByteWriter::write_i64(std::int64_t value) { write_u64(std::bit_cast<std::uint64_t>(value)); }

void ByteWriter::write_f32(float value, bool require_finite) {
    if (require_finite && !std::isfinite(value)) {
        fail(BinaryError::invalid_value, size(), "cannot encode nonfinite float");
    }
    write_u32(std::bit_cast<std::uint32_t>(value));
}

void ByteWriter::write_f64(double value, bool require_finite) {
    if (require_finite && !std::isfinite(value)) {
        fail(BinaryError::invalid_value, size(), "cannot encode nonfinite double");
    }
    write_u64(std::bit_cast<std::uint64_t>(value));
}

void ByteWriter::write_bool(bool value) {
    write_u8(static_cast<std::uint8_t>(value ? 1U : 0U));
}

void ByteWriter::write_varuint(std::uint64_t value) {
    std::array<std::byte, 10> encoded{};
    std::size_t count{};
    do {
        auto octet = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) {
            octet = static_cast<std::uint8_t>(octet | 0x80U);
        }
        encoded[count++] = static_cast<std::byte>(octet);
    } while (value != 0);
    write_bytes(std::span<const std::byte>(encoded).first(count));
}

void ByteWriter::write_varsint(std::int64_t value) {
    const auto magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1))
                                    : static_cast<std::uint64_t>(value);
    write_varuint((magnitude << 1U) | static_cast<std::uint64_t>(value < 0));
}

bool ByteWriter::overlaps(std::span<const std::byte> data) const noexcept {
    if (data.empty() || bytes_.empty()) {
        return false;
    }
    const std::less<const std::byte*> less;
    return less(data.data(), bytes_.data() + bytes_.size()) &&
           less(bytes_.data(), data.data() + data.size());
}

void ByteWriter::write_bytes(std::span<const std::byte> data) {
    if (data.size() > remaining_capacity()) {
        fail(BinaryError::limit_exceeded, size(), "byte sequence exceeds writer limit");
    }
    if (data.empty()) {
        return;
    }
    if (overlaps(data)) {
        const std::vector<std::byte> copy(data.begin(), data.end());
        prepare(copy.size());
        bytes_.insert(bytes_.end(), copy.begin(), copy.end());
        return;
    }
    prepare(data.size());
    bytes_.insert(bytes_.end(), data.begin(), data.end());
}

void ByteWriter::write_bytes(std::string_view data) {
    write_bytes(as_bytes(data));
}

void ByteWriter::write_zeroes(std::size_t count) {
    prepare(count);
    bytes_.resize(size() + count, std::byte{});
}

void ByteWriter::write_string(std::string_view text, std::size_t maximum_bytes) {
    std::size_t additional{};
    if (text.size() > maximum_bytes || text.size() > UINT32_MAX ||
        !checked_add(text.size(), 4, additional) || additional > remaining_capacity()) {
        fail(BinaryError::limit_exceeded, size(), "string exceeds writer limit");
    }
    if (!valid_utf8(text)) {
        fail(BinaryError::invalid_utf8, size(), "cannot encode invalid UTF-8 string");
    }
    std::string owned;
    if (overlaps(as_bytes(text))) {
        owned.assign(text);
        text = owned;
    }
    prepare(additional);
    write_u32(static_cast<std::uint32_t>(text.size()));
    write_bytes(text);
}

void ByteWriter::write_cstring(std::string_view text, std::size_t maximum_bytes) {
    std::size_t additional{};
    if (text.size() > maximum_bytes || !checked_add(text.size(), 1, additional) ||
        additional > remaining_capacity()) {
        fail(BinaryError::limit_exceeded, size(), "C string exceeds writer limit");
    }
    if (!valid_utf8(text) || text.find('\0') != std::string_view::npos) {
        fail(BinaryError::invalid_utf8, size(), "C string is invalid or contains NUL");
    }
    std::string owned;
    if (overlaps(as_bytes(text))) {
        owned.assign(text);
        text = owned;
    }
    prepare(additional);
    write_bytes(text);
    write_u8(0);
}

void ByteWriter::write_sized_bytes(std::span<const std::byte> data) {
    std::size_t additional{};
    if (data.size() > UINT32_MAX || !checked_add(data.size(), 4, additional) ||
        additional > remaining_capacity()) {
        fail(BinaryError::limit_exceeded, size(), "sized byte sequence exceeds limit");
    }
    std::vector<std::byte> owned;
    if (overlaps(data)) {
        owned.assign(data.begin(), data.end());
        data = owned;
    }
    prepare(additional);
    write_u32(static_cast<std::uint32_t>(data.size()));
    write_bytes(data);
}

void ByteWriter::write_u32_array(std::span<const std::uint32_t> values) {
    std::size_t byte_count{};
    std::size_t additional{};
    if (values.size() > UINT32_MAX || !checked_multiply(values.size(), 4, byte_count) ||
        !checked_add(byte_count, 4, additional)) {
        fail(BinaryError::limit_exceeded, size(), "integer array size overflows");
    }
    prepare(additional);
    write_u32(static_cast<std::uint32_t>(values.size()));
    for (const auto value : values) {
        write_u32(value);
    }
}

void ByteWriter::write_f32_array(std::span<const float> values, bool require_finite) {
    std::size_t byte_count{};
    std::size_t additional{};
    if (values.size() > UINT32_MAX || !checked_multiply(values.size(), 4, byte_count) ||
        !checked_add(byte_count, 4, additional)) {
        fail(BinaryError::limit_exceeded, size(), "float array size overflows");
    }
    if (require_finite && std::any_of(values.begin(), values.end(),
                                     [](float value) { return !std::isfinite(value); })) {
        fail(BinaryError::invalid_value, size(), "float array contains nonfinite value");
    }
    prepare(additional);
    write_u32(static_cast<std::uint32_t>(values.size()));
    for (const auto value : values) {
        write_f32(value, false);
    }
}

void ByteWriter::align(std::size_t alignment) {
    const auto target = aligned_size(size(), alignment);
    if (!target) {
        fail(BinaryError::invalid_value, size(), "invalid writer alignment");
    }
    write_zeroes(*target - size());
}

void ByteWriter::patch_integer(std::size_t offset, std::uint64_t value,
                               std::size_t width) {
    if (offset > size() || width > size() - offset) {
        fail(BinaryError::truncated, offset, "patch lies outside existing output");
    }
    for (std::size_t index = 0; index < width; ++index) {
        const auto shift_index = order_ == ByteOrder::little ? index : width - 1 - index;
        bytes_[offset + index] = static_cast<std::byte>((value >> (shift_index * 8U)) & 0xffU);
    }
}

void ByteWriter::patch_u16(std::size_t offset, std::uint16_t value) {
    patch_integer(offset, value, 2);
}

void ByteWriter::patch_u32(std::size_t offset, std::uint32_t value) {
    patch_integer(offset, value, 4);
}

void ByteWriter::patch_u64(std::size_t offset, std::uint64_t value) {
    patch_integer(offset, value, 8);
}

void ByteWriter::patch_bytes(std::size_t offset, std::span<const std::byte> data) {
    if (offset > size() || data.size() > size() - offset) {
        fail(BinaryError::truncated, offset, "byte patch lies outside existing output");
    }
    if (!data.empty()) {
        std::memmove(bytes_.data() + offset, data.data(), data.size());
    }
}

void Crc32::update(std::span<const std::byte> bytes) noexcept {
    for (const auto octet : bytes) {
        state_ ^= byte_value(octet);
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const auto mask = 0U - (state_ & 1U);
            state_ = (state_ >> 1U) ^ (0xedb88320U & mask);
        }
    }
}

void Crc32::update(std::string_view text) noexcept { update(as_bytes(text)); }
void Crc32::reset() noexcept { state_ = 0xffffffffU; }
std::uint32_t Crc32::value() const noexcept { return state_ ^ 0xffffffffU; }

void Fnv1a64::update(std::span<const std::byte> bytes) noexcept {
    for (const auto octet : bytes) {
        state_ ^= byte_value(octet);
        state_ *= 1099511628211ULL;
    }
}

void Fnv1a64::update(std::string_view text) noexcept { update(as_bytes(text)); }
void Fnv1a64::reset() noexcept { state_ = 14695981039346656037ULL; }
std::uint64_t Fnv1a64::value() const noexcept { return state_; }

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    Crc32 hash;
    hash.update(bytes);
    return hash.value();
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    Fnv1a64 hash;
    hash.update(bytes);
    return hash.value();
}

std::uint64_t fnv1a64(std::string_view text) noexcept { return fnv1a64(as_bytes(text)); }

std::string encode_hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::size_t count{};
    if (!checked_multiply(bytes.size(), 2, count)) {
        fail(BinaryError::limit_exceeded, 0, "hexadecimal output length overflows");
    }
    std::string result(count, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = byte_value(bytes[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 0x0fU];
    }
    return result;
}

std::vector<std::byte> decode_hex(std::string_view text, std::size_t maximum_bytes) {
    if ((text.size() & 1U) != 0) {
        fail(BinaryError::invalid_value, text.size(), "hexadecimal text has odd length");
    }
    if (text.size() / 2 > maximum_bytes) {
        fail(BinaryError::limit_exceeded, 0, "hexadecimal output exceeds limit");
    }
    std::vector<std::byte> result(text.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto high = hex_digit(text[index * 2]);
        const auto low = hex_digit(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            fail(BinaryError::invalid_value, index * 2, "invalid hexadecimal digit");
        }
        result[index] = static_cast<std::byte>((high << 4) | low);
    }
    return result;
}

const BinaryChunk* ChunkArchive::find(std::uint32_t id) const noexcept {
    const auto found = std::find_if(chunks.begin(), chunks.end(),
                                    [id](const BinaryChunk& chunk) { return chunk.id == id; });
    return found == chunks.end() ? nullptr : &*found;
}

BinaryChunk* ChunkArchive::find(std::uint32_t id) noexcept {
    const auto found = std::find_if(chunks.begin(), chunks.end(),
                                    [id](const BinaryChunk& chunk) { return chunk.id == id; });
    return found == chunks.end() ? nullptr : &*found;
}

std::vector<std::byte> encode_archive(const ChunkArchive& archive, BinaryLimits limits) {
    if (archive.schema == 0) {
        fail(BinaryError::invalid_value, 12, "archive schema must be nonzero");
    }
    if (archive.chunks.size() > limits.max_chunks || archive.chunks.size() > UINT32_MAX) {
        fail(BinaryError::limit_exceeded, 24, "archive chunk count exceeds limit");
    }
    std::size_t table_bytes{};
    std::size_t payload_offset{};
    if (!checked_multiply(archive.chunks.size(), archive_entry_size, table_bytes) ||
        !checked_add(archive_header_size, table_bytes, payload_offset)) {
        fail(BinaryError::limit_exceeded, 24, "archive table length overflows");
    }
    std::vector<const BinaryChunk*> ordered;
    ordered.reserve(archive.chunks.size());
    std::size_t total_size = payload_offset;
    for (const auto& chunk : archive.chunks) {
        if (chunk.id == 0 || chunk.version == 0) {
            fail(BinaryError::invalid_value, 0, "chunk ID and version must be nonzero");
        }
        if (chunk.data.size() > limits.max_chunk_bytes ||
            !checked_add(total_size, chunk.data.size(), total_size)) {
            fail(BinaryError::limit_exceeded, 0, "chunk length exceeds archive limit");
        }
        ordered.push_back(&chunk);
    }
    if (!checked_add(total_size, archive_checksum_size, total_size) ||
        total_size > limits.max_total_bytes) {
        fail(BinaryError::limit_exceeded, 0, "archive exceeds total byte limit");
    }
    std::sort(ordered.begin(), ordered.end(),
               [](const BinaryChunk* a, const BinaryChunk* b) { return a->id < b->id; });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (ordered[index - 1]->id == ordered[index]->id) {
            fail(BinaryError::duplicate_identifier, index, "duplicate chunk ID");
        }
    }
    ByteWriter writer(limits.max_total_bytes);
    writer.reserve(total_size);
    writer.write_bytes("GECHNK01");
    writer.write_u16(archive_version);
    writer.write_u16(static_cast<std::uint16_t>(archive_header_size));
    writer.write_u32(archive.schema);
    writer.write_u64(archive.revision);
    writer.write_u32(static_cast<std::uint32_t>(ordered.size()));
    writer.write_u32(0);
    writer.write_u64(static_cast<std::uint64_t>(total_size));
    std::size_t offset = payload_offset;
    for (const auto* chunk : ordered) {
        writer.write_u32(chunk->id);
        writer.write_u16(chunk->version);
        writer.write_u16(0);
        writer.write_u64(static_cast<std::uint64_t>(offset));
        writer.write_u64(static_cast<std::uint64_t>(chunk->data.size()));
        writer.write_u32(crc32(chunk->data));
        writer.write_u32(0);
        offset += chunk->data.size();
    }
    for (const auto* chunk : ordered) {
        writer.write_bytes(chunk->data);
    }
    writer.write_u32(crc32(writer.bytes()));
    return writer.take();
}

ChunkArchive decode_archive(std::span<const std::byte> bytes, BinaryLimits limits) {
    ByteReader reader(bytes, ByteOrder::little, limits);
    if (bytes.size() < archive_header_size + archive_checksum_size) {
        fail(BinaryError::truncated, 0, "archive header or checksum is truncated");
    }
    const auto body = bytes.first(bytes.size() - archive_checksum_size);
    ByteReader checksum_reader(bytes.last(archive_checksum_size), ByteOrder::little, limits);
    if (crc32(body) != checksum_reader.read_u32()) {
        fail(BinaryError::checksum_mismatch, body.size(), "archive checksum mismatch");
    }
    reader.expect("GECHNK01");
    if (reader.read_u16() != archive_version) {
        fail(BinaryError::unsupported_version, 8, "unsupported archive version");
    }
    if (reader.read_u16() != archive_header_size) {
        fail(BinaryError::invalid_value, 10, "unexpected archive header length");
    }
    ChunkArchive result;
    result.schema = reader.read_u32();
    result.revision = reader.read_u64();
    if (result.schema == 0) {
        fail(BinaryError::invalid_value, 12, "archive schema must be nonzero");
    }
    const auto count = static_cast<std::size_t>(reader.read_u32());
    reader.require_zero(4);
    if (reader.read_u64() != static_cast<std::uint64_t>(bytes.size())) {
        fail(BinaryError::invalid_value, 32, "archive declared length mismatch");
    }
    std::size_t table_bytes{};
    std::size_t expected_offset{};
    if (count > limits.max_chunks ||
        !checked_multiply(count, archive_entry_size, table_bytes) ||
        !checked_add(archive_header_size, table_bytes, expected_offset) ||
        expected_offset > body.size()) {
        fail(BinaryError::limit_exceeded, 24, "invalid archive chunk table size");
    }
    struct Entry {
        std::uint32_t id;
        std::uint16_t version;
        std::size_t offset;
        std::size_t size;
        std::uint32_t checksum;
    };
    std::vector<Entry> entries;
    entries.reserve(count);
    std::uint32_t previous_id{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto entry_position = reader.position();
        Entry entry{};
        entry.id = reader.read_u32();
        entry.version = reader.read_u16();
        reader.require_zero(2);
        entry.offset = bounded_size(reader.read_u64(), body.size(), entry_position + 8);
        entry.size = bounded_size(reader.read_u64(), limits.max_chunk_bytes, entry_position + 16);
        entry.checksum = reader.read_u32();
        reader.require_zero(4);
        if (entry.id == 0 || entry.version == 0) {
            fail(BinaryError::invalid_value, entry_position, "invalid chunk identity/version");
        }
        if (entry.id <= previous_id) {
            const auto error = entry.id == previous_id ? BinaryError::duplicate_identifier
                                                       : BinaryError::noncanonical;
            fail(error, entry_position, "chunk IDs must be unique and ascending");
        }
        if (entry.offset != expected_offset) {
            fail(BinaryError::overlapping_range, entry_position + 8,
                 "chunk ranges must be contiguous and follow table order");
        }
        if (entry.size > body.size() - entry.offset) {
            fail(BinaryError::truncated, entry.offset, "chunk extends beyond archive payload");
        }
        expected_offset += entry.size;
        previous_id = entry.id;
        entries.push_back(entry);
    }
    if (expected_offset != body.size()) {
        fail(BinaryError::trailing_data, expected_offset, "unused archive payload bytes");
    }
    for (const auto& entry : entries) {
        if (crc32(body.subspan(entry.offset, entry.size)) != entry.checksum) {
            fail(BinaryError::checksum_mismatch, entry.offset, "chunk checksum mismatch");
        }
    }
    result.chunks.reserve(count);
    for (const auto& entry : entries) {
        const auto data = body.subspan(entry.offset, entry.size);
        BinaryChunk chunk;
        chunk.id = entry.id;
        chunk.version = entry.version;
        chunk.data.assign(data.begin(), data.end());
        result.chunks.push_back(std::move(chunk));
    }
    return result;
}

std::vector<std::byte> encode_frame(const BinaryFrame& frame, BinaryLimits limits) {
    std::size_t total{};
    if (frame.type == 0) {
        fail(BinaryError::invalid_value, 12, "frame type must be nonzero");
    }
    if (frame.payload.size() > limits.max_frame_bytes || frame.payload.size() > UINT32_MAX ||
        !checked_add(frame_header_size, frame.payload.size(), total) ||
        total > limits.max_total_bytes) {
        fail(BinaryError::limit_exceeded, 8, "frame payload exceeds byte limit");
    }
    ByteWriter writer(limits.max_total_bytes);
    writer.reserve(total);
    writer.write_bytes("GEFR");
    writer.write_u16(frame_version);
    writer.write_u16(0);
    writer.write_u32(static_cast<std::uint32_t>(frame.payload.size()));
    writer.write_u32(frame.type);
    writer.write_u64(frame.sequence);
    Crc32 checksum;
    checksum.update(writer.bytes());
    checksum.update(frame.payload);
    writer.write_u32(checksum.value());
    writer.write_bytes(frame.payload);
    return writer.take();
}

FrameDecoder::FrameDecoder(BinaryLimits limits) : limits_(limits) {
    if (limits_.max_total_bytes < frame_header_size) {
        throw std::invalid_argument("frame decoder capacity must fit a header");
    }
    limits_.max_frame_bytes = std::min(limits_.max_frame_bytes,
                                       limits_.max_total_bytes - frame_header_size);
}

std::size_t FrameDecoder::buffered_bytes() const noexcept {
    return buffer_.size() - begin_;
}

std::size_t FrameDecoder::available_capacity() const noexcept {
    return limits_.max_total_bytes - buffered_bytes();
}

std::uint64_t FrameDecoder::frames_decoded() const noexcept { return frames_decoded_; }
bool FrameDecoder::failed() const noexcept { return failed_; }

void FrameDecoder::compact() {
    if (begin_ == 0) {
        return;
    }
    const auto remaining = buffered_bytes();
    if (remaining != 0) {
        std::memmove(buffer_.data(), buffer_.data() + begin_, remaining);
    }
    buffer_.resize(remaining);
    begin_ = 0;
}

std::size_t FrameDecoder::feed(std::span<const std::byte> bytes) {
    if (failed_) {
        fail(BinaryError::decoder_failed, begin_, "reset failed decoder before feeding");
    }
    const auto count = std::min(bytes.size(), available_capacity());
    if (count == 0) {
        return 0;
    }
    compact();
    const auto required = buffer_.size() + count;
    if (required > buffer_.capacity()) {
        const auto growth = buffer_.capacity() <= limits_.max_total_bytes / 2
            ? buffer_.capacity() * 2 : limits_.max_total_bytes;
        buffer_.reserve(std::max(required, growth));
    }
    const auto accepted = bytes.first(count);
    buffer_.insert(buffer_.end(), accepted.begin(), accepted.end());
    return count;
}

std::optional<std::size_t> FrameDecoder::frame_size() const {
    if (failed_) {
        fail(BinaryError::decoder_failed, begin_, "frame decoder is failed");
    }
    if (buffered_bytes() < frame_header_size) {
        return std::nullopt;
    }
    const auto data = std::span<const std::byte>(buffer_).subspan(begin_, frame_header_size);
    ByteReader reader(data, ByteOrder::little, limits_);
    reader.expect("GEFR");
    if (reader.read_u16() != frame_version) {
        fail(BinaryError::unsupported_version, begin_ + 4, "unsupported frame version");
    }
    reader.require_zero(2);
    const auto payload_size = static_cast<std::size_t>(reader.read_u32());
    if (payload_size > limits_.max_frame_bytes) {
        fail(BinaryError::limit_exceeded, begin_ + 8, "frame length exceeds decoder limit");
    }
    if (reader.read_u32() == 0) {
        fail(BinaryError::invalid_value, begin_ + 12, "frame type must be nonzero");
    }
    return frame_header_size + payload_size;
}

bool FrameDecoder::has_complete_frame() const {
    const auto count = frame_size();
    return count && *count <= buffered_bytes();
}

std::optional<BinaryFrame> FrameDecoder::next() {
    try {
        const auto count = frame_size();
        if (!count || *count > buffered_bytes()) {
            return std::nullopt;
        }
        const auto bytes = std::span<const std::byte>(buffer_).subspan(begin_, *count);
        ByteReader reader(bytes, ByteOrder::little, limits_);
        reader.skip(12);
        BinaryFrame result;
        result.type = reader.read_u32();
        result.sequence = reader.read_u64();
        const auto expected_crc = reader.read_u32();
        const auto payload = reader.unread();
        Crc32 checksum;
        checksum.update(bytes.first(frame_header_size - 4));
        checksum.update(payload);
        if (checksum.value() != expected_crc) {
            fail(BinaryError::checksum_mismatch, begin_ + 24, "frame checksum mismatch");
        }
        result.payload.assign(payload.begin(), payload.end());
        begin_ += *count;
        if (frames_decoded_ != UINT64_MAX) {
            ++frames_decoded_;
        }
        if (begin_ == buffer_.size()) {
            buffer_.clear();
            begin_ = 0;
        }
        return result;
    } catch (const BinaryException&) {
        failed_ = true;
        throw;
    }
}

void FrameDecoder::finish() const {
    if (failed_) {
        fail(BinaryError::decoder_failed, begin_, "cannot finish a failed frame stream");
    }
    if (buffered_bytes() != 0) {
        fail(BinaryError::truncated, begin_, "unconsumed or incomplete final frame");
    }
}

void FrameDecoder::reset() noexcept {
    buffer_.clear();
    begin_ = 0;
    frames_decoded_ = 0;
    failed_ = false;
}

BitReader::BitReader(std::span<const std::byte> bytes) : bytes_(bytes) {
    if (!checked_multiply(bytes.size(), 8, bit_size_)) {
        fail(BinaryError::limit_exceeded, 0, "bit input length overflows");
    }
}

std::size_t BitReader::position() const noexcept { return position_; }
std::size_t BitReader::remaining() const noexcept { return bit_size_ - position_; }

std::uint64_t BitReader::read(unsigned width) {
    if (width > 64U) {
        fail(BinaryError::invalid_value, position_ / 8, "bit read width exceeds 64");
    }
    if (width > remaining()) {
        fail(BinaryError::truncated, position_ / 8, "packed bit input is truncated");
    }
    std::uint64_t result{};
    for (unsigned index = 0; index < width; ++index) {
        const auto source_bit = position_ + index;
        const auto octet = byte_value(bytes_[source_bit / 8]);
        const auto bit = static_cast<std::uint64_t>((octet >> (source_bit % 8)) & 1U);
        result |= bit << index;
    }
    position_ += width;
    return result;
}

std::int64_t BitReader::read_signed(unsigned width) {
    if (width == 0 || width > 64U) {
        fail(BinaryError::invalid_value, position_ / 8, "signed bit width must be 1..64");
    }
    auto bits = read(width);
    if (width < 64U && (bits & (std::uint64_t{1} << (width - 1U))) != 0) {
        bits |= UINT64_MAX << width;
    }
    return std::bit_cast<std::int64_t>(bits);
}

bool BitReader::read_bool() { return read(1) != 0; }

void BitReader::skip(std::size_t bits) {
    if (bits > remaining()) {
        fail(BinaryError::truncated, position_ / 8, "packed bit skip exceeds input");
    }
    position_ += bits;
}

void BitReader::align(bool require_zero_padding) {
    const auto padding = static_cast<unsigned>((8 - position_ % 8) % 8);
    auto candidate = *this;
    const auto value = candidate.read(padding);
    if (require_zero_padding && value != 0) {
        fail(BinaryError::noncanonical, position_ / 8, "bit alignment padding is nonzero");
    }
    position_ = candidate.position_;
}

void BitReader::require_end(bool allow_zero_padding) {
    if (remaining() == 0) {
        return;
    }
    if (!allow_zero_padding || remaining() >= 8) {
        fail(BinaryError::trailing_data, position_ / 8, "trailing packed bit data");
    }
    auto candidate = *this;
    if (candidate.read(static_cast<unsigned>(remaining())) != 0) {
        fail(BinaryError::noncanonical, position_ / 8, "final bit padding is nonzero");
    }
    position_ = candidate.position_;
}

BitWriter::BitWriter(std::size_t maximum_bytes) : maximum_bytes_(maximum_bytes) {
    if (maximum_bytes_ > std::numeric_limits<std::size_t>::max() / 8) {
        throw std::invalid_argument("bit writer capacity overflows bit count");
    }
}

std::size_t BitWriter::bit_size() const noexcept { return bit_size_; }
std::span<const std::byte> BitWriter::bytes() const noexcept { return bytes_; }

void BitWriter::write(std::uint64_t value, unsigned width) {
    if (width > 64U || (width < 64U && (value >> width) != 0)) {
        fail(BinaryError::invalid_value, bit_size_ / 8, "value does not fit bit width");
    }
    std::size_t target_bits{};
    if (!checked_add(bit_size_, width, target_bits)) {
        fail(BinaryError::limit_exceeded, bit_size_ / 8, "bit output length overflows");
    }
    const auto target_bytes = target_bits / 8 + static_cast<std::size_t>(target_bits % 8 != 0);
    if (target_bytes > maximum_bytes_) {
        fail(BinaryError::limit_exceeded, bit_size_ / 8, "bit output exceeds capacity");
    }
    bytes_.resize(target_bytes, std::byte{});
    for (unsigned index = 0; index < width; ++index) {
        const auto destination_bit = bit_size_ + index;
        const auto bit = static_cast<std::uint8_t>((value >> index) & 1U);
        bytes_[destination_bit / 8] |= static_cast<std::byte>(bit << (destination_bit % 8));
    }
    bit_size_ = target_bits;
}

void BitWriter::write_signed(std::int64_t value, unsigned width) {
    if (width == 0 || width > 64U) {
        fail(BinaryError::invalid_value, bit_size_ / 8, "signed bit width must be 1..64");
    }
    if (width < 64U) {
        const auto limit = std::int64_t{1} << (width - 1U);
        if (value < -limit || value >= limit) {
            fail(BinaryError::invalid_value, bit_size_ / 8, "signed value exceeds bit width");
        }
    }
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto mask = width == 64U ? UINT64_MAX : (std::uint64_t{1} << width) - 1;
    write(bits & mask, width);
}

void BitWriter::write_bool(bool value) { write(value ? 1U : 0U, 1); }

void BitWriter::align() {
    write(0, static_cast<unsigned>((8 - bit_size_ % 8) % 8));
}

void BitWriter::clear() noexcept {
    bytes_.clear();
    bit_size_ = 0;
}

std::vector<std::byte> BitWriter::take() noexcept {
    std::vector<std::byte> result;
    result.swap(bytes_);
    bit_size_ = 0;
    return result;
}

std::uint16_t float_to_half(float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<unsigned>((bits >> 23U) & 0xffU);
    const auto mantissa = bits & 0x7fffffU;
    if (exponent == 0xffU) {
        const auto payload = mantissa == 0 ? 0U : ((mantissa >> 13U) | 0x0200U);
        return static_cast<std::uint16_t>(sign | 0x7c00U | payload);
    }
    const int half_exponent = static_cast<int>(exponent) - 127 + 15;
    if (half_exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7c00U);
    }
    if (half_exponent <= 0) {
        if (half_exponent < -10) {
            return sign;
        }
        const auto significand = mantissa | 0x800000U;
        const auto shift = static_cast<unsigned>(14 - half_exponent);
        auto rounded = significand >> shift;
        const auto discarded = significand & ((1U << shift) - 1U);
        const auto halfway = 1U << (shift - 1U);
        if (discarded > halfway || (discarded == halfway && (rounded & 1U) != 0)) {
            ++rounded;
        }
        return static_cast<std::uint16_t>(sign | rounded);
    }
    auto result = (static_cast<std::uint32_t>(half_exponent) << 10U) | (mantissa >> 13U);
    const auto discarded = mantissa & 0x1fffU;
    if (discarded > 0x1000U || (discarded == 0x1000U && (result & 1U) != 0)) {
        ++result;
    }
    return static_cast<std::uint16_t>(sign | result);
}

float half_to_float(std::uint16_t value) noexcept {
    const auto sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
    const auto exponent = static_cast<std::uint32_t>((value >> 10U) & 0x1fU);
    auto mantissa = static_cast<std::uint32_t>(value & 0x03ffU);
    std::uint32_t bits{};
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int normalized_exponent = -14;
            while ((mantissa & 0x0400U) == 0) {
                mantissa <<= 1U;
                --normalized_exponent;
            }
            mantissa &= 0x03ffU;
            bits = sign | (static_cast<std::uint32_t>(normalized_exponent + 127) << 23U)
                        | (mantissa << 13U);
        }
    } else if (exponent == 31U) {
        bits = sign | 0x7f800000U | (mantissa << 13U);
        if (mantissa != 0) {
            bits |= 0x00400000U;
        }
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    return std::bit_cast<float>(bits);
}

std::uint32_t quantize_unorm(double value, unsigned bits) {
    require_normalized_bits(bits, 1);
    if (!std::isfinite(value)) {
        throw std::invalid_argument("cannot quantize nonfinite normalized value");
    }
    const auto maximum = bits == 32U ? UINT32_MAX : (std::uint32_t{1} << bits) - 1;
    const auto scaled = std::clamp(value, 0.0, 1.0) * static_cast<double>(maximum);
    return static_cast<std::uint32_t>(std::floor(scaled + 0.5));
}

double dequantize_unorm(std::uint32_t value, unsigned bits) {
    require_normalized_bits(bits, 1);
    const auto maximum = bits == 32U ? UINT32_MAX : (std::uint32_t{1} << bits) - 1;
    if (value > maximum) {
        throw std::invalid_argument("normalized integer exceeds declared width");
    }
    return static_cast<double>(value) / static_cast<double>(maximum);
}

std::int32_t quantize_snorm(double value, unsigned bits) {
    require_normalized_bits(bits, 2);
    if (!std::isfinite(value)) {
        throw std::invalid_argument("cannot quantize nonfinite signed normalized value");
    }
    const auto maximum = static_cast<std::int32_t>((std::uint32_t{1} << (bits - 1U)) - 1U);
    const auto scaled = std::clamp(value, -1.0, 1.0) * static_cast<double>(maximum);
    return static_cast<std::int32_t>(std::round(scaled));
}

double dequantize_snorm(std::int32_t value, unsigned bits) {
    require_normalized_bits(bits, 2);
    const auto maximum = static_cast<std::int32_t>((std::uint32_t{1} << (bits - 1U)) - 1U);
    const auto minimum = -static_cast<std::int64_t>(maximum) - 1;
    if (static_cast<std::int64_t>(value) < minimum || value > maximum) {
        throw std::invalid_argument("signed normalized integer exceeds width");
    }
    return std::max(-1.0, static_cast<double>(value) / static_cast<double>(maximum));
}

std::vector<std::byte> rle_encode(std::span<const std::byte> bytes,
                                 std::size_t maximum_output_bytes) {
    ByteWriter writer(maximum_output_bytes);
    std::size_t cursor{};
    while (cursor < bytes.size()) {
        std::size_t repeat = 1;
        while (repeat < 128 && repeat < bytes.size() - cursor &&
               bytes[cursor + repeat] == bytes[cursor]) {
            ++repeat;
        }
        if (repeat >= 3) {
            writer.write_u8(static_cast<std::uint8_t>(0x80U | (repeat - 1)));
            writer.write_u8(byte_value(bytes[cursor]));
            cursor += repeat;
            continue;
        }
        const auto start = cursor;
        cursor += repeat;
        while (cursor < bytes.size() && cursor - start < 128) {
            if (bytes.size() - cursor >= 3 && bytes[cursor] == bytes[cursor + 1] &&
                bytes[cursor] == bytes[cursor + 2]) {
                break;
            }
            ++cursor;
        }
        const auto count = cursor - start;
        writer.write_u8(static_cast<std::uint8_t>(count - 1));
        writer.write_bytes(bytes.subspan(start, count));
    }
    return writer.take();
}

std::vector<std::byte> rle_decode(std::span<const std::byte> bytes,
                                 std::size_t expected_output_bytes,
                                 std::size_t maximum_output_bytes) {
    if (expected_output_bytes > maximum_output_bytes) {
        fail(BinaryError::limit_exceeded, 0, "RLE output length exceeds limit");
    }
    ByteWriter writer(expected_output_bytes);
    writer.reserve(expected_output_bytes);
    std::size_t cursor{};
    while (cursor < bytes.size()) {
        const auto control = byte_value(bytes[cursor++]);
        const auto count = static_cast<std::size_t>(control & 0x7fU) + 1;
        if (count > writer.remaining_capacity()) {
            fail(BinaryError::limit_exceeded, cursor - 1, "RLE packet exceeds decoded length");
        }
        if ((control & 0x80U) != 0) {
            if (cursor == bytes.size()) {
                fail(BinaryError::truncated, cursor, "RLE repeat byte is missing");
            }
            const auto value = byte_value(bytes[cursor++]);
            for (std::size_t index = 0; index < count; ++index) {
                writer.write_u8(value);
            }
        } else {
            if (count > bytes.size() - cursor) {
                fail(BinaryError::truncated, cursor, "RLE literal packet is truncated");
            }
            writer.write_bytes(bytes.subspan(cursor, count));
            cursor += count;
        }
    }
    if (writer.size() != expected_output_bytes) {
        fail(BinaryError::truncated, cursor, "RLE decoded length does not match expectation");
    }
    return writer.take();
}

} // namespace ge::core
