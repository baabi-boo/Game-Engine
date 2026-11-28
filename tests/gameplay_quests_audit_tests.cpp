#include "test_support.hpp"

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/gameplay/quests.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace ge;
using namespace ge::gameplay;

void check_owned_cleanup(std::string_view reason, std::size_t mission_text_bytes) {
    for (const bool fail_quest : {false, true}) {
        core::Kernel kernel;
        auto& registry = kernel.services().emplace<ecs::Registry>(8);
        MissionLimits limits;
        limits.name_bytes = 1;
        limits.text_bytes = mission_text_bytes;
        auto& missions = kernel.services().emplace<MissionService>(kernel, registry, limits);
        QuestService quests(kernel, registry);
        const PersistentId identity{1};
        const auto actor = registry.create(identity);

        MissionDefinition mission;
        mission.id = "m";
        MissionObjectiveDefinition objective;
        objective.id = "o";
        mission.objectives.push_back(objective);
        CHECK(missions.define(std::move(mission)));

        QuestDefinition quest;
        quest.id = "q";
        QuestStageDefinition stage;
        stage.id = "s";
        stage.mission = "m";
        quest.stages.push_back(stage);
        CHECK(quests.define(std::move(quest)));
        CHECK(quests.accept(actor, "q"));
        quests.update({1, 0.1, 0.1});
        const auto run_id = quests.inspect(identity, "q")->stages.front().mission_run;
        CHECK(run_id != 0);
        CHECK(missions.inspect(run_id)->status == MissionStatus::Active);

        CHECK(fail_quest ? quests.fail(identity, "q", reason)
                         : quests.abandon(identity, "q", QuestTerminationPolicy::CancelOwned, reason));
        CHECK(quests.inspect(identity, "q")->cleanup_pending);
        quests.update({2, 0.1, 0.2});

        const auto run = missions.inspect(run_id);
        CHECK(run);
        CHECK(run->status == (fail_quest ? MissionStatus::Failed : MissionStatus::Cancelled));
        CHECK(!run->reason.empty());
        CHECK(run->reason.size() <= mission_text_bytes);
        CHECK(core::valid_utf8(run->reason));
        const auto record = quests.inspect(identity, "q");
        CHECK(record);
        CHECK(record->status == (fail_quest ? QuestStatus::Failed : QuestStatus::Abandoned));
        CHECK(record->reason == reason);
        CHECK(!record->cleanup_pending);
    }
}

void long_reasons_do_not_block_owned_cleanup() {
    check_owned_cleanup(std::string(600, 'x'), MissionLimits{}.text_bytes);
}

void cleanup_reasons_preserve_utf8_and_nonempty_failure_text() {
    check_owned_cleanup(std::string(511, 'x') + "\xE2\x82\xAC", MissionLimits{}.text_bytes);
    check_owned_cleanup("\xE2\x82\xAC", 1);
}

} // namespace

int main() {
    return ge::test::run("gameplay_quests_audit", {
        {"long reasons do not block owned cleanup", long_reasons_do_not_block_owned_cleanup},
        {"cleanup diagnostics respect UTF-8 bounds", cleanup_reasons_preserve_utf8_and_nonempty_failure_text},
    });
}
