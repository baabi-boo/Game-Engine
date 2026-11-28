#include <ge/terrain/heightfield.hpp>

#include <ge/core/binary.hpp>
#include <ge/core/runtime.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ge::terrain {
namespace {

constexpr double coordinate_limit = 1.0e9;
constexpr std::size_t absolute_samples = 1'050'625;
constexpr std::size_t operation_limit = 67'108'864;

bool bounded(double value, double maximum) noexcept {
    return std::isfinite(value) && std::abs(value) <= maximum;
}

bool valid_description(const HeightfieldDesc& desc, const HeightfieldLimits& limits) noexcept {
    if (limits.max_axis < 2 || limits.max_axis > 1025 || limits.max_samples > absolute_samples ||
        limits.max_samples < 4 || !bounded(limits.max_absolute_height, 1.0e7) ||
        limits.max_absolute_height <= 0.0) {
        return false;
    }
    if (desc.columns < 2 || desc.rows < 2 || desc.columns > limits.max_axis ||
        desc.rows > limits.max_axis ||
        static_cast<std::size_t>(desc.columns) * desc.rows > limits.max_samples) {
        return false;
    }
    if (!math::finite(desc.origin) || !math::finite(desc.spacing) ||
        desc.spacing.x < 1.0e-6 || desc.spacing.y < 1.0e-6 ||
        desc.spacing.x > 1.0e6 || desc.spacing.y > 1.0e6 ||
        !bounded(desc.initial_height, limits.max_absolute_height)) {
        return false;
    }
    const double end_x = desc.origin.x + static_cast<double>(desc.columns - 1) * desc.spacing.x;
    const double end_z = desc.origin.z + static_cast<double>(desc.rows - 1) * desc.spacing.y;
    return bounded(desc.origin.x, coordinate_limit) && bounded(desc.origin.y, coordinate_limit) &&
        bounded(desc.origin.z, coordinate_limit) && bounded(end_x, coordinate_limit) &&
        bounded(end_z, coordinate_limit);
}

std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double lattice(std::int64_t x, std::int64_t z, std::uint64_t seed) noexcept {
    const auto hash = mix(seed ^ mix(static_cast<std::uint64_t>(x)) ^
        (mix(static_cast<std::uint64_t>(z)) + 0x9e3779b97f4a7c15ULL));
    return static_cast<double>(hash >> 11U) * (2.0 / 9007199254740992.0) - 1.0;
}

double noise_value(double x, double z, std::uint64_t seed) noexcept {
    const double floor_x = std::floor(x);
    const double floor_z = std::floor(z);
    const auto ix = static_cast<std::int64_t>(floor_x);
    const auto iz = static_cast<std::int64_t>(floor_z);
    const double tx = x - floor_x;
    const double tz = z - floor_z;
    const double sx = tx * tx * tx * (tx * (tx * 6.0 - 15.0) + 10.0);
    const double sz = tz * tz * tz * (tz * (tz * 6.0 - 15.0) + 10.0);
    const double a = math::lerp(lattice(ix, iz, seed), lattice(ix + 1, iz, seed), sx);
    const double b = math::lerp(lattice(ix, iz + 1, seed), lattice(ix + 1, iz + 1, seed), sx);
    return math::lerp(a, b, sz);
}

bool valid_mesh_options(TerrainMeshOptions options) noexcept {
    return options.stride > 0 && options.stride <= 1024 &&
        bounded(options.skirt_depth, 1.0e7) && options.skirt_depth >= 0.0 &&
        std::isfinite(options.uv_metres) && options.uv_metres >= 1.0e-6 &&
        options.uv_metres <= 1.0e9;
}

std::vector<std::uint32_t> mesh_axis(std::uint32_t samples, std::uint32_t stride) {
    std::vector<std::uint32_t> result;
    for (std::uint32_t index = 0; index < samples - 1; index += stride) {
        result.push_back(index);
    }
    result.push_back(samples - 1);
    return result;
}

bool axis_aligned(const math::Mat4& matrix) noexcept {
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const double value = matrix(row, column);
            if (!std::isfinite(value)) {
                return false;
            }
            if (row != column && column != 3 && value != 0.0) {
                return false;
            }
        }
    }
    return matrix(0, 0) > 1.0e-12 && matrix(1, 1) > 1.0e-12 &&
        matrix(2, 2) > 1.0e-12 && matrix(3, 3) == 1.0;
}

std::optional<math::Mat4> current_matrix(scene::Scene& scene, ge::RuntimeHandle entity) {
    if (const auto* cached = scene.cached_world_matrix(entity)) {
        return *cached;
    }
    return scene.world_matrix(entity);
}

std::optional<math::Vec3> authored_origin(core::Kernel& kernel) {
    if (const auto* partition = kernel.services().find<scene::WorldPartition>()) {
        const auto state = partition->origin();
        return state.consistent ? std::optional{state.absolute_origin} : std::nullopt;
    }
    return math::Vec3{};
}

template<class T>
std::optional<T> parse_number(std::string_view text) noexcept {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_real(std::string_view text) noexcept {
    const auto value = parse_number<double>(text);
    return value && std::isfinite(*value) ? value : std::nullopt;
}

} // namespace

Heightfield::Heightfield(HeightfieldDesc description, HeightfieldLimits limits)
    : description_(description), limits_(limits) {
    if (!valid_description(description, limits)) {
        throw std::invalid_argument("invalid heightfield description or limits");
    }
    heights_.assign(static_cast<std::size_t>(description.columns) * description.rows,
                    description.initial_height);
}

const HeightfieldDesc& Heightfield::description() const noexcept { return description_; }
const HeightfieldLimits& Heightfield::limits() const noexcept { return limits_; }
std::span<const double> Heightfield::heights() const noexcept { return heights_; }
std::uint64_t Heightfield::revision() const noexcept { return revision_; }

std::size_t Heightfield::index(std::uint32_t x, std::uint32_t z) const noexcept {
    return static_cast<std::size_t>(z) * description_.columns + x;
}

bool Heightfield::valid_height(double value) const noexcept {
    return bounded(value, limits_.max_absolute_height);
}

math::Aabb Heightfield::bounds() const noexcept {
    const auto range = std::minmax_element(heights_.begin(), heights_.end());
    const auto& desc = description_;
    return {{desc.origin.x, desc.origin.y + *range.first, desc.origin.z},
            {desc.origin.x + static_cast<double>(desc.columns - 1) * desc.spacing.x,
             desc.origin.y + *range.second,
             desc.origin.z + static_cast<double>(desc.rows - 1) * desc.spacing.y}};
}

std::optional<double> Heightfield::height(std::uint32_t x, std::uint32_t z) const noexcept {
    if (x >= description_.columns || z >= description_.rows) {
        return std::nullopt;
    }
    return heights_[index(x, z)];
}

std::optional<TerrainSample> Heightfield::sample(
    double world_x, double world_z, SampleBoundary boundary) const noexcept {
    if (!std::isfinite(world_x) || !std::isfinite(world_z)) {
        return std::nullopt;
    }
    const auto& desc = description_;
    double gx = (world_x - desc.origin.x) / desc.spacing.x;
    double gz = (world_z - desc.origin.z) / desc.spacing.y;
    const double maximum_x = static_cast<double>(desc.columns - 1);
    const double maximum_z = static_cast<double>(desc.rows - 1);
    // Compare in authored space: normalizing an exact endpoint can round past
    // the final grid index when the origin or spacing is fractional.
    if (boundary == SampleBoundary::Reject &&
        (world_x < desc.origin.x || world_z < desc.origin.z ||
         world_x > desc.origin.x + maximum_x * desc.spacing.x ||
         world_z > desc.origin.z + maximum_z * desc.spacing.y)) {
        return std::nullopt;
    }
    gx = std::clamp(gx, 0.0, maximum_x);
    gz = std::clamp(gz, 0.0, maximum_z);
    const auto x = std::min(static_cast<std::uint32_t>(gx), desc.columns - 2);
    const auto z = std::min(static_cast<std::uint32_t>(gz), desc.rows - 2);
    const double tx = gx - static_cast<double>(x);
    const double tz = gz - static_cast<double>(z);
    const double h00 = heights_[index(x, z)];
    const double h10 = heights_[index(x + 1, z)];
    const double h01 = heights_[index(x, z + 1)];
    const double h11 = heights_[index(x + 1, z + 1)];
    TerrainSample result;
    result.position = {desc.origin.x + gx * desc.spacing.x,
        desc.origin.y + math::lerp(math::lerp(h00, h10, tx), math::lerp(h01, h11, tx), tz),
        desc.origin.z + gz * desc.spacing.y};
    result.gradient = {math::lerp(h10 - h00, h11 - h01, tz) / desc.spacing.x,
                       math::lerp(h01 - h00, h11 - h10, tx) / desc.spacing.y};
    result.normal = math::normalized(math::Vec3{-result.gradient.x, 1.0, -result.gradient.y});
    result.slope_radians = std::atan(std::hypot(result.gradient.x, result.gradient.y));
    result.cell_x = x;
    result.cell_z = z;
    result.revision = revision_;
    return result;
}

math::Vec3 Heightfield::vertex_normal(std::uint32_t x, std::uint32_t z) const noexcept {
    if (x >= description_.columns || z >= description_.rows) {
        return {0.0, 1.0, 0.0};
    }
    const auto left = x == 0 ? x : x - 1;
    const auto right = std::min(x + 1, description_.columns - 1);
    const auto near = z == 0 ? z : z - 1;
    const auto far = std::min(z + 1, description_.rows - 1);
    const double dx = (heights_[index(right, z)] - heights_[index(left, z)]) /
        (static_cast<double>(right - left) * description_.spacing.x);
    const double dz = (heights_[index(x, far)] - heights_[index(x, near)]) /
        (static_cast<double>(far - near) * description_.spacing.y);
    return math::normalized(math::Vec3{-dx, 1.0, -dz});
}

bool Heightfield::commit(std::vector<double> values) {
    if (values.size() != heights_.size() ||
        !std::all_of(values.begin(), values.end(), [this](double value) { return valid_height(value); })) {
        return false;
    }
    if (values == heights_) {
        return true;
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    heights_.swap(values);
    ++revision_;
    return true;
}

bool Heightfield::replace(std::span<const double> samples) {
    if (samples.size() != heights_.size()) {
        return false;
    }
    return commit(std::vector<double>(samples.begin(), samples.end()));
}

bool Heightfield::edit(std::span<const HeightEdit> edits) {
    if (edits.size() > limits_.max_samples) {
        return false;
    }
    for (const auto& sample_edit : edits) {
        if (sample_edit.x >= description_.columns || sample_edit.z >= description_.rows ||
            !valid_height(sample_edit.height)) {
            return false;
        }
    }
    auto values = heights_;
    for (const auto& sample_edit : edits) {
        values[index(sample_edit.x, sample_edit.z)] = sample_edit.height;
    }
    return commit(std::move(values));
}

bool Heightfield::brush(const TerrainBrush& brush_value) {
    if (!math::finite(brush_value.center) || !bounded(brush_value.radius, coordinate_limit) ||
        brush_value.radius <= 0.0 || !bounded(brush_value.strength, limits_.max_absolute_height) ||
        !bounded(brush_value.target_height, coordinate_limit + limits_.max_absolute_height) ||
        (brush_value.mode != BrushMode::Raise && brush_value.mode != BrushMode::Flatten) ||
        (brush_value.falloff != BrushFalloff::Constant && brush_value.falloff != BrushFalloff::Linear &&
         brush_value.falloff != BrushFalloff::Smooth) ||
        (brush_value.mode == BrushMode::Flatten &&
         (brush_value.strength < 0.0 || brush_value.strength > 1.0))) {
        return false;
    }
    auto values = heights_;
    const auto& desc = description_;
    for (std::uint32_t z = 0; z < desc.rows; ++z) {
        for (std::uint32_t x = 0; x < desc.columns; ++x) {
            if (brush_value.preserve_border && (x == 0 || z == 0 || x + 1 == desc.columns || z + 1 == desc.rows)) {
                continue;
            }
            const double dx = desc.origin.x + static_cast<double>(x) * desc.spacing.x - brush_value.center.x;
            const double dz = desc.origin.z + static_cast<double>(z) * desc.spacing.y - brush_value.center.y;
            const double distance = std::hypot(dx, dz);
            if (distance > brush_value.radius) {
                continue;
            }
            double weight = 1.0 - distance / brush_value.radius;
            if (brush_value.falloff == BrushFalloff::Constant) {
                weight = 1.0;
            } else if (brush_value.falloff == BrushFalloff::Smooth) {
                weight = weight * weight * (3.0 - 2.0 * weight);
            }
            double& value = values[index(x, z)];
            if (brush_value.mode == BrushMode::Raise) {
                value += brush_value.strength * weight;
            } else {
                value = math::lerp(value, brush_value.target_height - desc.origin.y,
                                   brush_value.strength * weight);
            }
        }
    }
    return commit(std::move(values));
}

bool Heightfield::smooth(SmoothingOptions options) {
    if (options.iterations > 128 || !std::isfinite(options.strength) || options.strength < 0.0 ||
        options.strength > 1.0 || heights_.size() * options.iterations > operation_limit) {
        return false;
    }
    auto current = heights_;
    auto next = heights_;
    const auto& desc = description_;
    for (std::uint32_t iteration = 0; iteration < options.iterations; ++iteration) {
        for (std::uint32_t z = 0; z < desc.rows; ++z) {
            for (std::uint32_t x = 0; x < desc.columns; ++x) {
                if (options.preserve_border && (x == 0 || z == 0 || x + 1 == desc.columns || z + 1 == desc.rows)) {
                    continue;
                }
                const auto left = x == 0 ? x : x - 1;
                const auto right = std::min(x + 1, desc.columns - 1);
                const auto near = z == 0 ? z : z - 1;
                const auto far = std::min(z + 1, desc.rows - 1);
                const double average = 0.25 * (current[index(left, z)] + current[index(right, z)] +
                    current[index(x, near)] + current[index(x, far)]);
                next[index(x, z)] = math::lerp(current[index(x, z)], average, options.strength);
            }
        }
        current.swap(next);
    }
    return commit(std::move(current));
}

bool Heightfield::generate(TerrainNoise options) {
    if (options.octaves == 0 || options.octaves > 16 || !bounded(options.frequency, 1024.0) ||
        options.frequency <= 0.0 || !valid_height(options.amplitude) || options.amplitude < 0.0 ||
        !valid_height(options.offset) || !std::isfinite(options.persistence) || options.persistence < 0.0 ||
        options.persistence > 1.0 || !std::isfinite(options.lacunarity) || options.lacunarity < 1.0 ||
        options.lacunarity > 4.0 || heights_.size() * options.octaves > operation_limit) {
        return false;
    }
    const double max_frequency = options.frequency * std::pow(options.lacunarity,
        static_cast<double>(options.octaves - 1));
    if (max_frequency * coordinate_limit > 4503599627370496.0) {
        return false;
    }
    auto values = heights_;
    const auto& desc = description_;
    for (std::uint32_t z = 0; z < desc.rows; ++z) {
        for (std::uint32_t x = 0; x < desc.columns; ++x) {
            const double px = desc.origin.x + static_cast<double>(x) * desc.spacing.x;
            const double pz = desc.origin.z + static_cast<double>(z) * desc.spacing.y;
            double frequency = options.frequency;
            double weight = 1.0;
            double weight_sum = 0.0;
            double sum = 0.0;
            for (std::uint32_t octave = 0; octave < options.octaves; ++octave) {
                double noise = noise_value(px * frequency, pz * frequency, mix(options.seed + octave));
                if (options.ridged) {
                    noise = 1.0 - 2.0 * std::abs(noise);
                }
                sum += noise * weight;
                weight_sum += weight;
                weight *= options.persistence;
                frequency *= options.lacunarity;
            }
            values[index(x, z)] = options.offset + options.amplitude * sum / weight_sum;
        }
    }
    return commit(std::move(values));
}

std::optional<ErosionResult> Heightfield::erode(ErosionOptions options) {
    if (options.iterations > 256 || !bounded(options.talus_slope, 1.0e6) || options.talus_slope < 0.0 ||
        !std::isfinite(options.transport_fraction) || options.transport_fraction < 0.0 ||
        options.transport_fraction > 0.25 || options.work_limit > operation_limit ||
        heights_.size() * options.iterations > options.work_limit) {
        return std::nullopt;
    }
    ErosionResult result;
    long double original_mass = 0.0L;
    for (const double value : heights_) {
        original_mass += static_cast<long double>(value);
    }
    result.mass_before = static_cast<double>(original_mass);
    auto values = heights_;
    std::vector<double> deltas(values.size(), 0.0);
    const auto& desc = description_;
    const auto border = [&desc](std::uint32_t x, std::uint32_t z) {
        return x == 0 || z == 0 || x + 1 == desc.columns || z + 1 == desc.rows;
    };
    for (std::uint32_t iteration = 0; iteration < options.iterations; ++iteration) {
        std::fill(deltas.begin(), deltas.end(), 0.0);
        const auto transfer = [&](std::uint32_t x, std::uint32_t z,
                                  std::uint32_t other_x, std::uint32_t other_z, double distance) {
            if (options.preserve_border && (border(x, z) || border(other_x, other_z))) {
                return;
            }
            const auto a = index(x, z);
            const auto b = index(other_x, other_z);
            const double difference = values[a] - values[b];
            const double excess = std::abs(difference) - options.talus_slope * distance;
            if (excess <= 0.0) {
                return;
            }
            const double amount = std::copysign(excess * options.transport_fraction, difference);
            if (amount != 0.0) {
                deltas[a] -= amount;
                deltas[b] += amount;
                ++result.transfers;
                result.moved_height += std::abs(amount);
            }
        };
        for (std::uint32_t z = 0; z < desc.rows; ++z) {
            for (std::uint32_t x = 0; x < desc.columns; ++x) {
                if (x + 1 < desc.columns) {
                    transfer(x, z, x + 1, z, desc.spacing.x);
                }
                if (z + 1 < desc.rows) {
                    transfer(x, z, x, z + 1, desc.spacing.y);
                }
            }
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] += deltas[i];
            if (!valid_height(values[i])) {
                return std::nullopt;
            }
        }
        ++result.iterations;
    }
    long double final_mass = 0.0L;
    for (const double value : values) {
        final_mass += static_cast<long double>(value);
    }
    result.mass_after = static_cast<double>(final_mass);
    if (!commit(std::move(values))) {
        return std::nullopt;
    }
    return result;
}

geometry::MeshData Heightfield::mesh(TerrainMeshOptions options, geometry::MeshLimits limits) const {
    if (!valid_mesh_options(options)) {
        throw std::invalid_argument("invalid terrain mesh options");
    }
    const auto xs = mesh_axis(description_.columns, options.stride);
    const auto zs = mesh_axis(description_.rows, options.stride);
    const auto top_count = xs.size() * zs.size();
    const auto perimeter = 2 * (xs.size() + zs.size()) - 4;
    const auto vertex_count = top_count + (options.skirt_depth > 0.0 ? perimeter * 4 : 0);
    const auto index_count = (xs.size() - 1) * (zs.size() - 1) * 6 +
        (options.skirt_depth > 0.0 ? perimeter * 6 : 0);
    if (vertex_count > limits.max_vertices || index_count > limits.max_indices || limits.max_submeshes < 1 ||
        vertex_count >= geometry::invalid_mesh_index || index_count >= geometry::invalid_mesh_index) {
        throw std::length_error("terrain mesh exceeds mesh limits");
    }
    geometry::MeshData result;
    result.vertices.reserve(vertex_count);
    result.indices.reserve(index_count);
    for (const auto z : zs) {
        for (const auto x : xs) {
            geometry::Vertex vertex;
            vertex.position = {description_.origin.x + static_cast<double>(x) * description_.spacing.x,
                description_.origin.y + heights_[index(x, z)],
                description_.origin.z + static_cast<double>(z) * description_.spacing.y};
            vertex.normal = vertex_normal(x, z);
            const auto tangent = math::normalized(math::Vec3{vertex.normal.y, -vertex.normal.x, 0.0});
            vertex.tangent = {tangent.x, tangent.y, tangent.z, -1.0};
            vertex.uv = {vertex.position.x / options.uv_metres, vertex.position.z / options.uv_metres};
            result.vertices.push_back(vertex);
        }
    }
    for (std::size_t z = 0; z + 1 < zs.size(); ++z) {
        for (std::size_t x = 0; x + 1 < xs.size(); ++x) {
            const auto a = static_cast<geometry::MeshIndex>(z * xs.size() + x);
            const auto b = a + 1;
            const auto c = static_cast<geometry::MeshIndex>((z + 1) * xs.size() + x);
            const auto d = c + 1;
            result.indices.insert(result.indices.end(), {a, c, b, b, c, d});
        }
    }
    if (options.skirt_depth > 0.0) {
        std::vector<geometry::MeshIndex> boundary;
        boundary.reserve(perimeter);
        for (std::size_t x = 0; x < xs.size(); ++x) {
            boundary.push_back(static_cast<geometry::MeshIndex>(x));
        }
        for (std::size_t z = 1; z < zs.size(); ++z) {
            boundary.push_back(static_cast<geometry::MeshIndex>(z * xs.size() + xs.size() - 1));
        }
        for (std::size_t x = xs.size() - 1; x > 0; --x) {
            boundary.push_back(static_cast<geometry::MeshIndex>((zs.size() - 1) * xs.size() + x - 1));
        }
        for (std::size_t z = zs.size() - 1; z > 1; --z) {
            boundary.push_back(static_cast<geometry::MeshIndex>((z - 1) * xs.size()));
        }
        for (std::size_t i = 0; i < boundary.size(); ++i) {
            auto upper_a = result.vertices[boundary[i]];
            auto upper_b = result.vertices[boundary[(i + 1) % boundary.size()]];
            const auto normal = math::normalized(math::cross(upper_b.position - upper_a.position,
                                                             math::Vec3{0.0, -1.0, 0.0}));
            const auto tangent = math::normalized(upper_b.position - upper_a.position);
            upper_a.normal = normal;
            upper_b.normal = normal;
            upper_a.tangent = {tangent.x, tangent.y, tangent.z, 1.0};
            upper_b.tangent = upper_a.tangent;
            auto lower_a = upper_a;
            auto lower_b = upper_b;
            lower_a.position.y -= options.skirt_depth;
            lower_b.position.y -= options.skirt_depth;
            const double edge_length = math::distance(upper_a.position, upper_b.position);
            upper_a.uv = {0.0, 0.0};
            upper_b.uv = {edge_length / options.uv_metres, 0.0};
            lower_a.uv = {0.0, options.skirt_depth / options.uv_metres};
            lower_b.uv = {upper_b.uv.x, lower_a.uv.y};
            const auto a = static_cast<geometry::MeshIndex>(result.vertices.size());
            result.vertices.insert(result.vertices.end(), {upper_a, upper_b, lower_a, lower_b});
            result.indices.insert(result.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
        }
    }
    result.submeshes.push_back({0, static_cast<geometry::MeshIndex>(result.indices.size()), options.material_slot});
    if (!geometry::validate_mesh(result, limits)) {
        throw std::invalid_argument("terrain mesh failed shared mesh validation");
    }
    return result;
}

std::vector<std::byte> Heightfield::encode() const {
    core::ByteWriter writer(128 + heights_.size() * sizeof(double));
    writer.write_bytes("GEHTFLD1");
    writer.write_u32(description_.columns);
    writer.write_u32(description_.rows);
    writer.write_f64(description_.origin.x);
    writer.write_f64(description_.origin.y);
    writer.write_f64(description_.origin.z);
    writer.write_f64(description_.spacing.x);
    writer.write_f64(description_.spacing.y);
    writer.write_f64(description_.initial_height);
    for (const double value : heights_) {
        writer.write_f64(value);
    }
    writer.write_u32(core::crc32(writer.bytes()));
    return writer.take();
}

Heightfield Heightfield::decode(std::span<const std::byte> bytes, HeightfieldLimits limits) {
    if (bytes.size() < 68 || bytes.size() > 128 + absolute_samples * sizeof(double)) {
        throw std::invalid_argument("invalid heightfield payload size");
    }
    core::ByteReader checksum_reader(bytes.last(4));
    if (checksum_reader.read_u32() != core::crc32(bytes.first(bytes.size() - 4))) {
        throw std::invalid_argument("heightfield checksum mismatch");
    }
    core::ByteReader reader(bytes.first(bytes.size() - 4));
    reader.expect("GEHTFLD1");
    HeightfieldDesc desc;
    desc.columns = reader.read_u32();
    desc.rows = reader.read_u32();
    desc.origin = {reader.read_f64(), reader.read_f64(), reader.read_f64()};
    desc.spacing = {reader.read_f64(), reader.read_f64()};
    desc.initial_height = reader.read_f64();
    if (!valid_description(desc, limits) ||
        reader.remaining() != static_cast<std::size_t>(desc.columns) * desc.rows * sizeof(double)) {
        throw std::invalid_argument("invalid heightfield dimensions or trailing payload");
    }
    Heightfield result(desc, limits);
    std::vector<double> samples(result.heights_.size());
    for (double& value : samples) {
        value = reader.read_f64();
    }
    reader.require_end();
    if (!result.commit(std::move(samples))) {
        throw std::invalid_argument("heightfield sample exceeds height limit");
    }
    return result;
}

struct TerrainService::Record {
    TerrainHandle handle{};
    std::string name;
    std::optional<TileCoordinate> tile;
    std::unique_ptr<Heightfield> field;
    TerrainMeshOptions mesh_options{};
    geometry::MeshHandle mesh{};
    ge::RuntimeHandle entity{};
    std::shared_ptr<const geometry::MeshLibrary::LifetimeIdentity> mesh_owner;
    const scene::Scene* scene_owner{};
    bool dirty{true};

    [[nodiscard]] bool accepts(const geometry::MeshLibrary* meshes, const scene::Scene* scene) const noexcept {
        return (!mesh_owner || (meshes != nullptr && mesh_owner == meshes->lifetime_identity())) &&
            (scene_owner == nullptr || scene_owner == scene);
    }
};

TerrainService::TerrainService(TerrainServiceLimits limits, TileLayout layout)
    : limits_(limits), layout_(layout), generations_(limits.capacity) {
    HeightfieldDesc desc;
    if (layout.cells_x == 0 || layout.cells_x > 1024 || layout.cells_z == 0 || layout.cells_z > 1024 ||
        limits.capacity == 0 || limits.capacity > 4096 || limits.total_samples < 4 ||
        limits.total_samples > 67'108'864) {
        throw std::invalid_argument("invalid terrain service limits");
    }
    desc.columns = layout.cells_x + 1;
    desc.rows = layout.cells_z + 1;
    desc.origin = layout.origin;
    desc.spacing = layout.spacing;
    if (!valid_description(desc, limits.heightfield)) {
        throw std::invalid_argument("invalid terrain tile layout");
    }
    records_.resize(static_cast<std::size_t>(limits.capacity) + 1);
}

TerrainService::~TerrainService() = default;

TerrainService::Record* TerrainService::record(TerrainHandle handle) noexcept {
    return generations_.alive({handle.index, handle.generation}) ? records_[handle.index].get() : nullptr;
}

const TerrainService::Record* TerrainService::record(TerrainHandle handle) const noexcept {
    return generations_.alive({handle.index, handle.generation}) ? records_[handle.index].get() : nullptr;
}

bool TerrainService::contains(TerrainHandle handle) const noexcept { return record(handle) != nullptr; }
const TileLayout& TerrainService::layout() const noexcept { return layout_; }

TerrainHandle TerrainService::find(std::string_view name) const noexcept {
    for (const auto& value : records_) {
        if (value && value->name == name) {
            return value->handle;
        }
    }
    return {};
}

TerrainHandle TerrainService::find(TileCoordinate coordinate) const noexcept {
    for (const auto& value : records_) {
        if (value && value->tile == coordinate) {
            return value->handle;
        }
    }
    return {};
}

const Heightfield* TerrainService::get(TerrainHandle handle) const noexcept {
    const auto* value = record(handle);
    return value ? value->field.get() : nullptr;
}

std::optional<TerrainInfo> TerrainService::inspect(TerrainHandle handle) const noexcept {
    const auto* value = record(handle);
    if (!value) {
        return std::nullopt;
    }
    return TerrainInfo{handle, value->name, value->tile, value->field->bounds(), value->field->revision(),
                       value->mesh, value->entity, value->dirty};
}

bool TerrainService::matches_render_scene(TerrainHandle handle,
    const scene::Scene& scene) const noexcept {
    const auto* value = record(handle);
    return value && value->scene_owner == &scene;
}

std::vector<TerrainHandle> TerrainService::handles() const {
    std::vector<TerrainHandle> result;
    result.reserve(generations_.size());
    for (const auto& value : records_) {
        if (value) {
            result.push_back(value->handle);
        }
    }
    return result;
}

TerrainHandle TerrainService::create(std::string_view name, Heightfield field) {
    if (name.empty() || name.size() > 256 || name.find('\0') != std::string_view::npos || find(name) ||
        !valid_description(field.description(), limits_.heightfield) ||
        field.heights().size() != static_cast<std::size_t>(field.description().columns) * field.description().rows ||
        field.heights().size() > limits_.total_samples - samples_ ||
        revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }
    for (const double height : field.heights()) {
        if (!bounded(height, limits_.heightfield.max_absolute_height)) {
            return {};
        }
    }
    auto value = std::make_unique<Record>();
    value->name = name;
    value->field = std::make_unique<Heightfield>(std::move(field));
    const auto identity = generations_.acquire();
    if (!identity) {
        return {};
    }
    value->handle = {identity->index, identity->generation};
    const auto handle = value->handle;
    samples_ += value->field->heights().size();
    records_[handle.index] = std::move(value);
    ++revision_;
    return handle;
}

namespace {

bool share_edge(const Heightfield& source, TileCoordinate source_tile,
                Heightfield& target, TileCoordinate target_tile) {
    const auto dx = static_cast<std::int64_t>(target_tile.x) - source_tile.x;
    const auto dz = static_cast<std::int64_t>(target_tile.z) - source_tile.z;
    if (std::abs(dx) > 1 || std::abs(dz) > 1 || (dx == 0 && dz == 0)) {
        return true;
    }
    const auto last_x = source.description().columns - 1;
    const auto last_z = source.description().rows - 1;
    const auto first_x = dx > 0 ? last_x : 0U;
    const auto final_x = dx < 0 ? 0U : last_x;
    const auto first_z = dz > 0 ? last_z : 0U;
    const auto final_z = dz < 0 ? 0U : last_z;
    std::vector<HeightEdit> edits;
    edits.reserve(static_cast<std::size_t>(final_x - first_x + 1) * (final_z - first_z + 1));
    for (auto z = first_z; z <= final_z; ++z) {
        for (auto x = first_x; x <= final_x; ++x) {
            const auto target_x = dx == 0 ? x : (dx > 0 ? 0U : last_x);
            const auto target_z = dz == 0 ? z : (dz > 0 ? 0U : last_z);
            edits.push_back({target_x, target_z, *source.height(x, z)});
        }
    }
    return target.edit(edits);
}

bool neighbors(TileCoordinate a, TileCoordinate b) noexcept {
    return std::abs(static_cast<std::int64_t>(a.x) - b.x) <= 1 &&
           std::abs(static_cast<std::int64_t>(a.z) - b.z) <= 1;
}

} // namespace

TerrainHandle TerrainService::create_tile(std::string_view name, TileCoordinate coordinate, TerrainNoise noise) {
    if (find(coordinate)) {
        return {};
    }
    HeightfieldDesc desc;
    desc.columns = layout_.cells_x + 1;
    desc.rows = layout_.cells_z + 1;
    desc.origin = {layout_.origin.x + static_cast<double>(coordinate.x) * layout_.cells_x * layout_.spacing.x,
        layout_.origin.y,
        layout_.origin.z + static_cast<double>(coordinate.z) * layout_.cells_z * layout_.spacing.y};
    desc.spacing = layout_.spacing;
    if (!valid_description(desc, limits_.heightfield)) {
        return {};
    }
    Heightfield field(desc, limits_.heightfield);
    if (!field.generate(noise)) {
        return {};
    }
    for (const auto& existing : records_) {
        if (existing && existing->tile && neighbors(*existing->tile, coordinate) &&
            !share_edge(*existing->field, *existing->tile, field, coordinate)) {
            return {};
        }
    }
    const auto handle = create(name, std::move(field));
    if (auto* value = record(handle)) {
        value->tile = coordinate;
        for (const auto& existing : records_) {
            if (existing && existing->tile && neighbors(*existing->tile, coordinate)) {
                changed(*existing);
            }
        }
    }
    return handle;
}

bool TerrainService::destroy(TerrainHandle handle, geometry::MeshLibrary* meshes, scene::Scene* scene) {
    auto* value = record(handle);
    if (!value || (value->mesh && meshes == nullptr) || (value->entity && scene == nullptr) ||
        !value->accepts(meshes, scene) ||
        revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    if (value->entity && scene->contains(value->entity) &&
        !scene->destroy(value->entity, scene::DestroyPolicy::DetachChildren)) {
        return false;
    }
    if (value->entity && scene->registry().alive(value->entity) && !scene->registry().destroy(value->entity)) {
        return false;
    }
    if (value->mesh) {
        (void)meshes->destroy(value->mesh);
    }
    if (value->tile) {
        for (const auto& other : records_) {
            if (other && other->tile && neighbors(*value->tile, *other->tile)) {
                changed(*other);
            }
        }
    }
    samples_ -= value->field->heights().size();
    records_[handle.index].reset();
    (void)generations_.release({handle.index, handle.generation});
    ++revision_;
    return true;
}

void TerrainService::changed(Record& value) noexcept { value.dirty = true; }

bool TerrainService::commit_field(TerrainHandle handle, std::unique_ptr<Heightfield> replacement) {
    auto* value = record(handle);
    if (!value || revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    for (const double height : replacement->heights()) {
        if (!bounded(height, limits_.heightfield.max_absolute_height)) {
            return false;
        }
    }
    if (value->field->revision() == replacement->revision()) {
        return true;
    }
    std::vector<std::pair<Record*, std::unique_ptr<Heightfield>>> staged;
    if (value->tile) {
        for (const auto& other : records_) {
            if (!other || other.get() == value || !other->tile || !neighbors(*value->tile, *other->tile)) {
                continue;
            }
            auto field = std::make_unique<Heightfield>(*other->field);
            if (!share_edge(*replacement, *value->tile, *field, *other->tile)) {
                return false;
            }
            staged.emplace_back(other.get(), std::move(field));
        }
    }
    value->field.swap(replacement);
    changed(*value);
    for (auto& [other, field] : staged) {
        other->field.swap(field);
        changed(*other);
    }
    ++revision_;
    return true;
}

bool TerrainService::mutate(TerrainHandle handle, const std::function<bool(Heightfield&)>& operation) {
    const auto* value = record(handle);
    if (!value) {
        return false;
    }
    auto replacement = std::make_unique<Heightfield>(*value->field);
    return operation(*replacement) && commit_field(handle, std::move(replacement));
}

bool TerrainService::edit(TerrainHandle handle, std::span<const HeightEdit> edits) {
    return mutate(handle, [edits](Heightfield& field) { return field.edit(edits); });
}

bool TerrainService::brush(TerrainHandle handle, const TerrainBrush& brush_value) {
    return mutate(handle, [&brush_value](Heightfield& field) { return field.brush(brush_value); });
}

bool TerrainService::smooth(TerrainHandle handle, SmoothingOptions options) {
    return mutate(handle, [options](Heightfield& field) { return field.smooth(options); });
}

bool TerrainService::generate(TerrainHandle handle, TerrainNoise options) {
    return mutate(handle, [options](Heightfield& field) { return field.generate(options); });
}

std::optional<ErosionResult> TerrainService::erode(TerrainHandle handle, ErosionOptions options) {
    std::optional<ErosionResult> result;
    const bool committed = mutate(handle, [&result, options](Heightfield& field) {
        result = field.erode(options);
        return result.has_value();
    });
    return committed ? result : std::nullopt;
}

std::optional<TerrainSample> TerrainService::sample(TerrainHandle handle, double x, double z) const noexcept {
    const auto* field = get(handle);
    return field ? field->sample(x, z) : std::nullopt;
}

std::optional<TerrainSample> TerrainService::sample(double x, double z) const noexcept {
    for (const auto& value : records_) {
        if (value) {
            if (const auto result = value->field->sample(x, z)) {
                return result;
            }
        }
    }
    return std::nullopt;
}

std::optional<TerrainSample> TerrainService::sample_world(
    scene::Scene& scene, TerrainHandle handle, double x, double z) const {
    const auto* value = record(handle);
    if (!value || value->scene_owner != &scene || !value->entity || !std::isfinite(x) || !std::isfinite(z)) {
        return std::nullopt;
    }
    const auto matrix = current_matrix(scene, value->entity);
    if (!matrix || !axis_aligned(*matrix)) {
        return std::nullopt;
    }
    auto result = value->field->sample((x - (*matrix)(0, 3)) / (*matrix)(0, 0),
                                       (z - (*matrix)(2, 3)) / (*matrix)(2, 2));
    if (!result) {
        return std::nullopt;
    }
    result->position = math::transform_point(*matrix, result->position);
    result->gradient.x *= (*matrix)(1, 1) / (*matrix)(0, 0);
    result->gradient.y *= (*matrix)(1, 1) / (*matrix)(2, 2);
    if (!math::finite(result->position) || !math::finite(result->gradient)) {
        return std::nullopt;
    }
    const double magnitude = std::max({1.0, std::abs(result->gradient.x), std::abs(result->gradient.y)});
    result->normal = math::normalized(math::Vec3{-result->gradient.x / magnitude, 1.0 / magnitude,
                                                -result->gradient.y / magnitude});
    result->slope_radians = std::atan(std::hypot(result->gradient.x, result->gradient.y));
    return result;
}

std::optional<TerrainSample> TerrainService::sample_world(scene::Scene& scene, double x, double z) const {
    for (const auto& value : records_) {
        if (value) {
            if (const auto result = sample_world(scene, value->handle, x, z)) {
                return result;
            }
        }
    }
    return std::nullopt;
}

std::optional<math::Aabb> TerrainService::world_bounds(scene::Scene& scene, TerrainHandle handle) const {
    const auto* value = record(handle);
    const auto matrix = value && value->scene_owner == &scene ? current_matrix(scene, value->entity) : std::nullopt;
    if (!matrix || !axis_aligned(*matrix)) {
        return std::nullopt;
    }
    const auto bounds = math::transform_bounds(*matrix, value->field->bounds());
    return math::valid(bounds) ? std::optional<math::Aabb>{bounds} : std::nullopt;
}

TerrainStatistics TerrainService::statistics() const noexcept {
    TerrainStatistics result;
    result.heightfields = generations_.size();
    result.samples = samples_;
    result.revision = revision_;
    for (const auto& value : records_) {
        if (value) {
            result.dirty_meshes += value->dirty ? 1U : 0U;
            result.rendered_tiles += value->mesh && value->entity ? 1U : 0U;
        }
    }
    return result;
}

bool TerrainService::set_mesh_options(TerrainHandle handle, TerrainMeshOptions options) {
    auto* value = record(handle);
    if (!value || !valid_mesh_options(options) || revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    value->mesh_options = options;
    changed(*value);
    ++revision_;
    return true;
}

bool TerrainService::rebuild(TerrainHandle handle, geometry::MeshLibrary& meshes, scene::Scene& scene,
                             math::Vec3 coordinate_origin) {
    auto* value = record(handle);
    if (!value || !value->accepts(&meshes, &scene) || scene.registry().iterating() || !math::finite(coordinate_origin)) {
        return false;
    }
    value->dirty = true;
    auto mesh = value->field->mesh(value->mesh_options, meshes.limits().mesh);
    if (value->tile) {
        const auto& desc = value->field->description();
        const auto lookup = [&](double x, double z) {
            if (auto result = value->field->sample(x, z)) {
                return *result;
            }
            for (const auto& other : records_) {
                if (other && other->tile && neighbors(*other->tile, *value->tile)) {
                    if (auto result = other->field->sample(x, z)) {
                        return *result;
                    }
                }
            }
            return *value->field->sample(x, z, SampleBoundary::Clamp);
        };
        const auto xs = mesh_axis(desc.columns, value->mesh_options.stride);
        const auto zs = mesh_axis(desc.rows, value->mesh_options.stride);
        for (std::size_t i = 0; i < xs.size() * zs.size(); ++i) {
            auto& vertex = mesh.vertices[i];
            const auto a = lookup(vertex.position.x - desc.spacing.x, vertex.position.z);
            const auto b = lookup(vertex.position.x + desc.spacing.x, vertex.position.z);
            const auto c = lookup(vertex.position.x, vertex.position.z - desc.spacing.y);
            const auto d = lookup(vertex.position.x, vertex.position.z + desc.spacing.y);
            const double dx = (b.position.y - a.position.y) / (b.position.x - a.position.x);
            const double dz = (d.position.y - c.position.y) / (d.position.z - c.position.z);
            vertex.normal = math::normalized(math::Vec3{-dx, 1.0, -dz});
            const auto tangent = math::normalized(math::Vec3{1.0, dx, 0.0});
            vertex.tangent = {tangent.x, tangent.y, tangent.z, -1.0};
        }
    }
    if (meshes.contains(value->mesh)) {
        if (!meshes.replace(value->mesh, std::move(mesh))) {
            return false;
        }
    } else {
        value->mesh = meshes.create(std::move(mesh), "terrain/" + std::to_string(handle.index) + ":" +
                                   std::to_string(handle.generation));
    }
    value->mesh_owner = meshes.lifetime_identity();
    if (!scene.contains(value->entity)) {
        if (scene.registry().alive(value->entity)) {
            if (!scene.adopt(value->entity)) {
                return false;
            }
        } else {
            math::Transform transform;
            transform.position = -coordinate_origin;
            value->entity = scene.create(value->name, {}, transform);
        }
    }
    if (!value->entity) {
        return false;
    }
    value->scene_owner = &scene;
    if (!meshes.assign(scene.registry(), value->entity, value->mesh)) {
        return false;
    }
    scene.registry().emplace_or_replace<TerrainComponent>(value->entity,
        TerrainComponent{handle, value->field->revision()});
    const auto info = meshes.inspect(value->mesh);
    if (!info || !scene.set_local_bounds(value->entity, info->bounds)) {
        return false;
    }
    if (scene.update().error != scene::SceneError::None) {
        return false;
    }
    value->dirty = false;
    return true;
}

std::size_t TerrainService::rebuild_dirty(geometry::MeshLibrary& meshes, scene::Scene& scene,
                                         std::size_t limit, math::Vec3 coordinate_origin) {
    const auto budget = std::min<std::size_t>(limit, 64);
    std::size_t rebuilt = 0;
    std::size_t attempted = 0;
    std::size_t examined = 0;
    while (examined < records_.size() && attempted < budget) {
        rebuild_cursor_ = (rebuild_cursor_ + 1) % records_.size();
        ++examined;
        auto* value = records_[rebuild_cursor_].get();
        if (value && value->accepts(&meshes, &scene) &&
            (value->dirty || !meshes.contains(value->mesh) || !scene.contains(value->entity))) {
            ++attempted;
            if (rebuild(value->handle, meshes, scene, coordinate_origin)) {
                ++rebuilt;
            }
        }
    }
    return rebuilt;
}

void TerrainService::release_render_resources(geometry::MeshLibrary& meshes, scene::Scene& scene) {
    for (const auto& value : records_) {
        if (!value || !value->accepts(&meshes, &scene)) {
            continue;
        }
        if (scene.contains(value->entity) &&
            !scene.destroy(value->entity, scene::DestroyPolicy::DetachChildren, scene::ReparentMode::KeepLocal)) {
            continue;
        }
        if (scene.registry().alive(value->entity) && !scene.registry().destroy(value->entity)) {
            continue;
        }
        (void)meshes.destroy(value->mesh);
        value->entity = {};
        value->mesh = {};
        value->scene_owner = nullptr;
        value->mesh_owner.reset();
        value->dirty = true;
    }
}

void register_heightfield(core::Kernel& kernel) {
    if (!kernel.register_module({"terrain.heightfield", "Editable tiled terrain, sampling and thermal erosion", 1})) {
        return;
    }
    kernel.services().emplace<TerrainService>();
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        registry->register_component<TerrainComponent>("terrain.heightfield");
    }
    kernel.add_command("terrain.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 3 || args.size() > 4) {
            return {false, "usage: terrain.create NAME TILE_X TILE_Z [SEED]"};
        }
        const auto x = parse_number<std::int32_t>(args[1]);
        const auto z = parse_number<std::int32_t>(args[2]);
        const auto seed = args.size() == 4 ? parse_number<std::uint64_t>(args[3]) : std::optional<std::uint64_t>{1};
        if (!x || !z || !seed) {
            return {false, "invalid tile coordinates or seed"};
        }
        TerrainNoise noise;
        noise.seed = *seed;
        const auto handle = host.services().get<TerrainService>().create_tile(args[0], {*x, *z}, noise);
        return {handle.valid(), handle ? "terrain created; display mesh queued" : "terrain creation rejected"};
    }, "Create a deterministic tile on the shared terrain lattice");
    kernel.add_command("terrain.edit", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 4) {
            return {false, "usage: terrain.edit NAME SAMPLE_X SAMPLE_Z HEIGHT_OFFSET"};
        }
        const auto x = parse_number<std::uint32_t>(args[1]);
        const auto z = parse_number<std::uint32_t>(args[2]);
        const auto height = parse_real(args[3]);
        if (!x || !z || !height) {
            return {false, "invalid sample index or height"};
        }
        auto& service = host.services().get<TerrainService>();
        const std::array<HeightEdit, 1> edits{{{*x, *z, *height}}};
        const bool result = service.edit(service.find(args[0]), edits);
        return {result, result ? "height and shared tile boundaries updated" : "terrain edit rejected"};
    }, "Set one authored height sample transactionally");
    kernel.add_command("terrain.brush", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 5) {
            return {false, "usage: terrain.brush NAME AUTHORED_X AUTHORED_Z RADIUS RAISE_AMOUNT"};
        }
        const auto x = parse_real(args[1]);
        const auto z = parse_real(args[2]);
        const auto radius = parse_real(args[3]);
        const auto amount = parse_real(args[4]);
        if (!x || !z || !radius || !amount) {
            return {false, "brush requires finite numbers"};
        }
        auto& service = host.services().get<TerrainService>();
        const bool result = service.brush(service.find(args[0]), TerrainBrush{{*x, *z}, *radius, *amount});
        return {result, result ? "terrain brush committed" : "terrain brush rejected"};
    }, "Raise or lower a tile with a smooth radial brush in authored coordinates");
    kernel.add_command("terrain.erode", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) {
            return {false, "usage: terrain.erode NAME ITERATIONS"};
        }
        const auto iterations = parse_number<std::uint32_t>(args[1]);
        if (!iterations) {
            return {false, "invalid erosion iteration count"};
        }
        auto& service = host.services().get<TerrainService>();
        ErosionOptions options;
        options.iterations = *iterations;
        const auto result = service.erode(service.find(args[0]), options);
        return {result.has_value(), result ? "transfers=" + std::to_string(result->transfers) +
            " height_moved=" + std::to_string(result->moved_height) : "erosion rejected or work budget exceeded"};
    }, "Run bounded closed-domain thermal erosion while preserving the tile border");
    kernel.add_command("terrain.sample", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 3) {
            return {false, "usage: terrain.sample NAME WORLD_X WORLD_Z"};
        }
        const auto x = parse_real(args[1]);
        const auto z = parse_real(args[2]);
        auto* scene = host.services().find<scene::Scene>();
        if (!x || !z || scene == nullptr) {
            return {false, "finite coordinates and Scene service are required"};
        }
        const auto& service = host.services().get<TerrainService>();
        const auto result = service.sample_world(*scene, service.find(args[0]), *x, *z);
        return {result.has_value(), result ? "height=" + std::to_string(result->position.y) +
            " slope_radians=" + std::to_string(result->slope_radians) : "outside terrain or unsupported/missing scene binding"};
    }, "Query bilinear height and slope with current terrain Scene translation and scale");
    kernel.add_command("terrain.rebuild", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) {
            return {false, "usage: terrain.rebuild NAME"};
        }
        auto* meshes = host.services().find<geometry::MeshLibrary>();
        auto* scene = host.services().find<scene::Scene>();
        if (meshes == nullptr || scene == nullptr) {
            return {false, "MeshLibrary and Scene services are required"};
        }
        auto& service = host.services().get<TerrainService>();
        const auto origin = authored_origin(host);
        const bool result = origin && service.rebuild(service.find(args[0]), *meshes, *scene, *origin);
        return {result, result ? "terrain mesh and scene bounds rebuilt" : "terrain rebuild rejected"};
    }, "Publish a terrain mesh and its ECS/Scene render binding immediately");
    kernel.add_system({"terrain.heightfield.rebuild", core::Phase::Visibility, -150, {}},
        [](core::Kernel& host, const core::Frame&) {
            auto& service = host.services().get<TerrainService>();
            auto* meshes = host.services().find<geometry::MeshLibrary>();
            auto* scene = host.services().find<scene::Scene>();
            const auto origin = authored_origin(host);
            if (meshes != nullptr && scene != nullptr && origin) {
                try {
                    (void)service.rebuild_dirty(*meshes, *scene, 4, *origin);
                } catch (const std::exception& error) {
                    host.diagnose(core::Diagnostic::Severity::Warning, "terrain.heightfield", error.what());
                }
            }
            const auto stats = service.statistics();
            host.metric("terrain.tiles", static_cast<double>(stats.heightfields));
            host.metric("terrain.samples", static_cast<double>(stats.samples));
            host.metric("terrain.dirty_meshes", static_cast<double>(stats.dirty_meshes));
        });
    (void)kernel.order_after("geometry.mesh.bounds", "terrain.heightfield.rebuild");
    auto* host = &kernel;
    kernel.add_shutdown("terrain.heightfield.resources", [host] {
        auto* meshes = host->services().find<geometry::MeshLibrary>();
        auto* scene = host->services().find<scene::Scene>();
        if (meshes != nullptr && scene != nullptr) {
            host->services().get<TerrainService>().release_render_resources(*meshes, *scene);
        }
    });
}

} // namespace ge::terrain
