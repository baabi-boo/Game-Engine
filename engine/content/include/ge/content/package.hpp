#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ge {

inline constexpr std::uint32_t package_schema_version = 1;
inline constexpr std::size_t package_header_bytes = 64;
inline constexpr std::size_t package_region_bytes = 24;
inline constexpr std::size_t package_entity_bytes = 28;
inline constexpr std::size_t package_checksum_bytes = 4;
inline constexpr std::size_t max_package_bytes = 4 * 1024 * 1024;
inline constexpr std::uint32_t max_package_regions = 4096;
inline constexpr std::uint32_t max_package_entities = 65536;
// visual=1, collision=2, navigation=4, logic=8.
inline constexpr std::uint32_t package_readiness_mask = 15;

struct EntityDefinition {
    std::uint64_t id{};
    std::uint32_t region_id{};
    std::int32_t initial_value{};
    std::array<std::int32_t, 3> position_mm{};
};

struct RegionDefinition {
    std::uint32_t id{};
    std::uint32_t required_mask{};
    std::uint64_t budget_bytes{};
    std::uint32_t first_entity{};
    std::uint32_t entity_count{};
};

struct WorldPackage {
    std::uint64_t baseline_id{};
    // Carried source provenance: v1 does not verify this SHA256 digest natively.
    std::array<std::uint8_t, 32> source_digest{};
    std::vector<RegionDefinition> regions;
    std::vector<EntityDefinition> entities;
};

// Read the strict little-endian GEPACK01 format into temporary state and return
// only a fully validated package. Files larger than max_package_bytes, invalid
// records, unknown versions/reserved fields, and trailing data are rejected.
// Throws std::runtime_error on validation or file I/O failure. CRC32 detects
// accidental corruption; it does not authenticate the package or its source.
[[nodiscard]] WorldPackage read_package(const std::filesystem::path& path);

} // namespace ge
