#include <ge/render/visibility.hpp>
#include <ge/core/runtime.hpp>
#include <ge/render/lighting.hpp>
#include <ge/scene/scene.hpp>
#include <ge/spatial/index.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace ge::render {
namespace {

constexpr double coordinate_limit = 1.0e12;

bool between(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool bounded(math::Vec3 value, double limit = coordinate_limit) noexcept {
    return between(value.x, -limit, limit) && between(value.y, -limit, limit) &&
        between(value.z, -limit, limit);
}

bool affine(const math::Mat4& matrix) noexcept {
    return math::finite(matrix) && matrix(3, 0) == 0.0 && matrix(3, 1) == 0.0 &&
        matrix(3, 2) == 0.0 && matrix(3, 3) == 1.0;
}

bool same_matrix(const math::Mat4& first, const math::Mat4& second) noexcept {
    return first.data == second.data;
}

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) { ++value; }
}

template<class T>
std::optional<T> parse_number(std::string_view text) noexcept {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) { return std::nullopt; }
    return value;
}

std::optional<bool> parse_bool(std::string_view text) noexcept {
    if (text == "on" || text == "true" || text == "1") { return true; }
    if (text == "off" || text == "false" || text == "0") { return false; }
    return std::nullopt;
}

ecs::Entity resolve_entity(const ecs::Registry& registry, std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos) { return registry.find_name(text); }
    const auto index = parse_number<std::uint32_t>(text.substr(0, colon));
    const auto generation = parse_number<std::uint64_t>(text.substr(colon + 1));
    const ecs::Entity entity{index.value_or(0), generation.value_or(0)};
    return registry.alive(entity) ? entity : ecs::Entity{};
}

std::uint64_t mix(std::uint64_t signature, std::uint64_t value) noexcept {
    return signature ^ (value + 0x9e3779b97f4a7c15ULL + (signature << 6U) + (signature >> 2U));
}

std::uint64_t policy_signature(const VisibilityComponent& component,
                               geometry::MeshHandle base) noexcept {
    auto result = mix(base.generation, base.index);
    result = mix(result, static_cast<std::uint64_t>(component.metric));
    result = mix(result, std::bit_cast<std::uint64_t>(component.lod_bias));
    result = mix(result, std::bit_cast<std::uint64_t>(component.hysteresis));
    result = mix(result, component.forced_lod.value_or(std::numeric_limits<std::size_t>::max()));
    for (const auto& level : component.levels) {
        result = mix(result, level.mesh.index);
        result = mix(result, level.mesh.generation);
        result = mix(result, std::bit_cast<std::uint64_t>(level.threshold));
        result = mix(result, level.first_submesh);
        result = mix(result, level.submesh_count);
    }
    return result;
}

std::optional<bool> inherited_enabled(const scene::Scene& scene, ecs::Entity entity,
                                       std::size_t depth_limit) noexcept {
    const auto& registry = scene.registry();
    for (std::size_t depth = 0; entity; ++depth) {
        if (depth == depth_limit) { return std::nullopt; }
        if (!registry.alive(entity) || !registry.enabled(entity)) { return false; }
        entity = scene.parent(entity);
    }
    return true;
}

double projected_diameter(const CameraView& camera, const math::Aabb& bounds) noexcept {
    math::Vec2 minimum{math::infinity, math::infinity};
    math::Vec2 maximum{-math::infinity, -math::infinity};
    for (unsigned corner = 0; corner < 8U; ++corner) {
        const math::Vec3 point{(corner & 1U) ? bounds.max.x : bounds.min.x,
            (corner & 2U) ? bounds.max.y : bounds.min.y,
            (corner & 4U) ? bounds.max.z : bounds.min.z};
        const auto clip = camera.view_projection * math::homogeneous(point);
        if (!math::finite(clip) || clip.w <= 1.0e-9) { return coordinate_limit; }
        const math::Vec2 pixel{(clip.x / clip.w * 0.5 + 0.5) * camera.viewport.width,
            (0.5 - clip.y / clip.w * 0.5) * camera.viewport.height};
        if (!math::finite(pixel)) { return coordinate_limit; }
        minimum.x = std::min(minimum.x, pixel.x);
        minimum.y = std::min(minimum.y, pixel.y);
        maximum.x = std::max(maximum.x, pixel.x);
        maximum.y = std::max(maximum.y, pixel.y);
    }
    return std::clamp(std::max(maximum.x - minimum.x, maximum.y - minimum.y), 0.0, coordinate_limit);
}

bool valid_camera(const CameraView& camera, bool allow_stale) noexcept {
    const double right = static_cast<double>(camera.viewport.x) + camera.viewport.width;
    const double bottom = static_cast<double>(camera.viewport.y) + camera.viewport.height;
    return !camera.viewport.empty() && between(camera.viewport.x, -1.0e6, 1.0e6) &&
        between(camera.viewport.y, -1.0e6, 1.0e6) && between(right, -1.0e6, 1.0e6) &&
        between(bottom, -1.0e6, 1.0e6) && math::finite(camera.view) &&
        math::finite(camera.view_projection) && bounded(camera.position) && bounded(camera.forward) &&
        (camera.clip_depth == math::ClipDepth::zero_to_one ||
         camera.clip_depth == math::ClipDepth::negative_one_to_one) &&
        (camera.projection_mode == ProjectionMode::Perspective ||
         camera.projection_mode == ProjectionMode::Orthographic) && (allow_stale || !camera.stale);
}

bool refresh_camera(CameraView& camera, const math::Mat4& world) noexcept {
    const auto inverse_world = math::inverse(world);
    if (!inverse_world) { return false; }
    const auto combined = camera.projection * *inverse_world;
    const auto inverse_combined = math::inverse(combined);
    const auto frustum = math::extract_frustum(combined, camera.clip_depth);
    if (!inverse_combined || !frustum) { return false; }
    const auto position = math::transform_point(world, {});
    auto forward = math::transform_vector(world, {0.0, 0.0, -1.0});
    auto up = math::transform_vector(world, {0.0, 1.0, 0.0});
    if (!bounded(position) || !math::normalize(forward) || !math::normalize(up)) { return false; }
    camera.inverse_view = world;
    camera.view = *inverse_world;
    camera.view_projection = combined;
    camera.inverse_view_projection = *inverse_combined;
    camera.frustum = *frustum;
    camera.position = position;
    camera.forward = forward;
    camera.up = up;
    return true;
}

} // namespace

std::string_view describe(VisibilityError error) noexcept {
    switch (error) {
    case VisibilityError::None: return "none";
    case VisibilityError::InvalidLimits: return "invalid limits";
    case VisibilityError::InvalidSettings: return "invalid settings";
    case VisibilityError::InvalidEntity: return "invalid entity";
    case VisibilityError::InvalidComponent: return "invalid visibility component";
    case VisibilityError::InvalidView: return "invalid camera view";
    case VisibilityError::MissingService: return "missing service";
    case VisibilityError::SceneRejected: return "scene update rejected";
    case VisibilityError::EntityLimit: return "entity limit";
    case VisibilityError::HierarchyLimit: return "hierarchy limit";
    case VisibilityError::ViewLimit: return "view limit";
    case VisibilityError::DrawLimit: return "draw limit";
    case VisibilityError::TriangleLimit: return "triangle limit";
    case VisibilityError::Incomplete: return "incomplete selection";
    case VisibilityError::AllocationFailure: return "allocation failure";
    case VisibilityError::BuilderConflict: return "submission builder ownership conflict";
    case VisibilityError::BuilderRejected: return "submission builder rejected";
    case VisibilityError::GraphConflict: return "scene raster ownership conflict";
    case VisibilityError::GraphRejected: return "render graph rejected";
    case VisibilityError::RasterRejected: return "rasterization rejected";
    case VisibilityError::Busy: return "visibility service busy";
    case VisibilityError::Stopped: return "visibility service stopped";
    }
    return "unknown visibility error";
}

bool valid(const VisibilityLimits& limits) noexcept {
    return limits.scene_entities > 0 && limits.scene_entities <= 1048576 &&
        limits.hierarchy_depth > 0 && limits.hierarchy_depth <= 4096 &&
        limits.views > 0 && limits.views <= 64 && limits.draws_per_view > 0 &&
        limits.draws_per_view <= 65536 && limits.total_draws >= limits.draws_per_view &&
        limits.total_draws <= 1048576 && limits.query_hits > 0 && limits.query_hits <= 1048576 &&
        limits.query_visits > 0 && limits.query_visits <= 4194304 && limits.lod_levels <= 16 &&
        limits.history_entries > 0 && limits.history_entries <= 1048576 && limits.history_frames <= 1000000;
}

bool valid(const VisibilitySettings& settings) noexcept {
    return between(settings.distance_scale, 1.0e-6, 1.0e6) &&
        between(settings.projected_size_scale, 1.0e-6, 1.0e6);
}

bool valid(const VisibilityComponent& component, const VisibilityLimits& limits) noexcept {
    if (!between(component.minimum_distance, 0.0, coordinate_limit) ||
        !between(component.maximum_distance, component.minimum_distance, coordinate_limit) ||
        !between(component.minimum_projected_diameter, 0.0, coordinate_limit) ||
        !between(component.lod_bias, 1.0e-6, 1.0e6) || !between(component.hysteresis, 0.0, 0.49) ||
        component.metric > LodMetric::ProjectedDiameter || component.missing_lod > MissingLod::Reject ||
        component.levels.size() > limits.lod_levels ||
        (component.forced_lod && *component.forced_lod > component.levels.size())) { return false; }
    double previous = component.metric == LodMetric::Distance ? 0.0 : coordinate_limit;
    for (const auto& level : component.levels) {
        if (!level.mesh || !between(level.threshold, 1.0e-9, coordinate_limit) ||
            (component.metric == LodMetric::Distance ? level.threshold <= previous : level.threshold >= previous)) {
            return false;
        }
        previous = level.threshold;
    }
    return true;
}

struct VisibilityService::Impl {
    struct HistoryKey {
        ecs::Entity entity{};
        ecs::Entity camera{};
        std::size_t slot{};
        auto operator<=>(const HistoryKey&) const = default;
    };
    struct History {
        std::uint64_t signature{};
        std::uint64_t seen{};
        std::size_t level{};
    };
    struct CameraBase {
        CameraView sampled{};
        math::Mat4 matrix{};
        math::Mat4 inverse{};
    };
    struct Candidate {
        ecs::Entity entity{};
        geometry::MeshComponent mesh{};
        math::Mat4 matrix{};
        math::Aabb bounds{};
        const VisibilityComponent* policy{};
        std::uint64_t layers{};
    };

    scene::Scene& scene;
    VisibilityLimits limits;
    VisibilitySettings settings;
    VisibilityStatistics statistics;
    std::optional<VisibilityRenderedFrame> rendered;
    std::vector<VisibilityView> views;
    std::vector<CameraBase> camera_bases;
    std::map<HistoryKey, History> history;
    std::shared_ptr<VisibilitySubmissionBuilder> builder;
    std::string builder_owner;
    GraphPass graph_pass{};
    std::uint64_t graph_revision{};
    bool busy{};
    bool stopped{};

    Impl(scene::Scene& source, VisibilityLimits configuration) : scene(source), limits(configuration) {
        if (!valid(limits)) { throw std::invalid_argument("invalid visibility limits"); }
        (void)scene.registry().register_component<VisibilityComponent>("render.visibility");
    }

    VisibilityResult result(VisibilityError error = VisibilityError::None) noexcept {
        statistics.error = error;
        if (error != VisibilityError::None) { statistics.complete = false; }
        return {error, statistics.complete, statistics.uncertain != 0, statistics.framebuffer_changed};
    }

    void stop() noexcept {
        if (busy) { return; }
        stopped = true;
        rendered.reset();
        builder.reset();
        builder_owner.clear();
        views.clear();
        camera_bases.clear();
        history.clear();
    }

    void reset_frame() noexcept {
        auto serial = statistics.collections;
        increment(serial);
        statistics = {};
        statistics.collections = serial;
        rendered.reset();
        views.clear();
        for (auto iterator = history.begin(); iterator != history.end();) {
            const auto& key = iterator->first;
            const auto seen = iterator->second.seen;
            if (!scene.registry().alive(key.entity) || !scene.registry().alive(key.camera) ||
                serial - seen > limits.history_frames) { iterator = history.erase(iterator); }
            else { ++iterator; }
        }
    }

    void uncertain(VisibilityView& view) noexcept {
        ++view.uncertain;
        ++statistics.uncertain;
    }

    void reject(VisibilityView& view) noexcept {
        ++statistics.invalid;
        view.complete = false;
        statistics.complete = false;
        uncertain(view);
    }

    VisibilityError refresh_scene(const geometry::MeshLibrary& meshes, spatial::SpatialIndex* index) {
        auto& registry = scene.registry();
        if (registry.size() > limits.scene_entities || scene.size() > limits.scene_entities) {
            return VisibilityError::EntityLimit;
        }
        for (const auto entity : registry.view<geometry::MeshComponent>(true)) {
            if (!scene.contains(entity)) { continue; }
            const auto* mesh = registry.get<geometry::MeshComponent>(entity);
            const auto info = meshes.inspect(mesh->mesh);
            if (!info || !math::valid(info->bounds)) { continue; }
            if (auto* bounds = registry.get<ecs::BoundsComponent>(entity)) {
                if (bounds->local.min != info->bounds.min || bounds->local.max != info->bounds.max) {
                    bounds->local = info->bounds;
                    (void)registry.mark_changed<ecs::BoundsComponent>(entity);
                }
            } else {
                registry.emplace<ecs::BoundsComponent>(entity, ecs::BoundsComponent{info->bounds, info->bounds});
            }
        }
        const auto update = scene.update();
        if (update.error != scene::SceneError::None) { return VisibilityError::SceneRejected; }
        statistics.scene_revision = scene.revision();
        if (index && settings.use_spatial_index) {
            const auto synchronized = index->sync(true);
            if (synchronized.scene_busy) { return VisibilityError::SceneRejected; }
            statistics.scene_revision = scene.revision();
        }
        return VisibilityError::None;
    }

    bool refresh_view(CameraView& camera) {
        if (!settings.refresh_camera_matrices) { return true; }
        const auto* current = scene.cached_world_matrix(camera.entity);
        if (!current) { return false; }
        const auto found = std::find_if(camera_bases.begin(), camera_bases.end(), [&](const CameraBase& base) {
            return base.sampled.entity == camera.entity && base.sampled.slot == camera.slot &&
                base.sampled.revision == camera.revision && same_matrix(base.sampled.inverse_view, camera.inverse_view);
        });
        if (found == camera_bases.end()) {
            if (!same_matrix(*current, camera.inverse_view)) { camera.stale = true; }
            return true;
        }
        if (same_matrix(*current, found->matrix)) { return true; }
        const auto render_world = *current * found->inverse * camera.inverse_view;
        if (!refresh_camera(camera, render_world)) { return false; }
        ++statistics.refreshed_cameras;
        return true;
    }

    VisibilityError gather(const geometry::MeshLibrary& meshes, std::vector<Candidate>& output,
                            std::size_t hierarchy_limit) {
        static const VisibilityComponent defaults;
        const auto entities = scene.ordered_entities();
        std::vector<std::pair<ecs::Entity, bool>> ancestors;
        ancestors.reserve(std::min(entities.size(), hierarchy_limit));
        output.reserve(std::min(entities.size(), limits.scene_entities));
        const auto& registry = scene.registry();
        for (const auto entity : entities) {
            ++statistics.examined;
            const auto parent = scene.parent(entity);
            while (!ancestors.empty() && ancestors.back().first != parent) { ancestors.pop_back(); }
            const bool enabled = registry.enabled(entity) &&
                (!parent || (!ancestors.empty() && ancestors.back().second));
            if (ancestors.size() == hierarchy_limit) { return VisibilityError::HierarchyLimit; }
            ancestors.emplace_back(entity, enabled);
            const auto* component = registry.get<geometry::MeshComponent>(entity);
            if (!component) { continue; }
            const auto* policy = registry.get<VisibilityComponent>(entity);
            if (!policy) { policy = &defaults; }
            if (!enabled || !component->visible || !policy->enabled) { ++statistics.disabled; continue; }
            const auto* matrix = scene.cached_world_matrix(entity);
            const auto info = meshes.inspect(component->mesh);
            if (!valid(*policy, limits) || !matrix || !affine(*matrix) || !info || !math::valid(info->bounds)) {
                ++statistics.invalid;
                ++statistics.uncertain;
                statistics.complete = false;
                continue;
            }
            auto local = info->bounds;
            if (settings.enabled) {
                for (const auto& level : policy->levels) {
                    const auto alternate = meshes.inspect(level.mesh);
                    if (alternate && math::valid(alternate->bounds)) { local = math::merge(local, alternate->bounds); }
                }
            }
            const auto world = math::transform_bounds(*matrix, local);
            if (!math::valid(world) || !bounded(world.min) || !bounded(world.max)) {
                ++statistics.invalid;
                ++statistics.uncertain;
                statistics.complete = false;
                continue;
            }
            output.push_back({entity, *component, *matrix, world, policy, registry.layers(entity)});
        }
        return VisibilityError::None;
    }

    std::size_t choose_lod(const Candidate& candidate, VisibilityView& view,
                           double distance, double diameter) {
        const auto& policy = *candidate.policy;
        if (!settings.enabled || policy.levels.empty()) { return 0; }
        const HistoryKey key{candidate.entity, view.camera.entity, view.camera.slot};
        const auto signature = policy_signature(policy, candidate.mesh.mesh);
        const auto found = history.find(key);
        const bool retained = found != history.end() && found->second.signature == signature;
        const double metric = policy.metric == LodMetric::Distance ?
            distance * settings.distance_scale * policy.lod_bias :
            diameter * settings.projected_size_scale / policy.lod_bias;
        std::size_t selected{};
        if (policy.forced_lod) { selected = *policy.forced_lod; }
        else if (retained) {
            selected = std::min(found->second.level, policy.levels.size());
            while (selected < policy.levels.size()) {
                const double boundary = policy.levels[selected].threshold;
                const bool coarser = policy.metric == LodMetric::Distance ?
                    metric >= boundary * (1.0 + policy.hysteresis) :
                    metric <= boundary * (1.0 - policy.hysteresis);
                if (!coarser) { break; }
                ++selected;
            }
            while (selected > 0) {
                const double boundary = policy.levels[selected - 1].threshold;
                const bool finer = policy.metric == LodMetric::Distance ?
                    metric < boundary * (1.0 - policy.hysteresis) :
                    metric > boundary * (1.0 + policy.hysteresis);
                if (!finer) { break; }
                --selected;
            }
        } else {
            for (const auto& level : policy.levels) {
                const bool coarser = policy.metric == LodMetric::Distance ?
                    metric >= level.threshold : metric <= level.threshold;
                if (!coarser) { break; }
                ++selected;
            }
        }
        if (retained && found->second.level != selected) { ++statistics.lod_changes; }
        if (found != history.end()) { found->second = {signature, statistics.collections, selected}; }
        else if (history.size() < limits.history_entries) {
            history.emplace(key, History{signature, statistics.collections, selected});
        } else {
            ++statistics.history_dropped;
            uncertain(view);
        }
        return selected;
    }

    std::vector<ecs::Entity> query_index(VisibilityView& view, spatial::SpatialIndex* index) {
        std::vector<ecs::Entity> matching;
        if (!settings.enabled || !settings.use_spatial_index || !index) { return matching; }
        auto& query = view.query;
        query.used_index = true;
        const auto& sync = index->last_sync();
        query.stale = sync.scene_busy || sync.rejected != 0 || sync.capacity_failures != 0 ||
            sync.scene_revision != scene.revision();
        if (query.stale) {
            query.recovered_by_direct_tests = true;
            ++statistics.query_fallbacks;
            return matching;
        }
        std::vector<spatial::QueryHit> hits(std::min(limits.query_hits, index->tree().size()));
        spatial::QueryFilter filter;
        filter.layers = view.camera.visibility_mask;
        filter.max_visits = limits.query_visits;
        const auto report = index->tree().query_frustum(view.camera.frustum, hits, filter);
        query.visited = report.visited;
        query.matched = report.matched;
        query.written = report.written;
        query.truncated = report.truncated || report.traversal_exhausted;
        query.invalid = report.invalid_input;
        query.stale = report.stale;
        if (query.truncated || query.invalid || query.stale) {
            query.recovered_by_direct_tests = true;
            ++statistics.query_fallbacks;
            return matching;
        }
        matching.reserve(report.written);
        for (std::size_t hit = 0; hit < report.written; ++hit) { matching.push_back(hits[hit].entity); }
        std::sort(matching.begin(), matching.end());
        matching.erase(std::unique(matching.begin(), matching.end()), matching.end());
        return matching;
    }

    VisibilityError append(const Candidate& candidate, VisibilityView& view,
                            const geometry::MeshLibrary& meshes, const MaterialService& materials,
                            std::size_t selected, double distance, double diameter,
                            std::size_t draw_limit) {
        auto handle = candidate.mesh.mesh;
        auto first = candidate.mesh.first_submesh;
        auto count = candidate.mesh.submesh_count;
        if (selected != 0) {
            const auto& level = candidate.policy->levels[selected - 1];
            handle = level.mesh;
            first = level.first_submesh;
            count = level.submesh_count;
            if (!meshes.contains(handle)) {
                if (candidate.policy->missing_lod == MissingLod::Reject) { reject(view); return VisibilityError::None; }
                handle = candidate.mesh.mesh;
                first = candidate.mesh.first_submesh;
                count = candidate.mesh.submesh_count;
                selected = 0;
                ++statistics.lod_fallbacks;
            }
        }
        const auto* mesh = meshes.get(handle);
        const auto info = meshes.inspect(handle);
        if (!mesh || !info) { reject(view); return VisibilityError::None; }
        if (mesh->empty()) { return VisibilityError::None; }
        const auto range_count = std::max(std::size_t{1}, mesh->submeshes.size());
        const auto begin = static_cast<std::size_t>(first);
        if (begin >= range_count) { reject(view); return VisibilityError::None; }
        const auto remaining = range_count - begin;
        const auto ranges = count == 0 ? remaining : std::min(remaining, static_cast<std::size_t>(count));
        if (ranges > draw_limit - view.entries.size() || ranges > limits.total_draws - statistics.selected_draws) {
            view.complete = false;
            return VisibilityError::DrawLimit;
        }
        const auto actual_bounds = math::transform_bounds(candidate.matrix, info->bounds);
        for (std::size_t range = begin; range < begin + ranges; ++range) {
            VisibilityEntry entry;
            entry.mesh = handle;
            entry.mesh_revision = info->revision;
            entry.lod = selected;
            entry.ordinal = view.entries.size();
            entry.world_bounds = actual_bounds;
            entry.distance = distance;
            entry.projected_diameter = diameter;
            entry.draw.mesh = mesh;
            entry.draw.model = candidate.matrix;
            entry.draw.entity = candidate.entity;
            entry.draw.visibility_mask = candidate.layers & candidate.policy->view_mask;
            entry.draw.light_mask = candidate.layers & candidate.policy->light_mask;
            if (!mesh->submeshes.empty()) {
                const auto& submesh = mesh->submeshes[range];
                entry.draw.first_index = submesh.first_index;
                entry.draw.index_count = submesh.index_count;
                entry.material_slot = submesh.material_slot;
            }
            entry.material = materials.materials().assigned(scene.registry(), candidate.entity, entry.material_slot);
            if (!materials.materials().get(entry.material)) { entry.material = {}; }
            entry.draw.material = &materials.resolve(scene.registry(), candidate.entity, entry.material_slot);
            view.entries.push_back(entry);
            ++statistics.selected_draws;
        }
        return VisibilityError::None;
    }

    VisibilityError select(VisibilityView& view, std::span<const Candidate> candidates,
                            const geometry::MeshLibrary& meshes, const MaterialService& materials,
                            spatial::SpatialIndex* index, std::size_t draw_limit) {
        const auto indexed = query_index(view, index);
        view.entries.reserve(std::min(candidates.size(), draw_limit));
        for (const auto& candidate : candidates) {
            ++view.considered;
            const auto& policy = *candidate.policy;
            if ((candidate.layers & policy.view_mask & view.camera.visibility_mask) == 0) {
                ++statistics.layer_culled;
                ++view.culled;
                continue;
            }
            if (settings.enabled && policy.frustum_culling) {
                bool visible{};
                const auto proxy_bounds = index ? index->world_bounds(candidate.entity) : std::nullopt;
                const bool covered = view.query.used_index && !view.query.recovered_by_direct_tests &&
                    proxy_bounds && math::contains(*proxy_bounds, candidate.bounds);
                if (covered) { visible = std::binary_search(indexed.begin(), indexed.end(), candidate.entity); }
                else {
                    ++view.query.direct_tests;
                    visible = math::intersects(view.camera.frustum, candidate.bounds);
                }
                if (!visible) { ++statistics.frustum_culled; ++view.culled; continue; }
            }
            const double distance = math::length(math::center(candidate.bounds) - view.camera.position);
            const double radius = math::length(math::half_extent(candidate.bounds));
            const double diameter = projected_diameter(view.camera, candidate.bounds);
            if (!std::isfinite(distance) || !std::isfinite(radius)) { reject(view); continue; }
            if (settings.enabled && (distance + radius < policy.minimum_distance ||
                std::max(0.0, distance - radius) > policy.maximum_distance)) {
                ++statistics.distance_culled;
                ++view.culled;
                continue;
            }
            if (settings.enabled && diameter * settings.projected_size_scale < policy.minimum_projected_diameter) {
                ++statistics.size_culled;
                ++view.culled;
                continue;
            }
            const auto selected = choose_lod(candidate, view, distance, diameter);
            const auto error = append(candidate, view, meshes, materials, selected, distance, diameter, draw_limit);
            if (error != VisibilityError::None) { return error; }
        }
        return VisibilityError::None;
    }

    VisibilityResult collect(const geometry::MeshLibrary& meshes, const CameraService& cameras,
                              const MaterialService& materials, const SoftwareRenderer& renderer,
                              spatial::SpatialIndex* index) {
        reset_frame();
        if (scene.size() > renderer.limits().max_scene_entities || scene.registry().size() > renderer.limits().max_scene_entities) {
            return result(VisibilityError::EntityLimit);
        }
        const auto refreshed = refresh_scene(meshes, index);
        if (refreshed != VisibilityError::None) { return result(refreshed); }
        std::vector<CameraView> selected_views;
        if (renderer.settings().render_all_views) {
            const auto available = cameras.views();
            if (available.size() > std::min(limits.views, renderer.limits().max_views)) {
                return result(VisibilityError::ViewLimit);
            }
            selected_views.assign(available.begin(), available.end());
        } else if (const auto* current = cameras.current_view()) { selected_views.push_back(*current); }
        if (selected_views.empty()) { return result(VisibilityError::InvalidView); }
        std::vector<Candidate> candidates;
        const auto hierarchy_limit = std::min(limits.hierarchy_depth, renderer.limits().max_hierarchy_depth);
        const auto gathered = gather(meshes, candidates, hierarchy_limit);
        if (gathered != VisibilityError::None) { return result(gathered); }
        views.reserve(selected_views.size());
        for (auto camera : selected_views) {
            const auto enabled = inherited_enabled(scene, camera.entity, hierarchy_limit);
            if (!enabled) { return result(VisibilityError::HierarchyLimit); }
            const auto* component = cameras.component(camera.entity);
            if (!*enabled || !component || !component->enabled) { continue; }
            if (!refresh_view(camera) || !valid_camera(camera, renderer.settings().allow_stale_views)) {
                return result(VisibilityError::InvalidView);
            }
            const auto frustum = math::extract_frustum(camera.view_projection, camera.clip_depth);
            if (!frustum) { return result(VisibilityError::InvalidView); }
            camera.frustum = *frustum;
            camera.visibility_mask = component->visibility_mask;
            VisibilityView selected;
            selected.camera = camera;
            selected.complete = statistics.complete;
            if (camera.stale) { uncertain(selected); }
            const auto error = select(selected, candidates, meshes, materials, index,
                std::min(limits.draws_per_view, renderer.limits().max_draws));
            views.push_back(std::move(selected));
            if (error != VisibilityError::None) { return result(error); }
        }
        statistics.views = views.size();
        if (views.empty()) { return result(VisibilityError::InvalidView); }
        return result(statistics.complete ? VisibilityError::None : VisibilityError::Incomplete);
    }
};

VisibilityService::VisibilityService(scene::Scene& scene, VisibilityLimits limits)
    : impl_(std::make_shared<Impl>(scene, limits)) {}
VisibilityService::~VisibilityService() = default;

bool VisibilityService::configure(const VisibilitySettings& settings) noexcept {
    if (impl_->busy || impl_->stopped || !valid(settings)) { return false; }
    impl_->settings = settings;
    impl_->history.clear();
    return true;
}

const VisibilitySettings& VisibilityService::settings() const noexcept { return impl_->settings; }
const VisibilityLimits& VisibilityService::limits() const noexcept { return impl_->limits; }
const VisibilityStatistics& VisibilityService::statistics() const noexcept { return impl_->statistics; }
std::optional<VisibilityRenderedFrame> VisibilityService::rendered_frame() const { return impl_->rendered; }
std::span<const VisibilityView> VisibilityService::views() const noexcept { return impl_->views; }

const VisibilityView* VisibilityService::view(std::size_t slot) const noexcept {
    const auto found = std::find_if(impl_->views.begin(), impl_->views.end(),
        [slot](const VisibilityView& value) { return value.camera.slot == slot; });
    return found == impl_->views.end() ? nullptr : &*found;
}

bool VisibilityService::attach(ecs::Entity entity, const VisibilityComponent& component) {
    if (impl_->busy || impl_->stopped || !impl_->scene.contains(entity) ||
        !impl_->scene.registry().alive(entity) || !valid(component, impl_->limits)) { return false; }
    impl_->scene.registry().emplace_or_replace<VisibilityComponent>(entity, component);
    return true;
}

bool VisibilityService::detach(ecs::Entity entity) {
    return !impl_->busy && !impl_->stopped && impl_->scene.registry().remove<VisibilityComponent>(entity);
}

const VisibilityComponent* VisibilityService::component(ecs::Entity entity) const noexcept {
    return impl_->scene.registry().get<VisibilityComponent>(entity);
}

void VisibilityService::reset_history() noexcept {
    if (!impl_->busy) { impl_->history.clear(); }
}

void VisibilityService::capture_camera_bases(const CameraService& cameras) {
    if (impl_->busy || impl_->stopped) { return; }
    impl_->camera_bases.clear();
    if (cameras.views().size() > impl_->limits.views || impl_->scene.size() > impl_->limits.scene_entities ||
        impl_->scene.registry().size() > impl_->limits.scene_entities) { return; }
    try {
        if (impl_->scene.update().error != scene::SceneError::None) { return; }
        impl_->camera_bases.reserve(cameras.views().size());
        for (const auto& camera : cameras.views()) {
            const auto* matrix = impl_->scene.cached_world_matrix(camera.entity);
            if (!matrix || camera.stale) { continue; }
            const auto inverse = math::inverse(*matrix);
            if (inverse) { impl_->camera_bases.push_back({camera, *matrix, *inverse}); }
        }
    } catch (const std::bad_alloc&) { impl_->camera_bases.clear(); }
    catch (const std::length_error&) { impl_->camera_bases.clear(); }
}

namespace {
struct BusyScope {
    bool& busy;
    explicit BusyScope(bool& flag) noexcept : busy(flag) { busy = true; }
    ~BusyScope() { busy = false; }
};
}

VisibilityResult VisibilityService::collect(const geometry::MeshLibrary& meshes, const CameraService& cameras,
                                            const MaterialService& materials, const SoftwareRenderer& renderer,
                                            spatial::SpatialIndex* spatial) {
    impl_->rendered.reset();
    if (impl_->stopped) { return impl_->result(VisibilityError::Stopped); }
    if (impl_->busy) { return {VisibilityError::Busy, false, true, false}; }
    BusyScope scope(impl_->busy);
    try { return impl_->collect(meshes, cameras, materials, renderer, spatial); }
    catch (const std::bad_alloc&) { return impl_->result(VisibilityError::AllocationFailure); }
    catch (const std::length_error&) { return impl_->result(VisibilityError::AllocationFailure); }
}

VisibilityResult VisibilityService::render(const geometry::MeshLibrary& meshes, const CameraService& cameras,
                                           const MaterialService& materials, SoftwareRenderer& renderer,
                                           ImageService& images, spatial::SpatialIndex* spatial,
                                           const LightingService* lighting) {
    impl_->rendered.reset();
    if (impl_->stopped) { return impl_->result(VisibilityError::Stopped); }
    if (impl_->busy) { return {VisibilityError::Busy, false, true, false}; }
    BusyScope scope(impl_->busy);
    try {
        if (!renderer.settings().enabled) { impl_->reset_frame(); return impl_->result(); }
        const auto selection = impl_->collect(meshes, cameras, materials, renderer, spatial);
        if (!selection && (!impl_->settings.render_incomplete || selection.error != VisibilityError::Incomplete)) {
            return selection;
        }
        std::vector<VisibilitySubmission> outputs(impl_->views.size());
        std::vector<RasterViewSubmission> submissions;
        submissions.reserve(outputs.size());
        const auto retained_builder = impl_->builder;
        const auto registry_revision = impl_->scene.registry().revision();
        const auto mesh_revision = meshes.statistics().revision;
        const auto material_revision = materials.materials().revision();
        std::size_t total_draws{};
        std::size_t triangles{};
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            auto& output = outputs[index];
            const auto& selected = impl_->views[index];
            const auto maximum = std::min(impl_->limits.draws_per_view, renderer.limits().max_draws);
            if (retained_builder) {
                VisibilityBuildResult built;
                try {
                    built = retained_builder->build({impl_->scene, meshes, materials, selected,
                        renderer.settings(), renderer.limits(), maximum}, output);
                } catch (...) { return impl_->result(VisibilityError::BuilderRejected); }
                if (!built.success) { return impl_->result(VisibilityError::BuilderRejected); }
                if (built.uncertain) { ++impl_->statistics.uncertain; }
                if (!built.complete) {
                    impl_->statistics.complete = false;
                    if (!impl_->settings.render_incomplete) { return impl_->result(VisibilityError::Incomplete); }
                }
            } else {
                output.draws.reserve(selected.entries.size());
                for (const auto& entry : selected.entries) { output.draws.push_back(entry.draw); }
            }
            if (registry_revision != impl_->scene.registry().revision() || mesh_revision != meshes.statistics().revision ||
                material_revision != materials.materials().revision() || impl_->statistics.scene_revision != impl_->scene.revision()) {
                return impl_->result(VisibilityError::BuilderRejected);
            }
            if (output.draws.size() > maximum || output.owned_meshes.size() > maximum ||
                output.owned_materials.size() > maximum || output.draws.size() > impl_->limits.total_draws - total_draws) {
                return impl_->result(VisibilityError::DrawLimit);
            }
            std::unordered_set<const geometry::MeshData*> live_meshes;
            std::unordered_set<const PbrMaterial*> live_materials;
            for (const auto& entry : selected.entries) {
                live_meshes.insert(entry.draw.mesh);
                live_materials.insert(entry.draw.material);
            }
            for (const auto& owned : output.owned_meshes) { if (owned) { live_meshes.insert(owned.get()); } }
            for (const auto& owned : output.owned_materials) { if (owned) { live_materials.insert(owned.get()); } }
            for (const auto& draw : output.draws) {
                if (!draw.mesh || !live_meshes.contains(draw.mesh) ||
                    (draw.material && !live_materials.contains(draw.material)) ||
                    (draw.entity && !impl_->scene.registry().alive(draw.entity)) ||
                    draw.first_index > draw.mesh->indices.size()) { return impl_->result(VisibilityError::BuilderRejected); }
                if ((draw.visibility_mask & selected.camera.visibility_mask) == 0) { continue; }
                const auto available = draw.mesh->indices.size() - draw.first_index;
                const auto count = draw.index_count == 0 ? available : draw.index_count;
                if (count > available || count % 3 != 0) { return impl_->result(VisibilityError::BuilderRejected); }
                if (count / 3 > renderer.limits().max_input_triangles - triangles) {
                    return impl_->result(VisibilityError::TriangleLimit);
                }
                triangles += count / 3;
            }
            total_draws += output.draws.size();
            auto shading = materials.context(-selected.camera.forward);
            if (lighting) { shading.shadow_factor = LightingService::shadow_factor; shading.shadow_user = lighting; }
            submissions.push_back({selected.camera, output.draws, shading});
        }
        impl_->statistics.submitted_draws = total_draws;
        const auto drawn = renderer.draw_views(images.framebuffer(), submissions, &images.textures(), true);
        const auto& raster = renderer.statistics();
        impl_->statistics.raster_error = drawn.error;
        impl_->statistics.framebuffer_changed = drawn.framebuffer_changed;
        impl_->statistics.input_triangles = raster.input_triangles;
        impl_->statistics.rendered_draws = raster.draws_rendered;
        impl_->statistics.fragment_visits = raster.fragment_visits;
        impl_->statistics.fragments_written = raster.fragments_written;
        impl_->statistics.cleared_pixels = raster.cleared_pixels;
        if (!drawn) { return impl_->result(VisibilityError::RasterRejected); }
        return impl_->result(impl_->statistics.complete ? VisibilityError::None : VisibilityError::Incomplete);
    } catch (const std::bad_alloc&) { return impl_->result(VisibilityError::AllocationFailure); }
    catch (const std::length_error&) { return impl_->result(VisibilityError::AllocationFailure); }
}

bool VisibilityService::set_submission_builder(std::string_view owner,
                                               std::shared_ptr<VisibilitySubmissionBuilder> builder) {
    if (impl_->busy || impl_->stopped || owner.empty() || owner.size() > 128 || !builder ||
        (!impl_->builder_owner.empty() && impl_->builder_owner != owner)) { return false; }
    impl_->builder_owner = owner;
    impl_->builder = std::move(builder);
    return true;
}

bool VisibilityService::clear_submission_builder(std::string_view owner) noexcept {
    if (impl_->busy || impl_->builder_owner != owner) { return false; }
    impl_->builder.reset();
    impl_->builder_owner.clear();
    return true;
}

std::string_view VisibilityService::submission_builder_owner() const noexcept { return impl_->builder_owner; }

void VisibilityService::shutdown() noexcept {
    impl_->stop();
}

bool VisibilityService::install_graph(core::Kernel& kernel) {
    const auto fail = [&](VisibilityError error) {
        impl_->rendered.reset();
        impl_->statistics.error = error;
        return false;
    };
    if (impl_->stopped) { return fail(VisibilityError::Stopped); }
    if (impl_->busy) { return fail(VisibilityError::Busy); }
    auto* graph = kernel.services().find<RenderGraph>();
    if (!graph) { return fail(VisibilityError::MissingService); }
    const auto pass = graph->find_pass(graph_scene_pass);
    const auto color = graph->find_resource(graph_main_color);
    const auto depth = graph->find_resource(graph_main_depth);
    if (!pass || !color || !depth) { return fail(VisibilityError::GraphRejected); }
    const auto info = graph->pass_info(*pass);
    if (!info) { return fail(VisibilityError::GraphRejected); }
    if (*pass == impl_->graph_pass) {
        return (info->callback_owner == visibility_callback_owner && info->callback_revision == impl_->graph_revision)
            || fail(VisibilityError::GraphConflict);
    }
    if (info->callback_owner != graph_default_scene_callback_owner || info->resources.size() != 2) {
        return fail(VisibilityError::GraphConflict);
    }
    const auto declared = [&](GraphResource resource) {
        return std::any_of(info->resources.begin(), info->resources.end(), [&](const GraphUse& use) {
            return use.resource == resource && use.access == GraphAccess::ReadWrite;
        });
    };
    if (!declared(*color) || !declared(*depth)) { return fail(VisibilityError::GraphConflict); }
    try {
        const std::weak_ptr<Impl> weak = impl_;
        auto callback = [weak, host = &kernel, color_resource = *color, depth_resource = *depth](GraphContext& context) -> GraphPassResult {
            const auto state = weak.lock();
            if (!state || state->stopped) { return {false, false, "visibility owner stopped"}; }
            state->rendered.reset();
            auto* service = host->services().find<VisibilityService>();
            auto* renderer = host->services().find<SoftwareRenderer>();
            auto* images = host->services().find<ImageService>();
            const auto* meshes = host->services().find<geometry::MeshLibrary>();
            const auto* cameras = host->services().find<CameraService>();
            const auto* materials = host->services().find<MaterialService>();
            if (!service || service->impl_ != state || !renderer || !images || !meshes || !cameras || !materials) {
                return {false, false, "visibility render dependency unavailable"};
            }
            const auto target_depth = context.write_depth(depth_resource);
            if (context.write_image(color_resource) != &images->framebuffer().color() ||
                target_depth.data() != images->framebuffer().depth().data() ||
                target_depth.size() != images->framebuffer().depth().size()) {
                return {false, false, "visibility graph attachments no longer identify the shared framebuffer"};
            }
            VisibilityRenderedFrame rendered;
            rendered.views.reserve(state->limits.views);
            rendered.raster_settings = renderer->settings();
            const auto outcome = service->render(*meshes, *cameras, *materials, *renderer, *images,
                host->services().find<spatial::SpatialIndex>(), host->services().find<LightingService>());
            if (outcome && renderer->statistics().views != 0 && rendered.raster_settings.enabled) {
                rendered.tick = context.frame().tick;
                rendered.pass = context.pass();
                rendered.callback_owner = visibility_callback_owner;
                rendered.callback_revision = state->graph_revision;
                rendered.raster_frame = renderer->statistics().frame;
                rendered.target = &images->framebuffer();
                rendered.width = images->framebuffer().width();
                rendered.height = images->framebuffer().height();
                for (const auto& selected : state->views) { rendered.views.push_back(selected.camera); }
                state->rendered = std::move(rendered);
            }
            const auto& statistics = service->statistics();
            host->metric("render.visibility.selected", static_cast<double>(statistics.selected_draws));
            host->metric("render.visibility.uncertain", static_cast<double>(statistics.uncertain));
            host->metric("render.visibility.complete", statistics.complete ? 1.0 : 0.0);
            host->metric("render.visibility.error", static_cast<double>(statistics.error));
            host->metric("render.triangles", static_cast<double>(statistics.input_triangles));
            host->metric("render.draws", static_cast<double>(statistics.rendered_draws));
            host->metric("render.fragments", static_cast<double>(statistics.fragments_written));
            host->metric("render.fragment_visits", static_cast<double>(statistics.fragment_visits));
            host->metric("render.error", static_cast<double>(statistics.raster_error));
            host->metric("render.work_limited", statistics.raster_error == RasterError::WorkLimit ? 1.0 : 0.0);
            return {static_cast<bool>(outcome), outcome.framebuffer_changed, std::string(describe(outcome.error))};
        };
        if (!graph->replace_callback_if(*pass, info->callback_owner, info->callback_revision,
            visibility_callback_owner, std::move(callback))) { return fail(VisibilityError::GraphRejected); }
        impl_->graph_pass = *pass;
        impl_->graph_revision = graph->pass_info(*pass)->callback_revision;
        return true;
    } catch (const std::bad_alloc&) { return fail(VisibilityError::AllocationFailure); }
    catch (const std::length_error&) { return fail(VisibilityError::AllocationFailure); }
}

namespace {
std::string statistics_text(const VisibilityService& service) {
    const auto& stats = service.statistics();
    std::ostringstream text;
    text << describe(stats.error) << " collection=" << stats.collections << " views=" << stats.views
         << " examined=" << stats.examined << " selected=" << stats.selected_draws << " submitted=" << stats.submitted_draws
         << " invalid=" << stats.invalid << " frustum_culled=" << stats.frustum_culled << " layer_culled=" << stats.layer_culled
         << " distance_culled=" << stats.distance_culled << " size_culled=" << stats.size_culled
         << " lod_changes=" << stats.lod_changes << " lod_fallbacks=" << stats.lod_fallbacks
         << " query_fallbacks=" << stats.query_fallbacks << " uncertain=" << stats.uncertain
         << " complete=" << stats.complete << " triangles=" << stats.input_triangles
         << " raster=" << describe(stats.raster_error) << " builder=" << service.submission_builder_owner();
    return text.str();
}

core::CommandResult edit_component(core::Kernel& host, core::CommandArgs args, std::string_view operation) {
    auto& service = host.services().get<VisibilityService>();
    auto& scene = host.services().get<scene::Scene>();
    if (args.empty()) { return {false, "visibility component command requires ENTITY"}; }
    const auto entity = resolve_entity(scene.registry(), args[0]);
    if (!entity) { return {false, "entity name or index:generation is not live"}; }
    const auto* previous = service.component(entity);
    if (previous && !valid(*previous, service.limits())) { return {false, "existing policy is invalid; detach before replacing"}; }
    auto component = previous ? *previous : VisibilityComponent{};
    if (operation == "detach") {
        return {args.size() == 1 && service.detach(entity), "visibility component detach"};
    }
    if (operation == "attach") {
        if (args.size() > 2) { return {false, "usage: visibility.attach ENTITY [distance|pixels]"}; }
        if (args.size() == 2) {
            if (args[1] != "distance" && args[1] != "pixels") { return {false, "expected distance or pixels"}; }
            component.metric = args[1] == "distance" ? LodMetric::Distance : LodMetric::ProjectedDiameter;
        }
    } else if (operation == "range") {
        if (args.size() != 4) { return {false, "usage: visibility.range ENTITY MIN_METRES MAX_METRES MIN_PIXELS"}; }
        const auto minimum = parse_number<double>(args[1]);
        const auto maximum = parse_number<double>(args[2]);
        const auto pixels = parse_number<double>(args[3]);
        if (!minimum || !maximum || !pixels) { return {false, "expected finite numeric policy values"}; }
        component.minimum_distance = *minimum;
        component.maximum_distance = *maximum;
        component.minimum_projected_diameter = *pixels;
    } else if (operation == "lod") {
        if (args.size() != 3) { return {false, "usage: visibility.lod ENTITY MESH_NAME THRESHOLD"}; }
        const auto* meshes = host.services().find<geometry::MeshLibrary>();
        const auto threshold = parse_number<double>(args[2]);
        const auto mesh = meshes ? meshes->find(args[1]) : geometry::MeshHandle{};
        if (!mesh || !threshold || component.levels.size() == service.limits().lod_levels) {
            return {false, "missing mesh, invalid threshold or LOD capacity exhausted"};
        }
        component.levels.push_back({mesh, 0, 0, *threshold});
    } else if (operation == "force") {
        if (args.size() != 2) { return {false, "usage: visibility.force ENTITY auto|LEVEL"}; }
        if (args[1] == "auto") { component.forced_lod.reset(); }
        else {
            const auto level = parse_number<std::size_t>(args[1]);
            if (!level) { return {false, "invalid LOD level"}; }
            component.forced_lod = *level;
        }
    } else { return {false, "unknown visibility edit"}; }
    const bool attached = service.attach(entity, component);
    return {attached, attached ? "visibility policy updated" : "policy rejected; thresholds must be ordered and ranges valid"};
}
}

void register_visibility(core::Kernel& kernel) {
    if (!kernel.register_module({"render.visibility", "Per-view conservative scene culling and hysteretic mesh LOD", 1})) { return; }
    auto* scene = kernel.services().find<scene::Scene>();
    if (!scene) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "render.visibility", "Scene service required");
        return;
    }
    auto& service = kernel.services().emplace<VisibilityService>(*scene);
    auto settings = service.settings();
    settings.enabled = kernel.config().boolean("render.visibility.enabled", true);
    (void)service.configure(settings);
    (void)kernel.add_system({"render.visibility.camera_capture", core::Phase::Visibility, -7990, {"camera.update"}},
        [](core::Kernel& host, const core::Frame&) {
            if (const auto* cameras = host.services().find<CameraService>()) {
                host.services().get<VisibilityService>().capture_camera_bases(*cameras);
            }
        });
    (void)kernel.add_system({"render.visibility.install", core::Phase::Render, -70, {"render.lighting", "spatial.sync"}},
        [reported = VisibilityError::None](core::Kernel& host, const core::Frame&) mutable {
            auto& current = host.services().get<VisibilityService>();
            if (current.install_graph(host)) { reported = VisibilityError::None; return; }
            const auto error = current.statistics().error;
            if (reported != error) {
                reported = error;
                host.diagnose(core::Diagnostic::Severity::Warning, "render.visibility", std::string(describe(error)));
            }
        });
    (void)kernel.order_after("render.graph.frame", "render.visibility.install");
    (void)kernel.order_after("gameplay.interaction.update", "render.visibility.camera_capture");
    (void)kernel.add_shutdown("render.visibility", [weak = std::weak_ptr<VisibilityService::Impl>{service.impl_}] {
        if (const auto state = weak.lock()) { state->stop(); }
    });
    (void)kernel.add_command("visibility.stats", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        return {args.empty(), args.empty() ? statistics_text(host.services().get<VisibilityService>()) : "usage: visibility.stats"};
    }, "Inspect actual selected draws, culling, LOD, uncertainty and raster outcomes");
    (void)kernel.add_command("visibility.enabled", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) { return {false, "usage: visibility.enabled on|off"}; }
        const auto flag = parse_bool(args[0]);
        auto& current = host.services().get<VisibilityService>();
        auto configuration = current.settings();
        if (!flag) { return {false, "expected on or off"}; }
        configuration.enabled = *flag;
        return {current.configure(configuration), "visibility culling and automatic LOD policy updated"};
    }, "Enable or bypass culling and LOD while retaining the single graph submission owner");
    for (const std::string operation : {"attach", "detach", "range", "lod", "force"}) {
        (void)kernel.add_command("visibility." + operation,
            [operation](core::Kernel& host, core::CommandArgs args) { return edit_component(host, args, operation); },
            "Edit generation-checked scene visibility and LOD policy; invoke for usage");
    }
    (void)kernel.add_command("visibility.history.clear", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: visibility.history.clear"}; }
        host.services().get<VisibilityService>().reset_history();
        return {true, "LOD hysteresis history cleared"};
    }, "Reset retained per-camera LOD decisions after a discontinuous view change");
}

} // namespace ge::render
