#include "test_support.hpp"

#include <ge/traffic/roads.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace {

using namespace ge::traffic;

RoadDescriptor straight(std::string name, ge::math::Vec3 from, ge::math::Vec3 to) {
    RoadDescriptor descriptor;
    descriptor.name = std::move(name);
    CHECK(descriptor.centerline.assign({{0.0, from}, {1.0, to}}));
    descriptor.lanes.emplace_back();
    descriptor.sample_spacing = 1.0;
    return descriptor;
}

void replacement_reclaims_incident_connector_samples() {
    RoadLimits limits;
    limits.max_roads = 2;
    limits.max_lanes = 3;
    limits.max_total_samples = 10;
    RoadNetwork roads{limits};
    const auto first = roads.create(straight("first", {}, {0.0, 0.0, -1.0}));
    const auto second = roads.create(straight("second", {0.0, 0.0, -1.0}, {0.0, 0.0, -2.0}));
    CHECK(first && second);
    const auto from = roads.lanes(first).front();
    const auto to = roads.lanes(second).front();
    CHECK(roads.connect(from, to));
    CHECK(roads.statistics().samples == limits.max_total_samples);
    const auto path = roads.route(from, 0.0, to, 1.0);
    CHECK(path);
    const auto revision = roads.revision();
    CHECK(roads.replace(first, straight("first", {}, {0.0, 0.0, -3.0})).error == RoadError::Capacity);
    CHECK(roads.revision() == revision);
    CHECK(roads.route_current(path));
    CHECK(roads.connection(from, to));
    CHECK(roads.replace(first, straight("first", {}, {0.0, 0.0, -2.0})));
    CHECK(roads.statistics().samples == limits.max_total_samples);
    CHECK(roads.statistics().connections == 0);
    CHECK(!roads.lane(from));
    CHECK(!roads.route_current(path));
    CHECK(roads.lanes(first).front() != from);
    CHECK(roads.lane(to));
}

void partial_lane_time_budget_matches_returned_route() {
    RoadNetwork roads;
    auto curved = straight("curved", {}, {10.0, 0.0, -14.0});
    CHECK(curved.centerline.assign({{0.0, {}}, {0.25, {0.0, 0.0, -5.0}},
        {0.5, {1.0, 0.0, -10.0}}, {0.75, {5.0, 0.0, -14.0}}, {1.0, {10.0, 0.0, -14.0}}}));
    const auto first = roads.create(std::move(curved));
    const auto second = roads.create(straight("second", {10.0, 0.0, -14.0}, {11.0, 0.0, -14.0}));
    CHECK(first && second);
    const auto from = roads.lanes(first).front();
    const auto to = roads.lanes(second).front();
    CHECK(roads.connect(from, to));
    const auto samples = roads.samples(from);
    double begin = -1.0;
    for (std::size_t index = 1; index < samples.size(); ++index) {
        if (std::abs(samples[index].recommended_speed - samples[index - 1].recommended_speed) > 1.0) {
            begin = (samples[index].distance + samples[index - 1].distance) * 0.5;
            break;
        }
    }
    CHECK(begin >= 0.0);
    const auto unrestricted = roads.route(from, begin, to, 0.0);
    CHECK(unrestricted);
    RouteOptions options;
    options.max_seconds = unrestricted.travel_seconds + 1.0e-6;
    const auto bounded = roads.route(from, begin, to, 0.0, options);
    CHECK(bounded);
    CHECK(bounded.travel_seconds == unrestricted.travel_seconds);
    options.max_seconds = unrestricted.travel_seconds - 1.0e-6;
    CHECK(roads.route(from, begin, to, 0.0, options).error == RoadError::RouteBudget);
}

} // namespace

int main() {
    return ge::test::run("traffic_roads_audit", {
        {"replacement_reclaims_incident_connector_samples", replacement_reclaims_incident_connector_samples},
        {"partial_lane_time_budget_matches_returned_route", partial_lane_time_budget_matches_returned_route},
    });
}
