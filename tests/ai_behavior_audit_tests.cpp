#include "test_support.hpp"

#include <ge/ai/behavior.hpp>
#include <ge/core/runtime.hpp>

namespace {

using namespace ge;

struct Fixture {
    core::Kernel kernel;
    ecs::Registry& registry = kernel.services().emplace<ecs::Registry>(16);
    ai::BehaviorService behavior{kernel};
    ecs::Entity entity = registry.create();

    ~Fixture() { behavior.shutdown(); }

    void attach(ai::BehaviorTreeData data) {
        const auto tree = ai::BehaviorTree::create(std::move(data));
        CHECK(tree);
        CHECK(behavior.install("test", tree.tree));
        CHECK(behavior.attach(entity, "test", false));
    }
};

ai::BehaviorNode custom(std::string operation) {
    ai::BehaviorNode node;
    node.kind = ai::BehaviorNodeKind::Custom;
    node.operation = std::move(operation);
    return node;
}

void callbacks_cannot_continue_after_attachment_loss() {
    for (int loss = 0; loss < 3; ++loss) {
        for (int phase = 0; phase < 3; ++phase) {
            int ticks = 0;
            int exits = 0;
            int later_ticks = 0;
            Fixture fixture;
            const auto invalidate = [&] {
                if (loss == 0) {
                    CHECK(fixture.registry.remove<ai::BehaviorComponent>(fixture.entity));
                } else if (loss == 1) {
                    const auto copy = fixture.registry.require<ai::BehaviorComponent>(fixture.entity);
                    fixture.registry.emplace_or_replace<ai::BehaviorComponent>(fixture.entity, copy);
                } else {
                    CHECK(fixture.registry.destroy(fixture.entity));
                }
            };
            CHECK(fixture.behavior.register_action("first", {
                [&](ai::BehaviorActionContext&) {
                    if (phase == 0) { invalidate(); }
                    return true;
                },
                [&](ai::BehaviorActionContext&) {
                    ++ticks;
                    if (phase == 1) { invalidate(); }
                    return ai::BehaviorStatus::Success;
                },
                [&](ai::BehaviorActionContext&, ai::BehaviorStatus, ai::AbortReason) {
                    ++exits;
                    if (phase == 2) { invalidate(); }
                },
            }));
            CHECK(fixture.behavior.register_action("later", {
                {},
                [&](ai::BehaviorActionContext&) {
                    ++later_ticks;
                    return ai::BehaviorStatus::Success;
                },
                [](ai::BehaviorActionContext&, ai::BehaviorStatus, ai::AbortReason) {},
            }));
            ai::BehaviorNode sequence;
            sequence.kind = ai::BehaviorNodeKind::Sequence;
            sequence.children = {1, 2};
            fixture.attach({{sequence, custom("first"), custom("later")}, 0});
            fixture.behavior.advance({1, 0.1, 0.1});
            CHECK(ticks == (phase == 0 ? 0 : 1));
            CHECK(exits == 1);
            CHECK(later_ticks == 0);
            CHECK(!fixture.behavior.inspect(fixture.entity));
            CHECK(fixture.behavior.statistics().attachments == 0);
            CHECK(fixture.behavior.statistics().removed_stale == 1);
            CHECK(fixture.behavior.statistics().callback_faults == 0);
            const auto history = fixture.behavior.events_since(0);
            CHECK(history.events.size() == 2);
            CHECK(history.events.back().status == ai::BehaviorStatus::Aborted);
            CHECK(history.events.back().reason == ai::AbortReason::EntityLost);
            if (loss == 1) {
                CHECK(fixture.registry.require<ai::BehaviorComponent>(fixture.entity).status ==
                      ai::BehaviorStatus::Idle);
            }
            fixture.behavior.advance({2, 0.1, 0.2});
            CHECK(exits == 1);
        }
    }
}

void replacing_a_marker_does_not_transfer_running_action_ownership() {
    int ticks = 0;
    int exits = 0;
    ai::BehaviorStatus outcome = ai::BehaviorStatus::Idle;
    ai::AbortReason reason = ai::AbortReason::Completed;
    Fixture fixture;
    CHECK(fixture.behavior.register_action("running", {
        {},
        [&](ai::BehaviorActionContext&) { ++ticks; return ai::BehaviorStatus::Running; },
        [&](ai::BehaviorActionContext&, ai::BehaviorStatus status, ai::AbortReason why) {
            ++exits;
            outcome = status;
            reason = why;
        },
    }));
    fixture.attach({{custom("running")}, 0});
    fixture.behavior.advance({1, 0.1, 0.1});
    const auto copy = fixture.registry.require<ai::BehaviorComponent>(fixture.entity);
    fixture.registry.emplace_or_replace<ai::BehaviorComponent>(fixture.entity, copy);
    fixture.behavior.advance({2, 0.1, 0.2});
    CHECK(ticks == 1);
    CHECK(exits == 1);
    CHECK(outcome == ai::BehaviorStatus::Aborted);
    CHECK(reason == ai::AbortReason::EntityLost);
    CHECK(!fixture.behavior.inspect(fixture.entity));
    CHECK(fixture.registry.require<ai::BehaviorComponent>(fixture.entity).status == copy.status);
    CHECK(fixture.behavior.statistics().attachments == 0);
}

} // namespace

int main() {
    return test::run("ai_behavior_audit", {
        {"callback attachment loss", callbacks_cannot_continue_after_attachment_loss},
        {"replacement marker ownership", replacing_a_marker_does_not_transfer_running_action_ownership},
    });
}
