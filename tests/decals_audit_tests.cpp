#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/render/decals.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace {

using namespace ge;

struct GraphFixture {
    ecs::Registry registry;
    scene::Scene scene{registry};
    render::Framebuffer framebuffer{8, 8};
    core::Kernel kernel;
    render::RenderGraph& graph{kernel.services().emplace<render::RenderGraph>()};

    GraphFixture() { install_anchors(); }

    void install_anchors() {
        const auto color = graph.import_image(std::string(render::graph_main_color), framebuffer.color());
        const auto depth = graph.import_depth(std::string(render::graph_main_depth), framebuffer);
        CHECK(color && depth);
        const auto raster = graph.add_pass({std::string(render::graph_scene_pass),
            {{color, render::GraphAccess::Write}, {depth, render::GraphAccess::Write}}, {}},
            [](render::GraphContext&) { return render::GraphPassResult{}; });
        CHECK(raster);
        CHECK(graph.add_pass({std::string(render::graph_complete_pass), {}, {raster}},
            [](render::GraphContext&) { return render::GraphPassResult{}; }));
    }
};

void unrelated_graph_detach_preserves_the_owner() {
    GraphFixture fixture;
    render::DecalService decals{fixture.scene};
    CHECK(decals.install_graph(fixture.kernel) == render::DecalError::None);
    const auto pass = *fixture.graph.find_pass(render::graph_decals_pass);
    const auto revision = fixture.graph.pass_info(pass)->callback_revision;
    render::RenderGraph unrelated;
    CHECK(!decals.detach(unrelated));
    CHECK(decals.attachment_error() == render::DecalError::GraphConflict);
    CHECK(decals.attached(fixture.graph));
    CHECK(!decals.attached(unrelated));
    CHECK(fixture.graph.pass_info(pass)->callback_revision == revision);
    CHECK(decals.detach(fixture.graph));
    CHECK(decals.attachment_error() == render::DecalError::None);
    CHECK(!decals.attached(fixture.graph));
    CHECK(decals.install_graph(fixture.kernel) == render::DecalError::None);
    CHECK(decals.attached(fixture.graph));
}

void owning_graph_reset_recovers_and_stolen_callbacks_survive() {
    GraphFixture fixture;
    render::DecalService decals{fixture.scene};
    CHECK(decals.install_graph(fixture.kernel) == render::DecalError::None);
    CHECK(fixture.graph.clear());
    CHECK(decals.detach(fixture.graph));
    CHECK(decals.attachment_error() == render::DecalError::None);
    fixture.install_anchors();
    CHECK(decals.install_graph(fixture.kernel) == render::DecalError::None);
    CHECK(decals.attached(fixture.graph));
    const auto pass = *fixture.graph.find_pass(render::graph_decals_pass);
    bool replacement_called{};
    CHECK(fixture.graph.replace_callback(pass, [&](render::GraphContext&) {
        replacement_called = true;
        return render::GraphPassResult{};
    }));
    CHECK(!decals.detach(fixture.graph));
    CHECK(decals.attachment_error() == render::DecalError::GraphConflict);
    CHECK(decals.install_graph(fixture.kernel) == render::DecalError::GraphConflict);
    CHECK(fixture.graph.compile());
    CHECK(fixture.graph.execute({}));
    CHECK(replacement_called);
}

void destroyed_owner_leaves_a_safe_callback() {
    GraphFixture fixture;
    auto decals = std::make_unique<render::DecalService>(fixture.scene);
    CHECK(decals->install_graph(fixture.kernel) == render::DecalError::None);
    decals.reset();
    CHECK(fixture.graph.compile());
    CHECK(fixture.graph.execute({}));
}

void projection_clips_regions_and_rejects_stale_materials_atomically() {
    for (const auto clip_depth : {math::ClipDepth::zero_to_one, math::ClipDepth::negative_one_to_one}) {
        for (const auto mode : {render::ProjectionMode::Perspective, render::ProjectionMode::Orthographic}) {
            ecs::Registry registry;
            scene::Scene scene{registry};
            render::DecalService decals{scene};
            render::TextureLibrary textures;
            render::MaterialLibrary materials;
            render::PbrMaterial material;
            material.albedo = {1.0, 0.0, 0.0, 1.0};
            const auto material_handle = materials.create("decal", material);
            CHECK(material_handle);
            render::DecalProjector projector;
            projector.half_extent = {10.0, 10.0, 3.0};
            projector.edge_fade = {};
            projector.facing_fade = false;
            projector.material = material_handle;
            math::Transform transform;
            transform.position = {0.0, 0.0, -0.5};
            const auto handle = decals.create("near crossing", transform, projector);
            CHECK(handle);
            render::CameraView camera;
            camera.viewport = {0, 0, 8, 8};
            camera.clip_depth = clip_depth;
            camera.projection_mode = mode;
            camera.visibility_mask = ~std::uint64_t{};
            render::CameraProjection projection;
            projection.clip_depth = clip_depth;
            projection.mode = mode;
            camera.projection = *render::camera_projection(projection, camera.viewport);
            CHECK(render::refresh_camera_matrices(camera, math::Mat4{}));
            const auto clip = camera.projection * math::Vec4{0.0, 0.0, -2.0, 1.0};
            const double depth = clip_depth == math::ClipDepth::zero_to_one
                ? clip.z / clip.w : (clip.z / clip.w + 1.0) * 0.5;
            const std::array views{render::DecalView{camera, {2, 2, 4, 4}}};
            const render::Color background{0.2, 0.3, 0.4, 0.37};
            render::Image image{8, 8, background};
            std::array<double, 64> samples;
            samples.fill(depth);
            CHECK(decals.process(image, samples, views, textures, &materials));
            CHECK(decals.statistics().blended_pixels == 16);
            CHECK(image.at(3, 3) == render::Color{1.0, 0.0, 0.0, background.a});
            CHECK(image.at(1, 3) == background);
            CHECK(std::all_of(samples.begin(), samples.end(), [depth](double value) { return value == depth; }));

            CHECK(materials.erase(material_handle));
            const auto replacement = materials.create("replacement", {});
            CHECK(replacement && replacement != material_handle);
            image.clear(background);
            CHECK(decals.process(image, samples, views, textures, &materials).error == render::DecalError::MissingMaterial);
            CHECK(std::all_of(image.pixels().begin(), image.pixels().end(),
                [background](render::Color pixel) { return pixel == background; }));

            CHECK(registry.remove<render::DecalComponent>(decals.entity(handle)));
            CHECK(!decals.component(handle));
            CHECK(decals.process(image, samples, views, textures, &materials));
            CHECK(!decals.statistics().image_changed);
        }
    }
}

} // namespace

int main() {
    return ge::test::run("decals_audit", {
        {"unrelated graph detach", unrelated_graph_detach_preserves_the_owner},
        {"reset and callback ownership", owning_graph_reset_recovers_and_stolen_callbacks_survive},
        {"destroyed callback owner", destroyed_owner_leaves_a_safe_callback},
        {"projection and material lifetime", projection_clips_regions_and_rejects_stale_materials_atomically},
    });
}
