#include "test_support.hpp"

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using ge::core::json_escape;

void valid_unicode_and_json_syntax_are_preserved() {
    const std::string unicode{"caf\xc3\xa9 \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x8c\x8d"};
    CHECK(json_escape(unicode) == unicode);
    // Include the scalar-value boundaries on both sides of the surrogate range.
    const std::string boundaries{"\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf"
                                 "\xee\x80\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"};
    CHECK(json_escape(boundaries) == boundaries);
    std::string controls{"\"\\\n\t\r\b\f"};
    controls.push_back('\0');
    controls.push_back('\x1f');
    CHECK(json_escape(controls) == R"(\"\\\u000a\u0009\u000d\u0008\u000c\u0000\u001f)");
    CHECK(json_escape("").empty());
}

void malformed_bytes_are_replaced_without_losing_valid_neighbors() {
    const std::pair<std::string_view, std::string_view> cases[]{
        {"\xff", R"(\ufffd)"},
        {"\x80", R"(\ufffd)"},
        {"\xc0\xaf", R"(\ufffd\ufffd)"},
        {"\xed\xa0\x80", R"(\ufffd\ufffd\ufffd)"},
        {"\xf4\x90\x80\x80", R"(\ufffd\ufffd\ufffd\ufffd)"},
        {"\xe2\x82", R"(\ufffd\ufffd)"},
        {"\xe2" "A\x80", R"(\ufffdA\ufffd)"},
        {"\xe2\xc3\xa9", "\\ufffd\xc3\xa9"},
    };
    const std::string prefix{"\xe4\xb8\x96\xe7\x95\x8c:"};
    const std::string suffix{"\xf0\x9f\x8c\x8d"};
    for (const auto& [malformed, expected] : cases) {
        const auto escaped = json_escape(prefix + std::string(malformed) + suffix);
        CHECK(escaped == prefix + std::string(expected) + suffix);
        CHECK(ge::core::valid_utf8(escaped));
    }
}

void diagnostics_and_exception_messages_produce_utf8_reports() {
    ge::core::Kernel kernel;
    kernel.diagnose(ge::core::Diagnostic::Severity::Error, "source/\xff",
                    "invalid \xc0\xaf \"quoted\"\n");
    CHECK(kernel.add_command("fault", [](ge::core::Kernel&, ge::core::CommandArgs)
        -> ge::core::CommandResult {
        throw std::runtime_error("failed \xff caf\xc3\xa9");
    }));
    CHECK(!kernel.command("fault").success);
    CHECK(kernel.diagnostics().size() == 2);
    const auto report = kernel.report_json();
    CHECK(ge::core::valid_utf8(report));
    CHECK(report.find(R"("subsystem":"source/\ufffd")") != std::string::npos);
    CHECK(report.find(R"("message":"invalid \ufffd\ufffd \"quoted\"\u000a")") != std::string::npos);
    CHECK(report.find("\"message\":\"failed \\ufffd caf\xc3\xa9\"") != std::string::npos);
    // Serialization repairs the representation without mutating diagnostic evidence.
    CHECK(kernel.diagnostics().front().subsystem == "source/\xff");
    CHECK(kernel.diagnostics().back().message == "failed \xff caf\xc3\xa9");
}

} // namespace

int main() {
    return ge::test::run("json_diagnostics_audit", {
        {"valid_unicode_and_json_syntax", valid_unicode_and_json_syntax_are_preserved},
        {"malformed_bytes_and_valid_neighbors", malformed_bytes_are_replaced_without_losing_valid_neighbors},
        {"diagnostics_and_exception_reports", diagnostics_and_exception_messages_produce_utf8_reports},
    });
}
