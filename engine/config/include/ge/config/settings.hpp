#pragma once

#include <ge/core/signals.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ge::core { class Configuration; class Kernel; }

namespace ge::config {

using SettingValue = std::variant<bool, std::int64_t, double, std::string>;
enum class SettingType : std::uint8_t { Boolean, Integer, Number, Text };
enum class UnknownKeyPolicy : std::uint8_t { Reject, Ignore, Preserve };
enum class SettingsNotification : std::uint8_t { None, Queued, Dropped, Unavailable };

struct SettingsLimits {
    std::size_t max_keys{4096};
    std::size_t max_layers{32};
    std::size_t max_key_bytes{256};
    std::size_t max_text_bytes{65536};
    std::size_t max_document_bytes{1024U * 1024U};
    std::size_t max_total_bytes{8U * 1024U * 1024U};
    std::size_t max_line_bytes{65536};
    std::size_t max_include_files{32};
    std::size_t max_include_depth{8};
    std::size_t max_choices{128};
    std::size_t max_history{64};
    std::size_t max_history_bytes{16U * 1024U * 1024U};
};

struct SettingSchema {
    std::string key;
    SettingValue default_value{false};
    // Empty means the same key in Kernel::config(). Aliases must be unique.
    std::string kernel_key;
    std::optional<std::int64_t> integer_min;
    std::optional<std::int64_t> integer_max;
    std::optional<double> number_min;
    std::optional<double> number_max;
    std::size_t max_text_bytes{65536};
    std::vector<std::string> choices;
    std::string description;
};

struct SettingOrigin {
    std::string layer;
    std::string source;
    std::size_t line{};
    int priority{};
    std::uint64_t layer_order{};
    bool operator==(const SettingOrigin&) const = default;
};

struct EffectiveSetting {
    SettingValue value;
    SettingOrigin origin;
    std::string kernel_key;
    bool operator==(const EffectiveSetting&) const = default;
};

struct SettingChange {
    std::string key;
    std::optional<EffectiveSetting> before;
    std::optional<EffectiveSetting> after;
};

struct SettingsChanged {
    std::uint64_t revision{};
    std::string reason;
    std::vector<SettingChange> changes;
};

struct SettingsIssue {
    std::string source;
    std::size_t line{};
    std::string key;
    std::string message;
    bool warning{};
};

struct SettingsResult {
    bool success{};
    // True means settings and Kernel configuration committed. Notification
    // admission is separate; listener execution is deferred to SignalHub.
    bool applied{};
    std::uint64_t revision{};
    std::size_t changed{};
    SettingsNotification notification{SettingsNotification::None};
    std::vector<SettingsIssue> issues;
};

struct SettingsLayerInfo {
    std::string name;
    int priority{};
    std::uint64_t order{};
    bool enabled{};
    std::size_t entries{};
    std::string file;
    std::size_t dependencies{};
    bool watched{};
    bool dirty{};
};

struct SettingsHistory {
    std::uint64_t current_revision{};
    bool gap{};
    bool truncated{};
    std::vector<SettingsChanged> events;
};

[[nodiscard]] SettingType setting_type(const SettingValue& value) noexcept;
[[nodiscard]] std::string_view setting_type_name(SettingType type) noexcept;
[[nodiscard]] std::string format_setting(const SettingValue& value);
[[nodiscard]] std::optional<SettingValue> parse_setting(
    std::string_view literal, std::size_t maximum_text_bytes = 65536);

// GECONFIG1 is a deliberately small, case-sensitive UTF-8 line format:
// optional first-line "GECONFIG1"; key = true|false|integer|finite-number|"text";
// [section.name] prefixes following keys; # starts a comment outside strings.
// Keys are dot-separated ASCII segments beginning with a letter or underscore,
// followed by letters, digits, underscores or hyphens. Numbers are decimal;
// a decimal point or exponent selects double, otherwise signed 64-bit integer.
// Quoted text supports \", \\, \n, \r, \t and \uXXXX (including surrogate pairs).
// @include "relative/path" inserts a separate document at that position. Each
// included document starts in the root section. Repeated keys use the last
// definition. Includes require VFS load; text replacement rejects directives.
// The writer emits flat sorted keys with the header and no includes. No TOML,
// YAML, environment interpolation, arbitrary expressions or multiline strings.
//
// Single writer on the Kernel thread. Every mutation prepares private state
// and a Configuration copy before publication. Existing unrelated Kernel keys
// survive. Every successful mutation reapplies owned keys; apply() explicitly
// resynchronizes without editing layers. Larger priorities win, and later
// layer creation breaks ties; defaults are always below every enabled layer.
// Filesystem writes, SignalHub admission and downstream callbacks are separate
// effects; there is no cross-service transaction or durability guarantee.
class Settings final {
public:
    explicit Settings(core::Kernel& kernel, SettingsLimits limits = {});
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    Settings(Settings&&) = delete;
    Settings& operator=(Settings&&) = delete;

    SettingsResult define(SettingSchema schema);
    SettingsResult define(std::span<const SettingSchema> schemas);
    SettingsResult add_layer(std::string name, int priority, bool enabled = true);
    SettingsResult remove_layer(std::string_view name);
    SettingsResult configure_layer(std::string_view name, int priority, bool enabled);
    SettingsResult set(std::string_view layer, std::string key, SettingValue value);
    SettingsResult erase(std::string_view layer, std::string_view key);
    SettingsResult replace_layer(std::string_view layer, std::string_view document,
        UnknownKeyPolicy policy = UnknownKeyPolicy::Reject,
        std::string source = "<memory>");
    // Rejects conflicting input keys that map to the same setting through aliases.
    SettingsResult import_configuration(std::string_view layer,
        const core::Configuration& configuration,
        UnknownKeyPolicy policy = UnknownKeyPolicy::Preserve);
    SettingsResult load(std::string_view layer, std::string_view path,
        UnknownKeyPolicy policy = UnknownKeyPolicy::Reject);
    SettingsResult reload(std::string_view layer);
    // Saves export a snapshot without rebinding its source or clearing dirty.
    // Automatic polling skips dirty layers; explicit reload replaces edits.
    SettingsResult save_layer(std::string_view layer, std::string_view path,
        std::optional<std::uint64_t> expected_cache_key = std::nullopt);
    SettingsResult save_effective(std::string_view path,
        std::optional<std::uint64_t> expected_cache_key = std::nullopt);
    SettingsResult apply();
    bool watch(std::string_view layer, bool enabled);
    [[nodiscard]] std::vector<SettingsResult> poll_reloads(std::size_t budget = 1);

    [[nodiscard]] std::optional<EffectiveSetting> find(std::string_view key) const;
    [[nodiscard]] std::string text(std::string_view key, std::string_view fallback = {}) const;
    [[nodiscard]] bool boolean(std::string_view key, bool fallback = false) const;
    [[nodiscard]] std::int64_t integer(std::string_view key, std::int64_t fallback = 0) const;
    [[nodiscard]] double number(std::string_view key, double fallback = 0.0) const;
    [[nodiscard]] std::map<std::string, EffectiveSetting, std::less<>> snapshot() const;
    [[nodiscard]] std::vector<SettingSchema> schemas() const;
    [[nodiscard]] std::vector<SettingsLayerInfo> layers() const;
    [[nodiscard]] SettingsHistory changes_since(std::uint64_t revision,
                                               std::size_t limit = 64) const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] SettingsLimits limits() const noexcept;
    [[nodiscard]] core::Signal<SettingsChanged> changed_signal();
    // Retain the returned weak connection in the consumer. Initial application
    // uses find(); consumers detect missed queue admissions through revision or
    // changes_since(). This callback captures no Settings or Kernel pointer.
    [[nodiscard]] core::SignalConnection bind(std::string key,
        std::function<void(const SettingChange&, const core::SignalContext&)> callback,
        core::SignalListenerOptions options = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using SettingsService = Settings;
void register_settings(core::Kernel& kernel);

} // namespace ge::config
