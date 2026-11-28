#include <ge/render/rasterizer.hpp>
#include <ge/core/runtime.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
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

constexpr std::int64_t subpixel_scale = 256;
constexpr std::int64_t subpixel_half = subpixel_scale / 2;
// Endpoints stay within +/-256,000,000 subpixels. Differences and edge
// products therefore remain below 512,000,000 and 5.25e17 respectively.
constexpr double viewport_limit = 1000000.0;
constexpr double attribute_limit = 1.0e12;
constexpr double clip_limit = 1.0e100;
constexpr double minimum_clip_w = 1.0e-9;
constexpr std::size_t polygon_capacity = 16;

bool within(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool bounded(math::Vec2 value, double limit = attribute_limit) noexcept {
    return within(value.x, -limit, limit) && within(value.y, -limit, limit);
}

bool bounded(math::Vec3 value, double limit = attribute_limit) noexcept {
    return within(value.x, -limit, limit) && within(value.y, -limit, limit) &&
        within(value.z, -limit, limit);
}

bool bounded(math::Vec4 value, double limit = attribute_limit) noexcept {
    return bounded(math::xyz(value), limit) && within(value.w, -limit, limit);
}

bool valid_rect(Rect rectangle) noexcept {
    return rectangle.width >= 0 && rectangle.height >= 0;
}

bool valid_depth(DepthState state) noexcept {
    return state.compare <= DepthCompare::Always;
}

bool affine(const math::Mat4& matrix) noexcept {
    return math::finite(matrix) && matrix(3, 0) == 0.0 && matrix(3, 1) == 0.0 &&
        matrix(3, 2) == 0.0 && matrix(3, 3) == 1.0;
}

math::Vec3 unit(math::Vec3 value, math::Vec3 fallback) noexcept {
    const double scale = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
    if (!std::isfinite(scale) || scale == 0.0) {
        return fallback;
    }
    value /= scale;
    return value / std::sqrt(math::dot(value, value));
}

bool depth_passes(double incoming, double stored, DepthCompare compare) noexcept {
    switch (compare) {
    case DepthCompare::Never: return false;
    case DepthCompare::Less: return incoming < stored;
    case DepthCompare::LessEqual: return incoming <= stored;
    case DepthCompare::Equal: return incoming == stored;
    case DepthCompare::GreaterEqual: return incoming >= stored;
    case DepthCompare::Greater: return incoming > stored;
    case DepthCompare::NotEqual: return incoming != stored;
    case DepthCompare::Always: return true;
    }
    return false;
}

struct ClipVertex {
    math::Vec4 clip{};
    math::Vec3 position{};
    math::Vec3 normal{};
    math::Vec4 tangent{};
    math::Vec2 uv{};
    math::Vec4 color{};
};

struct ClipPolygon {
    std::array<ClipVertex, polygon_capacity> vertices{};
    std::size_t size{};
    bool changed{};
    bool invalid{};
};

struct ScreenVertex {
    ClipVertex attributes{};
    std::int64_t x{};
    std::int64_t y{};
    double inverse_w{};
    double depth{};
};

struct PreparedDraw {
    RasterDraw source{};
    const PbrMaterial* material{};
    math::Mat3 normal_matrix{};
    math::Mat3 linear_matrix{};
    std::size_t count{};
    std::size_t ordinal{};
    double tangent_sign{1.0};
    double sort_depth{};
    bool blended{};
};

struct PreparedView {
    CameraView camera{};
    ShadingContext lighting{};
    Rect rectangle{};
    std::vector<PreparedDraw> draws;
};

double plane_distance(math::Vec4 value, std::size_t plane, math::ClipDepth depth) noexcept {
    switch (plane) {
    case 0: return value.w - minimum_clip_w;
    case 1: return value.x + value.w;
    case 2: return value.w - value.x;
    case 3: return value.y + value.w;
    case 4: return value.w - value.y;
    case 5: return depth == math::ClipDepth::zero_to_one ? value.z : value.z + value.w;
    default: return value.w - value.z;
    }
}

ClipVertex interpolate_vertex(const ClipVertex& first, const ClipVertex& second,
                               double amount) noexcept {
    ClipVertex result;
    result.clip = math::lerp(first.clip, second.clip, amount);
    result.position = math::lerp(first.position, second.position, amount);
    result.normal = math::lerp(first.normal, second.normal, amount);
    result.tangent = math::lerp(first.tangent, second.tangent, amount);
    result.uv = math::lerp(first.uv, second.uv, amount);
    result.color = math::lerp(first.color, second.color, amount);
    return result;
}

void snap_to_plane(ClipVertex& vertex, std::size_t plane, math::ClipDepth depth) noexcept {
    switch (plane) {
    case 0: vertex.clip.w = minimum_clip_w; break;
    case 1: vertex.clip.x = -vertex.clip.w; break;
    case 2: vertex.clip.x = vertex.clip.w; break;
    case 3: vertex.clip.y = -vertex.clip.w; break;
    case 4: vertex.clip.y = vertex.clip.w; break;
    case 5:
        vertex.clip.z = depth == math::ClipDepth::zero_to_one ? 0.0 : -vertex.clip.w;
        break;
    default: vertex.clip.z = vertex.clip.w; break;
    }
}

ClipPolygon clip_triangle(const std::array<ClipVertex, 3>& triangle,
                          math::ClipDepth depth) noexcept {
    ClipPolygon polygon;
    std::copy(triangle.begin(), triangle.end(), polygon.vertices.begin());
    polygon.size = triangle.size();
    for (std::size_t plane = 0; plane < 7 && polygon.size != 0; ++plane) {
        ClipPolygon output;
        output.changed = polygon.changed;
        auto previous = polygon.vertices[polygon.size - 1];
        double previous_distance = plane_distance(previous.clip, plane, depth);
        bool previous_inside = previous_distance >= 0.0;
        for (std::size_t index = 0; index < polygon.size; ++index) {
            const auto& current = polygon.vertices[index];
            const double current_distance = plane_distance(current.clip, plane, depth);
            const bool current_inside = current_distance >= 0.0;
            if (previous_inside != current_inside) {
                // Shared edges use the same arithmetic order in either triangle.
                const ClipVertex* first = &previous;
                const ClipVertex* second = &current;
                double first_distance = previous_distance;
                double second_distance = current_distance;
                for (std::size_t component = 0; component < 4; ++component) {
                    if (first->clip[component] == second->clip[component]) {
                        continue;
                    }
                    if (first->clip[component] > second->clip[component]) {
                        std::swap(first, second);
                        std::swap(first_distance, second_distance);
                    }
                    break;
                }
                const double divisor = first_distance - second_distance;
                const double amount = first_distance / divisor;
                if (!std::isfinite(amount) || output.size == polygon_capacity) {
                    output.invalid = true;
                    return output;
                }
                auto crossing = interpolate_vertex(*first, *second, std::clamp(amount, 0.0, 1.0));
                snap_to_plane(crossing, plane, depth);
                output.vertices[output.size++] = crossing;
                output.changed = true;
            }
            if (current_inside) {
                if (output.size == polygon_capacity) {
                    output.invalid = true;
                    return output;
                }
                output.vertices[output.size++] = current;
            } else {
                output.changed = true;
            }
            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }
        polygon = output;
    }
    return polygon;
}

bool transform_vertex(const geometry::Vertex& source, const PreparedDraw& draw,
                       const CameraView& view, ClipVertex& destination) noexcept {
    destination.position = math::transform_point(draw.source.model, source.position);
    const auto transformed_normal = draw.normal_matrix * source.normal;
    const auto transformed_tangent = draw.linear_matrix * math::xyz(source.tangent);
    if (!math::finite(transformed_normal) || !math::finite(transformed_tangent)) {
        return false;
    }
    destination.normal = unit(transformed_normal, {0.0, 1.0, 0.0});
    const auto tangent = unit(transformed_tangent, {1.0, 0.0, 0.0});
    destination.tangent = math::homogeneous(tangent,
        (source.tangent.w < 0.0 ? -1.0 : 1.0) * draw.tangent_sign);
    destination.uv = source.uv;
    destination.color = source.color;
    destination.clip = view.view_projection * math::homogeneous(destination.position);
    return bounded(destination.position) && bounded(destination.clip, clip_limit);
}

bool project_vertex(const ClipVertex& source, const CameraView& view,
                     ScreenVertex& destination) noexcept {
    if (!bounded(source.clip, clip_limit) || source.clip.w < minimum_clip_w) {
        return false;
    }
    destination.attributes = source;
    destination.inverse_w = 1.0 / source.clip.w;
    const double ndc_x = std::clamp(source.clip.x * destination.inverse_w, -1.0, 1.0);
    const double ndc_y = std::clamp(source.clip.y * destination.inverse_w, -1.0, 1.0);
    const double x = static_cast<double>(view.viewport.x) +
        (ndc_x * 0.5 + 0.5) * static_cast<double>(view.viewport.width);
    const double y = static_cast<double>(view.viewport.y) +
        (0.5 - ndc_y * 0.5) * static_cast<double>(view.viewport.height);
    if (!within(x, -viewport_limit, viewport_limit) || !within(y, -viewport_limit, viewport_limit)) {
        return false;
    }
    destination.x = static_cast<std::int64_t>(std::llround(x * static_cast<double>(subpixel_scale)));
    destination.y = static_cast<std::int64_t>(std::llround(y * static_cast<double>(subpixel_scale)));
    destination.depth = source.clip.z * destination.inverse_w;
    if (view.clip_depth == math::ClipDepth::negative_one_to_one) {
        destination.depth = destination.depth * 0.5 + 0.5;
    }
    return within(destination.depth, -1.0e-8, 1.0 + 1.0e-8);
}

std::int64_t edge(const ScreenVertex& first, const ScreenVertex& second,
                  std::int64_t x, std::int64_t y) noexcept {
    return (second.x - first.x) * (y - first.y) -
        (second.y - first.y) * (x - first.x);
}

bool top_left(const ScreenVertex& first, const ScreenVertex& second) noexcept {
    const auto dy = second.y - first.y;
    return dy < 0 || (dy == 0 && second.x > first.x);
}

bool edge_inside(std::int64_t value, bool inclusive) noexcept {
    return value > 0 || (value == 0 && inclusive);
}

std::int64_t floor_divide(std::int64_t value) noexcept {
    const auto quotient = value / subpixel_scale;
    return value % subpixel_scale < 0 ? quotient - 1 : quotient;
}

std::int64_t ceil_divide(std::int64_t value) noexcept {
    return -floor_divide(-value);
}

Rect triangle_bounds(const std::array<ScreenVertex, 3>& vertices, Rect clip) noexcept {
    const auto min_x = std::min({vertices[0].x, vertices[1].x, vertices[2].x});
    const auto min_y = std::min({vertices[0].y, vertices[1].y, vertices[2].y});
    const auto max_x = std::max({vertices[0].x, vertices[1].x, vertices[2].x});
    const auto max_y = std::max({vertices[0].y, vertices[1].y, vertices[2].y});
    const auto left = std::max(ceil_divide(min_x - subpixel_half), static_cast<std::int64_t>(clip.x));
    const auto top = std::max(ceil_divide(min_y - subpixel_half), static_cast<std::int64_t>(clip.y));
    const auto right = std::min(floor_divide(max_x - subpixel_half) + 1,
        static_cast<std::int64_t>(clip.x) + clip.width);
    const auto bottom = std::min(floor_divide(max_y - subpixel_half) + 1,
        static_cast<std::int64_t>(clip.y) + clip.height);
    if (left >= right || top >= bottom) {
        return {};
    }
    return {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(right - left), static_cast<std::int32_t>(bottom - top)};
}

bool valid_vertex(const geometry::Vertex& vertex) noexcept {
    return bounded(vertex.position) && bounded(vertex.normal) && bounded(vertex.tangent) &&
        bounded(vertex.uv) && math::finite(vertex.color) &&
        within(vertex.color.x, 0.0, 1.0) && within(vertex.color.y, 0.0, 1.0) &&
        within(vertex.color.z, 0.0, 1.0) && within(vertex.color.w, 0.0, 1.0);
}

bool prepare_draw(const RasterDraw& source, PreparedDraw& destination) noexcept {
    static const PbrMaterial fallback;
    if (!source.mesh || source.mesh->vertices.empty() || !affine(source.model) ||
        source.first_index > source.mesh->indices.size() || source.first_index % 3 != 0 ||
        (source.scissor && !valid_rect(*source.scissor)) ||
        (source.depth && !valid_depth(*source.depth)) ||
        (source.cull && *source.cull > CullMode::Front) ||
        (source.fill && *source.fill > FillMode::Wireframe)) {
        return false;
    }
    const auto available = source.mesh->indices.size() - source.first_index;
    const auto count = source.index_count == 0 ? available : source.index_count;
    const auto* material = source.material ? source.material : &fallback;
    if (count > available || count % 3 != 0 || !validate_material(*material)) {
        return false;
    }
    const auto normals = math::normal_matrix(source.model);
    if (!normals || !math::finite(*normals)) {
        return false;
    }
    destination.source = source;
    destination.source.material = material;
    destination.material = material;
    destination.normal_matrix = *normals;
    destination.linear_matrix = math::upper3x3(source.model);
    const double determinant = math::determinant(destination.linear_matrix);
    if (!std::isfinite(determinant) || determinant == 0.0) {
        return false;
    }
    destination.tangent_sign = determinant < 0.0 ? -1.0 : 1.0;
    destination.count = count;
    destination.blended = material->alpha_mode == AlphaMode::Blend;
    return true;
}

bool validate_vertices(const PreparedDraw& draw, const CameraView& view, math::Vec3& centre) noexcept {
    math::Vec3 minimum{math::infinity, math::infinity, math::infinity};
    math::Vec3 maximum{-math::infinity, -math::infinity, -math::infinity};
    const auto& mesh = *draw.source.mesh;
    for (std::size_t index = 0; index < draw.count; ++index) {
        const auto vertex_index = static_cast<std::size_t>(mesh.indices[draw.source.first_index + index]);
        if (vertex_index >= mesh.vertices.size() || !valid_vertex(mesh.vertices[vertex_index])) {
            return false;
        }
        ClipVertex transformed;
        if (!transform_vertex(mesh.vertices[vertex_index], draw, view, transformed)) {
            return false;
        }
        minimum = math::min(minimum, mesh.vertices[vertex_index].position);
        maximum = math::max(maximum, mesh.vertices[vertex_index].position);
    }
    centre = draw.count == 0 ? math::Vec3{} : minimum * 0.5 + maximum * 0.5;
    return true;
}

bool valid_lighting(const ShadingContext& lighting) noexcept {
    return finite(lighting.ambient) && within(lighting.ambient.r, 0.0, material_hdr_limit) &&
        within(lighting.ambient.g, 0.0, material_hdr_limit) &&
        within(lighting.ambient.b, 0.0, material_hdr_limit) && within(lighting.ambient.a, 0.0, 1.0);
}

bool valid_view(const CameraView& view, bool allow_stale) noexcept {
    const double right = static_cast<double>(view.viewport.x) + view.viewport.width;
    const double bottom = static_cast<double>(view.viewport.y) + view.viewport.height;
    return !view.viewport.empty() && within(view.viewport.x, -viewport_limit, viewport_limit) &&
        within(view.viewport.y, -viewport_limit, viewport_limit) &&
        within(right, -viewport_limit, viewport_limit) && within(bottom, -viewport_limit, viewport_limit) &&
        math::finite(view.view_projection) && math::finite(view.view) &&
        bounded(view.position) && bounded(view.forward) &&
        (view.clip_depth == math::ClipDepth::zero_to_one ||
         view.clip_depth == math::ClipDepth::negative_one_to_one) &&
        (view.projection_mode == ProjectionMode::Perspective ||
         view.projection_mode == ProjectionMode::Orthographic) && (allow_stale || !view.stale);
}

std::size_t clear_rectangle(Framebuffer& target, Rect rectangle,
                           const RasterizerSettings& settings) noexcept {
    if (rectangle.empty() || (!settings.clear_color && !settings.clear_depth)) {
        return 0;
    }
    if (settings.clear_color && settings.clear_depth) {
        return target.clear_region(rectangle, settings.background, settings.background_depth);
    }
    if (settings.clear_color) {
        return target.color().fill_rect(rectangle, settings.background);
    }
    if (!target.has_depth()) {
        return 0;
    }
    auto depths = target.depth();
    for (std::int32_t row = 0; row < rectangle.height; ++row) {
        const auto offset = static_cast<std::size_t>(rectangle.y + row) * target.width() +
            static_cast<std::size_t>(rectangle.x);
        for (std::int32_t column = 0; column < rectangle.width; ++column) {
            depths[offset + static_cast<std::size_t>(column)] = settings.background_depth;
        }
    }
    return static_cast<std::size_t>(rectangle.width) * static_cast<std::size_t>(rectangle.height);
}

struct TriangleGradients {
    std::array<double, 3> dx{};
    std::array<double, 3> dy{};
    std::array<double, 3> edge_lengths{};
    double depth_slope{};
};

TriangleGradients gradients(const std::array<ScreenVertex, 3>& vertices, double inverse_area) noexcept {
    TriangleGradients result;
    double depth_dx{};
    double depth_dy{};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto& first = vertices[(index + 1) % 3];
        const auto& second = vertices[(index + 2) % 3];
        result.dx[index] = -static_cast<double>(second.y - first.y) *
            static_cast<double>(subpixel_scale) * inverse_area;
        result.dy[index] = static_cast<double>(second.x - first.x) *
            static_cast<double>(subpixel_scale) * inverse_area;
        result.edge_lengths[index] = std::hypot(static_cast<double>(second.x - first.x),
                                                static_cast<double>(second.y - first.y));
        depth_dx += result.dx[index] * vertices[index].depth;
        depth_dy += result.dy[index] * vertices[index].depth;
    }
    result.depth_slope = std::max(std::abs(depth_dx), std::abs(depth_dy));
    return result;
}

bool interpolate_surface(const std::array<ScreenVertex, 3>& vertices,
                          const std::array<double, 3>& barycentric,
                          const TriangleGradients& derivatives, bool front,
                          double lod_bias, SurfaceSample& surface) noexcept {
    std::array<double, 3> weights{};
    double divisor{};
    double divisor_dx{};
    double divisor_dy{};
    math::Vec2 uv_numerator{};
    math::Vec2 uv_numerator_dx{};
    math::Vec2 uv_numerator_dy{};
    for (std::size_t index = 0; index < 3; ++index) {
        weights[index] = barycentric[index] * vertices[index].inverse_w;
        divisor += weights[index];
        divisor_dx += derivatives.dx[index] * vertices[index].inverse_w;
        divisor_dy += derivatives.dy[index] * vertices[index].inverse_w;
        uv_numerator += vertices[index].attributes.uv * weights[index];
        uv_numerator_dx += vertices[index].attributes.uv *
            (derivatives.dx[index] * vertices[index].inverse_w);
        uv_numerator_dy += vertices[index].attributes.uv *
            (derivatives.dy[index] * vertices[index].inverse_w);
    }
    if (!std::isfinite(divisor) || divisor <= 0.0) {
        return false;
    }
    math::Vec3 normal{};
    math::Vec4 tangent{};
    math::Vec4 color{};
    for (std::size_t index = 0; index < 3; ++index) {
        const double weight = weights[index] / divisor;
        const auto& attributes = vertices[index].attributes;
        surface.world_position += attributes.position * weight;
        normal += attributes.normal * weight;
        tangent += attributes.tangent * weight;
        color += attributes.color * weight;
    }
    surface.world_normal = unit(normal, {0.0, 1.0, 0.0});
    const auto tangent_direction = math::xyz(tangent) - surface.world_normal *
        math::dot(surface.world_normal, math::xyz(tangent));
    const auto basis = math::orthonormal_basis(surface.world_normal);
    surface.world_tangent = math::homogeneous(unit(tangent_direction, basis.first),
                                              tangent.w < 0.0 ? -1.0 : 1.0);
    surface.vertex_color = clamped(Color::from_vector(color));
    surface.uv = uv_numerator / divisor;
    surface.lod = lod_bias;
    surface.front_facing = front;
    const double gradient_scale = std::exp2(lod_bias) / divisor;
    const auto uv_dx = (uv_numerator_dx - surface.uv * divisor_dx) * gradient_scale;
    const auto uv_dy = (uv_numerator_dy - surface.uv * divisor_dy) * gradient_scale;
    surface.gradients_valid = bounded(uv_dx) && bounded(uv_dy);
    if (surface.gradients_valid) {
        surface.uv_dx = uv_dx;
        surface.uv_dy = uv_dy;
    }
    return bounded(surface.world_position) && bounded(surface.uv) && finite(surface.vertex_color);
}

} // namespace

bool valid(const RasterizerLimits& limits) noexcept {
    return limits.max_draws > 0 && limits.max_draws <= 65536 &&
        limits.max_scene_entities > 0 && limits.max_scene_entities <= 1048576 &&
        limits.max_hierarchy_depth > 0 && limits.max_hierarchy_depth <= 4096 &&
        limits.max_views > 0 && limits.max_views <= 64 &&
        limits.max_input_triangles > 0 && limits.max_input_triangles <= 4194304 &&
        limits.max_raster_triangles > 0 && limits.max_raster_triangles <= 16777216 &&
        limits.max_fragment_visits > 0 && limits.max_fragment_visits <= 268435456 &&
        limits.max_light_visits <= 1073741824 && limits.max_lights_per_fragment <= material_light_limit &&
        limits.max_framebuffer_pixels > 0 && limits.max_framebuffer_pixels <= 67108864;
}

bool valid(const RasterizerSettings& settings) noexcept {
    return settings.cull <= CullMode::Front && settings.fill <= FillMode::Wireframe &&
        valid_depth(settings.depth) && finite(settings.background) &&
        within(settings.background.r, 0.0, material_hdr_limit) &&
        within(settings.background.g, 0.0, material_hdr_limit) &&
        within(settings.background.b, 0.0, material_hdr_limit) && within(settings.background.a, 0.0, 1.0) &&
        within(settings.background_depth, 0.0, 1.0) && within(settings.depth_bias, -1.0, 1.0) &&
        within(settings.slope_depth_bias, -16.0, 16.0) && within(settings.wire_width, 0.1, 64.0) &&
        within(settings.texture_lod_bias, -32.0, 32.0) &&
        (!settings.scissor || valid_rect(*settings.scissor));
}

std::string_view describe(RasterError error) noexcept {
    switch (error) {
    case RasterError::None: return "render completed";
    case RasterError::InvalidConfiguration: return "invalid rasterizer configuration";
    case RasterError::InvalidFramebuffer: return "invalid or oversized framebuffer";
    case RasterError::InvalidView: return "missing, stale or unsupported camera view";
    case RasterError::InvalidDraw: return "invalid draw geometry, material or transform";
    case RasterError::MissingService: return "required render service is unavailable";
    case RasterError::QueueCapacity: return "draw queue capacity exceeded";
    case RasterError::SceneRejected: return "scene synchronization or hierarchy rejected";
    case RasterError::AllocationFailure: return "render preparation allocation failed";
    case RasterError::WorkLimit: return "render work limit reached";
    }
    return "unknown rasterizer error";
}

struct SoftwareRenderer::Impl {
    explicit Impl(RasterizerLimits configuration) : limits(configuration) {
        if (!valid(limits)) {
            throw std::invalid_argument("invalid software rasterizer limits");
        }
    }
    RasterizerLimits limits;
    RasterizerSettings settings;
    RasterStatistics statistics;
    RasterError error{RasterError::None};
    std::vector<RasterDraw> queue;
    std::uint64_t serial{};
    ShadowFactor shadow_provider{};
    const void* shadow_user{};
    FragmentShader fragment_shader{};
    const void* fragment_user{};

    void begin() noexcept {
        if (serial != std::numeric_limits<std::uint64_t>::max()) {
            ++serial;
        }
        statistics = {};
        statistics.frame = serial;
        error = RasterError::None;
    }

    RasterResult result(RasterError outcome) noexcept {
        error = outcome;
        statistics.work_limited = statistics.work_limited || outcome == RasterError::WorkLimit;
        return {outcome, statistics.cleared_pixels != 0 || statistics.fragments_written != 0};
    }

    bool target_valid(const Framebuffer& target) const noexcept {
        return target.valid() && target.width() > 0 && target.height() > 0 &&
            target.width() <= 1000000 && target.height() <= 1000000 &&
            target.color().size() <= limits.max_framebuffer_pixels;
    }

    RasterError prepare(const Framebuffer& target, const CameraView& camera,
                        std::span<const RasterDraw> draws, const ShadingContext& lighting,
                        std::size_t& triangle_reservation, PreparedView& output) {
        if (!target_valid(target)) {
            return RasterError::InvalidFramebuffer;
        }
        if (!valid_view(camera, settings.allow_stale_views)) {
            return RasterError::InvalidView;
        }
        if (!valid_lighting(lighting)) {
            return RasterError::InvalidConfiguration;
        }
        if (draws.size() > limits.max_draws) {
            return RasterError::QueueCapacity;
        }
        output.camera = camera;
        output.lighting = lighting;
        output.rectangle = intersection(camera.viewport, target.color().bounds());
        if (settings.scissor) {
            output.rectangle = intersection(output.rectangle, *settings.scissor);
        }
        output.draws.reserve(draws.size());
        for (std::size_t index = 0; index < draws.size(); ++index) {
            const auto& source = draws[index];
            if ((source.visibility_mask & camera.visibility_mask) == 0) {
                continue;
            }
            PreparedDraw draw;
            if (!prepare_draw(source, draw)) {
                ++statistics.draws_rejected;
                return RasterError::InvalidDraw;
            }
            const auto triangles = draw.count / 3;
            if (triangles > limits.max_input_triangles - triangle_reservation) {
                return RasterError::WorkLimit;
            }
            triangle_reservation += triangles;
            math::Vec3 centre;
            if (!validate_vertices(draw, camera, centre)) {
                ++statistics.draws_rejected;
                return RasterError::InvalidDraw;
            }
            const auto world_centre = math::transform_point(source.model, centre);
            const auto eye_centre = camera.view * math::homogeneous(world_centre);
            if (!bounded(world_centre) || !math::finite(eye_centre)) {
                ++statistics.draws_rejected;
                return RasterError::InvalidDraw;
            }
            draw.sort_depth = -eye_centre.z;
            draw.ordinal = index;
            output.draws.push_back(draw);
        }
        if (settings.sort_transparency) {
            std::sort(output.draws.begin(), output.draws.end(), [](const auto& first, const auto& second) {
                if (first.blended != second.blended) {
                    return !first.blended;
                }
                if (first.blended && first.sort_depth != second.sort_depth) {
                    return first.sort_depth > second.sort_depth;
                }
                return first.ordinal < second.ordinal;
            });
        }
        return RasterError::None;
    }

    bool shade_pixel(Framebuffer& target, const PreparedView& view, const PreparedDraw& draw,
                     const std::array<ScreenVertex, 3>& vertices,
                     const std::array<double, 3>& barycentric,
                     const TriangleGradients& derivatives, bool front,
                     std::int32_t x, std::int32_t y, const TextureLibrary* textures) noexcept {
        double depth{};
        for (std::size_t index = 0; index < 3; ++index) {
            depth += barycentric[index] * vertices[index].depth;
        }
        depth = std::clamp(depth + settings.depth_bias +
            settings.slope_depth_bias * derivatives.depth_slope, 0.0, 1.0);
        auto state = draw.source.depth.value_or(settings.depth);
        if (draw.blended && !settings.blended_depth_write) {
            state.write_enabled = false;
        }
        if (state.test_enabled && target.has_depth()) {
            const auto stored = target.depth_at(x, y);
            const double previous = stored && std::isfinite(*stored) ? std::clamp(*stored, 0.0, 1.0) : 1.0;
            if (!depth_passes(depth, previous, state.compare)) {
                ++statistics.fragments_depth_rejected;
                return true;
            }
        }
        SurfaceSample surface;
        if (!interpolate_surface(vertices, barycentric, derivatives, front, settings.texture_lod_bias, surface)) {
            ++statistics.fragments_invalid;
            return true;
        }
        auto lighting = view.lighting;
        lighting.view_direction = view.camera.projection_mode == ProjectionMode::Orthographic ?
            -view.camera.forward : view.camera.position - surface.world_position;
        lighting.view_direction = unit(lighting.view_direction, -unit(view.camera.forward, {0.0, 0.0, -1.0}));
        lighting.light_mask &= draw.source.light_mask;
        lighting.max_lights = std::min({lighting.max_lights, limits.max_lights_per_fragment,
                                        lighting.lights.size(), material_light_limit});
        lighting.lights = lighting.lights.first(lighting.max_lights);
        const auto light_work = !fragment_shader && draw.material->shading_model == ShadingModel::Unlit ?
            std::size_t{0} : lighting.max_lights;
        if (light_work > limits.max_light_visits - statistics.light_visits) {
            return false;
        }
        statistics.light_visits += light_work;
        const auto shaded = fragment_shader ?
            fragment_shader(draw.source, surface, lighting, textures, fragment_user) :
            shade_material(*draw.material, surface, lighting, textures);
        ++statistics.fragments_shaded;
        statistics.lights_evaluated += std::min(shaded.lights_evaluated, light_work);
        const auto missing = static_cast<std::size_t>(shaded.missing_textures);
        statistics.missing_texture_samples += std::min(missing,
            std::numeric_limits<std::size_t>::max() - statistics.missing_texture_samples);
        if (!shaded.valid || !finite(shaded.color) ||
            !within(shaded.color.r, 0.0, material_hdr_limit) ||
            !within(shaded.color.g, 0.0, material_hdr_limit) ||
            !within(shaded.color.b, 0.0, material_hdr_limit) || !within(shaded.color.a, 0.0, 1.0)) {
            ++statistics.fragments_invalid;
            return true;
        }
        if (shaded.discarded) {
            ++statistics.fragments_discarded;
            return true;
        }
        if (target.write(x, y, depth, shaded.color, state,
                         draw.blended ? BlendMode::SourceOver : BlendMode::Replace)) {
            ++statistics.fragments_written;
            if (draw.blended) {
                ++statistics.blended_fragments;
            }
        } else {
            ++statistics.fragments_invalid;
        }
        return true;
    }

    bool raster_triangle(Framebuffer& target, const PreparedView& view, const PreparedDraw& draw,
                          std::array<ScreenVertex, 3> vertices, std::array<bool, 3> boundary,
                          const TextureLibrary* textures) noexcept {
        auto area = edge(vertices[0], vertices[1], vertices[2].x, vertices[2].y);
        if (area == 0) {
            ++statistics.triangles_degenerate;
            return true;
        }
        const bool front = settings.front_counter_clockwise ? area < 0 : area > 0;
        const auto cull = draw.source.cull.value_or(settings.cull);
        if ((cull == CullMode::Front && front) ||
            (cull == CullMode::Back && !front && !draw.material->double_sided)) {
            ++statistics.triangles_culled;
            return true;
        }
        if (area < 0) {
            std::swap(vertices[1], vertices[2]);
            std::swap(boundary[1], boundary[2]);
            area = -area;
        }
        Rect rectangle = view.rectangle;
        if (draw.source.scissor) {
            rectangle = intersection(rectangle, *draw.source.scissor);
        }
        rectangle = triangle_bounds(vertices, rectangle);
        if (rectangle.empty()) {
            return true;
        }
        const double inverse_area = 1.0 / static_cast<double>(area);
        const auto derivatives = gradients(vertices, inverse_area);
        const auto fill = draw.source.fill.value_or(settings.fill);
        std::array<std::int64_t, 3> row_edges{};
        std::array<std::int64_t, 3> step_x{};
        std::array<std::int64_t, 3> step_y{};
        std::array<bool, 3> inclusive{};
        const auto first_x = static_cast<std::int64_t>(rectangle.x) * subpixel_scale + subpixel_half;
        const auto first_y = static_cast<std::int64_t>(rectangle.y) * subpixel_scale + subpixel_half;
        for (std::size_t index = 0; index < 3; ++index) {
            const auto& first = vertices[(index + 1) % 3];
            const auto& second = vertices[(index + 2) % 3];
            row_edges[index] = edge(first, second, first_x, first_y);
            step_x[index] = -(second.y - first.y) * subpixel_scale;
            step_y[index] = (second.x - first.x) * subpixel_scale;
            inclusive[index] = top_left(first, second);
        }
        for (std::int32_t row = 0; row < rectangle.height; ++row) {
            auto edges = row_edges;
            for (std::int32_t column = 0; column < rectangle.width; ++column) {
                if (statistics.fragment_visits == limits.max_fragment_visits) {
                    return false;
                }
                ++statistics.fragment_visits;
                bool covered = true;
                bool wire_edge = false;
                std::array<double, 3> barycentric{};
                for (std::size_t index = 0; index < 3; ++index) {
                    covered = covered && edge_inside(edges[index], inclusive[index]);
                    barycentric[index] = static_cast<double>(edges[index]) * inverse_area;
                    const double wire_threshold = derivatives.edge_lengths[index] *
                        static_cast<double>(subpixel_scale) * settings.wire_width * 0.5;
                    wire_edge = wire_edge || (boundary[index] && static_cast<double>(edges[index]) <= wire_threshold);
                }
                if (covered && (fill == FillMode::Solid || wire_edge)) {
                    ++statistics.fragments_covered;
                    if (!shade_pixel(target, view, draw, vertices, barycentric, derivatives, front,
                                     rectangle.x + column, rectangle.y + row, textures)) {
                        return false;
                    }
                }
                for (std::size_t index = 0; index < 3; ++index) {
                    edges[index] += step_x[index];
                }
            }
            for (std::size_t index = 0; index < 3; ++index) {
                row_edges[index] += step_y[index];
            }
        }
        return true;
    }

    bool render_draw(Framebuffer& target, const PreparedView& view, const PreparedDraw& draw,
                      const TextureLibrary* textures) noexcept {
        ++statistics.draws_rendered;
        const auto& mesh = *draw.source.mesh;
        for (std::size_t index = 0; index < draw.count; index += 3) {
            if (statistics.input_triangles == limits.max_input_triangles) {
                return false;
            }
            ++statistics.input_triangles;
            std::array<ClipVertex, 3> triangle{};
            bool transformed = true;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto vertex_index = mesh.indices[draw.source.first_index + index + corner];
                transformed = transform_vertex(mesh.vertices[vertex_index], draw, view.camera, triangle[corner]) && transformed;
                ++statistics.vertices_transformed;
            }
            if (!transformed) {
                ++statistics.triangles_invalid;
                continue;
            }
            const auto polygon = clip_triangle(triangle, view.camera.clip_depth);
            if (polygon.invalid) {
                ++statistics.triangles_invalid;
                continue;
            }
            if (polygon.changed) {
                ++statistics.triangles_clipped;
            }
            if (polygon.size < 3) {
                continue;
            }
            std::array<ScreenVertex, polygon_capacity> screen{};
            bool projected = true;
            for (std::size_t corner = 0; corner < polygon.size; ++corner) {
                projected = project_vertex(polygon.vertices[corner], view.camera, screen[corner]) && projected;
            }
            if (!projected) {
                ++statistics.triangles_invalid;
                continue;
            }
            for (std::size_t corner = 1; corner + 1 < polygon.size; ++corner) {
                if (statistics.raster_triangles == limits.max_raster_triangles) {
                    return false;
                }
                ++statistics.raster_triangles;
                const std::array<ScreenVertex, 3> fan{screen[0], screen[corner], screen[corner + 1]};
                const std::array<bool, 3> boundary{true, corner + 2 == polygon.size, corner == 1};
                if (!raster_triangle(target, view, draw, fan, boundary, textures)) {
                    return false;
                }
            }
        }
        return true;
    }

    RasterResult execute(Framebuffer& target, std::span<const PreparedView> views,
                          const TextureLibrary* textures, bool clear) noexcept {
        for (const auto& view : views) {
            ++statistics.views;
            statistics.stale_views += view.camera.stale ? 1U : 0U;
            if (clear) {
                const auto cleared = clear_rectangle(target, view.rectangle, settings);
                statistics.cleared_pixels += std::min(cleared,
                    std::numeric_limits<std::size_t>::max() - statistics.cleared_pixels);
            }
            if (view.rectangle.empty()) {
                continue;
            }
            for (const auto& draw : view.draws) {
                if (!render_draw(target, view, draw, textures)) {
                    return result(RasterError::WorkLimit);
                }
            }
        }
        return result(RasterError::None);
    }

    RasterError collect(scene::Scene& scene, const geometry::MeshLibrary& meshes,
                         const MaterialService& materials, std::vector<RasterDraw>& output) {
        if (scene.registry().size() > limits.max_scene_entities || scene.size() > limits.max_scene_entities) {
            return RasterError::WorkLimit;
        }
        if (scene.update().error != scene::SceneError::None) {
            return RasterError::SceneRejected;
        }
        const auto entities = scene.ordered_entities();
        std::vector<std::pair<ecs::Entity, bool>> ancestors;
        ancestors.reserve(std::min(entities.size(), limits.max_hierarchy_depth));
        output.reserve(std::min(entities.size(), limits.max_draws));
        const auto& registry = scene.registry();
        for (const auto entity : entities) {
            ++statistics.entities_examined;
            const auto parent = scene.parent(entity);
            while (!ancestors.empty() && ancestors.back().first != parent) {
                ancestors.pop_back();
            }
            const bool enabled = registry.enabled(entity) &&
                (!parent || (!ancestors.empty() && ancestors.back().second));
            if (ancestors.size() == limits.max_hierarchy_depth) {
                return RasterError::WorkLimit;
            }
            ancestors.emplace_back(entity, enabled);
            const auto* component = registry.get<geometry::MeshComponent>(entity);
            if (!enabled || !component || !component->visible || registry.layers(entity) == 0) {
                ++statistics.entities_skipped;
                continue;
            }
            const auto* mesh = meshes.get(component->mesh);
            const auto* matrix = scene.cached_world_matrix(entity);
            if (!mesh || !matrix || mesh->empty()) {
                ++statistics.entities_skipped;
                continue;
            }
            const auto range_count = std::max(std::size_t{1}, mesh->submeshes.size());
            const auto first = static_cast<std::size_t>(component->first_submesh);
            if (first >= range_count) {
                ++statistics.entities_skipped;
                continue;
            }
            const auto remaining = range_count - first;
            const auto count = component->submesh_count == 0 ? remaining :
                std::min(remaining, static_cast<std::size_t>(component->submesh_count));
            if (count > limits.max_draws - output.size()) {
                return RasterError::QueueCapacity;
            }
            for (std::size_t range = first; range < first + count; ++range) {
                RasterDraw draw;
                draw.mesh = mesh;
                draw.model = *matrix;
                draw.entity = entity;
                draw.visibility_mask = registry.layers(entity);
                draw.light_mask = registry.layers(entity);
                std::uint32_t material_slot{};
                if (!mesh->submeshes.empty()) {
                    draw.first_index = mesh->submeshes[range].first_index;
                    draw.index_count = mesh->submeshes[range].index_count;
                    material_slot = mesh->submeshes[range].material_slot;
                }
                draw.material = &materials.resolve(registry, entity, material_slot);
                output.push_back(draw);
            }
        }
        return RasterError::None;
    }
};

SoftwareRenderer::SoftwareRenderer(RasterizerLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
SoftwareRenderer::~SoftwareRenderer() = default;

bool SoftwareRenderer::configure(const RasterizerSettings& settings) noexcept {
    if (!valid(settings)) {
        impl_->error = RasterError::InvalidConfiguration;
        return false;
    }
    impl_->settings = settings;
    impl_->error = RasterError::None;
    return true;
}

const RasterizerSettings& SoftwareRenderer::settings() const noexcept { return impl_->settings; }
const RasterizerLimits& SoftwareRenderer::limits() const noexcept { return impl_->limits; }
const RasterStatistics& SoftwareRenderer::statistics() const noexcept { return impl_->statistics; }
RasterError SoftwareRenderer::last_error() const noexcept { return impl_->error; }

void SoftwareRenderer::set_shadow_provider(ShadowFactor provider, const void* user) noexcept {
    impl_->shadow_provider = provider;
    impl_->shadow_user = provider ? user : nullptr;
}

bool SoftwareRenderer::clear_shadow_provider(ShadowFactor provider, const void* user) noexcept {
    if (impl_->shadow_provider != provider || impl_->shadow_user != user) {
        return false;
    }
    impl_->shadow_provider = nullptr;
    impl_->shadow_user = nullptr;
    return true;
}

void SoftwareRenderer::set_fragment_shader(FragmentShader shader, const void* user) noexcept {
    impl_->fragment_shader = shader;
    impl_->fragment_user = shader ? user : nullptr;
}

std::size_t SoftwareRenderer::queued_draws() const noexcept { return impl_->queue.size(); }
std::span<const RasterDraw> SoftwareRenderer::submissions() const noexcept { return impl_->queue; }
void SoftwareRenderer::clear_queue() noexcept { impl_->queue.clear(); }

bool SoftwareRenderer::submit(const RasterDraw& draw) {
    if (impl_->queue.size() == impl_->limits.max_draws) {
        impl_->error = RasterError::QueueCapacity;
        return false;
    }
    PreparedDraw prepared;
    if (!prepare_draw(draw, prepared)) {
        impl_->error = RasterError::InvalidDraw;
        return false;
    }
    if (prepared.count / 3 > impl_->limits.max_input_triangles) {
        impl_->error = RasterError::WorkLimit;
        return false;
    }
    try {
        impl_->queue.push_back(draw);
        impl_->error = RasterError::None;
        return true;
    } catch (const std::bad_alloc&) {
        impl_->error = RasterError::AllocationFailure;
    } catch (const std::length_error&) {
        impl_->error = RasterError::AllocationFailure;
    }
    return false;
}

RasterResult SoftwareRenderer::draw(Framebuffer& target, const CameraView& view,
                                    const RasterDraw& submission, const ShadingContext& lighting,
                                    const TextureLibrary* textures) {
    return draw(target, view, std::span<const RasterDraw>{&submission, 1}, lighting, textures, false);
}

RasterResult SoftwareRenderer::draw(Framebuffer& target, const CameraView& view,
                                    std::span<const RasterDraw> submissions,
                                    const ShadingContext& lighting, const TextureLibrary* textures, bool clear) {
    const RasterViewSubmission submission{view, submissions, lighting};
    return draw_views(target, std::span<const RasterViewSubmission>{&submission, 1}, textures, clear);
}

RasterResult SoftwareRenderer::draw_views(Framebuffer& target,
                                          std::span<const RasterViewSubmission> submissions,
                                          const TextureLibrary* textures, bool clear) {
    impl_->begin();
    if (!impl_->settings.enabled) {
        return impl_->result(RasterError::None);
    }
    if (submissions.empty()) {
        return impl_->result(RasterError::InvalidView);
    }
    if (submissions.size() > impl_->limits.max_views) {
        return impl_->result(RasterError::WorkLimit);
    }
    for (const auto& submission : submissions) {
        if (submission.draws.size() > impl_->limits.max_draws) {
            return impl_->result(RasterError::QueueCapacity);
        }
        if (submission.draws.size() > std::numeric_limits<std::size_t>::max() -
            impl_->statistics.draws_submitted) {
            return impl_->result(RasterError::WorkLimit);
        }
        impl_->statistics.draws_submitted += submission.draws.size();
    }
    try {
        std::vector<PreparedView> prepared(submissions.size());
        std::size_t triangle_reservation{};
        for (std::size_t index = 0; index < submissions.size(); ++index) {
            const auto& submission = submissions[index];
            const auto error = impl_->prepare(target, submission.view, submission.draws,
                submission.lighting, triangle_reservation, prepared[index]);
            if (error != RasterError::None) {
                return impl_->result(error);
            }
        }
        return impl_->execute(target, prepared, textures, clear);
    } catch (const std::bad_alloc&) {
        return impl_->result(RasterError::AllocationFailure);
    } catch (const std::length_error&) {
        return impl_->result(RasterError::AllocationFailure);
    }
}

RasterResult SoftwareRenderer::render(Framebuffer& target, const CameraView& view,
                                      const ShadingContext& lighting, const TextureLibrary* textures) {
    const auto outcome = draw(target, view, impl_->queue, lighting, textures, true);
    if (impl_->settings.enabled && (outcome.error == RasterError::None ||
        (outcome.error == RasterError::WorkLimit && impl_->statistics.views != 0))) {
        impl_->queue.clear();
    }
    return outcome;
}

RasterResult SoftwareRenderer::render_scene(scene::Scene& scene, const geometry::MeshLibrary& meshes,
                                            const CameraService& cameras, const MaterialService& materials,
                                            ImageService& images) {
    impl_->begin();
    if (!impl_->settings.enabled) {
        return impl_->result(RasterError::None);
    }
    try {
        auto& target = images.framebuffer();
        if (!impl_->target_valid(target)) {
            return impl_->result(RasterError::InvalidFramebuffer);
        }
        std::vector<CameraView> cameras_to_render;
        if (impl_->settings.render_all_views) {
            const auto available = cameras.views();
            if (available.size() > impl_->limits.max_views) {
                return impl_->result(RasterError::WorkLimit);
            }
            cameras_to_render.assign(available.begin(), available.end());
        } else if (const auto* current = cameras.current_view()) {
            cameras_to_render.push_back(*current);
        }
        if (cameras_to_render.empty()) {
            return impl_->result(RasterError::InvalidView);
        }
        for (const auto& camera : cameras_to_render) {
            if (!valid_view(camera, impl_->settings.allow_stale_views)) {
                return impl_->result(RasterError::InvalidView);
            }
        }
        std::vector<RasterDraw> draws;
        const auto collected = impl_->collect(scene, meshes, materials, draws);
        if (collected != RasterError::None) {
            return impl_->result(collected);
        }
        impl_->statistics.draws_submitted = draws.size();
        std::vector<PreparedView> prepared(cameras_to_render.size());
        std::size_t triangle_reservation{};
        for (std::size_t index = 0; index < cameras_to_render.size(); ++index) {
            const auto& camera = cameras_to_render[index];
            auto lighting = materials.context(-camera.forward);
            lighting.shadow_factor = impl_->shadow_provider;
            lighting.shadow_user = impl_->shadow_user;
            const auto error = impl_->prepare(target, camera, draws, lighting, triangle_reservation, prepared[index]);
            if (error != RasterError::None) {
                return impl_->result(error);
            }
        }
        return impl_->execute(target, prepared, &images.textures(), true);
    } catch (const std::bad_alloc&) {
        return impl_->result(RasterError::AllocationFailure);
    } catch (const std::length_error&) {
        return impl_->result(RasterError::AllocationFailure);
    }
}

namespace {

template<class Number>
std::optional<Number> number(std::string_view text) noexcept {
    Number value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> boolean(std::string_view text) noexcept {
    if (text == "on" || text == "true" || text == "1") {
        return true;
    }
    if (text == "off" || text == "false" || text == "0") {
        return false;
    }
    return std::nullopt;
}

std::string settings_text(const SoftwareRenderer& renderer) {
    const auto& settings = renderer.settings();
    std::ostringstream stream;
    stream << "enabled=" << settings.enabled << " all_views=" << settings.render_all_views
           << " allow_stale=" << settings.allow_stale_views << " transparency_sort=" << settings.sort_transparency
           << " cull=" << static_cast<unsigned>(settings.cull) << " fill=" << static_cast<unsigned>(settings.fill)
           << " clear_color=" << settings.clear_color << " clear_depth=" << settings.clear_depth
           << " depth_test=" << settings.depth.test_enabled << " depth_write=" << settings.depth.write_enabled
           << " blend_depth_write=" << settings.blended_depth_write << " wire_width=" << settings.wire_width
           << " depth_bias=" << settings.depth_bias << " slope_bias=" << settings.slope_depth_bias
           << " lod_bias=" << settings.texture_lod_bias << " queued=" << renderer.queued_draws();
    return stream.str();
}

std::string statistics_text(const SoftwareRenderer& renderer) {
    const auto& stats = renderer.statistics();
    std::ostringstream stream;
    stream << "frame=" << stats.frame << " views=" << stats.views << " stale=" << stats.stale_views
           << " draws=" << stats.draws_rendered << '/' << stats.draws_submitted
           << " entities=" << stats.entities_examined << " skipped=" << stats.entities_skipped
           << " triangles=" << stats.input_triangles << " clipped=" << stats.triangles_clipped
           << " culled=" << stats.triangles_culled << " degenerate=" << stats.triangles_degenerate
           << " invalid_triangles=" << stats.triangles_invalid << " raster_triangles=" << stats.raster_triangles
           << " visits=" << stats.fragment_visits << " covered=" << stats.fragments_covered
           << " depth_rejected=" << stats.fragments_depth_rejected << " discarded=" << stats.fragments_discarded
           << " shaded=" << stats.fragments_shaded << " written=" << stats.fragments_written
           << " blended=" << stats.blended_fragments << " light_visits=" << stats.light_visits
           << " lights_evaluated=" << stats.lights_evaluated << " missing_textures=" << stats.missing_texture_samples
           << " work_limited=" << stats.work_limited << " status=" << describe(renderer.last_error());
    return stream.str();
}

bool set_option(RasterizerSettings& settings, std::string_view key, std::string_view value) noexcept {
    if (key == "cull") {
        if (value == "none") { settings.cull = CullMode::None; return true; }
        if (value == "back") { settings.cull = CullMode::Back; return true; }
        if (value == "front") { settings.cull = CullMode::Front; return true; }
        return false;
    }
    if (key == "fill") {
        if (value == "solid") { settings.fill = FillMode::Solid; return true; }
        if (value == "wireframe") { settings.fill = FillMode::Wireframe; return true; }
        return false;
    }
    if (key == "front") {
        if (value != "ccw" && value != "cw") { return false; }
        settings.front_counter_clockwise = value == "ccw";
        return true;
    }
    if (key == "compare") {
        constexpr std::array<std::string_view, 8> names{
            "never", "less", "less_equal", "equal", "greater_equal", "greater", "not_equal", "always"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (value == names[index]) {
                settings.depth.compare = static_cast<DepthCompare>(index);
                return true;
            }
        }
        return false;
    }
    if (const auto flag = boolean(value)) {
        if (key == "enabled") { settings.enabled = *flag; return true; }
        if (key == "all_views") { settings.render_all_views = *flag; return true; }
        if (key == "allow_stale") { settings.allow_stale_views = *flag; return true; }
        if (key == "transparency_sort") { settings.sort_transparency = *flag; return true; }
        if (key == "clear_color") { settings.clear_color = *flag; return true; }
        if (key == "clear_depth") { settings.clear_depth = *flag; return true; }
        if (key == "depth_test") { settings.depth.test_enabled = *flag; return true; }
        if (key == "depth_write") { settings.depth.write_enabled = *flag; return true; }
        if (key == "blend_depth_write") { settings.blended_depth_write = *flag; return true; }
    }
    if (const auto scalar = number<double>(value)) {
        if (key == "wire_width") { settings.wire_width = *scalar; return true; }
        if (key == "depth_bias") { settings.depth_bias = *scalar; return true; }
        if (key == "slope_bias") { settings.slope_depth_bias = *scalar; return true; }
        if (key == "lod_bias") { settings.texture_lod_bias = *scalar; return true; }
        if (key == "background_depth") { settings.background_depth = *scalar; return true; }
    }
    return false;
}

core::CommandResult settings_command(core::Kernel& host, core::CommandArgs arguments) {
    auto& renderer = host.services().get<SoftwareRenderer>();
    if (arguments.empty()) {
        return {true, settings_text(renderer)};
    }
    auto settings = renderer.settings();
    bool parsed = false;
    if (arguments[0] == "background" && arguments.size() == 5) {
        std::array<double, 4> channels{};
        parsed = true;
        for (std::size_t index = 0; index < channels.size(); ++index) {
            const auto channel = number<double>(arguments[index + 1]);
            if (!channel) { parsed = false; break; }
            channels[index] = *channel;
        }
        settings.background = {channels[0], channels[1], channels[2], channels[3]};
    } else if (arguments[0] == "scissor" && arguments.size() == 2 && arguments[1] == "off") {
        settings.scissor.reset();
        parsed = true;
    } else if (arguments[0] == "scissor" && arguments.size() == 5) {
        std::array<std::int32_t, 4> coordinates{};
        parsed = true;
        for (std::size_t index = 0; index < coordinates.size(); ++index) {
            const auto coordinate = number<std::int32_t>(arguments[index + 1]);
            if (!coordinate) { parsed = false; break; }
            coordinates[index] = *coordinate;
        }
        settings.scissor = Rect{coordinates[0], coordinates[1], coordinates[2], coordinates[3]};
    } else if (arguments.size() == 2) {
        parsed = set_option(settings, arguments[0], arguments[1]);
    }
    if (!parsed || !renderer.configure(settings)) {
        return {false, "usage: render.settings [key value | background r g b a | scissor x y width height | scissor off]"};
    }
    return {true, settings_text(renderer)};
}

RasterResult render_services(core::Kernel& host) {
    auto* current_scene = host.services().find<scene::Scene>();
    const auto* meshes = host.services().find<geometry::MeshLibrary>();
    const auto* cameras = host.services().find<CameraService>();
    const auto* materials = host.services().find<MaterialService>();
    auto* images = host.services().find<ImageService>();
    auto& renderer = host.services().get<SoftwareRenderer>();
    if (!current_scene || !meshes || !cameras || !materials || !images) {
        return {RasterError::MissingService, false};
    }
    return renderer.render_scene(*current_scene, *meshes, *cameras, *materials, *images);
}

} // namespace

void register_rasterizer(core::Kernel& kernel) {
    if (!kernel.register_module({"render.rasterizer", "Bounded CPU triangle clipping, shading and rasterization", 1})) {
        return;
    }
    auto& renderer = kernel.services().emplace<SoftwareRenderer>();
    auto settings = renderer.settings();
    settings.enabled = kernel.config().boolean("render.enabled", settings.enabled);
    settings.render_all_views = kernel.config().boolean("render.all_views", settings.render_all_views);
    settings.allow_stale_views = kernel.config().boolean("render.allow_stale", settings.allow_stale_views);
    settings.texture_lod_bias = kernel.config().number("render.lod_bias", settings.texture_lod_bias);
    settings.wire_width = kernel.config().number("render.wire_width", settings.wire_width);
    const bool parsed_cull = set_option(settings, "cull", kernel.config().text("render.cull", "back"));
    const bool parsed_fill = set_option(settings, "fill", kernel.config().text("render.fill", "solid"));
    if (!parsed_cull || !parsed_fill || !renderer.configure(settings)) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "render.rasterizer", "Raster settings rejected; previous settings retained");
    }
    (void)kernel.add_system({"render.rasterizer", core::Phase::Render, 0, {"camera.update", "spatial.sync"}},
        [](core::Kernel& host, const core::Frame&) {
            const auto result = render_services(host);
            const auto& stats = host.services().get<SoftwareRenderer>().statistics();
            host.metric("render.triangles", static_cast<double>(stats.input_triangles));
            host.metric("render.fragments", static_cast<double>(stats.fragments_written));
            host.metric("render.fragment_visits", static_cast<double>(stats.fragment_visits));
            host.metric("render.draws", static_cast<double>(stats.draws_rendered));
            host.metric("render.work_limited", stats.work_limited ? 1.0 : 0.0);
            host.metric("render.error", static_cast<double>(result.error));
        });
    (void)kernel.add_command("render.frame", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        if (!arguments.empty()) { return {false, "usage: render.frame"}; }
        const auto result = render_services(host);
        return {static_cast<bool>(result), std::string(describe(result.error)) + "; " +
            statistics_text(host.services().get<SoftwareRenderer>())};
    }, "Render active camera views and scene meshes into the shared framebuffer");
    (void)kernel.add_command("render.settings", settings_command,
        "Inspect or transactionally change CPU rasterization, depth, clipping and clear settings");
    (void)kernel.add_command("render.stats", [](core::Kernel& host, core::CommandArgs arguments) -> core::CommandResult {
        if (!arguments.empty()) { return {false, "usage: render.stats"}; }
        return {true, statistics_text(host.services().get<SoftwareRenderer>())};
    }, "Inspect the last render attempt's bounded work and fragment statistics");
}

} // namespace ge::render
