#include <ge/geometry/mesh.hpp>
#include <ge/core/runtime.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace ge::geometry {
namespace {

constexpr MeshLimits hard_limits{};

bool valid_limits(MeshLimits limits) noexcept {
    return limits.max_vertices <= hard_limits.max_vertices &&
        limits.max_indices <= hard_limits.max_indices &&
        limits.max_submeshes <= hard_limits.max_submeshes &&
        limits.max_tessellation > 0 && limits.max_tessellation <= hard_limits.max_tessellation;
}

std::size_t saturated_add(std::size_t a, std::size_t b) noexcept {
    return b > std::numeric_limits<std::size_t>::max() - a
        ? std::numeric_limits<std::size_t>::max() : a + b;
}

std::size_t saturated_product(std::size_t a, std::size_t b) noexcept {
    return a != 0 && b > std::numeric_limits<std::size_t>::max() / a
        ? std::numeric_limits<std::size_t>::max() : a * b;
}

void require_mesh(const MeshData& mesh, MeshLimits limits) {
    const auto result = validate_mesh(mesh, limits);
    if (!result) {
        if (result.error == MeshError::CapacityExceeded) {
            throw std::length_error(std::string(result.message));
        }
        throw std::invalid_argument(std::string(result.message));
    }
}

void require_capacity(std::size_t vertices, std::size_t indices,
                      std::size_t submeshes, MeshLimits limits) {
    if (!valid_limits(limits)) {
        throw std::invalid_argument("invalid mesh limits");
    }
    if (vertices > limits.max_vertices || indices > limits.max_indices ||
        submeshes > limits.max_submeshes) {
        throw std::length_error("mesh capacity exceeded");
    }
}

void require_segments(std::uint32_t value, std::uint32_t minimum, MeshLimits limits) {
    if (value < minimum || value > limits.max_tessellation || !valid_limits(limits)) {
        throw std::invalid_argument("mesh tessellation outside configured range");
    }
}

bool positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

math::Vec3 unit(math::Vec3 vector, math::Vec3 fallback = {0.0, 1.0, 0.0}) noexcept {
    const auto scale = std::max({std::abs(vector.x), std::abs(vector.y), std::abs(vector.z)});
    if (!std::isfinite(scale) || scale == 0.0) {
        return fallback;
    }
    vector /= scale;
    return vector / std::sqrt(math::dot(vector, vector));
}

bool finite_vertex(const Vertex& vertex) noexcept {
    return math::finite(vertex.position) && math::finite(vertex.normal) &&
        math::finite(vertex.tangent) && math::finite(vertex.color) && math::finite(vertex.uv);
}

MeshValidation arithmetic_error(std::size_t element) noexcept {
    return {MeshError::NonFiniteResult, element, "mesh arithmetic produced a nonfinite result"};
}

void triangle(MeshData& mesh, MeshIndex a, MeshIndex b, MeshIndex c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
}

void finish_primitive(MeshData& mesh, MeshLimits limits) {
    if (!mesh.indices.empty()) {
        mesh.submeshes.push_back({0, static_cast<MeshIndex>(mesh.indices.size()), 0});
    }
    require_mesh(mesh, limits);
}

std::size_t range_count(const MeshData& mesh) noexcept {
    return mesh.submeshes.empty() ? (mesh.indices.empty() ? std::size_t{0} : std::size_t{1})
                                 : mesh.submeshes.size();
}

struct Cell {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};
    bool operator==(const Cell&) const = default;
};

struct CellHash {
    std::size_t operator()(Cell cell) const noexcept {
        auto result = std::hash<std::int64_t>{}(cell.x);
        for (const auto value : {cell.y, cell.z}) {
            result ^= std::hash<std::int64_t>{}(value) + std::size_t{0x9e3779b9U} +
                (result << 6U) + (result >> 2U);
        }
        return result;
    }
};

Cell cell_for(math::Vec3 point, double tolerance) {
    const auto coordinate = [tolerance](double value) -> std::int64_t {
        if (tolerance == 0.0) {
            return std::bit_cast<std::int64_t>(value == 0.0 ? 0.0 : value);
        }
        const double scaled = std::floor(value / tolerance);
        constexpr double safe_limit = 9.0e18;
        if (!std::isfinite(scaled) || scaled <= -safe_limit || scaled >= safe_limit) {
            throw std::invalid_argument("weld position exceeds quantization range");
        }
        return static_cast<std::int64_t>(scaled);
    };
    return {coordinate(point.x), coordinate(point.y), coordinate(point.z)};
}

bool near(double a, double b, double tolerance) noexcept {
    return a == b || std::abs(a - b) <= tolerance;
}

bool weld_match(const Vertex& a, const Vertex& b, const WeldOptions& options) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!near(a.position[axis], b.position[axis], options.position_tolerance)) {
            return false;
        }
    }
    if (options.position_only) {
        return true;
    }
    for (std::size_t axis = 0; axis < 4; ++axis) {
        if (!near(a.tangent[axis], b.tangent[axis], options.attribute_tolerance) ||
            !near(a.color[axis], b.color[axis], options.attribute_tolerance)) {
            return false;
        }
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!near(a.normal[axis], b.normal[axis], options.attribute_tolerance)) {
            return false;
        }
    }
    return near(a.uv.x, b.uv.x, options.attribute_tolerance) &&
        near(a.uv.y, b.uv.y, options.attribute_tolerance);
}

template<class Integer>
bool parse_integer(std::string_view text, Integer& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_handle(std::string_view text, MeshHandle& handle) {
    const auto separator = text.find(':');
    return separator != std::string_view::npos &&
        parse_integer(text.substr(0, separator), handle.index) &&
        parse_integer(text.substr(separator + 1), handle.generation) && handle.valid();
}

std::string handle_text(MeshHandle handle) {
    return std::to_string(handle.index) + ":" + std::to_string(handle.generation);
}

MeshHandle resolve_mesh(const MeshLibrary& library, std::string_view text) {
    if (const auto named = library.find(text)) {
        return named;
    }
    MeshHandle result;
    return parse_handle(text, result) ? result : MeshHandle{};
}

} // namespace

std::size_t MeshData::triangle_count() const noexcept { return indices.size() / 3; }
bool MeshData::empty() const noexcept { return indices.empty(); }

std::size_t MeshData::allocated_bytes() const noexcept {
    const auto vertex_bytes = saturated_product(vertices.capacity(), sizeof(Vertex));
    const auto index_bytes = saturated_product(indices.capacity(), sizeof(MeshIndex));
    const auto range_bytes = saturated_product(submeshes.capacity(), sizeof(SubmeshRange));
    return saturated_add(saturated_add(vertex_bytes, index_bytes), range_bytes);
}

MeshValidation::operator bool() const noexcept { return error == MeshError::None; }

MeshValidation validate_mesh(const MeshData& mesh, MeshLimits limits, bool reject_degenerate) noexcept {
    if (!valid_limits(limits)) {
        return {MeshError::InvalidLimits, 0, "invalid mesh limits"};
    }
    if (mesh.vertices.size() > limits.max_vertices || mesh.indices.size() > limits.max_indices ||
        mesh.submeshes.size() > limits.max_submeshes) {
        return {MeshError::CapacityExceeded, 0, "mesh capacity exceeded"};
    }
    if (mesh.indices.size() % 3 != 0) {
        return {MeshError::InvalidIndexCount, mesh.indices.size(), "indices must contain complete triangles"};
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (!finite_vertex(mesh.vertices[i])) {
            return {MeshError::NonFiniteVertex, i, "vertex attributes must be finite"};
        }
    }
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        if (mesh.indices[i] >= mesh.vertices.size()) {
            return {MeshError::IndexOutOfRange, i, "triangle index is outside vertex storage"};
        }
    }
    std::size_t next = 0;
    for (std::size_t i = 0; i < mesh.submeshes.size(); ++i) {
        const auto& range = mesh.submeshes[i];
        if (range.first_index != next || range.index_count == 0 || range.index_count % 3 != 0 ||
            range.index_count > mesh.indices.size() - next) {
            return {MeshError::InvalidSubmesh, i, "submeshes must partition complete triangles in order"};
        }
        next += range.index_count;
    }
    if (!mesh.submeshes.empty() && next != mesh.indices.size()) {
        return {MeshError::InvalidSubmesh, mesh.submeshes.size(), "submeshes do not cover all indices"};
    }
    if (reject_degenerate) {
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto a = mesh.vertices[mesh.indices[i]].position;
            const auto b = mesh.vertices[mesh.indices[i + 1]].position;
            const auto c = mesh.vertices[mesh.indices[i + 2]].position;
            const auto first = b - a;
            const auto second = c - a;
            if (!math::finite(first) || !math::finite(second)) {
                return arithmetic_error(i / 3);
            }
            const auto normal = math::cross(unit(first, {}), unit(second, {}));
            if (math::length_squared(normal) <= 1.0e-24) {
                return {MeshError::DegenerateTriangle, i / 3, "triangle has coincident or collinear vertices"};
            }
        }
    }
    return {};
}

std::optional<math::Aabb> mesh_bounds(const MeshData& mesh) noexcept {
    if (mesh.vertices.empty()) {
        return std::nullopt;
    }
    math::Aabb result;
    for (const auto& vertex : mesh.vertices) {
        if (!math::finite(vertex.position)) {
            return std::nullopt;
        }
        result.min = math::min(result.min, vertex.position);
        result.max = math::max(result.max, vertex.position);
    }
    return result;
}

MeshValidation generate_normals(MeshData& mesh, NormalWeighting weighting, MeshLimits limits) {
    if (const auto validation = validate_mesh(mesh, limits); !validation) {
        return validation;
    }
    if (weighting != NormalWeighting::Area && weighting != NormalWeighting::Uniform &&
        weighting != NormalWeighting::CornerAngle) {
        return {MeshError::InvalidArgument, 0, "unknown normal weighting"};
    }
    std::vector<math::Vec3> normals(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const std::array<MeshIndex, 3> indices{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
        const auto a = mesh.vertices[indices[0]].position;
        const auto b = mesh.vertices[indices[1]].position;
        const auto c = mesh.vertices[indices[2]].position;
        const auto area_normal = math::cross(b - a, c - a);
        if (!math::finite(area_normal)) {
            return arithmetic_error(i / 3);
        }
        const auto direction = unit(area_normal, {});
        for (std::size_t corner = 0; corner < 3; ++corner) {
            auto contribution = weighting == NormalWeighting::Area ? area_normal : direction;
            if (weighting == NormalWeighting::CornerAngle) {
                const auto origin = mesh.vertices[indices[corner]].position;
                const auto first = mesh.vertices[indices[(corner + 1) % 3]].position - origin;
                const auto second = mesh.vertices[indices[(corner + 2) % 3]].position - origin;
                const auto cosine = math::clamp(math::dot(unit(first, {}), unit(second, {})), -1.0, 1.0);
                contribution *= std::acos(cosine);
            }
            normals[indices[corner]] += contribution;
            if (!math::finite(normals[indices[corner]])) {
                return arithmetic_error(indices[corner]);
            }
        }
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        mesh.vertices[i].normal = unit(normals[i]);
    }
    return {};
}

MeshValidation generate_tangents(MeshData& mesh, MeshLimits limits) {
    if (const auto validation = validate_mesh(mesh, limits); !validation) {
        return validation;
    }
    std::vector<math::Vec3> tangents(mesh.vertices.size());
    std::vector<math::Vec3> bitangents(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto& a = mesh.vertices[mesh.indices[i]];
        const auto& b = mesh.vertices[mesh.indices[i + 1]];
        const auto& c = mesh.vertices[mesh.indices[i + 2]];
        const auto first = b.position - a.position;
        const auto second = c.position - a.position;
        const auto uv_first = b.uv - a.uv;
        const auto uv_second = c.uv - a.uv;
        const auto determinant = math::cross(uv_first, uv_second);
        if (!math::finite(first) || !math::finite(second) || !std::isfinite(determinant)) {
            return arithmetic_error(i / 3);
        }
        if (std::abs(determinant) <= 1.0e-20) {
            continue;
        }
        const auto tangent = (first * uv_second.y - second * uv_first.y) / determinant;
        const auto bitangent = (second * uv_first.x - first * uv_second.x) / determinant;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto index = mesh.indices[i + corner];
            tangents[index] += tangent;
            bitangents[index] += bitangent;
            if (!math::finite(tangents[index]) || !math::finite(bitangents[index])) {
                return arithmetic_error(index);
            }
        }
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto normal = unit(mesh.vertices[i].normal);
        const auto candidate = unit(tangents[i], {});
        const auto fallback = math::orthonormal_basis(normal).first;
        const auto tangent = unit(candidate - normal * math::dot(normal, candidate), fallback);
        const auto handedness = math::dot(math::cross(normal, tangent), unit(bitangents[i], {})) < 0.0;
        mesh.vertices[i].tangent = math::homogeneous(tangent, handedness ? -1.0 : 1.0);
    }
    return {};
}

MeshValidation transform_mesh(MeshData& mesh, const math::Mat4& transform, MeshLimits limits) {
    if (const auto validation = validate_mesh(mesh, limits); !validation) {
        return validation;
    }
    if (!math::finite(transform) || transform(3, 0) != 0.0 || transform(3, 1) != 0.0 ||
        transform(3, 2) != 0.0 || transform(3, 3) != 1.0) {
        return {MeshError::InvalidArgument, 0, "mesh transform must be finite and affine"};
    }
    const auto normal_transform = math::normal_matrix(transform);
    const auto determinant = math::determinant(math::upper3x3(transform));
    if (!normal_transform || !math::finite(*normal_transform) || !std::isfinite(determinant)) {
        return {MeshError::SingularTransform, 0, "mesh transform has no finite normal inverse"};
    }
    auto transformed = mesh.vertices;
    for (std::size_t i = 0; i < transformed.size(); ++i) {
        auto& vertex = transformed[i];
        const auto position = math::transform_point(transform, vertex.position);
        const auto raw_normal = *normal_transform * vertex.normal;
        const auto raw_tangent = math::transform_vector(transform, math::xyz(vertex.tangent));
        if (!math::finite(position) || !math::finite(raw_normal) || !math::finite(raw_tangent)) {
            return arithmetic_error(i);
        }
        vertex.position = position;
        vertex.normal = unit(raw_normal);
        const auto tangent = unit(raw_tangent, {});
        const auto orthogonal = unit(tangent - vertex.normal * math::dot(vertex.normal, tangent),
                                     math::orthonormal_basis(vertex.normal).first);
        const auto sign = (vertex.tangent.w < 0.0 ? -1.0 : 1.0) * (determinant < 0.0 ? -1.0 : 1.0);
        vertex.tangent = math::homogeneous(orthogonal, sign);
    }
    mesh.vertices.swap(transformed);
    if (determinant < 0.0) {
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
        }
    }
    return {};
}

MeshValidation remap_vertices(MeshData& mesh, std::span<const MeshIndex> new_to_old, MeshLimits limits) {
    if (const auto validation = validate_mesh(mesh, limits); !validation) {
        return validation;
    }
    if (new_to_old.size() != mesh.vertices.size()) {
        return {MeshError::InvalidArgument, 0, "vertex remap must be a complete permutation"};
    }
    std::vector<MeshIndex> old_to_new(mesh.vertices.size(), invalid_mesh_index);
    std::vector<Vertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (std::size_t i = 0; i < new_to_old.size(); ++i) {
        const auto old = new_to_old[i];
        if (old >= mesh.vertices.size() || old_to_new[old] != invalid_mesh_index) {
            return {MeshError::InvalidArgument, i, "vertex remap contains an invalid or duplicate entry"};
        }
        old_to_new[old] = static_cast<MeshIndex>(i);
        vertices.push_back(mesh.vertices[old]);
    }
    for (auto& index : mesh.indices) {
        index = old_to_new[index];
    }
    mesh.vertices.swap(vertices);
    return {};
}

MeshWeldResult weld_vertices(const MeshData& mesh, WeldOptions options, MeshLimits limits) {
    require_mesh(mesh, limits);
    if (!std::isfinite(options.position_tolerance) || options.position_tolerance < 0.0 ||
        !std::isfinite(options.attribute_tolerance) || options.attribute_tolerance < 0.0 ||
        options.comparison_limit == 0 || options.comparison_limit > 67'108'864) {
        throw std::invalid_argument("invalid vertex welding options");
    }
    MeshWeldResult result;
    result.mesh.vertices.reserve(mesh.vertices.size());
    result.mesh.indices.reserve(mesh.indices.size());
    result.mesh.submeshes.reserve(mesh.submeshes.size());
    result.old_to_new.reserve(mesh.vertices.size());
    std::unordered_map<Cell, std::vector<MeshIndex>, CellHash> buckets;
    buckets.reserve(mesh.vertices.size());
    std::size_t comparisons = 0;
    const int radius = options.position_tolerance == 0.0 ? 0 : 1;
    for (const auto& vertex : mesh.vertices) {
        const auto cell = cell_for(vertex.position, options.position_tolerance);
        auto selected = invalid_mesh_index;
        for (int z = -radius; z <= radius; ++z) {
            for (int y = -radius; y <= radius; ++y) {
                for (int x = -radius; x <= radius; ++x) {
                    const auto found = buckets.find({cell.x + x, cell.y + y, cell.z + z});
                    if (found == buckets.end()) {
                        continue;
                    }
                    for (const auto candidate : found->second) {
                        if (++comparisons > options.comparison_limit) {
                            throw std::length_error("vertex welding comparison budget exhausted");
                        }
                        if (candidate < selected && weld_match(vertex, result.mesh.vertices[candidate], options)) {
                            selected = candidate;
                        }
                    }
                }
            }
        }
        if (selected == invalid_mesh_index) {
            selected = static_cast<MeshIndex>(result.mesh.vertices.size());
            result.mesh.vertices.push_back(vertex);
            buckets[cell].push_back(selected);
        }
        result.old_to_new.push_back(selected);
    }
    const auto append_range = [&](SubmeshRange range, bool explicit_range) {
        const auto first = result.mesh.indices.size();
        const auto end = static_cast<std::size_t>(range.first_index) + range.index_count;
        for (std::size_t i = range.first_index; i < end; i += 3) {
            const auto a = result.old_to_new[mesh.indices[i]];
            const auto b = result.old_to_new[mesh.indices[i + 1]];
            const auto c = result.old_to_new[mesh.indices[i + 2]];
            if (options.remove_degenerate_triangles && (a == b || b == c || c == a)) {
                ++result.removed_triangles;
            } else {
                triangle(result.mesh, a, b, c);
            }
        }
        if (explicit_range && result.mesh.indices.size() != first) {
            result.mesh.submeshes.push_back({static_cast<MeshIndex>(first),
                static_cast<MeshIndex>(result.mesh.indices.size() - first), range.material_slot});
        }
    };
    if (mesh.submeshes.empty()) {
        append_range({0, static_cast<MeshIndex>(mesh.indices.size()), 0}, false);
    } else {
        for (const auto& range : mesh.submeshes) {
            append_range(range, true);
        }
    }
    return result;
}

MeshData merge_meshes(std::span<const MeshInstance> instances, MeshLimits limits) {
    if (instances.size() > hard_limits.max_submeshes) {
        throw std::length_error("too many mesh instances to merge");
    }
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::size_t submeshes = 0;
    for (const auto& instance : instances) {
        if (instance.mesh == nullptr) {
            throw std::invalid_argument("null mesh instance");
        }
        require_mesh(*instance.mesh, limits);
        vertices = saturated_add(vertices, instance.mesh->vertices.size());
        indices = saturated_add(indices, instance.mesh->indices.size());
        submeshes = saturated_add(submeshes, range_count(*instance.mesh));
        require_capacity(vertices, indices, submeshes, limits);
    }
    require_capacity(vertices, indices, submeshes, limits);
    MeshData result;
    result.vertices.reserve(vertices);
    result.indices.reserve(indices);
    result.submeshes.reserve(submeshes);
    for (const auto& instance : instances) {
        auto transformed = *instance.mesh;
        const auto transformed_result = transform_mesh(transformed, instance.transform, limits);
        if (!transformed_result) {
            throw std::invalid_argument(std::string(transformed_result.message));
        }
        const auto vertex_base = static_cast<MeshIndex>(result.vertices.size());
        const auto index_base = static_cast<MeshIndex>(result.indices.size());
        result.vertices.insert(result.vertices.end(), transformed.vertices.begin(), transformed.vertices.end());
        for (const auto index : transformed.indices) {
            result.indices.push_back(vertex_base + index);
        }
        if (transformed.submeshes.empty() && !transformed.indices.empty()) {
            result.submeshes.push_back({index_base, static_cast<MeshIndex>(transformed.indices.size()), 0});
        } else {
            for (auto range : transformed.submeshes) {
                range.first_index += index_base;
                result.submeshes.push_back(range);
            }
        }
    }
    return result;
}

bool MeshTopology::closed_manifold() const noexcept {
    return !edges.empty() && boundary_edges == 0 && nonmanifold_edges == 0 && inconsistent_edges == 0;
}

MeshTopology build_topology(const MeshData& mesh, MeshLimits limits) {
    require_mesh(mesh, limits);
    struct Incidence {
        MeshIndex low;
        MeshIndex high;
        MeshIndex face;
        std::size_t corner;
        bool forward;
    };
    std::vector<Incidence> incidences;
    incidences.reserve(mesh.indices.size());
    MeshTopology result;
    result.triangle_neighbors.resize(mesh.triangle_count(),
        {invalid_mesh_index, invalid_mesh_index, invalid_mesh_index});
    result.vertex_face_offsets.resize(mesh.vertices.size() + 1);
    result.vertex_faces.resize(mesh.indices.size());
    std::vector<MeshIndex> parents(mesh.triangle_count());
    std::iota(parents.begin(), parents.end(), MeshIndex{0});
    const auto root = [&parents](MeshIndex face) {
        while (parents[face] != face) {
            parents[face] = parents[parents[face]];
            face = parents[face];
        }
        return face;
    };
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto a = mesh.indices[i + corner];
            const auto b = mesh.indices[i + (corner + 1) % 3];
            if (a == b) {
                throw std::invalid_argument("topology requires distinct triangle vertices");
            }
            incidences.push_back({std::min(a, b), std::max(a, b),
                static_cast<MeshIndex>(i / 3), corner, a < b});
            ++result.vertex_face_offsets[static_cast<std::size_t>(a) + 1];
        }
    }
    std::partial_sum(result.vertex_face_offsets.begin(), result.vertex_face_offsets.end(),
                     result.vertex_face_offsets.begin());
    auto cursors = result.vertex_face_offsets;
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        result.vertex_faces[cursors[mesh.indices[i]]++] = static_cast<MeshIndex>(i / 3);
    }
    std::sort(incidences.begin(), incidences.end(), [](const Incidence& a, const Incidence& b) {
        return std::tie(a.low, a.high, a.face, a.corner) < std::tie(b.low, b.high, b.face, b.corner);
    });
    result.edge_faces.reserve(mesh.indices.size());
    result.edges.reserve(mesh.indices.size());
    for (std::size_t first = 0; first < incidences.size();) {
        auto end = first + 1;
        while (end < incidences.size() && incidences[end].low == incidences[first].low &&
               incidences[end].high == incidences[first].high) {
            ++end;
        }
        const auto count = end - first;
        const bool consistent = count != 2 || incidences[first].forward != incidences[first + 1].forward;
        result.edges.push_back({{incidences[first].low, incidences[first].high},
            static_cast<MeshIndex>(result.edge_faces.size()), static_cast<MeshIndex>(count), consistent});
        result.boundary_edges += count == 1 ? 1U : 0U;
        result.nonmanifold_edges += count > 2 ? 1U : 0U;
        result.inconsistent_edges += consistent ? 0U : 1U;
        const auto first_root = root(incidences[first].face);
        for (auto i = first; i < end; ++i) {
            result.edge_faces.push_back(incidences[i].face);
            parents[root(incidences[i].face)] = first_root;
        }
        if (count == 2) {
            const auto& a = incidences[first];
            const auto& b = incidences[first + 1];
            result.triangle_neighbors[a.face][a.corner] = b.face;
            result.triangle_neighbors[b.face][b.corner] = a.face;
        }
        first = end;
    }
    result.triangle_components.resize(mesh.triangle_count());
    std::vector<MeshIndex> labels(mesh.triangle_count(), invalid_mesh_index);
    for (std::size_t i = 0; i < mesh.triangle_count(); ++i) {
        auto& label = labels[root(static_cast<MeshIndex>(i))];
        if (label == invalid_mesh_index) {
            label = static_cast<MeshIndex>(result.component_count++);
        }
        result.triangle_components[i] = label;
    }
    return result;
}

MeshData make_plane(PlaneOptions options, MeshLimits limits) {
    if (!positive(options.width) || !positive(options.depth)) {
        throw std::invalid_argument("plane dimensions must be positive and finite");
    }
    require_segments(options.segments_x, 1, limits);
    require_segments(options.segments_z, 1, limits);
    const auto columns = static_cast<std::size_t>(options.segments_x) + 1;
    const auto rows = static_cast<std::size_t>(options.segments_z) + 1;
    const auto index_count = static_cast<std::size_t>(options.segments_x) * options.segments_z * 6;
    require_capacity(columns * rows, index_count, 1, limits);
    MeshData mesh;
    mesh.vertices.reserve(columns * rows);
    mesh.indices.reserve(index_count);
    for (std::size_t z = 0; z < rows; ++z) {
        const auto v = static_cast<double>(z) / static_cast<double>(options.segments_z);
        for (std::size_t x = 0; x < columns; ++x) {
            const auto u = static_cast<double>(x) / static_cast<double>(options.segments_x);
            Vertex vertex;
            vertex.position = {(u - 0.5) * options.width, 0.0, (v - 0.5) * options.depth};
            vertex.uv = {u, v};
            vertex.tangent.w = -1.0;
            mesh.vertices.push_back(vertex);
        }
    }
    for (std::size_t z = 0; z + 1 < rows; ++z) {
        for (std::size_t x = 0; x + 1 < columns; ++x) {
            const auto a = static_cast<MeshIndex>(z * columns + x);
            const auto c = a + static_cast<MeshIndex>(columns);
            triangle(mesh, a, c, a + 1);
            triangle(mesh, a + 1, c, c + 1);
        }
    }
    finish_primitive(mesh, limits);
    return mesh;
}

MeshData make_box(math::Vec3 size, MeshLimits limits) {
    if (!positive(size.x) || !positive(size.y) || !positive(size.z)) {
        throw std::invalid_argument("box dimensions must be positive and finite");
    }
    require_capacity(24, 36, 1, limits);
    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);
    const auto half = size * 0.5;
    const auto face = [&mesh, half](math::Vec3 normal, math::Vec3 u, math::Vec3 v) {
        const auto first = static_cast<MeshIndex>(mesh.vertices.size());
        for (const auto uv : {math::Vec2{0.0, 0.0}, math::Vec2{1.0, 0.0},
                              math::Vec2{1.0, 1.0}, math::Vec2{0.0, 1.0}}) {
            Vertex vertex;
            vertex.position = (normal + u * (uv.x * 2.0 - 1.0) + v * (uv.y * 2.0 - 1.0)) * half;
            vertex.normal = normal;
            vertex.tangent = math::homogeneous(u, 1.0);
            vertex.uv = uv;
            mesh.vertices.push_back(vertex);
        }
        triangle(mesh, first, first + 1, first + 2);
        triangle(mesh, first, first + 2, first + 3);
    };
    face({0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    face({0.0, 0.0, -1.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    face({1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0});
    face({-1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0});
    face({0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, -1.0});
    face({0.0, -1.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0});
    finish_primitive(mesh, limits);
    return mesh;
}

MeshData make_uv_sphere(SphereOptions options, MeshLimits limits) {
    if (!positive(options.radius)) {
        throw std::invalid_argument("sphere radius must be positive and finite");
    }
    require_segments(options.longitudes, 3, limits);
    require_segments(options.latitudes, 2, limits);
    const auto columns = static_cast<std::size_t>(options.longitudes) + 1;
    const auto rows = static_cast<std::size_t>(options.latitudes) + 1;
    const auto indices = static_cast<std::size_t>(options.longitudes) * (options.latitudes - 1) * 6;
    require_capacity(columns * rows, indices, 1, limits);
    MeshData mesh;
    mesh.vertices.reserve(columns * rows);
    mesh.indices.reserve(indices);
    for (std::size_t y = 0; y < rows; ++y) {
        const auto v = static_cast<double>(y) / static_cast<double>(options.latitudes);
        const auto latitude = v * math::pi;
        const auto radial = y == 0 || y + 1 == rows ? 0.0 : std::sin(latitude);
        for (std::size_t x = 0; x < columns; ++x) {
            const auto u = static_cast<double>(x) / static_cast<double>(options.longitudes);
            const auto longitude = x + 1 == columns ? 0.0 : u * math::tau;
            Vertex vertex;
            vertex.normal = {radial * std::cos(longitude), std::cos(latitude), radial * std::sin(longitude)};
            vertex.position = vertex.normal * options.radius;
            vertex.tangent = {-std::sin(longitude), 0.0, std::cos(longitude), 1.0};
            vertex.uv = {u, v};
            mesh.vertices.push_back(vertex);
        }
    }
    for (std::size_t y = 0; y + 1 < rows; ++y) {
        for (std::size_t x = 0; x + 1 < columns; ++x) {
            const auto a = static_cast<MeshIndex>(y * columns + x);
            const auto c = a + static_cast<MeshIndex>(columns);
            if (y != 0) {
                triangle(mesh, a, a + 1, c);
            }
            if (y + 2 != rows) {
                triangle(mesh, a + 1, c + 1, c);
            }
        }
    }
    finish_primitive(mesh, limits);
    return mesh;
}

MeshData make_cylinder(CylinderOptions options, MeshLimits limits) {
    if (!positive(options.radius) || !positive(options.height)) {
        throw std::invalid_argument("cylinder dimensions must be positive and finite");
    }
    require_segments(options.radial_segments, 3, limits);
    require_segments(options.height_segments, 1, limits);
    const auto columns = static_cast<std::size_t>(options.radial_segments) + 1;
    const auto rows = static_cast<std::size_t>(options.height_segments) + 1;
    const auto cap_vertices = options.capped ? 2 * (columns + 1) : 0;
    const auto indices = static_cast<std::size_t>(options.radial_segments) *
        (options.height_segments + (options.capped ? 1U : 0U)) * 6;
    require_capacity(columns * rows + cap_vertices, indices, 1, limits);
    MeshData mesh;
    mesh.vertices.reserve(columns * rows + cap_vertices);
    mesh.indices.reserve(indices);
    for (std::size_t y = 0; y < rows; ++y) {
        const auto v = static_cast<double>(y) / static_cast<double>(options.height_segments);
        for (std::size_t x = 0; x < columns; ++x) {
            const auto u = static_cast<double>(x) / static_cast<double>(options.radial_segments);
            const auto angle = x + 1 == columns ? 0.0 : u * math::tau;
            Vertex vertex;
            vertex.normal = {std::cos(angle), 0.0, std::sin(angle)};
            vertex.position = {vertex.normal.x * options.radius, (v - 0.5) * options.height,
                               vertex.normal.z * options.radius};
            vertex.tangent = {-std::sin(angle), 0.0, std::cos(angle), -1.0};
            vertex.uv = {u, v};
            mesh.vertices.push_back(vertex);
        }
    }
    for (std::size_t y = 0; y + 1 < rows; ++y) {
        for (std::size_t x = 0; x + 1 < columns; ++x) {
            const auto a = static_cast<MeshIndex>(y * columns + x);
            const auto c = a + static_cast<MeshIndex>(columns);
            triangle(mesh, a, c, a + 1);
            triangle(mesh, a + 1, c, c + 1);
        }
    }
    if (options.capped) {
        for (const double sign : {-1.0, 1.0}) {
            Vertex vertex;
            vertex.position = {0.0, sign * options.height * 0.5, 0.0};
            vertex.normal = {0.0, sign, 0.0};
            vertex.tangent.w = -sign;
            vertex.uv = {0.5, 0.5};
            const auto center = static_cast<MeshIndex>(mesh.vertices.size());
            mesh.vertices.push_back(vertex);
            for (std::size_t x = 0; x < columns; ++x) {
                const auto angle = x + 1 == columns ? 0.0 : static_cast<double>(x) * math::tau /
                    static_cast<double>(options.radial_segments);
                const auto cosine = std::cos(angle);
                const auto sine = std::sin(angle);
                vertex.position.x = cosine * options.radius;
                vertex.position.z = sine * options.radius;
                vertex.uv = {cosine * 0.5 + 0.5, sine * 0.5 + 0.5};
                mesh.vertices.push_back(vertex);
            }
            for (std::uint32_t x = 0; x < options.radial_segments; ++x) {
                const auto current = center + 1 + x;
                if (sign > 0.0) {
                    triangle(mesh, center, current + 1, current);
                } else {
                    triangle(mesh, center, current, current + 1);
                }
            }
        }
    }
    finish_primitive(mesh, limits);
    return mesh;
}

struct MeshLibrary::Record {
    MeshData mesh;
    std::string name;
    math::Aabb bounds;
    MeshHandle handle;
    std::size_t bytes{};
    std::uint64_t revision{};
};

MeshLibrary::MeshLibrary(MeshLibraryLimits limits) : limits_(limits), generations_(limits.capacity) {
    if (!valid_limits(limits.mesh) || limits.capacity > 65'536 ||
        limits.resident_bytes == 0 || limits.resident_bytes > 1'073'741'824) {
        throw std::invalid_argument("invalid mesh library limits");
    }
    records_.reserve(limits.capacity);
}

MeshLibrary::~MeshLibrary() = default;

std::unique_ptr<MeshLibrary::Record> MeshLibrary::prepare(MeshData mesh, std::string_view name) const {
    require_mesh(mesh, limits_.mesh);
    if (name.size() > 256 || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid mesh name");
    }
    auto record = std::make_unique<Record>();
    record->bounds = mesh_bounds(mesh).value_or(math::Aabb{});
    record->mesh = std::move(mesh);
    record->name = name;
    record->bytes = saturated_add(sizeof(Record), record->mesh.allocated_bytes());
    record->bytes = saturated_add(record->bytes, record->name.capacity() + 1);
    return record;
}

MeshHandle MeshLibrary::create(MeshData mesh, std::string_view name) {
    if (!name.empty() && find(name)) {
        throw std::invalid_argument("mesh name already exists");
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("mesh library revision exhausted");
    }
    auto record = prepare(std::move(mesh), name);
    if (record->bytes > limits_.resident_bytes - resident_bytes_) {
        throw std::length_error("mesh library resident budget exhausted");
    }
    const auto acquired = generations_.acquire();
    if (!acquired) {
        throw std::length_error("mesh library handle capacity exhausted");
    }
    if (records_.size() < acquired->index) {
        records_.resize(acquired->index);
    }
    record->handle = {acquired->index, acquired->generation};
    record->revision = ++revision_;
    const auto result = record->handle;
    resident_bytes_ += record->bytes;
    records_[static_cast<std::size_t>(acquired->index) - 1] = std::move(record);
    return result;
}

bool MeshLibrary::replace(MeshHandle handle, MeshData mesh) {
    if (!contains(handle)) {
        return false;
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("mesh library revision exhausted");
    }
    auto& current = records_[static_cast<std::size_t>(handle.index) - 1];
    auto replacement = prepare(std::move(mesh), current->name);
    const auto remaining = resident_bytes_ - current->bytes;
    if (replacement->bytes > limits_.resident_bytes - remaining) {
        throw std::length_error("mesh library resident budget exhausted");
    }
    replacement->handle = handle;
    replacement->revision = ++revision_;
    resident_bytes_ = remaining + replacement->bytes;
    current.swap(replacement);
    return true;
}

bool MeshLibrary::destroy(MeshHandle handle) noexcept {
    if (!contains(handle)) {
        return false;
    }
    auto& record = records_[static_cast<std::size_t>(handle.index) - 1];
    resident_bytes_ -= record->bytes;
    record.reset();
    static_cast<void>(generations_.release({handle.index, handle.generation}));
    if (revision_ != std::numeric_limits<std::uint64_t>::max()) {
        ++revision_;
    }
    return true;
}

void MeshLibrary::clear() noexcept {
    for (auto& record : records_) {
        if (record) {
            static_cast<void>(destroy(record->handle));
        }
    }
}

bool MeshLibrary::contains(MeshHandle handle) const noexcept {
    return generations_.alive({handle.index, handle.generation});
}

const MeshData* MeshLibrary::get(MeshHandle handle) const noexcept {
    return contains(handle) ? &records_[static_cast<std::size_t>(handle.index) - 1]->mesh : nullptr;
}

MeshHandle MeshLibrary::find(std::string_view name) const noexcept {
    if (!name.empty()) {
        for (const auto& record : records_) {
            if (record && record->name == name) {
                return record->handle;
            }
        }
    }
    return {};
}

std::optional<MeshInfo> MeshLibrary::inspect(MeshHandle handle) const noexcept {
    if (!contains(handle)) {
        return std::nullopt;
    }
    const auto& record = *records_[static_cast<std::size_t>(handle.index) - 1];
    return MeshInfo{handle, record.name, record.mesh.vertices.size(), record.mesh.triangle_count(),
        range_count(record.mesh), record.bytes, record.bounds, record.revision};
}

std::vector<MeshHandle> MeshLibrary::handles() const {
    std::vector<MeshHandle> result;
    result.reserve(records_.size());
    for (const auto& record : records_) {
        if (record) {
            result.push_back(record->handle);
        }
    }
    return result;
}

MeshStatistics MeshLibrary::statistics() const noexcept {
    MeshStatistics result;
    result.resident_bytes = resident_bytes_;
    result.capacity = limits_.capacity;
    result.revision = revision_;
    for (const auto& record : records_) {
        if (record) {
            ++result.meshes;
            result.vertices += record->mesh.vertices.size();
            result.triangles += record->mesh.triangle_count();
        }
    }
    return result;
}

const MeshLibraryLimits& MeshLibrary::limits() const noexcept { return limits_; }

bool MeshLibrary::assign(ecs::Registry& registry, ge::RuntimeHandle entity, MeshHandle mesh,
                         std::uint32_t first_submesh, std::uint32_t submesh_count) const {
    const auto info = inspect(mesh);
    if (!registry.alive(entity) || !info || first_submesh >= info->submeshes ||
        submesh_count > info->submeshes - first_submesh) {
        return false;
    }
    registry.emplace_or_replace<MeshComponent>(entity, MeshComponent{mesh, first_submesh, submesh_count, true});
    return true;
}

void register_mesh(core::Kernel& kernel) {
    if (!kernel.register_module({"geometry.mesh", "Validated shared meshes, primitives and topology", 1})) {
        return;
    }
    kernel.services().emplace<MeshLibrary>();
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        registry->register_component<MeshComponent>("geometry.mesh");
    }
    kernel.add_command("mesh.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 4) {
            return {false, "usage: mesh.create NAME plane|box|sphere|cylinder [SIZE] [SEGMENTS]"};
        }
        double size = 1.0;
        std::uint32_t segments = 16;
        if (args.size() >= 3) {
            const auto parsed = std::from_chars(args[2].data(), args[2].data() + args[2].size(), size);
            if (parsed.ec != std::errc{} || parsed.ptr != args[2].data() + args[2].size() || !positive(size)) {
                return {false, "size must be a positive finite number"};
            }
        }
        if (args.size() == 4 && !parse_integer(args[3], segments)) {
            return {false, "invalid tessellation"};
        }
        auto& library = host.services().get<MeshLibrary>();
        try {
            MeshData mesh;
            const auto limits = library.limits().mesh;
            if (args[1] == "plane") {
                mesh = make_plane({size, size, segments, segments}, limits);
            } else if (args[1] == "box") {
                mesh = make_box({size, size, size}, limits);
            } else if (args[1] == "sphere") {
                mesh = make_uv_sphere({size, segments, std::max(2U, segments / 2)}, limits);
            } else if (args[1] == "cylinder") {
                mesh = make_cylinder({size, size, segments, 1, true}, limits);
            } else {
                return {false, "unknown primitive"};
            }
            const auto handle = library.create(std::move(mesh), args[0]);
            return {true, "created mesh " + handle_text(handle)};
        } catch (const std::exception& error) {
            return {false, error.what()};
        }
    }, "Create a bounded procedural mesh in the shared mesh library");
    kernel.add_command("mesh.assign", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) {
            return {false, "usage: mesh.assign ENTITY_NAME_OR_INDEX:GENERATION MESH_NAME_OR_INDEX:GENERATION"};
        }
        auto* registry = host.services().find<ecs::Registry>();
        if (registry == nullptr) {
            return {false, "entity registry is unavailable"};
        }
        auto entity = registry->find_name(args[0]);
        MeshHandle token;
        if (!entity && parse_handle(args[0], token)) {
            entity = {token.index, token.generation};
        }
        const auto& library = host.services().get<MeshLibrary>();
        const bool assigned = library.assign(*registry, entity, resolve_mesh(library, args[1]));
        return {assigned, assigned ? "mesh assigned" : "invalid entity or mesh handle"};
    }, "Attach an existing shared mesh to an existing ECS entity");
    kernel.add_command("mesh.inspect", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        const auto& library = host.services().get<MeshLibrary>();
        if (args.empty()) {
            const auto stats = library.statistics();
            return {true, "meshes=" + std::to_string(stats.meshes) + " vertices=" + std::to_string(stats.vertices) +
                " triangles=" + std::to_string(stats.triangles) + " bytes=" + std::to_string(stats.resident_bytes)};
        }
        if (args.size() != 1) {
            return {false, "usage: mesh.inspect [MESH_NAME_OR_INDEX:GENERATION]"};
        }
        const auto info = library.inspect(resolve_mesh(library, args[0]));
        if (!info) {
            return {false, "mesh handle is missing or stale"};
        }
        return {true, "mesh=" + handle_text(info->handle) + " name=" + std::string(info->name) +
            " vertices=" + std::to_string(info->vertices) + " triangles=" + std::to_string(info->triangles) +
            " submeshes=" + std::to_string(info->submeshes) + " revision=" + std::to_string(info->revision)};
    }, "Inspect mesh identity, geometry counts and resident storage");
    kernel.add_system({"geometry.mesh.bounds", core::Phase::Visibility, -100, {}},
        [](core::Kernel& host, const core::Frame&) {
            const auto& library = host.services().get<MeshLibrary>();
            const auto* scene_service = host.services().find<scene::Scene>();
            std::size_t stale = 0;
            if (auto* registry = host.services().find<ecs::Registry>()) {
                if (registry->component_type<MeshComponent>() == 0) {
                    registry->register_component<MeshComponent>("geometry.mesh");
                }
                for (const auto entity : registry->view<MeshComponent>()) {
                    const auto& component = registry->require<MeshComponent>(entity);
                    const auto info = library.inspect(component.mesh);
                    if (!info || !math::valid(info->bounds) || component.first_submesh >= info->submeshes ||
                        component.submesh_count > info->submeshes - component.first_submesh) {
                        ++stale;
                        continue;
                    }
                    auto world = info->bounds;
                    if (scene_service != nullptr && scene_service->contains(entity)) {
                        const auto* matrix = scene_service->cached_world_matrix(entity);
                        if (matrix == nullptr) {
                            ++stale;
                            continue;
                        }
                        world = math::transform_bounds(*matrix, info->bounds);
                    } else if (const auto* transform = registry->get<ecs::TransformComponent>(entity)) {
                        world = math::transform_bounds(math::matrix(transform->world), info->bounds);
                    }
                    if (!math::valid(world)) {
                        ++stale;
                        continue;
                    }
                    if (auto* bounds = registry->get<ecs::BoundsComponent>(entity)) {
                        if (bounds->local.min != info->bounds.min || bounds->local.max != info->bounds.max ||
                            bounds->world.min != world.min || bounds->world.max != world.max) {
                            *bounds = {info->bounds, world};
                            registry->mark_changed<ecs::BoundsComponent>(entity);
                        }
                    } else {
                        registry->emplace<ecs::BoundsComponent>(entity, ecs::BoundsComponent{info->bounds, world});
                    }
                }
            }
            const auto stats = library.statistics();
            host.metric("mesh.count", static_cast<double>(stats.meshes));
            host.metric("mesh.vertices", static_cast<double>(stats.vertices));
            host.metric("mesh.triangles", static_cast<double>(stats.triangles));
            host.metric("mesh.resident_bytes", static_cast<double>(stats.resident_bytes));
            host.metric("mesh.invalid_components", static_cast<double>(stale));
        });
}

} // namespace ge::geometry
