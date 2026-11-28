#include <ge/core/runtime.hpp>
#include <ge/gameplay/dialogue.hpp>

#include "test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
using namespace ge::gameplay;

DialogueGraph provider_graph() {
    DialogueGraph graph;
    graph.id = "retirement";
    graph.entry = "end";
    DialogueNode end;
    end.id = "end";
    end.kind = DialogueNodeKind::End;
    DialogueEffect effect;
    effect.kind = DialogueEffectKind::Provider;
    effect.key = "work";
    effect.provider = "async";
    end.effects.push_back(effect);
    graph.nodes.push_back(end);
    return graph;
}

void pending_provider_cancellation_survives_session_release() {
    ge::core::Kernel kernel;
    ge::ecs::Registry registry(4);
    DialogueService service(kernel, registry);
    const auto actor = registry.create({1}, "Actor");
    CHECK(service.define(provider_graph()));
    CHECK(service.configure_delivery({71, 9}));

    std::vector<DialogueEffectRequest> requests;
    std::size_t cancellations{};
    CHECK(service.register_effect("async", [&](const DialogueEffectRequest& request) {
        requests.push_back(request);
        if (request.phase == DialogueEffectPhase::Apply) {
            return DialogueEffectResult{DialogueEffectOutcome::Pending, "admitted"};
        }
        ++cancellations;
        if (cancellations == 1) { return DialogueEffectResult{DialogueEffectOutcome::Pending, "cancelling"}; }
        if (cancellations == 2) { return DialogueEffectResult{DialogueEffectOutcome::Retry, "busy"}; }
        if (cancellations == 3) { throw std::runtime_error("cancellation unavailable"); }
        if (cancellations == 4) { return DialogueEffectResult{DialogueEffectOutcome::Rejected, "still running"}; }
        return DialogueEffectResult{DialogueEffectOutcome::Completed, "retired"};
    }));
    const auto started = service.start(actor, {}, "retirement");
    CHECK(started);
    service.update({1, 0.0, 0.0});
    service.update({2, 0.0, 0.0});
    CHECK(requests.size() == 1);
    const DialogueEffectId runtime_id{started.session.scope, 1};
    CHECK(service.receipt(runtime_id)->state == DialogueReceiptState::Pending);
    CHECK(service.cancel(started.session));
    CHECK(service.receipt(runtime_id)->state == DialogueReceiptState::Abandoned);
    CHECK(service.receipt(runtime_id)->cancellation_pending);
    CHECK(service.unregister_effect("async"));
    CHECK(service.release(started.session));

    for (std::uint64_t tick = 3; tick <= 5; ++tick) {
        service.update({tick, 0.0, 0.0});
        CHECK(service.receipt(runtime_id)->cancellation_pending);
    }
    service.update({6, 0.0, 0.0});
    CHECK(!service.receipt(runtime_id)->cancellation_pending);
    service.update({7, 0.0, 0.0});
    CHECK(cancellations == 5);
    CHECK(requests.size() == 6);
    CHECK(requests.front().phase == DialogueEffectPhase::Apply);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        CHECK(requests[index].id == (DialogueEffectId{71, 9}));
        CHECK(requests[index].context.session == started.session);
        if (index != 0) { CHECK(requests[index].phase == DialogueEffectPhase::Cancel); }
    }
    CHECK(service.receipt(runtime_id)->state == DialogueReceiptState::Abandoned);
    CHECK(service.delivery_cursor().next_sequence == 10);
}

void uncertain_apply_is_retired_without_reapplication() {
    ge::core::Kernel kernel;
    ge::ecs::Registry registry(4);
    DialogueService service(kernel, registry);
    const auto actor = registry.create({2}, "Actor");
    CHECK(service.define(provider_graph()));
    CHECK(service.configure_delivery({72, 1}));
    std::size_t applications{};
    std::size_t cancellations{};
    DialogueError reentrant_error{};
    CHECK(service.register_effect("async", [&](const DialogueEffectRequest& request) {
        reentrant_error = service.cancel(request.context.session).error;
        if (request.phase == DialogueEffectPhase::Apply) {
            ++applications;
            throw std::runtime_error("external outcome unknown");
        }
        ++cancellations;
        return DialogueEffectResult{
            cancellations == 1 ? DialogueEffectOutcome::Pending : DialogueEffectOutcome::Completed, {}};
    }));
    const auto started = service.start(actor, {}, "retirement");
    CHECK(started);
    service.update({1, 0.0, 0.0});
    service.update({2, 0.0, 0.0});
    CHECK(service.inspect(started.session)->state == DialogueState::Failed);
    CHECK(service.inspect(started.session)->error == DialogueError::EffectRejected);
    const DialogueEffectId runtime_id{started.session.scope, 1};
    CHECK(service.receipt(runtime_id)->cancellation_pending);
    service.update({3, 0.0, 0.0});
    service.update({4, 0.0, 0.0});
    CHECK(!service.receipt(runtime_id)->cancellation_pending);
    CHECK(applications == 1);
    CHECK(cancellations == 2);
    CHECK(reentrant_error == DialogueError::Busy);
}
} // namespace

int main() {
    return ge::test::run("gameplay_dialogue_audit", {
        {"pending_provider_cancellation_survives_session_release", pending_provider_cancellation_survives_session_release},
        {"uncertain_apply_is_retired_without_reapplication", uncertain_apply_is_retired_without_reapplication},
    });
}
