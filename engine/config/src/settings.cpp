#include <ge/config/settings.hpp>

#include <ge/core/runtime.hpp>
#include <ge/io/filesystem.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ge::config {
namespace {

using ValueMap = std::map<std::string, EffectiveSetting, std::less<>>;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool valid_utf8(std::string_view text) {
    std::size_t position{};
    while (position < text.size()) {
        const auto first = static_cast<unsigned char>(text[position++]);
        if (first < 0x80U) {
            continue;
        }
        std::uint32_t code{};
        std::uint32_t minimum{};
        std::size_t remaining{};
        if (first >= 0xc2U && first <= 0xdfU) {
            code = first & 0x1fU;
            minimum = 0x80U;
            remaining = 1;
        } else if (first >= 0xe0U && first <= 0xefU) {
            code = first & 0x0fU;
            minimum = 0x800U;
            remaining = 2;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            code = first & 0x07U;
            minimum = 0x10000U;
            remaining = 3;
        } else {
            return false;
        }
        if (remaining > text.size() - position) {
            return false;
        }
        for (std::size_t index = 0; index < remaining; ++index) {
            const auto next = static_cast<unsigned char>(text[position++]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3fU);
        }
        if (code < minimum || code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

bool valid_key(std::string_view key, std::size_t maximum) {
    if (key.empty() || key.size() > maximum) {
        return false;
    }
    bool segment_start = true;
    for (const char value : key) {
        const bool alpha = (value >= 'a' && value <= 'z') ||
                           (value >= 'A' && value <= 'Z') || value == '_';
        if (value == '.') {
            if (segment_start) {
                return false;
            }
            segment_start = true;
        } else {
            if (!alpha && (segment_start || !((value >= '0' && value <= '9') || value == '-'))) {
                return false;
            }
            segment_start = false;
        }
    }
    return !segment_start;
}

void append_codepoint(std::string& result, std::uint32_t code) {
    if (code <= 0x7fU) {
        result.push_back(static_cast<char>(code));
    } else if (code <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (code >> 6U)));
        result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else if (code <= 0xffffU) {
        result.push_back(static_cast<char>(0xe0U | (code >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xf0U | (code >> 18U)));
        result.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    }
}

std::optional<std::uint32_t> hex_quad(std::string_view input, std::size_t& position) {
    if (position > input.size() || input.size() - position < 4) {
        return std::nullopt;
    }
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        const char character = input[position++];
        std::uint32_t digit{};
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint32_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<std::uint32_t>(character - 'a') + 10U;
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<std::uint32_t>(character - 'A') + 10U;
        } else {
            return std::nullopt;
        }
        value = (value << 4U) | digit;
    }
    return value;
}

std::optional<std::string> quoted_text(std::string_view input, std::size_t maximum) {
    if (input.size() < 2 || input.front() != '"' || input.back() != '"') {
        return std::nullopt;
    }
    input.remove_suffix(1);
    std::string result;
    for (std::size_t position = 1; position < input.size();) {
        const char character = input[position++];
        if (character == '"' || static_cast<unsigned char>(character) < 0x20U) {
            return std::nullopt;
        }
        if (character != '\\') {
            result.push_back(character);
        } else {
            if (position == input.size()) {
                return std::nullopt;
            }
            const char escaped = input[position++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                auto code = hex_quad(input, position);
                if (!code) {
                    return std::nullopt;
                }
                if (*code >= 0xd800U && *code <= 0xdbffU) {
                    if (input.size() - position < 6 || input[position] != '\\' || input[position + 1] != 'u') {
                        return std::nullopt;
                    }
                    position += 2;
                    const auto low = hex_quad(input, position);
                    if (!low || *low < 0xdc00U || *low > 0xdfffU) {
                        return std::nullopt;
                    }
                    code = 0x10000U + ((*code - 0xd800U) << 10U) + (*low - 0xdc00U);
                } else if (*code >= 0xdc00U && *code <= 0xdfffU) {
                    return std::nullopt;
                }
                append_codepoint(result, *code);
                break;
            }
            default: return std::nullopt;
            }
        }
        if (result.size() > maximum) {
            return std::nullopt;
        }
    }
    return valid_utf8(result) ? std::optional<std::string>{std::move(result)} : std::nullopt;
}

std::string quote(std::string_view input) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result = "\"";
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20U) {
                result += "\\u00";
                result.push_back(hex[byte >> 4U]);
                result.push_back(hex[byte & 0x0fU]);
            } else {
                result.push_back(character);
            }
        }
    }
    result.push_back('"');
    return result;
}

std::string kernel_text(const SettingValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return format_setting(value);
}

struct Failure final : std::runtime_error {
    SettingsIssue issue;
    explicit Failure(SettingsIssue detail)
        : std::runtime_error(detail.message), issue(std::move(detail)) {}
};

[[noreturn]] void fail(std::string message, std::string source = {},
                      std::size_t line = 0, std::string key = {}) {
    throw Failure({std::move(source), line, std::move(key), std::move(message), false});
}

void charge(std::size_t& used, std::size_t amount, std::size_t maximum) {
    if (used > maximum || amount > maximum - used) {
        fail("settings byte budget exceeded");
    }
    used += amount;
}

std::size_t value_bytes(const SettingValue& value) {
    const auto* text = std::get_if<std::string>(&value);
    return sizeof(SettingValue) + (text ? text->size() : 0);
}

struct Entry {
    SettingValue value;
    std::string source;
    std::size_t line{};
    bool kernel_encoded{};
};

struct Stamp {
    std::string path;
    std::uint64_t key{};
    bool operator==(const Stamp&) const = default;
};

struct Layer {
    std::string name;
    int priority{};
    std::uint64_t order{};
    bool enabled{true};
    UnknownKeyPolicy policy{UnknownKeyPolicy::Reject};
    std::map<std::string, Entry, std::less<>> entries;
    std::string file;
    std::vector<Stamp> stamps;
    bool watched{};
    bool dirty{};
};

struct State {
    std::map<std::string, SettingSchema, std::less<>> schemas;
    std::vector<Layer> layers;
    ValueMap effective;
    std::set<std::string, std::less<>> owned_kernel_keys;
    std::uint64_t revision{};
    std::uint64_t next_order{1};
};

Layer& layer_named(State& state, std::string_view name) {
    const auto found = std::find_if(state.layers.begin(), state.layers.end(),
        [name](const Layer& layer) { return layer.name == name; });
    if (found == state.layers.end()) {
        fail("unknown settings layer", {}, 0, std::string(name));
    }
    return *found;
}

SettingsResult rejected(std::uint64_t revision, SettingsIssue issue) {
    SettingsResult result;
    result.revision = revision;
    result.issues.push_back(std::move(issue));
    return result;
}

std::size_t event_bytes(const SettingsChanged& event) {
    std::size_t total = event.reason.size();
    for (const auto& change : event.changes) {
        charge(total, sizeof(SettingChange) + change.key.size(), std::numeric_limits<std::size_t>::max());
        for (const auto* side : {&change.before, &change.after}) {
            if (*side) {
                const auto& entry = **side;
                charge(total, value_bytes(entry.value), std::numeric_limits<std::size_t>::max());
                charge(total, entry.origin.layer.size() + entry.origin.source.size() + entry.kernel_key.size(),
                       std::numeric_limits<std::size_t>::max());
            }
        }
    }
    return total;
}

std::string result_text(const SettingsResult& result) {
    std::string text = result.success ? "accepted" : "rejected";
    if (result.applied) {
        text += " revision=" + std::to_string(result.revision) + " changes=" + std::to_string(result.changed);
    }
    if (result.notification == SettingsNotification::Dropped) {
        text += " notification=dropped (configuration applied)";
    } else if (result.notification == SettingsNotification::Unavailable) {
        text += " notification=unavailable (configuration applied)";
    }
    for (const auto& issue : result.issues) {
        text += "\n" + issue.source + ":" + std::to_string(issue.line) + " " + issue.key + " " + issue.message;
    }
    return text;
}

} // namespace

SettingType setting_type(const SettingValue& value) noexcept {
    return static_cast<SettingType>(value.index());
}

std::string_view setting_type_name(SettingType type) noexcept {
    switch (type) {
    case SettingType::Boolean: return "boolean";
    case SettingType::Integer: return "integer";
    case SettingType::Number: return "number";
    case SettingType::Text: return "text";
    }
    return "invalid";
}

std::string format_setting(const SettingValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return quote(*text);
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "true" : "false";
    }
    std::array<char, 128> buffer{};
    std::to_chars_result converted;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *integer);
    } else {
        const double number = std::get<double>(value);
        if (!std::isfinite(number)) {
            throw std::invalid_argument("nonfinite setting value");
        }
        converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number,
                                  std::chars_format::general, std::numeric_limits<double>::max_digits10);
    }
    if (converted.ec != std::errc{}) {
        throw std::runtime_error("setting number formatting failed");
    }
    std::string result(buffer.data(), converted.ptr);
    if (std::holds_alternative<double>(value) && result.find_first_of(".eE") == std::string::npos) {
        result += ".0";
    }
    return result;
}

std::optional<SettingValue> parse_setting(std::string_view literal, std::size_t maximum_text_bytes) {
    literal = trim(literal);
    if (literal.empty()) {
        return std::nullopt;
    }
    if (literal.front() == '"') {
        auto value = quoted_text(literal, maximum_text_bytes);
        return value ? std::optional<SettingValue>{std::move(*value)} : std::nullopt;
    }
    if (literal == "true" || literal == "false") {
        return SettingValue{literal == "true"};
    }
    const char* first = literal.data();
    const char* last = first + literal.size();
    if (literal.find_first_of(".eE") == std::string_view::npos) {
        std::int64_t value{};
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec == std::errc{} && parsed.ptr == last) {
            return SettingValue{value};
        }
    } else {
        double value{};
        const auto parsed = std::from_chars(first, last, value, std::chars_format::general);
        if (parsed.ec == std::errc{} && parsed.ptr == last && std::isfinite(value)) {
            return SettingValue{value};
        }
    }
    return std::nullopt;
}

struct Settings::Impl {
    struct HistoryEntry {
        std::shared_ptr<const SettingsChanged> event;
        std::size_t bytes{};
    };
    struct LoadContext {
        io::VirtualFileSystem* filesystem{};
        std::vector<std::string> active;
        std::vector<Stamp> stamps;
        std::size_t total_bytes{};
        std::size_t entry_bytes{};
    };
    core::Kernel& kernel;
    SettingsLimits bounds;
    std::unique_ptr<State> state{std::make_unique<State>()};
    std::deque<HistoryEntry> history;
    std::size_t history_bytes{};
    std::size_t poll_cursor{};

    Impl(core::Kernel& host, SettingsLimits limits) : kernel(host), bounds(limits) {
        if (bounds.max_keys == 0 || bounds.max_keys > 4096 || bounds.max_layers == 0 ||
            bounds.max_layers > 1024 || bounds.max_key_bytes == 0 || bounds.max_key_bytes > 256 ||
            bounds.max_text_bytes > 65536 || bounds.max_line_bytes == 0 || bounds.max_document_bytes == 0 ||
            bounds.max_include_depth == 0 || bounds.max_include_depth > 64 ||
            bounds.max_include_files == 0 || bounds.max_include_files > 4096 || bounds.max_choices > 4096 ||
            bounds.max_history > 4096 || bounds.max_total_bytes < sizeof(State) ||
            bounds.max_total_bytes > 256U * 1024U * 1024U ||
            bounds.max_document_bytes > 64U * 1024U * 1024U ||
            bounds.max_line_bytes > bounds.max_document_bytes ||
            bounds.max_history_bytes > 256U * 1024U * 1024U) {
            throw std::invalid_argument("invalid settings limits");
        }
    }

    void validate_value(SettingValue& value, const SettingSchema* schema,
                        const std::string& key, const Entry* entry = nullptr) const {
        const auto error = [&](std::string message) {
            fail(std::move(message), entry ? entry->source : "<schema>", entry ? entry->line : 0, key);
        };
        if (schema && std::holds_alternative<double>(schema->default_value) &&
            std::holds_alternative<std::int64_t>(value)) {
            const auto integer = std::get<std::int64_t>(value);
            constexpr std::int64_t exact = 9007199254740992LL;
            if (integer < -exact || integer > exact) {
                error("integer cannot be represented exactly as a number");
            }
            value = static_cast<double>(integer);
        }
        if (schema && setting_type(value) != setting_type(schema->default_value)) {
            error("setting type does not match its schema");
        }
        if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number) || (schema && schema->number_min && *number < *schema->number_min) ||
                (schema && schema->number_max && *number > *schema->number_max)) {
                error("number is nonfinite or outside its allowed range");
            }
        } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            if (schema && ((schema->integer_min && *integer < *schema->integer_min) ||
                           (schema->integer_max && *integer > *schema->integer_max))) {
                error("integer is outside its allowed range");
            }
        } else if (const auto* text = std::get_if<std::string>(&value)) {
            const auto maximum = schema ? std::min(bounds.max_text_bytes, schema->max_text_bytes) : bounds.max_text_bytes;
            if (text->size() > maximum || !valid_utf8(*text)) {
                error("text exceeds its byte limit or is not valid UTF-8");
            }
            if (schema && !schema->choices.empty() &&
                std::find(schema->choices.begin(), schema->choices.end(), *text) == schema->choices.end()) {
                error("text is not an allowed choice");
            }
        }
    }

    void validate_schema(SettingSchema& schema) const {
        if (!valid_key(schema.key, bounds.max_key_bytes) ||
            (!schema.kernel_key.empty() && !valid_key(schema.kernel_key, bounds.max_key_bytes))) {
            fail("invalid schema or Kernel key", "<schema>", 0, schema.key);
        }
        const auto type = setting_type(schema.default_value);
        if ((type != SettingType::Integer && (schema.integer_min || schema.integer_max)) ||
            (type != SettingType::Number && (schema.number_min || schema.number_max)) ||
            (type != SettingType::Text && !schema.choices.empty()) ||
            (schema.integer_min && schema.integer_max && *schema.integer_min > *schema.integer_max) ||
            (schema.number_min && (!std::isfinite(*schema.number_min))) ||
            (schema.number_max && (!std::isfinite(*schema.number_max))) ||
            (schema.number_min && schema.number_max && *schema.number_min > *schema.number_max) ||
            schema.choices.size() > bounds.max_choices || schema.description.size() > 4096 ||
            !valid_utf8(schema.description)) {
            fail("inconsistent schema constraints", "<schema>", 0, schema.key);
        }
        std::set<std::string, std::less<>> choices;
        for (const auto& choice : schema.choices) {
            if (choice.size() > std::min(bounds.max_text_bytes, schema.max_text_bytes) ||
                !valid_utf8(choice) || !choices.insert(choice).second) {
                fail("invalid or duplicate schema choice", "<schema>", 0, schema.key);
            }
        }
        validate_value(schema.default_value, &schema, schema.key);
    }

    void resolve(State& next, SettingsResult& result) const {
        if (next.schemas.size() > bounds.max_keys || next.layers.size() > bounds.max_layers) {
            fail("settings key or layer capacity exceeded");
        }
        std::size_t bytes = sizeof(State);
        next.effective.clear();
        for (auto& [key, schema] : next.schemas) {
            validate_schema(schema);
            charge(bytes, sizeof(SettingSchema) + key.size() + schema.kernel_key.size() + schema.description.size(), bounds.max_total_bytes);
            charge(bytes, value_bytes(schema.default_value), bounds.max_total_bytes);
            for (const auto& choice : schema.choices) {
                charge(bytes, sizeof(std::string) + choice.size(), bounds.max_total_bytes);
            }
            SettingOrigin origin{"<defaults>", "<schema>", 0, std::numeric_limits<int>::min(), 0};
            next.effective.emplace(key, EffectiveSetting{schema.default_value, std::move(origin),
                schema.kernel_key.empty() ? key : schema.kernel_key});
        }
        std::vector<Layer*> ordered;
        for (auto& layer : next.layers) {
            ordered.push_back(&layer);
        }
        std::sort(ordered.begin(), ordered.end(), [](const Layer* left, const Layer* right) {
            return left->priority == right->priority ? left->order < right->order : left->priority < right->priority;
        });
        for (auto* layer : ordered) {
            if (layer->policy != UnknownKeyPolicy::Reject && layer->policy != UnknownKeyPolicy::Ignore &&
                layer->policy != UnknownKeyPolicy::Preserve) {
                fail("invalid unknown-key policy", layer->name);
            }
            charge(bytes, sizeof(Layer) + layer->name.size() + layer->file.size(), bounds.max_total_bytes);
            for (const auto& stamp : layer->stamps) {
                charge(bytes, sizeof(Stamp) + stamp.path.size(), bounds.max_total_bytes);
            }
            if (layer->entries.size() > bounds.max_keys) {
                fail("layer entry capacity exceeded", layer->name);
            }
            for (auto entry = layer->entries.begin(); entry != layer->entries.end();) {
                const auto& key = entry->first;
                auto& record = entry->second;
                if (!valid_key(key, bounds.max_key_bytes)) {
                    fail("invalid setting key", record.source, record.line, key);
                }
                const auto schema = next.schemas.find(key);
                if (schema == next.schemas.end() && layer->policy != UnknownKeyPolicy::Preserve) {
                    if (layer->policy == UnknownKeyPolicy::Reject) {
                        fail("unknown setting key", record.source, record.line, key);
                    }
                    result.issues.push_back({record.source, record.line, key, "ignored unknown key", true});
                    entry = layer->entries.erase(entry);
                    continue;
                }
                if (record.kernel_encoded && schema != next.schemas.end()) {
                    if (setting_type(schema->second.default_value) != SettingType::Text) {
                        const auto& raw = std::get<std::string>(record.value);
                        auto parsed = parse_setting(raw, bounds.max_text_bytes);
                        if (setting_type(schema->second.default_value) == SettingType::Boolean) {
                            if (raw == "1" || raw == "on") { parsed = SettingValue{true}; }
                            if (raw == "0" || raw == "off") { parsed = SettingValue{false}; }
                        }
                        if (!parsed) {
                            fail("Kernel value cannot be parsed for schema", record.source, record.line, key);
                        }
                        record.value = std::move(*parsed);
                    }
                    record.kernel_encoded = false;
                }
                validate_value(record.value, schema == next.schemas.end() ? nullptr : &schema->second, key, &record);
                charge(bytes, sizeof(Entry) + key.size() + record.source.size(), bounds.max_total_bytes);
                charge(bytes, value_bytes(record.value), bounds.max_total_bytes);
                if (layer->enabled) {
                    const std::string target = schema == next.schemas.end() || schema->second.kernel_key.empty()
                        ? key : schema->second.kernel_key;
                    next.effective.insert_or_assign(key, EffectiveSetting{record.value,
                        {layer->name, record.source, record.line, layer->priority, layer->order}, target});
                }
                ++entry;
            }
        }
        if (next.effective.size() > bounds.max_keys) {
            fail("effective setting capacity exceeded");
        }
        next.owned_kernel_keys.clear();
        for (const auto& [key, entry] : next.effective) {
            charge(bytes, sizeof(EffectiveSetting) + key.size() + entry.kernel_key.size() +
                   entry.origin.layer.size() + entry.origin.source.size(), bounds.max_total_bytes);
            charge(bytes, value_bytes(entry.value), bounds.max_total_bytes);
            if (!next.owned_kernel_keys.insert(entry.kernel_key).second) {
                fail("two setting keys bind the same Kernel key", {}, 0, key);
            }
        }
    }

    template<class Edit>
    SettingsResult mutate(std::string reason, Edit&& edit) {
        SettingsResult result;
        result.revision = state->revision;
        try {
            auto next = std::make_unique<State>(*state);
            edit(*next);
            resolve(*next, result);
            if (state->revision == std::numeric_limits<std::uint64_t>::max()) {
                fail("settings revision exhausted");
            }
            next->revision = state->revision + 1;
            core::Configuration bridge = kernel.config();
            for (const auto& key : state->owned_kernel_keys) {
                static_cast<void>(bridge.erase(key));
            }
            for (const auto& [key, entry] : next->effective) {
                if (!bridge.set(entry.kernel_key, kernel_text(entry.value))) {
                    fail("Kernel configuration refused the prepared value", {}, 0, key);
                }
            }
            auto event = std::make_shared<SettingsChanged>();
            event->revision = next->revision;
            event->reason = std::move(reason);
            for (const auto& [key, previous] : state->effective) {
                const auto found = next->effective.find(key);
                if (found == next->effective.end()) {
                    event->changes.push_back({key, previous, std::nullopt});
                } else if (previous != found->second) {
                    event->changes.push_back({key, previous, found->second});
                }
            }
            for (const auto& [key, entry] : next->effective) {
                if (!state->effective.contains(key)) {
                    event->changes.push_back({key, std::nullopt, entry});
                }
            }
            std::sort(event->changes.begin(), event->changes.end(), [](const SettingChange& left, const SettingChange& right) {
                return left.key < right.key;
            });
            const auto dynamic_bytes = event_bytes(*event);
            const auto bytes = sizeof(SettingsChanged) + dynamic_bytes;
            auto staged_history = history;
            auto staged_bytes = history_bytes;
            if (bounds.max_history != 0 && bytes <= bounds.max_history_bytes) {
                while (!staged_history.empty() && (staged_history.size() >= bounds.max_history ||
                       bytes > bounds.max_history_bytes - staged_bytes)) {
                    staged_bytes -= staged_history.front().bytes;
                    staged_history.pop_front();
                }
                staged_history.push_back({event, bytes});
                staged_bytes += bytes;
            } else {
                staged_history.clear();
                staged_bytes = 0;
            }
            static_assert(std::is_nothrow_move_assignable_v<core::Configuration>);
            kernel.config() = std::move(bridge);
            state.swap(next);
            history.swap(staged_history);
            history_bytes = staged_bytes;
            result.success = true;
            result.applied = true;
            result.revision = state->revision;
            result.changed = event->changes.size();
            auto* hub = kernel.services().find<core::SignalHub>();
            result.notification = hub ? SettingsNotification::Dropped : SettingsNotification::Unavailable;
            if (hub) {
                try {
                    const auto signal = hub->channel<SettingsChanged>("config.settings.changed");
                    const auto receipt = hub->enqueue_shared(signal,
                        std::shared_ptr<const SettingsChanged>(std::move(event)), dynamic_bytes);
                    result.notification = receipt ? SettingsNotification::Queued : SettingsNotification::Dropped;
                } catch (...) {
                    result.notification = SettingsNotification::Dropped;
                }
            }
            return result;
        } catch (const Failure& error) {
            return rejected(state->revision, error.issue);
        } catch (const io::IoException& error) {
            return rejected(state->revision, {error.path(), 0, {}, error.what(), false});
        } catch (const std::exception& error) {
            return rejected(state->revision, {{}, 0, {}, error.what(), false});
        }
    }

    void parse_document(std::string_view document, const std::string& source,
                        Layer& layer, LoadContext& context) const {
        if (document.size() > bounds.max_document_bytes || !valid_utf8(document)) {
            fail("document exceeds its byte limit or is not UTF-8", source);
        }
        std::string section;
        std::size_t offset{};
        std::size_t line_number{};
        while (offset < document.size()) {
            ++line_number;
            const auto newline = document.find('\n', offset);
            const auto end = newline == std::string_view::npos ? document.size() : newline;
            auto line = document.substr(offset, end - offset);
            offset = newline == std::string_view::npos ? document.size() : newline + 1;
            if (line.size() > bounds.max_line_bytes) {
                fail("line byte limit exceeded", source, line_number);
            }
            bool quoted{};
            bool escaped{};
            for (std::size_t index = 0; index < line.size(); ++index) {
                const char character = line[index];
                if (!quoted && character == '#') {
                    line = line.substr(0, index);
                    break;
                }
                if (quoted && !escaped && character == '\\') {
                    escaped = true;
                } else {
                    if (!escaped && character == '"') {
                        quoted = !quoted;
                    }
                    escaped = false;
                }
            }
            line = trim(line);
            if (line.empty() || (line_number == 1 && line == "GECONFIG1")) {
                continue;
            }
            if (line.starts_with("@include ") || line.starts_with("@include\t")) {
                if (!context.filesystem) {
                    fail("includes require a VFS load", source, line_number);
                }
                const auto relative = quoted_text(trim(line.substr(8)), 4096);
                if (!relative || relative->empty() || relative->front() == '/') {
                    fail("include requires a quoted relative VFS path", source, line_number);
                }
                const auto path = io::join_virtual_path(io::virtual_parent(source), *relative);
                read_document(path, layer, context);
                continue;
            }
            if (line.front() == '[') {
                if (line.back() != ']') {
                    fail("section requires a closing bracket", source, line_number);
                }
                const auto name = trim(line.substr(1, line.size() - 2));
                if (!valid_key(name, bounds.max_key_bytes)) {
                    fail("invalid section name", source, line_number);
                }
                section = name;
                continue;
            }
            const auto equals = line.find('=');
            if (equals == std::string_view::npos) {
                fail("expected key = literal", source, line_number);
            }
            const auto local_key = trim(line.substr(0, equals));
            if (!valid_key(local_key, bounds.max_key_bytes)) {
                fail("invalid setting key", source, line_number);
            }
            std::string key = section.empty() ? std::string(local_key) : section + "." + std::string(local_key);
            if (!valid_key(key, bounds.max_key_bytes)) {
                fail("combined section and key exceeds the key limit", source, line_number);
            }
            auto value = parse_setting(line.substr(equals + 1), bounds.max_text_bytes);
            if (!value) {
                fail("invalid scalar literal", source, line_number, key);
            }
            if (!layer.entries.contains(key) && layer.entries.size() >= bounds.max_keys) {
                fail("layer entry capacity exceeded", source, line_number, key);
            }
            const auto previous = layer.entries.find(key);
            if (previous != layer.entries.end()) {
                context.entry_bytes -= sizeof(Entry) + previous->first.size() +
                    previous->second.source.size() + value_bytes(previous->second.value);
            }
            charge(context.entry_bytes, sizeof(Entry) + key.size() + source.size() + value_bytes(*value), bounds.max_total_bytes);
            layer.entries.insert_or_assign(std::move(key), Entry{std::move(*value), source, line_number});
        }
    }

    void read_document(const std::string& path, Layer& layer, LoadContext& context) const {
        if (context.active.size() >= bounds.max_include_depth || context.stamps.size() >= bounds.max_include_files) {
            fail("include depth or file count exceeded", path);
        }
        if (std::find(context.active.begin(), context.active.end(), path) != context.active.end()) {
            fail("include cycle detected", path);
        }
        auto cursor = context.filesystem->open_read(path);
        if (cursor.size() > static_cast<std::uint64_t>(bounds.max_document_bytes)) {
            fail("file byte limit exceeded", path);
        }
        const auto size = static_cast<std::size_t>(cursor.size());
        charge(context.total_bytes, size, bounds.max_total_bytes);
        const auto key = io::metadata_cache_key(cursor.metadata());
        context.stamps.push_back({path, key});
        context.active.push_back(path);
        std::string document(size, '\0');
        const auto read = cursor.read(std::as_writable_bytes(std::span<char>(document.data(), document.size())));
        if (read != size) {
            fail("short configuration read", path);
        }
        parse_document(document, path, layer, context);
        context.active.pop_back();
        if (context.filesystem->cache_key(path) != key) {
            fail("configuration changed during reading", path);
        }
    }

    io::VirtualFileSystem& filesystem() const {
        auto* service = kernel.services().find<io::VirtualFileSystem>();
        if (!service) {
            fail("VirtualFileSystem service is unavailable");
        }
        return *service;
    }

    template<class Entries>
    SettingsResult save(const Entries& entries, std::string_view path,
                        std::optional<std::uint64_t> expected) const {
        try {
            std::string document = "GECONFIG1\n";
            if (document.size() > bounds.max_document_bytes) {
                fail("document budget cannot contain the format header", std::string(path));
            }
            if (document.size() - 1 > bounds.max_line_bytes) {
                fail("line budget cannot contain the format header", std::string(path));
            }
            for (const auto& [key, entry] : entries) {
                const std::string line = key + " = " + format_setting(entry.value) + "\n";
                if (line.size() - 1 > bounds.max_line_bytes || document.size() > bounds.max_document_bytes ||
                    line.size() > bounds.max_document_bytes - document.size()) {
                    fail("serialized configuration exceeds document or line limits", std::string(path), 0, key);
                }
                document += line;
            }
            io::WriteOptions options;
            options.create_parents = true;
            options.expected_cache_key = expected;
            filesystem().write_text(path, document, options);
            SettingsResult result;
            result.success = true;
            result.revision = state->revision;
            return result;
        } catch (const Failure& error) {
            return rejected(state->revision, error.issue);
        } catch (const io::IoException& error) {
            return rejected(state->revision, {error.path(), 0, {}, error.what(), false});
        } catch (const std::exception& error) {
            return rejected(state->revision, {std::string(path), 0, {}, error.what(), false});
        }
    }
};

Settings::Settings(core::Kernel& kernel, SettingsLimits limits)
    : impl_(std::make_unique<Impl>(kernel, limits)) {}
Settings::~Settings() = default;

SettingsResult Settings::define(SettingSchema schema) {
    return define(std::span<const SettingSchema>(&schema, 1));
}

SettingsResult Settings::define(std::span<const SettingSchema> schemas) {
    return impl_->mutate("schema", [&](State& state) {
        if (schemas.size() > impl_->bounds.max_keys) {
            fail("schema batch capacity exceeded");
        }
        std::set<std::string, std::less<>> keys;
        std::size_t bytes{};
        for (const auto& schema : schemas) {
            if (schema.choices.size() > impl_->bounds.max_choices) {
                fail("schema choice capacity exceeded", "<schema>", 0, schema.key);
            }
            charge(bytes, sizeof(SettingSchema) + schema.key.size() + schema.kernel_key.size() +
                schema.description.size() + value_bytes(schema.default_value), impl_->bounds.max_total_bytes);
            for (const auto& choice : schema.choices) {
                charge(bytes, sizeof(std::string) + choice.size(), impl_->bounds.max_total_bytes);
            }
            if (!keys.insert(schema.key).second) {
                fail("duplicate key in schema batch", "<schema>", 0, schema.key);
            }
            if (!schema.kernel_key.empty() && schema.kernel_key != schema.key) {
                for (auto& layer : state.layers) {
                    const auto imported = layer.entries.find(schema.kernel_key);
                    if (imported != layer.entries.end() && imported->second.kernel_encoded) {
                        if (layer.entries.contains(schema.key)) {
                            fail("Kernel import alias conflicts with an existing override", layer.name, 0, schema.key);
                        }
                        auto entry = layer.entries.extract(imported);
                        entry.key() = schema.key;
                        layer.entries.insert(std::move(entry));
                    }
                }
            }
            state.schemas.insert_or_assign(schema.key, schema);
        }
    });
}

SettingsResult Settings::add_layer(std::string name, int priority, bool enabled) {
    return impl_->mutate("layer.add", [&](State& state) {
        if (!valid_key(name, std::min<std::size_t>(128, impl_->bounds.max_key_bytes)) ||
            state.layers.size() >= impl_->bounds.max_layers || state.next_order == std::numeric_limits<std::uint64_t>::max()) {
            fail("invalid layer name or layer capacity exhausted");
        }
        if (std::any_of(state.layers.begin(), state.layers.end(), [&](const Layer& layer) { return layer.name == name; })) {
            fail("layer already exists", name);
        }
        Layer layer;
        layer.name = std::move(name);
        layer.priority = priority;
        layer.order = state.next_order++;
        layer.enabled = enabled;
        state.layers.push_back(std::move(layer));
    });
}

SettingsResult Settings::remove_layer(std::string_view name) {
    return impl_->mutate("layer.remove", [&](State& state) {
        static_cast<void>(layer_named(state, name));
        std::erase_if(state.layers, [name](const Layer& layer) { return layer.name == name; });
    });
}

SettingsResult Settings::configure_layer(std::string_view name, int priority, bool enabled) {
    return impl_->mutate("layer.configure", [&](State& state) {
        auto& layer = layer_named(state, name);
        layer.priority = priority;
        layer.enabled = enabled;
    });
}

SettingsResult Settings::set(std::string_view name, std::string key, SettingValue value) {
    return impl_->mutate("value.set", [&](State& state) {
        auto& layer = layer_named(state, name);
        layer.entries.insert_or_assign(std::move(key), Entry{std::move(value), "<runtime>", 0});
        layer.dirty = true;
    });
}

SettingsResult Settings::erase(std::string_view name, std::string_view key) {
    return impl_->mutate("value.erase", [&](State& state) {
        auto& layer = layer_named(state, name);
        const auto found = layer.entries.find(key);
        if (found == layer.entries.end()) {
            fail("key has no override in this layer", layer.name, 0, std::string(key));
        }
        layer.entries.erase(found);
        layer.dirty = true;
    });
}

SettingsResult Settings::replace_layer(std::string_view name, std::string_view document,
                                      UnknownKeyPolicy policy, std::string source) {
    return impl_->mutate("layer.replace", [&](State& state) {
        if (source.size() > 4096 || !valid_utf8(source)) {
            fail("invalid source label");
        }
        auto& layer = layer_named(state, name);
        layer.entries.clear();
        layer.policy = policy;
        layer.file.clear();
        layer.stamps.clear();
        layer.watched = false;
        layer.dirty = true;
        Impl::LoadContext context;
        impl_->parse_document(document, source, layer, context);
    });
}

SettingsResult Settings::import_configuration(std::string_view name,
    const core::Configuration& configuration, UnknownKeyPolicy policy) {
    return impl_->mutate("kernel.import", [&](State& state) {
        auto& layer = layer_named(state, name);
        layer.entries.clear();
        layer.policy = policy;
        layer.file.clear();
        layer.stamps.clear();
        layer.watched = false;
        layer.dirty = true;
        std::map<std::string, const SettingSchema*, std::less<>> targets;
        for (const auto& [key, schema] : state.schemas) {
            targets.emplace(schema.kernel_key.empty() ? key : schema.kernel_key, &schema);
        }
        std::size_t bytes{};
        for (const auto& [key, raw] : configuration.values()) {
            const auto schema = targets.find(key);
            const auto& setting_key = schema == targets.end() ? key : schema->second->key;
            charge(bytes, sizeof(Entry) + setting_key.size() + raw.size(), impl_->bounds.max_total_bytes);
            if (!layer.entries.emplace(setting_key, Entry{raw, "<kernel>", 0, true}).second) {
                fail("Kernel import keys map to the same setting", "<kernel>", 0, setting_key);
            }
        }
    });
}

SettingsResult Settings::load(std::string_view name, std::string_view path, UnknownKeyPolicy policy) {
    return impl_->mutate("file.load", [&](State& state) {
        auto& layer = layer_named(state, name);
        layer.file = io::normalize_virtual_path(path);
        layer.entries.clear();
        layer.policy = policy;
        layer.dirty = false;
        Impl::LoadContext context;
        context.filesystem = &impl_->filesystem();
        impl_->read_document(layer.file, layer, context);
        for (const auto& stamp : context.stamps) {
            if (context.filesystem->cache_key(stamp.path) != stamp.key) {
                fail("configuration dependency changed before publication", stamp.path);
            }
        }
        layer.stamps = std::move(context.stamps);
    });
}

SettingsResult Settings::reload(std::string_view name) {
    try {
        const auto& layer = layer_named(*impl_->state, name);
        if (layer.file.empty()) {
            fail("layer is not associated with a file", layer.name);
        }
        const std::string path = layer.file;
        return load(name, path, layer.policy);
    } catch (const Failure& error) {
        return rejected(revision(), error.issue);
    }
}

SettingsResult Settings::save_layer(std::string_view name, std::string_view path,
                                   std::optional<std::uint64_t> expected_cache_key) {
    try {
        const auto& layer = layer_named(*impl_->state, name);
        return impl_->save(layer.entries, path, expected_cache_key);
    } catch (const Failure& error) {
        return rejected(revision(), error.issue);
    }
}

SettingsResult Settings::save_effective(std::string_view path,
                                       std::optional<std::uint64_t> expected_cache_key) {
    return impl_->save(impl_->state->effective, path, expected_cache_key);
}

SettingsResult Settings::apply() {
    return impl_->mutate("kernel.apply", [](State&) {});
}

bool Settings::watch(std::string_view name, bool enabled) {
    const auto found = std::find_if(impl_->state->layers.begin(), impl_->state->layers.end(),
        [name](const Layer& layer) { return layer.name == name; });
    if (found == impl_->state->layers.end() || (enabled && found->file.empty())) {
        return false;
    }
    found->watched = enabled;
    return true;
}

std::vector<SettingsResult> Settings::poll_reloads(std::size_t budget) {
    std::vector<SettingsResult> results;
    const auto count = impl_->state->layers.size();
    if (count == 0 || budget == 0) {
        return results;
    }
    auto* filesystem = impl_->kernel.services().find<io::VirtualFileSystem>();
    if (!filesystem) {
        return results;
    }
    std::size_t checked{};
    for (std::size_t scanned = 0; scanned < count && checked < budget; ++scanned) {
        impl_->poll_cursor %= count;
        auto& layer = impl_->state->layers[impl_->poll_cursor++];
        if (!layer.watched || layer.dirty || layer.file.empty()) {
            continue;
        }
        ++checked;
        const std::string name = layer.name;
        auto observed = layer.stamps;
        bool changed{};
        try {
            for (auto& stamp : observed) {
                const auto metadata = filesystem->metadata(stamp.path);
                const auto key = metadata ? io::metadata_cache_key(*metadata) : 0;
                changed = changed || key != stamp.key;
                stamp.key = key;
            }
        } catch (const std::exception& error) {
            results.push_back(rejected(revision(), {layer.file, 0, {}, error.what(), false}));
            continue;
        }
        if (changed) {
            auto result = reload(name);
            if (!result.success) {
                auto& retained = layer_named(*impl_->state, name);
                retained.stamps = std::move(observed);
                for (const auto& issue : result.issues) {
                    if (retained.stamps.size() < impl_->bounds.max_include_files && issue.source.starts_with('/') &&
                        std::none_of(retained.stamps.begin(), retained.stamps.end(), [&](const Stamp& stamp) {
                            return stamp.path == issue.source;
                        })) {
                        retained.stamps.push_back({issue.source, 0});
                    }
                }
            }
            results.push_back(std::move(result));
        }
    }
    return results;
}

std::optional<EffectiveSetting> Settings::find(std::string_view key) const {
    const auto found = impl_->state->effective.find(key);
    return found == impl_->state->effective.end() ? std::nullopt : std::optional<EffectiveSetting>{found->second};
}

std::string Settings::text(std::string_view key, std::string_view fallback) const {
    const auto value = find(key);
    const auto* text_value = value ? std::get_if<std::string>(&value->value) : nullptr;
    return text_value ? *text_value : std::string(fallback);
}

bool Settings::boolean(std::string_view key, bool fallback) const {
    const auto value = find(key);
    const auto* boolean_value = value ? std::get_if<bool>(&value->value) : nullptr;
    return boolean_value ? *boolean_value : fallback;
}

std::int64_t Settings::integer(std::string_view key, std::int64_t fallback) const {
    const auto value = find(key);
    const auto* integer_value = value ? std::get_if<std::int64_t>(&value->value) : nullptr;
    return integer_value ? *integer_value : fallback;
}

double Settings::number(std::string_view key, double fallback) const {
    const auto value = find(key);
    const auto* number_value = value ? std::get_if<double>(&value->value) : nullptr;
    return number_value ? *number_value : fallback;
}

std::map<std::string, EffectiveSetting, std::less<>> Settings::snapshot() const {
    return impl_->state->effective;
}

std::vector<SettingSchema> Settings::schemas() const {
    std::vector<SettingSchema> result;
    result.reserve(impl_->state->schemas.size());
    for (const auto& [key, schema] : impl_->state->schemas) {
        static_cast<void>(key);
        result.push_back(schema);
    }
    return result;
}

std::vector<SettingsLayerInfo> Settings::layers() const {
    std::vector<SettingsLayerInfo> result;
    for (const auto& layer : impl_->state->layers) {
        result.push_back({layer.name, layer.priority, layer.order, layer.enabled, layer.entries.size(),
                          layer.file, layer.stamps.size(), layer.watched, layer.dirty});
    }
    std::sort(result.begin(), result.end(), [](const SettingsLayerInfo& left, const SettingsLayerInfo& right) {
        return left.priority == right.priority ? left.order < right.order : left.priority < right.priority;
    });
    return result;
}

SettingsHistory Settings::changes_since(std::uint64_t cursor, std::size_t limit) const {
    SettingsHistory result;
    result.current_revision = revision();
    result.gap = cursor > revision() || (cursor < revision() &&
        (impl_->history.empty() || impl_->history.front().event->revision - 1 > cursor));
    limit = std::min(limit, impl_->bounds.max_history);
    for (const auto& record : impl_->history) {
        if (record.event->revision > cursor) {
            if (result.events.size() >= limit) {
                result.truncated = true;
                break;
            }
            result.events.push_back(*record.event);
        }
    }
    return result;
}

std::uint64_t Settings::revision() const noexcept { return impl_->state->revision; }
SettingsLimits Settings::limits() const noexcept { return impl_->bounds; }

core::Signal<SettingsChanged> Settings::changed_signal() {
    auto* hub = impl_->kernel.services().find<core::SignalHub>();
    return hub ? hub->channel<SettingsChanged>("config.settings.changed") : core::Signal<SettingsChanged>{};
}

core::SignalConnection Settings::bind(std::string key,
    std::function<void(const SettingChange&, const core::SignalContext&)> callback,
    core::SignalListenerOptions options) {
    auto* hub = impl_->kernel.services().find<core::SignalHub>();
    if (!hub || !callback || !valid_key(key, impl_->bounds.max_key_bytes)) {
        return {};
    }
    const bool once = options.once;
    options.once = false;
    auto token = std::make_shared<core::SignalSubscription>();
    auto connection = hub->connect(changed_signal(),
        [hub, token, once, key = std::move(key), callback = std::move(callback)](
            const SettingsChanged& event, const core::SignalContext& context) {
            const auto found = std::lower_bound(event.changes.begin(), event.changes.end(), key,
                [](const SettingChange& change, const std::string& target) { return change.key < target; });
            if (found != event.changes.end() && found->key == key) {
                if (once) {
                    static_cast<void>(hub->disconnect(*token));
                }
                callback(*found, context);
            }
        }, std::move(options));
    *token = connection.token();
    return connection;
}

void register_settings(core::Kernel& kernel) {
    if (kernel.services().find<Settings>()) {
        return;
    }
    const core::Configuration startup = kernel.config();
    auto& settings = kernel.services().emplace<Settings>(kernel);
    static_cast<void>(kernel.register_module({"config.settings", "Layered typed configuration and bounded GECONFIG1 persistence", 1}));
    const auto report = [&kernel](const SettingsResult& result) {
        if (!result.success) {
            kernel.diagnose(core::Diagnostic::Severity::Error, "config.settings", result_text(result));
        }
    };
    report(settings.add_layer("startup", 100));
    report(settings.add_layer("runtime", 1000));
    const auto imported = settings.import_configuration("startup", startup);
    report(imported);
    if (imported.success) {
        std::array<SettingSchema, 3> schemas;
        schemas[0].key = "runtime.step";
        schemas[0].default_value = 1.0 / 60.0;
        schemas[0].number_min = 0.000001;
        schemas[0].number_max = 1.0;
        schemas[0].description = "Seconds supplied to the runtime tick loop";
        schemas[1].key = "settings.reload.enabled";
        schemas[1].default_value = true;
        schemas[2].key = "settings.reload.budget";
        schemas[2].default_value = std::int64_t{1};
        schemas[2].integer_min = 1;
        schemas[2].integer_max = 32;
        report(settings.define(schemas));
    }
    const auto command_result = [](SettingsResult result) {
        return core::CommandResult{result.success, result_text(result)};
    };
    static_cast<void>(kernel.add_command("settings.layer", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 3) {
            return core::CommandResult{false, "usage: settings.layer name priority [on|off]"};
        }
        int priority{};
        const auto parsed = std::from_chars(args[1].data(), args[1].data() + args[1].size(), priority);
        if (parsed.ec != std::errc{} || parsed.ptr != args[1].data() + args[1].size() ||
            (args.size() == 3 && args[2] != "on" && args[2] != "off")) {
            return core::CommandResult{false, "invalid priority or enabled flag"};
        }
        auto& service = host.services().get<Settings>();
        const auto layers = service.layers();
        const bool exists = std::any_of(layers.begin(), layers.end(), [&](const SettingsLayerInfo& layer) { return layer.name == args[0]; });
        const bool enabled = args.size() != 3 || args[2] == "on";
        return command_result(exists ? service.configure_layer(args[0], priority, enabled)
                                     : service.add_layer(args[0], priority, enabled));
    }, "Create or configure a layer; higher priority and later creation win"));
    static_cast<void>(kernel.add_command("settings.define", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: settings.define key scalar-default"};
        }
        auto value = parse_setting(args[1]);
        if (!value) {
            return core::CommandResult{false, "default must be a scalar literal; preserve quotes for text"};
        }
        SettingSchema schema;
        schema.key = args[0];
        schema.default_value = std::move(*value);
        return command_result(host.services().get<Settings>().define(std::move(schema)));
    }, "Define a typed default; C++ schema API additionally supplies ranges and choices"));
    static_cast<void>(kernel.add_command("settings.set", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 3) {
            return core::CommandResult{false, "usage: settings.set layer key scalar-literal"};
        }
        auto value = parse_setting(args[2]);
        if (!value) {
            return core::CommandResult{false, "invalid scalar literal; preserve inner quotes for text"};
        }
        return command_result(host.services().get<Settings>().set(args[0], args[1], std::move(*value)));
    }, "Validate and commit a layer override to Settings and Kernel configuration"));
    static_cast<void>(kernel.add_command("settings.erase", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: settings.erase layer key"};
        }
        return command_result(host.services().get<Settings>().erase(args[0], args[1]));
    }, "Remove one override and reveal the next lower layer or default"));
    static_cast<void>(kernel.add_command("settings.remove-layer", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: settings.remove-layer name"};
        }
        return command_result(host.services().get<Settings>().remove_layer(args[0]));
    }, "Remove a named override layer transactionally"));
    static_cast<void>(kernel.add_command("settings.load", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() < 2 || args.size() > 3) {
            return core::CommandResult{false, "usage: settings.load layer virtual-path [reject|ignore|preserve]"};
        }
        UnknownKeyPolicy policy = UnknownKeyPolicy::Reject;
        if (args.size() == 3) {
            if (args[2] == "ignore") { policy = UnknownKeyPolicy::Ignore; }
            else if (args[2] == "preserve") { policy = UnknownKeyPolicy::Preserve; }
            else if (args[2] != "reject") { return core::CommandResult{false, "unknown key policy"}; }
        }
        return command_result(host.services().get<Settings>().load(args[0], args[1], policy));
    }, "Replace a layer from GECONFIG1 and bounded relative includes"));
    static_cast<void>(kernel.add_command("settings.save", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: settings.save layer|effective virtual-path"};
        }
        auto& service = host.services().get<Settings>();
        return command_result(args[0] == "effective" ? service.save_effective(args[1]) : service.save_layer(args[0], args[1]));
    }, "Export flat sorted values through VFS; includes and comments are not retained"));
    static_cast<void>(kernel.add_command("settings.reload", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: settings.reload layer"};
        }
        return command_result(host.services().get<Settings>().reload(args[0]));
    }, "Explicitly reload a file layer, replacing unsaved overrides"));
    static_cast<void>(kernel.add_command("settings.watch", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2 || (args[1] != "on" && args[1] != "off")) {
            return core::CommandResult{false, "usage: settings.watch layer on|off"};
        }
        const bool accepted = host.services().get<Settings>().watch(args[0], args[1] == "on");
        return core::CommandResult{accepted, accepted ? "watch updated; dirty layers require explicit reload" : "unknown or non-file layer"};
    }, "Poll a layer's own file metadata without consuming FileChange events"));
    static_cast<void>(kernel.add_command("settings.get", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 1) {
            return core::CommandResult{false, "usage: settings.get key"};
        }
        const auto value = host.services().get<Settings>().find(args[0]);
        if (!value) {
            return core::CommandResult{false, "setting not found"};
        }
        return core::CommandResult{true, args[0] + " = " + format_setting(value->value) + " layer=" +
            value->origin.layer + " source=" + value->origin.source + ":" + std::to_string(value->origin.line) +
            " kernel=" + value->kernel_key};
    }, "Inspect an effective typed value, winning layer, source line and Kernel binding"));
    static_cast<void>(kernel.add_command("settings.inspect", [](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: settings.inspect"};
        }
        auto& service = host.services().get<Settings>();
        std::string text = "revision=" + std::to_string(service.revision());
        for (const auto& layer : service.layers()) {
            text += "\n" + layer.name + " priority=" + std::to_string(layer.priority) +
                " entries=" + std::to_string(layer.entries) + (layer.enabled ? " enabled" : " disabled") +
                (layer.watched ? " watched" : "") + (layer.dirty ? " dirty" : "") + " file=" + layer.file;
        }
        return core::CommandResult{true, std::move(text)};
    }, "List ordered layers and persistence state"));
    static_cast<void>(kernel.add_command("settings.apply", [command_result](core::Kernel& host, core::CommandArgs args) {
        if (!args.empty()) {
            return core::CommandResult{false, "usage: settings.apply"};
        }
        return command_result(host.services().get<Settings>().apply());
    }, "Reapply authoritative settings values to actual Kernel configuration"));
    static_cast<void>(kernel.add_system({"config.settings.reload", core::Phase::PreSimulation, -9900, {}},
        [](core::Kernel& host, const core::Frame&) {
            if (!host.config().boolean("settings.reload.enabled", true)) {
                return;
            }
            const auto budget = std::clamp<std::int64_t>(host.config().integer("settings.reload.budget", 1), 1, 32);
            auto& service = host.services().get<Settings>();
            for (const auto& result : service.poll_reloads(static_cast<std::size_t>(budget))) {
                if (!result.success || result.notification == SettingsNotification::Dropped) {
                    host.diagnose(core::Diagnostic::Severity::Warning, "config.settings", result_text(result));
                }
            }
            host.metric("settings.revision", static_cast<double>(service.revision()));
        }));
}

} // namespace ge::config
