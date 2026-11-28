#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/render/material.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; class WorldPartition; }
namespace ge::render { struct CameraView; }

namespace ge::effects {

inline constexpr std::size_t particle_curve_keys = 8;
inline constexpr std::size_t particle_burst_keys = 8;
inline constexpr std::size_t particle_trail_points = 16;

struct EmitterHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0 && generation != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const EmitterHandle&) const = default;
};

struct ParticleHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0 && generation != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    auto operator<=>(const ParticleHandle&) const = default;
};

enum class ParticleSpace : std::uint8_t { Local, World };
enum class EmissionShapeKind : std::uint8_t { Point, Box, Sphere, Disk, Cone };
enum class ParticlePlayback : std::uint8_t { Playing, Paused, Stopped };
enum class ParticleError : std::uint8_t {
    None,
    InvalidDescription,
    InvalidHandle,
    InvalidSource,
    Capacity,
    SceneBusy,
    SceneRejected,
    MissingMaterial,
    MissingMesh,
    InvalidTime,
    InvalidOrigin,
    AllocationFailure,
    Closed
};

[[nodiscard]] std::string_view describe(ParticleError error) noexcept;

struct ScalarCurveKey {
    double age{};
    double value{1.0};
};

struct ColorCurveKey {
    double age{};
    math::Vec4 value{1.0, 1.0, 1.0, 1.0};
};

struct ScalarCurve {
    std::array<ScalarCurveKey, particle_curve_keys> keys{{{0.0, 1.0}, {1.0, 0.0}}};
    std::size_t count{2};
    [[nodiscard]] double sample(double normalized_age) const noexcept;
};

struct ColorCurve {
    std::array<ColorCurveKey, particle_curve_keys> keys{{
        {0.0, {1.0, 0.7, 0.2, 1.0}}, {1.0, {0.3, 0.1, 0.03, 0.0}}}};
    std::size_t count{2};
    [[nodiscard]] math::Vec4 sample(double normalized_age) const noexcept;
};

struct ParticleRange {
    double minimum{};
    double maximum{};
};

struct EmissionShape {
    EmissionShapeKind kind{EmissionShapeKind::Point};
    math::Vec3 center{};
    math::Vec3 half_extents{0.5, 0.5, 0.5};
    math::Vec3 axis{0.0, 1.0, 0.0};
    double radius{0.5};
    double height{1.0};
    bool surface_only{};
};

struct ParticleForces {
    // Forces use the particle's simulation space. Wind drag is exponential.
    math::Vec3 acceleration{0.0, -9.81, 0.0};
    math::Vec3 wind_velocity{};
    double drag{0.1};
    math::Vec3 radial_center{};
    double radial_strength{};
    double radial_softening{0.1};
    double maximum_acceleration{10000.0};
    double maximum_speed{10000.0};
};

struct ParticleTrailSettings {
    bool enabled{};
    std::size_t points{8};
    double sample_interval{0.04};
    double minimum_distance{0.01};
    double lifetime{0.5};
    double width{0.04};
    double tail_opacity{};
};

struct ParticleBurst {
    double time{};
    std::uint32_t count{16};
};

struct EmitterDescription {
    ParticleSpace space{ParticleSpace::World};
    EmissionShape shape{};
    ParticleForces forces{};
    ParticleTrailSettings trail{};
    ParticleRange lifetime{0.5, 1.5};
    ParticleRange speed{1.0, 3.0};
    ParticleRange size{0.08, 0.15};
    ParticleRange rotation{-math::pi, math::pi};
    ParticleRange angular_velocity{-1.0, 1.0};
    ScalarCurve size_over_age{};
    ColorCurve color_over_age{};
    std::array<ParticleBurst, particle_burst_keys> bursts{};
    std::size_t burst_count{};
    std::size_t maximum_live{1024};
    std::size_t maximum_emit_per_update{1024};
    std::uint64_t maximum_total_emissions{}; // Zero means no lifetime quota.
    std::uint64_t seed{1};
    std::uint64_t layers{1};
    double rate{20.0};
    double duration{5.0};
    double spread_radians{0.35};
    bool looping{true};
    bool radial_velocity{};
    bool billboards{true};
    bool depth_sort{true};
    render::MaterialHandle material{}; // Invalid selects the shared unlit blend material.
};

struct ParticleLimits {
    std::uint32_t emitters{128};
    std::uint32_t particles{16384};
    std::size_t emissions_per_update{4096};
    std::size_t queued_burst_particles{65536};
    std::size_t mesh_quads_per_update{65536};
    std::size_t mesh_quads_per_emitter{16384};
    std::size_t substeps{16};
    double maximum_step{1.0 / 60.0};
    double maximum_delta{0.25};
    double coordinate_limit{1.0e9};
};

struct ParticleValidation {
    bool valid{true};
    std::string_view field{};
    [[nodiscard]] explicit operator bool() const noexcept { return valid; }
};

[[nodiscard]] ParticleValidation validate(const EmitterDescription& description,
                                           const ParticleLimits& limits = {}) noexcept;
[[nodiscard]] bool valid(const ParticleLimits& limits) noexcept;

struct ParticleState {
    ParticleHandle handle{};
    EmitterHandle emitter{};
    math::Vec3 position{};
    math::Vec3 previous_position{};
    math::Vec3 velocity{};
    math::Vec4 color{1.0, 1.0, 1.0, 1.0};
    double age{};
    double lifetime{1.0};
    double initial_size{0.1};
    double size{0.1};
    double rotation{};
    double angular_velocity{};
    std::uint64_t serial{};
};

struct ParticleTrailPoint {
    math::Vec3 position{};
    math::Vec4 color{};
    double age{}; // Sample time measured from the owning particle's birth.
    double width{};
};

struct EmitterState {
    EmitterHandle handle{};
    std::string name;
    ecs::Entity source{};
    ecs::Entity render_entity{};
    geometry::MeshHandle mesh{};
    render::MaterialHandle material{};
    ParticlePlayback playback{ParticlePlayback::Playing};
    ParticleSpace space{ParticleSpace::World};
    std::size_t live_particles{};
    std::size_t queued_particles{};
    std::uint64_t emitted{};
    std::uint64_t dropped{};
    double elapsed{};
    double fractional_emission{};
    bool owns_source{};
};

// Present only on service-owned nodes; neither this nor snapshots transfer ownership.
// Removing or replacing the installed component relinquishes the service's node
// ownership, even if the replacement carries the same emitter handle.
struct ParticleBindingComponent {
    EmitterHandle emitter{};
    bool source{};
};

struct ParticleStatistics {
    std::size_t emitters{};
    std::size_t particles{};
    std::size_t trail_points{};
    std::size_t mesh_quads{}; // Geometry admitted by the most recent rebuild.
    std::size_t queued_particles{};
    std::uint64_t emitted{};
    std::uint64_t expired{};
    std::uint64_t dropped{};
    std::uint64_t invalid_particles{};
    std::uint64_t mesh_failures{};
    std::uint64_t origin_resets{};
    std::uint64_t updates{};
    double simulated_seconds{};
    double dropped_seconds{};
    std::uint64_t origin_epoch{};
};

// Single writer, borrowing Scene/MeshLibrary/MaterialService until shutdown().
// Handles belong to one service lifetime. Snapshots are values in the emitter's
// declared simulation space. Reproducibility requires identical seed, controls,
// source transforms, update intervals and admission budgets. Pause freezes both
// emission and existing particles; stop only stops emission. Frame time beyond
// the configured catch-up limit and excess scheduled emissions are discarded.
// Local centers/velocities follow the source's exact affine matrix; billboard
// sizes and ribbon widths remain world metres. Trails retire with their particle.
// The destructor touches no borrowed services; owners must call shutdown first.
class ParticleService final {
public:
    ParticleService(scene::Scene& scene, geometry::MeshLibrary& meshes,
                    render::MaterialService& materials, ParticleLimits limits = {});
    ~ParticleService();
    ParticleService(const ParticleService&) = delete;
    ParticleService& operator=(const ParticleService&) = delete;
    ParticleService(ParticleService&&) = delete;
    ParticleService& operator=(ParticleService&&) = delete;

    // A null source creates an owned Scene root; an existing source is borrowed.
    [[nodiscard]] EmitterHandle create(std::string_view name,
                                        const EmitterDescription& description = {},
                                        ecs::Entity source = {});
    bool destroy(EmitterHandle emitter);
    bool play(EmitterHandle emitter, bool restart = false);
    bool pause(EmitterHandle emitter);
    bool stop(EmitterHandle emitter, bool clear_particles = false);
    bool clear(EmitterHandle emitter);
    std::size_t clear_all();
    // Queued bursts respect per-emitter, global and lifetime admission limits.
    [[nodiscard]] std::size_t burst(EmitterHandle emitter, std::size_t count);
    bool set_rate(EmitterHandle emitter, double particles_per_second);
    bool set_forces(EmitterHandle emitter, const ParticleForces& forces);
    bool set_material(EmitterHandle emitter, render::MaterialHandle material);
    bool set_curves(EmitterHandle emitter, const ScalarCurve& size, const ColorCurve& color);
    bool kill(ParticleHandle particle);
    void update(double delta_seconds);
    // Uses one current camera for all billboards, including multi-view rendering.
    // Call after camera.update and before geometry.mesh.bounds/spatial.sync.
    std::size_t rebuild_meshes(const render::CameraView* camera);
    // World runtime coordinates subtract offset. Scene roots must already have
    // shifted; local particles follow their source hierarchy without data edits.
    // World render roots are reset to identity to avoid applying the shift twice.
    // Use this for standalone origin ownership, or synchronize_origin for a
    // WorldPartition. Do not apply both methods for the same committed shift.
    bool shift_origin(math::Vec3 offset);
    bool synchronize_origin(const scene::WorldPartition& partition);
    void shutdown();

    [[nodiscard]] bool contains(EmitterHandle emitter) const noexcept;
    [[nodiscard]] bool contains(ParticleHandle particle) const noexcept;
    [[nodiscard]] EmitterHandle find(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<EmitterState> inspect(EmitterHandle emitter) const;
    [[nodiscard]] std::optional<EmitterDescription> description(EmitterHandle emitter) const;
    [[nodiscard]] std::optional<ParticleState> inspect(ParticleHandle particle) const noexcept;
    [[nodiscard]] std::vector<EmitterState> emitters() const;
    [[nodiscard]] std::vector<ParticleState> particles(EmitterHandle emitter,
                                                     std::size_t limit = 4096) const;
    [[nodiscard]] std::vector<ParticleTrailPoint> trail(ParticleHandle particle) const;
    [[nodiscard]] ParticleStatistics statistics() const noexcept;
    [[nodiscard]] const ParticleLimits& limits() const noexcept;
    [[nodiscard]] ParticleError last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_particles(core::Kernel& kernel);

} // namespace ge::effects
