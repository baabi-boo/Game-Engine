#include "test_support.hpp"

#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/render/visibility.hpp>
#include <ge/scene/scene.hpp>
#include <ge/spatial/index.hpp>

namespace {

constexpr ge::math::Vec3 far_origin{1.0e11, -2.0e11, 3.0e11};

struct Fixture {
    ge::ecs::Registry registry;
    ge::scene::Scene scene{registry};
    ge::geometry::MeshLibrary meshes;
    ge::render::CameraService cameras{scene};
    ge::render::MaterialService materials;
    ge::render::SoftwareRenderer renderer;
    ge::render::VisibilityService visibility{scene};
    ge::spatial::SpatialIndex spatial{registry, scene};
    ge::geometry::MeshHandle mesh;
    ge::ecs::Entity camera;

    explicit Fixture(ge::math::ClipDepth depth) {
        CHECK(cameras.set_framebuffer_size(64, 48));
        ge::render::CameraComponent settings;
        settings.projection.clip_depth = depth;
        ge::math::Transform pose;
        pose.position = far_origin;
        camera = cameras.create("far camera", pose, settings);
        CHECK(camera);
        mesh = meshes.create(ge::geometry::make_box());
        auto raster_settings = renderer.settings();
        raster_settings.allow_stale_views = false;
        CHECK(renderer.configure(raster_settings));
    }

    ge::ecs::Entity box(ge::math::Vec3 offset) {
        ge::math::Transform pose;
        pose.position = far_origin + offset;
        const auto entity = scene.create("box", {}, pose);
        CHECK(entity);
        registry.emplace<ge::geometry::MeshComponent>(entity, ge::geometry::MeshComponent{mesh});
        return entity;
    }

    ge::render::CameraView sample() {
        cameras.update(0.0);
        CHECK(cameras.current_view());
        CHECK(!cameras.current_view()->stale);
        visibility.capture_camera_bases(cameras);
        return *cameras.current_view();
    }

    void check_selected(ge::ecs::Entity expected, bool indexed) const {
        CHECK(visibility.views().size() == 1);
        const auto* view = visibility.view(0);
        CHECK(view);
        CHECK(view->complete);
        CHECK(!view->camera.stale);
        CHECK(view->entries.size() == 1);
        CHECK(view->entries.front().draw.entity == expected);
        CHECK(view->query.used_index == indexed);
        CHECK(!view->query.recovered_by_direct_tests);
        CHECK(visibility.statistics().frustum_culled == 1);
    }
};

void far_origin_collection_keeps_the_camera_frustum() {
    for (const auto depth : {ge::math::ClipDepth::zero_to_one,
                             ge::math::ClipDepth::negative_one_to_one}) {
        for (const bool indexed : {false, true}) {
            Fixture fixture{depth};
            const auto front = fixture.box({0.0, 0.0, -10.0});
            (void)fixture.box({0.0, 0.0, 10.0});
            (void)fixture.sample();
            CHECK(fixture.visibility.collect(fixture.meshes, fixture.cameras, fixture.materials,
                fixture.renderer, indexed ? &fixture.spatial : nullptr));
            fixture.check_selected(front, indexed);
            CHECK(fixture.visibility.statistics().refreshed_cameras == 0);
        }
    }
}

void late_camera_refresh_preserves_effects_and_reaches_rasterization() {
    for (const auto depth : {ge::math::ClipDepth::zero_to_one,
                             ge::math::ClipDepth::negative_one_to_one}) {
        Fixture fixture{depth};
        const auto original = fixture.box({0.0, 0.0, -10.0});
        const auto moved = fixture.box({20.0, 0.0, -10.0});
        ge::render::CameraRecoil effect;
        effect.translation = {0.25, 0.0, 0.0};
        effect.rotation = {0.0, 0.02, 0.0};
        CHECK(fixture.cameras.recoil(fixture.camera, effect));
        const auto sampled = fixture.sample();
        CHECK(fixture.visibility.collect(fixture.meshes, fixture.cameras, fixture.materials,
            fixture.renderer, &fixture.spatial));
        fixture.check_selected(original, true);

        const ge::math::Vec3 translation{20.0, 0.0, 0.0};
        CHECK(fixture.scene.set_world_position(fixture.camera, far_origin + translation));
        ge::render::ImageService images;
        CHECK(images.framebuffer().resize(64, 48));
        CHECK(fixture.visibility.render(fixture.meshes, fixture.cameras, fixture.materials,
            fixture.renderer, images, &fixture.spatial));
        fixture.check_selected(moved, true);
        CHECK(fixture.visibility.statistics().refreshed_cameras == 1);
        const auto& refreshed = fixture.visibility.view(0)->camera;
        CHECK(ge::math::distance(refreshed.position, sampled.position + translation) < 1.0e-3);
        CHECK(ge::math::distance(refreshed.forward, sampled.forward) < 1.0e-12);
        CHECK(refreshed.revision == sampled.revision);
        CHECK(fixture.renderer.statistics().views == 1);
        CHECK(fixture.renderer.statistics().draws_rendered == 1);
        CHECK(fixture.renderer.statistics().fragments_written > 0);
    }
}

} // namespace

int main() {
    return ge::test::run("visibility_audit", {
        {"far_origin_collection", far_origin_collection_keeps_the_camera_frustum},
        {"late_camera_refresh", late_camera_refresh_preserves_effects_and_reaches_rasterization},
    });
}
