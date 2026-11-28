#include "test_support.hpp"

#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/render/camera.hpp>
#include <ge/render/lighting.hpp>
#include <ge/render/rasterizer.hpp>
#include <ge/scene/scene.hpp>

#include <optional>

namespace {

using namespace ge;

struct Fixture {
    ecs::Registry registry;
    scene::Scene scene{registry};
    geometry::MeshLibrary meshes;
    render::MaterialService materials;
    render::LightingService lighting{scene, materials};

    ecs::Entity box(geometry::MeshHandle mesh, math::Vec3 position) {
        math::Transform pose;
        pose.position = position;
        const auto entity = scene.create("caster", {}, pose);
        CHECK(entity);
        registry.emplace<geometry::MeshComponent>(entity, geometry::MeshComponent{mesh});
        return entity;
    }

    void light(render::LightType type, math::Vec3 position = {}) {
        render::LightComponent component;
        component.light.type = type;
        component.light.position = position;
        component.light.direction = {0.0, 0.0, -1.0};
        component.light.range = 20.0;
        component.shadow.resolution = 64;
        component.shadow.pcf_radius = 0;
        component.shadow.constant_bias = 0.0;
        component.shadow.slope_bias = 0.0;
        component.shadow.normal_offset = 0.0;
        component.shadow.fallback_extent = 8.0;
        component.shadow.directional_distance = 20.0;
        component.shadow.directional_padding = 0.0;
        CHECK(lighting.create("light", component));
    }

    double visibility(math::Vec3 position) const {
        render::MaterialSample surface;
        surface.world_position = position;
        surface.geometric_normal = {0.0, 0.0, 1.0};
        const auto sample = render::sample_light(materials.lights().front(), position);
        CHECK(sample.valid);
        return lighting.visibility(0, surface, sample);
    }
};

void far_origin_shadow_views_render_and_sample() {
    constexpr math::Vec3 origin{1.0e11, -2.0e11, 3.0e11};
    for (const auto type : {render::LightType::Directional,
                            render::LightType::Point, render::LightType::Spot}) {
        Fixture fixture;
        fixture.light(type, origin);
        const auto mesh = fixture.meshes.create(geometry::make_box({2.0, 2.0, 2.0}));
        (void)fixture.box(mesh, origin + math::Vec3{0.0, 0.0, -5.0});
        CHECK(fixture.lighting.update(fixture.meshes));
        const std::size_t expected_maps = type == render::LightType::Point ? 6 : 1;
        CHECK(fixture.lighting.statistics().ready_maps == expected_maps);
        CHECK(fixture.lighting.shadow_maps().size() == expected_maps);
        CHECK(fixture.visibility(origin + math::Vec3{0.0, 0.0, -7.0}) == 0.0);
        CHECK(fixture.visibility(origin + math::Vec3{3.0, 0.0, -7.0}) == 1.0);
        CHECK(fixture.lighting.update(fixture.meshes));
        CHECK(fixture.lighting.statistics().reused_maps == expected_maps);
        CHECK(fixture.lighting.statistics().rebuilt_maps == 0);
    }
}

void directional_shadows_fit_far_origin_camera_snapshots() {
    constexpr math::Vec3 origin{1.0e11, -2.0e11, 3.0e11};
    for (const auto depth : {math::ClipDepth::zero_to_one, math::ClipDepth::negative_one_to_one}) {
        Fixture fixture;
        render::CameraService cameras{fixture.scene};
        CHECK(cameras.set_framebuffer_size(64, 64));
        render::CameraComponent settings;
        settings.projection.clip_depth = depth;
        math::Transform pose;
        pose.position = origin;
        CHECK(cameras.create("camera", pose, settings));
        cameras.update(0.0);
        CHECK(cameras.current_view());
        CHECK(!cameras.current_view()->stale);
        fixture.light(render::LightType::Directional, origin);
        const auto mesh = fixture.meshes.create(geometry::make_box({2.0, 2.0, 2.0}));
        (void)fixture.box(mesh, origin + math::Vec3{0.0, 0.0, -5.0});
        CHECK(fixture.lighting.update(fixture.meshes, &cameras));
        CHECK(fixture.lighting.statistics().ready_maps == 1);
        CHECK(fixture.visibility(origin + math::Vec3{0.0, 0.0, -7.0}) == 0.0);
    }
}

void independent_mesh_libraries_invalidate_shadow_snapshots() {
    Fixture fixture;
    fixture.light(render::LightType::Spot);
    const auto broad = fixture.meshes.create(geometry::make_box({2.0, 2.0, 2.0}));
    geometry::MeshLibrary other;
    const auto narrow = other.create(geometry::make_box({0.2, 0.2, 0.2}));
    CHECK(broad == narrow);
    CHECK(fixture.meshes.inspect(broad)->revision == other.inspect(narrow)->revision);
    (void)fixture.box(broad, {0.0, 0.0, -5.0});
    CHECK(fixture.lighting.update(fixture.meshes));
    CHECK(fixture.visibility({0.5, 0.0, -7.0}) == 0.0);
    CHECK(fixture.lighting.update(other));
    CHECK(fixture.lighting.statistics().rebuilt_maps == 1);
    CHECK(fixture.lighting.statistics().reused_maps == 0);
    CHECK(fixture.visibility({0.5, 0.0, -7.0}) == 1.0);
    CHECK(fixture.lighting.update(other));
    CHECK(fixture.lighting.statistics().reused_maps == 1);
}

void reconstructed_mesh_library_invalidates_shadow_snapshots() {
    Fixture fixture;
    fixture.light(render::LightType::Spot);
    std::optional<geometry::MeshLibrary> meshes;
    meshes.emplace();
    const auto* address = &*meshes;
    const auto broad = meshes->create(geometry::make_box({2.0, 2.0, 2.0}));
    (void)fixture.box(broad, {0.0, 0.0, -5.0});
    CHECK(fixture.lighting.update(*meshes));
    CHECK(fixture.visibility({0.5, 0.0, -7.0}) == 0.0);
    meshes.reset();
    meshes.emplace();
    CHECK(address == &*meshes);
    const auto narrow = meshes->create(geometry::make_box({0.2, 0.2, 0.2}));
    CHECK(broad == narrow);
    CHECK(fixture.lighting.update(*meshes));
    CHECK(fixture.visibility({0.5, 0.0, -7.0}) == 1.0);
    CHECK(fixture.lighting.statistics().rebuilt_maps == 1);
    CHECK(fixture.lighting.statistics().reused_maps == 0);
}

double replacement_provider(const render::Light&, const render::MaterialSample&,
                            const render::LightSample&, const void*) noexcept {
    return 0.5;
}

void shadow_provider_release_preserves_replacements() {
    Fixture fixture;
    render::SoftwareRenderer first;
    render::SoftwareRenderer second;
    fixture.lighting.bind_renderer(first);
    first.set_shadow_provider(replacement_provider, &fixture);
    fixture.lighting.bind_renderer(second);
    CHECK(first.clear_shadow_provider(replacement_provider, &fixture));
    CHECK(!second.clear_shadow_provider(render::LightingService::shadow_factor, &fixture));
    CHECK(!second.clear_shadow_provider(replacement_provider, &fixture.lighting));
    fixture.lighting.shutdown();
    CHECK(second.clear_shadow_provider(nullptr));

    render::LightingService successor{fixture.scene, fixture.materials};
    fixture.lighting.bind_renderer(first);
    successor.bind_renderer(first);
    fixture.lighting.shutdown();
    CHECK(first.clear_shadow_provider(render::LightingService::shadow_factor, &successor));
    successor.shutdown();
}

} // namespace

int main() {
    return test::run("lighting audit", {
        {"far origin shadow maps", far_origin_shadow_views_render_and_sample},
        {"far origin camera coverage", directional_shadows_fit_far_origin_camera_snapshots},
        {"independent mesh library snapshots", independent_mesh_libraries_invalidate_shadow_snapshots},
        {"reconstructed mesh library snapshots", reconstructed_mesh_library_invalidates_shadow_snapshots},
        {"shadow provider ownership", shadow_provider_release_preserves_replacements},
    });
}
