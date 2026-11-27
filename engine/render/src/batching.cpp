#include <ge/render/batching.hpp>

#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace ge::render {
namespace {

bool between(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) { ++value; }
}

bool same_sampler(const SamplerState& first, const SamplerState& second) noexcept {
    return first.address_u == second.address_u && first.address_v == second.address_v &&
        first.filter == second.filter && first.border == second.border &&
        first.lod_bias == second.lod_bias && first.min_lod == second.min_lod &&
        first.max_lod == second.max_lod;
}

bool same_texture(const MaterialTexture& first, const MaterialTexture& second) noexcept {
    return first.texture == second.texture && same_sampler(first.sampler, second.sampler) &&
        first.scale == second.scale && first.offset == second.offset &&
        first.rotation == second.rotation && first.channel == second.channel;
}

bool same_material(const PbrMaterial& first, const PbrMaterial& second) noexcept {
    return first.albedo == second.albedo && first.emissive == second.emissive &&
        first.metalness == second.metalness && first.roughness == second.roughness &&
        first.dielectric_f0 == second.dielectric_f0 &&
        first.emissive_strength == second.emissive_strength &&
        first.normal_scale == second.normal_scale &&
        first.occlusion_strength == second.occlusion_strength &&
        first.alpha_cutoff == second.alpha_cutoff && first.alpha_mode == second.alpha_mode &&
        first.shading_model == second.shading_model && first.double_sided == second.double_sided &&
        first.use_vertex_color == second.use_vertex_color &&
        same_texture(first.albedo_texture, second.albedo_texture) &&
        same_texture(first.metalness_texture, second.metalness_texture) &&
        same_texture(first.roughness_texture, second.roughness_texture) &&
        same_texture(first.normal_texture, second.normal_texture) &&
        same_texture(first.emissive_texture, second.emissive_texture) &&
        same_texture(first.occlusion_texture, second.occlusion_texture);
}

bool same_appearance(const BatchAppearanceComponent& first,
                     const BatchAppearanceComponent& second) noexcept {
    return first.tint == second.tint && first.opacity == second.opacity &&
        first.emissive_scale == second.emissive_scale && first.roughness == second.roughness &&
        first.metalness == second.metalness && first.alpha_mode == second.alpha_mode;
}

bool material_change(const BatchAppearanceComponent& appearance) noexcept {
    return appearance.tint != Color::white() || appearance.opacity != 1.0 ||
        appearance.emissive_scale != 1.0 || appearance.roughness.has_value() ||
        appearance.metalness.has_value() || appearance.alpha_mode.has_value();
}

bool same_depth(const std::optional<DepthState>& first,
                const std::optional<DepthState>& second) noexcept {
    if (first.has_value() != second.has_value()) { return false; }
    return !first || (first->compare == second->compare &&
        first->test_enabled == second->test_enabled && first->write_enabled == second->write_enabled);
}

bool reorder_depth(const RasterDraw& draw, const RasterizerSettings& settings) noexcept {
    const auto depth = draw.depth.value_or(settings.depth);
    return depth.test_enabled && depth.write_enabled &&
        (depth.compare == DepthCompare::Less || depth.compare == DepthCompare::LessEqual ||
         depth.compare == DepthCompare::Greater || depth.compare == DepthCompare::GreaterEqual);
}

struct Coverage {
    double left{};
    double top{};
    double right{};
    double bottom{};
    bool known{};
};

Coverage coverage(const VisibilityEntry& entry, const VisibilityBuildContext& context) noexcept {
    if (!math::valid(entry.world_bounds) || context.view.camera.viewport.empty()) { return {}; }
    Coverage result{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), true};
    const auto& bounds = entry.world_bounds;
    const auto& viewport = context.view.camera.viewport;
    for (std::size_t corner = 0; corner < 8; ++corner) {
        const math::Vec4 world{(corner & 1U) != 0 ? bounds.max.x : bounds.min.x,
                               (corner & 2U) != 0 ? bounds.max.y : bounds.min.y,
                               (corner & 4U) != 0 ? bounds.max.z : bounds.min.z, 1.0};
        const auto clip = context.view.camera.view_projection * world;
        if (!math::finite(clip) || clip.w <= 1.0e-8) { return {}; }
        const double x = static_cast<double>(viewport.x) +
            (clip.x / clip.w * 0.5 + 0.5) * static_cast<double>(viewport.width);
        const double y = static_cast<double>(viewport.y) +
            (0.5 - clip.y / clip.w * 0.5) * static_cast<double>(viewport.height);
        if (!std::isfinite(x) || !std::isfinite(y)) { return {}; }
        result.left = std::min(result.left, x);
        result.right = std::max(result.right, x);
        result.top = std::min(result.top, y);
        result.bottom = std::max(result.bottom, y);
    }
    const auto fill = entry.draw.fill.value_or(context.raster_settings.fill);
    const double padding = fill == FillMode::Wireframe ? context.raster_settings.wire_width + 2.0 : 2.0;
    result.left -= padding;
    result.top -= padding;
    result.right += padding;
    result.bottom += padding;
    auto clip = viewport;
    if (context.raster_settings.scissor) { clip = intersection(clip, *context.raster_settings.scissor); }
    if (entry.draw.scissor) { clip = intersection(clip, *entry.draw.scissor); }
    result.left = std::max(result.left, static_cast<double>(clip.x));
    result.top = std::max(result.top, static_cast<double>(clip.y));
    result.right = std::min(result.right, static_cast<double>(clip.x) + static_cast<double>(clip.width));
    result.bottom = std::min(result.bottom, static_cast<double>(clip.y) + static_cast<double>(clip.height));
    return result;
}

bool disjoint(const Coverage& first, const Coverage& second) noexcept {
    if (!first.known || !second.known) { return false; }
    return first.left >= first.right || first.top >= first.bottom ||
        second.left >= second.right || second.top >= second.bottom ||
        first.right < second.left || second.right < first.left ||
        first.bottom < second.top || second.bottom < first.top;
}

struct PreparedInstance {
    RasterDraw draw{};
    BatchInstance information{};
    std::shared_ptr<const geometry::MeshData> mesh_owner;
    std::shared_ptr<const PbrMaterial> material_owner;
    Coverage screen{};
    std::uint64_t group_tag{};
    bool reorderable{};
    bool isolated{};
};

bool compatible(const PreparedInstance& first, const PreparedInstance& second) noexcept {
    const auto& lhs = first.draw;
    const auto& rhs = second.draw;
    return !first.isolated && !second.isolated && first.group_tag == second.group_tag &&
        lhs.mesh == rhs.mesh && lhs.material == rhs.material && lhs.first_index == rhs.first_index &&
        lhs.index_count == rhs.index_count && lhs.visibility_mask == rhs.visibility_mask &&
        lhs.light_mask == rhs.light_mask && lhs.scissor == rhs.scissor && lhs.cull == rhs.cull &&
        lhs.fill == rhs.fill && same_depth(lhs.depth, rhs.depth) &&
        first.information.mesh == second.information.mesh &&
        first.information.material == second.information.material &&
        first.information.material_slot == second.information.material_slot &&
        first.information.mesh_revision == second.information.mesh_revision &&
        first.information.material_revision == second.information.material_revision;
}

struct MeshCacheEntry {
    const geometry::MeshLibrary* library{};
    geometry::MeshHandle handle{};
    std::uint64_t revision{};
    std::size_t first{};
    std::size_t count{};
    std::uint32_t material_slot{};
    std::uint64_t used{};
    std::size_t charge{};
    std::shared_ptr<const geometry::MeshData> prepared;
};

struct MaterialCacheEntry {
    const MaterialService* service{};
    MaterialHandle handle{};
    std::uint64_t revision{};
    std::uint64_t used{};
    std::size_t charge{};
    PbrMaterial source{};
    BatchAppearanceComponent appearance{};
    std::shared_ptr<const PbrMaterial> prepared;
};

} // namespace

std::string_view describe(BatchError error) noexcept {
    switch (error) {
    case BatchError::None: return "none";
    case BatchError::InvalidLimits: return "invalid batching limits";
    case BatchError::InvalidSettings: return "invalid batching settings";
    case BatchError::InvalidAppearance: return "invalid authored batch appearance";
    case BatchError::InvalidEntity: return "entity is stale or absent from the scene";
    case BatchError::InvalidDraw: return "selected draw is invalid";
    case BatchError::StaleSelection: return "selected mesh or material generation/revision is stale";
    case BatchError::InstanceLimit: return "instance submission limit reached";
    case BatchError::GroupLimit: return "instance group limit reached";
    case BatchError::PreparationLimit: return "required material preparation budget exhausted";
    case BatchError::AllocationFailure: return "batch preparation allocation failed";
    case BatchError::BuilderConflict: return "another owner holds the visibility builder";
    case BatchError::MissingVisibility: return "visibility service is not registered";
    case BatchError::Busy: return "batch builder is already preparing a view";
    case BatchError::Stopped: return "batch builder has shut down";
    }
    return "unknown batching error";
}

bool valid(const BatchingLimits& limits) noexcept {
    return limits.instances_per_view > 0 && limits.instances_per_view <= 65536 &&
        limits.groups_per_view > 0 && limits.groups_per_view <= limits.instances_per_view &&
        limits.instances_per_group > 0 && limits.instances_per_group <= limits.instances_per_view &&
        limits.mesh_cache_entries <= 4096 && limits.material_cache_entries <= 65536 &&
        limits.cache_bytes <= 512U * 1024U * 1024U &&
        limits.preparation_indices <= 6'291'456 && limits.preparation_vertices <= 1'048'576 &&
        limits.preparation_bytes <= 256U * 1024U * 1024U &&
        limits.reorder_window <= 4096 && limits.reorder_comparisons <= 16'777'216 &&
        limits.cache_idle_builds <= 1'000'000;
}

bool valid(const BatchingSettings& settings) noexcept {
    return settings.order == BatchOrder::Preserve || settings.order == BatchOrder::DisjointOpaque;
}

bool valid(const BatchAppearanceComponent& appearance) noexcept {
    return between(appearance.tint.r, 0.0, 1.0) && between(appearance.tint.g, 0.0, 1.0) &&
        between(appearance.tint.b, 0.0, 1.0) && between(appearance.tint.a, 0.0, 1.0) &&
        between(appearance.opacity, 0.0, 1.0) && between(appearance.emissive_scale, 0.0, 1.0e6) &&
        (!appearance.roughness || between(*appearance.roughness, 0.0, 1.0)) &&
        (!appearance.metalness || between(*appearance.metalness, 0.0, 1.0)) &&
        (!appearance.alpha_mode || *appearance.alpha_mode == AlphaMode::Opaque ||
         *appearance.alpha_mode == AlphaMode::Mask || *appearance.alpha_mode == AlphaMode::Blend);
}

struct BatchingService::Impl final : VisibilitySubmissionBuilder {
    BatchingLimits limits;
    BatchingSettings settings;
    BatchingStatistics statistics;
    std::vector<BatchInstance> instances;
    std::vector<BatchGroup> groups;
    std::vector<MeshCacheEntry> mesh_cache;
    std::vector<MaterialCacheEntry> material_cache;
    std::size_t cache_bytes{};
    std::uint64_t epoch{};
    bool busy{};
    bool stopped{};

    explicit Impl(BatchingLimits configuration) : limits(configuration) {
        if (!valid(limits)) { throw std::invalid_argument("invalid batching limits"); }
    }

    VisibilityBuildResult fail(BatchError error) noexcept {
        statistics.error = error;
        return {false, false, false};
    }

    void reset_statistics(const VisibilityBuildContext& context) noexcept {
        const auto builds = statistics.builds;
        const auto successes = statistics.successful_builds;
        const bool installed = statistics.installed;
        statistics = {};
        statistics.builds = builds;
        statistics.successful_builds = successes;
        statistics.installed = installed;
        increment(statistics.builds);
        statistics.camera = context.view.camera.entity;
        statistics.view_slot = context.view.camera.slot;
        statistics.selected = context.view.entries.size();
        if (epoch == std::numeric_limits<std::uint64_t>::max()) { clear_cache(); epoch = 0; }
        ++epoch;
    }

    void clear_cache() noexcept {
        mesh_cache.clear();
        material_cache.clear();
        cache_bytes = 0;
    }

    void prune(const VisibilityBuildContext& context) noexcept {
        std::erase_if(mesh_cache, [&](const MeshCacheEntry& entry) {
            const auto info = entry.library == &context.meshes ? context.meshes.inspect(entry.handle) : std::nullopt;
            const bool stale = !info || info->revision != entry.revision;
            const bool expired = epoch - entry.used > limits.cache_idle_builds;
            if (!stale && !expired) { return false; }
            cache_bytes -= entry.charge;
            ++statistics.cache_evictions;
            return true;
        });
        std::erase_if(material_cache, [&](const MaterialCacheEntry& entry) {
            bool stale = entry.service != &context.materials;
            if (!stale && entry.handle) {
                const auto info = context.materials.materials().inspect(entry.handle);
                stale = !info || info->revision != entry.revision || !same_material(entry.source, *info->material);
            } else if (!stale) {
                stale = !same_material(entry.source, context.materials.fallback());
            }
            const bool expired = epoch - entry.used > limits.cache_idle_builds;
            if (!stale && !expired) { return false; }
            cache_bytes -= entry.charge;
            ++statistics.cache_evictions;
            return true;
        });
    }

    bool evict_oldest(bool meshes_only, bool materials_only) noexcept {
        std::size_t mesh_slot = mesh_cache.size();
        std::size_t material_slot = material_cache.size();
        auto oldest = std::numeric_limits<std::uint64_t>::max();
        if (!materials_only) {
            for (std::size_t index = 0; index < mesh_cache.size(); ++index) {
                if (mesh_cache[index].prepared.use_count() == 1 && mesh_cache[index].used <= oldest) {
                    oldest = mesh_cache[index].used;
                    mesh_slot = index;
                }
            }
        }
        if (!meshes_only) {
            for (std::size_t index = 0; index < material_cache.size(); ++index) {
                if (material_cache[index].prepared.use_count() == 1 && material_cache[index].used <= oldest) {
                    oldest = material_cache[index].used;
                    material_slot = index;
                    mesh_slot = mesh_cache.size();
                }
            }
        }
        if (mesh_slot < mesh_cache.size()) {
            cache_bytes -= mesh_cache[mesh_slot].charge;
            mesh_cache.erase(mesh_cache.begin() + static_cast<std::ptrdiff_t>(mesh_slot));
        } else if (material_slot < material_cache.size()) {
            cache_bytes -= material_cache[material_slot].charge;
            material_cache.erase(material_cache.begin() + static_cast<std::ptrdiff_t>(material_slot));
        } else { return false; }
        ++statistics.cache_evictions;
        return true;
    }

    bool cache_room(std::size_t bytes, bool mesh) noexcept {
        if (!settings.retain_prepared_resources || bytes > limits.cache_bytes) { return false; }
        const auto capacity = mesh ? limits.mesh_cache_entries : limits.material_cache_entries;
        if (capacity == 0) { return false; }
        while ((mesh ? mesh_cache.size() : material_cache.size()) >= capacity) {
            if (!evict_oldest(mesh, !mesh)) { return false; }
        }
        while (bytes > limits.cache_bytes - cache_bytes) {
            if (!evict_oldest(false, false)) { return false; }
        }
        return true;
    }

    BatchError check(const VisibilityBuildContext& context, const VisibilityEntry& entry,
                     PreparedInstance& destination) const noexcept {
        if (!context.scene.contains(entry.draw.entity) || !context.scene.registry().alive(entry.draw.entity)) {
            return BatchError::InvalidEntity;
        }
        const auto* source = context.meshes.get(entry.mesh);
        const auto info = context.meshes.inspect(entry.mesh);
        if (!source || !info || source != entry.draw.mesh || info->revision != entry.mesh_revision) {
            return BatchError::StaleSelection;
        }
        if (!math::finite(entry.draw.model) || entry.draw.first_index > source->indices.size() ||
            entry.draw.first_index % 3 != 0) { return BatchError::InvalidDraw; }
        const auto available = source->indices.size() - entry.draw.first_index;
        const auto count = entry.draw.index_count == 0 ? available : entry.draw.index_count;
        if (count > available || count % 3 != 0) { return BatchError::InvalidDraw; }
        const auto material = context.materials.materials().inspect(entry.material);
        const auto* resolved = material ? material->material : &context.materials.fallback();
        if ((entry.material && !material) || entry.draw.material != resolved) { return BatchError::StaleSelection; }
        destination.draw = entry.draw;
        destination.draw.index_count = count;
        auto& result = destination.information;
        result.entity = entry.draw.entity;
        result.mesh = entry.mesh;
        result.material = entry.material;
        result.mesh_revision = entry.mesh_revision;
        result.material_revision = material ? material->revision : 0;
        result.model = entry.draw.model;
        result.world_bounds = entry.world_bounds;
        result.selected_ordinal = entry.ordinal;
        result.lod = entry.lod;
        result.material_slot = entry.material_slot;
        return BatchError::None;
    }

    BatchError prepare_material(const VisibilityBuildContext& context,
                                const BatchAppearanceComponent& appearance,
                                PreparedInstance& instance) {
        if (!material_change(appearance)) { return BatchError::None; }
        const auto& source = *instance.draw.material;
        for (auto& cached : material_cache) {
            if (cached.service == &context.materials && cached.handle == instance.information.material &&
                cached.revision == instance.information.material_revision &&
                same_appearance(cached.appearance, appearance) &&
                same_material(cached.source, source)) {
                cached.used = epoch;
                instance.material_owner = cached.prepared;
                instance.draw.material = cached.prepared.get();
                instance.information.appearance_applied = true;
                ++statistics.material_cache_hits;
                return BatchError::None;
            }
        }
        if (sizeof(PbrMaterial) > limits.preparation_bytes - statistics.prepared_bytes) {
            return BatchError::PreparationLimit;
        }
        auto prepared = std::make_shared<PbrMaterial>(source);
        prepared->albedo = source.albedo * appearance.tint;
        prepared->albedo.a *= appearance.opacity;
        prepared->emissive.r *= appearance.emissive_scale;
        prepared->emissive.g *= appearance.emissive_scale;
        prepared->emissive.b *= appearance.emissive_scale;
        if (appearance.roughness) { prepared->roughness = *appearance.roughness; }
        if (appearance.metalness) { prepared->metalness = *appearance.metalness; }
        if (appearance.alpha_mode) { prepared->alpha_mode = *appearance.alpha_mode; }
        else if (appearance.opacity < 1.0 || appearance.tint.a < 1.0) { prepared->alpha_mode = AlphaMode::Blend; }
        if (!validate_material(*prepared)) { return BatchError::InvalidAppearance; }
        statistics.prepared_bytes += sizeof(PbrMaterial);
        instance.material_owner = prepared;
        instance.draw.material = prepared.get();
        instance.information.appearance_applied = true;
        const auto charge = sizeof(MaterialCacheEntry) + sizeof(PbrMaterial);
        if (cache_room(charge, false)) {
            material_cache.push_back({&context.materials, instance.information.material,
                instance.information.material_revision, epoch, charge, source, appearance, std::move(prepared)});
            cache_bytes += charge;
        }
        return BatchError::None;
    }

    BatchError prepare_geometry(const VisibilityBuildContext& context, PreparedInstance& instance) {
        const auto& source = *instance.draw.mesh;
        const auto first = instance.draw.first_index;
        const auto count = instance.draw.index_count;
        if (!settings.prepare_submesh_ranges || count == 0 || (first == 0 && count == source.indices.size())) {
            return BatchError::None;
        }
        for (auto& cached : mesh_cache) {
            if (cached.library == &context.meshes && cached.handle == instance.information.mesh &&
                cached.revision == instance.information.mesh_revision && cached.first == first && cached.count == count &&
                cached.material_slot == instance.information.material_slot) {
                cached.used = epoch;
                instance.mesh_owner = cached.prepared;
                instance.draw.mesh = cached.prepared.get();
                instance.draw.first_index = 0;
                instance.information.geometry_prepared = true;
                ++statistics.mesh_cache_hits;
                return BatchError::None;
            }
        }
        const auto maximum_vertices = std::min(source.vertices.size(), count);
        const auto estimated = maximum_vertices * sizeof(geometry::Vertex) +
            count * sizeof(geometry::MeshIndex) * 2 + sizeof(geometry::SubmeshRange) + sizeof(geometry::MeshData);
        if (count > limits.preparation_indices - statistics.prepared_indices ||
            maximum_vertices > limits.preparation_vertices - statistics.prepared_vertices ||
            estimated > limits.preparation_bytes - statistics.prepared_bytes) {
            ++statistics.preparation_fallbacks;
            return BatchError::None;
        }
        std::vector<geometry::MeshIndex> unique;
        unique.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto vertex = source.indices[first + index];
            if (static_cast<std::size_t>(vertex) >= source.vertices.size()) { return BatchError::InvalidDraw; }
            unique.push_back(vertex);
        }
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        auto prepared = std::make_shared<geometry::MeshData>();
        prepared->vertices.reserve(unique.size());
        prepared->indices.reserve(count);
        prepared->submeshes.reserve(1);
        for (const auto vertex : unique) { prepared->vertices.push_back(source.vertices[vertex]); }
        for (std::size_t index = 0; index < count; ++index) {
            const auto source_index = source.indices[first + index];
            const auto found = std::lower_bound(unique.begin(), unique.end(), source_index);
            const auto offset = static_cast<std::size_t>(found - unique.begin());
            if (found == unique.end() || *found != source_index || offset >= geometry::invalid_mesh_index) {
                return BatchError::InvalidDraw;
            }
            prepared->indices.push_back(static_cast<geometry::MeshIndex>(offset));
        }
        prepared->submeshes.push_back({0, static_cast<geometry::MeshIndex>(count), instance.information.material_slot});
        const auto bytes = prepared->allocated_bytes() + sizeof(geometry::MeshData) +
            unique.capacity() * sizeof(geometry::MeshIndex);
        if (bytes > limits.preparation_bytes - statistics.prepared_bytes) {
            ++statistics.preparation_fallbacks;
            return BatchError::None;
        }
        statistics.prepared_bytes += bytes;
        statistics.prepared_indices += count;
        statistics.prepared_vertices += unique.size();
        instance.mesh_owner = prepared;
        instance.draw.mesh = prepared.get();
        instance.draw.first_index = 0;
        instance.information.geometry_prepared = true;
        const auto charge = prepared->allocated_bytes() + sizeof(geometry::MeshData) + sizeof(MeshCacheEntry);
        if (cache_room(charge, true)) {
            mesh_cache.push_back({&context.meshes, instance.information.mesh,
                instance.information.mesh_revision, first, count, instance.information.material_slot,
                epoch, charge, std::move(prepared)});
            cache_bytes += charge;
        }
        return BatchError::None;
    }

    BatchError prepare(const VisibilityBuildContext& context, const VisibilityEntry& entry,
                       PreparedInstance& instance) {
        const auto checked = check(context, entry, instance);
        if (checked != BatchError::None) { return checked; }
        const auto triangles = instance.draw.index_count / 3;
        if (triangles > context.raster_limits.max_input_triangles - statistics.input_triangles) {
            return BatchError::InstanceLimit;
        }
        const auto* appearance = context.scene.registry().get<BatchAppearanceComponent>(entry.draw.entity);
        if (settings.enabled && settings.appearance_enabled && appearance && appearance->enabled &&
            (!appearance->material_slot || *appearance->material_slot == entry.material_slot)) {
            if (!valid(*appearance)) { return BatchError::InvalidAppearance; }
            const auto adjusted = prepare_material(context, *appearance, instance);
            if (adjusted != BatchError::None) { return adjusted; }
            instance.group_tag = appearance->group_tag;
            instance.isolated = appearance->isolate;
        } else { appearance = nullptr; }
        if (settings.enabled) {
            const auto geometry = prepare_geometry(context, instance);
            if (geometry != BatchError::None) { return geometry; }
        }
        instance.information.transparent = instance.draw.material->alpha_mode == AlphaMode::Blend;
        instance.screen = coverage(entry, context);
        instance.reorderable = settings.enabled && settings.order == BatchOrder::DisjointOpaque &&
            context.raster_settings.sort_transparency && !instance.information.transparent &&
            !instance.isolated && (!appearance || !appearance->preserve_order) &&
            reorder_depth(instance.draw, context.raster_settings) && instance.screen.known;
        statistics.input_triangles += triangles;
        statistics.appearance_instances += instance.information.appearance_applied ? 1U : 0U;
        statistics.transparent_instances += instance.information.transparent ? 1U : 0U;
        statistics.ordered_instances += instance.reorderable ? 0U : 1U;
        return BatchError::None;
    }

    bool compare_budget() noexcept {
        if (statistics.reorder_comparisons >= limits.reorder_comparisons) {
            statistics.order_work_limited = true;
            return false;
        }
        ++statistics.reorder_comparisons;
        return true;
    }

    std::size_t group_end(std::vector<std::size_t>& order, std::size_t begin,
                          const std::vector<PreparedInstance>& prepared) noexcept {
        const auto& first = prepared[order[begin]];
        std::size_t end = begin + 1;
        if (!settings.enabled || first.isolated) { return end; }
        while (end < order.size() && end - begin < limits.instances_per_group) {
            if (compatible(first, prepared[order[end]])) { ++end; continue; }
            if (!first.reorderable || limits.reorder_window == 0 || statistics.order_work_limited) { break; }
            const auto stop = end + std::min(limits.reorder_window, order.size() - end);
            bool moved{};
            for (std::size_t candidate = end + 1; candidate < stop; ++candidate) {
                if (!compare_budget()) { break; }
                const auto& selected = prepared[order[candidate]];
                if (!selected.reorderable || !compatible(first, selected)) { continue; }
                bool safe = true;
                for (std::size_t crossed = end; crossed < candidate; ++crossed) {
                    if (!compare_budget() || !prepared[order[crossed]].reorderable ||
                        !disjoint(selected.screen, prepared[order[crossed]].screen)) { safe = false; break; }
                }
                if (!safe) { continue; }
                const auto retained = order[candidate];
                for (std::size_t shifted = candidate; shifted > end; --shifted) { order[shifted] = order[shifted - 1]; }
                order[end] = retained;
                ++statistics.reordered_instances;
                moved = true;
                break;
            }
            if (!moved) { break; }
            ++end;
        }
        return end;
    }

    VisibilityBuildResult build(const VisibilityBuildContext& context, VisibilitySubmission& output) override {
        if (busy) { return {false, false, false}; }
        if (stopped) { return fail(BatchError::Stopped); }
        busy = true;
        struct Guard { bool& active; ~Guard() { active = false; } } guard{busy};
        reset_statistics(context);
        const auto maximum = std::min({limits.instances_per_view, context.maximum_draws, context.raster_limits.max_draws});
        if (context.view.entries.size() > maximum) { return fail(BatchError::InstanceLimit); }
        try {
            prune(context);
            std::vector<PreparedInstance> prepared;
            prepared.reserve(context.view.entries.size());
            for (const auto& entry : context.view.entries) {
                PreparedInstance instance;
                const auto error = prepare(context, entry, instance);
                if (error != BatchError::None) { return fail(error); }
                prepared.push_back(std::move(instance));
            }
            std::vector<std::size_t> order(prepared.size());
            std::iota(order.begin(), order.end(), std::size_t{});
            VisibilitySubmission staged;
            staged.draws.reserve(prepared.size());
            staged.owned_meshes.reserve(prepared.size());
            staged.owned_materials.reserve(prepared.size());
            std::vector<BatchInstance> next_instances;
            std::vector<BatchGroup> next_groups;
            next_instances.reserve(prepared.size());
            next_groups.reserve(std::min(prepared.size(), limits.groups_per_view));
            std::size_t begin{};
            while (begin < order.size()) {
                if (next_groups.size() >= limits.groups_per_view) { return fail(BatchError::GroupLimit); }
                const auto end = group_end(order, begin, prepared);
                const auto& seed = prepared[order[begin]];
                BatchGroup group;
                group.mesh = seed.information.mesh;
                group.material = seed.information.material;
                group.mesh_revision = seed.information.mesh_revision;
                group.material_revision = seed.information.material_revision;
                group.group_tag = seed.group_tag;
                group.visibility_mask = seed.draw.visibility_mask;
                group.light_mask = seed.draw.light_mask;
                group.first_instance = next_instances.size();
                group.instance_count = end - begin;
                group.first_index = seed.draw.first_index;
                group.index_count = seed.draw.index_count;
                group.transparent = seed.information.transparent;
                group.geometry_prepared = seed.information.geometry_prepared;
                for (std::size_t index = begin; index < end; ++index) {
                    auto& instance = prepared[order[index]];
                    instance.information.group = next_groups.size();
                    instance.information.output_draw = staged.draws.size();
                    group.ordered = group.ordered || !instance.reorderable;
                    group.input_triangles += instance.draw.index_count / 3;
                    if (instance.mesh_owner) { staged.owned_meshes.push_back(instance.mesh_owner); }
                    if (instance.material_owner) { staged.owned_materials.push_back(instance.material_owner); }
                    staged.draws.push_back(instance.draw);
                    next_instances.push_back(instance.information);
                }
                next_groups.push_back(group);
                begin = end;
            }
            output = std::move(staged);
            instances.swap(next_instances);
            groups.swap(next_groups);
            statistics.emitted = instances.size();
            statistics.groups = groups.size();
            statistics.shared_instances = instances.size() - groups.size();
            increment(statistics.successful_builds);
            statistics.error = BatchError::None;
            return {true, context.view.complete, false};
        } catch (const std::bad_alloc&) { return fail(BatchError::AllocationFailure); }
        catch (const std::length_error&) { return fail(BatchError::AllocationFailure); }
    }
};

BatchingService::BatchingService(BatchingLimits limits) : impl_(std::make_shared<Impl>(limits)) {}
BatchingService::~BatchingService() = default;

bool BatchingService::configure(const BatchingSettings& settings) noexcept {
    if (impl_->busy || impl_->stopped) { impl_->statistics.error = impl_->stopped ? BatchError::Stopped : BatchError::Busy; return false; }
    if (!valid(settings)) { impl_->statistics.error = BatchError::InvalidSettings; return false; }
    impl_->settings = settings;
    if (!settings.retain_prepared_resources) { impl_->clear_cache(); }
    impl_->statistics.error = BatchError::None;
    return true;
}

const BatchingSettings& BatchingService::settings() const noexcept { return impl_->settings; }
const BatchingLimits& BatchingService::limits() const noexcept { return impl_->limits; }
const BatchingStatistics& BatchingService::statistics() const noexcept { return impl_->statistics; }
std::span<const BatchInstance> BatchingService::instances() const noexcept { return impl_->instances; }
std::span<const BatchGroup> BatchingService::groups() const noexcept { return impl_->groups; }

BatchCacheStatistics BatchingService::cache_statistics() const noexcept {
    BatchCacheStatistics result{impl_->mesh_cache.size(), impl_->material_cache.size(), impl_->cache_bytes, 0};
    for (const auto& entry : impl_->mesh_cache) { result.externally_retained += entry.prepared.use_count() > 1 ? 1U : 0U; }
    for (const auto& entry : impl_->material_cache) { result.externally_retained += entry.prepared.use_count() > 1 ? 1U : 0U; }
    return result;
}

std::span<const BatchInstance> BatchingService::group_instances(std::size_t group) const noexcept {
    if (group >= impl_->groups.size()) { return {}; }
    const auto& selected = impl_->groups[group];
    return std::span<const BatchInstance>(impl_->instances).subspan(selected.first_instance, selected.instance_count);
}

std::shared_ptr<VisibilitySubmissionBuilder> BatchingService::submission_builder() const noexcept { return impl_; }
VisibilityBuildResult BatchingService::build(const VisibilityBuildContext& context, VisibilitySubmission& output) {
    return impl_->build(context, output);
}

bool BatchingService::install(VisibilityService& visibility) {
    if (impl_->busy || impl_->stopped) {
        impl_->statistics.error = impl_->stopped ? BatchError::Stopped : BatchError::Busy;
        return false;
    }
    if (visibility.submission_builder_owner() == batching_builder_owner) { impl_->statistics.installed = true; return true; }
    try {
        if (!visibility.set_submission_builder(batching_builder_owner, impl_)) {
            impl_->statistics.installed = false;
            impl_->statistics.error = BatchError::BuilderConflict;
            return false;
        }
    } catch (const std::bad_alloc&) { impl_->statistics.error = BatchError::AllocationFailure; return false; }
    catch (const std::length_error&) { impl_->statistics.error = BatchError::AllocationFailure; return false; }
    impl_->statistics.installed = true;
    impl_->statistics.error = BatchError::None;
    return true;
}

bool BatchingService::uninstall(VisibilityService& visibility) noexcept {
    if (impl_->busy) { return false; }
    if (visibility.submission_builder_owner() != batching_builder_owner) { impl_->statistics.installed = false; return false; }
    const bool released = visibility.clear_submission_builder(batching_builder_owner);
    if (released) { impl_->statistics.installed = false; }
    return released;
}

void BatchingService::shutdown(VisibilityService* visibility) noexcept {
    if (impl_->busy) { return; }
    if (visibility) { (void)uninstall(*visibility); }
    impl_->stopped = true;
    impl_->statistics.installed = false;
    impl_->statistics.error = BatchError::Stopped;
    impl_->clear_cache();
    impl_->instances.clear();
    impl_->groups.clear();
}

void BatchingService::clear_cache() noexcept {
    if (!impl_->busy) { impl_->clear_cache(); }
}

bool BatchingService::invalidate(geometry::MeshHandle mesh) noexcept {
    if (impl_->busy) { return false; }
    const auto removed = std::erase_if(impl_->mesh_cache, [&](const MeshCacheEntry& entry) {
        if (entry.handle != mesh) { return false; }
        impl_->cache_bytes -= entry.charge;
        return true;
    });
    return removed != 0;
}

bool BatchingService::invalidate(MaterialHandle material) noexcept {
    if (impl_->busy) { return false; }
    const auto removed = std::erase_if(impl_->material_cache, [&](const MaterialCacheEntry& entry) {
        if (entry.handle != material) { return false; }
        impl_->cache_bytes -= entry.charge;
        return true;
    });
    return removed != 0;
}

bool BatchingService::set_appearance(scene::Scene& scene, ge::RuntimeHandle entity,
                                     const BatchAppearanceComponent& appearance) {
    if (impl_->busy || impl_->stopped) { return false; }
    if (!scene.contains(entity) || !scene.registry().alive(entity)) { impl_->statistics.error = BatchError::InvalidEntity; return false; }
    if (!valid(appearance)) { impl_->statistics.error = BatchError::InvalidAppearance; return false; }
    try {
        (void)scene.registry().emplace_or_replace<BatchAppearanceComponent>(entity, appearance);
        impl_->statistics.error = BatchError::None;
        return true;
    } catch (const std::bad_alloc&) { impl_->statistics.error = BatchError::AllocationFailure; }
    catch (const std::length_error&) { impl_->statistics.error = BatchError::AllocationFailure; }
    catch (const std::logic_error&) { impl_->statistics.error = BatchError::Busy; }
    return false;
}

bool BatchingService::remove_appearance(scene::Scene& scene, ge::RuntimeHandle entity) {
    if (impl_->busy || impl_->stopped || !scene.contains(entity)) { return false; }
    try { return scene.registry().remove<BatchAppearanceComponent>(entity); }
    catch (const std::bad_alloc&) { impl_->statistics.error = BatchError::AllocationFailure; return false; }
    catch (const std::length_error&) { impl_->statistics.error = BatchError::AllocationFailure; return false; }
    catch (const std::logic_error&) { impl_->statistics.error = BatchError::Busy; return false; }
}

const BatchAppearanceComponent* BatchingService::appearance(const scene::Scene& scene,
                                                            ge::RuntimeHandle entity) const noexcept {
    return scene.contains(entity) ? scene.registry().get<BatchAppearanceComponent>(entity) : nullptr;
}

namespace {

template<class Number>
std::optional<Number> number(std::string_view text) noexcept {
    Number result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) { return std::nullopt; }
    return result;
}

std::optional<bool> flag(std::string_view text) noexcept {
    if (text == "on" || text == "true" || text == "1") { return true; }
    if (text == "off" || text == "false" || text == "0") { return false; }
    return std::nullopt;
}

ge::RuntimeHandle resolve(const scene::Scene& scene, std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos) { return scene.registry().find_name(text); }
    const auto index = number<std::uint32_t>(text.substr(0, colon));
    const auto generation = number<std::uint64_t>(text.substr(colon + 1));
    return index && generation ? ge::RuntimeHandle{*index, *generation} : ge::RuntimeHandle{};
}

std::string stats_text(const BatchingService& service) {
    const auto& stats = service.statistics();
    const auto cache = service.cache_statistics();
    std::ostringstream result;
    result << "builds=" << stats.builds << " successful=" << stats.successful_builds
        << " view=" << stats.view_slot << " selected=" << stats.selected << " emitted=" << stats.emitted
        << " groups=" << stats.groups << " grouped_instances=" << stats.shared_instances
        << " ordered=" << stats.ordered_instances << " transparent=" << stats.transparent_instances
        << " appearance=" << stats.appearance_instances << " triangles=" << stats.input_triangles
        << " reordered=" << stats.reordered_instances << " comparisons=" << stats.reorder_comparisons
        << " reorder_limited=" << stats.order_work_limited << " mesh_hits=" << stats.mesh_cache_hits
        << " material_hits=" << stats.material_cache_hits << " cache_meshes=" << cache.meshes
        << " cache_materials=" << cache.materials << " cache_bytes=" << cache.bytes
        << " pinned_entries=" << cache.externally_retained << " evictions=" << stats.cache_evictions
        << " prepared_bytes=" << stats.prepared_bytes << " preparation_fallbacks=" << stats.preparation_fallbacks
        << " installed=" << stats.installed << " error=" << describe(stats.error);
    return result.str();
}

core::CommandResult appearance_command(core::Kernel& host, core::CommandArgs args) {
    if (args.size() < 2) { return {false, "usage: batch.appearance inspect|clear|tint|opacity|emissive|roughness|metalness|alpha|order|isolate|enabled|slot|tag entity [value ...]"}; }
    auto* scene = host.services().find<scene::Scene>();
    if (!scene) { return {false, "Scene service required"}; }
    auto& service = host.services().get<BatchingService>();
    const auto entity = resolve(*scene, args[1]);
    if (!scene->contains(entity)) { return {false, "entity name or index:generation is not present in Scene"}; }
    const auto* existing = service.appearance(*scene, entity);
    auto edited = existing ? *existing : BatchAppearanceComponent{};
    const auto& operation = args[0];
    if (operation == "inspect" && args.size() == 2) {
        std::ostringstream result;
        result << "attached=" << (existing != nullptr) << " enabled=" << edited.enabled
            << " tint=" << edited.tint.r << ',' << edited.tint.g << ',' << edited.tint.b << ',' << edited.tint.a
            << " opacity=" << edited.opacity << " emissive=" << edited.emissive_scale
            << " preserve_order=" << edited.preserve_order << " isolate=" << edited.isolate << " tag=" << edited.group_tag;
        if (edited.roughness) { result << " roughness=" << *edited.roughness; }
        if (edited.metalness) { result << " metalness=" << *edited.metalness; }
        if (edited.material_slot) { result << " slot=" << *edited.material_slot; }
        if (edited.alpha_mode) { result << " alpha=" << static_cast<unsigned>(*edited.alpha_mode); }
        return {true, result.str()};
    }
    if (operation == "clear" && args.size() == 2) {
        return {service.remove_appearance(*scene, entity), "authored appearance removal requested"};
    }
    if (operation == "tint" && (args.size() == 5 || args.size() == 6)) {
        const auto red = number<double>(args[2]);
        const auto green = number<double>(args[3]);
        const auto blue = number<double>(args[4]);
        const auto alpha = args.size() == 6 ? number<double>(args[5]) : std::optional<double>{1.0};
        if (!red || !green || !blue || !alpha) { return {false, "expected finite tint RGB[A] components in [0,1]"}; }
        edited.tint = {*red, *green, *blue, *alpha};
    } else if (args.size() != 3) {
        return {false, "this appearance operation requires exactly one value"};
    } else if (operation == "opacity" || operation == "emissive") {
        const auto value = number<double>(args[2]);
        if (!value) { return {false, "expected a finite numeric value"}; }
        if (operation == "opacity") { edited.opacity = *value; }
        else { edited.emissive_scale = *value; }
    } else if (operation == "roughness" || operation == "metalness") {
        auto& value = operation == "roughness" ? edited.roughness : edited.metalness;
        if (args[2] == "inherit") { value.reset(); }
        else {
            value = number<double>(args[2]);
            if (!value) { return {false, "expected inherit or a value in [0,1]"}; }
        }
    } else if (operation == "alpha") {
        if (args[2] == "inherit") { edited.alpha_mode.reset(); }
        else if (args[2] == "opaque") { edited.alpha_mode = AlphaMode::Opaque; }
        else if (args[2] == "mask") { edited.alpha_mode = AlphaMode::Mask; }
        else if (args[2] == "blend") { edited.alpha_mode = AlphaMode::Blend; }
        else { return {false, "expected inherit, opaque, mask or blend"}; }
    } else if (operation == "order" || operation == "isolate" || operation == "enabled") {
        const auto value = flag(args[2]);
        if (!value) { return {false, "expected on or off"}; }
        if (operation == "order") { edited.preserve_order = *value; }
        else if (operation == "isolate") { edited.isolate = *value; }
        else { edited.enabled = *value; }
    } else if (operation == "slot") {
        if (args[2] == "all") { edited.material_slot.reset(); }
        else {
            edited.material_slot = number<std::uint32_t>(args[2]);
            if (!edited.material_slot) { return {false, "expected all or a material slot index"}; }
        }
    } else if (operation == "tag") {
        const auto value = number<std::uint64_t>(args[2]);
        if (!value) { return {false, "expected an unsigned grouping tag"}; }
        edited.group_tag = *value;
    } else { return {false, "unknown appearance operation"}; }
    const bool changed = service.set_appearance(*scene, entity, edited);
    return {changed, changed ? "authored appearance updated" : std::string(describe(service.statistics().error))};
}

core::CommandResult group_command(core::Kernel& host, core::CommandArgs args) {
    auto& service = host.services().get<BatchingService>();
    if (args.size() > 1) { return {false, "usage: batch.groups [group_index]"}; }
    std::ostringstream result;
    if (args.empty()) {
        const auto groups = service.groups();
        const auto count = std::min(std::size_t{128}, groups.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& group = groups[index];
            result << index << ": mesh=" << group.mesh.index << ':' << group.mesh.generation
                << " material=" << group.material.index << ':' << group.material.generation
                << " first=" << group.first_instance << " instances=" << group.instance_count
                << " triangles=" << group.input_triangles << " ordered=" << group.ordered
                << " transparent=" << group.transparent << '\n';
        }
        if (count < groups.size()) { result << "remaining_groups=" << groups.size() - count; }
        return {true, result.str()};
    }
    const auto group = number<std::size_t>(args[0]);
    if (!group || *group >= service.groups().size()) { return {false, "group index outside the last successful view"}; }
    const auto instances = service.group_instances(*group);
    const auto count = std::min(std::size_t{256}, instances.size());
    for (const auto& instance : instances.first(count)) {
        result << "entity=" << instance.entity.index << ':' << instance.entity.generation
            << " draw=" << instance.output_draw << " selected=" << instance.selected_ordinal
            << " lod=" << instance.lod << " slot=" << instance.material_slot
            << " mesh_revision=" << instance.mesh_revision << " material_revision=" << instance.material_revision
            << " prepared=" << instance.geometry_prepared << " appearance=" << instance.appearance_applied << '\n';
    }
    if (count < instances.size()) { result << "remaining_instances=" << instances.size() - count; }
    return {true, result.str()};
}

} // namespace

void register_batching(core::Kernel& kernel) {
    if (!kernel.register_module({"render.batching", "Bounded visibility instance grouping and immutable appearance preparation", 1})) { return; }
    auto& service = kernel.services().emplace<BatchingService>();
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        (void)registry->register_component<BatchAppearanceComponent>("render.batch_appearance");
    }
    auto settings = service.settings();
    settings.enabled = kernel.config().boolean("render.batching.enabled", true);
    settings.appearance_enabled = kernel.config().boolean("render.batching.appearance", true);
    settings.prepare_submesh_ranges = kernel.config().boolean("render.batching.prepare", true);
    settings.retain_prepared_resources = kernel.config().boolean("render.batching.retain", true);
    settings.order = kernel.config().boolean("render.batching.reorder", true) ? BatchOrder::DisjointOpaque : BatchOrder::Preserve;
    (void)service.configure(settings);
    (void)kernel.add_system({"render.batching.install", core::Phase::Render, -65, {"render.visibility.install"}},
        [reported = BatchError::None](core::Kernel& host, const core::Frame&) mutable {
            auto& current = host.services().get<BatchingService>();
            auto* visibility = host.services().find<VisibilityService>();
            const bool installed = visibility && current.install(*visibility);
            const auto error = installed ? BatchError::None : visibility ? current.statistics().error : BatchError::MissingVisibility;
            if (error != BatchError::None && error != reported) {
                host.diagnose(core::Diagnostic::Severity::Warning, "render.batching", std::string(describe(error)));
            }
            reported = error;
        });
    (void)kernel.order_after("render.graph.frame", "render.batching.install");
    (void)kernel.add_system({"render.batching.metrics", core::Phase::EndFrame, 40, {}},
        [reported = BatchError::None](core::Kernel& host, const core::Frame&) mutable {
            const auto& current = host.services().get<BatchingService>();
            const auto& stats = current.statistics();
            host.metric("render.batching.instances", static_cast<double>(stats.emitted));
            host.metric("render.batching.groups", static_cast<double>(stats.groups));
            host.metric("render.batching.cache_bytes", static_cast<double>(current.cache_statistics().bytes));
            host.metric("render.batching.reordered", static_cast<double>(stats.reordered_instances));
            if (stats.error != BatchError::None && stats.error != reported) {
                host.diagnose(core::Diagnostic::Severity::Warning, "render.batching", std::string(describe(stats.error)));
            }
            reported = stats.error;
        });
    (void)kernel.add_shutdown("render.batching", [owner = &service, host = &kernel] {
        owner->shutdown(host->services().find<VisibilityService>());
    });
    (void)kernel.add_command("batch.stats", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        return {args.empty(), args.empty() ? stats_text(host.services().get<BatchingService>()) : "usage: batch.stats"};
    }, "Inspect the last view's actual submission groups, preparation budgets and cache ownership");
    (void)kernel.add_command("batch.groups", group_command, "Inspect bounded contiguous groups and generation-checked instance provenance");
    (void)kernel.add_command("batch.appearance", appearance_command, "Edit per-entity authored appearance without mutating shared library materials");
    (void)kernel.add_command("batch.configure", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: batch.configure enabled|appearance|prepare|retain|reorder on|off"}; }
        const auto value = flag(args[1]);
        if (!value) { return {false, "expected on or off"}; }
        auto& current = host.services().get<BatchingService>();
        auto configuration = current.settings();
        if (args[0] == "enabled") { configuration.enabled = *value; }
        else if (args[0] == "appearance") { configuration.appearance_enabled = *value; }
        else if (args[0] == "prepare") { configuration.prepare_submesh_ranges = *value; }
        else if (args[0] == "retain") { configuration.retain_prepared_resources = *value; }
        else if (args[0] == "reorder") { configuration.order = *value ? BatchOrder::DisjointOpaque : BatchOrder::Preserve; }
        else { return {false, "unknown batching setting"}; }
        const bool changed = current.configure(configuration);
        return {changed, changed ? "batching configuration updated" : std::string(describe(current.statistics().error))};
    }, "Configure submission preparation and conservative disjoint-coverage grouping");
    (void)kernel.add_command("batch.cache.clear", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: batch.cache.clear"}; }
        host.services().get<BatchingService>().clear_cache();
        return {true, "preparation cache released; existing submissions retain their owned resources"};
    }, "Release retained preparation resources without invalidating already prepared submissions");
    (void)kernel.add_command("batch.cache.invalidate", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: batch.cache.invalidate mesh|material resource_name"}; }
        auto& current = host.services().get<BatchingService>();
        if (args[0] == "mesh") {
            const auto* meshes = host.services().find<geometry::MeshLibrary>();
            if (!meshes) { return {false, "MeshLibrary service required"}; }
            const auto handle = meshes->find(args[1]);
            if (!handle) { return {false, "mesh name is not registered"}; }
            const bool removed = current.invalidate(handle);
            return {true, removed ? "prepared mesh entries invalidated" : "mesh has no retained prepared entries"};
        }
        if (args[0] == "material") {
            const auto* materials = host.services().find<MaterialService>();
            if (!materials) { return {false, "MaterialService required"}; }
            const auto handle = materials->materials().find(args[1]);
            if (!handle) { return {false, "material name is not registered"}; }
            const bool removed = current.invalidate(handle);
            return {true, removed ? "prepared material entries invalidated" : "material has no retained prepared entries"};
        }
        return {false, "expected mesh or material"};
    }, "Invalidate generation-scoped cached preparation for one live resource");
}

} // namespace ge::render
