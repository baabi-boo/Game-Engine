#include <ge/text/localization.hpp>

#include <ge/config/settings.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/io/filesystem.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <set>
#include <utility>

namespace ge::text {
namespace {

[[noreturn]] void fail(TextError code, std::size_t offset, std::string message) {
    throw TextException(code, offset, std::move(message));
}

void append(std::string& target, std::string_view value, std::size_t maximum) {
    std::size_t next{};
    if (!core::checked_add(target.size(), value.size(), next) || next > maximum) {
        fail(TextError::LimitExceeded, target.size(), "text output byte limit exceeded");
    }
    target.append(value);
}

void charge(std::size_t& total, std::size_t amount, std::size_t maximum) {
    std::size_t next{};
    if (!core::checked_add(total, amount, next) || next > maximum) {
        fail(TextError::LimitExceeded, total, "catalog accounting limit exceeded");
    }
    total = next;
}

bool letter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

char lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

char upper(char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

bool identifier(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) {
        return letter(c) || digit(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ':';
    });
}

void validate_limits(const LocalizationLimits& limits) {
    if (limits.max_catalogs == 0 || limits.max_catalogs > 1024 || limits.max_entries == 0 ||
        limits.max_key_bytes == 0 || limits.max_pattern_bytes == 0 ||
        limits.max_catalog_bytes == 0 || limits.max_total_bytes == 0 ||
        limits.max_document_bytes == 0 || limits.max_fallbacks == 0 || limits.max_fallbacks > 64 ||
        limits.max_depth == 0 || limits.max_depth > 64 || limits.max_nodes == 0 ||
        limits.max_arguments == 0 || limits.max_output_bytes == 0 || limits.max_history > 65536) {
        fail(TextError::InvalidArgument, 0, "invalid localization limits");
    }
}

void encode_scalar(std::string& result, char32_t scalar, std::size_t maximum,
                   std::size_t source_offset) {
    const auto value = static_cast<std::uint32_t>(scalar);
    std::array<char, 4> bytes{};
    std::size_t count{};
    if (value <= 0x7fU) {
        bytes[0] = static_cast<char>(value);
        count = 1;
    } else if (value <= 0x7ffU) {
        bytes[0] = static_cast<char>(0xc0U | (value >> 6U));
        bytes[1] = static_cast<char>(0x80U | (value & 0x3fU));
        count = 2;
    } else if (value <= 0xffffU && !(value >= 0xd800U && value <= 0xdfffU)) {
        bytes[0] = static_cast<char>(0xe0U | (value >> 12U));
        bytes[1] = static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
        bytes[2] = static_cast<char>(0x80U | (value & 0x3fU));
        count = 3;
    } else if (value >= 0x10000U && value <= 0x10ffffU) {
        bytes[0] = static_cast<char>(0xf0U | (value >> 18U));
        bytes[1] = static_cast<char>(0x80U | ((value >> 12U) & 0x3fU));
        bytes[2] = static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
        bytes[3] = static_cast<char>(0x80U | (value & 0x3fU));
        count = 4;
    } else {
        fail(TextError::InvalidUnicode, source_offset, "invalid Unicode scalar");
    }
    if (result.size() > maximum || count > maximum - result.size()) {
        fail(TextError::LimitExceeded, source_offset, "UTF-8 byte limit exceeded");
    }
    result.append(bytes.data(), count);
}

std::string_view language(std::string_view locale) noexcept {
    return locale.substr(0, locale.find('-'));
}

std::optional<std::int64_t> exact_integer(const FormatValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return *integer;
    }
    if (const auto* number = std::get_if<double>(&value)) {
        if (std::isfinite(*number) && std::trunc(*number) == *number &&
            *number >= -9223372036854775808.0 && *number < 9223372036854775808.0) {
            return static_cast<std::int64_t>(*number);
        }
    }
    return std::nullopt;
}

std::string_view plural_rule(const FormatValue& value, std::string_view locale) {
    const auto integer = exact_integer(value);
    if (!integer) {
        return "other";
    }
    const auto magnitude = *integer < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(*integer)
                                        : static_cast<std::uint64_t>(*integer);
    const auto lang = language(locale);
    if (lang == "fr") {
        return magnitude <= 1 ? "one" : "other";
    }
    if (lang == "en" || lang == "de" || lang == "es" || lang == "it") {
        return magnitude == 1 ? "one" : "other";
    }
    if (lang == "ru" || lang == "uk") {
        const auto ten = magnitude % 10U;
        const auto hundred = magnitude % 100U;
        if (ten == 1 && hundred != 11) {
            return "one";
        }
        if (ten >= 2 && ten <= 4 && !(hundred >= 12 && hundred <= 14)) {
            return "few";
        }
        return "many";
    }
    return "other";
}

enum class NodeKind : std::uint8_t { Literal, Argument, Number, Plural, Select, Pound };
struct Node {
    NodeKind kind{NodeKind::Literal};
    std::string value;
    std::map<std::string, std::vector<Node>, std::less<>> choices;
};

class PatternParser final {
public:
    PatternParser(std::string_view source, const LocalizationLimits& limits)
        : source_(source), limits_(limits) {}
    std::vector<Node> parse() {
        if (source_.size() > limits_.max_pattern_bytes || !core::valid_utf8(source_)) {
            fail(TextError::InvalidSyntax, 0, "pattern size or UTF-8 is invalid");
        }
        return sequence(0, false);
    }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_; }
private:
    void whitespace() {
        while (position_ < source_.size() && (source_[position_] == ' ' || source_[position_] == '\t')) {
            ++position_;
        }
    }
    void expect(char value) {
        whitespace();
        if (position_ == source_.size() || source_[position_] != value) {
            fail(TextError::InvalidSyntax, position_, "unexpected message punctuation");
        }
        ++position_;
    }
    std::string token() {
        whitespace();
        const auto start = position_;
        while (position_ < source_.size()) {
            const char c = source_[position_];
            if (c == '{' || c == '}' || c == ',' || c == ' ' || c == '\t') {
                break;
            }
            ++position_;
        }
        return std::string(source_.substr(start, position_ - start));
    }
    void add(std::vector<Node>& output, Node node) {
        if (nodes_ == limits_.max_nodes) {
            fail(TextError::LimitExceeded, position_, "message node limit exceeded");
        }
        ++nodes_;
        output.push_back(std::move(node));
    }
    Node placeholder(std::size_t depth) {
        Node node;
        node.value = token();
        if (!identifier(node.value, limits_.max_key_bytes)) {
            fail(TextError::InvalidArgument, position_, "invalid message argument name");
        }
        whitespace();
        if (position_ < source_.size() && source_[position_] == '}') {
            ++position_;
            node.kind = NodeKind::Argument;
            return node;
        }
        expect(',');
        const auto type = token();
        if (type == "number") {
            node.kind = NodeKind::Number;
            expect('}');
            return node;
        }
        if (type != "plural" && type != "select") {
            fail(TextError::InvalidSyntax, position_, "unknown message formatter");
        }
        node.kind = type == "plural" ? NodeKind::Plural : NodeKind::Select;
        expect(',');
        for (;;) {
            whitespace();
            if (position_ < source_.size() && source_[position_] == '}') {
                ++position_;
                break;
            }
            const auto selector = token();
            if (node.kind == NodeKind::Plural && !selector.empty() && selector.front() == '=') {
                std::int64_t number{};
                const auto part = std::string_view(selector).substr(1);
                const auto result = std::from_chars(part.data(), part.data() + part.size(), number);
                if (result.ec != std::errc{} || result.ptr != part.data() + part.size() ||
                    std::to_string(number) != part) {
                    fail(TextError::InvalidSyntax, position_, "invalid exact plural selector");
                }
            } else if (!identifier(selector, limits_.max_key_bytes)) {
                fail(TextError::InvalidSyntax, position_, "invalid message selector");
            } else if (node.kind == NodeKind::Plural && selector != "zero" && selector != "one" &&
                       selector != "two" && selector != "few" && selector != "many" && selector != "other") {
                fail(TextError::InvalidSyntax, position_, "unknown plural category");
            }
            if (nodes_ == limits_.max_nodes) {
                fail(TextError::LimitExceeded, position_, "message choice limit exceeded");
            }
            ++nodes_;
            expect('{');
            auto body = sequence(depth + 1, true);
            if (!node.choices.emplace(selector, std::move(body)).second) {
                fail(TextError::Duplicate, position_, "duplicate message selector");
            }
        }
        if (!node.choices.contains("other")) {
            fail(TextError::InvalidSyntax, position_, "message choice requires other branch");
        }
        return node;
    }
    std::vector<Node> sequence(std::size_t depth, bool nested) {
        if (depth > limits_.max_depth) {
            fail(TextError::LimitExceeded, position_, "message nesting limit exceeded");
        }
        std::vector<Node> result;
        std::string literal;
        const auto flush = [&]() {
            if (!literal.empty()) {
                add(result, Node{NodeKind::Literal, std::move(literal), {}});
                literal.clear();
            }
        };
        while (position_ < source_.size()) {
            const char c = source_[position_++];
            if (c == '\\') {
                if (position_ == source_.size()) {
                    fail(TextError::InvalidSyntax, position_, "unfinished message escape");
                }
                const char escaped = source_[position_++];
                if (escaped != '{' && escaped != '}' && escaped != '#' && escaped != '\\') {
                    fail(TextError::InvalidSyntax, position_, "unsupported message escape");
                }
                literal.push_back(escaped);
            } else if (c == '{') {
                flush();
                add(result, placeholder(depth));
            } else if (c == '#') {
                flush();
                add(result, Node{NodeKind::Pound, {}, {}});
            } else if (c == '}') {
                if (!nested) {
                    fail(TextError::InvalidSyntax, position_ - 1, "unmatched closing message brace");
                }
                flush();
                return result;
            } else {
                literal.push_back(c);
            }
        }
        if (nested) {
            fail(TextError::InvalidSyntax, position_, "unclosed message branch");
        }
        flush();
        return result;
    }
    std::string_view source_;
    const LocalizationLimits& limits_;
    std::size_t position_{};
    std::size_t nodes_{};
};

using ArgumentMap = std::map<std::string_view, const FormatValue*, std::less<>>;

ArgumentMap arguments_for(std::span<const FormatArgument> arguments, const LocalizationLimits& limits) {
    if (arguments.size() > limits.max_arguments) {
        fail(TextError::LimitExceeded, 0, "argument count limit exceeded");
    }
    ArgumentMap result;
    for (const auto& argument : arguments) {
        if (!identifier(argument.name, limits.max_key_bytes)) {
            fail(TextError::InvalidArgument, 0, "invalid argument name");
        }
        if (const auto* text = std::get_if<std::string>(&argument.value)) {
            if (text->size() > limits.max_output_bytes || !core::valid_utf8(*text)) {
                fail(TextError::InvalidArgument, 0, "invalid UTF-8 argument or argument too large");
            }
        }
        if (const auto* number = std::get_if<double>(&argument.value); number && !std::isfinite(*number)) {
            fail(TextError::InvalidArgument, 0, "nonfinite message argument");
        }
        if (!result.emplace(argument.name, &argument.value).second) {
            fail(TextError::Duplicate, 0, "duplicate argument name");
        }
    }
    return result;
}

std::string value_text(const FormatValue& value, std::string_view locale, std::size_t maximum) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "true" : "false";
    }
    return format_number(value, locale, {}, maximum);
}

void evaluate(const std::vector<Node>& nodes, const ArgumentMap& arguments,
              std::string_view locale, const FormatValue* plural, std::string& output,
              std::size_t maximum) {
    for (const auto& node : nodes) {
        if (node.kind == NodeKind::Literal) {
            append(output, node.value, maximum);
            continue;
        }
        if (node.kind == NodeKind::Pound) {
            append(output, plural ? format_number(*plural, locale, {}, maximum) : "#", maximum);
            continue;
        }
        const auto found = arguments.find(node.value);
        if (found == arguments.end()) {
            fail(TextError::MissingArgument, 0, "missing message argument: " + node.value);
        }
        const auto& value = *found->second;
        if (node.kind == NodeKind::Argument || node.kind == NodeKind::Number) {
            append(output, node.kind == NodeKind::Number ? format_number(value, locale, {}, maximum)
                                                         : value_text(value, locale, maximum), maximum);
            continue;
        }
        std::string selector;
        const FormatValue* nested_plural = plural;
        if (node.kind == NodeKind::Plural) {
            if (!std::holds_alternative<double>(value) && !std::holds_alternative<std::int64_t>(value)) {
                fail(TextError::InvalidArgument, 0, "plural argument must be numeric");
            }
            const auto integer = exact_integer(value);
            selector = integer ? "=" + std::to_string(*integer) : std::string{};
            if (!node.choices.contains(selector)) {
                selector = plural_rule(value, locale);
            }
            nested_plural = &value;
        } else {
            if (!std::holds_alternative<std::string>(value) && !std::holds_alternative<bool>(value)) {
                fail(TextError::InvalidArgument, 0, "select argument must be text or boolean");
            }
            selector = value_text(value, locale, maximum);
        }
        const auto choice = node.choices.find(selector);
        const auto& body = choice == node.choices.end() ? node.choices.at("other") : choice->second;
        evaluate(body, arguments, locale, nested_plural, output, maximum);
    }
}

} // namespace

TextException::TextException(TextError code, std::size_t offset, std::string message)
    : std::runtime_error(std::move(message)), code_(code), offset_(offset) {}
TextError TextException::code() const noexcept { return code_; }
std::size_t TextException::offset() const noexcept { return offset_; }
Utf8Cursor::Utf8Cursor(std::string_view text) noexcept : text_(text) {}
std::size_t Utf8Cursor::position() const noexcept { return position_; }
bool Utf8Cursor::empty() const noexcept { return position_ == text_.size(); }

std::optional<UnicodeScalar> Utf8Cursor::next() {
    if (empty()) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(text_[position_]);
    const std::size_t width = first <= 0x7fU ? 1U : first >= 0xc2U && first <= 0xdfU ? 2U
        : first >= 0xe0U && first <= 0xefU ? 3U : first >= 0xf0U && first <= 0xf4U ? 4U : 0U;
    if (width == 0 || width > text_.size() - position_ ||
        !core::valid_utf8(text_.substr(position_, width))) {
        fail(TextError::InvalidUnicode, position_, "malformed UTF-8 scalar");
    }
    std::uint32_t scalar = width == 1 ? first : first & ((1U << (7U - static_cast<unsigned>(width))) - 1U);
    for (std::size_t i = 1; i < width; ++i) {
        scalar = (scalar << 6U) | (static_cast<unsigned char>(text_[position_ + i]) & 0x3fU);
    }
    const UnicodeScalar result{static_cast<char32_t>(scalar), position_, width};
    position_ += width;
    return result;
}

std::size_t scalar_count(std::string_view text) {
    Utf8Cursor cursor(text);
    std::size_t count{};
    while (cursor.next()) {
        ++count;
    }
    return count;
}

std::u16string to_utf16(std::string_view text, std::size_t maximum_units) {
    std::u16string output;
    Utf8Cursor cursor(text);
    while (const auto scalar = cursor.next()) {
        const auto value = static_cast<std::uint32_t>(scalar->value);
        const std::size_t needed = value < 0x10000U ? 1U : 2U;
        if (output.size() > maximum_units || needed > maximum_units - output.size()) {
            fail(TextError::LimitExceeded, scalar->byte_offset, "UTF-16 unit limit exceeded");
        }
        if (needed == 1) {
            output.push_back(static_cast<char16_t>(value));
        } else {
            output.push_back(static_cast<char16_t>(0xd800U + ((value - 0x10000U) >> 10U)));
            output.push_back(static_cast<char16_t>(0xdc00U + ((value - 0x10000U) & 0x3ffU)));
        }
    }
    return output;
}

std::string from_utf16(std::u16string_view text, std::size_t maximum_bytes) {
    std::string output;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto source_offset = i;
        auto value = static_cast<std::uint32_t>(text[i]);
        if (value >= 0xd800U && value <= 0xdbffU) {
            const auto next = i + 1 == text.size() ? 0U : static_cast<std::uint32_t>(text[i + 1]);
            if (next < 0xdc00U || next > 0xdfffU) {
                fail(TextError::InvalidUnicode, i, "unpaired UTF-16 high surrogate");
            }
            value = 0x10000U + ((value - 0xd800U) << 10U) + (next - 0xdc00U);
            ++i;
        } else if (value >= 0xdc00U && value <= 0xdfffU) {
            fail(TextError::InvalidUnicode, i, "unpaired UTF-16 low surrogate");
        }
        encode_scalar(output, static_cast<char32_t>(value), maximum_bytes, source_offset);
    }
    return output;
}

std::string truncate_utf8(std::string_view text, std::size_t maximum_bytes, std::string_view suffix) {
    if (!core::valid_utf8(text) || !core::valid_utf8(suffix)) {
        fail(TextError::InvalidUnicode, 0, "truncation requires valid UTF-8");
    }
    if (text.size() <= maximum_bytes) {
        return std::string(text);
    }
    if (suffix.size() > maximum_bytes) {
        fail(TextError::LimitExceeded, 0, "truncation suffix exceeds byte limit");
    }
    const auto budget = maximum_bytes - suffix.size();
    Utf8Cursor cursor(text);
    std::size_t end{};
    while (const auto scalar = cursor.next()) {
        if (scalar->byte_offset + scalar->byte_width > budget) {
            break;
        }
        end = cursor.position();
    }
    std::string output(text.substr(0, end));
    append(output, suffix, maximum_bytes);
    return output;
}

std::string substring_utf8(std::string_view text, std::size_t first_scalar, std::size_t count) {
    Utf8Cursor cursor(text);
    std::size_t index{};
    std::size_t start = text.size();
    std::size_t end = text.size();
    while (const auto scalar = cursor.next()) {
        if (index == first_scalar) {
            start = scalar->byte_offset;
        }
        if (index >= first_scalar && index - first_scalar == count) {
            end = scalar->byte_offset;
        }
        ++index;
    }
    return std::string(text.substr(start, end - start));
}

std::string canonical_locale(std::string_view locale) {
    if (locale.empty() || locale.size() > 63) {
        fail(TextError::InvalidLocale, 0, "locale tag must contain 1..63 bytes");
    }
    std::string output;
    std::size_t begin{};
    while (begin < locale.size()) {
        const auto end = locale.find_first_of("-_", begin);
        auto part = locale.substr(begin, end == std::string_view::npos ? locale.size() - begin : end - begin);
        const bool letters = std::all_of(part.begin(), part.end(), letter);
        if (part.empty() || part.size() > 8 || (begin == 0 && (part.size() < 2 || !letters)) ||
            !std::all_of(part.begin(), part.end(), [](char c) { return letter(c) || digit(c); })) {
            fail(TextError::InvalidLocale, begin, "unsupported locale tag syntax");
        }
        if (begin != 0) {
            output.push_back('-');
        }
        for (std::size_t i = 0; i < part.size(); ++i) {
            const bool uppercase = begin != 0 && letters && (part.size() == 2 || (part.size() == 4 && i == 0));
            output.push_back(uppercase ? upper(part[i]) : lower(part[i]));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
        if (begin == locale.size()) {
            fail(TextError::InvalidLocale, begin, "trailing locale separator");
        }
    }
    return output;
}

std::string format_number(const FormatValue& value, std::string_view locale,
                          NumberOptions options, std::size_t maximum_bytes) {
    if (options.maximum_fraction_digits > 9 || options.minimum_fraction_digits > options.maximum_fraction_digits) {
        fail(TextError::InvalidArgument, 0, "invalid numeric fraction precision");
    }
    const auto canonical = canonical_locale(locale);
    std::array<char, 512> storage{};
    std::to_chars_result conversion{};
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        conversion = std::to_chars(storage.data(), storage.data() + storage.size(), *integer);
    } else if (const auto* number = std::get_if<double>(&value); number && std::isfinite(*number)) {
        conversion = std::to_chars(storage.data(), storage.data() + storage.size(), *number,
            std::chars_format::fixed, static_cast<int>(options.maximum_fraction_digits));
    } else {
        fail(TextError::InvalidArgument, 0, "number formatter requires a finite numeric argument");
    }
    if (conversion.ec != std::errc{}) {
        fail(TextError::LimitExceeded, 0, "number conversion capacity exceeded");
    }
    std::string digits(storage.data(), conversion.ptr);
    auto decimal = digits.find('.');
    if (decimal != std::string::npos) {
        while (digits.size() - decimal - 1 > options.minimum_fraction_digits && digits.back() == '0') {
            digits.pop_back();
        }
        if (digits.back() == '.') {
            digits.pop_back();
        }
    }
    decimal = digits.find('.');
    const auto integer_end = decimal == std::string::npos ? digits.size() : decimal;
    auto fraction = decimal == std::string::npos ? std::string{} : digits.substr(decimal + 1);
    fraction.append(options.minimum_fraction_digits > fraction.size()
        ? options.minimum_fraction_digits - fraction.size() : 0, '0');
    std::string_view group = ",";
    std::string_view point = ".";
    const auto lang = language(canonical);
    if (lang == "de" || lang == "es" || lang == "it" || lang == "pt") {
        group = ".";
        point = ",";
    } else if (lang == "fr" || lang == "ru") {
        group = lang == "fr" ? "\xE2\x80\xAF" : "\xC2\xA0";
        point = ",";
    }
    std::string output;
    const std::size_t start = !digits.empty() && digits.front() == '-' ? 1U : 0U;
    if (start != 0) {
        append(output, "-", maximum_bytes);
    }
    for (std::size_t i = start; i < integer_end; ++i) {
        if (options.grouping && i > start && (integer_end - i) % 3 == 0) {
            append(output, group, maximum_bytes);
        }
        append(output, std::string_view(digits).substr(i, 1), maximum_bytes);
    }
    if (!fraction.empty()) {
        append(output, point, maximum_bytes);
        append(output, fraction, maximum_bytes);
    }
    return output;
}

std::string format_message(std::string_view pattern, std::span<const FormatArgument> arguments,
                           std::string_view locale, LocalizationLimits limits) {
    validate_limits(limits);
    const auto canonical = canonical_locale(locale);
    const auto nodes = PatternParser(pattern, limits).parse();
    const auto values = arguments_for(arguments, limits);
    std::string output;
    evaluate(nodes, values, canonical, nullptr, output, limits.max_output_bytes);
    return output;
}

struct Catalog::Impl {
    CatalogDefinition definition;
    LocalizationLimits limits;
    std::map<std::string, std::vector<Node>, std::less<>> compiled;
    std::size_t bytes{};
};

Catalog::Catalog(CatalogDefinition definition, LocalizationLimits limits)
    : impl_(std::make_unique<Impl>()) {
    validate_limits(limits);
    definition.locale = canonical_locale(definition.locale);
    if (definition.version == 0 || definition.entries.size() > limits.max_entries ||
        definition.fallbacks.size() > limits.max_fallbacks) {
        fail(TextError::LimitExceeded, 0, "invalid catalog version or entry/fallback count");
    }
    std::set<std::string, std::less<>> unique;
    charge(impl_->bytes, definition.locale.size(), limits.max_catalog_bytes);
    for (auto& fallback : definition.fallbacks) {
        fallback = canonical_locale(fallback);
        if (fallback == definition.locale || !unique.insert(fallback).second) {
            fail(TextError::Duplicate, 0, "duplicate or self-referencing catalog fallback");
        }
        charge(impl_->bytes, fallback.size(), limits.max_catalog_bytes);
    }
    for (const auto& [key, pattern] : definition.entries) {
        if (!identifier(key, limits.max_key_bytes)) {
            fail(TextError::InvalidKey, 0, "invalid catalog key");
        }
        if (std::any_of(pattern.begin(), pattern.end(), [](char c) {
            return static_cast<unsigned char>(c) < 0x20U && c != '\n' && c != '\r' && c != '\t';
        })) {
            fail(TextError::InvalidSyntax, 0, "catalog pattern contains unsupported control bytes");
        }
        PatternParser parser(pattern, limits);
        auto compiled = parser.parse();
        std::size_t node_bytes{};
        if (!core::checked_multiply(parser.node_count(), sizeof(Node), node_bytes)) {
            fail(TextError::LimitExceeded, 0, "compiled message size overflow");
        }
        charge(impl_->bytes, node_bytes, limits.max_catalog_bytes);
        charge(impl_->bytes, key.size(), limits.max_catalog_bytes);
        charge(impl_->bytes, key.size(), limits.max_catalog_bytes);
        charge(impl_->bytes, pattern.size(), limits.max_catalog_bytes);
        charge(impl_->bytes, pattern.size(), limits.max_catalog_bytes);
        impl_->compiled.emplace(key, std::move(compiled));
    }
    impl_->definition = std::move(definition);
    impl_->limits = limits;
}

Catalog::~Catalog() = default;
const CatalogDefinition& Catalog::definition() const noexcept { return impl_->definition; }
std::size_t Catalog::accounted_bytes() const noexcept { return impl_->bytes; }

std::optional<std::string_view> Catalog::find(std::string_view key) const {
    const auto found = impl_->definition.entries.find(key);
    return found == impl_->definition.entries.end() ? std::nullopt : std::optional<std::string_view>{found->second};
}

std::string Catalog::format(std::string_view key, std::span<const FormatArgument> arguments) const {
    const auto found = impl_->compiled.find(key);
    if (found == impl_->compiled.end()) {
        fail(TextError::MissingKey, 0, "catalog key is absent");
    }
    const auto values = arguments_for(arguments, impl_->limits);
    std::string output;
    evaluate(found->second, values, impl_->definition.locale, nullptr, output, impl_->limits.max_output_bytes);
    return output;
}

namespace {

std::string quoted(std::string_view& line, std::size_t maximum, std::size_t line_number) {
    line = trim(line);
    if (line.empty() || line.front() != '"') {
        fail(TextError::InvalidSyntax, line_number, "catalog expects a quoted string");
    }
    line.remove_prefix(1);
    std::string output;
    while (!line.empty()) {
        char value = line.front();
        line.remove_prefix(1);
        if (value == '"') {
            return output;
        }
        if (value == '\\') {
            if (line.empty()) {
                fail(TextError::InvalidSyntax, line_number, "unfinished catalog escape");
            }
            value = line.front();
            line.remove_prefix(1);
            if (value == 'n') { value = '\n'; }
            else if (value == 'r') { value = '\r'; }
            else if (value == 't') { value = '\t'; }
            else if (value != '\\' && value != '"') {
                fail(TextError::InvalidSyntax, line_number, "unsupported catalog escape");
            }
        } else if (static_cast<unsigned char>(value) < 0x20U) {
            fail(TextError::InvalidSyntax, line_number, "literal control byte in catalog string");
        }
        append(output, std::string_view(&value, 1), maximum);
    }
    fail(TextError::InvalidSyntax, line_number, "unterminated catalog string");
}

void write_quoted(std::string& output, std::string_view text, std::size_t maximum) {
    append(output, "\"", maximum);
    for (const char c : text) {
        switch (c) {
        case '"': append(output, "\\\"", maximum); break;
        case '\\': append(output, "\\\\", maximum); break;
        case '\n': append(output, "\\n", maximum); break;
        case '\r': append(output, "\\r", maximum); break;
        case '\t': append(output, "\\t", maximum); break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                fail(TextError::InvalidSyntax, 0, "catalog text contains an unrepresentable control byte");
            }
            append(output, std::string_view(&c, 1), maximum);
        }
    }
    append(output, "\"", maximum);
}

std::string_view word(std::string_view& line) {
    line = trim(line);
    const auto end = line.find_first_of(" \t#");
    const auto result = line.substr(0, end);
    line.remove_prefix(end == std::string_view::npos ? line.size() : end);
    return result;
}

void require_line_end(std::string_view line, std::size_t number) {
    line = trim(line);
    if (!line.empty() && line.front() != '#') {
        fail(TextError::InvalidSyntax, number, "unexpected trailing catalog text");
    }
}

} // namespace

std::shared_ptr<const Catalog> parse_catalog(std::string_view document, LocalizationLimits limits) {
    validate_limits(limits);
    if (document.size() > limits.max_document_bytes || !core::valid_utf8(document)) {
        fail(TextError::InvalidSyntax, 0, "catalog document size or UTF-8 is invalid");
    }
    CatalogDefinition definition;
    std::size_t line_number{};
    std::size_t source_bytes{};
    bool header{};
    while (!document.empty()) {
        ++line_number;
        const auto newline = document.find('\n');
        auto line = trim(document.substr(0, newline));
        document.remove_prefix(newline == std::string_view::npos ? document.size() : newline + 1);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!header) {
            if (word(line) != "GELOC1") {
                fail(TextError::UnsupportedVersion, line_number, "expected GELOC1 catalog header");
            }
            definition.locale = word(line);
            const auto version = word(line);
            const auto parsed = std::from_chars(version.data(), version.data() + version.size(), definition.version);
            if (version.empty() || parsed.ec != std::errc{} || parsed.ptr != version.data() + version.size() ||
                definition.version == 0) {
                fail(TextError::InvalidSyntax, line_number, "invalid catalog content version");
            }
            header = true;
        } else if (line.front() == '@') {
            if (word(line) != "@fallback" || definition.fallbacks.size() >= limits.max_fallbacks) {
                fail(TextError::InvalidSyntax, line_number, "unknown directive or fallback limit exceeded");
            }
            definition.fallbacks.emplace_back(word(line));
        } else {
            if (definition.entries.size() >= limits.max_entries) {
                fail(TextError::LimitExceeded, line_number, "catalog entry limit exceeded");
            }
            auto key = quoted(line, limits.max_key_bytes, line_number);
            line = trim(line);
            if (line.empty() || line.front() != '=') {
                fail(TextError::InvalidSyntax, line_number, "catalog entry requires equals sign");
            }
            line.remove_prefix(1);
            auto pattern = quoted(line, limits.max_pattern_bytes, line_number);
            charge(source_bytes, key.size(), limits.max_catalog_bytes);
            charge(source_bytes, pattern.size(), limits.max_catalog_bytes);
            if (!definition.entries.emplace(std::move(key), std::move(pattern)).second) {
                fail(TextError::Duplicate, line_number, "duplicate catalog key");
            }
        }
        require_line_end(line, line_number);
    }
    if (!header) {
        fail(TextError::InvalidSyntax, 0, "empty catalog document");
    }
    return std::make_shared<const Catalog>(std::move(definition), limits);
}

std::string write_catalog(const Catalog& catalog, std::size_t maximum_bytes) {
    const auto& definition = catalog.definition();
    std::string output;
    append(output, "GELOC1 " + definition.locale + " " + std::to_string(definition.version) + "\n", maximum_bytes);
    for (const auto& fallback : definition.fallbacks) {
        append(output, "@fallback " + fallback + "\n", maximum_bytes);
    }
    for (const auto& [key, pattern] : definition.entries) {
        write_quoted(output, key, maximum_bytes);
        append(output, " = ", maximum_bytes);
        write_quoted(output, pattern, maximum_bytes);
        append(output, "\n", maximum_bytes);
    }
    return output;
}

struct LocalizationService::Impl {
    struct Record {
        std::shared_ptr<const Catalog> catalog;
        std::string source;
        std::uint64_t cache_key{};
    };
    struct PendingChange {
        LocalizationChanged event;
        std::deque<LocalizationChanged> history;
    };
    core::Kernel& kernel;
    LocalizationLimits limits;
    std::map<std::string, Record, std::less<>> records;
    std::string active{"en"};
    std::string default_tag{"en"};
    std::uint64_t revision{};
    std::size_t bytes{};
    std::deque<LocalizationChanged> history;
    core::Signal<LocalizationChanged> signal;
    core::SignalConnection settings_connection;
    std::uint64_t settings_revision{std::numeric_limits<std::uint64_t>::max()};
    bool settings_ready{};
    bool stopped{};

    Impl(core::Kernel& owner, LocalizationLimits bounds) : kernel(owner), limits(bounds) {}

    LocalizationResult result(bool success, bool changed, std::string message) const {
        return {success, changed, revision, std::move(message)};
    }
    PendingChange prepare(std::string reason, std::string affected, std::string locale_value) const {
        if (revision == std::numeric_limits<std::uint64_t>::max()) {
            fail(TextError::RevisionExhausted, 0, "localization revision exhausted");
        }
        PendingChange pending{{revision + 1, std::move(locale_value), std::move(affected), std::move(reason)}, history};
        if (limits.max_history != 0) {
            if (pending.history.size() == limits.max_history) {
                pending.history.pop_front();
            }
            pending.history.push_back(pending.event);
        }
        return pending;
    }
    void commit(PendingChange&& pending) noexcept {
        revision = pending.event.revision;
        history.swap(pending.history);
        try {
            if (auto* hub = kernel.services().find<core::SignalHub>()) {
                if (!signal) {
                    signal = hub->channel<LocalizationChanged>("text.localization.changed");
                }
                const auto dynamic = pending.event.locale.size() + pending.event.affected_catalog.size() + pending.event.reason.size();
                static_cast<void>(hub->enqueue(signal, std::move(pending.event), dynamic));
            }
        } catch (...) {
            // Notification is best-effort after publication; revision/history are authoritative.
        }
    }
    LocalizationResult admit(std::shared_ptr<const Catalog> catalog_value, std::string source,
                             std::uint64_t cache_key) {
        const auto& tag = catalog_value->definition().locale;
        const auto existing = records.find(tag);
        if (existing == records.end() && records.size() >= limits.max_catalogs) {
            return result(false, false, "catalog count limit exceeded");
        }
        const auto previous_bytes = existing == records.end() ? 0 : existing->second.catalog->accounted_bytes();
        auto next_bytes = bytes - previous_bytes;
        charge(next_bytes, catalog_value->accounted_bytes(), limits.max_total_bytes);
        auto pending = prepare("catalog installed", tag, active);
        LocalizationResult success{true, true, pending.event.revision, "catalog installed"};
        auto next_records = records;
        next_records.insert_or_assign(tag, Record{std::move(catalog_value), std::move(source), cache_key});
        records.swap(next_records);
        bytes = next_bytes;
        commit(std::move(pending));
        return success;
    }
    LocalizationResult activate(std::string tag) {
        if (tag == active) {
            return result(true, false, "locale unchanged");
        }
        auto pending = prepare("locale selected", {}, tag);
        LocalizationResult success{true, true, pending.event.revision, "locale selected"};
        active.swap(tag);
        commit(std::move(pending));
        return success;
    }
};

LocalizationService::LocalizationService(core::Kernel& kernel, LocalizationLimits limits)
    : impl_(std::make_unique<Impl>(kernel, limits)) {
    validate_limits(limits);
}
LocalizationService::~LocalizationService() = default;
std::string_view LocalizationService::locale() const noexcept { return impl_->active; }
std::string_view LocalizationService::default_locale() const noexcept { return impl_->default_tag; }
std::uint64_t LocalizationService::revision() const noexcept { return impl_->revision; }
LocalizationLimits LocalizationService::limits() const noexcept { return impl_->limits; }

LocalizationResult LocalizationService::install(CatalogDefinition definition) {
    try {
        return impl_->admit(std::make_shared<const Catalog>(std::move(definition), impl_->limits), {}, 0);
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::replace(std::string_view document) {
    try {
        return impl_->admit(parse_catalog(document, impl_->limits), {}, 0);
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::load(std::string_view path) {
    try {
        auto* filesystem = impl_->kernel.services().find<io::VirtualFileSystem>();
        if (!filesystem) {
            return impl_->result(false, false, "virtual filesystem is unavailable");
        }
        auto normalized = io::normalize_virtual_path(path, filesystem->limits().max_path_bytes);
        const auto before = filesystem->cache_key(normalized);
        auto parsed = parse_catalog(filesystem->read_text(normalized, impl_->limits.max_document_bytes), impl_->limits);
        const auto after = filesystem->cache_key(normalized);
        if (before != after) {
            return impl_->result(false, false, "catalog source changed during read");
        }
        return impl_->admit(std::move(parsed), std::move(normalized), after);
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::reload(std::string_view locale_value) {
    try {
        const auto found = impl_->records.find(canonical_locale(locale_value));
        if (found == impl_->records.end() || found->second.source.empty()) {
            return impl_->result(false, false, "catalog has no reload source");
        }
        const auto source = found->second.source;
        auto* filesystem = impl_->kernel.services().find<io::VirtualFileSystem>();
        if (!filesystem) {
            return impl_->result(false, false, "virtual filesystem is unavailable");
        }
        const auto before = filesystem->cache_key(source);
        auto parsed = parse_catalog(filesystem->read_text(source, impl_->limits.max_document_bytes), impl_->limits);
        if (parsed->definition().locale != found->first || filesystem->cache_key(source) != before) {
            return impl_->result(false, false, "reload locale or source revision changed");
        }
        return impl_->admit(std::move(parsed), source, before);
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::save(std::string_view locale_value, std::string_view path,
                                            std::optional<std::uint64_t> expected_cache_key) {
    try {
        const auto current = catalog(locale_value);
        auto* filesystem = impl_->kernel.services().find<io::VirtualFileSystem>();
        if (!current || !filesystem) {
            return impl_->result(false, false, "catalog or virtual filesystem is unavailable");
        }
        io::WriteOptions options;
        options.expected_cache_key = expected_cache_key;
        options.create_parents = true;
        auto success = impl_->result(true, false, "catalog written");
        filesystem->write_text(path, write_catalog(*current, impl_->limits.max_document_bytes), options);
        return success;
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::remove(std::string_view locale_value) {
    try {
        const auto found = impl_->records.find(canonical_locale(locale_value));
        if (found == impl_->records.end()) {
            return impl_->result(false, false, "catalog is absent");
        }
        auto pending = impl_->prepare("catalog removed", found->first, impl_->active);
        LocalizationResult success{true, true, pending.event.revision, "catalog removed"};
        impl_->bytes -= found->second.catalog->accounted_bytes();
        impl_->records.erase(found);
        impl_->commit(std::move(pending));
        return success;
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::set_default_locale(std::string_view locale_value) {
    try {
        auto tag = canonical_locale(locale_value);
        if (tag == impl_->default_tag) {
            return impl_->result(true, false, "default locale unchanged");
        }
        auto pending = impl_->prepare("default locale selected", tag, impl_->active);
        LocalizationResult success{true, true, pending.event.revision, "default locale selected"};
        impl_->default_tag.swap(tag);
        impl_->commit(std::move(pending));
        return success;
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::synchronize_settings() {
    if (impl_->stopped) {
        return impl_->result(false, false, "localization settings binding is stopped");
    }
    try {
        auto* settings = impl_->kernel.services().find<config::Settings>();
        if (!settings) {
            return impl_->activate(canonical_locale(impl_->kernel.config().text("text.locale", "en")));
        }
        if (!impl_->settings_ready) {
            config::SettingSchema schema;
            schema.key = "text.locale";
            schema.default_value = std::string("en");
            schema.kernel_key = "text.locale";
            schema.max_text_bytes = 63;
            schema.description = "Selected localization locale tag";
            const auto defined = settings->define(std::move(schema));
            if (!defined.success) {
                return impl_->result(false, false, "locale settings schema registration failed");
            }
            const auto layers = settings->layers();
            if (std::none_of(layers.begin(), layers.end(), [](const config::SettingsLayerInfo& layer) {
                return layer.name == "text.localization";
            })) {
                const auto added = settings->add_layer("text.localization", 1000);
                if (!added.success) {
                    return impl_->result(false, false, "locale settings layer registration failed");
                }
            }
            impl_->settings_ready = true;
        }
        if (!impl_->settings_connection.connected()) {
            impl_->settings_connection = settings->bind("text.locale",
                [this](const config::SettingChange&, const core::SignalContext&) {
                    static_cast<void>(synchronize_settings());
                });
        }
        if (impl_->settings_revision == settings->revision()) {
            return impl_->result(true, false, "locale settings unchanged");
        }
        auto tag = canonical_locale(settings->text("text.locale", "en"));
        auto result = impl_->activate(std::move(tag));
        impl_->settings_revision = settings->revision();
        return result;
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

LocalizationResult LocalizationService::use_locale(std::string_view locale_value) {
    try {
        auto tag = canonical_locale(locale_value);
        auto* settings = impl_->kernel.services().find<config::Settings>();
        if (!settings) {
            if (!impl_->kernel.config().set("text.locale", tag)) {
                return impl_->result(false, false, "Kernel locale configuration rejected");
            }
            return impl_->activate(std::move(tag));
        }
        auto synchronized = synchronize_settings();
        if (!impl_->settings_ready) {
            return synchronized;
        }
        const auto changed = settings->set("text.localization", "text.locale", tag);
        if (!changed.success) {
            return impl_->result(false, false, "locale setting could not be applied");
        }
        auto result = synchronize_settings();
        if (result.success && impl_->active != tag) {
            return impl_->result(false, result.changed, "locale request is shadowed by another settings layer");
        }
        return result;
    } catch (const std::exception& error) {
        return impl_->result(false, false, error.what());
    }
}

void LocalizationService::disconnect_settings() noexcept {
    impl_->stopped = true;
    static_cast<void>(impl_->settings_connection.disconnect());
}

std::vector<std::string> LocalizationService::fallback_chain(std::string_view locale_value) const {
    std::vector<std::string> output;
    std::set<std::string, std::less<>> seen;
    const auto visit = [&](const auto& self, std::string tag) -> void {
        if (seen.contains(tag)) {
            return;
        }
        if (output.size() >= impl_->limits.max_fallbacks) {
            fail(TextError::LimitExceeded, 0, "locale fallback chain limit exceeded");
        }
        seen.insert(tag);
        output.push_back(tag);
        const auto found = impl_->records.find(tag);
        if (found != impl_->records.end()) {
            for (const auto& explicit_tag : found->second.catalog->definition().fallbacks) {
                self(self, explicit_tag);
            }
        }
        const auto parent = tag.rfind('-');
        if (parent != std::string::npos) {
            tag.resize(parent);
            self(self, std::move(tag));
        }
    };
    visit(visit, locale_value.empty() ? impl_->active : canonical_locale(locale_value));
    visit(visit, impl_->default_tag);
    return output;
}

LookupResult LocalizationService::lookup(std::string_view key, std::string_view locale_value,
                                         MissingKeyPolicy policy) const {
    if (!identifier(key, impl_->limits.max_key_bytes)) {
        fail(TextError::InvalidKey, 0, "invalid localization lookup key");
    }
    LookupResult result;
    result.requested_locale = locale_value.empty() ? impl_->active : canonical_locale(locale_value);
    result.service_revision = impl_->revision;
    for (const auto& tag : fallback_chain(result.requested_locale)) {
        const auto found = impl_->records.find(tag);
        if (found == impl_->records.end()) {
            continue;
        }
        if (const auto pattern = found->second.catalog->find(key)) {
            result.found = true;
            result.text = *pattern;
            result.resolved_locale = tag;
            result.catalog_version = found->second.catalog->definition().version;
            return result;
        }
    }
    switch (policy) {
    case MissingKeyPolicy::ReturnKey: append(result.text, key, impl_->limits.max_output_bytes); break;
    case MissingKeyPolicy::Empty: break;
    case MissingKeyPolicy::Marker:
        append(result.text, "[[", impl_->limits.max_output_bytes);
        append(result.text, key, impl_->limits.max_output_bytes);
        append(result.text, "]]", impl_->limits.max_output_bytes);
        break;
    case MissingKeyPolicy::Error: fail(TextError::MissingKey, 0, "missing localization key: " + std::string(key));
    }
    return result;
}

LookupResult LocalizationService::format(std::string_view key, std::span<const FormatArgument> arguments,
                                         std::string_view locale_value, MissingKeyPolicy policy) const {
    auto result = lookup(key, locale_value, policy);
    if (result.found) {
        result.text = impl_->records.at(result.resolved_locale).catalog->format(key, arguments);
    }
    return result;
}

std::shared_ptr<const Catalog> LocalizationService::catalog(std::string_view locale_value) const {
    const auto found = impl_->records.find(canonical_locale(locale_value));
    return found == impl_->records.end() ? nullptr : found->second.catalog;
}

std::vector<CatalogInfo> LocalizationService::catalogs() const {
    std::vector<CatalogInfo> output;
    output.reserve(impl_->records.size());
    for (const auto& [tag, record] : impl_->records) {
        output.push_back({tag, record.catalog->definition().version, record.catalog->definition().entries.size(),
                          record.catalog->accounted_bytes(), record.source, record.cache_key});
    }
    return output;
}

LocalizationHistory LocalizationService::changes_since(std::uint64_t revision_value, std::size_t limit) const {
    LocalizationHistory result;
    result.current_revision = impl_->revision;
    result.gap = revision_value > impl_->revision ||
        (revision_value < impl_->revision && (impl_->history.empty() || revision_value < impl_->history.front().revision - 1));
    for (const auto& event : impl_->history) {
        if (event.revision > revision_value) {
            if (result.events.size() >= limit) {
                result.truncated = true;
                break;
            }
            result.events.push_back(event);
        }
    }
    return result;
}

core::Signal<LocalizationChanged> LocalizationService::changed_signal() {
    if (auto* hub = impl_->kernel.services().find<core::SignalHub>(); hub && !impl_->signal) {
        impl_->signal = hub->channel<LocalizationChanged>("text.localization.changed");
    }
    return impl_->signal;
}

void register_localization(core::Kernel& kernel) {
    if (!kernel.register_module({"text.localization", "Unicode utilities and immutable localization catalogs", 1})) {
        return;
    }
    auto& service = kernel.services().emplace<LocalizationService>(kernel);
    static_cast<void>(service.synchronize_settings());
    kernel.add_shutdown("text.localization", [target = &service] { target->disconnect_settings(); });
    kernel.add_system({"text.localization", core::Phase::PreSimulation, -8700, {}},
        [](core::Kernel& owner, const core::Frame&) {
            auto& current = owner.services().get<LocalizationService>();
            const auto result = current.synchronize_settings();
            if (!result.success) {
                owner.diagnose(core::Diagnostic::Severity::Warning, "text.localization", result.message);
            }
            owner.metric("text.localization.revision", static_cast<double>(current.revision()));
        });
    kernel.add_command("text.load", [](core::Kernel& owner, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: text.load <vfs-path>"}; }
        const auto result = owner.services().get<LocalizationService>().load(args[0]);
        return {result.success, result.message};
    }, "Load or replace a GELOC1 catalog through VFS");
    kernel.add_command("text.use", [](core::Kernel& owner, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: text.use <locale>"}; }
        const auto result = owner.services().get<LocalizationService>().use_locale(args[0]);
        return {result.success, result.message};
    }, "Set text.locale through Settings");
    kernel.add_command("text.reload", [](core::Kernel& owner, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: text.reload <locale>"}; }
        const auto result = owner.services().get<LocalizationService>().reload(args[0]);
        return {result.success, result.message};
    }, "Reload a catalog while retaining its last valid version on errors");
    kernel.add_command("text.save", [](core::Kernel& owner, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: text.save <locale> <vfs-path>"}; }
        const auto result = owner.services().get<LocalizationService>().save(args[0], args[1]);
        return {result.success, result.message};
    }, "Write a sorted GELOC1 catalog");
    kernel.add_command("text.format", [](core::Kernel& owner, core::CommandArgs args) -> core::CommandResult {
        auto& current = owner.services().get<LocalizationService>();
        if (args.empty() || args.size() - 1 > current.limits().max_arguments) {
            return {false, "usage: text.format <key> [name=value ...]"};
        }
        std::vector<FormatArgument> values;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto equal = args[i].find('=');
            if (equal == std::string::npos) { return {false, "argument requires name=value"}; }
            const auto literal = args[i].substr(equal + 1);
            const auto typed = config::parse_setting(literal, current.limits().max_output_bytes);
            FormatValue value = literal;
            if (typed) {
                std::visit([&value](const auto& item) { value = item; }, *typed);
            }
            values.push_back({args[i].substr(0, equal), std::move(value)});
        }
        try {
            const auto result = current.format(args[0], values, {}, MissingKeyPolicy::Error);
            return {true, result.text};
        } catch (const std::exception& error) {
            return {false, error.what()};
        }
    }, "Format a localized key with named typed arguments");
    kernel.add_command("text.inspect", [](core::Kernel& owner, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: text.inspect"}; }
        const auto& current = owner.services().get<LocalizationService>();
        std::string message = "locale=" + std::string(current.locale()) + " revision=" + std::to_string(current.revision());
        for (const auto& info : current.catalogs()) {
            message += "\n" + info.locale + " version=" + std::to_string(info.version) +
                " entries=" + std::to_string(info.entries) + " bytes=" + std::to_string(info.accounted_bytes);
        }
        return {true, std::move(message)};
    }, "Inspect active locale and admitted catalog versions");
}

} // namespace ge::text
