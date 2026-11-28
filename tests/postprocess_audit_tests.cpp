#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/render/postprocess.hpp>

#include <cmath>
#include <string>

namespace {

using namespace ge::render;

struct GraphFixture {
    Image image{1, 1, Color::white()};
    RenderGraph graph;

    GraphFixture() { install(); }

    void install() {
        const auto color = graph.import_image(std::string(graph_main_color), image);
        CHECK(color);
        const auto scene = graph.add_pass({std::string(graph_scene_pass),
            {{color, GraphAccess::ReadWrite}}, {}, 0, true},
            [](GraphContext&) -> GraphPassResult { return {}; });
        CHECK(scene);
        CHECK(graph.add_pass({std::string(graph_complete_pass),
            {{color, GraphAccess::Read}}, {scene}, 0, true},
            [](GraphContext&) -> GraphPassResult { return {}; }));
        CHECK(graph.set_output(std::string(graph_main_color), color));
    }

    GraphResult execute() { return graph.execute({1, 0.016, 0.016}); }
};

void unrelated_detach_preserves_attachment_and_frame_output() {
    GraphFixture current;
    GraphFixture unrelated;
    PostprocessService service;
    CHECK(service.attach(current.graph) == PostprocessError::None);
    const auto pass = current.graph.find_pass(graph_postprocess_pass);
    CHECK(pass);
    const auto revision = current.graph.pass_info(*pass)->callback_revision;
    const auto* storage = current.image.pixels().data();

    CHECK(!service.detach(unrelated.graph));
    CHECK(service.attachment_error() == PostprocessError::GraphConflict);
    CHECK(service.attached(current.graph));
    CHECK(current.graph.pass_info(*pass)->callback_revision == revision);
    const auto processed = current.execute();
    CHECK(processed);
    CHECK(processed.imported_output_changed);
    CHECK(current.graph.output_image(graph_main_color) == &current.image);
    CHECK(current.image.pixels().data() == storage);
    CHECK(std::abs(current.image.at(0, 0).r - 0.5) < 1.0e-12);
    CHECK(service.statistics().completed == 1);

    CHECK(service.detach(current.graph));
    CHECK(service.attachment_error() == PostprocessError::None);
    CHECK(!service.attached(current.graph));
    current.image.clear(Color::white());
    const auto detached = current.execute();
    CHECK(detached);
    CHECK(!detached.imported_output_changed);
    CHECK(current.image.at(0, 0) == Color::white());
    CHECK(service.statistics().completed == 1);
    CHECK(service.attach(current.graph) == PostprocessError::None);
    CHECK(current.execute());
    CHECK(std::abs(current.image.at(0, 0).r - 0.5) < 1.0e-12);
}

void handoff_reset_and_destruction_preserve_callback_lifetimes() {
    GraphFixture previous;
    GraphFixture current;
    {
        PostprocessService service;
        CHECK(service.attach(previous.graph) == PostprocessError::None);
        CHECK(service.attach(current.graph) == PostprocessError::None);
        CHECK(!service.detach(previous.graph));
        CHECK(service.attached(current.graph));
        CHECK(previous.execute());
        CHECK(previous.image.at(0, 0) == Color::white());
        CHECK(current.execute());
        CHECK(std::abs(current.image.at(0, 0).r - 0.5) < 1.0e-12);
        CHECK(service.statistics().completed == 1);

        CHECK(current.graph.clear());
        CHECK(service.detach(current.graph));
        CHECK(service.attachment_error() == PostprocessError::None);
        CHECK(!service.attached(current.graph));
        current.install();
        CHECK(service.attach(current.graph) == PostprocessError::None);
    }
    current.image.clear(Color::white());
    const auto expired = current.execute();
    CHECK(expired);
    CHECK(!expired.imported_output_changed);
    CHECK(current.image.at(0, 0) == Color::white());

    PostprocessService survivor;
    {
        GraphFixture temporary;
        CHECK(survivor.attach(temporary.graph) == PostprocessError::None);
    }
    survivor.stop();
    CHECK(survivor.process(current.image, 0.016).error == PostprocessError::Stopped);
}

} // namespace

int main() {
    return ge::test::run("postprocess audit", {
        {"unrelated detach preserves attachment and frame output",
            unrelated_detach_preserves_attachment_and_frame_output},
        {"handoff, reset and destruction preserve callback lifetimes",
            handoff_reset_and_destruction_preserve_callback_lifetimes},
    });
}
