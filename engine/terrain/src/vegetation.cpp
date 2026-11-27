#include <ge/terrain/vegetation.hpp>

#include <ge/core/runtime.hpp>
#include <ge/render/camera.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace ge::terrain {
namespace {

constexpr double coordinate_limit = 1.0e9;
constexpr std::size_t absolute_instance_limit = 262144;
constexpr std::size_t absolute_candidate_limit = 1'048'576;

bool bounded(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool bounded_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte >= 32 && byte != 127;
    });
}

void advance_revision(std::uint64_t& revision) noexcept {
    if (revision != std::numeric_limits<std::uint64_t>::max()) {
        ++revision;
    }
}

std::uint64_t scramble(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double random_unit(std::uint64_t seed, std::uint64_t ordinal, std::uint64_t channel) noexcept {
    const auto bits = scramble(seed ^ scramble(ordinal) ^ scramble(channel));
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

double interval_probability(double value, double low, double high, double feather) noexcept {
    if (value < low || value > high) {
        return 0.0;
    }
    if (feather <= 0.0) {
        return 1.0;
    }
    return math::smoothstep(0.0, feather, std::min(value - low, high - value));
}

double exclusion_probability(math::Vec2 point, const VegetationPatchDesc& patch) noexcept {
    double probability = 1.0;
    for (const auto& area : patch.exclusions) {
        const auto nearest = math::clamp(point, area.minimum, area.maximum);
        const double distance = math::length(point - nearest);
        if (distance == 0.0) {
            return 0.0;
        }
        if (area.feather > 0.0) {
            probability *= math::smoothstep(0.0, area.feather, distance);
        }
    }
    return probability;
}

bool resources_available(const VegetationSpeciesDesc& species,
                         const geometry::MeshLibrary& meshes,
                         const render::MaterialLibrary* materials,
                         math::Aabb& bounds) noexcept {
    bounds = {};
    for (std::uint32_t index = 0; index < species.lod_count; ++index) {
        const auto info = meshes.inspect(species.lods[index].mesh);
        if (!info || info->triangles == 0 || !math::valid(info->bounds)) {
            return false;
        }
        bounds = math::merge(bounds, info->bounds);
    }
    for (const auto material : species.materials) {
        if (material && (!materials || !materials->get(material))) {
            return false;
        }
    }
    return math::valid(bounds);
}

const render::MaterialLibrary* material_library(core::Kernel& host) noexcept {
    const auto* service = host.services().find<render::MaterialService>();
    return service ? &service->materials() : nullptr;
}

bool enabled_hierarchy(const scene::Scene& scene, ge::RuntimeHandle entity) noexcept {
    std::size_t depth{};
    while (entity && depth++ < scene.limits().max_depth) {
        if (!scene.contains(entity) || !scene.registry().enabled(entity)) {
            return false;
        }
        entity = scene.parent(entity);
    }
    return !entity;
}

struct Candidate {
    std::uint64_t ordinal{};
    std::uint64_t priority{};
    math::Vec2 position{};
};

using SpacingCell = std::pair<std::int64_t, std::int64_t>;

SpacingCell spacing_cell(math::Vec2 point, math::Vec2 origin, double spacing) noexcept {
    return {static_cast<std::int64_t>(std::floor((point.x - origin.x) / spacing)),
            static_cast<std::int64_t>(std::floor((point.y - origin.y) / spacing))};
}

VegetationError distribute(const VegetationPatchDesc& patch,
                          const VegetationSpeciesDesc& species,
                          const Heightfield& field, const VegetationLimits& limits,
                          std::vector<VegetationInstance>& instances,
                          VegetationDistribution& statistics) {
    const auto size = patch.maximum - patch.minimum;
    const double expected = size.x * size.y * species.density * patch.density_multiplier;
    const auto seed = scramble(patch.seed ^ scramble(species.seed));
    const auto maximum = std::min(static_cast<std::size_t>(patch.maximum_instances),
                                  limits.instances_per_patch);
    const double limited = std::min(expected, static_cast<double>(maximum));
    auto requested = static_cast<std::size_t>(std::floor(limited));
    if (requested < maximum && random_unit(seed, 0, 0) < limited - std::floor(limited)) {
        ++requested;
    }
    statistics.requested = requested;
    statistics.truncated = expected > static_cast<double>(maximum);
    if (requested == 0) {
        return VegetationError::None;
    }
    const auto candidate_count = std::min(limits.candidates_per_patch,
        requested * static_cast<std::size_t>(patch.attempts_per_instance));
    const double preferred_columns = std::sqrt(static_cast<double>(candidate_count) * size.x / size.y);
    const auto columns = static_cast<std::size_t>(math::clamp(std::floor(preferred_columns),
        1.0, static_cast<double>(candidate_count)));
    const auto rows = std::max(std::size_t{1}, candidate_count / columns);
    std::vector<Candidate> candidates;
    candidates.reserve(columns * rows);
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            const auto ordinal = static_cast<std::uint64_t>(row * columns + column + 1);
            const double x = (static_cast<double>(column) + random_unit(seed, ordinal, 1)) /
                             static_cast<double>(columns);
            const double z = (static_cast<double>(row) + random_unit(seed, ordinal, 2)) /
                             static_cast<double>(rows);
            candidates.push_back({ordinal, scramble(seed ^ ordinal),
                {patch.minimum.x + size.x * x, patch.minimum.y + size.y * z}});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        if (left.priority != right.priority) {
            return left.priority < right.priority;
        }
        return left.ordinal < right.ordinal;
    });
    instances.reserve(requested);
    std::map<SpacingCell, std::vector<std::size_t>> grid;
    const double spacing_squared = species.minimum_spacing * species.minimum_spacing;
    for (const auto& candidate : candidates) {
        if (instances.size() == requested) {
            break;
        }
        ++statistics.candidates;
        const auto sample = field.sample(candidate.position.x, candidate.position.y);
        if (!sample) {
            ++statistics.outside_terrain;
            continue;
        }
        double probability = interval_probability(sample->position.y, species.minimum_altitude,
            species.maximum_altitude, species.altitude_feather);
        probability *= interval_probability(sample->slope_radians, species.minimum_slope,
            species.maximum_slope, species.slope_feather);
        probability *= exclusion_probability(candidate.position, patch);
        if (random_unit(seed, candidate.ordinal, 3) >= probability) {
            ++statistics.rule_rejections;
            continue;
        }
        SpacingCell cell{};
        bool separated = true;
        if (species.minimum_spacing > 0.0) {
            cell = spacing_cell(candidate.position, patch.minimum, species.minimum_spacing);
            for (std::int64_t dz = -1; dz <= 1 && separated; ++dz) {
                for (std::int64_t dx = -1; dx <= 1 && separated; ++dx) {
                    const auto found = grid.find({cell.first + dx, cell.second + dz});
                    if (found == grid.end()) {
                        continue;
                    }
                    for (const auto index : found->second) {
                        if (statistics.spacing_comparisons >= limits.spacing_comparisons) {
                            return VegetationError::WorkLimit;
                        }
                        ++statistics.spacing_comparisons;
                        const auto position = instances[index].authored_pose.position;
                        const math::Vec2 delta{position.x - candidate.position.x,
                                              position.z - candidate.position.y};
                        if (math::length_squared(delta) < spacing_squared) {
                            separated = false;
                            break;
                        }
                    }
                }
            }
        }
        if (!separated) {
            ++statistics.spacing_rejections;
            continue;
        }
        VegetationInstance instance;
        instance.candidate_id = candidate.ordinal;
        instance.normal = sample->normal;
        instance.authored_pose.position = sample->position;
        instance.authored_pose.position.y += species.surface_offset;
        const double scale_rank = random_unit(seed, candidate.ordinal, 4);
        instance.authored_pose.scale = math::lerp(species.minimum_scale, species.maximum_scale, scale_rank);
        const auto alignment = math::slerp(math::Quat{},
            math::from_to({0.0, 1.0, 0.0}, sample->normal), species.normal_alignment);
        const auto yaw = math::from_axis_angle({0.0, 1.0, 0.0},
            math::tau * random_unit(seed, candidate.ordinal, 5));
        const double azimuth = math::tau * random_unit(seed, candidate.ordinal, 6);
        const double tilt = species.random_tilt * std::sqrt(random_unit(seed, candidate.ordinal, 7));
        instance.authored_pose.rotation = math::normalized(alignment * yaw *
            math::from_axis_angle({std::cos(azimuth), 0.0, std::sin(azimuth)}, tilt));
        instance.density_rank = random_unit(seed, candidate.ordinal, 8);
        instance.wind_phase = math::tau * random_unit(seed, candidate.ordinal, 9);
        if (!math::finite(instance.authored_pose)) {
            return VegetationError::InvalidDescription;
        }
        if (species.minimum_spacing > 0.0) {
            grid[cell].push_back(instances.size());
        }
        instances.push_back(instance);
    }
    statistics.accepted = instances.size();
    statistics.truncated = statistics.truncated || instances.size() < requested;
    return VegetationError::None;
}

// Registry deletion does not allocate. Pruning Scene afterwards is best effort
// during exception unwinding; dead handles are already unusable at that point.
class StagedEntities final {
public:
    StagedEntities(scene::Scene& owner, std::size_t capacity) : scene_(owner) {
        entities_.reserve(capacity);
    }
    ~StagedEntities() {
        if (!committed_) {
            for (auto entity = entities_.rbegin(); entity != entities_.rend(); ++entity) {
                try {
                    (void)scene_.registry().destroy(*entity);
                } catch (...) {
                }
            }
            try {
                (void)scene_.update();
            } catch (...) {
            }
        }
    }
    void add(ge::RuntimeHandle entity) { entities_.push_back(entity); }
    void commit() noexcept { committed_ = true; }
private:
    scene::Scene& scene_;
    std::vector<ge::RuntimeHandle> entities_;
    bool committed_{};
};

std::optional<double> parse_real(std::string_view text) noexcept {
    double result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::uint64_t> parse_seed(std::string_view text) noexcept {
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return result;
}

} // namespace

std::string_view describe(VegetationError error) noexcept {
    switch (error) {
    case VegetationError::None: return "ok";
    case VegetationError::InvalidDescription: return "invalid vegetation description";
    case VegetationError::MissingSpecies: return "species is missing or stale";
    case VegetationError::MissingPatch: return "patch is missing or stale";
    case VegetationError::MissingTerrain: return "terrain is missing or stale";
    case VegetationError::MissingBinding: return "terrain has no live Scene binding";
    case VegetationError::MissingResource: return "mesh or material is missing or stale";
    case VegetationError::Capacity: return "vegetation or Scene capacity exhausted";
    case VegetationError::WorkLimit: return "spacing comparison budget exhausted";
    case VegetationError::SceneRejected: return "Scene rejected vegetation hierarchy";
    }
    return "unknown vegetation error";
}

bool valid(const VegetationSpeciesDesc& value) noexcept {
    if (!bounded_name(value.name) || value.lod_count == 0 || value.lod_count > value.lods.size() ||
        value.materials.size() > 256 || !bounded(value.density, 0.0, 1.0e5) ||
        !bounded(value.minimum_spacing, 0.0, 1.0e6) ||
        (value.minimum_spacing > 0.0 && value.minimum_spacing < 1.0e-4) ||
        !bounded(value.minimum_altitude, -coordinate_limit, coordinate_limit) ||
        !bounded(value.maximum_altitude, value.minimum_altitude, coordinate_limit) ||
        !bounded(value.minimum_slope, 0.0, math::pi * 0.5) ||
        !bounded(value.maximum_slope, value.minimum_slope, math::pi * 0.5) ||
        !bounded(value.altitude_feather, 0.0, coordinate_limit) ||
        !bounded(value.slope_feather, 0.0, math::pi * 0.5) ||
        !bounded(value.normal_alignment, 0.0, 1.0) ||
        !bounded(value.random_tilt, 0.0, math::pi * 0.25) ||
        !bounded(value.surface_offset, -1000.0, 1000.0) ||
        !bounded(value.cull_distance, 0.01, 1.0e7) ||
        !bounded(value.fade_distance, 0.0, value.cull_distance) ||
        !bounded(value.wind_amplitude, 0.0, 0.5) ||
        !bounded(value.wind_frequency, 0.0, 20.0)) {
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!bounded(value.minimum_scale[axis], 0.001, 1000.0) ||
            !bounded(value.maximum_scale[axis], value.minimum_scale[axis], 1000.0)) {
            return false;
        }
    }
    double previous_distance = -1.0;
    double previous_density = 1.0;
    for (std::uint32_t index = 0; index < value.lod_count; ++index) {
        const auto& lod = value.lods[index];
        if (!lod.mesh || !bounded(lod.distance, 0.0, value.cull_distance) ||
            lod.distance <= previous_distance || !bounded(lod.density, 0.0, previous_density)) {
            return false;
        }
        previous_distance = lod.distance;
        previous_density = lod.density;
    }
    return value.lods[0].distance == 0.0;
}

bool valid(const VegetationPatchDesc& value) noexcept {
    if (!bounded_name(value.name) || !value.terrain || !value.species ||
        !math::finite(value.minimum) || !math::finite(value.maximum) ||
        !bounded(value.minimum.x, -coordinate_limit, coordinate_limit) ||
        !bounded(value.minimum.y, -coordinate_limit, coordinate_limit) ||
        !bounded(value.maximum.x, -coordinate_limit, coordinate_limit) ||
        !bounded(value.maximum.y, -coordinate_limit, coordinate_limit) ||
        !bounded(value.maximum.x - value.minimum.x, 1.0e-4, 1.0e6) ||
        !bounded(value.maximum.y - value.minimum.y, 1.0e-4, 1.0e6) ||
        !bounded(value.density_multiplier, 0.0, 1000.0) ||
        value.maximum_instances > absolute_instance_limit || value.attempts_per_instance == 0 ||
        value.attempts_per_instance > 64 || value.exclusions.size() > 256) {
        return false;
    }
    for (const auto& area : value.exclusions) {
        if (!math::finite(area.minimum) || !math::finite(area.maximum) ||
            area.minimum.x > area.maximum.x || area.minimum.y > area.maximum.y ||
            !bounded(area.feather, 0.0, 1.0e6)) {
            return false;
        }
    }
    return true;
}

struct VegetationService::Species {
    VegetationSpeciesHandle handle{};
    VegetationSpeciesDesc description;
    std::uint64_t revision{1};
};

struct VegetationService::Patch {
    VegetationPatchHandle handle{};
    VegetationPatchDesc description;
    std::unique_ptr<VegetationSpeciesDesc> built_species;
    VegetationSpeciesHandle built_species_handle{};
    std::vector<VegetationInstance> instances;
    ge::RuntimeHandle root{};
    ge::RuntimeHandle terrain_entity{};
    math::Aabb bounds{};
    VegetationDistribution distribution{};
    std::uint64_t revision{};
    std::uint64_t terrain_revision{};
    std::uint64_t species_revision{};
    std::uint64_t layers{1};
    bool dirty{true};
};

VegetationService::VegetationService(VegetationLimits limits)
    : limits_(limits), species_generations_(limits.species), patch_generations_(limits.patches) {
    if (limits.species > 4096 || limits.patches > 4096 ||
        limits.total_instances > absolute_instance_limit || limits.instances_per_patch > 65536 ||
        limits.instances_per_patch > limits.total_instances ||
        limits.candidates_per_patch > absolute_candidate_limit ||
        limits.candidates_per_patch < limits.instances_per_patch ||
        limits.spacing_comparisons > 16'777'216 || limits.exclusions_per_patch > 256 ||
        limits.materials_per_species > 256) {
        throw std::invalid_argument("invalid vegetation limits");
    }
    species_.resize(limits.species);
    patches_.resize(limits.patches);
}

VegetationService::~VegetationService() = default;

VegetationService::Species* VegetationService::species_record(VegetationSpeciesHandle handle) noexcept {
    return species_generations_.alive({handle.index, handle.generation}) ? species_[handle.index - 1].get() : nullptr;
}

const VegetationService::Species* VegetationService::species_record(VegetationSpeciesHandle handle) const noexcept {
    return species_generations_.alive({handle.index, handle.generation}) ? species_[handle.index - 1].get() : nullptr;
}

VegetationService::Patch* VegetationService::patch_record(VegetationPatchHandle handle) noexcept {
    return patch_generations_.alive({handle.index, handle.generation}) ? patches_[handle.index - 1].get() : nullptr;
}

const VegetationService::Patch* VegetationService::patch_record(VegetationPatchHandle handle) const noexcept {
    return patch_generations_.alive({handle.index, handle.generation}) ? patches_[handle.index - 1].get() : nullptr;
}

void VegetationService::changed() noexcept { advance_revision(revision_); }

VegetationSpeciesHandle VegetationService::add_species(VegetationSpeciesDesc description) {
    if (!valid(description) || description.materials.size() > limits_.materials_per_species ||
        find_species(description.name)) {
        return {};
    }
    auto value = std::make_unique<Species>();
    value->description = std::move(description);
    const auto acquired = species_generations_.acquire();
    if (!acquired) {
        return {};
    }
    const VegetationSpeciesHandle handle{acquired->index, acquired->generation};
    value->handle = handle;
    species_[handle.index - 1] = std::move(value);
    changed();
    return handle;
}

bool VegetationService::replace_species(VegetationSpeciesHandle handle, VegetationSpeciesDesc description) {
    auto* value = species_record(handle);
    const auto duplicate = find_species(description.name);
    if (!value || !valid(description) || description.materials.size() > limits_.materials_per_species ||
        (duplicate && duplicate != handle)) {
        return false;
    }
    value->description = std::move(description);
    advance_revision(value->revision);
    for (auto& patch : patches_) {
        if (patch && patch->description.species == handle) {
            patch->dirty = true;
        }
    }
    changed();
    return true;
}

bool VegetationService::remove_species(VegetationSpeciesHandle handle) {
    if (!species_record(handle)) {
        return false;
    }
    for (const auto& patch : patches_) {
        if (patch && (patch->description.species == handle || patch->built_species_handle == handle)) {
            return false;
        }
    }
    species_[handle.index - 1].reset();
    (void)species_generations_.release({handle.index, handle.generation});
    changed();
    return true;
}

VegetationSpeciesHandle VegetationService::find_species(std::string_view name) const noexcept {
    for (const auto& value : species_) {
        if (value && value->description.name == name) {
            return value->handle;
        }
    }
    return {};
}

std::optional<VegetationSpeciesInfo> VegetationService::inspect_species(VegetationSpeciesHandle handle) const noexcept {
    const auto* value = species_record(handle);
    if (!value) {
        return std::nullopt;
    }
    std::size_t references{};
    for (const auto& patch : patches_) {
        references += patch && (patch->description.species == handle ||
            patch->built_species_handle == handle) ? 1U : 0U;
    }
    return VegetationSpeciesInfo{handle, &value->description, value->revision, references};
}

std::vector<VegetationSpeciesHandle> VegetationService::species() const {
    std::vector<VegetationSpeciesHandle> result;
    result.reserve(species_generations_.size());
    for (const auto& value : species_) {
        if (value) {
            result.push_back(value->handle);
        }
    }
    return result;
}

VegetationPatchHandle VegetationService::create_patch(VegetationPatchDesc description) {
    if (!valid(description) || !species_record(description.species) ||
        description.maximum_instances > limits_.instances_per_patch ||
        description.exclusions.size() > limits_.exclusions_per_patch || find_patch(description.name)) {
        return {};
    }
    auto value = std::make_unique<Patch>();
    value->description = std::move(description);
    const auto acquired = patch_generations_.acquire();
    if (!acquired) {
        return {};
    }
    const VegetationPatchHandle handle{acquired->index, acquired->generation};
    value->handle = handle;
    patches_[handle.index - 1] = std::move(value);
    changed();
    return handle;
}

bool VegetationService::replace_patch(VegetationPatchHandle handle, VegetationPatchDesc description) {
    auto* value = patch_record(handle);
    const auto duplicate = find_patch(description.name);
    if (!value || !valid(description) || !species_record(description.species) ||
        description.maximum_instances > limits_.instances_per_patch ||
        description.exclusions.size() > limits_.exclusions_per_patch || (duplicate && duplicate != handle)) {
        return false;
    }
    value->description = std::move(description);
    value->dirty = true;
    changed();
    return true;
}

bool VegetationService::destroy_patch(VegetationPatchHandle handle, scene::Scene& scene) {
    auto* value = patch_record(handle);
    if (!value || scene.registry().iterating()) {
        return false;
    }
    if (scene.contains(value->root) && !scene.destroy(value->root)) {
        return false;
    }
    for (const auto& instance : value->instances) {
        if (scene.registry().alive(instance.entity)) {
            (void)scene.registry().destroy(instance.entity);
        }
    }
    instances_ -= value->instances.size();
    const auto visible = static_cast<std::size_t>(std::count_if(value->instances.begin(),
        value->instances.end(), [](const auto& instance) { return instance.visible; }));
    visible_ -= std::min(visible_, visible);
    patches_[handle.index - 1].reset();
    (void)patch_generations_.release({handle.index, handle.generation});
    changed();
    return true;
}

VegetationPatchHandle VegetationService::find_patch(std::string_view name) const noexcept {
    for (const auto& value : patches_) {
        if (value && value->description.name == name) {
            return value->handle;
        }
    }
    return {};
}

std::optional<VegetationPatchInfo> VegetationService::inspect_patch(VegetationPatchHandle handle) const noexcept {
    const auto* value = patch_record(handle);
    if (!value) {
        return std::nullopt;
    }
    return VegetationPatchInfo{handle, &value->description, value->root, value->terrain_entity,
        value->bounds, value->distribution, value->revision, value->terrain_revision,
        value->species_revision, value->dirty};
}

std::vector<VegetationPatchHandle> VegetationService::patches() const {
    std::vector<VegetationPatchHandle> result;
    result.reserve(patch_generations_.size());
    for (const auto& value : patches_) {
        if (value) {
            result.push_back(value->handle);
        }
    }
    return result;
}

std::span<const VegetationInstance> VegetationService::instances(VegetationPatchHandle handle) const noexcept {
    const auto* value = patch_record(handle);
    return value ? std::span<const VegetationInstance>{value->instances} : std::span<const VegetationInstance>{};
}

bool VegetationService::mark_dirty(VegetationPatchHandle handle) noexcept {
    auto* value = patch_record(handle);
    if (!value) {
        return false;
    }
    value->dirty = true;
    return true;
}

void VegetationService::mark_all_dirty() noexcept {
    for (auto& value : patches_) {
        if (value) {
            value->dirty = true;
        }
    }
}

VegetationBuildResult VegetationService::rebuild(VegetationPatchHandle handle,
    const TerrainService& terrain, const geometry::MeshLibrary& meshes,
    const render::MaterialLibrary* materials, scene::Scene& scene) {
    auto* patch = patch_record(handle);
    if (!patch) {
        return {VegetationError::MissingPatch};
    }
    const auto* species_value = species_record(patch->description.species);
    if (!species_value) {
        return {VegetationError::MissingSpecies};
    }
    const auto terrain_info = terrain.inspect(patch->description.terrain);
    const auto* field = terrain.get(patch->description.terrain);
    if (!terrain_info || !field) {
        return {VegetationError::MissingTerrain};
    }
    if (!terrain_info->entity || !scene.contains(terrain_info->entity)) {
        return {VegetationError::MissingBinding};
    }
    if (scene.registry().iterating()) {
        return {VegetationError::SceneRejected};
    }
    math::Aabb mesh_bounds;
    if (!resources_available(species_value->description, meshes, materials, mesh_bounds)) {
        return {VegetationError::MissingResource};
    }
    if (!patch->dirty && patch->terrain_revision == terrain_info->revision &&
        patch->species_revision == species_value->revision &&
        patch->terrain_entity == terrain_info->entity && scene.contains(patch->root) &&
        scene.parent(patch->root) == terrain_info->entity &&
        std::all_of(patch->instances.begin(), patch->instances.end(), [&](const auto& instance) {
            return scene.contains(instance.entity) && scene.parent(instance.entity) == patch->root &&
                scene.registry().has<geometry::MeshComponent>(instance.entity) &&
                scene.registry().has<render::MaterialComponent>(instance.entity) &&
                scene.registry().has<VegetationInstanceComponent>(instance.entity);
        })) {
        return {VegetationError::None, patch->distribution, patch->revision, false};
    }
    auto snapshot = std::make_unique<VegetationSpeciesDesc>(species_value->description);
    std::vector<VegetationInstance> replacement;
    VegetationBuildResult result;
    result.error = distribute(patch->description, *snapshot, *field, limits_, replacement, result.distribution);
    if (!result) {
        return result;
    }
    const auto existing_other = instances_ - patch->instances.size();
    if (replacement.size() > limits_.total_instances - existing_other ||
        scene.size() > scene.limits().max_nodes ||
        replacement.size() + 1 > scene.limits().max_nodes - scene.size() ||
        replacement.size() + 1 > scene.registry().capacity() - scene.registry().size()) {
        result.error = VegetationError::Capacity;
        return result;
    }
    StagedEntities staged(scene, replacement.size() + 1);
    const auto root = scene.create("vegetation/" + patch->description.name, terrain_info->entity);
    if (!root) {
        result.error = VegetationError::SceneRejected;
        return result;
    }
    staged.add(root);
    (void)scene.registry().set_enabled(root, false);
    (void)scene.registry().set_layers(root, patch->description.layers);
    auto next_revision = patch->revision;
    advance_revision(next_revision);
    math::Aabb authored_bounds;
    for (auto& instance : replacement) {
        instance.entity = scene.create("plant/" + std::to_string(instance.candidate_id), root, instance.authored_pose);
        if (!instance.entity) {
            result.error = VegetationError::SceneRejected;
            return result;
        }
        staged.add(instance.entity);
        if (!meshes.assign(scene.registry(), instance.entity, snapshot->lods[0].mesh) ||
            !scene.set_local_bounds(instance.entity, mesh_bounds)) {
            result.error = VegetationError::SceneRejected;
            return result;
        }
        (void)scene.registry().set_layers(instance.entity, patch->description.layers);
        scene.registry().emplace<render::MaterialComponent>(instance.entity,
            render::MaterialComponent{snapshot->materials});
        scene.registry().emplace<VegetationInstanceComponent>(instance.entity,
            VegetationInstanceComponent{handle, patch->description.species, instance.candidate_id,
                next_revision, instance.density_rank, instance.wind_phase, 0});
        const auto bounds = math::transform_bounds(math::matrix(instance.authored_pose), mesh_bounds);
        if (!math::valid(bounds)) {
            result.error = VegetationError::InvalidDescription;
            return result;
        }
        authored_bounds = math::merge(authored_bounds, bounds);
    }
    if (scene.contains(patch->root) && !scene.destroy(patch->root)) {
        result.error = VegetationError::SceneRejected;
        return result;
    }
    for (const auto& instance : patch->instances) {
        (void)scene.registry().destroy(instance.entity);
    }
    patch->instances.swap(replacement);
    patch->built_species.swap(snapshot);
    patch->built_species_handle = patch->description.species;
    patch->root = root;
    patch->terrain_entity = terrain_info->entity;
    patch->terrain_revision = terrain_info->revision;
    patch->species_revision = species_value->revision;
    patch->revision = next_revision;
    patch->bounds = authored_bounds;
    patch->distribution = result.distribution;
    patch->layers = patch->description.layers;
    patch->dirty = false;
    instances_ = existing_other + patch->instances.size();
    (void)scene.registry().set_enabled(root, patch->description.enabled);
    staged.commit();
    advance_revision(rebuilds_);
    changed();
    result.revision = patch->revision;
    result.changed = true;
    return result;
}

std::size_t VegetationService::refresh(const TerrainService& terrain,
    const geometry::MeshLibrary& meshes, const render::MaterialLibrary* materials,
    scene::Scene& scene, std::size_t patch_budget) {
    std::size_t rebuilt{};
    std::size_t attempted{};
    if (patches_.empty() || patch_budget == 0 || scene.registry().iterating()) {
        return rebuilt;
    }
    for (std::size_t visited = 0; visited < patches_.size() && attempted < patch_budget; ++visited) {
        const auto index = refresh_cursor_++ % patches_.size();
        auto* patch = patches_[index].get();
        if (!patch) {
            continue;
        }
        const auto terrain_info = terrain.inspect(patch->description.terrain);
        const auto* species_value = species_record(patch->description.species);
        const bool current = terrain_info && species_value && !patch->dirty &&
            patch->terrain_revision == terrain_info->revision &&
            patch->terrain_entity == terrain_info->entity &&
            patch->species_revision == species_value->revision && scene.contains(patch->root) &&
            scene.parent(patch->root) == terrain_info->entity;
        if (current) {
            continue;
        }
        patch->dirty = true;
        ++attempted;
        const auto result = rebuild(patch->handle, terrain, meshes, materials, scene);
        rebuilt += result && result.changed ? 1U : 0U;
    }
    return rebuilt;
}

bool VegetationService::set_view(VegetationView value) noexcept {
    if (!math::finite(value.position) || !bounded(value.density, 0.0, 1.0) ||
        !bounded(value.lod_distance_scale, 0.01, 100.0)) {
        return false;
    }
    if (value.frustum) {
        for (const auto& plane : value.frustum->planes) {
            if (!math::valid(plane)) {
                return false;
            }
        }
    }
    view_ = std::move(value);
    return true;
}

const VegetationView& VegetationService::view() const noexcept { return view_; }

bool VegetationService::set_wind(math::Vec2 direction, double strength) noexcept {
    if (!math::finite(direction) || !bounded(strength, 0.0, 1.0) ||
        (strength > 0.0 && math::length_squared(direction) < 1.0e-12)) {
        return false;
    }
    wind_direction_ = math::normalized(direction, {1.0, 0.0});
    wind_strength_ = strength;
    return true;
}

void VegetationService::update(scene::Scene& scene, const geometry::MeshLibrary& meshes,
    const render::MaterialLibrary* materials, double elapsed_seconds) {
    if (!std::isfinite(elapsed_seconds) || scene.registry().iterating()) {
        return;
    }
    (void)scene.update();
    visible_ = 0;
    missing_entities_ = 0;
    std::vector<scene::LocalTransformEdit> wind_edits;
    wind_edits.reserve(instances_);
    for (auto& patch : patches_) {
        if (!patch || !patch->built_species) {
            continue;
        }
        const auto& species_value = *patch->built_species;
        math::Aabb mesh_bounds;
        const bool resources = resources_available(species_value, meshes, materials, mesh_bounds);
        const auto* parent_matrix = scene.cached_world_matrix(patch->root);
        const auto inverse_parent = parent_matrix ? math::inverse(*parent_matrix) : std::optional<math::Mat4>{};
        const bool attached = scene.contains(patch->terrain_entity) &&
            scene.parent(patch->root) == patch->terrain_entity;
        const bool parent_enabled = attached && inverse_parent && enabled_hierarchy(scene, patch->root);
        patch->dirty = patch->dirty || !attached;
        for (auto& instance : patch->instances) {
            auto* mesh_component = scene.registry().get<geometry::MeshComponent>(instance.entity);
            auto* component = scene.registry().get<VegetationInstanceComponent>(instance.entity);
            if (!scene.contains(instance.entity) || !mesh_component || !component ||
                !scene.registry().has<render::MaterialComponent>(instance.entity) ||
                scene.parent(instance.entity) != patch->root) {
                ++missing_entities_;
                patch->dirty = true;
                instance.visible = false;
                if (mesh_component) {
                    mesh_component->visible = false;
                    (void)scene.registry().mark_changed<geometry::MeshComponent>(instance.entity);
                }
                continue;
            }
            auto pose = instance.authored_pose;
            if (inverse_parent && species_value.normal_alignment > 0.0) {
                const auto normal_matrix = math::transpose(math::upper3x3(*inverse_parent));
                const auto world_normal = math::normalized(normal_matrix * instance.normal, instance.normal);
                const auto local_normal = math::normalized(
                    math::transform_vector(*inverse_parent, world_normal), instance.normal);
                const auto original = math::slerp(math::Quat{},
                    math::from_to({0.0, 1.0, 0.0}, instance.normal), species_value.normal_alignment);
                const auto corrected = math::slerp(math::Quat{},
                    math::from_to({0.0, 1.0, 0.0}, local_normal), species_value.normal_alignment);
                pose.rotation = math::normalized(corrected * math::conjugate(original) * pose.rotation);
            }
            const double period = species_value.wind_frequency > 0.0 ?
                1.0 / species_value.wind_frequency : 1.0;
            const double phase = std::remainder(elapsed_seconds, period) *
                species_value.wind_frequency * math::tau + instance.wind_phase;
            const double bend = std::sin(phase) * species_value.wind_amplitude * wind_strength_;
            pose.rotation = math::normalized(pose.rotation *
                math::from_axis_angle({wind_direction_.y, 0.0, -wind_direction_.x}, bend));
            const auto current = scene.local_transform(instance.entity);
            if (current && (current->position != pose.position || current->rotation != pose.rotation ||
                            current->scale != pose.scale)) {
                wind_edits.push_back({instance.entity, pose});
            }
            std::uint32_t lod{};
            bool visible = resources && parent_enabled && (view_.layers & patch->layers) != 0;
            double density = view_.density;
            if (visible && view_.enabled) {
                const auto transform = *parent_matrix * math::matrix(pose);
                const auto bounds = math::transform_bounds(transform, mesh_bounds);
                const double distance = math::distance(view_.position,
                    math::transform_point(*parent_matrix, pose.position));
                const double lod_distance = distance * view_.lod_distance_scale;
                visible = std::isfinite(distance) && distance < species_value.cull_distance &&
                    math::valid(bounds) && (!view_.frustum || math::intersects(*view_.frustum, bounds));
                while (lod + 1 < species_value.lod_count &&
                       lod_distance >= species_value.lods[lod + 1].distance) {
                    ++lod;
                }
                if (species_value.fade_distance < species_value.cull_distance) {
                    density *= 1.0 - math::smoothstep(species_value.fade_distance,
                        species_value.cull_distance, distance);
                }
            }
            density *= species_value.lods[lod].density;
            visible = visible && instance.density_rank < density;
            const auto mesh = species_value.lods[lod].mesh;
            if (mesh_component->mesh != mesh || mesh_component->visible != visible) {
                mesh_component->mesh = mesh;
                mesh_component->visible = visible;
                (void)scene.registry().mark_changed<geometry::MeshComponent>(instance.entity);
            }
            if (component->lod != lod) {
                component->lod = lod;
                (void)scene.registry().mark_changed<VegetationInstanceComponent>(instance.entity);
            }
            instance.lod = lod;
            instance.visible = visible;
            visible_ += visible ? 1U : 0U;
        }
    }
    if (!wind_edits.empty()) {
        (void)scene.apply_local_transforms(wind_edits);
    }
    (void)scene.update();
}

void VegetationService::clear(scene::Scene& scene) {
    if (scene.registry().iterating()) {
        return;
    }
    for (auto& value : patches_) {
        if (value) {
            const auto handle = value->handle;
            (void)destroy_patch(handle, scene);
        }
    }
    (void)scene.update();
}

VegetationStatistics VegetationService::statistics() const noexcept {
    VegetationStatistics result;
    result.species = species_generations_.size();
    result.patches = patch_generations_.size();
    result.instances = instances_;
    result.visible = visible_;
    result.missing_entities = missing_entities_;
    result.revision = revision_;
    result.rebuilds = rebuilds_;
    for (const auto& patch : patches_) {
        result.dirty_patches += patch && patch->dirty ? 1U : 0U;
    }
    return result;
}

const VegetationLimits& VegetationService::limits() const noexcept { return limits_; }

void register_vegetation(core::Kernel& kernel) {
    if (!kernel.register_module({"terrain.vegetation", "Deterministic terrain vegetation and Scene instances", 1})) {
        return;
    }
    kernel.services().emplace<VegetationService>();
    if (auto* registry = kernel.services().find<ecs::Registry>()) {
        registry->register_component<VegetationInstanceComponent>("terrain.vegetation.instance");
    }
    kernel.add_command("vegetation.species", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 5) {
            return {false, "usage: vegetation.species name mesh [density] [spacing] [material]"};
        }
        const auto* meshes = host.services().find<geometry::MeshLibrary>();
        const auto* materials = material_library(host);
        if (!meshes) {
            return {false, "mesh library unavailable"};
        }
        VegetationSpeciesDesc description;
        description.name = args[0];
        description.lods[0].mesh = meshes->find(args[1]);
        const auto density = args.size() > 2 ? parse_real(args[2]) : std::optional<double>{0.1};
        const auto spacing = args.size() > 3 ? parse_real(args[3]) : std::optional<double>{1.0};
        if (!density || !spacing || !description.lods[0].mesh) {
            return {false, "invalid density, spacing, or mesh name"};
        }
        description.density = *density;
        description.minimum_spacing = *spacing;
        if (args.size() == 5) {
            const auto material = materials ? materials->find(args[4]) : render::MaterialHandle{};
            if (!material) {
                return {false, "material name not found"};
            }
            description.materials.push_back(material);
        }
        const auto handle = host.services().get<VegetationService>().add_species(std::move(description));
        return {static_cast<bool>(handle), handle ? "species created" : "species rejected or name already exists"};
    }, "Create a species borrowing an existing mesh and optional material");
    kernel.add_command("vegetation.patch", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 3 || args.size() > 5) {
            return {false, "usage: vegetation.patch name terrain species [seed] [density_multiplier]"};
        }
        auto& service = host.services().get<VegetationService>();
        const auto* terrain = host.services().find<TerrainService>();
        const auto terrain_handle = terrain ? terrain->find(args[1]) : TerrainHandle{};
        const auto terrain_info = terrain ? terrain->inspect(terrain_handle) : std::optional<TerrainInfo>{};
        const auto seed = args.size() > 3 ? parse_seed(args[3]) : std::optional<std::uint64_t>{1};
        const auto density = args.size() > 4 ? parse_real(args[4]) : std::optional<double>{1.0};
        if (!terrain_info || !seed || !density) {
            return {false, "terrain, seed, or density multiplier is invalid"};
        }
        VegetationPatchDesc description;
        description.name = args[0];
        description.terrain = terrain_handle;
        description.species = service.find_species(args[2]);
        description.minimum = {terrain_info->bounds.min.x, terrain_info->bounds.min.z};
        description.maximum = {terrain_info->bounds.max.x, terrain_info->bounds.max.z};
        description.seed = *seed;
        description.density_multiplier = *density;
        description.maximum_instances = static_cast<std::uint32_t>(service.limits().instances_per_patch);
        const auto handle = service.create_patch(std::move(description));
        return {static_cast<bool>(handle), handle ? "patch created; rebuild scheduled" : "patch description rejected"};
    }, "Distribute a species over an existing terrain's authored bounds");
    kernel.add_command("vegetation.rebuild", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 1) {
            return {false, "usage: vegetation.rebuild patch|all"};
        }
        auto& service = host.services().get<VegetationService>();
        const auto* terrain = host.services().find<TerrainService>();
        const auto* meshes = host.services().find<geometry::MeshLibrary>();
        auto* scene = host.services().find<scene::Scene>();
        const auto* materials = material_library(host);
        if (!terrain || !meshes || !scene) {
            return {false, "terrain, mesh library, or Scene unavailable"};
        }
        if (args[0] == "all") {
            service.mark_all_dirty();
            const auto count = service.refresh(*terrain, *meshes, materials, *scene, service.limits().patches);
            const auto dirty = service.statistics().dirty_patches;
            return {dirty == 0, "rebuilt=" + std::to_string(count) + " pending=" + std::to_string(dirty)};
        }
        const auto handle = service.find_patch(args[0]);
        (void)service.mark_dirty(handle);
        const auto result = service.rebuild(handle, *terrain, *meshes, materials, *scene);
        return {static_cast<bool>(result), std::string(describe(result.error)) +
            " instances=" + std::to_string(result.distribution.accepted) +
            " candidates=" + std::to_string(result.distribution.candidates)};
    }, "Transactionally replace vegetation entities; failures retain existing instances");
    kernel.add_command("vegetation.clear", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto* scene = host.services().find<scene::Scene>();
        if (!scene || args.size() > 1) {
            return {false, "usage: vegetation.clear [patch]; Scene required"};
        }
        auto& service = host.services().get<VegetationService>();
        if (args.empty()) {
            service.clear(*scene);
            return {service.statistics().patches == 0, "remaining patches=" + std::to_string(service.statistics().patches)};
        }
        const bool removed = service.destroy_patch(service.find_patch(args[0]), *scene);
        return {removed, removed ? "patch removed; shared assets retained" : "patch removal rejected"};
    }, "Destroy owned vegetation entities and patch records");
    kernel.add_command("vegetation.inspect", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        const auto& service = host.services().get<VegetationService>();
        if (args.size() > 1) {
            return {false, "usage: vegetation.inspect [patch]"};
        }
        if (!args.empty()) {
            const auto info = service.inspect_patch(service.find_patch(args[0]));
            if (!info) {
                return {false, "patch not found"};
            }
            return {true, "revision=" + std::to_string(info->revision) +
                " terrain_revision=" + std::to_string(info->terrain_revision) +
                " instances=" + std::to_string(info->distribution.accepted) +
                " rejected_spacing=" + std::to_string(info->distribution.spacing_rejections) +
                " dirty=" + std::to_string(info->dirty)};
        }
        const auto stats = service.statistics();
        return {true, "species=" + std::to_string(stats.species) + " patches=" + std::to_string(stats.patches) +
            " instances=" + std::to_string(stats.instances) + " visible=" + std::to_string(stats.visible) +
            " dirty=" + std::to_string(stats.dirty_patches)};
    }, "Inspect actual vegetation distribution, revisions, and visibility");
    kernel.add_command("vegetation.density", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) {
            return {false, "usage: vegetation.density patch multiplier"};
        }
        auto& service = host.services().get<VegetationService>();
        const auto handle = service.find_patch(args[0]);
        const auto info = service.inspect_patch(handle);
        const auto density = parse_real(args[1]);
        if (!info || !density) {
            return {false, "patch or multiplier is invalid"};
        }
        auto description = *info->description;
        description.density_multiplier = *density;
        const bool changed = service.replace_patch(handle, std::move(description));
        return {changed, changed ? "density updated; rebuild scheduled" : "density out of range"};
    }, "Change authored distribution density and schedule replacement");
    kernel.add_command("vegetation.wind", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 3) {
            return {false, "usage: vegetation.wind x z strength"};
        }
        const auto x = parse_real(args[0]);
        const auto z = parse_real(args[1]);
        const auto strength = parse_real(args[2]);
        const bool accepted = x && z && strength &&
            host.services().get<VegetationService>().set_wind({*x, *z}, *strength);
        return {accepted, accepted ? "wind updated" : "invalid direction or strength outside [0,1]"};
    }, "Set authored wind direction and bounded global strength");
    kernel.add_system({"terrain.vegetation", core::Phase::Visibility, -140, {"terrain.heightfield.rebuild"}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& service = host.services().get<VegetationService>();
            const auto* terrain = host.services().find<TerrainService>();
            const auto* meshes = host.services().find<geometry::MeshLibrary>();
            auto* scene = host.services().find<scene::Scene>();
            if (!terrain || !meshes || !scene) {
                return;
            }
            const auto* materials = material_library(host);
            const auto budget = std::clamp(host.config().integer("vegetation.rebuild_budget", 2),
                                           std::int64_t{0}, std::int64_t{16});
            (void)service.refresh(*terrain, *meshes, materials, *scene, static_cast<std::size_t>(budget));
            VegetationView view;
            view.density = math::saturate(host.config().number("vegetation.density", 1.0));
            if (const auto* cameras = host.services().find<render::CameraService>()) {
                if (const auto* camera = cameras->current_view(); camera && !camera->stale) {
                    view.position = camera->position;
                    view.frustum = camera->frustum;
                    view.layers = camera->visibility_mask;
                    view.enabled = true;
                }
            }
            (void)service.set_view(std::move(view));
            service.update(*scene, *meshes, materials, frame.elapsed_seconds);
            const auto stats = service.statistics();
            host.metric("vegetation.instances", static_cast<double>(stats.instances));
            host.metric("vegetation.visible", static_cast<double>(stats.visible));
            host.metric("vegetation.dirty_patches", static_cast<double>(stats.dirty_patches));
        });
    kernel.add_shutdown("terrain.vegetation", [host = &kernel] {
        auto* service = host->services().find<VegetationService>();
        auto* scene = host->services().find<scene::Scene>();
        if (service && scene) {
            service->clear(*scene);
        }
    });
}

} // namespace ge::terrain
