#pragma once

#include <ge/core/identity.hpp>
#include <ge/math/math.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::ecs { class Registry; }

namespace ge::geometry {

using MeshIndex = std::uint32_t;
inline constexpr MeshIndex invalid_mesh_index = std::numeric_limits<MeshIndex>::max();

struct Vertex {
    math::Vec3 position{};
    math::Vec3 normal{0.0, 1.0, 0.0};
    math::Vec4 tangent{1.0, 0.0, 0.0, 1.0};
    math::Vec4 color{1.0, 1.0, 1.0, 1.0};
    math::Vec2 uv{};
    bool operator==(const Vertex&) const = default;
};

struct SubmeshRange {
    MeshIndex first_index{};
    MeshIndex index_count{};
    std::uint32_t material_slot{};
    bool operator==(const SubmeshRange&) const = default;
};

// Counterclockwise triangles face outward. Explicit submeshes form a complete,
// ordered partition of indices; an empty submesh list means one implicit range.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<MeshIndex> indices;
    std::vector<SubmeshRange> submeshes;
    [[nodiscard]] std::size_t triangle_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t allocated_bytes() const noexcept;
};

struct MeshLimits {
    // These defaults are also the absolute algorithm ceilings; callers may lower them.
    std::size_t max_vertices{1'048'576};
    std::size_t max_indices{6'291'456};
    std::size_t max_submeshes{4096}; // Includes an implicit range when indices are present.
    std::uint32_t max_tessellation{1024};
};

enum class MeshError : std::uint8_t {
    None,
    InvalidLimits,
    CapacityExceeded,
    InvalidIndexCount,
    NonFiniteVertex,
    IndexOutOfRange,
    InvalidSubmesh,
    DegenerateTriangle,
    InvalidArgument,
    NonFiniteResult,
    SingularTransform
};

struct MeshValidation {
    MeshError error{MeshError::None};
    std::size_t element{};
    std::string_view message{};
    [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] MeshValidation validate_mesh(const MeshData& mesh, MeshLimits limits = {},
                                          bool reject_degenerate = false) noexcept;
[[nodiscard]] std::optional<math::Aabb> mesh_bounds(const MeshData& mesh) noexcept;

enum class NormalWeighting : std::uint8_t { Area, Uniform, CornerAngle };

// In-place operations publish only after validation and arithmetic succeed.
// Allocation failures propagate; the original mesh remains unchanged.
[[nodiscard]] MeshValidation generate_normals(MeshData& mesh,
                                              NormalWeighting weighting = NormalWeighting::Area,
                                              MeshLimits limits = {});
[[nodiscard]] MeshValidation generate_tangents(MeshData& mesh, MeshLimits limits = {});
[[nodiscard]] MeshValidation transform_mesh(MeshData& mesh, const math::Mat4& transform,
                                           MeshLimits limits = {});
[[nodiscard]] MeshValidation remap_vertices(MeshData& mesh,
                                           std::span<const MeshIndex> new_to_old,
                                           MeshLimits limits = {});

struct WeldOptions {
    double position_tolerance{1.0e-6};
    double attribute_tolerance{1.0e-6};
    bool position_only{};
    bool remove_degenerate_triangles{true}; // Removes triangles with repeated remapped indices.
    std::size_t comparison_limit{16'777'216};
};

struct MeshWeldResult {
    MeshData mesh;
    std::vector<MeshIndex> old_to_new;
    std::size_t removed_triangles{};
};

struct MeshInstance {
    const MeshData* mesh{};
    math::Mat4 transform{};
};

// Value-producing algorithms throw invalid_argument for malformed inputs and
// length_error for configured limits. They never modify their inputs.
[[nodiscard]] MeshWeldResult weld_vertices(const MeshData& mesh, WeldOptions options = {},
                                          MeshLimits limits = {});
[[nodiscard]] MeshData merge_meshes(std::span<const MeshInstance> instances,
                                   MeshLimits limits = {});

struct TopologyEdge {
    std::array<MeshIndex, 2> vertices{};
    MeshIndex first_face{};
    MeshIndex face_count{};
    bool consistent_winding{true};
};

struct MeshTopology {
    std::vector<TopologyEdge> edges;
    std::vector<MeshIndex> edge_faces;
    std::vector<std::array<MeshIndex, 3>> triangle_neighbors;
    std::vector<MeshIndex> vertex_face_offsets;
    std::vector<MeshIndex> vertex_faces;
    std::vector<MeshIndex> triangle_components;
    std::size_t boundary_edges{};
    std::size_t nonmanifold_edges{};
    std::size_t inconsistent_edges{};
    std::size_t component_count{};
    // Edge-manifold classification; vertex links and self-intersections are not tested.
    [[nodiscard]] bool closed_manifold() const noexcept;
};

[[nodiscard]] MeshTopology build_topology(const MeshData& mesh, MeshLimits limits = {});

struct PlaneOptions {
    double width{1.0};
    double depth{1.0};
    std::uint32_t segments_x{1};
    std::uint32_t segments_z{1};
};

struct SphereOptions {
    double radius{0.5};
    std::uint32_t longitudes{32};
    std::uint32_t latitudes{16};
};

struct CylinderOptions {
    double radius{0.5};
    double height{1.0};
    std::uint32_t radial_segments{32};
    std::uint32_t height_segments{1};
    bool capped{true};
};

[[nodiscard]] MeshData make_plane(PlaneOptions options = {}, MeshLimits limits = {});
[[nodiscard]] MeshData make_box(math::Vec3 size = {1.0, 1.0, 1.0}, MeshLimits limits = {});
[[nodiscard]] MeshData make_uv_sphere(SphereOptions options = {}, MeshLimits limits = {});
[[nodiscard]] MeshData make_cylinder(CylinderOptions options = {}, MeshLimits limits = {});

struct MeshHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0 && generation != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const MeshHandle&) const = default;
};

struct MeshComponent {
    MeshHandle mesh{};
    std::uint32_t first_submesh{};
    std::uint32_t submesh_count{}; // Zero selects every range from first_submesh onward.
    bool visible{true};
};

struct MeshLibraryLimits {
    std::uint32_t capacity{4096};
    std::size_t resident_bytes{268'435'456};
    MeshLimits mesh{};
};

struct MeshInfo {
    MeshHandle handle{};
    std::string_view name;
    std::size_t vertices{};
    std::size_t triangles{};
    std::size_t submeshes{};
    std::size_t allocated_bytes{};
    math::Aabb bounds{};
    std::uint64_t revision{};
};

struct MeshStatistics {
    std::size_t meshes{};
    std::size_t vertices{};
    std::size_t triangles{};
    std::size_t resident_bytes{};
    std::uint32_t capacity{};
    std::uint64_t revision{};
};

// Single-writer owner. Handles are scoped to this library lifetime; immutable
// borrowed data and name views survive until that handle is replaced/destroyed.
class MeshLibrary final {
public:
    explicit MeshLibrary(MeshLibraryLimits limits = {});
    ~MeshLibrary();
    MeshLibrary(const MeshLibrary&) = delete;
    MeshLibrary& operator=(const MeshLibrary&) = delete;
    MeshLibrary(MeshLibrary&&) = delete;
    MeshLibrary& operator=(MeshLibrary&&) = delete;
    [[nodiscard]] MeshHandle create(MeshData mesh, std::string_view name = {});
    bool replace(MeshHandle handle, MeshData mesh);
    bool destroy(MeshHandle handle) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool contains(MeshHandle handle) const noexcept;
    [[nodiscard]] const MeshData* get(MeshHandle handle) const noexcept;
    [[nodiscard]] MeshHandle find(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<MeshInfo> inspect(MeshHandle handle) const noexcept;
    [[nodiscard]] std::vector<MeshHandle> handles() const;
    [[nodiscard]] MeshStatistics statistics() const noexcept;
    [[nodiscard]] const MeshLibraryLimits& limits() const noexcept;
    bool assign(ecs::Registry& registry, ge::RuntimeHandle entity, MeshHandle mesh,
                std::uint32_t first_submesh = 0, std::uint32_t submesh_count = 0) const;
private:
    struct Record;
    [[nodiscard]] std::unique_ptr<Record> prepare(MeshData mesh, std::string_view name) const;
    MeshLibraryLimits limits_;
    ge::GenerationPool generations_;
    std::vector<std::unique_ptr<Record>> records_;
    std::size_t resident_bytes_{};
    std::uint64_t revision_{};
};

void register_mesh(core::Kernel& kernel);

} // namespace ge::geometry
