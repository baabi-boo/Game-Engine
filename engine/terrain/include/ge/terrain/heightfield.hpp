#pragma once

#include <ge/core/identity.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/math/math.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::terrain {

struct HeightfieldLimits {
    std::uint32_t max_axis{1025};
    std::size_t max_samples{1'050'625};
    double max_absolute_height{1'000'000.0};
};

struct HeightfieldDesc {
    std::uint32_t columns{65};
    std::uint32_t rows{65};
    math::Vec3 origin{};
    math::Vec2 spacing{1.0, 1.0};
    double initial_height{};
};

enum class SampleBoundary : std::uint8_t { Reject, Clamp };

struct TerrainSample {
    math::Vec3 position{};
    math::Vec3 normal{0.0, 1.0, 0.0};
    math::Vec2 gradient{};
    double slope_radians{};
    std::uint32_t cell_x{};
    std::uint32_t cell_z{};
    std::uint64_t revision{};
};

struct HeightEdit {
    std::uint32_t x{};
    std::uint32_t z{};
    double height{}; // Offset relative to origin.y.
};

enum class BrushMode : std::uint8_t { Raise, Flatten };
enum class BrushFalloff : std::uint8_t { Constant, Linear, Smooth };

struct TerrainBrush {
    math::Vec2 center{}; // Authored X and Z.
    double radius{1.0};
    double strength{1.0};
    double target_height{}; // Authored Y for Flatten.
    BrushMode mode{BrushMode::Raise};
    BrushFalloff falloff{BrushFalloff::Smooth};
    bool preserve_border{};
};

struct SmoothingOptions {
    std::uint32_t iterations{1};
    double strength{0.5};
    bool preserve_border{true};
};

struct TerrainNoise {
    std::uint64_t seed{1};
    std::uint32_t octaves{5};
    double frequency{0.015625};
    double amplitude{10.0};
    double persistence{0.5};
    double lacunarity{2.0};
    double offset{};
    bool ridged{};
};

struct ErosionOptions {
    std::uint32_t iterations{16};
    double talus_slope{0.75};
    double transport_fraction{0.25};
    bool preserve_border{true};
    std::size_t work_limit{16'777'216};
};

struct ErosionResult {
    std::size_t transfers{};
    // Sums of height offsets, not physical mass; floating-point roundoff remains.
    double moved_height{};
    double mass_before{};
    double mass_after{};
    std::uint32_t iterations{};
};

struct TerrainMeshOptions {
    std::uint32_t stride{1};
    double skirt_depth{};
    double uv_metres{16.0};
    std::uint32_t material_slot{};
};

// Single-writer value object. Coordinates are authored axis-aligned metres, with
// +Y up. Heights store offsets from origin.y; sample() returns authored positions.
// Bilinear queries describe the continuous heightfield, not exact intersections
// against its piecewise planar display mesh. Edits stage all values before commit.
class Heightfield final {
public:
    explicit Heightfield(HeightfieldDesc description = {}, HeightfieldLimits limits = {});
    [[nodiscard]] const HeightfieldDesc& description() const noexcept;
    [[nodiscard]] const HeightfieldLimits& limits() const noexcept;
    [[nodiscard]] std::span<const double> heights() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] math::Aabb bounds() const noexcept;
    [[nodiscard]] std::optional<double> height(std::uint32_t x, std::uint32_t z) const noexcept;
    [[nodiscard]] std::optional<TerrainSample> sample(
        double world_x, double world_z, SampleBoundary boundary = SampleBoundary::Reject) const noexcept;
    [[nodiscard]] math::Vec3 vertex_normal(std::uint32_t x, std::uint32_t z) const noexcept;
    bool replace(std::span<const double> samples);
    bool edit(std::span<const HeightEdit> edits);
    bool brush(const TerrainBrush& brush);
    bool smooth(SmoothingOptions options = {});
    bool generate(TerrainNoise options = {});
    [[nodiscard]] std::optional<ErosionResult> erode(ErosionOptions options = {});
    [[nodiscard]] geometry::MeshData mesh(TerrainMeshOptions options = {},
                                        geometry::MeshLimits limits = {}) const;
    [[nodiscard]] std::vector<std::byte> encode() const;
    [[nodiscard]] static Heightfield decode(std::span<const std::byte> bytes,
                                           HeightfieldLimits limits = {});
private:
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t z) const noexcept;
    [[nodiscard]] bool valid_height(double value) const noexcept;
    bool commit(std::vector<double> values);
    HeightfieldDesc description_;
    HeightfieldLimits limits_;
    std::vector<double> heights_;
    std::uint64_t revision_{1};
};

struct TerrainHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0 && generation != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const TerrainHandle&) const = default;
};

struct TileCoordinate {
    std::int32_t x{};
    std::int32_t z{};
    auto operator<=>(const TileCoordinate&) const = default;
};

struct TerrainComponent {
    TerrainHandle terrain{};
    std::uint64_t mesh_revision{};
};

struct TerrainInfo {
    TerrainHandle handle{};
    std::string_view name;
    std::optional<TileCoordinate> tile;
    math::Aabb bounds{};
    std::uint64_t revision{};
    geometry::MeshHandle mesh{};
    ge::RuntimeHandle entity{};
    bool mesh_dirty{};
};

struct TerrainStatistics {
    std::size_t heightfields{};
    std::size_t samples{};
    std::size_t dirty_meshes{};
    std::size_t rendered_tiles{};
    std::uint64_t revision{};
};

struct TerrainServiceLimits {
    std::uint32_t capacity{256};
    std::size_t total_samples{8'388'608};
    HeightfieldLimits heightfield{};
};

struct TileLayout {
    std::uint32_t cells_x{64};
    std::uint32_t cells_z{64};
    math::Vec3 origin{};
    math::Vec2 spacing{1.0, 1.0};
};

// Tile records share one configured lattice. Each edit stages affected neighbor
// boundaries, including diagonal corners, and commits matching shared samples.
// Returned field pointers survive until replacement/destroy; do not retain them
// across service edits. Render entities are owned root nodes initially translated
// by minus authored_origin; their vertices contain authored positions.
// Each render binding retains its MeshLibrary identity and borrows its Scene.
// Keep those owners alive until destroy/release_render_resources, and pass the
// same owners for that binding. Release it before rebuilding with different owners.
// sample() queries authored coordinates;
// sample_world() includes current Scene translation and positive axis-aligned scale,
// including origin rebases. Unsupported rotation, shear or reflection returns null.
class TerrainService final {
public:
    explicit TerrainService(TerrainServiceLimits limits = {}, TileLayout layout = {});
    ~TerrainService();
    TerrainService(const TerrainService&) = delete;
    TerrainService& operator=(const TerrainService&) = delete;
    [[nodiscard]] TerrainHandle create(std::string_view name, Heightfield field);
    [[nodiscard]] TerrainHandle create_tile(std::string_view name, TileCoordinate coordinate,
                                           TerrainNoise noise = {});
    bool destroy(TerrainHandle handle, geometry::MeshLibrary* meshes = nullptr,
                 scene::Scene* scene = nullptr);
    [[nodiscard]] bool contains(TerrainHandle handle) const noexcept;
    [[nodiscard]] TerrainHandle find(std::string_view name) const noexcept;
    [[nodiscard]] TerrainHandle find(TileCoordinate coordinate) const noexcept;
    [[nodiscard]] const Heightfield* get(TerrainHandle handle) const noexcept;
    [[nodiscard]] std::optional<TerrainInfo> inspect(TerrainHandle handle) const noexcept;
    // Checks the Scene owner of an existing render binding. Consumers must
    // separately validate that the bound entity still exists before using it.
    [[nodiscard]] bool matches_render_scene(TerrainHandle handle,
        const scene::Scene& scene) const noexcept;
    [[nodiscard]] std::vector<TerrainHandle> handles() const;
    [[nodiscard]] std::optional<TerrainSample> sample(TerrainHandle handle,
                                                    double world_x, double world_z) const noexcept;
    [[nodiscard]] std::optional<TerrainSample> sample(double world_x, double world_z) const noexcept;
    [[nodiscard]] std::optional<TerrainSample> sample_world(scene::Scene& scene,
        TerrainHandle handle, double world_x, double world_z) const;
    [[nodiscard]] std::optional<TerrainSample> sample_world(scene::Scene& scene,
        double world_x, double world_z) const;
    [[nodiscard]] std::optional<math::Aabb> world_bounds(scene::Scene& scene,
                                                       TerrainHandle handle) const;
    [[nodiscard]] TerrainStatistics statistics() const noexcept;
    [[nodiscard]] const TileLayout& layout() const noexcept;
    bool edit(TerrainHandle handle, std::span<const HeightEdit> edits);
    bool brush(TerrainHandle handle, const TerrainBrush& brush);
    bool smooth(TerrainHandle handle, SmoothingOptions options = {});
    bool generate(TerrainHandle handle, TerrainNoise options = {});
    [[nodiscard]] std::optional<ErosionResult> erode(TerrainHandle handle, ErosionOptions options = {});
    bool set_mesh_options(TerrainHandle handle, TerrainMeshOptions options);
    // authored_origin only positions a new binding; existing entity poses survive rebuilds.
    bool rebuild(TerrainHandle handle, geometry::MeshLibrary& meshes, scene::Scene& scene,
                 math::Vec3 authored_origin = {});
    [[nodiscard]] std::size_t rebuild_dirty(geometry::MeshLibrary& meshes, scene::Scene& scene,
                                           std::size_t limit = 4, math::Vec3 authored_origin = {});
    void release_render_resources(geometry::MeshLibrary& meshes, scene::Scene& scene);
private:
    struct Record;
    [[nodiscard]] Record* record(TerrainHandle handle) noexcept;
    [[nodiscard]] const Record* record(TerrainHandle handle) const noexcept;
    bool mutate(TerrainHandle handle, const std::function<bool(Heightfield&)>& operation);
    bool commit_field(TerrainHandle handle, std::unique_ptr<Heightfield> replacement);
    void changed(Record& value) noexcept;
    TerrainServiceLimits limits_;
    TileLayout layout_;
    ge::GenerationPool generations_;
    std::vector<std::unique_ptr<Record>> records_;
    std::size_t samples_{};
    std::uint64_t revision_{};
    std::size_t rebuild_cursor_{};
};

void register_heightfield(core::Kernel& kernel);

} // namespace ge::terrain
