#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/render/graph.hpp>

#include <algorithm>

namespace {

void check_rejected_depth_import(ge::render::GraphLimits limits,
                                 ge::render::Framebuffer& framebuffer) {
    ge::render::RenderGraph graph(limits);
    const auto depth = graph.import_depth("depth", framebuffer);
    CHECK(depth);
    CHECK(graph.set_output("depth", depth));
    int callbacks = 0;
    CHECK(graph.add_pass({"read", {{depth, ge::render::GraphAccess::Read}}, {}},
        [&callbacks](ge::render::GraphContext&) {
            ++callbacks;
            return ge::render::GraphPassResult{};
        }));

    const auto compiled = graph.compile();
    CHECK(compiled.error == ge::render::GraphError::InvalidImport);
    CHECK(compiled.resource == depth);
    CHECK(!graph.compiled());
    CHECK(graph.schedule().empty());
    CHECK(graph.execute({}).error == ge::render::GraphError::InvalidImport);
    CHECK(callbacks == 0);
    CHECK(graph.output_depth("depth", true).empty());
}

void depth_imports_obey_dimension_and_pixel_limits() {
    ge::render::GraphLimits limits;
    limits.images.max_dimension = 2;
    limits.images.max_pixels = 4;
    ge::render::Framebuffer too_wide(3, 1);
    check_rejected_depth_import(limits, too_wide);

    limits.images.max_dimension = 4;
    limits.images.max_pixels = 3;
    ge::render::Framebuffer too_many_pixels(2, 2);
    check_rejected_depth_import(limits, too_many_pixels);
}

void resized_depth_is_revalidated_before_callbacks_and_output_publication() {
    ge::render::GraphLimits limits;
    limits.images.max_dimension = 2;
    limits.images.max_pixels = 4;
    ge::render::Framebuffer framebuffer(2, 2);
    ge::render::RenderGraph graph(limits);
    const auto depth = graph.import_depth("depth", framebuffer);
    CHECK(depth);
    CHECK(graph.set_output("depth", depth));
    int callbacks = 0;
    CHECK(graph.add_pass({"write", {{depth, ge::render::GraphAccess::Write}}, {}},
        [depth, &callbacks](ge::render::GraphContext& context) {
            ++callbacks;
            const auto values = context.write_depth(depth);
            std::fill(values.begin(), values.end(), 0.25);
            return ge::render::GraphPassResult{true, true, {}};
        }));
    CHECK(graph.execute({1, 0.0, 0.0}));
    CHECK(callbacks == 1);
    CHECK(graph.output_depth("depth").size() == 4);
    CHECK(graph.output_depth("depth").front() == 0.25);

    CHECK(framebuffer.resize(3, 1));
    const auto rejected = graph.execute({2, 0.0, 0.0});
    CHECK(rejected.error == ge::render::GraphError::InvalidImport);
    CHECK(rejected.resource == depth);
    CHECK(!rejected.partial_output);
    CHECK(!rejected.imported_output_changed);
    CHECK(callbacks == 1);
    CHECK(graph.statistics().completed_passes == 0);
    CHECK(graph.output_depth("depth").empty());
    CHECK(graph.output_depth("depth", true).empty());
    CHECK(!graph.resource_info(depth)->initialized);
    CHECK(std::all_of(framebuffer.depth().begin(), framebuffer.depth().end(),
                      [](double value) { return value == 1.0; }));

    CHECK(framebuffer.resize(2, 2));
    CHECK(graph.execute({3, 0.0, 0.0}));
    CHECK(callbacks == 2);
    CHECK(graph.output_depth("depth").size() == 4);
    CHECK(graph.output_depth("depth").front() == 0.25);
}

void optional_empty_depth_remains_a_valid_import() {
    ge::render::Framebuffer framebuffer(2, 2, false);
    ge::render::RenderGraph graph;
    const auto depth = graph.import_depth("depth", framebuffer);
    CHECK(depth);
    CHECK(graph.set_output("depth", depth));
    CHECK(graph.execute({}));
    CHECK(graph.resource_info(depth)->initialized);
    CHECK(graph.output_depth("depth").empty());
}

} // namespace

int main() {
    return ge::test::run("render_graph_audit", {
        {"depth import extent limits", depth_imports_obey_dimension_and_pixel_limits},
        {"resized depth validation and recovery", resized_depth_is_revalidated_before_callbacks_and_output_publication},
        {"optional empty depth", optional_empty_depth_remains_a_valid_import},
    });
}
