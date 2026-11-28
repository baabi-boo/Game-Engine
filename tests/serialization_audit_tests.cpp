#include <ge/core/binary.hpp>
#include <ge/scene/serialization.hpp>

#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace {

using namespace ge::scene;

std::vector<std::byte> encoded(const SceneDocument& document,
                               const SceneArchiveLimits& limits = {}) {
    std::vector<std::byte> bytes;
    CHECK(encode_scene_document(document, bytes, limits));
    return bytes;
}

void check_budget_boundary(const SceneDocument& document, SceneArchiveLimits limits,
                           bool section_limit) {
    CHECK(validate_scene_document(document, limits));
    const auto bytes = encoded(document, limits);
    SceneDocument decoded;
    CHECK(decode_scene_document(bytes, decoded, limits));
    CHECK(encoded(decoded, limits) == bytes);
    SceneDocumentStore accepted(limits);
    CHECK(accepted.replace(document));
    CHECK(accepted.epoch() == 1);
    CHECK(encoded(accepted.document(), limits) == bytes);

    auto rejected_limits = limits;
    --(section_limit ? rejected_limits.max_section_bytes : rejected_limits.max_archive_bytes);
    CHECK(validate_scene_document(document, rejected_limits).error == DocumentError::Limit);
    const std::vector<std::byte> sentinel{std::byte{0x42}, std::byte{0x17}};
    auto destination = sentinel;
    CHECK(encode_scene_document(document, destination, rejected_limits).error == DocumentError::Limit);
    CHECK(destination == sentinel);

    SceneDocument retained;
    retained.revision = 19;
    const auto retained_bytes = encoded(retained, rejected_limits);
    SceneDocumentStore rejected(rejected_limits);
    CHECK(rejected.replace(retained));
    const auto epoch = rejected.epoch();
    CHECK(rejected.replace(document).error == DocumentError::Limit);
    CHECK(rejected.epoch() == epoch);
    CHECK(encoded(rejected.document(), rejected_limits) == retained_bytes);
    auto transaction = rejected.begin();
    CHECK(transaction.replace(document).error == DocumentError::Limit);
    CHECK(transaction.active());
    CHECK(encoded(transaction.document(), rejected_limits) == retained_bytes);
    CHECK(decode_scene_document(bytes, retained, rejected_limits).error == DocumentError::Limit);
    CHECK(encoded(retained, rejected_limits) == retained_bytes);

    ge::ecs::Registry registry;
    Scene scene(registry);
    const auto existing = scene.create("existing");
    CHECK(existing);
    CHECK(scene.update().error == SceneError::None);
    const auto scene_revision = scene.revision();
    const auto registry_revision = registry.revision();
    const auto failed = import_scene_document(document, scene, {}, rejected_limits);
    CHECK(failed.status.error == DocumentError::Limit);
    CHECK(failed.nodes.empty());
    CHECK(scene.size() == 1 && registry.size() == 1);
    CHECK(scene.revision() == scene_revision && registry.revision() == registry_revision);
    CHECK(scene.contains(existing) && registry.name(existing) == "existing");
    const auto imported = import_scene_document(document, scene, {}, limits);
    CHECK(imported);
    CHECK(imported.nodes.size() == document.snapshot.nodes.size());
    CHECK(scene.size() == 1 + document.snapshot.nodes.size());
    CHECK(scene.contains(existing) && registry.name(existing) == "existing");
}

void encoded_sections_and_archive_obey_exact_budgets() {
    SceneDocument manifest;
    manifest.name = "n";
    manifest.metadata.emplace("k", "value");
    SceneArchiveLimits limits;
    limits.max_section_bytes = 39; // 24 fixed + name + two string lengths + key/value.
    check_budget_boundary(manifest, limits, true);

    SceneDocument nodes;
    SnapshotNode node;
    node.name = "node";
    node.bounds = ge::math::Aabb{{-1, -2, -3}, {1, 2, 3}};
    node.tags = {"one", "two"};
    nodes.snapshot.nodes.push_back(node);
    limits.max_section_bytes = 180; // 4 count + 110 fixed + 4 name + 48 bounds + 14 tags.
    check_budget_boundary(nodes, limits, true);

    SceneDocument extension;
    extension.extensions.push_back({first_scene_extension, 1, false,
                                    std::vector<std::byte>(32, std::byte{0x5a})});
    limits.max_section_bytes = 36;
    check_budget_boundary(extension, limits, true);

    auto combined = manifest;
    combined.snapshot = nodes.snapshot;
    combined.extensions = extension.extensions;
    limits.max_section_bytes = 180;
    limits.max_archive_bytes = 427; // Envelope/header/checksum + 3 entries + all sections.
    CHECK(encoded(combined).size() == limits.max_archive_bytes);
    check_budget_boundary(combined, limits, false);
}

void minimum_archive_capacity_is_savable() {
    for (const std::size_t capacity : {165U, 166U, 167U}) {
        SceneArchiveLimits limits;
        limits.max_archive_bytes = capacity;
        CHECK(validate_scene_document({}, limits).error == DocumentError::Limit);
        CHECK_THROWS_AS(SceneDocumentStore(limits), std::invalid_argument);
    }
    SceneArchiveLimits limits;
    limits.max_archive_bytes = 168;
    SceneDocumentStore store(limits);
    std::vector<std::byte> bytes;
    CHECK(store.encode(bytes));
    CHECK(bytes.size() == 168);
    CHECK(store.decode(bytes));
    CHECK(store.epoch() == 1);
    CHECK(encoded(store.document(), limits) == bytes);
}

void truncated_fixed_node_is_rejected_before_record_decode() {
    SceneDocument document;
    document.snapshot.nodes.emplace_back();
    const auto bytes = encoded(document);
    auto archive = ge::core::decode_archive(std::span<const std::byte>(bytes).subspan(32));
    auto* nodes = archive.find(2);
    CHECK(nodes && nodes->data.size() == 114);
    nodes->data.pop_back(); // One node has only 109 of its 110 mandatory bytes.
    const auto payload = ge::core::encode_archive(archive);
    ge::core::ByteWriter writer;
    writer.write_bytes("GESCENE1");
    writer.write_u16(1);
    writer.write_u16(0);
    writer.write_u32(0);
    writer.write_u64(static_cast<std::uint64_t>(payload.size()));
    writer.write_u32(ge::core::crc32(payload));
    writer.write_u32(0);
    writer.write_bytes(payload);
    SceneDocument retained;
    retained.name = "retained";
    const auto retained_bytes = encoded(retained);
    CHECK(decode_scene_document(writer.bytes(), retained).error == DocumentError::Limit);
    CHECK(encoded(retained) == retained_bytes);
}

SceneDocument hierarchy(std::array<std::uint64_t, 3> identities, std::size_t parent) {
    SceneDocument document;
    for (const auto identity : identities) {
        SnapshotNode node;
        node.persistent = ge::PersistentId{identity};
        document.snapshot.nodes.push_back(node);
    }
    document.snapshot.nodes.back().parent_index = parent;
    return document;
}

void check_parent_diff(const SceneDocument& before, const SceneDocument& after, bool moved) {
    SceneDocumentPatch patch;
    CHECK(diff_scene_documents(before, after, patch));
    const auto reverse = reverse_scene_patch(patch);
    for (const auto* summary : std::array<const SceneDocumentPatch*, 2>{&patch, &reverse}) {
        const auto changes = std::count_if(summary->changes.begin(), summary->changes.end(),
            [](const SceneNodeChange& change) {
                return change.persistent == ge::PersistentId{3} &&
                    change.kind == SceneNodeChangeKind::Moved &&
                    change.before_index == 2 && change.after_index == 2;
            });
        CHECK(changes == (moved ? 1 : 0));
        CHECK(std::none_of(summary->changes.begin(), summary->changes.end(),
            [](const SceneNodeChange& change) {
                return change.persistent == ge::PersistentId{3} &&
                    change.kind == SceneNodeChangeKind::Modified;
            }));
    }
    auto applied = before;
    CHECK(apply_scene_patch(applied, patch));
    CHECK(encoded(applied) == encoded(after));
    CHECK(apply_scene_patch(applied, reverse));
    CHECK(encoded(applied) == encoded(before));
}

void diffs_follow_parent_identity_and_preserve_ordinal_fallback() {
    const auto before = hierarchy({1, 2, 3}, 0);
    check_parent_diff(before, hierarchy({2, 1, 3}, 0), true); // Parent ID changed at same index.
    check_parent_diff(before, hierarchy({2, 1, 3}, 1), true); // Same ID; ordinal move is conservative.
    check_parent_diff(before, before, false);
    const auto anonymous = hierarchy({0, 0, 3}, 0);
    check_parent_diff(anonymous, anonymous, false);
    check_parent_diff(anonymous, hierarchy({0, 0, 3}, 1), true);
    check_parent_diff(anonymous, hierarchy({1, 0, 3}, 0), true);
    check_parent_diff(hierarchy({0, 0, 3}, no_parent), anonymous, true);
    auto named = anonymous;
    named.snapshot.nodes[0].name = "first";
    named.snapshot.nodes[1].name = "second";
    auto reordered = named;
    std::swap(reordered.snapshot.nodes[0], reordered.snapshot.nodes[1]);
    check_parent_diff(named, reordered, false); // Anonymous parents retain ordinal matching.
}

} // namespace

int main() {
    return ge::test::run("serialization_audit", {
        {"encoded_sections_and_archive_obey_exact_budgets", encoded_sections_and_archive_obey_exact_budgets},
        {"minimum_archive_capacity_is_savable", minimum_archive_capacity_is_savable},
        {"truncated_fixed_node_is_rejected_before_record_decode", truncated_fixed_node_is_rejected_before_record_decode},
        {"diffs_follow_parent_identity_and_preserve_ordinal_fallback", diffs_follow_parent_identity_and_preserve_ordinal_fallback},
    });
}
