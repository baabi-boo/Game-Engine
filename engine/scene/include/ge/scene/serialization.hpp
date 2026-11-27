#pragma once

#include <ge/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::io { class VirtualFileSystem; }

namespace ge::scene {

inline constexpr std::uint32_t scene_document_version = 1;
inline constexpr std::uint32_t first_scene_extension = 0x100U;

enum class DocumentError : std::uint8_t {
    None,
    Limit,
    Version,
    InvalidHierarchy,
    InvalidIdentity,
    InvalidText,
    InvalidTransform,
    InvalidBounds,
    InvalidSection,
    CorruptArchive,
    Conflict,
    InactiveTransaction,
    MissingNode,
    UnsupportedSection,
    SceneRejected,
    IoFailure,
    AllocationFailure
};

[[nodiscard]] std::string_view describe(DocumentError error) noexcept;

struct DocumentResult {
    DocumentError error{DocumentError::None};
    std::size_t index{no_parent};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == DocumentError::None;
    }
};

struct SceneArchiveLimits {
    std::size_t max_archive_bytes{64U * 1024U * 1024U};
    std::size_t max_section_bytes{32U * 1024U * 1024U};
    std::size_t max_nodes{65536};
    std::size_t max_depth{4096};
    std::size_t max_string_bytes{4096};
    std::size_t max_total_string_bytes{16U * 1024U * 1024U};
    std::size_t max_metadata{128};
    std::size_t max_extensions{256};
};

// Extension IDs >= 0x100 are application-owned. The envelope is understood by
// this module; payload semantics and component attachment remain with its owner.
struct SceneExtension {
    std::uint32_t id{};
    std::uint16_t version{1};
    bool required{};
    std::vector<std::byte> payload;
    bool operator==(const SceneExtension&) const = default;
};

struct SceneDocument {
    std::uint32_t version{scene_document_version};
    std::uint64_t revision{};
    std::string name;
    SceneSnapshot snapshot;
    std::map<std::string, std::string, std::less<>> metadata;
    std::vector<SceneExtension> extensions;
    void swap(SceneDocument& other) noexcept;
};

enum class UnknownSectionPolicy : std::uint8_t {
    PreserveOptional,
    DiscardOptional,
    RejectUnknown
};

struct SceneSectionVersion {
    std::uint32_t id{};
    std::uint16_t version{};
};

struct SceneDecodeOptions {
    UnknownSectionPolicy unknown_sections{UnknownSectionPolicy::PreserveOptional};
    // Exact versions whose payload semantics the caller undertakes to process.
    // This acknowledgement does not install or execute a component codec.
    std::span<const SceneSectionVersion> understood_sections{};
};

[[nodiscard]] DocumentResult validate_scene_document(
    const SceneDocument& document, const SceneArchiveLimits& limits = {});
[[nodiscard]] DocumentResult encode_scene_document(
    const SceneDocument& document, std::vector<std::byte>& destination,
    const SceneArchiveLimits& limits = {});
[[nodiscard]] DocumentResult decode_scene_document(
    std::span<const std::byte> bytes, SceneDocument& destination,
    const SceneArchiveLimits& limits = {}, SceneDecodeOptions options = {});

struct SceneDocumentStatistics {
    std::size_t nodes{};
    std::size_t roots{};
    std::size_t maximum_depth{};
    std::size_t persistent_nodes{};
    std::size_t enabled_nodes{};
    std::size_t bounded_nodes{};
    std::size_t tags{};
    std::size_t string_bytes{};
    std::size_t extension_bytes{};
    std::uint64_t revision{};
};

[[nodiscard]] SceneDocumentStatistics inspect_scene_document(const SceneDocument& document);
[[nodiscard]] std::optional<std::size_t> find_document_node(
    const SceneDocument& document, PersistentId persistent) noexcept;
[[nodiscard]] DocumentResult capture_scene_document(
    Scene& scene, SceneDocument& destination, Entity subtree = {},
    const SceneArchiveLimits& limits = {});

struct SceneImportOptions {
    Entity parent{};
    // False leaves persistent IDs unassigned; Registry still owns runtime allocation.
    bool preserve_persistent_ids{};
    bool allow_unapplied_required_sections{};
};

struct ImportedSceneNode {
    std::size_t document_index{};
    PersistentId persistent{};
    Entity destination{};
};

struct SceneDocumentImport {
    DocumentResult status;
    CloneResult scene_result;
    std::vector<ImportedSceneNode> nodes;
    std::vector<std::uint32_t> unapplied_sections;
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(status);
    }
};

// Additive import into the existing Registry. Scene rolls back newly allocated
// entities on failure; existing entities are never replaced. Registry revisions,
// events and handle generations may still advance. Scene::update may synchronize
// previously pending registry edits. No arbitrary component payload is restored.
// Runtime source handles are intentionally absent from files; use nodes to map
// document indices/identities to imported live entities after a file load.
[[nodiscard]] SceneDocumentImport import_scene_document(
    const SceneDocument& document, Scene& scene, SceneImportOptions options = {},
    const SceneArchiveLimits& limits = {});

enum class SceneNodeChangeKind : std::uint8_t { Added, Removed, Modified, Moved };

struct SceneNodeChange {
    SceneNodeChangeKind kind{SceneNodeChangeKind::Modified};
    PersistentId persistent{};
    std::size_t before_index{no_parent};
    std::size_t after_index{no_parent};
};

// An exact precondition, not merely a hash: applying compares canonical bytes.
// The replacement is decoded and validated before publication. Changes are an
// inspection summary; the complete documents retain opaque extension payloads.
// Anonymous nodes are compared by ordinal, and moved indices are reported
// conservatively. Applying drops transient source handles, as file loading does.
struct SceneDocumentPatch {
    std::vector<std::byte> expected;
    std::vector<std::byte> replacement;
    std::vector<SceneNodeChange> changes;
    bool metadata_changed{};
    bool extensions_changed{};
};

[[nodiscard]] DocumentResult diff_scene_documents(
    const SceneDocument& before, const SceneDocument& after,
    SceneDocumentPatch& destination, const SceneArchiveLimits& limits = {});
[[nodiscard]] SceneDocumentPatch reverse_scene_patch(const SceneDocumentPatch& patch);
[[nodiscard]] DocumentResult apply_scene_patch(
    SceneDocument& document, const SceneDocumentPatch& patch,
    const SceneArchiveLimits& limits = {}, SceneDecodeOptions options = {});

class SceneDocumentStore;

// Move-only private copy. Failed edits and failed commits retain both the last
// committed document and the transaction's preceding valid draft. Cancellation
// has no live-scene side effects. Single-writer use is required throughout.
// Structural helpers reject opaque extensions: their owner must update any
// embedded indices/references and submit the complete candidate through replace.
class SceneDocumentTransaction final {
public:
    SceneDocumentTransaction(SceneDocumentTransaction&& other) noexcept;
    SceneDocumentTransaction& operator=(SceneDocumentTransaction&& other) noexcept;
    SceneDocumentTransaction(const SceneDocumentTransaction&) = delete;
    SceneDocumentTransaction& operator=(const SceneDocumentTransaction&) = delete;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const SceneDocument& document() const noexcept;
    [[nodiscard]] std::uint64_t base_epoch() const noexcept;
    DocumentResult replace(SceneDocument candidate);
    DocumentResult append(SnapshotNode node, std::size_t parent = no_parent);
    DocumentResult erase_subtree(std::size_t index);
    DocumentResult reparent(std::size_t index, std::size_t parent,
                            std::size_t sibling_index = append_child);
    DocumentResult rename(std::size_t index, std::string_view name);
    DocumentResult transform(std::size_t index, const math::Transform& value);
    DocumentResult set_enabled(std::size_t index, bool enabled);
    DocumentResult set_metadata(std::string_view key, std::optional<std::string_view> value);
    DocumentResult set_extension(SceneExtension extension);
    DocumentResult erase_extension(std::uint32_t id);
    void cancel() noexcept;

private:
    friend class SceneDocumentStore;
    SceneDocumentTransaction(const SceneDocumentStore* owner, std::uint64_t epoch,
                             SceneDocument document, SceneArchiveLimits limits);
    [[nodiscard]] DocumentResult editable(std::size_t index = no_parent) const;
    const SceneDocumentStore* owner_{};
    std::uint64_t epoch_{};
    SceneDocument document_;
    SceneArchiveLimits limits_;
};

class SceneDocumentStore final {
public:
    explicit SceneDocumentStore(SceneArchiveLimits limits = {});
    SceneDocumentStore(const SceneDocumentStore&) = delete;
    SceneDocumentStore& operator=(const SceneDocumentStore&) = delete;
    [[nodiscard]] const SceneDocument& document() const noexcept;
    [[nodiscard]] const SceneArchiveLimits& limits() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] SceneDocumentTransaction begin() const;
    DocumentResult commit(SceneDocumentTransaction& transaction);
    DocumentResult replace(SceneDocument candidate);
    DocumentResult decode(std::span<const std::byte> bytes, SceneDecodeOptions options = {});
    DocumentResult encode(std::vector<std::byte>& destination) const;
    DocumentResult capture(Scene& scene, Entity subtree = {});
    DocumentResult load(io::VirtualFileSystem& files, std::string_view path,
                        SceneDecodeOptions options = {});
    // Uses VFS publication semantics; does not promise power-loss durability.
    DocumentResult save(io::VirtualFileSystem& files, std::string_view path,
                        bool overwrite = true) const;
    DocumentResult apply(const SceneDocumentPatch& patch, SceneDecodeOptions options = {});
    [[nodiscard]] SceneDocumentImport import_into(
        Scene& scene, SceneImportOptions options = {}) const;

private:
    SceneArchiveLimits limits_;
    SceneDocument document_;
    std::uint64_t epoch_{};
};

using SceneSerializationService = SceneDocumentStore;
void register_serialization(core::Kernel& kernel);

} // namespace ge::scene
