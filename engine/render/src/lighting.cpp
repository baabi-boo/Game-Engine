#include <ge/render/lighting.hpp>

#include <ge/core/runtime.hpp>
#include <ge/render/camera.hpp>
#include <ge/render/rasterizer.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ge::render {
namespace {

constexpr std::size_t no_map = std::numeric_limits<std::size_t>::max();
constexpr std::uint64_t hash_basis = 14695981039346656037ULL;

bool within(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

void advance(std::uint64_t& value) noexcept {
    value = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
}

void hash_word(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 255U;
        hash *= 1099511628211ULL;
    }
}

void hash_scalar(std::uint64_t& hash, double value) noexcept {
    hash_word(hash, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}

void hash_vector(std::uint64_t& hash, math::Vec3 value) noexcept {
    hash_scalar(hash, value.x);
    hash_scalar(hash, value.y);
    hash_scalar(hash, value.z);
}

void hash_matrix(std::uint64_t& hash, const math::Mat4& value) noexcept {
    for (const auto scalar : value.data) {
        hash_scalar(hash, scalar);
    }
}

bool same_light(const Light& first, const Light& second) noexcept {
    return first.type == second.type && first.position == second.position &&
        first.direction == second.direction && first.color == second.color &&
        first.intensity == second.intensity && first.range == second.range &&
        first.radius == second.radius && first.inner_cone == second.inner_cone &&
        first.outer_cone == second.outer_cone && first.layers == second.layers &&
        first.enabled == second.enabled && first.casts_shadow == second.casts_shadow;
}

bool same_list(std::span<const Light> first, std::span<const Light> second) noexcept {
    return first.size() == second.size() &&
        std::equal(first.begin(), first.end(), second.begin(), same_light);
}

std::array<math::Vec3, 8> corners(const math::Aabb& bounds) noexcept {
    return {{{bounds.min.x, bounds.min.y, bounds.min.z},
             {bounds.max.x, bounds.min.y, bounds.min.z},
             {bounds.min.x, bounds.max.y, bounds.min.z},
             {bounds.max.x, bounds.max.y, bounds.min.z},
             {bounds.min.x, bounds.min.y, bounds.max.z},
             {bounds.max.x, bounds.min.y, bounds.max.z},
             {bounds.min.x, bounds.max.y, bounds.max.z},
             {bounds.max.x, bounds.max.y, bounds.max.z}}};
}

math::Vec3 stable_up(math::Vec3 direction) noexcept {
    return std::abs(direction.y) > 0.95 ? math::Vec3{0.0, 0.0, 1.0} :
                                        math::Vec3{0.0, 1.0, 0.0};
}

RasterizerLimits raster_limits(const LightingLimits& limits) {
    if (!valid(limits)) {
        throw std::invalid_argument("invalid lighting limits");
    }
    RasterizerLimits result;
    result.max_draws = limits.caster_draws;
    result.max_scene_entities = limits.scene_entities;
    result.max_hierarchy_depth = limits.hierarchy_depth;
    result.max_views = 1;
    result.max_input_triangles = limits.triangles_per_map;
    result.max_raster_triangles = limits.triangles_per_map * 8;
    result.max_fragment_visits = limits.fragments_per_map;
    result.max_light_visits = 0;
    result.max_lights_per_fragment = 0;
    result.max_framebuffer_pixels = limits.shadow_pixels;
    return result;
}

ShadingResult depth_shader(const RasterDraw& draw, SurfaceSample& surface,
                           ShadingContext&, const TextureLibrary* textures,
                           const void*) noexcept {
    ShadingResult result;
    result.color = Color::white();
    result.valid = true;
    result.discarded = false;
    if (draw.material && draw.material->alpha_mode == AlphaMode::Mask) {
        const auto sampled = sample_material(*draw.material, surface, textures);
        result.valid = sampled.valid;
        result.discarded = sampled.discarded;
        result.missing_textures = sampled.missing_textures;
    }
    return result;
}

bool finish_view(CameraView& view) noexcept {
    view.view_projection = view.projection * view.view;
    const auto inverse_view = math::inverse(view.view);
    const auto inverse_projection = math::inverse(view.projection);
    const auto inverse_combined = math::inverse(view.view_projection);
    const auto frustum = math::extract_frustum(view.view_projection, view.clip_depth);
    if (!inverse_view || !inverse_projection || !inverse_combined || !frustum) {
        return false;
    }
    view.inverse_view = *inverse_view;
    view.inverse_projection = *inverse_projection;
    view.inverse_view_projection = *inverse_combined;
    view.frustum = *frustum;
    return math::finite(view.view_projection);
}

struct Caster {
    RasterDraw draw;
    math::Aabb bounds;
    std::uint64_t signature{};
};

void hash_alpha(std::uint64_t& hash, const PbrMaterial& material,
                const TextureLibrary* textures) noexcept {
    hash_word(hash, static_cast<std::uint64_t>(material.alpha_mode));
    hash_word(hash, material.double_sided ? 1 : 0);
    if (material.alpha_mode != AlphaMode::Mask) {
        return;
    }
    hash_scalar(hash, material.albedo.a);
    hash_scalar(hash, material.alpha_cutoff);
    hash_word(hash, material.use_vertex_color ? 1 : 0);
    const auto& binding = material.albedo_texture;
    hash_word(hash, binding.texture.owner);
    hash_word(hash, binding.texture.index);
    hash_word(hash, binding.texture.generation);
    const auto* texture = textures ? textures->get(binding.texture) : nullptr;
    hash_word(hash, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texture)));
    hash_scalar(hash, binding.scale.x);
    hash_scalar(hash, binding.scale.y);
    hash_scalar(hash, binding.offset.x);
    hash_scalar(hash, binding.offset.y);
    hash_scalar(hash, binding.rotation);
    hash_word(hash, static_cast<std::uint64_t>(binding.sampler.address_u));
    hash_word(hash, static_cast<std::uint64_t>(binding.sampler.address_v));
    hash_word(hash, static_cast<std::uint64_t>(binding.sampler.filter));
    hash_scalar(hash, binding.sampler.border.a);
    hash_scalar(hash, binding.sampler.lod_bias);
    hash_scalar(hash, binding.sampler.min_lod);
    hash_scalar(hash, binding.sampler.max_lod);
}

bool receiver_corners(const CameraView* camera, const PublishedLight& light,
                       bool allow_stale, std::array<math::Vec3, 8>& output) noexcept {
    if (!camera) {
        const auto extent = math::Vec3{light.shadow.fallback_extent,
            light.shadow.fallback_extent, light.shadow.fallback_extent};
        output = corners({light.light.position - extent, light.light.position + extent});
        return true;
    }
    if ((!allow_stale && camera->stale) || camera->viewport.empty()) {
        return false;
    }
    const std::array<math::Vec2, 4> pixels{{
        {static_cast<double>(camera->viewport.x), static_cast<double>(camera->viewport.y)},
        {static_cast<double>(camera->viewport.x) + camera->viewport.width,
         static_cast<double>(camera->viewport.y)},
        {static_cast<double>(camera->viewport.x),
         static_cast<double>(camera->viewport.y) + camera->viewport.height},
        {static_cast<double>(camera->viewport.x) + camera->viewport.width,
         static_cast<double>(camera->viewport.y) + camera->viewport.height}}};
    const auto forward = math::normalized(camera->forward);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto near_point = camera_unproject(*camera, {pixels[index].x, pixels[index].y, 0.0});
        const auto far_point = camera_unproject(*camera, {pixels[index].x, pixels[index].y, 1.0});
        if (!near_point || !far_point) {
            return false;
        }
        const double near_distance = math::dot(*near_point - camera->position, forward);
        const double far_distance = math::dot(*far_point - camera->position, forward);
        if (!within(near_distance, 0.0, light.shadow.directional_distance) ||
            !std::isfinite(far_distance) || far_distance <= near_distance) {
            return false;
        }
        const double fraction = std::clamp((light.shadow.directional_distance - near_distance) /
            (far_distance - near_distance), 0.0, 1.0);
        output[index] = *near_point;
        output[index + 4] = math::lerp(*near_point, *far_point, fraction);
    }
    return true;
}

bool directional_view(const PublishedLight& light, const CameraView* camera,
                       std::span<const Caster> casters, bool allow_stale, CameraView& view) {
    std::array<math::Vec3, 8> receivers;
    if (!receiver_corners(camera, light, allow_stale, receivers)) {
        return false;
    }
    math::Vec3 centre{};
    for (const auto point : receivers) {
        centre += point / 8.0;
    }
    const auto forward = math::normalized(light.light.direction);
    const auto up = stable_up(forward);
    auto preliminary = math::look_at(centre, centre + forward, up);
    math::Aabb local;
    for (const auto point : receivers) {
        local = math::include(local, math::transform_point(preliminary, point));
    }
    if (!math::valid(local)) {
        return false;
    }
    const double padding = light.shadow.directional_padding;
    local.min.x -= padding;
    local.min.y -= padding;
    local.max.x += padding;
    local.max.y += padding;
    // Extend depth for off-camera occluders whose bounds overlap receiver XY.
    // Farther casters than the configured extrusion are intentionally excluded.
    const double maximum_z = local.max.z + light.shadow.directional_distance;
    for (const auto& caster : casters) {
        if ((caster.draw.visibility_mask & view.visibility_mask) == 0) {
            continue;
        }
        const auto box = math::transform_bounds(preliminary, caster.bounds);
        if (math::valid(box) && box.max.x >= local.min.x && box.min.x <= local.max.x &&
            box.max.y >= local.min.y && box.min.y <= local.max.y && box.min.z <= maximum_z) {
            local.max.z = std::max(local.max.z, std::min(box.max.z, maximum_z));
        }
    }
    const double distance = local.max.z + padding + light.shadow.near_plane;
    view.position = centre - forward * distance;
    view.forward = forward;
    view.up = up;
    view.view = math::look_at(view.position, view.position + forward, up);
    view.near_plane = light.shadow.near_plane;
    view.far_plane = std::max(view.near_plane + 0.1, distance - local.min.z + padding);
    double width = std::max(local.max.x - local.min.x, 0.01);
    double height = std::max(local.max.y - local.min.y, 0.01);
    const double texel_x = width / static_cast<double>(light.shadow.resolution);
    const double texel_y = height / static_cast<double>(light.shadow.resolution);
    const double x = std::floor((local.min.x + local.max.x) * 0.5 / texel_x) * texel_x;
    const double y = std::floor((local.min.y + local.max.y) * 0.5 / texel_y) * texel_y;
    width += 2.0 * texel_x;
    height += 2.0 * texel_y;
    view.projection_mode = ProjectionMode::Orthographic;
    view.projection = math::orthographic_zero_to_one(x - width * 0.5, x + width * 0.5,
        y - height * 0.5, y + height * 0.5, view.near_plane, view.far_plane);
    return finish_view(view);
}

constexpr std::array<math::Vec3, 6> cube_directions{{
    {1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
    {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}}};

bool make_view(const PublishedLight& light, std::size_t face, const CameraView* camera,
               std::span<const Caster> casters, bool allow_stale, CameraView& view) {
    view = {};
    view.entity = light.entity;
    view.slot = light.slot;
    view.viewport = {0, 0, static_cast<std::int32_t>(light.shadow.resolution),
        static_cast<std::int32_t>(light.shadow.resolution)};
    view.clip_depth = math::ClipDepth::zero_to_one;
    view.visibility_mask = light.shadow.caster_mask & light.light.layers;
    if (light.light.type == LightType::Directional) {
        return directional_view(light, camera, casters, allow_stale, view);
    }
    const bool point = light.light.type == LightType::Point;
    if ((!point && light.light.outer_cone >= math::pi * 0.4999) ||
        light.shadow.near_plane >= light.light.range) {
        return false;
    }
    view.position = light.light.position;
    view.forward = point ? cube_directions[face] : math::normalized(light.light.direction);
    view.up = stable_up(view.forward);
    view.view = math::look_at(view.position, view.position + view.forward, view.up);
    view.near_plane = light.shadow.near_plane;
    view.far_plane = light.light.range;
    view.projection_mode = ProjectionMode::Perspective;
    view.projection = math::perspective_zero_to_one(point ? math::pi * 0.5 :
        light.light.outer_cone * 2.0, 1.0, view.near_plane, view.far_plane);
    return finish_view(view);
}

std::size_t cube_face(math::Vec3 direction) noexcept {
    const auto absolute = math::abs(direction);
    if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
        return direction.x >= 0.0 ? 0 : 1;
    }
    if (absolute.y >= absolute.z) {
        return direction.y >= 0.0 ? 2 : 3;
    }
    return direction.z >= 0.0 ? 4 : 5;
}

} // namespace

std::string_view describe(LightingError error) noexcept {
    switch (error) {
    case LightingError::None: return "ready";
    case LightingError::InvalidConfiguration: return "invalid lighting configuration";
    case LightingError::InvalidLight: return "invalid light";
    case LightingError::InvalidEntity: return "invalid scene entity";
    case LightingError::SceneRejected: return "scene synchronization rejected";
    case LightingError::Capacity: return "lighting capacity reached";
    case LightingError::OwnershipLost: return "external light edit preserved; publication switched to manual";
    case LightingError::UnsupportedProjection: return "shadow projection unsupported";
    case LightingError::UnsupportedMaterial: return "blended shadow transmission unsupported";
    case LightingError::RasterRejected: return "shadow rasterization rejected";
    case LightingError::WorkLimit: return "shadow work deferred or limited";
    case LightingError::AllocationFailure: return "lighting allocation failed";
    }
    return "unknown lighting error";
}

bool valid(const ShadowSettings& settings) noexcept {
    return settings.resolution >= 16 && settings.resolution <= 2048 &&
        settings.pcf_radius <= 3 && within(settings.constant_bias, 0.0, 0.1) &&
        within(settings.slope_bias, 0.0, 0.1) && within(settings.normal_offset, 0.0, 10.0) &&
        within(settings.near_plane, 0.0001, 10000.0) &&
        within(settings.directional_distance, 0.01, 100000.0) &&
        within(settings.directional_padding, 0.0, 10000.0) &&
        within(settings.fallback_extent, 0.01, 100000.0) && within(settings.strength, 0.0, 1.0);
}

bool valid(const LightingLimits& limits) noexcept {
    return limits.scene_entities > 0 && limits.scene_entities <= 1048576 &&
        limits.hierarchy_depth > 0 && limits.hierarchy_depth <= 4096 &&
        limits.lights > 0 && limits.lights <= material_light_limit &&
        limits.shadow_lights <= 64 && limits.shadow_maps <= 96 &&
        limits.shadow_pixels > 0 && limits.shadow_pixels <= 16777216 &&
        limits.caster_draws > 0 && limits.caster_draws <= 65536 &&
        limits.triangles_per_map > 0 && limits.triangles_per_map <= 1048576 &&
        limits.fragments_per_map > 0 && limits.fragments_per_map <= 268435456 &&
        limits.maps_per_update > 0 && limits.maps_per_update <= 96 &&
        limits.fragments_per_map <= std::numeric_limits<std::size_t>::max() / limits.maps_per_update &&
        limits.maximum_resolution >= 16 && limits.maximum_resolution <= 2048;
}

bool valid(const LightingSettings& settings) noexcept {
    return settings.publication <= LightPublication::SceneOnly && valid(settings.manual_shadows);
}

struct LightingService::Impl {
    struct Map {
        ShadowMapInfo info;
        CameraView view;
        std::unique_ptr<Framebuffer> target;
        std::vector<std::size_t> casters;
    };

    scene::Scene& scene;
    MaterialService& materials;
    LightingLimits limits;
    LightingSettings settings;
    LightingStatistics stats;
    SoftwareRenderer depth_renderer;
    SoftwareRenderer* renderer{};
    std::vector<Light> manual;
    std::vector<Light> last_publication;
    std::vector<PublishedLight> published;
    std::vector<std::unique_ptr<Map>> maps;
    std::vector<ShadowMapInfo> map_info;
    std::vector<std::array<std::size_t, 6>> lookup;
    std::uint64_t expected_revision{};
    std::uint64_t invalidation{1};
    std::size_t build_cursor{};
    bool owned{};

    Impl(scene::Scene& source, MaterialService& destination, LightingLimits configuration)
        : scene(source), materials(destination), limits(configuration),
          depth_renderer(raster_limits(configuration)) {
        settings.manual_shadows.resolution = std::min(settings.manual_shadows.resolution, limits.maximum_resolution);
        RasterizerSettings depth_settings;
        depth_settings.cull = CullMode::None;
        depth_settings.sort_transparency = false;
        depth_settings.background = Color::white();
        if (!depth_renderer.configure(depth_settings)) {
            throw std::invalid_argument("shadow rasterizer configuration rejected");
        }
        depth_renderer.set_fragment_shader(depth_shader);
    }

    void fail(LightingError error) noexcept {
        stats.partial = true;
        if (stats.error == LightingError::None) {
            stats.error = error;
        }
    }

    bool publication_current() const noexcept {
        return same_list(materials.lights(), last_publication);
    }

    bool release() {
        if (!owned) {
            return true;
        }
        if (!publication_current()) {
            owned = false;
            settings.publication = LightPublication::Manual;
            fail(LightingError::OwnershipLost);
            return false;
        }
        if (!materials.set_lights(manual)) {
            fail(LightingError::Capacity);
            return false;
        }
        owned = false;
        expected_revision = materials.lighting_revision();
        return true;
    }

    bool node_enabled(ecs::Entity entity) noexcept {
        for (std::size_t depth = 0; entity && depth < limits.hierarchy_depth; ++depth) {
            if (!scene.registry().enabled(entity)) {
                return false;
            }
            entity = scene.parent(entity);
        }
        if (entity) {
            fail(LightingError::WorkLimit);
        }
        return !entity;
    }

    void clear_maps() noexcept {
        maps.clear();
        map_info.clear();
        lookup.clear();
        build_cursor = 0;
    }

    bool collect_lights() {
        if (owned && !publication_current()) {
            owned = false;
            settings.publication = LightPublication::Manual;
            clear_maps();
            fail(LightingError::OwnershipLost);
        }
        const auto capacity = std::min(limits.lights, materials.materials().limits().lights);
        std::vector<PublishedLight> next;
        std::vector<Light> incoming;
        next.reserve(capacity);
        incoming.reserve(capacity);
        if (settings.publication != LightPublication::Manual && !owned) {
            manual.assign(materials.lights().begin(), materials.lights().end());
        }
        const auto manual_source = settings.publication == LightPublication::Manual ?
            materials.lights() : std::span<const Light>{manual};
        if (settings.publication != LightPublication::SceneOnly) {
            for (const auto& light : manual_source) {
                if (next.size() >= capacity) {
                    ++stats.truncated_lights;
                    continue;
                }
                next.push_back({next.size(), {}, light, settings.manual_shadows, true});
                incoming.push_back(light);
                ++stats.manual_lights;
            }
        }
        // Preserving an oversized manual prefix is more important than admission
        // of scene lights. Never publish a silently truncated manual list.
        if (settings.publication == LightPublication::AppendScene && manual.size() > capacity) {
            fail(LightingError::Capacity);
            return false;
        }
        if (settings.publication != LightPublication::Manual) {
            const auto entities = scene.registry().view<LightComponent>(true);
            for (const auto entity : entities) {
                const auto& component = scene.registry().require<LightComponent>(entity);
                if (!scene.contains(entity) || !node_enabled(entity) || !component.light.enabled) {
                    continue;
                }
                const auto* matrix = scene.cached_world_matrix(entity);
                Light light = component.light;
                if (!matrix || !validate_light(light) || !valid(component.shadow)) {
                    ++stats.rejected_lights;
                    continue;
                }
                light.position = math::transform_point(*matrix, light.position);
                light.direction = math::normalized(math::transform_vector(*matrix, light.direction));
                if (component.inherit_entity_layers) {
                    light.layers &= scene.registry().layers(entity);
                }
                if (!validate_light(light)) {
                    ++stats.rejected_lights;
                    continue;
                }
                if (next.size() >= capacity) {
                    ++stats.truncated_lights;
                    continue;
                }
                next.push_back({next.size(), entity, light, component.shadow, false});
                incoming.push_back(light);
                ++stats.scene_lights;
            }
            if (!same_list(materials.lights(), incoming) && !materials.set_lights(incoming)) {
                fail(LightingError::Capacity);
                return false;
            }
            owned = true;
            last_publication.swap(incoming);
        } else {
            std::vector<Light> current(materials.lights().begin(), materials.lights().end());
            last_publication.swap(current);
        }
        published.swap(next);
        expected_revision = materials.lighting_revision();
        stats.publication_revision = expected_revision;
        stats.publication_owned = owned;
        stats.published_lights = materials.lights().size();
        if (stats.rejected_lights != 0) {
            fail(LightingError::InvalidLight);
        }
        if (stats.truncated_lights != 0) {
            fail(LightingError::Capacity);
        }
        return true;
    }

    bool collect_casters(const geometry::MeshLibrary& meshes, const TextureLibrary* textures,
                         std::vector<Caster>& output) {
        const auto entities = scene.registry().view<geometry::MeshComponent>(true);
        output.reserve(std::min(entities.size(), limits.caster_draws));
        for (const auto entity : entities) {
            const auto& component = scene.registry().require<geometry::MeshComponent>(entity);
            const auto* caster = scene.registry().get<ShadowCasterComponent>(entity);
            if (!scene.contains(entity) || !component.visible || !node_enabled(entity) ||
                (caster && !caster->enabled) || scene.registry().layers(entity) == 0) {
                continue;
            }
            const auto* mesh = meshes.get(component.mesh);
            const auto info = meshes.inspect(component.mesh);
            const auto* matrix = scene.cached_world_matrix(entity);
            if (!mesh || !info || !matrix || mesh->empty()) {
                ++stats.invalid_casters;
                continue;
            }
            const auto bounds = math::transform_bounds(*matrix, info->bounds);
            if (!math::valid(bounds) || !math::normal_matrix(*matrix)) {
                ++stats.invalid_casters;
                continue;
            }
            const auto ranges = std::max(std::size_t{1}, mesh->submeshes.size());
            const auto first = static_cast<std::size_t>(component.first_submesh);
            if (first >= ranges) {
                ++stats.invalid_casters;
                continue;
            }
            const auto count = component.submesh_count == 0 ? ranges - first :
                std::min(ranges - first, static_cast<std::size_t>(component.submesh_count));
            for (std::size_t index = first; index < first + count; ++index) {
                RasterDraw draw;
                draw.mesh = mesh;
                draw.model = *matrix;
                draw.entity = entity;
                draw.visibility_mask = scene.registry().layers(entity);
                draw.cull = !caster || caster->two_sided ? CullMode::None : CullMode::Back;
                std::uint32_t slot{};
                if (!mesh->submeshes.empty()) {
                    draw.first_index = mesh->submeshes[index].first_index;
                    draw.index_count = mesh->submeshes[index].index_count;
                    slot = mesh->submeshes[index].material_slot;
                } else {
                    draw.index_count = mesh->indices.size();
                }
                draw.material = &materials.resolve(scene.registry(), entity, slot);
                if (draw.material->alpha_mode == AlphaMode::Blend) {
                    ++stats.blended_casters_skipped;
                    continue;
                }
                if (output.size() >= limits.caster_draws) {
                    fail(LightingError::Capacity);
                    return false;
                }
                std::uint64_t signature = hash_basis;
                hash_word(signature, entity.index);
                hash_word(signature, entity.generation);
                hash_word(signature, component.mesh.index);
                hash_word(signature, component.mesh.generation);
                hash_word(signature, info->revision);
                hash_matrix(signature, draw.model);
                hash_word(signature, draw.first_index);
                hash_word(signature, draw.index_count);
                hash_word(signature, static_cast<std::uint64_t>(*draw.cull));
                hash_alpha(signature, *draw.material, textures);
                output.push_back({draw, bounds, signature});
            }
        }
        stats.caster_draws = output.size();
        if (stats.invalid_casters != 0) {
            fail(LightingError::InvalidEntity);
        }
        if (stats.blended_casters_skipped != 0) {
            fail(LightingError::UnsupportedMaterial);
        }
        return true;
    }

    void plan_maps(std::span<const Caster> casters, const CameraView* camera) {
        map_info.clear();
        std::vector<std::unique_ptr<Map>> next;
        next.reserve(limits.shadow_maps);
        lookup.assign(published.size(), {no_map, no_map, no_map, no_map, no_map, no_map});
        std::size_t pixels{};
        std::size_t admitted_lights{};
        for (const auto& light : published) {
            if (!light.light.enabled || !light.light.casts_shadow || !light.shadow.enabled ||
                light.shadow.strength == 0.0 || light.light.intensity == 0.0) {
                continue;
            }
            const std::size_t faces = light.light.type == LightType::Point ? 6 : 1;
            stats.requested_maps += faces;
            if ((faces == 6 && !settings.point_shadows) ||
                light.shadow.resolution > limits.maximum_resolution) {
                ++stats.unsupported_lights;
                fail(LightingError::UnsupportedProjection);
                continue;
            }
            const auto per_map = static_cast<std::size_t>(light.shadow.resolution) *
                static_cast<std::size_t>(light.shadow.resolution);
            if (admitted_lights >= limits.shadow_lights || faces > limits.shadow_maps - next.size() ||
                per_map > (limits.shadow_pixels - pixels) / faces) {
                stats.rejected_maps += faces;
                fail(LightingError::Capacity);
                continue;
            }
            std::array<CameraView, 6> views;
            bool supported = true;
            for (std::size_t face = 0; face < faces; ++face) {
                if (!make_view(light, face, camera, casters, settings.allow_stale_camera, views[face])) {
                    supported = false;
                    break;
                }
            }
            if (!supported) {
                ++stats.unsupported_lights;
                stats.rejected_maps += faces;
                fail(LightingError::UnsupportedProjection);
                continue;
            }
            ++admitted_lights;
            pixels += faces * per_map;
            for (std::size_t face = 0; face < faces; ++face) {
                std::unique_ptr<Map> map;
                for (auto& previous : maps) {
                    if (previous && previous->info.light_slot == light.slot && previous->info.face == face) {
                        map = std::move(previous);
                        break;
                    }
                }
                if (!map) {
                    map = std::make_unique<Map>();
                }
                const auto& view = views[face];
                auto signature = hash_basis;
                hash_word(signature, invalidation);
                hash_matrix(signature, view.view_projection);
                hash_vector(signature, light.light.position);
                hash_vector(signature, light.light.direction);
                hash_word(signature, view.visibility_mask);
                hash_word(signature, light.shadow.resolution);
                map->casters.clear();
                for (std::size_t index = 0; index < casters.size(); ++index) {
                    const auto& caster = casters[index];
                    if ((caster.draw.visibility_mask & view.visibility_mask) != 0 &&
                        math::intersects(view.frustum, caster.bounds)) {
                        map->casters.push_back(index);
                        hash_word(signature, caster.signature);
                    }
                }
                hash_word(signature, map->casters.size());
                if (map->info.source_signature != signature || !map->target ||
                    map->target->width() != light.shadow.resolution) {
                    map->info.ready = false;
                }
                if (map->target && map->target->width() != light.shadow.resolution) {
                    map->target.reset();
                }
                map->view = view;
                map->info.light_slot = light.slot;
                map->info.face = face;
                map->info.resolution = light.shadow.resolution;
                map->info.source_signature = signature;
                map->info.caster_draws = map->casters.size();
                lookup[light.slot][face] = next.size();
                next.push_back(std::move(map));
            }
        }
        // Delete obsolete attachments before allocating any replacement targets.
        maps.clear();
        maps.swap(next);
    }

    void render_maps(std::span<const Caster> casters, const TextureLibrary* textures) {
        if (maps.empty()) {
            return;
        }
        const auto start = build_cursor % maps.size();
        std::size_t attempts{};
        for (std::size_t offset = 0; offset < maps.size(); ++offset) {
            const auto index = (start + offset) % maps.size();
            auto& map = *maps[index];
            if (map.info.ready) {
                ++stats.reused_maps;
                continue;
            }
            if (attempts >= limits.maps_per_update) {
                ++stats.deferred_maps;
                map.info.error = LightingError::WorkLimit;
                fail(LightingError::WorkLimit);
                continue;
            }
            ++attempts;
            build_cursor = (index + 1) % maps.size();
            if (!map.target) {
                map.target = std::make_unique<Framebuffer>(map.info.resolution, map.info.resolution,
                    true, ImageLimits{limits.maximum_resolution, limits.shadow_pixels});
            }
            std::vector<RasterDraw> draws;
            draws.reserve(map.casters.size());
            for (const auto caster : map.casters) {
                draws.push_back(casters[caster].draw);
            }
            ShadingContext context;
            context.ambient = Color::black();
            context.max_lights = 0;
            const auto outcome = depth_renderer.draw(*map.target, map.view, draws, context, textures, true);
            const auto& rendered = depth_renderer.statistics();
            stats.fragment_visits += rendered.fragment_visits;
            stats.input_triangles += rendered.input_triangles;
            map.info.ready = static_cast<bool>(outcome) && rendered.triangles_invalid == 0 &&
                rendered.fragments_invalid == 0;
            if (map.info.ready) {
                map.info.error = LightingError::None;
                map.info.built_update = stats.updates;
                ++stats.rebuilt_maps;
            } else {
                map.info.error = outcome.error == RasterError::WorkLimit ? LightingError::WorkLimit :
                    LightingError::RasterRejected;
                ++stats.rejected_maps;
                fail(map.info.error);
            }
        }
        map_info.clear();
        map_info.reserve(maps.size());
        for (const auto& map : maps) {
            map_info.push_back(map->info);
            stats.ready_maps += map->info.ready ? 1U : 0U;
            if (map->target) {
                stats.resident_pixels += static_cast<std::size_t>(map->target->width()) * map->target->height();
            }
        }
    }

    double sample(std::size_t slot, const MaterialSample& surface, const LightSample& illumination) const noexcept {
        if (!settings.enabled || !settings.shadows || slot >= published.size() || slot >= lookup.size() ||
            materials.lighting_revision() != expected_revision || !math::finite(surface.world_position) ||
            !math::finite(surface.geometric_normal) || !math::finite(illumination.direction)) {
            return 1.0;
        }
        const auto& light = published[slot];
        const auto normal = math::normalized(surface.geometric_normal);
        const auto position = surface.world_position + normal * light.shadow.normal_offset;
        const auto face = light.light.type == LightType::Point ? cube_face(position - light.light.position) : 0;
        const auto index = lookup[slot][face];
        if (index == no_map || index >= maps.size() || !maps[index]->info.ready || !maps[index]->target) {
            return 1.0;
        }
        const auto& map = *maps[index];
        const auto projected = map.view.view_projection * math::homogeneous(position);
        if (!math::finite(projected) || projected.w <= math::epsilon) {
            return 1.0;
        }
        const double u = projected.x / projected.w * 0.5 + 0.5;
        const double v = 0.5 - projected.y / projected.w * 0.5;
        const double ndc_depth = projected.z / projected.w;
        const double depth = map.view.clip_depth == math::ClipDepth::negative_one_to_one ?
            ndc_depth * 0.5 + 0.5 : ndc_depth;
        if (!within(u, 0.0, 1.0) || !within(v, 0.0, 1.0) || !within(depth, 0.0, 1.0)) {
            return 1.0;
        }
        const double cosine = std::clamp(math::dot(normal, math::normalized(illumination.direction)), 0.0, 1.0);
        const double bias = light.shadow.constant_bias + light.shadow.slope_bias * (1.0 - cosine);
        const auto column = static_cast<std::int32_t>(std::floor(u * map.info.resolution));
        const auto row = static_cast<std::int32_t>(std::floor(v * map.info.resolution));
        const auto radius = static_cast<std::int32_t>(light.shadow.pcf_radius);
        double visible{};
        double samples{};
        for (std::int32_t y = -radius; y <= radius; ++y) {
            for (std::int32_t x = -radius; x <= radius; ++x) {
                const auto stored = map.target->depth_at(column + x, row + y);
                visible += !stored || !std::isfinite(*stored) || depth - bias <= *stored ? 1.0 : 0.0;
                samples += 1.0;
            }
        }
        return 1.0 - light.shadow.strength * (1.0 - visible / samples);
    }
};

LightingService::LightingService(scene::Scene& scene, MaterialService& materials, LightingLimits limits)
    : impl_(std::make_unique<Impl>(scene, materials, limits)) {}

LightingService::~LightingService() = default;

bool LightingService::configure(const LightingSettings& settings) {
    if (!valid(settings) || settings.manual_shadows.resolution > impl_->limits.maximum_resolution) {
        impl_->stats.error = LightingError::InvalidConfiguration;
        return false;
    }
    if ((settings.publication != impl_->settings.publication || !settings.enabled) && !impl_->release()) {
        return false;
    }
    impl_->settings = settings;
    invalidate();
    return true;
}

const LightingSettings& LightingService::settings() const noexcept { return impl_->settings; }
const LightingLimits& LightingService::limits() const noexcept { return impl_->limits; }
const LightingStatistics& LightingService::statistics() const noexcept { return impl_->stats; }
LightingError LightingService::last_error() const noexcept { return impl_->stats.error; }

ge::RuntimeHandle LightingService::create(std::string_view name, const LightComponent& component) {
    if (!validate_light(component.light) || !valid(component.shadow) ||
        component.shadow.resolution > impl_->limits.maximum_resolution) {
        impl_->stats.error = LightingError::InvalidLight;
        return {};
    }
    if (impl_->scene.registry().size() >= impl_->limits.scene_entities ||
        impl_->scene.registry().view<LightComponent>(true).size() >= impl_->limits.lights) {
        impl_->stats.error = LightingError::Capacity;
        return {};
    }
    const auto entity = impl_->scene.create(name);
    if (!entity) {
        impl_->stats.error = LightingError::InvalidEntity;
        return {};
    }
    try {
        if (attach(entity, component)) {
            return entity;
        }
    } catch (...) {
        (void)impl_->scene.destroy(entity);
        throw;
    }
    (void)impl_->scene.destroy(entity);
    return {};
}

bool LightingService::attach(ge::RuntimeHandle entity, const LightComponent& component) {
    if (!impl_->scene.contains(entity)) {
        impl_->stats.error = LightingError::InvalidEntity;
        return false;
    }
    if (!validate_light(component.light) || !valid(component.shadow) ||
        component.shadow.resolution > impl_->limits.maximum_resolution) {
        impl_->stats.error = LightingError::InvalidLight;
        return false;
    }
    auto& registry = impl_->scene.registry();
    if (!registry.has<LightComponent>(entity) && registry.view<LightComponent>(true).size() >= impl_->limits.lights) {
        impl_->stats.error = LightingError::Capacity;
        return false;
    }
    registry.emplace_or_replace<LightComponent>(entity, component);
    invalidate();
    return true;
}

bool LightingService::detach(ge::RuntimeHandle entity) {
    const bool removed = impl_->scene.registry().remove<LightComponent>(entity);
    if (removed) {
        invalidate();
    }
    return removed;
}

bool LightingService::set_light(ge::RuntimeHandle entity, const Light& light) {
    const auto* previous = component(entity);
    if (!previous) {
        return false;
    }
    auto replacement = *previous;
    replacement.light = light;
    return attach(entity, replacement);
}

bool LightingService::set_shadow(ge::RuntimeHandle entity, const ShadowSettings& settings) {
    const auto* previous = component(entity);
    if (!previous) {
        return false;
    }
    auto replacement = *previous;
    replacement.shadow = settings;
    return attach(entity, replacement);
}

const LightComponent* LightingService::component(ge::RuntimeHandle entity) const noexcept {
    return impl_->scene.registry().get<LightComponent>(entity);
}

void LightingService::bind_renderer(SoftwareRenderer& renderer) noexcept {
    if (impl_->renderer && impl_->renderer != &renderer) {
        impl_->renderer->set_shadow_provider(nullptr);
    }
    impl_->renderer = &renderer;
    renderer.set_shadow_provider(shadow_factor, this);
}

void LightingService::shutdown() noexcept {
    if (impl_->renderer) {
        impl_->renderer->set_shadow_provider(nullptr);
        impl_->renderer = nullptr;
    }
    try {
        (void)impl_->release();
    } catch (...) {
        impl_->stats.error = LightingError::AllocationFailure;
    }
    impl_->settings.enabled = false;
    impl_->clear_maps();
}

void LightingService::invalidate() noexcept {
    advance(impl_->invalidation);
    for (auto& map : impl_->maps) {
        map->info.ready = false;
    }
    for (auto& info : impl_->map_info) {
        info.ready = false;
    }
}

bool LightingService::update(const geometry::MeshLibrary& meshes, const CameraService* cameras,
                              const TextureLibrary* textures) {
    auto updates = impl_->stats.updates;
    advance(updates);
    impl_->stats = {};
    impl_->stats.updates = updates;
    try {
        if (!impl_->settings.enabled) {
            const bool released = impl_->release();
            impl_->clear_maps();
            impl_->published.clear();
            impl_->stats.manual_lights = impl_->materials.lights().size();
            impl_->stats.published_lights = impl_->materials.lights().size();
            impl_->stats.publication_revision = impl_->materials.lighting_revision();
            return released;
        }
        if (impl_->scene.size() > impl_->limits.scene_entities ||
            impl_->scene.registry().size() > impl_->limits.scene_entities) {
            impl_->fail(LightingError::Capacity);
            impl_->clear_maps();
            return false;
        }
        if (impl_->scene.update().error != scene::SceneError::None) {
            impl_->fail(LightingError::SceneRejected);
            impl_->clear_maps();
            return false;
        }
        if (!impl_->collect_lights()) {
            impl_->clear_maps();
            return false;
        }
        if (!impl_->settings.shadows || impl_->published.empty()) {
            impl_->clear_maps();
            return !impl_->stats.partial;
        }
        std::vector<Caster> casters;
        if (!impl_->collect_casters(meshes, textures, casters)) {
            impl_->clear_maps();
            return false;
        }
        impl_->plan_maps(casters, cameras ? cameras->current_view() : nullptr);
        impl_->render_maps(casters, textures);
        return !impl_->stats.partial;
    } catch (const std::bad_alloc&) {
        impl_->fail(LightingError::AllocationFailure);
    } catch (const std::length_error&) {
        impl_->fail(LightingError::AllocationFailure);
    } catch (const std::invalid_argument&) {
        impl_->fail(LightingError::UnsupportedProjection);
    }
    impl_->clear_maps();
    return false;
}

std::span<const PublishedLight> LightingService::published() const noexcept { return impl_->published; }
std::span<const ShadowMapInfo> LightingService::shadow_maps() const noexcept { return impl_->map_info; }

const Framebuffer* LightingService::shadow_framebuffer(std::size_t map) const noexcept {
    return map < impl_->maps.size() ? impl_->maps[map]->target.get() : nullptr;
}

const CameraView* LightingService::shadow_view(std::size_t map) const noexcept {
    return map < impl_->maps.size() ? &impl_->maps[map]->view : nullptr;
}

double LightingService::visibility(std::size_t slot, const MaterialSample& surface,
                                   const LightSample& illumination) const noexcept {
    return impl_->sample(slot, surface, illumination);
}

double LightingService::shadow_factor(const Light& light, const MaterialSample& surface,
                                       const LightSample& illumination, const void* user) noexcept {
    if (!user) {
        return 1.0;
    }
    const auto& service = *static_cast<const LightingService*>(user);
    const auto& state = *service.impl_;
    if (state.materials.lighting_revision() != state.expected_revision) {
        return 1.0;
    }
    const auto lights = state.materials.lights();
    const auto base = reinterpret_cast<std::uintptr_t>(lights.data());
    const auto address = reinterpret_cast<std::uintptr_t>(&light);
    if (address < base || (address - base) % sizeof(Light) != 0) {
        return 1.0;
    }
    const auto slot = static_cast<std::size_t>((address - base) / sizeof(Light));
    if (slot >= lights.size() || slot >= state.published.size() || &lights[slot] != &light ||
        !same_light(light, state.published[slot].light)) {
        return 1.0;
    }
    return state.sample(slot, surface, illumination);
}

namespace {

template<class Number>
std::optional<Number> parse_number(std::string_view text) noexcept {
    Number value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string status_text(const LightingService& service) {
    const auto& stats = service.statistics();
    std::ostringstream text;
    text << "mode=" << static_cast<unsigned>(service.settings().publication)
         << " updates=" << stats.updates << " scene=" << stats.scene_lights
         << " manual=" << stats.manual_lights << " published=" << stats.published_lights
         << " owned=" << stats.publication_owned << " maps=" << stats.ready_maps << '/' << stats.requested_maps
         << " rebuilt=" << stats.rebuilt_maps << " reused=" << stats.reused_maps
         << " deferred=" << stats.deferred_maps << " rejected=" << stats.rejected_maps
         << " pixels=" << stats.resident_pixels << " triangles=" << stats.input_triangles
         << " fragments=" << stats.fragment_visits << " blended_skipped=" << stats.blended_casters_skipped
         << " invalid_casters=" << stats.invalid_casters << " partial=" << stats.partial
         << " status=" << describe(stats.error);
    return text.str();
}

bool prepare_services(core::Kernel& host) {
    auto* service = host.services().find<LightingService>();
    const auto* meshes = host.services().find<geometry::MeshLibrary>();
    auto* renderer = host.services().find<SoftwareRenderer>();
    const auto* images = host.services().find<ImageService>();
    if (!service || !meshes || !renderer) {
        if (service) {
            service->invalidate();
        }
        return false;
    }
    service->bind_renderer(*renderer);
    return service->update(*meshes, host.services().find<CameraService>(), images ? &images->textures() : nullptr);
}

core::CommandResult edit_light(core::Kernel& host, core::CommandArgs args) {
    if (args.size() < 3) {
        return {false, "usage: lighting.set entity intensity|range|radius|enabled|shadow value; position|direction|color x y z"};
    }
    const auto entity = ecs::parse_entity(args[0]);
    auto& service = host.services().get<LightingService>();
    const auto* component = entity ? service.component(*entity) : nullptr;
    if (!component) {
        return {false, "light component not found"};
    }
    auto light = component->light;
    bool parsed = false;
    if (args.size() == 3) {
        const auto value = parse_number<double>(args[2]);
        if (value && args[1] == "intensity") { light.intensity = *value; parsed = true; }
        if (value && args[1] == "range") { light.range = *value; parsed = true; }
        if (value && args[1] == "radius") { light.radius = *value; parsed = true; }
        if (args[2] == "on" || args[2] == "off") {
            if (args[1] == "enabled") { light.enabled = args[2] == "on"; parsed = true; }
            if (args[1] == "shadow") { light.casts_shadow = args[2] == "on"; parsed = true; }
        }
    } else if (args.size() == 5) {
        const auto x = parse_number<double>(args[2]);
        const auto y = parse_number<double>(args[3]);
        const auto z = parse_number<double>(args[4]);
        if (x && y && z) {
            if (args[1] == "position") { light.position = {*x, *y, *z}; parsed = true; }
            if (args[1] == "direction") { light.direction = {*x, *y, *z}; parsed = true; }
            if (args[1] == "color") { light.color = {*x, *y, *z, 1.0}; parsed = true; }
        }
    }
    const bool changed = parsed && service.set_light(*entity, light);
    return {changed, changed ? "light updated; publication refreshes before rendering" : "light edit rejected"};
}

} // namespace

void register_lighting(core::Kernel& kernel) {
    if (!kernel.register_module({"render.lighting", "Scene lights and bounded cached CPU depth shadows", 1})) {
        return;
    }
    auto* scene_service = kernel.services().find<scene::Scene>();
    auto* materials = kernel.services().find<MaterialService>();
    if (!scene_service || !materials) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "render.lighting", "Scene and MaterialService are required");
        return;
    }
    scene_service->registry().register_component<LightComponent>("render.light");
    scene_service->registry().register_component<ShadowCasterComponent>("render.shadow_caster");
    auto& service = kernel.services().emplace<LightingService>(*scene_service, *materials);
    auto settings = service.settings();
    settings.enabled = kernel.config().boolean("lighting.enabled", settings.enabled);
    settings.shadows = kernel.config().boolean("lighting.shadows", settings.shadows);
    settings.point_shadows = kernel.config().boolean("lighting.point_shadows", settings.point_shadows);
    const auto mode = kernel.config().text("lighting.mode", "append");
    if (mode == "manual") { settings.publication = LightPublication::Manual; }
    if (mode == "scene") { settings.publication = LightPublication::SceneOnly; }
    if ((mode != "manual" && mode != "append" && mode != "scene") || !service.configure(settings)) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "render.lighting", "Lighting configuration rejected");
    }
    auto* service_pointer = &service;
    (void)kernel.add_shutdown("render.lighting", [service_pointer] { service_pointer->shutdown(); });
    (void)kernel.add_system({"render.lighting", core::Phase::Render, -100, {"camera.update", "spatial.sync"}},
        [](core::Kernel& host, const core::Frame&) {
            const bool complete = prepare_services(host);
            const auto& stats = host.services().get<LightingService>().statistics();
            host.metric("lighting.lights", static_cast<double>(stats.published_lights));
            host.metric("lighting.shadow_maps", static_cast<double>(stats.ready_maps));
            host.metric("lighting.shadow_rebuilds", static_cast<double>(stats.rebuilt_maps));
            host.metric("lighting.shadow_pixels", static_cast<double>(stats.resident_pixels));
            host.metric("lighting.partial", complete ? 0.0 : 1.0);
            host.metric("lighting.error", static_cast<double>(stats.error));
            if (stats.error == LightingError::OwnershipLost) {
                host.diagnose(core::Diagnostic::Severity::Warning, "render.lighting", std::string(describe(stats.error)));
            }
        });
    (void)kernel.order_after("render.rasterizer", "render.lighting");
    (void)kernel.add_command("lighting.stats", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: lighting.stats"}; }
        return {true, status_text(host.services().get<LightingService>())};
    }, "Inspect light ownership, shadow coverage, cache reuse and bounded work");
    (void)kernel.add_command("lighting.refresh", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: lighting.refresh"}; }
        const bool complete = prepare_services(host);
        return {complete, status_text(host.services().get<LightingService>())};
    }, "Prepare lights and offscreen shadow maps without rendering the main target");
    (void)kernel.add_command("lighting.mode", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: lighting.mode manual|append|scene"}; }
        auto& lighting = host.services().get<LightingService>();
        auto configuration = lighting.settings();
        if (args[0] == "manual") { configuration.publication = LightPublication::Manual; }
        else if (args[0] == "append") { configuration.publication = LightPublication::AppendScene; }
        else if (args[0] == "scene") { configuration.publication = LightPublication::SceneOnly; }
        else { return {false, "unknown publication mode"}; }
        const bool changed = lighting.configure(configuration);
        return {changed, changed ? "publication mode changed; existing manual lights retained for restoration" :
            std::string(describe(lighting.last_error()))};
    }, "Choose manual lights, append scene lights, or temporarily own the complete light list");
    (void)kernel.add_command("lighting.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: lighting.create name directional|point|spot"}; }
        auto& lighting = host.services().get<LightingService>();
        LightComponent component;
        component.shadow = lighting.settings().manual_shadows;
        if (args[1] == "directional") { component.light.type = LightType::Directional; }
        else if (args[1] == "point") { component.light.type = LightType::Point; }
        else if (args[1] == "spot") { component.light.type = LightType::Spot; }
        else { return {false, "unknown light type"}; }
        const auto entity = lighting.create(args[0], component);
        return {static_cast<bool>(entity), entity ? ecs::format_entity(entity) : "light creation rejected"};
    }, "Create a scene light using the shared Registry entity allocator");
    (void)kernel.add_command("lighting.detach", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: lighting.detach entity"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const bool removed = entity && host.services().get<LightingService>().detach(*entity);
        return {removed, removed ? "light component detached" : "light component not found"};
    }, "Detach lighting while preserving the scene entity and its other components");
    (void)kernel.add_command("lighting.set", edit_light, "Edit a real scene light's transform, radiance or shadow enablement");
}

} // namespace ge::render
