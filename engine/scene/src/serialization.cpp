#include <ge/scene/serialization.hpp>
#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/io/filesystem.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ge::scene {
namespace {

constexpr std::string_view archive_magic{"GESCENE1"};
constexpr std::size_t envelope_bytes = 32;
constexpr std::uint32_t archive_schema = 0x53434e45U;
constexpr std::uint32_t manifest_section = 1;
constexpr std::uint32_t nodes_section = 2;
constexpr std::uint32_t disk_no_parent = std::numeric_limits<std::uint32_t>::max();

[[noreturn]] void reject(DocumentError error, std::string message,
                         std::size_t index = no_parent) {
    throw DocumentResult{error, index, std::move(message)};
}

void require_result(const DocumentResult& result) {
    if (!result) {
        throw result;
    }
}

template<class Function>
DocumentResult guarded(Function&& function) {
    try {
        function();
        return {};
    } catch (const DocumentResult& failure) {
        return failure;
    } catch (const core::BinaryException& failure) {
        auto error = DocumentError::CorruptArchive;
        if (failure.error() == core::BinaryError::limit_exceeded) {
            error = DocumentError::Limit;
        } else if (failure.error() == core::BinaryError::unsupported_version) {
            error = DocumentError::Version;
        } else if (failure.error() == core::BinaryError::invalid_utf8) {
            error = DocumentError::InvalidText;
        }
        return {error, no_parent, failure.what()};
    } catch (const io::IoException& failure) {
        return {DocumentError::IoFailure, no_parent, failure.what()};
    } catch (const std::bad_alloc&) {
        return {DocumentError::AllocationFailure, no_parent, {}};
    } catch (const std::length_error& failure) {
        return {DocumentError::Limit, no_parent, failure.what()};
    } catch (const std::exception& failure) {
        return {DocumentError::SceneRejected, no_parent, failure.what()};
    }
}

void check_limits(const SceneArchiveLimits& limits) {
    if (limits.max_archive_bytes < 165U ||
        limits.max_section_bytes < 32U || limits.max_depth == 0 ||
        limits.max_nodes >= disk_no_parent || limits.max_extensions > 65534U ||
        limits.max_nodes > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        limits.max_metadata > 65536U || limits.max_string_bytes == 0) {
        reject(DocumentError::Limit, "invalid scene archive limits");
    }
}

core::BinaryLimits binary_limits(const SceneArchiveLimits& limits) {
    core::BinaryLimits result;
    result.max_total_bytes = limits.max_archive_bytes;
    result.max_string_bytes = limits.max_string_bytes;
    result.max_chunk_bytes = limits.max_section_bytes;
    result.max_chunks = limits.max_extensions + 2U;
    return result;
}

void charge(std::size_t& total, std::size_t amount, std::size_t maximum,
            std::string_view description) {
    if (amount > maximum || total > maximum - amount) {
        reject(DocumentError::Limit, std::string(description));
    }
    total += amount;
}

void check_text(std::string_view text, std::size_t maximum, bool nonempty,
                bool no_controls, std::size_t index = no_parent) {
    const bool controls = std::any_of(text.begin(), text.end(), [](char character) {
        return static_cast<unsigned char>(character) < 0x20U;
    });
    if (text.size() > maximum || (nonempty && text.empty()) ||
        text.find('\0') != std::string_view::npos || !core::valid_utf8(text) ||
        (no_controls && controls)) {
        reject(DocumentError::InvalidText, "invalid UTF-8 text, length, or control byte", index);
    }
}

void validate_document(const SceneDocument& document, const SceneArchiveLimits& limits) {
    check_limits(limits);
    if (document.version != scene_document_version || document.snapshot.version != 1) {
        reject(DocumentError::Version, "unsupported document or scene snapshot version");
    }
    if (document.snapshot.nodes.size() > limits.max_nodes ||
        document.metadata.size() > limits.max_metadata ||
        document.extensions.size() > limits.max_extensions) {
        reject(DocumentError::Limit, "scene document count exceeds configured limit");
    }
    std::size_t string_bytes{};
    const auto text = [&](std::string_view value, std::size_t maximum,
                          bool nonempty, bool controls, std::size_t index = no_parent) {
        check_text(value, std::min(maximum, limits.max_string_bytes), nonempty, controls, index);
        charge(string_bytes, value.size(), limits.max_total_string_bytes, "total scene text budget exceeded");
    };
    text(document.name, 1024, false, false);
    for (const auto& [key, value] : document.metadata) {
        text(key, 128, true, true);
        text(value, limits.max_string_bytes, false, false);
    }
    std::unordered_set<std::uint64_t> identities;
    std::unordered_set<Entity, ecs::EntityHash> sources;
    std::vector<std::size_t> depths;
    depths.reserve(document.snapshot.nodes.size());
    for (std::size_t index = 0; index < document.snapshot.nodes.size(); ++index) {
        const auto& node = document.snapshot.nodes[index];
        if (node.parent_index != no_parent && node.parent_index >= index) {
            reject(DocumentError::InvalidHierarchy, "parents must precede their children", index);
        }
        const auto depth = node.parent_index == no_parent ? 0 : depths[node.parent_index] + 1U;
        if (depth >= limits.max_depth) {
            reject(DocumentError::InvalidHierarchy, "scene hierarchy depth limit exceeded", index);
        }
        depths.push_back(depth);
        if (node.persistent && !identities.insert(node.persistent.value).second) {
            reject(DocumentError::InvalidIdentity, "duplicate persistent identity", index);
        }
        if ((node.source.index != 0 || node.source.generation != 0) &&
            (!node.source || !sources.insert(node.source).second)) {
            reject(DocumentError::InvalidIdentity, "invalid or duplicate transient source handle", index);
        }
        text(node.name, 1024, false, false, index);
        const auto norm = math::length(node.local.rotation);
        auto normalized = node.local;
        normalized.rotation = math::normalized(node.local.rotation);
        if (!math::finite(node.local) || !std::isfinite(norm) || norm <= math::epsilon ||
            !math::finite(math::matrix(normalized))) {
            reject(DocumentError::InvalidTransform, "nonfinite or degenerate local transform", index);
        }
        if (node.bounds && !math::valid(*node.bounds)) {
            reject(DocumentError::InvalidBounds, "invalid local bounds", index);
        }
        if (node.tags.size() > 64U) {
            reject(DocumentError::Limit, "too many tags on node", index);
        }
        std::unordered_set<std::string_view> tags;
        for (const auto& tag : node.tags) {
            text(tag, 128, true, true, index);
            if (!tags.insert(tag).second) {
                reject(DocumentError::InvalidText, "duplicate tag", index);
            }
        }
    }
    std::unordered_set<std::uint32_t> section_ids;
    std::size_t payload_bytes{};
    for (const auto& extension : document.extensions) {
        if (extension.id < first_scene_extension || extension.version == 0 ||
            !section_ids.insert(extension.id).second) {
            reject(DocumentError::InvalidSection, "invalid or duplicate extension identifier/version");
        }
        if (extension.payload.size() > limits.max_section_bytes - 4U) {
            reject(DocumentError::Limit, "extension payload exceeds section budget");
        }
        charge(payload_bytes, extension.payload.size(), limits.max_archive_bytes,
               "combined extension payload budget exceeded");
    }
}

void write_vec3(core::ByteWriter& writer, math::Vec3 value) {
    writer.write_f64(value.x == 0.0 ? 0.0 : value.x);
    writer.write_f64(value.y == 0.0 ? 0.0 : value.y);
    writer.write_f64(value.z == 0.0 ? 0.0 : value.z);
}

math::Vec3 read_vec3(core::ByteReader& reader) {
    const auto x = reader.read_f64();
    const auto y = reader.read_f64();
    const auto z = reader.read_f64();
    return {x, y, z};
}

void write_transform(core::ByteWriter& writer, const math::Transform& value) {
    write_vec3(writer, value.position);
    writer.write_f64(value.rotation.x == 0.0 ? 0.0 : value.rotation.x);
    writer.write_f64(value.rotation.y == 0.0 ? 0.0 : value.rotation.y);
    writer.write_f64(value.rotation.z == 0.0 ? 0.0 : value.rotation.z);
    writer.write_f64(value.rotation.w == 0.0 ? 0.0 : value.rotation.w);
    write_vec3(writer, value.scale);
}

math::Transform read_transform(core::ByteReader& reader) {
    math::Transform value;
    value.position = read_vec3(reader);
    value.rotation.x = reader.read_f64();
    value.rotation.y = reader.read_f64();
    value.rotation.z = reader.read_f64();
    value.rotation.w = reader.read_f64();
    value.scale = read_vec3(reader);
    return value;
}

std::vector<std::byte> encode_document(const SceneDocument& document,
                                      const SceneArchiveLimits& limits) {
    validate_document(document, limits);
    core::ChunkArchive archive;
    archive.schema = archive_schema;
    archive.revision = document.revision;
    core::ByteWriter manifest(limits.max_section_bytes);
    manifest.write_u32(document.version);
    manifest.write_u32(document.snapshot.version);
    manifest.write_u64(document.snapshot.revision);
    manifest.write_string(document.name, limits.max_string_bytes);
    manifest.write_u32(static_cast<std::uint32_t>(document.metadata.size()));
    for (const auto& [key, value] : document.metadata) {
        manifest.write_string(key, limits.max_string_bytes);
        manifest.write_string(value, limits.max_string_bytes);
    }
    archive.chunks.push_back({manifest_section, 1, manifest.take()});
    core::ByteWriter nodes(limits.max_section_bytes);
    nodes.write_u32(static_cast<std::uint32_t>(document.snapshot.nodes.size()));
    for (const auto& node : document.snapshot.nodes) {
        nodes.write_u64(node.persistent.value);
        nodes.write_u32(node.parent_index == no_parent ? disk_no_parent :
                       static_cast<std::uint32_t>(node.parent_index));
        nodes.write_bool(node.enabled);
        nodes.write_u64(node.layers);
        nodes.write_string(node.name, limits.max_string_bytes);
        write_transform(nodes, node.local);
        nodes.write_bool(node.bounds.has_value());
        if (node.bounds) {
            write_vec3(nodes, node.bounds->min);
            write_vec3(nodes, node.bounds->max);
        }
        auto tags = node.tags;
        std::sort(tags.begin(), tags.end());
        nodes.write_u32(static_cast<std::uint32_t>(tags.size()));
        for (const auto& tag : tags) {
            nodes.write_string(tag, limits.max_string_bytes);
        }
    }
    archive.chunks.push_back({nodes_section, 1, nodes.take()});
    for (const auto& extension : document.extensions) {
        core::ByteWriter section(limits.max_section_bytes);
        section.write_bool(extension.required);
        section.write_zeroes(3);
        section.write_bytes(extension.payload);
        archive.chunks.push_back({extension.id, extension.version, section.take()});
    }
    auto inner_limits = binary_limits(limits);
    inner_limits.max_total_bytes -= envelope_bytes;
    const auto payload = core::encode_archive(archive, inner_limits);
    core::ByteWriter output(limits.max_archive_bytes);
    output.write_bytes(archive_magic);
    output.write_u16(1);
    output.write_u16(0);
    output.write_u32(0);
    output.write_u64(static_cast<std::uint64_t>(payload.size()));
    output.write_u32(core::crc32(payload));
    output.write_u32(0);
    output.write_bytes(payload);
    return output.take();
}

std::string read_text(core::ByteReader& reader, std::size_t& budget,
                      const SceneArchiveLimits& limits) {
    const auto text = reader.read_string_view();
    charge(budget, text.size(), limits.max_total_string_bytes, "decoded scene text budget exceeded");
    return std::string(text);
}

SceneDocument decode_document(std::span<const std::byte> bytes,
                               const SceneArchiveLimits& limits,
                               SceneDecodeOptions options) {
    check_limits(limits);
    core::ByteReader input(bytes, core::ByteOrder::little, binary_limits(limits));
    input.expect(archive_magic);
    const auto major = input.read_u16();
    const auto minor = input.read_u16();
    if (major != 1 || minor != 0) {
        reject(DocumentError::Version, "unsupported scene archive major/minor version");
    }
    input.require_zero(4);
    const auto payload_size = input.read_u64();
    const auto checksum = input.read_u32();
    input.require_zero(4);
    if (payload_size != input.remaining() || core::crc32(input.unread()) != checksum) {
        reject(DocumentError::CorruptArchive, "scene envelope length or checksum mismatch");
    }
    const auto archive = core::decode_archive(input.unread(), binary_limits(limits));
    if (archive.schema != archive_schema) {
        reject(DocumentError::Version, "chunk archive is not a scene document");
    }
    const auto* manifest = archive.find(manifest_section);
    const auto* nodes = archive.find(nodes_section);
    if (!manifest || !nodes || manifest->version != 1 || nodes->version != 1) {
        reject(DocumentError::Version, "required scene section missing or unsupported");
    }
    if (options.understood_sections.size() > limits.max_extensions) {
        reject(DocumentError::Limit, "too many acknowledged extension versions");
    }
    if (options.unknown_sections != UnknownSectionPolicy::PreserveOptional &&
        options.unknown_sections != UnknownSectionPolicy::DiscardOptional &&
        options.unknown_sections != UnknownSectionPolicy::RejectUnknown) {
        reject(DocumentError::InvalidSection, "invalid unknown-section policy");
    }
    SceneDocument result;
    result.revision = archive.revision;
    std::size_t string_bytes{};
    core::ByteReader info(manifest->data, core::ByteOrder::little, binary_limits(limits));
    result.version = info.read_u32();
    result.snapshot.version = info.read_u32();
    if (result.version != scene_document_version || result.snapshot.version != 1) {
        reject(DocumentError::Version, "unsupported document or snapshot version");
    }
    result.snapshot.revision = info.read_u64();
    result.name = read_text(info, string_bytes, limits);
    const auto metadata_count = info.read_u32();
    if (metadata_count > limits.max_metadata || metadata_count > info.remaining() / 2U) {
        reject(DocumentError::Limit, "invalid metadata count");
    }
    std::string previous_key;
    for (std::uint32_t index = 0; index < metadata_count; ++index) {
        auto key = read_text(info, string_bytes, limits);
        auto value = read_text(info, string_bytes, limits);
        if (index != 0 && key <= previous_key) {
            reject(DocumentError::InvalidText, "metadata keys are not strictly sorted");
        }
        previous_key = key;
        result.metadata.emplace(std::move(key), std::move(value));
    }
    info.require_end();
    core::ByteReader records(nodes->data, core::ByteOrder::little, binary_limits(limits));
    const auto count = records.read_u32();
    constexpr std::size_t minimum_node_bytes = 107;
    if (count > limits.max_nodes || count > records.remaining() / minimum_node_bytes) {
        reject(DocumentError::Limit, "invalid node count or truncated node section");
    }
    result.snapshot.nodes.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        SnapshotNode node;
        node.persistent = PersistentId{records.read_u64()};
        const auto parent = records.read_u32();
        node.parent_index = parent == disk_no_parent ? no_parent : static_cast<std::size_t>(parent);
        node.enabled = records.read_bool();
        node.layers = records.read_u64();
        node.name = read_text(records, string_bytes, limits);
        node.local = read_transform(records);
        if (records.read_bool()) {
            const auto low = read_vec3(records);
            const auto high = read_vec3(records);
            node.bounds = math::Aabb{low, high};
        }
        const auto tags = records.read_u32();
        if (tags > 64U || tags > records.remaining()) {
            reject(DocumentError::Limit, "invalid node tag count", index);
        }
        node.tags.reserve(tags);
        for (std::uint32_t tag_index = 0; tag_index < tags; ++tag_index) {
            auto tag = read_text(records, string_bytes, limits);
            if (!node.tags.empty() && tag <= node.tags.back()) {
                reject(DocumentError::InvalidText, "tags are not strictly sorted", index);
            }
            node.tags.push_back(std::move(tag));
        }
        result.snapshot.nodes.push_back(std::move(node));
    }
    records.require_end();
    for (const auto& chunk : archive.chunks) {
        if (chunk.id == manifest_section || chunk.id == nodes_section) {
            continue;
        }
        if (chunk.id < first_scene_extension || chunk.version == 0) {
            reject(DocumentError::InvalidSection, "reserved or invalid scene section");
        }
        core::ByteReader extension(chunk.data, core::ByteOrder::little, binary_limits(limits));
        const bool required = extension.read_bool();
        extension.require_zero(3);
        const bool understood = std::any_of(options.understood_sections.begin(),
            options.understood_sections.end(), [&](const SceneSectionVersion& version) {
                return version.id == chunk.id && version.version == chunk.version;
            });
        if (!understood && (required || options.unknown_sections == UnknownSectionPolicy::RejectUnknown)) {
            reject(DocumentError::UnsupportedSection, "unacknowledged scene extension/version: " +
                   std::to_string(chunk.id));
        }
        if (understood || options.unknown_sections == UnknownSectionPolicy::PreserveOptional) {
            SceneExtension saved{chunk.id, chunk.version, required, {}};
            saved.payload.assign(extension.unread().begin(), extension.unread().end());
            result.extensions.push_back(std::move(saved));
        }
    }
    validate_document(result, limits);
    return result;
}

bool same_node_payload(const SnapshotNode& first, const SnapshotNode& second) {
    if (first.persistent != second.persistent || first.name != second.name ||
        first.local.position != second.local.position ||
        first.local.rotation != second.local.rotation || first.local.scale != second.local.scale ||
        first.enabled != second.enabled || first.layers != second.layers ||
        first.bounds.has_value() != second.bounds.has_value()) {
        return false;
    }
    if (first.bounds && (first.bounds->min != second.bounds->min ||
                         first.bounds->max != second.bounds->max)) {
        return false;
    }
    auto first_tags = first.tags;
    auto second_tags = second.tags;
    std::sort(first_tags.begin(), first_tags.end());
    std::sort(second_tags.begin(), second_tags.end());
    return first_tags == second_tags;
}

std::vector<SceneSectionVersion> acknowledged_sections(const SceneDocument& document) {
    std::vector<SceneSectionVersion> result;
    result.reserve(document.extensions.size());
    for (const auto& extension : document.extensions) {
        result.push_back({extension.id, extension.version});
    }
    return result;
}

} // namespace

std::string_view describe(DocumentError error) noexcept {
    switch (error) {
    case DocumentError::None: return "success";
    case DocumentError::Limit: return "document limit exceeded";
    case DocumentError::Version: return "unsupported document version";
    case DocumentError::InvalidHierarchy: return "invalid hierarchy";
    case DocumentError::InvalidIdentity: return "invalid identity";
    case DocumentError::InvalidText: return "invalid text";
    case DocumentError::InvalidTransform: return "invalid transform";
    case DocumentError::InvalidBounds: return "invalid bounds";
    case DocumentError::InvalidSection: return "invalid extension section";
    case DocumentError::CorruptArchive: return "corrupt scene archive";
    case DocumentError::Conflict: return "document changed since transaction began";
    case DocumentError::InactiveTransaction: return "transaction is inactive or foreign";
    case DocumentError::MissingNode: return "document node does not exist";
    case DocumentError::UnsupportedSection: return "unsupported required extension";
    case DocumentError::SceneRejected: return "live scene rejected import or capture";
    case DocumentError::IoFailure: return "scene file operation failed";
    case DocumentError::AllocationFailure: return "scene document allocation failed";
    }
    return "unknown document error";
}

void SceneDocument::swap(SceneDocument& other) noexcept {
    std::swap(version, other.version);
    std::swap(revision, other.revision);
    name.swap(other.name);
    std::swap(snapshot.version, other.snapshot.version);
    std::swap(snapshot.revision, other.snapshot.revision);
    snapshot.nodes.swap(other.snapshot.nodes);
    metadata.swap(other.metadata);
    extensions.swap(other.extensions);
}

DocumentResult validate_scene_document(const SceneDocument& document,
                                       const SceneArchiveLimits& limits) {
    return guarded([&] { validate_document(document, limits); });
}

DocumentResult encode_scene_document(const SceneDocument& document,
                                     std::vector<std::byte>& destination,
                                     const SceneArchiveLimits& limits) {
    return guarded([&] {
        auto candidate = encode_document(document, limits);
        destination.swap(candidate);
    });
}

DocumentResult decode_scene_document(std::span<const std::byte> bytes,
                                     SceneDocument& destination,
                                     const SceneArchiveLimits& limits,
                                     SceneDecodeOptions options) {
    return guarded([&] {
        auto candidate = decode_document(bytes, limits, options);
        destination.swap(candidate);
    });
}

SceneDocumentStatistics inspect_scene_document(const SceneDocument& document) {
    SceneDocumentStatistics result;
    result.nodes = document.snapshot.nodes.size();
    result.revision = document.revision;
    result.string_bytes = document.name.size();
    std::vector<std::size_t> depths(result.nodes);
    for (std::size_t index = 0; index < result.nodes; ++index) {
        const auto& node = document.snapshot.nodes[index];
        if (node.parent_index == no_parent) {
            ++result.roots;
        } else if (node.parent_index < index) {
            depths[index] = depths[node.parent_index] + 1U;
        }
        result.maximum_depth = std::max(result.maximum_depth, depths[index]);
        result.persistent_nodes += node.persistent ? 1U : 0U;
        result.enabled_nodes += node.enabled ? 1U : 0U;
        result.bounded_nodes += node.bounds ? 1U : 0U;
        result.tags += node.tags.size();
        result.string_bytes += node.name.size();
        for (const auto& tag : node.tags) {
            result.string_bytes += tag.size();
        }
    }
    for (const auto& [key, value] : document.metadata) {
        result.string_bytes += key.size() + value.size();
    }
    for (const auto& extension : document.extensions) {
        result.extension_bytes += extension.payload.size();
    }
    return result;
}

std::optional<std::size_t> find_document_node(const SceneDocument& document,
                                             PersistentId persistent) noexcept {
    if (!persistent) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < document.snapshot.nodes.size(); ++index) {
        if (document.snapshot.nodes[index].persistent == persistent) {
            return index;
        }
    }
    return std::nullopt;
}

DocumentResult capture_scene_document(Scene& scene, SceneDocument& destination,
                                      Entity subtree, const SceneArchiveLimits& limits) {
    return guarded([&] {
        if (scene.registry().iterating()) {
            reject(DocumentError::SceneRejected, "cannot capture while Registry is iterating");
        }
        if (!subtree && scene.size() > limits.max_nodes) {
            reject(DocumentError::Limit, "live scene exceeds document node limit");
        }
        auto snapshot = scene.snapshot(subtree);
        if (!snapshot) {
            reject(DocumentError::SceneRejected, "scene snapshot rejected the selected hierarchy");
        }
        SceneDocument candidate;
        candidate.snapshot = std::move(*snapshot);
        candidate.revision = candidate.snapshot.revision;
        candidate.name = subtree ? std::string(scene.registry().name(subtree)) : "Scene";
        (void)encode_document(candidate, limits);
        destination.swap(candidate);
    });
}

SceneDocumentImport import_scene_document(const SceneDocument& document, Scene& scene,
                                           SceneImportOptions options,
                                           const SceneArchiveLimits& limits) {
    SceneDocumentImport result;
    result.status = guarded([&] {
        validate_document(document, limits);
        for (const auto& extension : document.extensions) {
            if (extension.required && !options.allow_unapplied_required_sections) {
                reject(DocumentError::UnsupportedSection, "import requires an owner for required extension " +
                       std::to_string(extension.id));
            }
            result.unapplied_sections.push_back(extension.id);
        }
        result.nodes.reserve(document.snapshot.nodes.size());
        result.scene_result = scene.import_snapshot(document.snapshot, options.parent,
                                                   options.preserve_persistent_ids);
        if (!result.scene_result) {
            reject(DocumentError::SceneRejected, std::string(describe(result.scene_result.error)));
        }
        for (std::size_t index = 0; index < result.scene_result.entities.size(); ++index) {
            result.nodes.push_back({index, document.snapshot.nodes[index].persistent,
                                    result.scene_result.entities[index].destination});
        }
    });
    return result;
}

DocumentResult diff_scene_documents(const SceneDocument& before, const SceneDocument& after,
                                     SceneDocumentPatch& destination,
                                     const SceneArchiveLimits& limits) {
    return guarded([&] {
        SceneDocumentPatch candidate;
        candidate.expected = encode_document(before, limits);
        candidate.replacement = encode_document(after, limits);
        candidate.metadata_changed = before.name != after.name || before.metadata != after.metadata ||
            before.revision != after.revision || before.snapshot.revision != after.snapshot.revision;
        std::map<std::uint32_t, const SceneExtension*> prior_extensions;
        for (const auto& extension : before.extensions) {
            prior_extensions.emplace(extension.id, &extension);
        }
        candidate.extensions_changed = before.extensions.size() != after.extensions.size();
        for (const auto& extension : after.extensions) {
            const auto found = prior_extensions.find(extension.id);
            if (found == prior_extensions.end() || *found->second != extension) {
                candidate.extensions_changed = true;
            }
        }
        std::unordered_map<std::uint64_t, std::size_t> after_ids;
        for (std::size_t index = 0; index < after.snapshot.nodes.size(); ++index) {
            if (const auto persistent = after.snapshot.nodes[index].persistent) {
                after_ids.emplace(persistent.value, index);
            }
        }
        std::vector<bool> matched(after.snapshot.nodes.size());
        for (std::size_t index = 0; index < before.snapshot.nodes.size(); ++index) {
            const auto& previous = before.snapshot.nodes[index];
            std::size_t next = no_parent;
            if (previous.persistent) {
                const auto found = after_ids.find(previous.persistent.value);
                if (found != after_ids.end()) {
                    next = found->second;
                }
            } else if (index < after.snapshot.nodes.size() && !after.snapshot.nodes[index].persistent) {
                next = index;
            }
            if (next == no_parent) {
                candidate.changes.push_back({SceneNodeChangeKind::Removed, previous.persistent, index, no_parent});
                continue;
            }
            matched[next] = true;
            const auto& current = after.snapshot.nodes[next];
            if (!same_node_payload(previous, current)) {
                candidate.changes.push_back({SceneNodeChangeKind::Modified, previous.persistent, index, next});
            }
            if (index != next || previous.parent_index != current.parent_index) {
                candidate.changes.push_back({SceneNodeChangeKind::Moved, previous.persistent, index, next});
            }
        }
        for (std::size_t index = 0; index < after.snapshot.nodes.size(); ++index) {
            if (!matched[index]) {
                candidate.changes.push_back({SceneNodeChangeKind::Added,
                    after.snapshot.nodes[index].persistent, no_parent, index});
            }
        }
        std::swap(destination, candidate);
    });
}

SceneDocumentPatch reverse_scene_patch(const SceneDocumentPatch& patch) {
    SceneDocumentPatch result;
    result.expected = patch.replacement;
    result.replacement = patch.expected;
    result.metadata_changed = patch.metadata_changed;
    result.extensions_changed = patch.extensions_changed;
    result.changes.reserve(patch.changes.size());
    for (auto change : patch.changes) {
        std::swap(change.before_index, change.after_index);
        if (change.kind == SceneNodeChangeKind::Added) {
            change.kind = SceneNodeChangeKind::Removed;
        } else if (change.kind == SceneNodeChangeKind::Removed) {
            change.kind = SceneNodeChangeKind::Added;
        }
        result.changes.push_back(change);
    }
    return result;
}

DocumentResult apply_scene_patch(SceneDocument& document, const SceneDocumentPatch& patch,
                                 const SceneArchiveLimits& limits, SceneDecodeOptions options) {
    return guarded([&] {
        if (patch.expected.size() > limits.max_archive_bytes ||
            patch.replacement.size() > limits.max_archive_bytes) {
            reject(DocumentError::Limit, "scene patch exceeds archive budget");
        }
        if (encode_document(document, limits) != patch.expected) {
            reject(DocumentError::Conflict, "scene patch exact base precondition failed");
        }
        const auto understood = acknowledged_sections(document);
        if (options.understood_sections.empty()) {
            options.understood_sections = understood;
        }
        auto candidate = decode_document(patch.replacement, limits, options);
        if (encode_document(candidate, limits) != patch.replacement) {
            reject(DocumentError::CorruptArchive, "patch replacement is not canonically encoded");
        }
        document.swap(candidate);
    });
}

SceneDocumentTransaction::SceneDocumentTransaction(const SceneDocumentStore* owner,
    std::uint64_t epoch, SceneDocument document, SceneArchiveLimits limits)
    : owner_(owner), epoch_(epoch), document_(std::move(document)), limits_(limits) {}

SceneDocumentTransaction::SceneDocumentTransaction(SceneDocumentTransaction&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), epoch_(other.epoch_),
      document_(std::move(other.document_)), limits_(other.limits_) {}

SceneDocumentTransaction& SceneDocumentTransaction::operator=(SceneDocumentTransaction&& other) noexcept {
    if (this != &other) {
        owner_ = std::exchange(other.owner_, nullptr);
        epoch_ = other.epoch_;
        document_ = std::move(other.document_);
        limits_ = other.limits_;
    }
    return *this;
}

bool SceneDocumentTransaction::active() const noexcept { return owner_ != nullptr; }
const SceneDocument& SceneDocumentTransaction::document() const noexcept { return document_; }
std::uint64_t SceneDocumentTransaction::base_epoch() const noexcept { return epoch_; }
void SceneDocumentTransaction::cancel() noexcept { owner_ = nullptr; }

DocumentResult SceneDocumentTransaction::editable(std::size_t index) const {
    if (!active()) {
        return {DocumentError::InactiveTransaction, no_parent, "transaction is no longer active"};
    }
    if (index != no_parent && index >= document_.snapshot.nodes.size()) {
        return {DocumentError::MissingNode, index, "node index outside the transaction"};
    }
    return {};
}

DocumentResult SceneDocumentTransaction::replace(SceneDocument candidate) {
    return guarded([&] {
        require_result(editable());
        (void)encode_document(candidate, limits_);
        document_.swap(candidate);
    });
}

DocumentResult SceneDocumentTransaction::append(SnapshotNode node, std::size_t parent) {
    return guarded([&] {
        require_result(editable(parent));
        if (!document_.extensions.empty()) {
            reject(DocumentError::UnsupportedSection, "structural edits require extension-owner replacement");
        }
        if (document_.snapshot.nodes.size() >= limits_.max_nodes) {
            reject(DocumentError::Limit, "transaction node capacity exhausted");
        }
        auto candidate = document_;
        node.parent_index = parent;
        candidate.snapshot.nodes.push_back(std::move(node));
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::erase_subtree(std::size_t index) {
    return guarded([&] {
        require_result(editable(index));
        if (index == no_parent) {
            reject(DocumentError::MissingNode, "a concrete node index is required");
        }
        if (!document_.extensions.empty()) {
            reject(DocumentError::UnsupportedSection, "structural edits require extension-owner replacement");
        }
        auto candidate = document_;
        auto previous = std::move(candidate.snapshot.nodes);
        candidate.snapshot.nodes.clear();
        std::vector<std::size_t> remap(previous.size(), no_parent);
        std::vector<bool> removed(previous.size());
        candidate.snapshot.nodes.reserve(previous.size());
        for (std::size_t current = 0; current < previous.size(); ++current) {
            const auto parent = previous[current].parent_index;
            removed[current] = current == index || (parent != no_parent && removed[parent]);
            if (removed[current]) {
                continue;
            }
            remap[current] = candidate.snapshot.nodes.size();
            previous[current].parent_index = parent == no_parent ? no_parent : remap[parent];
            candidate.snapshot.nodes.push_back(std::move(previous[current]));
        }
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::reparent(std::size_t index, std::size_t parent,
                                                  std::size_t sibling_index) {
    return guarded([&] {
        require_result(editable(index));
        require_result(editable(parent));
        if (index == no_parent) {
            reject(DocumentError::MissingNode, "a concrete node index is required");
        }
        if (!document_.extensions.empty()) {
            reject(DocumentError::UnsupportedSection, "structural edits require extension-owner replacement");
        }
        for (auto ancestor = parent; ancestor != no_parent;
             ancestor = document_.snapshot.nodes[ancestor].parent_index) {
            if (ancestor == index) {
                reject(DocumentError::InvalidHierarchy, "reparent would create a cycle", index);
            }
        }
        auto candidate = document_;
        auto previous = std::move(candidate.snapshot.nodes);
        candidate.snapshot.nodes.clear();
        const auto root = previous.size();
        std::vector<std::vector<std::size_t>> children(previous.size() + 1U);
        for (std::size_t current = 0; current < previous.size(); ++current) {
            if (current != index) {
                const auto ancestor = previous[current].parent_index;
                children[ancestor == no_parent ? root : ancestor].push_back(current);
            }
        }
        auto& siblings = children[parent == no_parent ? root : parent];
        const auto insertion = sibling_index == append_child ? siblings.size() : sibling_index;
        if (insertion > siblings.size()) {
            reject(DocumentError::InvalidHierarchy, "sibling insertion index is outside parent");
        }
        siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(insertion), index);
        std::vector<std::pair<std::size_t, std::size_t>> stack;
        for (auto item = children[root].rbegin(); item != children[root].rend(); ++item) {
            stack.emplace_back(*item, no_parent);
        }
        candidate.snapshot.nodes.reserve(previous.size());
        while (!stack.empty()) {
            const auto [source, destination_parent] = stack.back();
            stack.pop_back();
            const auto destination = candidate.snapshot.nodes.size();
            previous[source].parent_index = destination_parent;
            candidate.snapshot.nodes.push_back(std::move(previous[source]));
            for (auto child = children[source].rbegin(); child != children[source].rend(); ++child) {
                stack.emplace_back(*child, destination);
            }
        }
        if (candidate.snapshot.nodes.size() != previous.size()) {
            reject(DocumentError::InvalidHierarchy, "reparent did not retain the complete hierarchy");
        }
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::rename(std::size_t index, std::string_view name) {
    return guarded([&] {
        require_result(editable(index));
        check_text(name, std::min<std::size_t>(1024, limits_.max_string_bytes), false, false, index);
        if (index == no_parent) {
            reject(DocumentError::MissingNode, "a concrete node index is required");
        }
        auto candidate = document_;
        candidate.snapshot.nodes[index].name = name;
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::transform(std::size_t index, const math::Transform& value) {
    return guarded([&] {
        require_result(editable(index));
        if (index == no_parent) {
            reject(DocumentError::MissingNode, "a concrete node index is required");
        }
        auto candidate = document_;
        candidate.snapshot.nodes[index].local = value;
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::set_enabled(std::size_t index, bool enabled) {
    return guarded([&] {
        require_result(editable(index));
        if (index == no_parent) {
            reject(DocumentError::MissingNode, "a concrete node index is required");
        }
        auto candidate = document_;
        candidate.snapshot.nodes[index].enabled = enabled;
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::set_metadata(std::string_view key,
                                                      std::optional<std::string_view> value) {
    return guarded([&] {
        require_result(editable());
        check_text(key, std::min<std::size_t>(128, limits_.max_string_bytes), true, true);
        if (value) {
            check_text(*value, limits_.max_string_bytes, false, false);
        }
        auto candidate = document_;
        if (value) {
            candidate.metadata.insert_or_assign(std::string(key), std::string(*value));
        } else {
            const auto found = candidate.metadata.find(key);
            if (found != candidate.metadata.end()) {
                candidate.metadata.erase(found);
            }
        }
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::set_extension(SceneExtension extension) {
    return guarded([&] {
        require_result(editable());
        auto candidate = document_;
        const auto found = std::find_if(candidate.extensions.begin(), candidate.extensions.end(),
            [&](const SceneExtension& existing) { return existing.id == extension.id; });
        if (found == candidate.extensions.end()) {
            candidate.extensions.push_back(std::move(extension));
        } else {
            *found = std::move(extension);
        }
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentTransaction::erase_extension(std::uint32_t id) {
    return guarded([&] {
        require_result(editable());
        auto candidate = document_;
        std::erase_if(candidate.extensions, [id](const SceneExtension& extension) {
            return extension.id == id;
        });
        require_result(replace(std::move(candidate)));
    });
}

SceneDocumentStore::SceneDocumentStore(SceneArchiveLimits limits) : limits_(limits) {
    const auto status = validate_scene_document(document_, limits_);
    if (!status) {
        throw std::invalid_argument(status.message);
    }
}

const SceneDocument& SceneDocumentStore::document() const noexcept { return document_; }
const SceneArchiveLimits& SceneDocumentStore::limits() const noexcept { return limits_; }
std::uint64_t SceneDocumentStore::epoch() const noexcept { return epoch_; }

SceneDocumentTransaction SceneDocumentStore::begin() const {
    return SceneDocumentTransaction(this, epoch_, document_, limits_);
}

DocumentResult SceneDocumentStore::commit(SceneDocumentTransaction& transaction) {
    return guarded([&] {
        if (transaction.owner_ != this) {
            reject(DocumentError::InactiveTransaction, "transaction is inactive or belongs to another store");
        }
        if (transaction.epoch_ != epoch_) {
            reject(DocumentError::Conflict, "document epoch changed during the transaction");
        }
        if (document_.revision == std::numeric_limits<std::uint64_t>::max()) {
            reject(DocumentError::Limit, "document revision exhausted");
        }
        auto candidate = transaction.document_;
        candidate.revision = document_.revision + 1U;
        require_result(replace(std::move(candidate)));
        transaction.cancel();
    });
}

DocumentResult SceneDocumentStore::replace(SceneDocument candidate) {
    return guarded([&] {
        if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            reject(DocumentError::Limit, "document epoch exhausted");
        }
        (void)encode_document(candidate, limits_);
        document_.swap(candidate);
        ++epoch_;
    });
}

DocumentResult SceneDocumentStore::decode(std::span<const std::byte> bytes, SceneDecodeOptions options) {
    return guarded([&] {
        auto candidate = decode_document(bytes, limits_, options);
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentStore::encode(std::vector<std::byte>& destination) const {
    return encode_scene_document(document_, destination, limits_);
}

DocumentResult SceneDocumentStore::capture(Scene& scene, Entity subtree) {
    return guarded([&] {
        SceneDocument candidate;
        require_result(capture_scene_document(scene, candidate, subtree, limits_));
        require_result(replace(std::move(candidate)));
    });
}

DocumentResult SceneDocumentStore::load(io::VirtualFileSystem& files, std::string_view path,
                                       SceneDecodeOptions options) {
    return guarded([&] {
        const auto bytes = files.read(path, limits_.max_archive_bytes);
        require_result(decode(bytes, options));
    });
}

DocumentResult SceneDocumentStore::save(io::VirtualFileSystem& files, std::string_view path,
                                       bool overwrite) const {
    return guarded([&] {
        const auto bytes = encode_document(document_, limits_);
        io::WriteOptions options;
        options.overwrite = overwrite;
        options.create_parents = true;
        files.write(path, bytes, options);
    });
}

DocumentResult SceneDocumentStore::apply(const SceneDocumentPatch& patch, SceneDecodeOptions options) {
    return guarded([&] {
        auto candidate = document_;
        require_result(apply_scene_patch(candidate, patch, limits_, options));
        require_result(replace(std::move(candidate)));
    });
}

SceneDocumentImport SceneDocumentStore::import_into(Scene& scene, SceneImportOptions options) const {
    return import_scene_document(document_, scene, options, limits_);
}

namespace {

core::CommandResult command_result(const DocumentResult& result, std::string success) {
    if (result) {
        return {true, std::move(success)};
    }
    std::string message(describe(result.error));
    if (!result.message.empty()) {
        message += ": " + result.message;
    }
    if (result.index != no_parent) {
        message += " (node " + std::to_string(result.index) + ")";
    }
    return {false, std::move(message)};
}

std::optional<std::size_t> parse_index(std::string_view text) {
    std::size_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

bool parse_number(std::string_view text, double& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

} // namespace

void register_serialization(core::Kernel& kernel) {
    if (kernel.services().find<SceneDocumentStore>()) {
        return;
    }
    SceneArchiveLimits limits;
    limits.max_archive_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("scene.serialization.archive_bytes", 67108864), 4096, 1073741824));
    limits.max_section_bytes = std::min(limits.max_section_bytes, limits.max_archive_bytes);
    limits.max_nodes = static_cast<std::size_t>(std::clamp<std::int64_t>(
        kernel.config().integer("scene.serialization.nodes", 65536), 0, 1000000));
    auto& store = kernel.services().emplace<SceneDocumentStore>(limits);
    kernel.register_module({"scene.serialization", "Versioned scene documents and private-copy transactions", 1});
    kernel.add_command("scene.document.capture", [](core::Kernel& host, core::CommandArgs args) {
        auto* scene = host.services().find<Scene>();
        if (!scene || args.size() > 1) {
            return core::CommandResult{false, "usage: scene.document.capture [scene-path]; Scene required"};
        }
        Entity subtree{};
        if (!args.empty()) {
            const auto found = scene->find_path(args[0]);
            if (!found) {
                return core::CommandResult{false, std::string(describe(found.error))};
            }
            subtree = found.entity;
        }
        auto& documents = host.services().get<SceneDocumentStore>();
        const auto status = documents.capture(*scene, subtree);
        return command_result(status, "Captured scene hierarchy and core fields; arbitrary components are excluded");
    }, "Capture the live scene or one subtree into the current document");
    for (const bool saving : {false, true}) {
        kernel.add_command(saving ? "scene.document.save" : "scene.document.load",
            [saving](core::Kernel& host, core::CommandArgs args) {
                auto* files = host.services().find<io::VirtualFileSystem>();
                if (!files || args.size() != 1) {
                    return core::CommandResult{false, "one virtual path and the VFS service are required"};
                }
                auto& documents = host.services().get<SceneDocumentStore>();
                const auto status = saving ? documents.save(*files, args[0]) : documents.load(*files, args[0]);
                return command_result(status, saving ? "Scene document saved" : "Scene document loaded");
            }, saving ? "Save the current document through VFS" : "Validate and load a scene document through VFS");
    }
    kernel.add_command("scene.document.import", [](core::Kernel& host, core::CommandArgs args) {
        auto* scene = host.services().find<Scene>();
        if (!scene || args.size() > 2) {
            return core::CommandResult{false, "usage: scene.document.import [parent-path] [preserve|anonymous]"};
        }
        SceneImportOptions options;
        if (!args.empty()) {
            const auto parent = scene->find_path(args[0]);
            if (!parent) {
                return core::CommandResult{false, std::string(describe(parent.error))};
            }
            options.parent = parent.entity;
        }
        if (args.size() == 2) {
            if (args[1] != "preserve" && args[1] != "anonymous") {
                return core::CommandResult{false, "identity mode must be preserve or anonymous"};
            }
            options.preserve_persistent_ids = args[1] == "preserve";
        }
        const auto result = host.services().get<SceneDocumentStore>().import_into(*scene, options);
        return command_result(result.status, "Added " + std::to_string(result.nodes.size()) +
            " live nodes; unapplied optional sections=" + std::to_string(result.unapplied_sections.size()));
    }, "Add the current document to the shared Scene; existing entities remain");
    kernel.add_command("scene.document.inspect", [](core::Kernel& host, core::CommandArgs args) {
        const auto& document = host.services().get<SceneDocumentStore>().document();
        std::ostringstream output;
        if (args.empty()) {
            const auto stats = inspect_scene_document(document);
            output << document.name << " revision=" << stats.revision << " nodes=" << stats.nodes
                   << " roots=" << stats.roots << " depth=" << stats.maximum_depth
                   << " identities=" << stats.persistent_nodes << " text_bytes=" << stats.string_bytes
                   << " extensions=" << document.extensions.size() << " extension_bytes=" << stats.extension_bytes;
        } else {
            const auto index = args.size() == 1 ? parse_index(args[0]) : std::nullopt;
            if (!index || *index >= document.snapshot.nodes.size()) {
                return core::CommandResult{false, "usage: scene.document.inspect [valid-node-index]"};
            }
            const auto& node = document.snapshot.nodes[*index];
            output << "node=" << *index << " name=" << node.name << " persistent=" << node.persistent.value
                   << " parent=" << (node.parent_index == no_parent ? "root" : std::to_string(node.parent_index))
                   << " enabled=" << node.enabled << " layers=" << node.layers << " tags=" << node.tags.size();
        }
        return core::CommandResult{true, output.str()};
    }, "Inspect document statistics or a node without importing it");
    kernel.add_command("scene.document.edit", [](core::Kernel& host, core::CommandArgs args) {
        auto& documents = host.services().get<SceneDocumentStore>();
        const auto index = args.size() >= 2 ? parse_index(args[0]) : std::nullopt;
        if (!index || *index >= documents.document().snapshot.nodes.size()) {
            return core::CommandResult{false, "usage: scene.document.edit index name|enabled|position|erase|parent values"};
        }
        auto transaction = documents.begin();
        DocumentResult status{DocumentError::InvalidText, *index, "invalid edit operation or argument count"};
        if (args[1] == "name" && args.size() == 3) {
            status = transaction.rename(*index, args[2]);
        } else if (args[1] == "enabled" && args.size() == 3 && (args[2] == "true" || args[2] == "false")) {
            status = transaction.set_enabled(*index, args[2] == "true");
        } else if (args[1] == "erase" && args.size() == 2) {
            status = transaction.erase_subtree(*index);
        } else if (args[1] == "parent" && args.size() == 3) {
            const auto parent = args[2] == "root" ? std::optional<std::size_t>{no_parent} : parse_index(args[2]);
            if (parent) {
                status = transaction.reparent(*index, *parent);
            }
        } else if (args[1] == "position" && args.size() == 5) {
            auto transform = transaction.document().snapshot.nodes[*index].local;
            if (parse_number(args[2], transform.position.x) && parse_number(args[3], transform.position.y) &&
                parse_number(args[4], transform.position.z)) {
                status = transaction.transform(*index, transform);
            }
        }
        if (status) {
            status = documents.commit(transaction);
        }
        return command_result(status, "Document edit committed");
    }, "Edit a private document copy and publish it only after validation");
    auto* service = &store;
    kernel.add_system({"scene.serialization.metrics", core::Phase::EndFrame, 500, {}},
        [service](core::Kernel& host, const core::Frame&) {
            host.metric("scene.document.nodes", static_cast<double>(service->document().snapshot.nodes.size()));
            host.metric("scene.document.epoch", static_cast<double>(service->epoch()));
        });
}

} // namespace ge::scene

