#include "test_support.hpp"

#include <ge/ai/perception.hpp>
#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>
#include <ge/spatial/index.hpp>

#include <array>

namespace {

struct Fixture {
    ge::ecs::Registry registry{8};
    ge::scene::Scene scene{registry};
    ge::spatial::SpatialIndex spatial{registry, scene};
    ge::ai::PerceptionService perception;
    std::array<ge::ai::SensorHandle, 2> sensors;

    explicit Fixture(ge::ai::PerceptionLimits limits, double interval = 0.0)
        : perception(registry, scene, limits) {
        ge::ai::SensorConfig config;
        config.eye_offset = {};
        config.interval_seconds = interval;
        config.sight_samples = 1;
        config.occlusion = ge::ai::OcclusionMode::ConservativeBounds;
        for (auto& sensor : sensors) {
            sensor = perception.attach(scene.create(), config);
            CHECK(sensor);
        }
    }

    void update(std::uint64_t tick) {
        perception.update({tick, 0.1, 0.1 * static_cast<double>(tick)},
                          &spatial, nullptr, nullptr, nullptr);
    }

    void target() {
        const auto entity = scene.create();
        CHECK(entity);
        CHECK(scene.set_local_position(entity, {0.0, 0.0, -5.0}));
        CHECK(scene.set_local_bounds(entity, {{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}}));
        CHECK(perception.set_target(entity, {}));
    }
};

void partial_sight_budgets_rotate_between_sensors() {
    for (const bool limit_candidates : {false, true}) {
        ge::ai::PerceptionLimits limits;
        limits.max_sensors = 4;
        if (limit_candidates) { limits.candidate_visits_per_frame = 1; }
        else { limits.line_queries_per_frame = 1; }
        Fixture fixture(limits);
        fixture.target();

        fixture.update(1);
        CHECK(fixture.perception.inspect(fixture.sensors[0])->memories.size() == 1);
        CHECK(fixture.perception.inspect(fixture.sensors[1])->memories.empty());
        CHECK(fixture.perception.decision(fixture.sensors[1])->deferred);
        fixture.update(2);
        const auto second = fixture.perception.inspect(fixture.sensors[1]);
        CHECK(second->memories.size() == 1);
        CHECK(second->memories.front().sight == ge::ai::SightState::Clear);
        CHECK(second->memories.front().sight_tick == 2);
        CHECK(fixture.perception.statistics().sensed == 2);
        CHECK(fixture.perception.statistics().line_queries == 1);
        if (limit_candidates) { CHECK(fixture.perception.statistics().candidates == 1); }
    }
}

void utility_budget_rotates_when_sensors_are_not_due() {
    ge::ai::PerceptionLimits limits;
    limits.max_sensors = 4;
    limits.utility_evaluations_per_frame = 1;
    Fixture fixture(limits, 10.0);
    ge::ai::UtilityRule investigate;
    investigate.action = ge::ai::UtilityAction::Investigate;
    for (const auto sensor : fixture.sensors) {
        CHECK(fixture.perception.set_rules(sensor, std::array{investigate}));
    }
    CHECK(fixture.perception.emit({}));

    fixture.update(1);
    CHECK(fixture.perception.decision(fixture.sensors[0])->has_position);
    CHECK(!fixture.perception.decision(fixture.sensors[1])->has_position);
    fixture.update(2);
    CHECK(fixture.perception.decision(fixture.sensors[1])->has_position);
    CHECK(fixture.perception.statistics().sensed == 0);
    CHECK(fixture.perception.statistics().utility_evaluations == 1);
}

void sensor_budget_resumes_at_the_first_deferred_sensor() {
    ge::ai::PerceptionLimits limits;
    limits.max_sensors = 4;
    limits.sensors_per_frame = 1;
    Fixture fixture(limits);
    fixture.target();
    fixture.update(1);
    CHECK(fixture.perception.inspect(fixture.sensors[1])->memories.empty());
    fixture.update(2);
    CHECK(fixture.perception.inspect(fixture.sensors[1])->memories.size() == 1);
    CHECK(fixture.perception.statistics().sensed == 1);
}

} // namespace

int main() {
    return ge::test::run("ai_perception_audit", {
        {"partial sight budgets rotate", partial_sight_budgets_rotate_between_sensors},
        {"utility rotates between samples", utility_budget_rotates_when_sensors_are_not_due},
        {"sensor budget resumes deferred work", sensor_budget_resumes_at_the_first_deferred_sensor},
    });
}
