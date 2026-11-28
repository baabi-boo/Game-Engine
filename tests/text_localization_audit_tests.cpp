#include <ge/text/localization.hpp>

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace {

void utf16_output_limits_report_input_offsets() {
    struct Example {
        std::u16string_view input;
        std::size_t maximum_bytes;
        std::size_t expected_offset;
    };
    const std::array examples{
        Example{u"\u20acX", 3, 1},
        Example{u"\u20ac\U0001f600", 6, 1},
        Example{u"\U0001f600X", 4, 2},
    };
    for (const auto& example : examples) {
        bool rejected = false;
        try {
            static_cast<void>(ge::text::from_utf16(example.input, example.maximum_bytes));
        } catch (const ge::text::TextException& error) {
            rejected = true;
            CHECK(error.code() == ge::text::TextError::LimitExceeded);
            CHECK(error.offset() == example.expected_offset);
        }
        CHECK(rejected);
    }
    CHECK(ge::text::from_utf16(u"\u20ac\U0001f600", 7) == "\xE2\x82\xAC\xF0\x9F\x98\x80");
}

} // namespace

int main() {
    return ge::test::run("text_localization_audit", {
        {"utf16_output_limits_report_input_offsets", utf16_output_limits_report_input_offsets},
    });
}
