#include <ge/content/package.hpp>

#include <algorithm>
#include <bit>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>

namespace ge {
namespace {

[[noreturn]] void invalid(const char* reason) {
    throw std::runtime_error(std::string("invalid world package: ") + reason);
}

// Decode wire fields explicitly: native structure layout, alignment, and host
// byte order never become part of the file format.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::span<const std::uint8_t> take(std::size_t count) {
        if (count > bytes_.size() - position_) {
            invalid("truncated record");
        }
        const auto result = bytes_.subspan(position_, count);
        position_ += count;
        return result;
    }

    [[nodiscard]] std::uint32_t u32() {
        const auto bytes = take(4);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(bytes[i]) << (8U * i);
        }
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        const auto bytes = take(8);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
        }
        return value;
    }

    [[nodiscard]] std::int32_t i32() {
        return std::bit_cast<std::int32_t>(u32());
    }

    [[nodiscard]] bool finished() const noexcept {
        return position_ == bytes_.size();
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    // Reflected IEEE CRC32, with the same initialization/finalization as
    // Python's zlib.crc32(data).
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t low_bit_mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & low_bit_mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        invalid("cannot open file");
    }

    const auto end = file.tellg();
    if (end == std::ifstream::pos_type(-1)) {
        invalid("cannot determine file size");
    }
    const auto length = static_cast<std::streamoff>(end);
    if (length < static_cast<std::streamoff>(package_header_bytes + package_checksum_bytes)) {
        invalid("truncated header or checksum");
    }
    if (length > static_cast<std::streamoff>(max_package_bytes)) {
        invalid("file exceeds 4 MiB limit");
    }

    // The allocation is bounded before converting the file length to size_t.
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0, std::ios::beg);
    if (!file) {
        invalid("cannot seek file");
    }
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size())) {
        invalid("truncated file or read failure");
    }

    // Also reject growth after tellg(), rather than silently ignoring new data.
    char extra = 0;
    if (file.get(extra)) {
        invalid("trailing data or file changed during read");
    }
    if (!file.eof() || file.bad()) {
        invalid("file read failure");
    }
    return bytes;
}

} // namespace

WorldPackage read_package(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    const std::span<const std::uint8_t> all(bytes);
    const auto payload = all.first(all.size() - package_checksum_bytes);
    Reader checksum_reader(all.last(package_checksum_bytes));
    if (crc32(payload) != checksum_reader.u32()) {
        invalid("CRC32 mismatch");
    }

    Reader reader(payload);
    constexpr std::array<std::uint8_t, 8> magic{'G', 'E', 'P', 'A', 'C', 'K', '0', '1'};
    const auto actual_magic = reader.take(magic.size());
    if (!std::equal(magic.begin(), magic.end(), actual_magic.begin())) {
        invalid("unknown magic");
    }
    if (reader.u32() != package_schema_version) {
        invalid("unsupported schema version");
    }
    const auto region_count = reader.u32();
    const auto entity_count = reader.u32();
    if (region_count == 0 || region_count > max_package_regions) {
        invalid("region count outside limits");
    }
    if (entity_count == 0 || entity_count > max_package_entities) {
        invalid("entity count outside limits");
    }
    if (reader.u32() != 0) {
        invalid("nonzero reserved header field");
    }

    // Counts are bounded above, and the calculation is explicitly widened
    // before multiplication/addition. No unchecked count controls allocation.
    const std::uint64_t expected_size = package_header_bytes
        + static_cast<std::uint64_t>(package_region_bytes) * region_count
        + static_cast<std::uint64_t>(package_entity_bytes) * entity_count
        + package_checksum_bytes;
    if (expected_size != bytes.size()) {
        invalid("file size does not match record counts");
    }

    WorldPackage package;
    package.baseline_id = reader.u64();
    if (package.baseline_id == 0) {
        invalid("zero baseline ID");
    }
    const auto digest = reader.take(package.source_digest.size());
    std::copy(digest.begin(), digest.end(), package.source_digest.begin());
    package.regions.reserve(region_count);

    std::uint32_t previous_region_id = 0;
    std::uint32_t next_entity = 0;
    for (std::uint32_t index = 0; index < region_count; ++index) {
        RegionDefinition region;
        region.id = reader.u32();
        region.required_mask = reader.u32();
        region.budget_bytes = reader.u64();
        region.first_entity = reader.u32();
        region.entity_count = reader.u32();

        if (region.id == 0 || region.id <= previous_region_id) {
            invalid("region IDs must be nonzero, unique, and increasing");
        }
        if (region.required_mask == 0 || (region.required_mask & ~package_readiness_mask) != 0) {
            invalid("invalid required readiness mask");
        }
        if (region.budget_bytes == 0) {
            invalid("zero region budget");
        }
        if (region.first_entity != next_entity || region.entity_count == 0) {
            invalid("entity ranges must be nonempty and contiguous");
        }
        // next_entity is always <= entity_count. Subtraction avoids overflow
        // even when a malformed region claims UINT32_MAX entities.
        if (region.entity_count > entity_count - next_entity) {
            invalid("entity range exceeds entity count");
        }
        next_entity += region.entity_count;
        previous_region_id = region.id;
        package.regions.push_back(region);
    }
    if (next_entity != entity_count) {
        invalid("entity ranges do not cover all entities");
    }

    package.entities.reserve(entity_count);
    std::vector<std::uint64_t> all_entity_ids;
    all_entity_ids.reserve(entity_count);
    for (const auto& region : package.regions) {
        std::uint64_t previous_entity_id = 0;
        for (std::uint32_t index = 0; index < region.entity_count; ++index) {
            EntityDefinition entity;
            entity.id = reader.u64();
            entity.region_id = reader.u32();
            entity.initial_value = reader.i32();
            for (auto& coordinate : entity.position_mm) {
                coordinate = reader.i32();
            }
            if (entity.id == 0 || entity.id <= previous_entity_id) {
                invalid("entity IDs must be nonzero and increasing within each region");
            }
            if (entity.region_id != region.id) {
                invalid("entity ownership does not match its region range");
            }
            previous_entity_id = entity.id;
            all_entity_ids.push_back(entity.id);
            package.entities.push_back(entity);
        }
    }
    if (!reader.finished()) {
        invalid("trailing payload data");
    }

    // A bounded sort also catches duplicates across different regions without
    // depending on hash-table behavior for untrusted IDs.
    std::sort(all_entity_ids.begin(), all_entity_ids.end());
    if (std::adjacent_find(all_entity_ids.begin(), all_entity_ids.end()) != all_entity_ids.end()) {
        invalid("globally duplicate entity ID");
    }
    return package;
}

} // namespace ge
