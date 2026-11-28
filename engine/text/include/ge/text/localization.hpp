#pragma once

#include <ge/core/signals.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ge::core { class Kernel; }

namespace ge::text {

enum class TextError : std::uint8_t {
    InvalidUnicode, InvalidLocale, InvalidKey, InvalidSyntax, InvalidArgument,
    MissingArgument, MissingKey, LimitExceeded, Duplicate, UnsupportedVersion,
    MissingService, SourceChanged, RevisionExhausted
};

class TextException final : public std::runtime_error {
public:
    TextException(TextError code, std::size_t offset, std::string message);
    [[nodiscard]] TextError code() const noexcept;
    // Byte offset for UTF-8 input/patterns; UTF-16 input unit offset for from_utf16.
    // Catalog line syntax uses a one-based line number. Zero may mean unspecified.
    [[nodiscard]] std::size_t offset() const noexcept;
private:
    TextError code_;
    std::size_t offset_;
};

struct UnicodeScalar {
    char32_t value{};
    std::size_t byte_offset{};
    std::size_t byte_width{};
};

// Strict Unicode scalar operations, not grapheme segmentation, normalization,
// shaping, bidi processing, case folding, or display-width measurement.
// A cursor borrows its input and rejects malformed input without advancing.
class Utf8Cursor final {
public:
    explicit Utf8Cursor(std::string_view text) noexcept;
    [[nodiscard]] std::optional<UnicodeScalar> next();
    [[nodiscard]] std::size_t position() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
private:
    std::string_view text_;
    std::size_t position_{};
};

[[nodiscard]] std::size_t scalar_count(std::string_view text);
[[nodiscard]] std::u16string to_utf16(std::string_view text,
    std::size_t maximum_units = 1024U * 1024U);
[[nodiscard]] std::string from_utf16(std::u16string_view text,
    std::size_t maximum_bytes = 4U * 1024U * 1024U);
[[nodiscard]] std::string truncate_utf8(std::string_view text, std::size_t maximum_bytes,
    std::string_view suffix = {});
[[nodiscard]] std::string substring_utf8(std::string_view text, std::size_t first_scalar,
    std::size_t count);

// Deliberate locale-tag subset: 2..8 ASCII language letters, then 1..8 ASCII
// alphanumeric subtags, with '-' separators (or input '_'). Canonicalizes case;
// no registry validation, aliases, private-use, extension semantics or likely tags.
[[nodiscard]] std::string canonical_locale(std::string_view locale);

struct LocalizationLimits {
    std::size_t max_catalogs{64};
    std::size_t max_entries{32768};
    std::size_t max_key_bytes{256};
    std::size_t max_pattern_bytes{16384};
    std::size_t max_catalog_bytes{8U * 1024U * 1024U};
    std::size_t max_total_bytes{32U * 1024U * 1024U};
    std::size_t max_document_bytes{16U * 1024U * 1024U};
    std::size_t max_fallbacks{16};
    std::size_t max_depth{16};
    std::size_t max_nodes{4096};
    std::size_t max_arguments{128};
    std::size_t max_output_bytes{1024U * 1024U};
    std::size_t max_history{128};
};

using FormatValue = std::variant<std::string, std::int64_t, double, bool>;
struct FormatArgument {
    std::string name;
    FormatValue value;
};

struct NumberOptions {
    unsigned minimum_fraction_digits{};
    unsigned maximum_fraction_digits{3};
    bool grouping{true};
};

// Finite decimal formatting uses ASCII digits, fixed notation and 0..9 fraction
// digits. en/default: ',' grouping and '.' decimal; de/es/it/pt: '.' and ',';
// fr: narrow NBSP and ','; ru: NBSP and ','. Other locales use default separators.
[[nodiscard]] std::string format_number(const FormatValue& value, std::string_view locale,
    NumberOptions options = {}, std::size_t maximum_bytes = 4096);

// Patterns: {name}, {n,number}, {n,plural,one{...} other{...}},
// {kind,select,label{...} other{...}}. Plural accepts exact signed integer =N
// branches; '#' uses the nearest enclosing plural. Escape braces, '#' and '\\'
// with '\\'. No implicit evaluation of inserted argument text. Every choice
// needs 'other'. Rules: en/de/es/it one iff integer abs(n)==1; fr integer 0/1;
// ru/uk one/few/many by integer mod 10/100; ja/zh/ko and unknown: other.
// Integer rules require int64 or an in-range integral double; other numbers use
// other. Branch selection uses the supplied value before display rounding.
// Names/choice labels use ASCII letters, digits, '_', '-', '.', '/', ':'. Boolean
// values insert literal true/false and select matching true/false branches.
// This is an explicit small rule set, not full ICU/CLDR MessageFormat.
[[nodiscard]] std::string format_message(std::string_view pattern,
    std::span<const FormatArgument> arguments = {}, std::string_view locale = "en",
    LocalizationLimits limits = {});

struct CatalogDefinition {
    std::string locale;
    std::uint64_t version{1};
    std::vector<std::string> fallbacks;
    std::map<std::string, std::string, std::less<>> entries;
};

// Immutable compiled catalog. Snapshots retained by callers survive replacement;
// Limits charge content and compiled node storage, excluding allocator overhead
// and caller-retained snapshots. A retained find() view requires its Catalog alive.
class Catalog final {
public:
    explicit Catalog(CatalogDefinition definition, LocalizationLimits limits = {});
    ~Catalog();
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    [[nodiscard]] const CatalogDefinition& definition() const noexcept;
    [[nodiscard]] std::optional<std::string_view> find(std::string_view key) const;
    [[nodiscard]] std::string format(std::string_view key,
        std::span<const FormatArgument> arguments = {}) const;
    [[nodiscard]] std::size_t accounted_bytes() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// GELOC1 <locale> <positive-content-version> header; @fallback <locale> lines;
// then "key" = "pattern" lines, with # comments outside quoted strings.
// Strings escape \", \\, \n, \r and \t; all other Unicode is literal UTF-8.
// Duplicate keys/fallbacks, unknown directives and malformed patterns are errors.
// Keys use the same ASCII identifier alphabet as message names. Writer sorts keys.
// No includes or host filesystem access.
[[nodiscard]] std::shared_ptr<const Catalog> parse_catalog(std::string_view document,
    LocalizationLimits limits = {});
[[nodiscard]] std::string write_catalog(const Catalog& catalog,
    std::size_t maximum_bytes = 16U * 1024U * 1024U);

enum class MissingKeyPolicy : std::uint8_t { ReturnKey, Empty, Marker, Error };
struct LookupResult {
    bool found{};
    std::string text;
    std::string requested_locale;
    std::string resolved_locale;
    std::uint64_t catalog_version{};
    std::uint64_t service_revision{};
};

struct LocalizationResult {
    bool success{};
    bool changed{};
    std::uint64_t revision{};
    std::string message;
};

struct CatalogInfo {
    std::string locale;
    std::uint64_t version{};
    std::size_t entries{};
    std::size_t accounted_bytes{};
    std::string source;
    std::uint64_t source_cache_key{};
};

struct LocalizationChanged {
    std::uint64_t revision{};
    std::string locale;
    std::string affected_catalog;
    std::string reason;
};

struct LocalizationHistory {
    std::uint64_t current_revision{};
    bool gap{};
    bool truncated{};
    std::vector<LocalizationChanged> events;
};

// Single writer on the Kernel thread. Registration binds Settings key text.locale
// (default en) and writes explicit use_locale requests into text.localization.
// Lookup visits requested locale, explicit fallbacks depth-first, parent tags,
// then default locale; each tag appears once, so cycles terminate deterministically.
// Rejected reloads preserve the previous catalog. Reload is explicit; the service
// never consumes the shared FileChange queue. Destruction touches no other service.
// max_fallbacks also bounds a resolved chain; exceeding it throws TextException.
// Versions are authored identifiers, not monotonic update guards. A successful
// replacement advances the service revision even if the authored version repeats.
// Signals are best-effort after commit; consumers recover through revision/history.
// VFS cache-key checks are optimistic metadata checks; native files are not
// snapshots. Settings commits and VFS writes are separate effects from service state.
class LocalizationService final {
public:
    explicit LocalizationService(core::Kernel& kernel, LocalizationLimits limits = {});
    ~LocalizationService();
    LocalizationService(const LocalizationService&) = delete;
    LocalizationService& operator=(const LocalizationService&) = delete;
    LocalizationResult install(CatalogDefinition definition);
    LocalizationResult replace(std::string_view document);
    LocalizationResult load(std::string_view path);
    LocalizationResult reload(std::string_view locale);
    LocalizationResult save(std::string_view locale, std::string_view path,
        std::optional<std::uint64_t> expected_cache_key = std::nullopt);
    LocalizationResult remove(std::string_view locale);
    LocalizationResult use_locale(std::string_view locale);
    LocalizationResult set_default_locale(std::string_view locale);
    LocalizationResult synchronize_settings();
    void disconnect_settings() noexcept;
    [[nodiscard]] LookupResult lookup(std::string_view key,
        std::string_view locale = {}, MissingKeyPolicy policy = MissingKeyPolicy::ReturnKey) const;
    [[nodiscard]] LookupResult format(std::string_view key,
        std::span<const FormatArgument> arguments = {}, std::string_view locale = {},
        MissingKeyPolicy policy = MissingKeyPolicy::ReturnKey) const;
    [[nodiscard]] std::vector<std::string> fallback_chain(std::string_view locale = {}) const;
    [[nodiscard]] std::shared_ptr<const Catalog> catalog(std::string_view locale) const;
    [[nodiscard]] std::vector<CatalogInfo> catalogs() const;
    [[nodiscard]] LocalizationHistory changes_since(std::uint64_t revision,
        std::size_t limit = 128) const;
    [[nodiscard]] core::Signal<LocalizationChanged> changed_signal();
    [[nodiscard]] std::string_view locale() const noexcept;
    [[nodiscard]] std::string_view default_locale() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] LocalizationLimits limits() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_localization(core::Kernel& kernel);

} // namespace ge::text
