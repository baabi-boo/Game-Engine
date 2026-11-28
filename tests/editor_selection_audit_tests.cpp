#include "test_support.hpp"

#include <ge/editor/selection.hpp>
#include <ge/scene/scene.hpp>

#include <array>

namespace {

void registry_transform_edits_invalidate_pending_picks() {
    for (const bool initially_hit : {false, true}) {
        ge::ecs::Registry registry;
        ge::scene::Scene scene{registry};
        ge::geometry::MeshLibrary meshes;
        ge::editor::SelectionService selection{scene};
        const auto retained = scene.create("retained selection");
        ge::math::Transform pose;
        pose.position = {initially_hit ? 0.0 : 10.0, 0.0, -5.0};
        const auto target = scene.create("pick target", {}, pose);
        CHECK(retained && target);
        CHECK(scene.set_local_bounds(target, {{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}}));
        CHECK(selection.apply(std::array{retained}));
        CHECK(selection.set_hover(retained));
        const auto before = selection.snapshot();
        const auto history_before = selection.statistics();

        const auto pending = selection.raycast({{}, {0.0, 0.0, -1.0}}, meshes);
        CHECK(pending.report.complete);
        CHECK(pending.hits.size() == (initially_hit ? 1U : 0U));
        if (initially_hit) { CHECK(pending.hits.front().entity == target); }

        // Registry local transforms are authoritative inputs that Scene discovers
        // on synchronization, including edits without an explicit dirty marker.
        registry.require<ge::ecs::TransformComponent>(target).local.position.x = initially_hit ? 10.0 : 0.0;
        CHECK(scene.revision() == pending.report.scene_revision);
        const auto rejected = selection.choose(pending);
        CHECK(rejected.error == ge::editor::SelectionError::Incomplete);
        CHECK(!rejected.changed);
        CHECK(scene.revision() > pending.report.scene_revision);
        const auto after = selection.snapshot();
        CHECK(after.entities == before.entities);
        CHECK(after.primary == before.primary);
        CHECK(after.hover == before.hover);
        CHECK(after.revision == before.revision);
        CHECK(selection.statistics().undo_entries == history_before.undo_entries);
        CHECK(selection.statistics().redo_entries == history_before.redo_entries);

        const auto fresh = selection.raycast({{}, {0.0, 0.0, -1.0}}, meshes);
        CHECK(fresh.report.complete);
        CHECK(fresh.hits.size() == (initially_hit ? 0U : 1U));
        CHECK(selection.choose(fresh));
        CHECK(selection.selected().size() == (initially_hit ? 0U : 1U));
        if (!initially_hit) { CHECK(selection.primary() == target); }
    }
}

} // namespace

int main() {
    return ge::test::run("editor_selection_audit", {
        {"registry_transform_edits_invalidate_pending_picks", registry_transform_edits_invalidate_pending_picks},
    });
}
