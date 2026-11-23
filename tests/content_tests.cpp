#include <ge/content/package.hpp>

#include "test_support.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

std::filesystem::path golden_path;
std::filesystem::path fixture_path;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64U; ++attempt) {
            path_ = root / ("ge-content-tests-" + std::to_string(stamp) + "-" +
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

[[nodiscard]] std::vector<std::uint8_t> read_golden_hex() {
    std::ifstream input(golden_path);
    CHECK(input.is_open());
    std::vector<std::uint8_t> bytes;
    std::string token;
    while (input >> token) {
        unsigned value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        CHECK(token.size() == 2U && result.ec == std::errc{} &&
              result.ptr == token.data() + token.size() && value <= 0xFFU);
        CHECK(bytes.size() < 120U);
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    CHECK(input.eof() && !input.bad());
    CHECK(bytes.size() == 120U);
    return bytes;
}

void write_binary(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.is_open());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    CHECK(output.good());
}

void check_region(const ge::RegionDefinition& actual, const ge::RegionDefinition& expected) {
    CHECK(actual.id == expected.id);
    CHECK(actual.required_mask == expected.required_mask);
    CHECK(actual.budget_bytes == expected.budget_bytes);
    CHECK(actual.first_entity == expected.first_entity);
    CHECK(actual.entity_count == expected.entity_count);
}

void check_entity(const ge::EntityDefinition& actual, const ge::EntityDefinition& expected) {
    CHECK(actual.id == expected.id);
    CHECK(actual.region_id == expected.region_id);
    CHECK(actual.initial_value == expected.initial_value);
    CHECK(actual.position_mm == expected.position_mm);
}

void frozen_v1_boundary_package() {
    // These checked-in bytes were transcribed from the wire specification, not
    // emitted by the production cooker or reconstructed by this test.
    const auto bytes = read_golden_hex();
    // Freeze the little-endian trailer as well as the decoded fields below.
    CHECK(bytes[116] == 0x99U);
    CHECK(bytes[117] == 0xD3U);
    CHECK(bytes[118] == 0xFDU);
    CHECK(bytes[119] == 0xB7U);
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "package-v1.gepack";
    write_binary(path, bytes);

    const auto package = ge::read_package(path);
    CHECK(package.baseline_id == 0xFEDCBA9876543210ULL);
    constexpr std::array<std::uint8_t, 32> expected_digest{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    CHECK(package.source_digest == expected_digest);
    CHECK(package.regions.size() == 1U);
    CHECK(package.entities.size() == 1U);
    check_region(package.regions[0], {0x89ABCDEFU, 13U, 0x8877665544332211ULL, 0U, 1U});
    check_entity(package.entities[0], {
        0xF1E2D3C4B5A69788ULL, 0x89ABCDEFU, std::numeric_limits<std::int32_t>::min(),
        {std::numeric_limits<std::int32_t>::max(), 0, -123456789},
    });
}

void cooked_fixture_all_fields() {
    const auto package = ge::read_package(fixture_path);
    CHECK(package.baseline_id == 1U);
    constexpr std::array<std::uint8_t, 32> expected_digest{
        0x45, 0x48, 0xF9, 0xD6, 0xCD, 0x41, 0x04, 0x4F,
        0x88, 0xC6, 0xFD, 0x34, 0x3B, 0x91, 0x67, 0xDB,
        0x1E, 0x61, 0xBF, 0x74, 0x81, 0x3F, 0x97, 0x66,
        0xF7, 0x1D, 0x9C, 0x35, 0xBA, 0xC4, 0x07, 0x63,
    };
    CHECK(package.source_digest == expected_digest);
    constexpr std::array<ge::RegionDefinition, 3> expected_regions{{
        {1U, 11U, 64U, 0U, 2U},
        {2U, 15U, 64U, 2U, 1U},
        {3U, 9U, 64U, 3U, 1U},
    }};
    constexpr std::array<ge::EntityDefinition, 4> expected_entities{{
        {1001U, 1U, 0, {0, 0, 0}},
        {1002U, 1U, 7, {1000, 0, -1000}},
        {2001U, 2U, 0, {100000, 0, 0}},
        {3001U, 3U, 3, {200000, 0, 0}},
    }};
    CHECK(package.regions.size() == expected_regions.size());
    CHECK(package.entities.size() == expected_entities.size());
    for (std::size_t index = 0; index < expected_regions.size(); ++index) {
        check_region(package.regions[index], expected_regions[index]);
    }
    for (std::size_t index = 0; index < expected_entities.size(); ++index) {
        check_entity(package.entities[index], expected_entities[index]);
    }
}

void invalid_paths_are_rejected() {
    CHECK_THROWS_AS(ge::read_package(std::filesystem::path{}), std::runtime_error);
    // A filesystem::path can retain NUL even though the native open operation
    // would truncate it and silently load the valid filename prefix.
    auto embedded_nul = fixture_path.native();
    embedded_nul.push_back(std::filesystem::path::value_type{});
    embedded_nul += std::filesystem::path("not-a-real-package").native();
    CHECK_THROWS_AS(ge::read_package(std::filesystem::path{embedded_nul}), std::runtime_error);
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    // Preserve native file arguments, including UTF-16 paths on Windows.
    using CommandCharacter = std::filesystem::path::value_type;
    constexpr CommandCharacter golden_option[]{'-', '-', 'g', 'o', 'l', 'd', 'e', 'n', '\0'};
    constexpr CommandCharacter fixture_option[]{'-', '-', 'f', 'i', 'x', 't', 'u', 'r', 'e', '\0'};
    constexpr std::string_view usage =
        "usage: ge_content_tests --golden package-v1.hex --fixture district.gepack\n";
    if (argc != 5) {
        std::cerr << usage;
        return 2;
    }
    for (int index = 1; index < argc; index += 2) {
        const std::basic_string_view<CommandCharacter> option(argv[index]);
        if (option == golden_option && golden_path.empty()) {
            golden_path = argv[index + 1];
        } else if (option == fixture_option && fixture_path.empty()) {
            fixture_path = argv[index + 1];
        } else {
            std::cerr << usage;
            return 2;
        }
    }
    if (golden_path.empty() || fixture_path.empty()) {
        std::cerr << usage;
        return 2;
    }
    return ge::test::run("content", {
        {"frozen_v1_boundary_package", frozen_v1_boundary_package},
        {"cooked_fixture_all_fields", cooked_fixture_all_fields},
        {"invalid_paths_are_rejected", invalid_paths_are_rejected},
    });
}
