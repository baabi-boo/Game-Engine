#pragma once

#include <ge/terrain/heightfield.hpp>
#include <ge/render/material.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }

namespace ge::terrain {

struct VegetationSpeciesHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const VegetationSpeciesHandle&) const = default;
};

struct VegetationPatchHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const VegetationPatchHandle&) const = default;
};

struct VegetationLod {
    geometry::MeshHandle mesh{};
    double distance{};
    double density{1.0};
};

// Mesh/material handles are borrowed. Vegetation never destroys shared assets;
// missing resources reject rebuilds and hide affected live instances on update.
// All placement rules use authored terrain metres, before the binding transform.
struct VegetationSpeciesDesc {
    std::string name;
    std::array<VegetationLod, 4> lods{};
    std::uint32_t lod_count{1};
    std::vector<render::MaterialHandle> materials;
    std::uint64_t seed{1};
    double density{0.1};
    double minimum_spacing{1.0};
    double minimum_altitude{-1'000'000.0};
    double maximum_altitude{1'000'000.0};
    double minimum_slope{};
    double maximum_slope{math::radians(45.0)};
    double altitude_feather{};
    double slope_feather{};
    math::Vec3 minimum_scale{0.8, 0.8, 0.8};
    math::Vec3 maximum_scale{1.2, 1.2, 1.2};
    double normal_alignment{1.0};
    double random_tilt{};
    double surface_offset{};
    double cull_distance{500.0};
    double fade_distance{400.0};
    double wind_amplitude{};
    double wind_frequency{1.0};
};

struct VegetationExclusion {
    math::Vec2 minimum{};
    math::Vec2 maximum{};
    double feather{};
};

struct VegetationPatchDesc {
    std::string name;
    TerrainHandle terrain{};
    VegetationSpeciesHandle species{};
    math::Vec2 minimum{};
    math::Vec2 maximum{64.0, 64.0};
    std::uint64_t seed{1};
    double density_multiplier{1.0};
    std::uint32_t maximum_instances{4096};
    std::uint32_t attempts_per_instance{6};
    std::uint64_t layers{1};
    bool enabled{true};
    std::vector<VegetationExclusion> exclusions;
};

struct VegetationLimits {
    std::uint32_t species{128};
    std::uint32_t patches{512};
    std::size_t total_instances{32768};
    std::size_t instances_per_patch{8192};
    std::size_t candidates_per_patch{131072};
    std::size_t spacing_comparisons{2'097'152};
    std::size_t exclusions_per_patch{64};
    std::size_t materials_per_species{32};
};

enum class VegetationError : std::uint8_t {
    None,
    InvalidDescription,
    MissingSpecies,
    MissingPatch,
    MissingTerrain,
    MissingBinding,
    MissingResource,
    Capacity,
    WorkLimit,
    SceneRejected
};

[[nodiscard]] std::string_view describe(VegetationError error) noexcept;
[[nodiscard]] bool valid(const VegetationSpeciesDesc& description) noexcept;
[[nodiscard]] bool valid(const VegetationPatchDesc& description) noexcept;

struct VegetationDistribution {
    std::size_t candidates{};
    std::size_t accepted{};
    std::size_t outside_terrain{};
    std::size_t rule_rejections{};
    std::size_t spacing_rejections{};
    std::size_t spacing_comparisons{};
    std::size_t requested{};
    bool truncated{};
};

struct VegetationBuildResult {
    VegetationError error{VegetationError::None};
    VegetationDistribution distribution{};
    std::uint64_t revision{};
    bool changed{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == VegetationError::None;
    }
};

// Exposed instance metadata supports later batching without duplicating assets.
// authored_pose is immutable between rebuilds; wind always starts from this pose.
struct VegetationInstance {
    ge::RuntimeHandle entity{};
    std::uint64_t candidate_id{};
    math::Transform authored_pose{};
    math::Vec3 normal{0.0, 1.0, 0.0};
    double density_rank{};
    double wind_phase{};
    std::uint32_t lod{};
    bool visible{true};
};

struct VegetationInstanceComponent {
    VegetationPatchHandle patch{};
    VegetationSpeciesHandle species{};
    std::uint64_t candidate_id{};
    std::uint64_t patch_revision{};
    double density_rank{};
    double wind_phase{};
    std::uint32_t lod{};
};

struct VegetationSpeciesInfo {
    VegetationSpeciesHandle handle{};
    const VegetationSpeciesDesc* description{};
    std::uint64_t revision{};
    std::size_t patches{};
};

struct VegetationPatchInfo {
    VegetationPatchHandle handle{};
    const VegetationPatchDesc* description{};
    ge::RuntimeHandle root{};
    ge::RuntimeHandle terrain_entity{};
    math::Aabb authored_bounds{};
    VegetationDistribution distribution{};
    std::uint64_t revision{};
    std::uint64_t terrain_revision{};
    std::uint64_t species_revision{};
    bool dirty{true};
};

struct VegetationView {
    math::Vec3 position{};
    std::optional<math::Frustum> frustum;
    std::uint64_t layers{~std::uint64_t{0}};
    double density{1.0};
    double lod_distance_scale{1.0};
    bool enabled{};
};

struct VegetationStatistics {
    std::size_t species{};
    std::size_t patches{};
    std::size_t instances{};
    std::size_t visible{};
    std::size_t dirty_patches{};
    std::size_t missing_entities{};
    std::uint64_t revision{};
    std::uint64_t rebuilds{};
};

// Single-writer service. A patch owns an identity root beneath the terrain's
// binding entity and each instance beneath that root. Local authored placement
// follows origin rebases automatically. Spacing applies within each patch.
// Rebuild stages a complete replacement hierarchy and preserves the previous
// hierarchy on failure. A built patch remains bound to its original Scene and
// MeshLibrary lifetime; destroy and recreate it to change owners. Explicit
// clear(scene) runs before that Scene's destruction; this service's destructor
// never dereferences other services.
class VegetationService final {
public:
    explicit VegetationService(VegetationLimits limits = {});
    ~VegetationService();
    VegetationService(const VegetationService&) = delete;
    VegetationService& operator=(const VegetationService&) = delete;
    [[nodiscard]] VegetationSpeciesHandle add_species(VegetationSpeciesDesc description);
    bool replace_species(VegetationSpeciesHandle handle, VegetationSpeciesDesc description);
    bool remove_species(VegetationSpeciesHandle handle);
    [[nodiscard]] VegetationSpeciesHandle find_species(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<VegetationSpeciesInfo> inspect_species(
        VegetationSpeciesHandle handle) const noexcept;
    [[nodiscard]] std::vector<VegetationSpeciesHandle> species() const;
    [[nodiscard]] VegetationPatchHandle create_patch(VegetationPatchDesc description);
    bool replace_patch(VegetationPatchHandle handle, VegetationPatchDesc description);
    bool destroy_patch(VegetationPatchHandle handle, scene::Scene& scene);
    [[nodiscard]] VegetationPatchHandle find_patch(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<VegetationPatchInfo> inspect_patch(
        VegetationPatchHandle handle) const noexcept;
    [[nodiscard]] std::vector<VegetationPatchHandle> patches() const;
    [[nodiscard]] std::span<const VegetationInstance> instances(
        VegetationPatchHandle handle) const noexcept;
    bool mark_dirty(VegetationPatchHandle handle) noexcept;
    void mark_all_dirty() noexcept;
    [[nodiscard]] VegetationBuildResult rebuild(VegetationPatchHandle handle,
        const TerrainService& terrain, const geometry::MeshLibrary& meshes,
        const render::MaterialLibrary* materials, scene::Scene& scene);
    [[nodiscard]] std::size_t refresh(const TerrainService& terrain,
        const geometry::MeshLibrary& meshes, const render::MaterialLibrary* materials,
        scene::Scene& scene, std::size_t patch_budget = 2);
    bool set_view(VegetationView view) noexcept;
    [[nodiscard]] const VegetationView& view() const noexcept;
    bool set_wind(math::Vec2 direction, double strength) noexcept;
    void update(scene::Scene& scene, const geometry::MeshLibrary& meshes,
                const render::MaterialLibrary* materials, double elapsed_seconds);
    void clear(scene::Scene& scene);
    [[nodiscard]] VegetationStatistics statistics() const noexcept;
    [[nodiscard]] const VegetationLimits& limits() const noexcept;
private:
    struct Species;
    struct Patch;
    [[nodiscard]] Species* species_record(VegetationSpeciesHandle handle) noexcept;
    [[nodiscard]] const Species* species_record(VegetationSpeciesHandle handle) const noexcept;
    [[nodiscard]] Patch* patch_record(VegetationPatchHandle handle) noexcept;
    [[nodiscard]] const Patch* patch_record(VegetationPatchHandle handle) const noexcept;
    void changed() noexcept;
    VegetationLimits limits_;
    ge::GenerationPool species_generations_;
    ge::GenerationPool patch_generations_;
    std::vector<std::unique_ptr<Species>> species_;
    std::vector<std::unique_ptr<Patch>> patches_;
    VegetationView view_{};
    math::Vec2 wind_direction_{1.0, 0.0};
    double wind_strength_{1.0};
    std::size_t instances_{};
    std::size_t visible_{};
    std::size_t missing_entities_{};
    std::size_t refresh_cursor_{};
    std::uint64_t revision_{};
    std::uint64_t rebuilds_{};
};

void register_vegetation(core::Kernel& kernel);

} // namespace ge::terrain
