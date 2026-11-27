#include <ge/render/decals.hpp>

#include <ge/core/runtime.hpp>
#include <ge/render/postprocess.hpp>
#include <ge/render/rasterizer.hpp>
#include <ge/render/visibility.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ge::render {
namespace {

bool between(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool add_charge(std::size_t& total, std::size_t count, std::size_t limit) noexcept {
    if (total > limit || count > limit - total) { return false; }
    total += count;
    return true;
}

bool multiply_charge(std::size_t& total, std::size_t count, std::size_t bytes,
                     std::size_t limit) noexcept {
    if (bytes != 0 && count > limit / bytes) { return false; }
    return add_charge(total, count * bytes, limit);
}

std::uint64_t allocate_owner() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("decal owner identity exhausted");
        }
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) { return value; }
    }
}

double smooth(double value) noexcept {
    const double amount = std::clamp(value, 0.0, 1.0);
    return amount * amount * (3.0 - 2.0 * amount);
}

bool bounded_color(Color color) noexcept {
    return finite(color) && std::abs(color.r) <= material_hdr_limit &&
           std::abs(color.g) <= material_hdr_limit && std::abs(color.b) <= material_hdr_limit &&
           between(color.a, 0.0, 1.0);
}

bool affine(const math::Mat4& matrix) noexcept {
    return math::finite(matrix) && std::abs(matrix(3, 0)) <= 1.0e-10 &&
           std::abs(matrix(3, 1)) <= 1.0e-10 && std::abs(matrix(3, 2)) <= 1.0e-10 &&
           std::abs(matrix(3, 3) - 1.0) <= 1.0e-10;
}

bool consistent_camera(const CameraView& camera) noexcept {
    if (camera.viewport.empty() || !math::finite(camera.projection) ||
        !math::finite(camera.inverse_projection) || !affine(camera.view) ||
        !affine(camera.inverse_view) || !math::finite(camera.position) ||
        !math::finite(camera.forward)) { return false; }
    if (camera.clip_depth != math::ClipDepth::zero_to_one &&
        camera.clip_depth != math::ClipDepth::negative_one_to_one) { return false; }
    if (camera.projection_mode != ProjectionMode::Perspective &&
        camera.projection_mode != ProjectionMode::Orthographic) { return false; }
    const auto identity = math::Mat4::identity();
    return math::almost_equal(camera.projection * camera.inverse_projection, identity, 1.0e-7) &&
           math::almost_equal(camera.view * camera.inverse_view, identity, 1.0e-7) &&
           math::almost_equal(camera.projection * camera.view, camera.view_projection, 1.0e-7);
}

struct Reconstructed {
    math::Vec3 world{};
    math::Vec3 eye{};
};

std::optional<Reconstructed> reconstruct(const CameraView& camera, std::int32_t x,
                                          std::int32_t y, double depth) noexcept {
    if (!between(depth, 0.0, 1.0)) { return std::nullopt; }
    const double nx = (static_cast<double>(x) + 0.5 - camera.viewport.x) /
                      static_cast<double>(camera.viewport.width) * 2.0 - 1.0;
    const double ny = 1.0 - (static_cast<double>(y) + 0.5 - camera.viewport.y) /
                      static_cast<double>(camera.viewport.height) * 2.0;
    const double nz = camera.clip_depth == math::ClipDepth::zero_to_one ? depth : depth * 2.0 - 1.0;
    const auto eye = camera.inverse_projection * math::Vec4{nx, ny, nz, 1.0};
    if (!math::finite(eye) || std::abs(eye.w) <= 1.0e-12) { return std::nullopt; }
    const auto position = math::xyz(eye) / eye.w;
    const auto world = math::transform_point(camera.inverse_view, position);
    if (!math::finite(position) || !math::finite(world)) { return std::nullopt; }
    return Reconstructed{world, position};
}

std::size_t pixel_index(std::int32_t x, std::int32_t y, std::uint32_t width) noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
}

math::Vec2 projector_uv(math::Vec3 local, math::Vec3 half_extent) noexcept {
    return {0.5 + local.x / (2.0 * half_extent.x), 0.5 - local.y / (2.0 * half_extent.y)};
}

math::Vec2 binding_vector(const MaterialTexture& binding, math::Vec2 value) noexcept {
    value = value * binding.scale;
    const double cosine = std::cos(binding.rotation);
    const double sine = std::sin(binding.rotation);
    return {cosine * value.x - sine * value.y, sine * value.x + cosine * value.y};
}

Color texture_sample(const MaterialTexture& binding, const TextureLibrary& textures,
                     math::Vec2 uv, math::Vec2 dx, math::Vec2 dy, bool gradients,
                     DecalStatistics& statistics) noexcept {
    if (!binding.texture) { return Color::white(); }
    const auto* texture = textures.get(binding.texture);
    if (!texture) { return Color::transparent(); }
    ++statistics.texture_samples;
    const auto mapped = binding_vector(binding, uv) + binding.offset;
    if (gradients) {
        return texture->sample_grad(mapped, binding_vector(binding, dx),
                                    binding_vector(binding, dy), binding.sampler);
    }
    return texture->sample(mapped, 0.0, binding.sampler);
}

struct PreparedProjector {
    DecalHandle handle{};
    DecalProjector settings{};
    math::Mat4 world{};
    math::Mat4 inverse{};
    math::Vec3 direction{};
    PbrMaterial material{};
    double opacity{};
};

struct ProjectionJob {
    std::size_t projector{};
    std::size_t view{};
    Rect rectangle{};
};

std::optional<Rect> projected_rectangle(const PreparedProjector& projector,
                                        const DecalView& view) noexcept {
    std::array<math::Vec4, 8> corners{};
    const auto clip_matrix = view.camera.view_projection * projector.world;
    const auto extent = projector.settings.half_extent;
    bool near_crossing{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const math::Vec3 local{(index & 1U) != 0 ? extent.x : -extent.x,
                               (index & 2U) != 0 ? extent.y : -extent.y,
                               (index & 4U) != 0 ? extent.z : -extent.z};
        corners[index] = clip_matrix * math::homogeneous(local);
        if (!math::finite(corners[index])) { return std::nullopt; }
        const auto clip = corners[index];
        near_crossing = near_crossing || clip.w <= 1.0e-9 ||
            (view.camera.clip_depth == math::ClipDepth::zero_to_one ? clip.z <= 0.0 : clip.z <= -clip.w);
    }
    for (std::size_t plane = 0; plane < 6; ++plane) {
        const bool outside = std::all_of(corners.begin(), corners.end(), [&](math::Vec4 clip) {
            switch (plane) {
            case 0: return clip.x < -clip.w;
            case 1: return clip.x > clip.w;
            case 2: return clip.y < -clip.w;
            case 3: return clip.y > clip.w;
            case 4: return view.camera.clip_depth == math::ClipDepth::zero_to_one ? clip.z < 0.0 : clip.z < -clip.w;
            default: return clip.z > clip.w;
            }
        });
        if (outside) { return Rect{}; }
    }
    if (near_crossing) { return view.region; }
    double left = std::numeric_limits<double>::infinity();
    double top = left;
    double right = -left;
    double bottom = -left;
    for (const auto clip : corners) {
        const double x = static_cast<double>(view.camera.viewport.x) +
                         (clip.x / clip.w + 1.0) * 0.5 * view.camera.viewport.width;
        const double y = static_cast<double>(view.camera.viewport.y) +
                         (1.0 - clip.y / clip.w) * 0.5 * view.camera.viewport.height;
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
    const double region_right = static_cast<double>(view.region.x) + view.region.width;
    const double region_bottom = static_cast<double>(view.region.y) + view.region.height;
    left = std::clamp(std::floor(left), static_cast<double>(view.region.x), region_right);
    right = std::clamp(std::ceil(right), static_cast<double>(view.region.x), region_right);
    top = std::clamp(std::floor(top), static_cast<double>(view.region.y), region_bottom);
    bottom = std::clamp(std::ceil(bottom), static_cast<double>(view.region.y), region_bottom);
    return Rect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::int32_t>(std::max(0.0, right - left)),
                static_cast<std::int32_t>(std::max(0.0, bottom - top))};
}

struct Derivatives {
    math::Vec3 dx{};
    math::Vec3 dy{};
    math::Vec3 normal{};
    bool valid{};
};

Derivatives surface_derivatives(const DecalView& view, const Reconstructed& center,
                                 std::int32_t x, std::int32_t y, std::span<const double> depth,
                                 std::uint32_t width, const DecalSettings& settings,
                                 DecalStatistics& statistics) noexcept {
    const double threshold = std::max(settings.normal_absolute_threshold,
        settings.normal_relative_threshold * std::abs(center.eye.z));
    const auto neighbor = [&](std::int32_t column, std::int32_t row) -> std::optional<Reconstructed> {
        if (!view.region.contains(column, row)) { return std::nullopt; }
        ++statistics.depth_samples;
        const double value = depth[pixel_index(column, row, width)];
        if (std::abs(value - view.clear_depth) <= settings.clear_depth_epsilon) { return std::nullopt; }
        const auto sample = reconstruct(view.camera, column, row, value);
        if (!sample || std::abs(sample->eye.z - center.eye.z) > threshold) { return std::nullopt; }
        return sample;
    };
    const auto left = neighbor(x - 1, y);
    const auto right = neighbor(x + 1, y);
    const auto up = neighbor(x, y - 1);
    const auto down = neighbor(x, y + 1);
    if ((!left && !right) || (!up && !down)) { return {}; }
    const auto choose = [&](const auto& negative, const auto& positive) {
        if (!positive) { return center.world - negative->world; }
        if (!negative) { return positive->world - center.world; }
        return std::abs(positive->eye.z - center.eye.z) <= std::abs(negative->eye.z - center.eye.z)
            ? positive->world - center.world : center.world - negative->world;
    };
    Derivatives result;
    result.dx = choose(left, right);
    result.dy = choose(up, down);
    result.normal = math::cross(result.dy, result.dx);
    if (!math::normalize(result.normal)) { return {}; }
    const auto toward_eye = view.camera.projection_mode == ProjectionMode::Orthographic
        ? -view.camera.forward : view.camera.position - center.world;
    if (math::dot(result.normal, toward_eye) < 0.0) { result.normal = -result.normal; }
    result.valid = math::finite(result.dx) && math::finite(result.dy);
    return result;
}

double edge_opacity(const DecalProjector& projector, math::Vec3 local) noexcept {
    double result = 1.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double remaining = 1.0 - std::abs(local[axis]) / projector.half_extent[axis];
        if (remaining < 0.0) { return 0.0; }
        if (projector.edge_fade[axis] > 0.0) { result *= smooth(remaining / projector.edge_fade[axis]); }
    }
    return result;
}

} // namespace

std::string_view describe(DecalError error) noexcept {
    switch (error) {
    case DecalError::None: return "none";
    case DecalError::InvalidLimits: return "invalid limits";
    case DecalError::InvalidSettings: return "invalid settings";
    case DecalError::InvalidProjector: return "invalid projector";
    case DecalError::InvalidEntity: return "invalid entity";
    case DecalError::InvalidHandle: return "invalid handle";
    case DecalError::InvalidImage: return "invalid image or depth";
    case DecalError::InvalidView: return "invalid camera view";
    case DecalError::AmbiguousDepth: return "ambiguous depth ownership";
    case DecalError::MissingTexture: return "missing texture";
    case DecalError::MissingMaterial: return "missing material";
    case DecalError::MissingService: return "missing service";
    case DecalError::Capacity: return "projector capacity";
    case DecalError::WorkLimit: return "work budget";
    case DecalError::ScratchLimit: return "scratch budget";
    case DecalError::SceneRejected: return "scene rejected";
    case DecalError::AllocationFailure: return "allocation failure";
    case DecalError::GraphMissing: return "graph anchors unavailable";
    case DecalError::GraphConflict: return "graph ownership conflict";
    case DecalError::GraphRejected: return "graph edit rejected";
    case DecalError::Busy: return "service busy";
    case DecalError::Stopped: return "service stopped";
    }
    return "unknown decal error";
}

std::string_view describe(DecalBlend mode) noexcept {
    switch (mode) {
    case DecalBlend::Alpha: return "alpha";
    case DecalBlend::Multiply: return "multiply";
    case DecalBlend::Add: return "add";
    case DecalBlend::Screen: return "screen";
    }
    return "unknown";
}

DecalHandle::operator bool() const noexcept { return owner != 0 && generation != 0; }
DecalResult::operator bool() const noexcept { return error == DecalError::None; }

bool valid(const DecalLimits& limits) noexcept {
    return limits.projectors > 0 && limits.projectors <= 65536 && limits.views > 0 && limits.views <= 64 &&
           limits.jobs > 0 && limits.jobs <= 1048576 && limits.image_pixels > 0 &&
           limits.image_pixels <= 16777216 && limits.candidate_pixels > 0 &&
           limits.sample_visits > 0 && limits.scratch_bytes >= sizeof(Color) &&
           between(limits.maximum_delta_seconds, 0.001, 10.0);
}

bool valid(const DecalSettings& settings) noexcept {
    return between(settings.clear_depth_epsilon, 0.0, 0.01) &&
           between(settings.normal_absolute_threshold, 1.0e-8, 1.0e6) &&
           between(settings.normal_relative_threshold, 0.0, 1.0) &&
           between(settings.output_ceiling, 1.0, material_hdr_limit);
}

bool valid(const DecalProjector& projector) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!between(projector.half_extent[axis], 1.0e-6, 1.0e6) ||
            !between(projector.edge_fade[axis], 0.0, 1.0)) { return false; }
    }
    if (!bounded_color(projector.tint) || !validate_texture_binding(projector.texture) ||
        !between(projector.opacity, 0.0, 1.0) || !between(projector.alpha_cutoff, 0.0, 1.0) ||
        !between(projector.facing_start, -1.0, 1.0) ||
        !between(projector.facing_full, projector.facing_start, 1.0) ||
        !between(projector.fade_in_seconds, 0.0, 1.0e9) ||
        !between(projector.lifetime_seconds, 0.0, 1.0e9) ||
        !between(projector.fade_out_seconds, 0.0, 1.0e9) ||
        !between(projector.maximum_distance, 1.0e-6, 1.0e12) ||
        !between(projector.distance_fade, 0.0, projector.maximum_distance)) { return false; }
    return projector.blend == DecalBlend::Alpha || projector.blend == DecalBlend::Multiply ||
           projector.blend == DecalBlend::Add || projector.blend == DecalBlend::Screen;
}

Color blend_decal(Color destination, Color source, double opacity, DecalBlend mode, double ceiling) noexcept {
    if (!bounded_color(destination) || !bounded_color(source) || !between(opacity, 0.0, 1.0) ||
        !between(ceiling, 1.0, material_hdr_limit)) { return destination; }
    const double amount = source.a * opacity;
    if (amount <= 0.0) { return destination; }
    const auto channel = [&](double target, double incoming) {
        double combined{};
        switch (mode) {
        case DecalBlend::Alpha: combined = target * (1.0 - amount) + incoming * amount; break;
        case DecalBlend::Multiply: combined = target * (1.0 - amount + incoming * amount); break;
        case DecalBlend::Add: combined = target + incoming * amount; break;
        case DecalBlend::Screen:
            combined = target + (1.0 - std::clamp(target, 0.0, 1.0)) * std::clamp(incoming, 0.0, 1.0) * amount;
            break;
        default: return target;
        }
        return std::clamp(combined, 0.0, ceiling);
    };
    return {channel(destination.r, source.r), channel(destination.g, source.g),
            channel(destination.b, source.b), destination.a};
}

struct DecalService::Impl {
    struct Slot {
        ecs::Entity entity{};
        std::uint64_t generation{1};
        double age{};
        bool active{};
    };
    scene::Scene& scene;
    DecalLimits limits;
    DecalSettings settings;
    DecalStatistics statistics;
    DecalError error{DecalError::None};
    DecalError graph_error{DecalError::None};
    std::uint64_t owner{allocate_owner()};
    std::vector<Slot> slots;
    GraphPass graph_pass{};
    GraphResource graph_color{};
    GraphResource graph_depth{};
    std::uint64_t graph_revision{};
    bool connected{};
    bool busy{};
    bool stopped{};

    Impl(scene::Scene& shared_scene, DecalLimits budget) : scene(shared_scene), limits(budget) {
        if (!valid(limits)) { throw std::invalid_argument("invalid decal limits"); }
        slots.reserve(limits.projectors);
        (void)scene.registry().register_component<DecalComponent>("render.decal");
    }
    DecalHandle identity(std::size_t index) const noexcept {
        return {owner, static_cast<std::uint32_t>(index), slots[index].generation};
    }
    const Slot* resolve(DecalHandle handle) const noexcept {
        if (handle.owner != owner || handle.index >= slots.size()) { return nullptr; }
        const auto& slot = slots[handle.index];
        const auto* component = scene.registry().get<DecalComponent>(slot.entity);
        return slot.active && slot.generation == handle.generation && scene.contains(slot.entity) &&
               component && component->handle == handle ? &slot : nullptr;
    }
    void retire(Slot& slot) noexcept {
        slot.active = false;
        slot.entity = {};
        slot.age = 0.0;
        if (slot.generation != std::numeric_limits<std::uint64_t>::max()) { ++slot.generation; }
    }
    DecalResult result(DecalError failure, bool changed = false) noexcept {
        error = failure;
        statistics.error = failure;
        statistics.image_changed = changed;
        if (failure == DecalError::None) { ++statistics.completed; }
        else { ++statistics.rejected; }
        return {failure, changed};
    }
    DecalError prepare(std::vector<PreparedProjector>& prepared, const TextureLibrary& textures,
                       const MaterialLibrary* materials) {
        const auto updated = scene.update();
        if (updated.error != scene::SceneError::None) { return DecalError::SceneRejected; }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            const auto id = identity(index);
            const auto* slot = resolve(id);
            if (!slot) { continue; }
            ++statistics.active_projectors;
            const auto& projector = scene.registry().get<DecalComponent>(slot->entity)->projector;
            if (!valid(projector)) { return DecalError::InvalidProjector; }
            if (!projector.enabled || !scene.effective_enabled(slot->entity) || projector.opacity == 0.0 ||
                (projector.lifetime_seconds > 0.0 && slot->age >= projector.lifetime_seconds)) { continue; }
            const auto* matrix = scene.cached_world_matrix(slot->entity);
            if (!matrix || !affine(*matrix)) { return DecalError::InvalidProjector; }
            const auto inverse = math::inverse(*matrix);
            if (!inverse) { return DecalError::InvalidProjector; }
            PreparedProjector item;
            item.handle = id;
            item.settings = projector;
            item.world = *matrix;
            item.inverse = *inverse;
            item.direction = math::transform_vector(*matrix, {0.0, 0.0, -1.0});
            if (!math::normalize(item.direction)) { return DecalError::InvalidProjector; }
            if (projector.material) {
                const auto* material = materials ? materials->get(projector.material) : nullptr;
                if (!material || !validate_material(*material)) { return DecalError::MissingMaterial; }
                item.material = *material;
            }
            const std::array bindings{projector.texture, item.material.albedo_texture, item.material.emissive_texture};
            for (const auto& binding : bindings) {
                if (binding.texture) {
                    const auto* texture = textures.get(binding.texture);
                    if (!texture || texture->empty()) { return DecalError::MissingTexture; }
                }
            }
            item.opacity = projector.opacity;
            if (projector.fade_in_seconds > 0.0) { item.opacity *= smooth(slot->age / projector.fade_in_seconds); }
            if (projector.lifetime_seconds > 0.0 && projector.fade_out_seconds > 0.0) {
                item.opacity *= smooth((projector.lifetime_seconds - slot->age) / projector.fade_out_seconds);
            }
            prepared.push_back(std::move(item));
        }
        std::sort(prepared.begin(), prepared.end(), [](const auto& left, const auto& right) {
            return left.settings.order != right.settings.order ? left.settings.order < right.settings.order
                                                               : left.handle.index < right.handle.index;
        });
        statistics.prepared_projectors = prepared.size();
        return DecalError::None;
    }
    bool paint(std::vector<Color>& output, std::span<const double> depth, std::uint32_t width,
               const ProjectionJob& job, const PreparedProjector& projector, const DecalView& view,
               const TextureLibrary& textures) noexcept {
        const auto& authored = projector.settings;
        for (std::int32_t y = job.rectangle.y; y < job.rectangle.y + job.rectangle.height; ++y) {
            for (std::int32_t x = job.rectangle.x; x < job.rectangle.x + job.rectangle.width; ++x) {
                ++statistics.visited_pixels;
                ++statistics.depth_samples;
                const auto index = pixel_index(x, y, width);
                if (std::abs(depth[index] - view.clear_depth) <= settings.clear_depth_epsilon) {
                    ++statistics.background_pixels;
                    continue;
                }
                const auto point = reconstruct(view.camera, x, y, depth[index]);
                if (!point) { return false; }
                const auto local = math::transform_point(projector.inverse, point->world);
                if (!math::finite(local)) { return false; }
                double opacity = projector.opacity * edge_opacity(authored, local);
                if (opacity <= 0.0) { ++statistics.outside_volume; continue; }
                const double distance = math::distance(view.camera.position, point->world);
                if (distance >= authored.maximum_distance) { continue; }
                if (authored.distance_fade > 0.0) {
                    opacity *= smooth((authored.maximum_distance - distance) / authored.distance_fade);
                }
                const auto derivatives = surface_derivatives(view, *point, x, y, depth, width, settings, statistics);
                if (authored.facing_fade) {
                    if (!derivatives.valid) { ++statistics.normal_rejections; continue; }
                    double facing = math::dot(derivatives.normal, -projector.direction);
                    if (authored.two_sided) { facing = std::abs(facing); }
                    opacity *= authored.facing_full > authored.facing_start
                        ? smooth((facing - authored.facing_start) / (authored.facing_full - authored.facing_start))
                        : (facing >= authored.facing_full ? 1.0 : 0.0);
                    if (opacity <= 0.0) { continue; }
                }
                const auto uv = projector_uv(local, authored.half_extent);
                math::Vec2 dx{};
                math::Vec2 dy{};
                if (derivatives.valid) {
                    const auto local_dx = math::transform_vector(projector.inverse, derivatives.dx);
                    const auto local_dy = math::transform_vector(projector.inverse, derivatives.dy);
                    dx = {local_dx.x / (2.0 * authored.half_extent.x), -local_dx.y / (2.0 * authored.half_extent.y)};
                    dy = {local_dy.x / (2.0 * authored.half_extent.x), -local_dy.y / (2.0 * authored.half_extent.y)};
                }
                const auto& material = projector.material;
                const auto base_map = texture_sample(material.albedo_texture, textures, uv, dx, dy, derivatives.valid, statistics);
                auto source = material.albedo * base_map;
                if (material.alpha_mode == AlphaMode::Mask && source.a < material.alpha_cutoff) { continue; }
                if (material.alpha_mode != AlphaMode::Blend) { source.a = 1.0; }
                const auto emissive_map = texture_sample(material.emissive_texture, textures, uv, dx, dy, derivatives.valid, statistics);
                source.r += material.emissive.r * material.emissive_strength * emissive_map.r;
                source.g += material.emissive.g * material.emissive_strength * emissive_map.g;
                source.b += material.emissive.b * material.emissive_strength * emissive_map.b;
                source = source * authored.tint * texture_sample(authored.texture, textures, uv, dx, dy, derivatives.valid, statistics);
                if (!finite(source) || !between(source.a, 0.0, 1.0)) { return false; }
                if (source.a <= authored.alpha_cutoff || opacity <= 0.0) { continue; }
                source.r = std::clamp(source.r, 0.0, settings.output_ceiling);
                source.g = std::clamp(source.g, 0.0, settings.output_ceiling);
                source.b = std::clamp(source.b, 0.0, settings.output_ceiling);
                const auto blended = blend_decal(output[index], source, opacity, authored.blend, settings.output_ceiling);
                if (blended != output[index]) { output[index] = blended; ++statistics.blended_pixels; }
            }
        }
        return true;
    }
    DecalResult run(Image& color, std::span<const double> depth, std::span<const DecalView> views,
                    const TextureLibrary& textures, const MaterialLibrary* materials) {
        const auto previous = statistics;
        statistics = {};
        statistics.attempts = previous.attempts + 1;
        statistics.completed = previous.completed;
        statistics.rejected = previous.rejected;
        statistics.expired = previous.expired;
        if (!settings.enabled) { return result(DecalError::None); }
        if (color.empty() || color.size() > limits.image_pixels || depth.size() != color.size() ||
            color.width() > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            color.height() > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            return result(DecalError::InvalidImage);
        }
        if (views.empty() || views.size() > limits.views) { return result(DecalError::InvalidView); }
        for (std::size_t index = 0; index < views.size(); ++index) {
            const auto& view = views[index];
            if (!consistent_camera(view.camera) || !between(view.clear_depth, 0.0, 1.0) ||
                view.region != intersection(view.region, intersection(color.bounds(), view.camera.viewport))) {
                return result(DecalError::InvalidView);
            }
            if (!view.depth_cleared) { return result(DecalError::AmbiguousDepth); }
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (!intersection(view.region, views[earlier].region).empty()) { return result(DecalError::AmbiguousDepth); }
            }
        }
        const auto maximum_jobs = std::min(limits.jobs, slots.size() * views.size());
        auto& scratch = statistics.scratch_bytes;
        if (!multiply_charge(scratch, slots.size(), sizeof(PreparedProjector), limits.scratch_bytes) ||
            !multiply_charge(scratch, maximum_jobs, sizeof(ProjectionJob), limits.scratch_bytes) ||
            !multiply_charge(scratch, views.size(), sizeof(CameraView) + sizeof(DecalView), limits.scratch_bytes) ||
            !multiply_charge(scratch, color.size(), sizeof(Color), limits.scratch_bytes)) {
            return result(DecalError::ScratchLimit);
        }
        std::vector<PreparedProjector> projectors;
        projectors.reserve(slots.size());
        const auto preparation = prepare(projectors, textures, materials);
        if (preparation != DecalError::None) { return result(preparation); }
        std::vector<ProjectionJob> jobs;
        jobs.reserve(maximum_jobs);
        for (std::size_t index = 0; index < projectors.size(); ++index) {
            for (std::size_t view = 0; view < views.size(); ++view) {
                if ((projectors[index].settings.view_mask & views[view].camera.visibility_mask) == 0 ||
                    views[view].region.empty() || projectors[index].opacity == 0.0) { continue; }
                const auto rectangle = projected_rectangle(projectors[index], views[view]);
                if (!rectangle) { return result(DecalError::InvalidProjector); }
                if (rectangle->empty()) { continue; }
                if (jobs.size() >= limits.jobs) { return result(DecalError::WorkLimit); }
                const auto area = static_cast<std::size_t>(rectangle->width) * static_cast<std::size_t>(rectangle->height);
                if (!add_charge(statistics.candidate_pixels, area, limits.candidate_pixels)) { return result(DecalError::WorkLimit); }
                jobs.push_back({index, view, *rectangle});
            }
        }
        statistics.jobs = jobs.size();
        if (jobs.empty()) { return result(DecalError::None); }
        std::size_t samples{};
        // Three image sweeps and at most five depth reads plus three eight-tap
        // texture samples per candidate; spare visits cover blending and writes.
        if (!multiply_charge(samples, color.size(), 4, limits.sample_visits) ||
            !multiply_charge(samples, statistics.candidate_pixels, 32, limits.sample_visits)) {
            return result(DecalError::WorkLimit);
        }
        for (std::size_t index = 0; index < color.size(); ++index) {
            if (!bounded_color(color.pixels()[index]) || !between(depth[index], 0.0, 1.0)) {
                return result(DecalError::InvalidImage);
            }
        }
        std::vector<Color> output(color.pixels().begin(), color.pixels().end());
        for (const auto& job : jobs) {
            if (!paint(output, depth, color.width(), job, projectors[job.projector], views[job.view], textures)) {
                return result(DecalError::InvalidImage);
            }
        }
        if (statistics.blended_pixels == 0) { return result(DecalError::None); }
        std::copy(output.begin(), output.end(), color.pixels().begin());
        return result(DecalError::None, true);
    }
};

DecalService::DecalService(scene::Scene& scene, DecalLimits limits) : impl_(std::make_shared<Impl>(scene, limits)) {}
DecalService::~DecalService() { stop(); }

DecalHandle DecalService::create(std::string_view name, const math::Transform& transform, const DecalProjector& projector) {
    if (impl_->stopped || impl_->busy || !valid(projector) || !math::finite(transform) || name.size() > 1024) {
        impl_->error = DecalError::InvalidProjector;
        return {};
    }
    ecs::Entity created{};
    try {
        created = impl_->scene.create(name, {}, transform);
        if (!created) { impl_->error = DecalError::SceneRejected; return {}; }
        const auto result = attach(created, projector);
        if (!result) { (void)impl_->scene.destroy(created); }
        return result;
    } catch (const std::exception&) {
        if (created) { (void)impl_->scene.destroy(created); }
        impl_->error = DecalError::AllocationFailure;
        return {};
    }
}

DecalHandle DecalService::attach(ecs::Entity entity, const DecalProjector& projector) {
    if (impl_->stopped || impl_->busy) { impl_->error = impl_->stopped ? DecalError::Stopped : DecalError::Busy; return {}; }
    if (!impl_->scene.contains(entity) || !impl_->scene.registry().alive(entity)) { impl_->error = DecalError::InvalidEntity; return {}; }
    if (!valid(projector)) { impl_->error = DecalError::InvalidProjector; return {}; }
    if (const auto existing = handle(entity)) { return replace(existing, projector) ? existing : DecalHandle{}; }
    std::size_t index{};
    for (; index < impl_->slots.size(); ++index) {
        auto& slot = impl_->slots[index];
        if (slot.active && !impl_->resolve(impl_->identity(index))) { impl_->retire(slot); }
        if (!slot.active && slot.generation != std::numeric_limits<std::uint64_t>::max()) { break; }
    }
    if (index == impl_->limits.projectors) { impl_->error = DecalError::Capacity; return {}; }
    try {
        if (index == impl_->slots.size()) { impl_->slots.emplace_back(); }
        const auto id = impl_->identity(index);
        (void)impl_->scene.registry().emplace_or_replace<DecalComponent>(entity, DecalComponent{id, projector});
        auto& slot = impl_->slots[index];
        slot.entity = entity;
        slot.age = 0.0;
        slot.active = true;
        impl_->error = DecalError::None;
        return id;
    } catch (const std::exception&) { impl_->error = DecalError::AllocationFailure; return {}; }
}

bool DecalService::erase(DecalHandle handle) {
    if (impl_->busy || impl_->stopped || !impl_->resolve(handle)) { impl_->error = DecalError::InvalidHandle; return false; }
    try {
        auto& slot = impl_->slots[handle.index];
        if (!impl_->scene.registry().remove<DecalComponent>(slot.entity)) { return false; }
        impl_->retire(slot);
        impl_->error = DecalError::None;
        return true;
    } catch (const std::exception&) { impl_->error = DecalError::SceneRejected; return false; }
}

std::size_t DecalService::clear() {
    std::size_t removed{};
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        if (impl_->resolve(impl_->identity(index)) && erase(impl_->identity(index))) { ++removed; }
    }
    return removed;
}

bool DecalService::replace(DecalHandle handle, const DecalProjector& projector) {
    if (impl_->busy || impl_->stopped || !impl_->resolve(handle)) { impl_->error = DecalError::InvalidHandle; return false; }
    if (!valid(projector)) { impl_->error = DecalError::InvalidProjector; return false; }
    const auto entity = impl_->slots[handle.index].entity;
    const bool edited = impl_->scene.registry().patch<DecalComponent>(entity, [&](DecalComponent& component) { component.projector = projector; });
    impl_->error = edited ? DecalError::None : DecalError::SceneRejected;
    return edited;
}

bool DecalService::restart(DecalHandle handle) noexcept {
    if (impl_->busy || impl_->stopped || !impl_->resolve(handle)) { return false; }
    impl_->slots[handle.index].age = 0.0;
    return true;
}

ecs::Entity DecalService::entity(DecalHandle handle) const noexcept {
    const auto* slot = impl_->resolve(handle);
    return slot ? slot->entity : ecs::Entity{};
}

DecalHandle DecalService::handle(ecs::Entity entity) const noexcept {
    const auto* component = impl_->scene.registry().get<DecalComponent>(entity);
    if (!component) { return {}; }
    const auto* slot = impl_->resolve(component->handle);
    return slot && slot->entity == entity ? component->handle : DecalHandle{};
}

const DecalComponent* DecalService::component(DecalHandle handle) const noexcept {
    const auto* slot = impl_->resolve(handle);
    return slot ? impl_->scene.registry().get<DecalComponent>(slot->entity) : nullptr;
}

std::vector<DecalHandle> DecalService::handles() const {
    std::vector<DecalHandle> result;
    result.reserve(impl_->slots.size());
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        const auto id = impl_->identity(index);
        if (impl_->resolve(id)) { result.push_back(id); }
    }
    return result;
}

std::optional<math::Aabb> DecalService::world_bounds(DecalHandle handle) {
    const auto* value = component(handle);
    if (!value || !valid(value->projector)) { return std::nullopt; }
    const auto matrix = impl_->scene.world_matrix(entity(handle));
    if (!matrix || !affine(*matrix)) { return std::nullopt; }
    const auto extent = value->projector.half_extent;
    const auto box = math::transform_bounds(*matrix, {-extent, extent});
    return math::valid(box) ? std::optional<math::Aabb>{box} : std::nullopt;
}

double DecalService::age(DecalHandle handle) const noexcept {
    const auto* slot = impl_->resolve(handle);
    return slot ? slot->age : 0.0;
}

bool DecalService::configure(const DecalSettings& settings) noexcept {
    if (impl_->busy || impl_->stopped || !valid(settings)) { impl_->error = DecalError::InvalidSettings; return false; }
    impl_->settings = settings;
    impl_->error = DecalError::None;
    return true;
}

const DecalSettings& DecalService::settings() const noexcept { return impl_->settings; }
const DecalLimits& DecalService::limits() const noexcept { return impl_->limits; }
const DecalStatistics& DecalService::statistics() const noexcept { return impl_->statistics; }
DecalError DecalService::last_error() const noexcept { return impl_->error; }
DecalError DecalService::attachment_error() const noexcept { return impl_->graph_error; }

void DecalService::advance(double delta_seconds) {
    if (impl_->busy || impl_->stopped || !between(delta_seconds, 0.0, 1.0e9)) { return; }
    const double delta = std::min(delta_seconds, impl_->limits.maximum_delta_seconds);
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        auto& slot = impl_->slots[index];
        if (!slot.active) { continue; }
        const auto* value = component(impl_->identity(index));
        if (!value) { impl_->retire(slot); continue; }
        const double lifetime = value->projector.lifetime_seconds;
        const double previous = slot.age;
        slot.age = std::min(1.0e9, slot.age + delta);
        if (lifetime > 0.0 && previous < lifetime && slot.age >= lifetime) { ++impl_->statistics.expired; }
    }
}

DecalResult DecalService::process(Image& color, std::span<const double> depth, std::span<const DecalView> views,
                                   const TextureLibrary& textures, const MaterialLibrary* materials) noexcept {
    if (impl_->stopped) { return {DecalError::Stopped, false}; }
    if (impl_->busy) { return {DecalError::Busy, false}; }
    impl_->busy = true;
    struct Release { bool& value; ~Release() { value = false; } } release{impl_->busy};
    try { return impl_->run(color, depth, views, textures, materials); }
    catch (const std::bad_alloc&) { return impl_->result(DecalError::AllocationFailure); }
    catch (const std::length_error&) { return impl_->result(DecalError::AllocationFailure); }
    catch (const std::exception&) { return impl_->result(DecalError::SceneRejected); }
}

namespace {
bool depth_settings_match(const RasterizerSettings& first, const RasterizerSettings& second) noexcept {
    return first.enabled == second.enabled && first.clear_depth == second.clear_depth &&
           first.background_depth == second.background_depth && first.scissor == second.scissor &&
           first.depth.test_enabled == second.depth.test_enabled && first.depth.write_enabled == second.depth.write_enabled &&
           first.depth.compare == second.depth.compare && first.depth_bias == second.depth_bias &&
           first.slope_depth_bias == second.slope_depth_bias && first.render_all_views == second.render_all_views &&
           first.allow_stale_views == second.allow_stale_views && first.blended_depth_write == second.blended_depth_write;
}

bool depth_policy_supported(const RasterizerSettings& settings) noexcept {
    return settings.enabled && settings.clear_depth && settings.depth.test_enabled && settings.depth.write_enabled &&
           settings.depth_bias == 0.0 && settings.slope_depth_bias == 0.0 &&
           (settings.depth.compare == DepthCompare::Less || settings.depth.compare == DepthCompare::LessEqual ||
            settings.depth.compare == DepthCompare::Greater || settings.depth.compare == DepthCompare::GreaterEqual);
}
}

bool DecalService::attached(const RenderGraph& graph) const noexcept {
    const auto info = graph.pass_info(impl_->graph_pass);
    return !impl_->stopped && impl_->connected && info && info->enabled &&
           info->callback_owner == graph_decals_callback_owner && info->callback_revision == impl_->graph_revision &&
           graph.find_resource(graph_main_color) == impl_->graph_color &&
           graph.find_resource(graph_main_depth) == impl_->graph_depth;
}

DecalError DecalService::install_graph(core::Kernel& kernel) {
    const auto fail = [&](DecalError error) { impl_->graph_error = error; return error; };
    if (impl_->stopped || impl_->busy) { return fail(impl_->stopped ? DecalError::Stopped : DecalError::Busy); }
    auto* graph = kernel.services().find<RenderGraph>();
    if (!graph) { return fail(DecalError::GraphMissing); }
    const auto scene_pass = graph->find_pass(graph_scene_pass);
    const auto complete = graph->find_pass(graph_complete_pass);
    const auto color = graph->find_resource(graph_main_color);
    const auto depth = graph->find_resource(graph_main_depth);
    if (!scene_pass || !complete || !color || !depth) { return fail(DecalError::GraphMissing); }
    const auto color_info = graph->resource_info(*color);
    const auto depth_info = graph->resource_info(*depth);
    if (!color_info || !depth_info || !color_info->imported || !depth_info->imported ||
        color_info->kind != GraphResourceKind::Image || depth_info->kind != GraphResourceKind::Depth) { return fail(DecalError::GraphMissing); }
    const auto existing = graph->find_pass(graph_decals_pass);
    const auto stamp = existing ? graph->pass_info(*existing) : std::nullopt;
    if (existing && (*existing != impl_->graph_pass || !stamp || !stamp->enabled || stamp->callback_owner != graph_decals_callback_owner ||
        stamp->callback_revision != impl_->graph_revision || *color != impl_->graph_color || *depth != impl_->graph_depth)) {
        return fail(DecalError::GraphConflict);
    }
    try {
        if (!attached(*graph)) {
            const std::weak_ptr<Impl> weak = impl_;
            GraphCallback callback = [weak, host = &kernel, color_resource = *color,
                                      depth_resource = *depth, raster_pass = *scene_pass](GraphContext& context) -> GraphPassResult {
                const auto state = weak.lock();
                if (!state || state->stopped || !state->connected || context.pass() != state->graph_pass) { return {}; }
                if (!state->settings.enabled || std::none_of(state->slots.begin(), state->slots.end(),
                    [&state](const auto& slot) {
                        const auto* value = state->scene.registry().get<DecalComponent>(slot.entity);
                        return slot.active && value && value->projector.enabled && value->projector.opacity > 0.0 &&
                            (value->projector.lifetime_seconds <= 0.0 || slot.age < value->projector.lifetime_seconds);
                    })) { return {}; }
                bool published{};
                try {
                    const auto reject = [&](DecalError error) -> GraphPassResult {
                        (void)state->result(error);
                        return {false, false, std::string(describe(error))};
                    };
                    auto* current_graph = host->services().find<RenderGraph>();
                    auto* service = host->services().find<DecalService>();
                    auto* images = host->services().find<ImageService>();
                    const auto* renderer = host->services().find<SoftwareRenderer>();
                    if (!current_graph || !service || service->impl_ != state || !images || !renderer) { return reject(DecalError::MissingService); }
                    const auto raster_stamp = current_graph->pass_info(raster_pass);
                    if (!raster_stamp || raster_stamp->state != GraphPassState::Succeeded || renderer->last_error() != RasterError::None) {
                        return reject(DecalError::AmbiguousDepth);
                    }
                    auto* target = context.write_image(color_resource);
                    const auto samples = context.read_depth(depth_resource);
                    const auto& framebuffer = images->framebuffer();
                    if (target != &framebuffer.color() || samples.data() != framebuffer.depth().data() ||
                        samples.size() != framebuffer.depth().size() || !framebuffer.valid()) { return reject(DecalError::InvalidImage); }
                    bool after_raster{};
                    bool intervening_pass{};
                    for (const auto pass : current_graph->schedule()) {
                        if (pass == raster_pass) { after_raster = true; continue; }
                        if (pass == context.pass()) { break; }
                        if (!after_raster) { continue; }
                        const auto info = current_graph->pass_info(pass);
                        intervening_pass = intervening_pass || (info && info->enabled);
                        if (info && info->enabled && std::any_of(info->resources.begin(), info->resources.end(), [&](const GraphUse& use) {
                            return use.resource == depth_resource && use.access != GraphAccess::Read;
                        })) { return reject(DecalError::AmbiguousDepth); }
                    }
                    std::vector<CameraView> cameras;
                    RasterizerSettings raster_settings = renderer->settings();
                    if (raster_stamp->callback_owner == visibility_callback_owner) {
                        const auto* visibility = host->services().find<VisibilityService>();
                        if (visibility && visibility->views().size() > state->limits.views) { return reject(DecalError::WorkLimit); }
                        auto rendered = visibility ? visibility->rendered_frame() : std::nullopt;
                        if (!rendered || rendered->tick != context.frame().tick || rendered->pass != raster_pass ||
                            rendered->callback_owner != raster_stamp->callback_owner ||
                            rendered->callback_revision != raster_stamp->callback_revision ||
                            rendered->raster_frame != renderer->statistics().frame || rendered->target != &framebuffer ||
                            rendered->width != framebuffer.width() || rendered->height != framebuffer.height() ||
                            !depth_settings_match(rendered->raster_settings, raster_settings)) { return reject(DecalError::AmbiguousDepth); }
                        cameras = std::move(rendered->views);
                        raster_settings = rendered->raster_settings;
                    } else if (raster_stamp->callback_owner == graph_default_scene_callback_owner) {
                        if (intervening_pass) { return reject(DecalError::AmbiguousDepth); }
                        const auto* camera_service = host->services().find<CameraService>();
                        if (!camera_service) { return reject(DecalError::MissingService); }
                        if (raster_settings.render_all_views) {
                            const auto available = camera_service->views();
                            if (available.size() > state->limits.views) { return reject(DecalError::WorkLimit); }
                            cameras.assign(available.begin(), available.end());
                        } else if (const auto* camera = camera_service->current_view()) { cameras.push_back(*camera); }
                    } else { return reject(DecalError::GraphConflict); }
                    if (!depth_policy_supported(raster_settings) || cameras.size() != renderer->statistics().views) {
                        return reject(DecalError::AmbiguousDepth);
                    }
                    if (cameras.size() > state->limits.views) { return reject(DecalError::WorkLimit); }
                    std::vector<DecalView> views;
                    views.reserve(cameras.size());
                    for (const auto& camera : cameras) {
                        auto region = intersection(camera.viewport, target->bounds());
                        if (raster_settings.scissor) { region = intersection(region, *raster_settings.scissor); }
                        views.push_back({camera, region, raster_settings.background_depth, raster_settings.clear_depth});
                    }
                    const auto* materials = host->services().find<MaterialService>();
                    const auto outcome = service->process(*target, samples, views, images->textures(), materials ? &materials->materials() : nullptr);
                    published = outcome.image_changed;
                    host->metric("render.decals.pixels", static_cast<double>(state->statistics.blended_pixels));
                    host->metric("render.decals.candidates", static_cast<double>(state->statistics.candidate_pixels));
                    host->metric("render.decals.error", static_cast<double>(outcome.error));
                    return {static_cast<bool>(outcome), outcome.image_changed, std::string(describe(outcome.error))};
                } catch (const std::bad_alloc&) {
                    (void)state->result(DecalError::AllocationFailure, published);
                    return {false, published, {}};
                } catch (const std::length_error&) {
                    (void)state->result(DecalError::AllocationFailure, published);
                    return {false, published, {}};
                }
            };
            GraphPass inserted;
            if (existing) {
                if (graph->replace_callback_if(*existing, graph_decals_callback_owner, impl_->graph_revision,
                    graph_decals_callback_owner, std::move(callback))) { inserted = *existing; }
            } else {
                const auto anchor = graph->find_pass(graph_postprocess_pass).value_or(*complete);
                inserted = graph->insert_before(anchor, {std::string(graph_decals_pass),
                    {{*color, GraphAccess::ReadWrite}, {*depth, GraphAccess::Read}}, {*scene_pass}, 0, true,
                    graph_decals_callback_owner}, std::move(callback));
            }
            if (!inserted) { return fail(DecalError::GraphRejected); }
            impl_->graph_pass = inserted;
            impl_->graph_color = *color;
            impl_->graph_depth = *depth;
            impl_->graph_revision = graph->pass_info(inserted)->callback_revision;
            impl_->connected = true;
        }
        if (const auto postprocess = graph->find_pass(graph_postprocess_pass)) {
            const auto info = graph->pass_info(*postprocess);
            if (!info || (std::find(info->after.begin(), info->after.end(), impl_->graph_pass) == info->after.end() &&
                !graph->add_dependency(*postprocess, impl_->graph_pass))) { return fail(DecalError::GraphRejected); }
        }
        return fail(DecalError::None);
    } catch (const std::bad_alloc&) { return fail(DecalError::AllocationFailure); }
    catch (const std::length_error&) { return fail(DecalError::AllocationFailure); }
}

bool DecalService::detach(RenderGraph& graph) {
    if (!graph.pass_info(impl_->graph_pass)) { impl_->connected = false; return true; }
    if (!graph.replace_callback_if(impl_->graph_pass, graph_decals_callback_owner, impl_->graph_revision,
        graph_decals_callback_owner, [](GraphContext&) -> GraphPassResult { return {}; })) {
        impl_->graph_error = DecalError::GraphConflict;
        return false;
    }
    impl_->graph_revision = graph.pass_info(impl_->graph_pass)->callback_revision;
    impl_->connected = false;
    return true;
}

void DecalService::stop() noexcept { impl_->stopped = true; impl_->connected = false; }

namespace {
std::optional<double> parse_number(std::string_view text) noexcept {
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value)
        ? std::optional<double>{value} : std::nullopt;
}

std::optional<math::Vec3> parse_vector(core::CommandArgs arguments, std::size_t offset) noexcept {
    if (offset > arguments.size() || arguments.size() - offset < 3) { return std::nullopt; }
    const auto x = parse_number(arguments[offset]);
    const auto y = parse_number(arguments[offset + 1]);
    const auto z = parse_number(arguments[offset + 2]);
    return x && y && z ? std::optional<math::Vec3>{math::Vec3{*x, *y, *z}} : std::nullopt;
}

ecs::Entity command_entity(scene::Scene& scene, std::string_view text) {
    if (const auto entity = ecs::parse_entity(text); entity && scene.registry().alive(*entity)) { return *entity; }
    const auto matches = scene.find_all_named(text, {}, 2);
    return matches.size() == 1 ? matches.front() : ecs::Entity{};
}

core::CommandResult edit_projector(core::Kernel& kernel, core::CommandArgs args) {
    if (args.size() < 3) { return {false, "usage: render.decals.set ENTITY FIELD VALUES"}; }
    auto& service = kernel.services().get<DecalService>();
    auto& scene = kernel.services().get<scene::Scene>();
    const auto handle = service.handle(command_entity(scene, args[0]));
    const auto* component = service.component(handle);
    if (!component) { return {false, "projector entity unavailable or ambiguous"}; }
    auto projector = component->projector;
    const auto& field = args[1];
    const auto scalar = parse_number(args[2]);
    const auto vector = parse_vector(args, 2);
    bool accepted{};
    if (field == "opacity" && args.size() == 3 && scalar) { projector.opacity = *scalar; accepted = true; }
    else if (field == "cutoff" && args.size() == 3 && scalar) { projector.alpha_cutoff = *scalar; accepted = true; }
    else if ((field == "volume" || field == "edge") && args.size() == 5 && vector) {
        if (field == "volume") { projector.half_extent = *vector; } else { projector.edge_fade = *vector; }
        accepted = true;
    } else if (field == "tint" && args.size() == 6 && vector) {
        if (const auto alpha = parse_number(args[5])) { projector.tint = {vector->x, vector->y, vector->z, *alpha}; accepted = true; }
    } else if (field == "lifetime" && args.size() == 5 && vector) {
        projector.fade_in_seconds = vector->x; projector.lifetime_seconds = vector->y; projector.fade_out_seconds = vector->z; accepted = true;
    } else if ((field == "facing" || field == "distance") && args.size() == 4 && scalar) {
        if (const auto other = parse_number(args[3])) {
            if (field == "facing") { projector.facing_start = *scalar; projector.facing_full = *other; projector.facing_fade = true; }
            else { projector.maximum_distance = *scalar; projector.distance_fade = *other; }
            accepted = true;
        }
    } else if ((field == "enabled" || field == "normals" || field == "two_sided") && args.size() == 3) {
        if (args[2] == "on" || args[2] == "off") {
            const bool enabled = args[2] == "on";
            if (field == "enabled") { projector.enabled = enabled; }
            else if (field == "normals") { projector.facing_fade = enabled; }
            else { projector.two_sided = enabled; }
            accepted = true;
        }
    } else if (field == "blend" && args.size() == 3) {
        for (const auto mode : {DecalBlend::Alpha, DecalBlend::Multiply, DecalBlend::Add, DecalBlend::Screen}) {
            if (args[2] == describe(mode)) { projector.blend = mode; accepted = true; }
        }
    } else if (field == "order" && args.size() == 3) {
        std::int32_t value{};
        const auto parsed = std::from_chars(args[2].data(), args[2].data() + args[2].size(), value);
        accepted = parsed.ec == std::errc{} && parsed.ptr == args[2].data() + args[2].size();
        if (accepted) { projector.order = value; }
    } else if (field == "texture" && args.size() == 3) {
        const auto* images = kernel.services().find<ImageService>();
        if (args[2] == "none") { projector.texture.texture = {}; accepted = true; }
        else if (images) {
            if (const auto texture = images->textures().find(args[2])) { projector.texture.texture = *texture; accepted = true; }
        }
    } else if (field == "material" && args.size() == 3) {
        const auto* materials = kernel.services().find<MaterialService>();
        if (args[2] == "none") { projector.material = {}; accepted = true; }
        else if (materials) { projector.material = materials->materials().find(args[2]); accepted = static_cast<bool>(projector.material); }
    }
    if (!accepted) { return {false, "invalid field, arity or value; fields: opacity cutoff volume edge tint lifetime facing distance enabled normals two_sided blend order texture material"}; }
    const bool changed = service.replace(handle, projector);
    return {changed, std::string(describe(service.last_error()))};
}
}

void register_decals(core::Kernel& kernel) {
    if (!kernel.register_module({"render.decals", "Bounded post-lighting box projection and color blending", 1})) { return; }
    auto* scene = kernel.services().find<scene::Scene>();
    if (!scene) { kernel.diagnose(core::Diagnostic::Severity::Error, "render.decals", "Scene service unavailable"); return; }
    auto& service = kernel.services().emplace<DecalService>(*scene);
    auto settings = service.settings();
    settings.enabled = kernel.config().boolean("render.decals.enabled", true);
    (void)service.configure(settings);
    (void)kernel.add_system({"render.decals.age", core::Phase::PostSimulation, 80, {}},
        [](core::Kernel& host, const core::Frame& frame) { host.services().get<DecalService>().advance(frame.delta_seconds); });
    (void)kernel.add_system({"render.decals.install", core::Phase::Render, -55, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& current = host.services().get<DecalService>();
            const auto previous = current.attachment_error();
            const auto outcome = current.install_graph(host);
            if (outcome != DecalError::None && outcome != previous) {
                host.diagnose(core::Diagnostic::Severity::Warning, "render.decals", std::string(describe(outcome)));
            }
        });
    (void)kernel.order_after("render.decals.install", "render.postprocess.install");
    (void)kernel.order_after("render.decals.install", "render.visibility.install");
    (void)kernel.order_after("render.graph.frame", "render.decals.install");
    (void)kernel.add_shutdown("render.decals.stop", [pointer = &service] { pointer->stop(); });
    (void)service.install_graph(kernel);
    (void)kernel.add_command("render.decals.set", edit_projector, "Edit projector color, texture, volume, fades or blend mode");
    (void)kernel.add_command("render.decals.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 7) { return {false, "usage: render.decals.create NAME X Y Z HALF_X HALF_Y HALF_Z"}; }
        const auto position = parse_vector(args, 1);
        const auto extent = parse_vector(args, 4);
        if (!position || !extent) { return {false, "expected finite position and half extents"}; }
        math::Transform transform; transform.position = *position;
        DecalProjector projector; projector.half_extent = *extent;
        auto& current = host.services().get<DecalService>();
        const auto handle = current.create(args[0], transform, projector);
        return {static_cast<bool>(handle), handle ? ecs::format_entity(current.entity(handle)) : std::string(describe(current.last_error()))};
    }, "Create a Scene projector facing local -Z at a runtime position");
    for (const std::string operation : {"attach", "remove", "restart"}) {
        (void)kernel.add_command("render.decals." + operation, [operation](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
            if (args.size() != 1) { return {false, "expected one entity name or index:generation"}; }
            auto& current = host.services().get<DecalService>();
            const auto entity = command_entity(host.services().get<scene::Scene>(), args[0]);
            if (!entity) { return {false, "entity unavailable or ambiguous"}; }
            const bool changed = operation == "attach" ? static_cast<bool>(current.attach(entity))
                : operation == "remove" ? current.erase(current.handle(entity)) : current.restart(current.handle(entity));
            return {changed, changed ? "projector updated" : std::string(describe(current.last_error()))};
        }, "Attach, remove or restart a projector without destroying its Scene entity");
    }
    (void)kernel.add_command("render.decals.place", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4 && args.size() != 5) { return {false, "usage: render.decals.place ENTITY X Y Z [absolute]"}; }
        auto position = parse_vector(args, 1);
        auto& current_scene = host.services().get<scene::Scene>();
        const auto entity = command_entity(current_scene, args[0]);
        if (!position || !host.services().get<DecalService>().handle(entity)) { return {false, "invalid projector or position"}; }
        if (args.size() == 5) {
            if (args[4] != "absolute") { return {false, "fifth argument must be absolute"}; }
            if (const auto* partition = host.services().find<scene::WorldPartition>()) {
                const auto origin = partition->origin();
                if (!origin.consistent) { return {false, "world origin is inconsistent"}; }
                *position -= origin.absolute_origin;
            }
        }
        const auto result = current_scene.set_world_position(entity, *position);
        return {static_cast<bool>(result), std::string(scene::describe(result.error))};
    }, "Move a projector in runtime or WorldPartition absolute coordinates");
    (void)kernel.add_command("render.decals.aim", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4) { return {false, "usage: render.decals.aim ENTITY X Y Z"}; }
        const auto target = parse_vector(args, 1);
        auto& current_scene = host.services().get<scene::Scene>();
        const auto entity = command_entity(current_scene, args[0]);
        if (!target || !host.services().get<DecalService>().handle(entity)) { return {false, "invalid projector or runtime target"}; }
        const auto result = current_scene.look_at(entity, *target);
        return {static_cast<bool>(result), std::string(scene::describe(result.error))};
    }, "Aim local -Z at a runtime world point through Scene");
    (void)kernel.add_command("render.decals.stats", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: render.decals.stats"}; }
        const auto& current = host.services().get<DecalService>();
        const auto& stats = current.statistics();
        std::ostringstream output;
        output << "error=" << describe(current.last_error()) << " graph=" << describe(current.attachment_error())
               << " projectors=" << stats.active_projectors << " jobs=" << stats.jobs << " candidates=" << stats.candidate_pixels
               << " blended=" << stats.blended_pixels << " normal_rejections=" << stats.normal_rejections
               << " scratch=" << stats.scratch_bytes << " completed=" << stats.completed << " rejected=" << stats.rejected;
        return {true, output.str()};
    }, "Inspect bounded work, depth normal rejection and graph attachment");
    (void)kernel.add_command("render.decals.list", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: render.decals.list"}; }
        const auto& current = host.services().get<DecalService>();
        std::ostringstream output;
        for (const auto handle : current.handles()) {
            const auto* value = current.component(handle);
            output << ecs::format_entity(current.entity(handle)) << " order=" << value->projector.order
                   << " opacity=" << value->projector.opacity << " blend=" << describe(value->projector.blend)
                   << " age=" << current.age(handle) << '\n';
        }
        return {true, output.str()};
    }, "List generation-checked Scene projectors and authoring state");
}

} // namespace ge::render
