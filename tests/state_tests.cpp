#include <ge/world/state.hpp>

#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ge::PersistentId;
using ge::world::PersistentState;
using ge::world::StateEntry;
using ge::world::decode_state;
using ge::world::encode_state;

constexpr std::array<StateEntry, 3> initial{{{{3U}, 30}, {{1U}, 10}, {{2U}, 20}}};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64U; ++attempt) {
            path_ = root / ("ge-state-tests-" + std::to_string(stamp) + "-" +
                            std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("cannot create test directory: " + error.message());
            }
        }
        throw std::runtime_error("cannot reserve test directory");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) {
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8U * i));
    }
}

void put_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i) {
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8U * i));
    }
}

void repair_checksum(std::vector<std::uint8_t>& bytes) {
    CHECK(bytes.size() >= 4U);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < bytes.size() - 4U; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    put_u32(bytes, bytes.size() - 4U, crc ^ 0xFFFFFFFFU);
}

void check_values(const PersistentState& state, std::span<const StateEntry> expected,
                  std::uint64_t sequence) {
    CHECK(state.sequence() == sequence);
    CHECK(state.size() == expected.size());
    for (const auto& entry : expected) {
        CHECK(state.get(entry.id) == entry.value);
    }
}

[[nodiscard]] std::vector<std::uint8_t> file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input.is_open());
    const auto length = static_cast<std::streamoff>(input.tellg());
    CHECK(length >= 0);
    CHECK(length <= static_cast<std::streamoff>(ge::world::max_state_file_bytes + 1U));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(input.good());
    return bytes;
}

void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.is_open());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    CHECK(output.good());
}

void check_no_temporary_files(const std::filesystem::path& parent) {
    const auto prefix = std::filesystem::path(".ge-state-").native();
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        CHECK(!entry.path().filename().native().starts_with(prefix));
    }
}

void initial_values_and_identity() {
    PersistentState state(7U, initial);
    CHECK(state.baseline_id() == 7U);
    check_values(state, initial, 0U);
    CHECK(!state.set(PersistentId{}, 55));
    CHECK(!state.set(PersistentId{99U}, 55));
    CHECK(!state.get(PersistentId{}).has_value());
    CHECK(!state.get(PersistentId{99U}).has_value());
    CHECK(state.size() == initial.size());
    CHECK(state.set(PersistentId{2U}, std::numeric_limits<std::int32_t>::min()));
    CHECK(state.get(PersistentId{2U}) == std::numeric_limits<std::int32_t>::min());
    CHECK(state.set(PersistentId{2U}, std::numeric_limits<std::int32_t>::max()));
    CHECK(state.get(PersistentId{2U}) == std::numeric_limits<std::int32_t>::max());
    CHECK(state.sequence() == 0U);

    CHECK_THROWS_AS(PersistentState(0U, initial), std::invalid_argument);
    const std::array<StateEntry, 2> duplicate{{{{1U}, 1}, {{1U}, 2}}};
    const std::array<StateEntry, 1> zero{{{{0U}, 1}}};
    CHECK_THROWS_AS(PersistentState(7U, duplicate), std::invalid_argument);
    CHECK_THROWS_AS(PersistentState(7U, zero), std::invalid_argument);
}

void owned_snapshot_and_golden_format() {
    // Generated independently using Python struct.pack and zlib.crc32, including
    // signed extrema and non-symmetric multi-byte baseline/sequence fields.
    constexpr std::array<std::uint8_t, 68> golden{
        0x47U, 0x45U, 0x53U, 0x54U, 0x41U, 0x54U, 0x45U, 0x31U, 0x01U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x80U, 0x70U, 0x60U, 0x50U, 0x40U, 0x30U, 0x20U, 0x10U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U, 0x02U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x80U, 0x09U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0xFFU, 0xFFU, 0xFFU, 0x7FU, 0x57U, 0x85U, 0x47U, 0x51U};
    const auto snapshot = [] {
        const std::array<StateEntry, 2> values{{
            {{9U}, std::numeric_limits<std::int32_t>::max()},
            {{2U}, std::numeric_limits<std::int32_t>::min()}}};
        PersistentState state(0x1020304050607080ULL, values);
        auto result = state.capture(0x0102030405060708ULL);
        CHECK(state.set(PersistentId{2U}, 0));
        CHECK(result.entries()[0].value == std::numeric_limits<std::int32_t>::min());
        return result;
    }();
    CHECK(snapshot.version() == ge::world::state_schema_version);
    CHECK(snapshot.baseline_id() == 0x1020304050607080ULL);
    CHECK(snapshot.sequence() == 0x0102030405060708ULL);
    CHECK(snapshot.entries().size() == 2U);
    CHECK(snapshot.entries()[0].id == PersistentId{2U});
    const auto encoded = encode_state(snapshot);
    CHECK(std::equal(encoded.begin(), encoded.end(), golden.begin(), golden.end()));
    const auto decoded = decode_state(golden);
    CHECK(decoded.baseline_id() == snapshot.baseline_id());
    CHECK(decoded.sequence() == snapshot.sequence());
    CHECK(std::equal(decoded.entries().begin(), decoded.entries().end(),
                     snapshot.entries().begin(), snapshot.entries().end()));
}

void malformed_restore_preserves_live_state() {
    PersistentState source(7U, initial);
    CHECK(source.set(PersistentId{1U}, -7));
    CHECK(source.set(PersistentId{2U}, -8));
    const auto valid = encode_state(source.capture(10U));
    PersistentState target(7U, initial);
    (void)target.capture(9U);

    const auto rejected = [&](std::span<const std::uint8_t> bytes) {
        CHECK_THROWS_AS(target.restore(bytes), std::runtime_error);
        check_values(target, initial, 9U);
    };
    for (std::size_t length = 0; length < valid.size(); ++length) {
        rejected(std::span(valid).first(length));
    }
    const auto changed_u32 = [&](std::size_t offset, std::uint32_t value) {
        auto bytes = valid;
        put_u32(bytes, offset, value);
        repair_checksum(bytes);
        rejected(bytes);
    };
    const auto changed_u64 = [&](std::size_t offset, std::uint64_t value) {
        auto bytes = valid;
        put_u64(bytes, offset, value);
        repair_checksum(bytes);
        rejected(bytes);
    };
    changed_u32(0U, 0U);                 // Magic, with valid checksum.
    changed_u32(8U, 2U);                 // Unsupported schema.
    changed_u32(12U, 1U);                // Reserved fields.
    changed_u32(36U, 1U);
    changed_u64(16U, 0U);                // Invalid baseline.
    changed_u64(16U, 8U);                // Different valid baseline.
    changed_u64(24U, 0U);                // Invalid sequence.
    changed_u64(24U, 8U);                // Older valid sequence.
    changed_u32(32U, 0U);                // Count/framing mismatches.
    changed_u32(32U, 2U);
    changed_u32(32U, 4U);
    changed_u32(32U, 65537U);             // Oversized count; framing also mismatches.
    changed_u32(32U, 0xFFFFFFFFU);        // Overflow-shaped count.
    changed_u64(40U, 0U);                // Zero ID.
    changed_u64(52U, 1U);                // Duplicate ID.
    changed_u64(40U, 3U);                // Noncanonical ordering.
    changed_u64(64U, 4U);                // Unknown ID replacing a known one.

    auto bytes = valid;
    bytes.back() ^= 1U;
    rejected(bytes);                      // Corrupt checksum.
    bytes = valid;
    bytes[48U] ^= 1U;
    rejected(bytes);                      // Corrupt scalar payload.
    bytes = valid;
    bytes.insert(bytes.end() - 4, 0U);
    repair_checksum(bytes);
    rejected(bytes);                      // Trailing data with valid checksum.
    bytes = valid;
    bytes.erase(bytes.begin() + 64, bytes.begin() + 76);
    put_u32(bytes, 32U, 2U);
    repair_checksum(bytes);
    rejected(bytes);                      // Well-formed save missing an ID.
    const std::vector<std::uint8_t> oversized(ge::world::max_state_file_bytes + 1U);
    rejected(oversized);

    target.restore(valid);
    CHECK(target.sequence() == 10U);
    CHECK(target.get(PersistentId{1U}) == -7);
    CHECK(target.get(PersistentId{2U}) == -8);
    CHECK(target.get(PersistentId{3U}) == 30);
}

void sequence_order_and_exhaustion() {
    PersistentState state(7U, initial);
    CHECK_THROWS_AS(state.capture(0U), std::runtime_error);
    check_values(state, initial, 0U);
    const auto first = state.capture(1U);
    CHECK_THROWS_AS(state.capture(1U), std::runtime_error);
    CHECK_THROWS_AS(state.capture(0U), std::runtime_error);
    CHECK(state.set(PersistentId{1U}, 40));
    const auto second = state.capture(2U);
    CHECK_THROWS_AS(state.restore(first), std::runtime_error);
    CHECK(state.sequence() == 2U);
    CHECK(state.get(PersistentId{1U}) == 40);
    CHECK(state.set(PersistentId{1U}, 50));
    state.restore(second);                // Equal-sequence replay is deliberate.
    CHECK(state.get(PersistentId{1U}) == 40);
    const auto last = state.capture(std::numeric_limits<std::uint64_t>::max());
    CHECK_THROWS_AS(state.capture(std::numeric_limits<std::uint64_t>::max()),
                    std::runtime_error);
    CHECK_THROWS_AS(state.capture(1U), std::runtime_error);
    CHECK_THROWS_AS(state.restore(second), std::runtime_error);
    CHECK(state.sequence() == std::numeric_limits<std::uint64_t>::max());
    PersistentState restarted(7U, initial);
    restarted.restore(last);
    CHECK(restarted.sequence() == std::numeric_limits<std::uint64_t>::max());
    CHECK(restarted.get(PersistentId{1U}) == 40);
}

void bounds_and_empty_state() {
    PersistentState empty(7U, {});
    const auto empty_bytes = encode_state(empty.capture(1U));
    CHECK(empty_bytes.size() == ge::world::state_header_bytes + ge::world::state_checksum_bytes);
    PersistentState empty_restart(7U, {});
    empty_restart.restore(empty_bytes);
    CHECK(empty_restart.size() == 0U);
    CHECK(empty_restart.sequence() == 1U);

    std::vector<StateEntry> values;
    values.reserve(ge::world::max_state_entries);
    for (std::size_t i = 0; i < ge::world::max_state_entries; ++i) {
        values.push_back({PersistentId{static_cast<std::uint64_t>(i) + 1U},
                          static_cast<std::int32_t>(i)});
    }
    values.back().id = PersistentId{std::numeric_limits<std::uint64_t>::max()};
    PersistentState state(7U, values);
    const auto bytes = encode_state(state.capture(1U));
    CHECK(bytes.size() <= ge::world::max_state_file_bytes);
    PersistentState restarted(7U, values);
    restarted.restore(bytes);
    CHECK(restarted.size() == ge::world::max_state_entries);
    CHECK(restarted.get(values.back().id) == values.back().value);

    // Keep all IDs valid and unique: the count cap must be the only reason
    // construction fails, rather than zero/duplicate-ID validation.
    const auto extra_id = static_cast<std::uint64_t>(ge::world::max_state_entries);
    values.insert(values.end() - 1, StateEntry{PersistentId{extra_id}, 0});
    CHECK_THROWS_AS(PersistentState(7U, values), std::invalid_argument);

    // Insert the same ID before UINT64_MAX in the accepted maximum-size wire
    // snapshot. Repair both framing and CRC so they cannot mask a missing cap.
    auto too_many = bytes;
    const auto last_record_offset = too_many.size() - ge::world::state_checksum_bytes -
                                    ge::world::state_entry_bytes;
    too_many.insert(too_many.begin() + static_cast<std::ptrdiff_t>(last_record_offset),
                    ge::world::state_entry_bytes, 0U);
    put_u64(too_many, last_record_offset, extra_id);
    put_u32(too_many, 32U, static_cast<std::uint32_t>(values.size()));
    repair_checksum(too_many);
    CHECK(too_many.size() < ge::world::max_state_file_bytes);
    // Check decoding directly: restoring to a smaller known-ID set would also
    // reject this input even if the decoder stopped enforcing its count cap.
    CHECK_THROWS_AS(decode_state(too_many), std::runtime_error);
}

void file_restart_replacement_and_failure_preservation() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / std::filesystem::path(u8"\u03b4-state.bin");
    PersistentState state(7U, initial);
    CHECK(state.set(PersistentId{2U}, -123));
    state.save(path, 1U);
    CHECK(state.sequence() == 1U);
    const auto first_bytes = file_bytes(path);
    CHECK(decode_state(first_bytes).sequence() == 1U);
    PersistentState restarted(7U, initial);
    restarted.restore(path);
    CHECK(restarted.sequence() == 1U);
    CHECK(restarted.get(PersistentId{2U}) == -123);
    CHECK(state.set(PersistentId{2U}, 999));
    state.restore(path);                 // Restore saved values after local edits.
    CHECK(state.get(PersistentId{2U}) == -123);
    CHECK(state.set(PersistentId{3U}, 456));
    state.save(path, 2U);                // Replace an existing save.
    const auto newer_bytes = file_bytes(path);
    CHECK(newer_bytes != first_bytes);
    CHECK(state.sequence() == 2U);
    restarted.restore(path);
    CHECK(restarted.sequence() == 2U);
    CHECK(restarted.get(PersistentId{3U}) == 456);

    CHECK_THROWS_AS(state.save(path, 2U), std::runtime_error);
    CHECK_THROWS_AS(state.save(path, 1U), std::runtime_error);
    CHECK(file_bytes(path) == newer_bytes);
    CHECK(state.sequence() == 2U);
    PersistentState stale_writer(7U, initial);
    CHECK_THROWS_AS(stale_writer.save(path, 1U), std::runtime_error);
    CHECK_THROWS_AS(stale_writer.save(path, 2U), std::runtime_error);
    check_values(stale_writer, initial, 0U);
    CHECK(file_bytes(path) == newer_bytes);

    PersistentState wrong_baseline(8U, initial);
    CHECK_THROWS_AS(wrong_baseline.restore(path), std::runtime_error);
    CHECK_THROWS_AS(wrong_baseline.save(path, 3U), std::runtime_error);
    check_values(wrong_baseline, initial, 0U);
    CHECK(file_bytes(path) == newer_bytes);
    const std::array<StateEntry, 3> other_ids{{{{1U}, 10}, {{2U}, 20}, {{4U}, 40}}};
    PersistentState wrong_ids(7U, other_ids);
    CHECK_THROWS_AS(wrong_ids.restore(path), std::runtime_error);
    CHECK_THROWS_AS(wrong_ids.save(path, 3U), std::runtime_error);
    check_values(wrong_ids, other_ids, 0U);
    CHECK(file_bytes(path) == newer_bytes);

    const auto bad_path = temporary.path() / "corrupt.bin";
    auto corrupt_bytes = newer_bytes;
    corrupt_bytes.back() ^= 1U;
    write_file(bad_path, corrupt_bytes);
    CHECK_THROWS_AS(stale_writer.restore(bad_path), std::runtime_error);
    CHECK_THROWS_AS(stale_writer.save(bad_path, 3U), std::runtime_error);
    check_values(stale_writer, initial, 0U);
    CHECK(file_bytes(bad_path) == corrupt_bytes);

    const auto absent_parent = temporary.path() / "absent" / "save.bin";
    CHECK_THROWS_AS(state.save(absent_parent, 3U), std::runtime_error);
    CHECK(!std::filesystem::exists(absent_parent));
    CHECK(state.sequence() == 2U);
    CHECK_THROWS_AS(state.save(temporary.path(), 3U), std::runtime_error);
    CHECK_THROWS_AS(stale_writer.restore(temporary.path() / "missing.bin"), std::runtime_error);
    CHECK(file_bytes(path) == newer_bytes);
    check_values(stale_writer, initial, 0U);

    const auto oversize_path = temporary.path() / "oversize.bin";
    const std::vector<std::uint8_t> oversized(ge::world::max_state_file_bytes + 1U);
    write_file(oversize_path, oversized);
    CHECK_THROWS_AS(stale_writer.restore(oversize_path), std::runtime_error);
    check_values(stale_writer, initial, 0U);
    CHECK_THROWS_AS(stale_writer.save(oversize_path, 3U), std::runtime_error);
    CHECK(file_bytes(oversize_path) == oversized);
    check_no_temporary_files(temporary.path());
}

} // namespace

int main() {
    return ge::test::run("state", {
        {"initial values and stable identity", initial_values_and_identity},
        {"owned snapshot and golden wire format", owned_snapshot_and_golden_format},
        {"malformed restore preserves live state", malformed_restore_preserves_live_state},
        {"sequence order and exhaustion", sequence_order_and_exhaustion},
        {"count boundary and empty state", bounds_and_empty_state},
        {"restart, replacement, and failure preservation",
         file_restart_replacement_and_failure_preservation},
    });
}
