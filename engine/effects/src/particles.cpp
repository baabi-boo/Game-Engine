#include <ge/effects/particles.hpp>

#include <ge/core/runtime.hpp>
#include <ge/render/camera.hpp>
#include <ge/scene/partition.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ge::effects {
namespace {

bool bounded(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool bounded(math::Vec3 value, double maximum) noexcept {
    return math::finite(value) && std::abs(value.x) <= maximum &&
        std::abs(value.y) <= maximum && std::abs(value.z) <= maximum;
}

void add_count(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}

bool valid_range(ParticleRange range, double low, double high) noexcept {
    return bounded(range.minimum, low, high) && bounded(range.maximum, range.minimum, high);
}

bool valid_curve(const ScalarCurve& curve) noexcept {
    if (curve.count == 0 || curve.count > curve.keys.size()) return false;
    double previous = -1.0;
    for (std::size_t i = 0; i < curve.count; ++i) {
        const auto key = curve.keys[i];
        if (!bounded(key.age, 0.0, 1.0) || key.age <= previous ||
            !bounded(key.value, 0.0, 1000.0)) return false;
        previous = key.age;
    }
    return true;
}

bool valid_curve(const ColorCurve& curve) noexcept {
    if (curve.count == 0 || curve.count > curve.keys.size()) return false;
    double previous = -1.0;
    for (std::size_t i = 0; i < curve.count; ++i) {
        const auto key = curve.keys[i];
        if (!bounded(key.age, 0.0, 1.0) || key.age <= previous ||
            !bounded(key.value.x, 0.0, 1.0e6) || !bounded(key.value.y, 0.0, 1.0e6) ||
            !bounded(key.value.z, 0.0, 1.0e6) || !bounded(key.value.w, 0.0, 1.0)) return false;
        previous = key.age;
    }
    return true;
}

bool valid_forces(const ParticleForces& forces, double coordinates) noexcept {
    return bounded(forces.acceleration, 1.0e6) && bounded(forces.wind_velocity, 1.0e6) &&
        bounded(forces.radial_center, coordinates) && bounded(forces.drag, 0.0, 10000.0) &&
        bounded(forces.radial_strength, -1.0e9, 1.0e9) &&
        bounded(forces.radial_softening, 1.0e-5, 1.0e6) &&
        bounded(forces.maximum_acceleration, 0.0, 1.0e6) &&
        bounded(forces.maximum_speed, 0.0, 1.0e6);
}

double random_unit(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<double>(value >> 11U) * (1.0 / 9007199254740992.0);
}

double random_range(std::uint64_t& state, ParticleRange range) noexcept {
    return math::lerp(range.minimum, range.maximum, random_unit(state));
}

math::Vec3 sample_shape(const EmissionShape& shape, std::uint64_t& seed) noexcept {
    const double a = random_unit(seed);
    const double b = random_unit(seed);
    const double c = random_unit(seed);
    const auto axis = math::normalized(shape.axis, {0.0, 1.0, 0.0});
    const auto [right, up] = math::orthonormal_basis(axis);
    switch (shape.kind) {
    case EmissionShapeKind::Point:
        return shape.center;
    case EmissionShapeKind::Box: {
        math::Vec3 offset{2.0 * a - 1.0, 2.0 * b - 1.0, 2.0 * c - 1.0};
        if (shape.surface_only) {
            const auto extents = shape.half_extents;
            const double x_area = extents.y * extents.z;
            const double y_area = extents.x * extents.z;
            const double choice = random_unit(seed) * (x_area + y_area + extents.x * extents.y);
            const std::size_t face = choice < x_area ? 0U : (choice < x_area + y_area ? 1U : 2U);
            offset[face] = random_unit(seed) < 0.5 ? -1.0 : 1.0;
        }
        return shape.center + offset * shape.half_extents;
    }
    case EmissionShapeKind::Sphere:
        return shape.center + math::uniform_sphere({a, b}) *
            (shape.radius * (shape.surface_only ? 1.0 : std::cbrt(c)));
    case EmissionShapeKind::Disk: {
        const double radius = shape.radius * (shape.surface_only ? 1.0 : std::sqrt(a));
        return shape.center + (right * std::cos(math::tau * b) +
            up * std::sin(math::tau * b)) * radius;
    }
    case EmissionShapeKind::Cone: {
        const double height = shape.surface_only ? std::sqrt(c) : std::cbrt(c);
        const double radius = shape.radius * height * (shape.surface_only ? 1.0 : std::sqrt(a));
        return shape.center + axis * (shape.height * height) +
            (right * std::cos(math::tau * b) + up * std::sin(math::tau * b)) * radius;
    }
    }
    return shape.center;
}

math::Vec3 sample_direction(const EmitterDescription& description, math::Vec3 position,
                            std::uint64_t& seed) noexcept {
    const auto axis = description.radial_velocity
        ? math::normalized(position - description.shape.center,
                           math::normalized(description.shape.axis, {0.0, 1.0, 0.0}))
        : math::normalized(description.shape.axis, {0.0, 1.0, 0.0});
    const auto [right, up] = math::orthonormal_basis(axis);
    const double cosine = math::lerp(std::cos(description.spread_radians), 1.0, random_unit(seed));
    const double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));
    const double angle = math::tau * random_unit(seed);
    return axis * cosine + (right * std::cos(angle) + up * std::sin(angle)) * sine;
}

template<class T>
std::optional<T> number(std::string_view text) noexcept {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

template<class Handle>
std::string handle_text(Handle handle) {
    return std::to_string(handle.index) + ":" + std::to_string(handle.generation);
}

EmitterHandle resolve(const ParticleService& service, std::string_view text) noexcept {
    if (const auto named = service.find(text)) return named;
    const auto colon = text.find(':');
    if (colon == std::string_view::npos) return {};
    const auto index = number<std::uint32_t>(text.substr(0, colon));
    const auto generation = number<std::uint64_t>(text.substr(colon + 1));
    return index && generation ? EmitterHandle{*index, *generation} : EmitterHandle{};
}

} // namespace

std::string_view describe(ParticleError error) noexcept {
    switch (error) {
    case ParticleError::None: return "none";
    case ParticleError::InvalidDescription: return "invalid emitter description";
    case ParticleError::InvalidHandle: return "missing or stale particle handle";
    case ParticleError::InvalidSource: return "missing source entity";
    case ParticleError::Capacity: return "particle budget exhausted";
    case ParticleError::SceneBusy: return "registry is being iterated";
    case ParticleError::SceneRejected: return "scene binding rejected";
    case ParticleError::MissingMaterial: return "material is unavailable";
    case ParticleError::MissingMesh: return "mesh is unavailable";
    case ParticleError::InvalidTime: return "invalid simulation time";
    case ParticleError::InvalidOrigin: return "origin shift cannot be applied";
    case ParticleError::AllocationFailure: return "resource allocation failed";
    case ParticleError::Closed: return "particle service is shut down";
    }
    return "unknown particle error";
}

double ScalarCurve::sample(double normalized_age) const noexcept {
    if (!valid_curve(*this) || !std::isfinite(normalized_age)) return 0.0;
    if (normalized_age <= keys[0].age) return keys[0].value;
    for (std::size_t i = 1; i < count; ++i) {
        if (normalized_age <= keys[i].age) {
            const double t = (normalized_age - keys[i - 1].age) / (keys[i].age - keys[i - 1].age);
            return math::lerp(keys[i - 1].value, keys[i].value, t);
        }
    }
    return keys[count - 1].value;
}

math::Vec4 ColorCurve::sample(double normalized_age) const noexcept {
    if (!valid_curve(*this) || !std::isfinite(normalized_age)) return {};
    if (normalized_age <= keys[0].age) return keys[0].value;
    for (std::size_t i = 1; i < count; ++i) {
        if (normalized_age <= keys[i].age) {
            const double t = (normalized_age - keys[i - 1].age) / (keys[i].age - keys[i - 1].age);
            return math::lerp(keys[i - 1].value, keys[i].value, t);
        }
    }
    return keys[count - 1].value;
}

bool valid(const ParticleLimits& limits) noexcept {
    return limits.emitters > 0 && limits.emitters <= 256 &&
        limits.particles > 0 && limits.particles <= 65536 &&
        limits.emissions_per_update > 0 && limits.emissions_per_update <= 65536 &&
        limits.queued_burst_particles > 0 && limits.queued_burst_particles <= 1048576 &&
        limits.mesh_quads_per_update > 0 && limits.mesh_quads_per_update <= 262144 &&
        limits.mesh_quads_per_emitter > 0 && limits.mesh_quads_per_emitter <= 262144 &&
        limits.substeps > 0 && limits.substeps <= 64 &&
        bounded(limits.maximum_step, 1.0e-5, 0.1) && bounded(limits.maximum_delta, 0.0, 1.0) &&
        limits.maximum_delta > 0.0 && bounded(limits.coordinate_limit, 1.0, 1.0e12);
}

ParticleValidation validate(const EmitterDescription& value, const ParticleLimits& limits) noexcept {
    if (!valid(limits)) return {false, "limits"};
    if (value.space != ParticleSpace::Local && value.space != ParticleSpace::World) return {false, "space"};
    if (value.shape.kind > EmissionShapeKind::Cone || !bounded(value.shape.center, limits.coordinate_limit) ||
        !bounded(value.shape.half_extents, 1.0e6) || value.shape.half_extents.x < 0.0 ||
        value.shape.half_extents.y < 0.0 || value.shape.half_extents.z < 0.0 ||
        !bounded(value.shape.axis, 1.0e6) || math::length_squared(value.shape.axis) <= 1.0e-12 ||
        !bounded(value.shape.radius, 0.0, 1.0e6) || !bounded(value.shape.height, 0.0, 1.0e6)) return {false, "shape"};
    if (!valid_forces(value.forces, limits.coordinate_limit)) return {false, "forces"};
    if (!valid_range(value.lifetime, 1.0e-4, 86400.0) || !valid_range(value.speed, 0.0, 1.0e6) ||
        !valid_range(value.size, 0.0, 10000.0) || !valid_range(value.rotation, -1.0e6, 1.0e6) ||
        !valid_range(value.angular_velocity, -10000.0, 10000.0)) return {false, "ranges"};
    if (!valid_curve(value.size_over_age) || !valid_curve(value.color_over_age)) return {false, "curves"};
    if (value.maximum_live == 0 || value.maximum_live > limits.particles ||
        value.maximum_emit_per_update == 0 || value.maximum_emit_per_update > 65536 ||
        !bounded(value.rate, 0.0, 1.0e7) || !bounded(value.duration, 1.0e-4, 86400.0) ||
        !bounded(value.spread_radians, 0.0, math::pi)) return {false, "emission"};
    if (value.burst_count > value.bursts.size()) return {false, "bursts"};
    for (std::size_t i = 0; i < value.burst_count; ++i) {
        if (!bounded(value.bursts[i].time, 0.0, value.duration) || value.bursts[i].count > 1048576)
            return {false, "bursts"};
    }
    const auto& trail = value.trail;
    if (trail.points < 2 || trail.points > particle_trail_points ||
        !bounded(trail.sample_interval, 1.0e-4, 60.0) || !bounded(trail.minimum_distance, 0.0, 1.0e6) ||
        !bounded(trail.lifetime, 1.0e-4, 60.0) || !bounded(trail.width, 0.0, 10000.0) ||
        !bounded(trail.tail_opacity, 0.0, 1.0)) return {false, "trail"};
    return {};
}

struct ParticleService::Impl {
    struct Particle {
        ParticleState state;
        std::array<ParticleTrailPoint, particle_trail_points> points{};
        std::size_t point_count{};
        double trail_clock{};
    };
    struct Emitter {
        EmitterState state;
        EmitterDescription description;
        std::vector<ParticleHandle> particles;
        std::uint64_t random{};
        std::uint64_t source_binding_instance{};
        std::uint64_t render_binding_instance{};
        std::size_t admitted{};
        bool started{};
    };
    scene::Scene& scene;
    geometry::MeshLibrary& meshes;
    render::MaterialService& materials;
    ParticleLimits limits;
    GenerationPool emitter_pool;
    GenerationPool particle_pool;
    std::vector<std::unique_ptr<Emitter>> emitters;
    std::vector<Particle> particles;
    render::MaterialHandle shared_material{};
    ParticleStatistics statistics;
    ParticleError error{ParticleError::None};
    math::Vec3 absolute_origin{};
    std::size_t simulation_cursor{};
    std::size_t mesh_cursor{};
    bool closed{};

    Impl(scene::Scene& shared_scene, geometry::MeshLibrary& shared_meshes,
         render::MaterialService& shared_materials, ParticleLimits configuration)
        : scene(shared_scene), meshes(shared_meshes), materials(shared_materials), limits(configuration),
          emitter_pool(configuration.emitters), particle_pool(configuration.particles),
          emitters(configuration.emitters), particles(configuration.particles) {}

    Emitter* get(EmitterHandle handle) noexcept {
        return emitter_pool.alive({handle.index, handle.generation}) ? emitters[handle.index - 1].get() : nullptr;
    }
    const Emitter* get(EmitterHandle handle) const noexcept {
        return emitter_pool.alive({handle.index, handle.generation}) ? emitters[handle.index - 1].get() : nullptr;
    }
    Particle* get(ParticleHandle handle) noexcept {
        return particle_pool.alive({handle.index, handle.generation}) ? &particles[handle.index - 1] : nullptr;
    }
    const Particle* get(ParticleHandle handle) const noexcept {
        return particle_pool.alive({handle.index, handle.generation}) ? &particles[handle.index - 1] : nullptr;
    }
    bool writable() noexcept {
        error = closed ? ParticleError::Closed :
            (scene.registry().iterating() ? ParticleError::SceneBusy : ParticleError::None);
        return error == ParticleError::None;
    }
    void drop(Emitter& emitter, std::uint64_t count) noexcept {
        add_count(emitter.state.dropped, count);
        add_count(statistics.dropped, count);
    }
    bool owns_node(const Emitter& emitter, bool source) const noexcept {
        const auto entity = source ? emitter.state.source : emitter.state.render_entity;
        const auto instance = source ? emitter.source_binding_instance : emitter.render_binding_instance;
        const auto* binding = scene.registry().get<ParticleBindingComponent>(entity);
        return instance != 0 && binding != nullptr && binding->emitter == emitter.state.handle &&
            binding->source == source && scene.registry().component_instance<ParticleBindingComponent>(entity) == instance;
    }
    void hide(Emitter& emitter) {
        if (!owns_node(emitter, false)) return;
        if (auto* binding = scene.registry().get<geometry::MeshComponent>(emitter.state.render_entity)) {
            binding->visible = false;
            (void)scene.registry().mark_changed<geometry::MeshComponent>(emitter.state.render_entity);
        }
        if (scene.contains(emitter.state.render_entity)) (void)scene.clear_bounds(emitter.state.render_entity);
    }
    void release(Emitter& emitter, std::size_t index) noexcept {
        const auto handle = emitter.particles[index];
        if (auto* particle = get(handle)) {
            particle->point_count = 0;
            particle->state = {};
            (void)particle_pool.release({handle.index, handle.generation});
        }
        emitter.particles[index] = emitter.particles.back();
        emitter.particles.pop_back();
        emitter.state.live_particles = emitter.particles.size();
    }
    void clear(Emitter& emitter) {
        while (!emitter.particles.empty()) release(emitter, emitter.particles.size() - 1);
        emitter.state.queued_particles = 0;
        hide(emitter);
    }
    void erase_node(const Emitter& emitter, bool source) {
        if (!owns_node(emitter, source)) return;
        const auto entity = source ? emitter.state.source : emitter.state.render_entity;
        if (scene.contains(entity)) {
            (void)scene.destroy(entity, scene::DestroyPolicy::PromoteChildren, scene::ReparentMode::KeepLocal);
        } else {
            (void)scene.registry().destroy(entity);
        }
    }
    void retire(EmitterHandle handle) {
        auto* emitter = get(handle);
        if (emitter == nullptr) return;
        clear(*emitter);
        erase_node(*emitter, false);
        if (emitter->state.owns_source) erase_node(*emitter, true);
        (void)meshes.destroy(emitter->state.mesh);
        emitters[handle.index - 1].reset();
        (void)emitter_pool.release({handle.index, handle.generation});
    }
    render::MaterialHandle default_material() {
        if (materials.materials().get(shared_material) != nullptr) return shared_material;
        render::PbrMaterial value;
        value.alpha_mode = render::AlphaMode::Blend;
        value.shading_model = render::ShadingModel::Unlit;
        value.double_sided = true;
        for (std::uint32_t i = 0; i <= materials.materials().limits().capacity; ++i) {
            const auto name = "effects.particles.shared." + std::to_string(i);
            if (materials.materials().find(name)) continue;
            shared_material = materials.materials().create(name, value);
            return shared_material;
        }
        return {};
    }
    bool binding_valid(const Emitter& emitter) const noexcept {
        const auto& state = emitter.state;
        return scene.contains(state.source) && scene.registry().alive(state.source) &&
            (!state.owns_source || owns_node(emitter, true)) &&
            scene.contains(state.render_entity) && owns_node(emitter, false) &&
            scene.parent(state.render_entity) == (state.space == ParticleSpace::Local ? state.source : ecs::Entity{});
    }
    bool enabled(ecs::Entity entity) const noexcept {
        for (std::size_t depth = 0; entity && depth < scene.limits().max_depth; ++depth) {
            if (!scene.registry().enabled(entity)) return false;
            entity = scene.parent(entity);
        }
        return !entity;
    }
    void append_trail(Particle& particle, const EmitterDescription& description) noexcept {
        const auto& settings = description.trail;
        if (!settings.enabled) return;
        if (particle.point_count == settings.points) {
            for (std::size_t i = 1; i < particle.point_count; ++i) particle.points[i - 1] = particle.points[i];
            --particle.point_count;
        }
        const auto& state = particle.state;
        particle.points[particle.point_count++] = {state.position, state.color, state.age,
            settings.width * description.size_over_age.sample(state.age / state.lifetime)};
        particle.trail_clock = 0.0;
    }
    bool emit(Emitter& emitter, const math::Mat4& source) {
        const auto token = particle_pool.acquire();
        if (!token) return false;
        const ParticleHandle handle{token->index, token->generation};
        try {
            emitter.particles.push_back(handle);
        } catch (...) {
            (void)particle_pool.release(*token);
            throw;
        }
        auto& particle = particles[token->index - 1];
        particle = {};
        auto& state = particle.state;
        const auto& description = emitter.description;
        state.handle = handle;
        state.emitter = emitter.state.handle;
        state.position = sample_shape(description.shape, emitter.random);
        const auto direction = sample_direction(description, state.position, emitter.random);
        const double speed = random_range(emitter.random, description.speed);
        state.velocity = direction * speed;
        if (description.space == ParticleSpace::World) {
            state.position = math::transform_point(source, state.position);
            state.velocity = math::transform_vector(source, state.velocity);
        }
        state.velocity = math::clamp_length(state.velocity, description.forces.maximum_speed);
        state.previous_position = state.position;
        state.lifetime = random_range(emitter.random, description.lifetime);
        state.initial_size = random_range(emitter.random, description.size);
        state.rotation = random_range(emitter.random, description.rotation);
        state.angular_velocity = random_range(emitter.random, description.angular_velocity);
        state.size = state.initial_size * description.size_over_age.sample(0.0);
        state.color = description.color_over_age.sample(0.0);
        if (!bounded(state.position, limits.coordinate_limit) || !math::finite(state.velocity)) {
            release(emitter, emitter.particles.size() - 1);
            add_count(statistics.invalid_particles);
            return false;
        }
        add_count(emitter.state.emitted);
        add_count(statistics.emitted);
        state.serial = emitter.state.emitted;
        emitter.state.live_particles = emitter.particles.size();
        append_trail(particle, description);
        return true;
    }
    std::uint64_t scheduled(Emitter& emitter, double delta) noexcept {
        const auto& description = emitter.description;
        const double old_time = emitter.state.elapsed;
        const double new_time = std::min(1.0e12, old_time + delta);
        const double active_delta = description.looping ? new_time - old_time :
            std::max(0.0, std::min(new_time, description.duration) - std::min(old_time, description.duration));
        const double amount = emitter.state.fractional_emission + active_delta * description.rate;
        auto count = static_cast<std::uint64_t>(std::floor(amount));
        emitter.state.fractional_emission = amount - static_cast<double>(count);
        for (std::size_t i = 0; i < description.burst_count; ++i) {
            const auto burst = description.bursts[i];
            double occurrences = 0.0;
            if (description.looping) {
                const double before = old_time < burst.time ? -1.0 : std::floor((old_time - burst.time) / description.duration);
                const double after = new_time < burst.time ? -1.0 : std::floor((new_time - burst.time) / description.duration);
                occurrences = after - before;
            } else if (burst.time > old_time && burst.time <= new_time) {
                occurrences = 1.0;
            }
            if (!emitter.started && burst.time == 0.0) occurrences += 1.0;
            add_count(count, static_cast<std::uint64_t>(std::max(0.0, occurrences)) * burst.count);
        }
        emitter.started = true;
        emitter.state.elapsed = new_time;
        if ((!description.looping && new_time >= description.duration) || new_time >= 1.0e12)
            emitter.state.playback = ParticlePlayback::Stopped;
        return count;
    }
    std::size_t build(Emitter& emitter, const render::CameraView& camera, std::size_t budget);
    void simulate(Emitter& emitter, double delta) noexcept {
        const auto& description = emitter.description;
        for (std::size_t i = 0; i < emitter.particles.size();) {
            auto& particle = particles[emitter.particles[i].index - 1];
            auto& state = particle.state;
            state.previous_position = state.position;
            state.age += delta;
            if (state.age >= state.lifetime) {
                release(emitter, i);
                add_count(statistics.expired);
                continue;
            }
            const auto& forces = description.forces;
            auto acceleration = forces.acceleration;
            const auto radial = forces.radial_center - state.position;
            const double denominator = math::length_squared(radial) + forces.radial_softening * forces.radial_softening;
            acceleration += math::normalized(radial) * (forces.radial_strength / denominator);
            acceleration = math::clamp_length(acceleration, forces.maximum_acceleration);
            const auto old_velocity = state.velocity;
            const double attenuation = std::exp(-forces.drag * delta);
            const double integral = forces.drag > 1.0e-8 ? -std::expm1(-forces.drag * delta) / forces.drag : delta;
            state.velocity = forces.wind_velocity + (state.velocity - forces.wind_velocity) * attenuation + acceleration * integral;
            state.velocity = math::clamp_length(state.velocity, forces.maximum_speed);
            state.position += (old_velocity + state.velocity) * (0.5 * delta);
            state.rotation = math::wrap_angle(state.rotation + state.angular_velocity * delta);
            state.size = state.initial_size * description.size_over_age.sample(state.age / state.lifetime);
            state.color = description.color_over_age.sample(state.age / state.lifetime);
            if (!bounded(state.position, limits.coordinate_limit) || !math::finite(state.velocity)) {
                release(emitter, i);
                add_count(statistics.invalid_particles);
                continue;
            }
            while (particle.point_count > 0 && state.age - particle.points[0].age > description.trail.lifetime) {
                for (std::size_t p = 1; p < particle.point_count; ++p) particle.points[p - 1] = particle.points[p];
                --particle.point_count;
            }
            particle.trail_clock += delta;
            if (description.trail.enabled && (particle.point_count == 0 ||
                (particle.trail_clock >= description.trail.sample_interval &&
                 math::distance(state.position, particle.points[particle.point_count - 1].position) >= description.trail.minimum_distance)))
                append_trail(particle, description);
            ++i;
        }
    }
};

ParticleService::ParticleService(scene::Scene& scene, geometry::MeshLibrary& meshes,
                                 render::MaterialService& materials, ParticleLimits limits) {
    if (!valid(limits)) throw std::invalid_argument("invalid particle service limits");
    impl_ = std::make_unique<Impl>(scene, meshes, materials, limits);
}

ParticleService::~ParticleService() = default;

EmitterHandle ParticleService::create(std::string_view name, const EmitterDescription& description,
                                      ecs::Entity source) {
    auto& data = *impl_;
    if (!data.writable()) return {};
    if (!validate(description, data.limits) || name.empty() || name.size() > 96 ||
        name.find('\0') != std::string_view::npos || find(name)) {
        data.error = ParticleError::InvalidDescription;
        return {};
    }
    if (source && (!data.scene.contains(source) || !data.scene.registry().alive(source))) {
        data.error = ParticleError::InvalidSource;
        return {};
    }
    const bool owns_source = !source;
    EmitterHandle handle;
    ecs::Entity binding;
    geometry::MeshHandle mesh;
    try {
        auto emitter = std::make_unique<Impl::Emitter>();
        emitter->state.name = name;
        emitter->description = description;
        emitter->random = description.seed;
        emitter->particles.reserve(std::min<std::size_t>(64, description.maximum_live));
        const auto material = description.material ? description.material : data.default_material();
        if (data.materials.materials().get(material) == nullptr) {
            data.error = ParticleError::MissingMaterial;
            return {};
        }
        const auto token = data.emitter_pool.acquire();
        if (!token) {
            data.error = ParticleError::Capacity;
            return {};
        }
        handle = {token->index, token->generation};
        if (owns_source) {
            source = data.scene.create(name);
            if (source) data.scene.registry().emplace<ParticleBindingComponent>(source,
                ParticleBindingComponent{handle, true});
        }
        if (!source) throw std::runtime_error("particle source creation failed");
        binding = data.scene.create(std::string(name) + ".particles",
            description.space == ParticleSpace::Local ? source : ecs::Entity{});
        if (!binding) throw std::runtime_error("particle render binding creation failed");
        data.scene.registry().emplace<ParticleBindingComponent>(binding, ParticleBindingComponent{handle, false});
        (void)data.scene.registry().set_layers(binding, description.layers);
        mesh = data.meshes.create({});
        if (!mesh) throw std::runtime_error("particle mesh creation failed");
        data.scene.registry().emplace<geometry::MeshComponent>(binding, geometry::MeshComponent{mesh, 0, 0, false});
        if (!data.materials.materials().assign(data.scene.registry(), binding, material))
            throw std::runtime_error("particle render resource assignment failed");
        emitter->state.handle = handle;
        emitter->state.source = source;
        emitter->state.render_entity = binding;
        emitter->state.mesh = mesh;
        emitter->state.material = material;
        emitter->state.space = description.space;
        emitter->state.owns_source = owns_source;
        if (owns_source) emitter->source_binding_instance =
            data.scene.registry().component_instance<ParticleBindingComponent>(source);
        emitter->render_binding_instance =
            data.scene.registry().component_instance<ParticleBindingComponent>(binding);
        data.emitters[handle.index - 1] = std::move(emitter);
        return handle;
    } catch (const std::exception&) {
        if (binding && data.scene.contains(binding)) (void)data.scene.destroy(binding);
        if (owns_source && source && data.scene.contains(source)) (void)data.scene.destroy(source);
        (void)data.meshes.destroy(mesh);
        (void)data.emitter_pool.release({handle.index, handle.generation});
        data.error = ParticleError::AllocationFailure;
        return {};
    }
}

bool ParticleService::destroy(EmitterHandle handle) {
    if (!impl_->writable()) return false;
    if (!contains(handle)) { impl_->error = ParticleError::InvalidHandle; return false; }
    impl_->retire(handle);
    return true;
}

bool ParticleService::clear(EmitterHandle handle) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    impl_->clear(*emitter);
    return true;
}

std::size_t ParticleService::clear_all() {
    if (!impl_->writable()) return 0;
    const auto count = static_cast<std::size_t>(impl_->particle_pool.size());
    for (auto& emitter : impl_->emitters) if (emitter) impl_->clear(*emitter);
    return count;
}

bool ParticleService::play(EmitterHandle handle, bool restart) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    if (restart) {
        impl_->clear(*emitter);
        emitter->state.elapsed = 0.0;
        emitter->state.fractional_emission = 0.0;
        emitter->state.emitted = 0;
        emitter->state.dropped = 0;
        emitter->random = emitter->description.seed;
        emitter->started = false;
    }
    emitter->state.playback = ParticlePlayback::Playing;
    return true;
}

bool ParticleService::pause(EmitterHandle handle) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    emitter->state.playback = ParticlePlayback::Paused;
    return true;
}

bool ParticleService::stop(EmitterHandle handle, bool clear_particles) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    emitter->state.playback = ParticlePlayback::Stopped;
    emitter->state.queued_particles = 0;
    if (clear_particles) impl_->clear(*emitter);
    return true;
}

std::size_t ParticleService::burst(EmitterHandle handle, std::size_t count) {
    if (!impl_->writable()) return 0;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return 0; }
    std::size_t queued = 0;
    for (const auto& item : impl_->emitters) if (item) queued += item->state.queued_particles;
    const auto accepted = std::min(count, impl_->limits.queued_burst_particles - queued);
    emitter->state.queued_particles += accepted;
    impl_->drop(*emitter, static_cast<std::uint64_t>(count - accepted));
    if (accepted != count) impl_->error = ParticleError::Capacity;
    return accepted;
}

bool ParticleService::set_rate(EmitterHandle handle, double rate) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    if (!bounded(rate, 0.0, 1.0e7)) { impl_->error = ParticleError::InvalidDescription; return false; }
    emitter->description.rate = rate;
    return true;
}

bool ParticleService::set_forces(EmitterHandle handle, const ParticleForces& forces) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    if (!valid_forces(forces, impl_->limits.coordinate_limit)) {
        impl_->error = ParticleError::InvalidDescription;
        return false;
    }
    emitter->description.forces = forces;
    return true;
}

bool ParticleService::set_curves(EmitterHandle handle, const ScalarCurve& size, const ColorCurve& color) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    if (!valid_curve(size) || !valid_curve(color)) { impl_->error = ParticleError::InvalidDescription; return false; }
    emitter->description.size_over_age = size;
    emitter->description.color_over_age = color;
    for (const auto particle_handle : emitter->particles) {
        auto& state = impl_->particles[particle_handle.index - 1].state;
        state.size = state.initial_size * size.sample(state.age / state.lifetime);
        state.color = color.sample(state.age / state.lifetime);
    }
    return true;
}

bool ParticleService::set_material(EmitterHandle handle, render::MaterialHandle material) {
    if (!impl_->writable()) return false;
    auto* emitter = impl_->get(handle);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    if (!impl_->binding_valid(*emitter)) { impl_->error = ParticleError::SceneRejected; return false; }
    try {
        const auto selected = material ? material : impl_->default_material();
        if (!impl_->materials.materials().assign(impl_->scene.registry(), emitter->state.render_entity, selected)) {
            impl_->error = ParticleError::MissingMaterial;
            return false;
        }
        emitter->description.material = material;
        emitter->state.material = selected;
        return true;
    } catch (const std::exception&) {
        impl_->error = ParticleError::AllocationFailure;
        return false;
    }
}

bool ParticleService::kill(ParticleHandle handle) {
    if (!impl_->writable()) return false;
    const auto* particle = impl_->get(handle);
    auto* emitter = particle == nullptr ? nullptr : impl_->get(particle->state.emitter);
    if (emitter == nullptr) { impl_->error = ParticleError::InvalidHandle; return false; }
    const auto found = std::find(emitter->particles.begin(), emitter->particles.end(), handle);
    if (found == emitter->particles.end()) return false;
    impl_->release(*emitter, static_cast<std::size_t>(found - emitter->particles.begin()));
    if (emitter->particles.empty()) impl_->hide(*emitter);
    return true;
}

void ParticleService::update(double delta_seconds) {
    auto& data = *impl_;
    if (!data.writable()) return;
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) { data.error = ParticleError::InvalidTime; return; }
    const double accepted = std::min({delta_seconds, data.limits.maximum_delta,
        data.limits.maximum_step * static_cast<double>(data.limits.substeps)});
    const auto steps = std::max<std::size_t>(1, std::min(data.limits.substeps,
        static_cast<std::size_t>(std::ceil(accepted / data.limits.maximum_step))));
    const double step = accepted / static_cast<double>(steps);
    data.statistics.dropped_seconds += std::min(delta_seconds - accepted, 1.0e300 - data.statistics.dropped_seconds);
    data.statistics.simulated_seconds = std::min(1.0e300, data.statistics.simulated_seconds + accepted);
    (void)data.scene.update();
    for (auto& emitter : data.emitters) if (emitter) emitter->admitted = 0;
    auto remaining = data.limits.emissions_per_update;
    for (std::size_t substep = 0; substep < steps; ++substep) {
        for (std::size_t offset = 0; offset < data.emitters.size(); ++offset) {
            auto* emitter = data.emitters[(offset + data.simulation_cursor) % data.emitters.size()].get();
            if (emitter == nullptr) continue;
            if (!data.binding_valid(*emitter)) { data.retire(emitter->state.handle); continue; }
            if (!data.enabled(emitter->state.source)) { data.hide(*emitter); continue; }
            if (emitter->state.playback == ParticlePlayback::Paused) continue;
            const auto* source = data.scene.cached_world_matrix(emitter->state.source);
            if (source == nullptr) { data.hide(*emitter); continue; }
            data.simulate(*emitter, step);
            std::uint64_t requested = static_cast<std::uint64_t>(emitter->state.queued_particles);
            emitter->state.queued_particles = 0;
            if (emitter->state.playback == ParticlePlayback::Playing) add_count(requested, data.scheduled(*emitter, step));
            auto allowed = std::min({emitter->description.maximum_live - emitter->particles.size(), remaining,
                emitter->description.maximum_emit_per_update - emitter->admitted,
                static_cast<std::size_t>(data.particle_pool.available_slots())});
            const auto quota = emitter->description.maximum_total_emissions;
            if (quota != 0) allowed = static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(allowed), quota - std::min(quota, emitter->state.emitted)));
            allowed = static_cast<std::size_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(allowed), requested));
            std::uint64_t emitted = 0;
            for (std::size_t i = 0; i < allowed; ++i) {
                --remaining;
                ++emitter->admitted;
                try {
                    if (data.emit(*emitter, *source)) ++emitted;
                } catch (const std::exception&) {
                    data.error = ParticleError::AllocationFailure;
                    break;
                }
            }
            data.drop(*emitter, requested - emitted);
            if (quota != 0 && emitter->state.emitted >= quota) emitter->state.playback = ParticlePlayback::Stopped;
            if (emitter->particles.empty()) data.hide(*emitter);
        }
    }
    data.simulation_cursor = (data.simulation_cursor + 1) % data.emitters.size();
    add_count(data.statistics.updates);
}

namespace {

bool append_quad(geometry::MeshData& mesh, const std::array<math::Vec3, 4>& points,
                 const std::array<math::Vec4, 4>& colors, const math::Mat4& model,
                 const math::Mat4& inverse_model, math::Vec3 normal, math::Vec3 tangent,
                 double coordinate_limit, double first_v = 1.0, double last_v = 0.0) {
    std::array<geometry::Vertex, 4> vertices;
    const std::array<math::Vec2, 4> uvs{{{0.0, first_v}, {1.0, first_v}, {1.0, last_v}, {0.0, last_v}}};
    normal = math::normalized(math::transpose(math::upper3x3(model)) * normal, {0.0, 0.0, 1.0});
    tangent = math::normalized(math::transform_vector(inverse_model, tangent), {1.0, 0.0, 0.0});
    const double handedness = math::determinant(math::upper3x3(model)) < 0.0 ? -1.0 : 1.0;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto local = math::transform_point(inverse_model, points[i]);
        if (!bounded(points[i], coordinate_limit) || !bounded(local, coordinate_limit)) return false;
        vertices[i] = {local, normal, {tangent.x, tangent.y, tangent.z, handedness}, colors[i], uvs[i]};
    }
    const auto base = static_cast<geometry::MeshIndex>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());
    const std::array<geometry::MeshIndex, 6> indices{{base, base + 1U, base + 2U, base, base + 2U, base + 3U}};
    mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
    return true;
}

} // namespace

std::size_t ParticleService::Impl::build(Emitter& emitter, const render::CameraView& camera, std::size_t budget) {
    const auto& description = emitter.description;
    math::Mat4 model;
    if (description.space == ParticleSpace::Local) {
        const auto* matrix = scene.cached_world_matrix(emitter.state.source);
        if (matrix == nullptr) { hide(emitter); return 0; }
        model = *matrix;
    }
    const auto inverse_model = math::inverse(model);
    if (!inverse_model) { hide(emitter); return 0; }
    struct Entry { const Particle* particle; math::Vec3 center; double depth; };
    std::vector<Entry> order;
    order.reserve(emitter.particles.size());
    const auto forward = math::normalized(camera.forward, {0.0, 0.0, -1.0});
    const auto fallback_basis = math::orthonormal_basis(forward);
    const auto right = math::normalized(math::cross(forward, camera.up), fallback_basis.first);
    const auto up = math::normalized(math::cross(right, forward), fallback_basis.second);
    for (const auto handle : emitter.particles) {
        const auto& particle = particles[handle.index - 1];
        const auto center = math::transform_point(model, particle.state.position);
        if (bounded(center, limits.coordinate_limit)) order.push_back({&particle, center, math::dot(center - camera.position, forward)});
    }
    if (description.depth_sort) std::sort(order.begin(), order.end(), [](const Entry& first, const Entry& second) {
        return first.depth == second.depth ? first.particle->state.serial < second.particle->state.serial : first.depth > second.depth;
    });
    const auto& mesh_limits = meshes.limits().mesh;
    const auto available = std::min({budget, limits.mesh_quads_per_emitter,
        mesh_limits.max_vertices / 4, mesh_limits.max_indices / 6,
        order.size() * ((description.billboards ? 1U : 0U) + (description.trail.enabled ? description.trail.points : 0U))});
    geometry::MeshData mesh;
    mesh.vertices.reserve(available * 4);
    mesh.indices.reserve(available * 6);
    std::size_t quads = 0;
    for (const auto& entry : order) {
        if (quads == available) break;
        const auto& particle = *entry.particle;
        const auto& state = particle.state;
        if (description.billboards && state.size > 1.0e-9 && state.color.w > 0.0) {
            const auto horizontal = (right * std::cos(state.rotation) + up * std::sin(state.rotation)) * (state.size * 0.5);
            const auto vertical = (up * std::cos(state.rotation) - right * std::sin(state.rotation)) * (state.size * 0.5);
            const std::array<math::Vec3, 4> points{{entry.center - horizontal - vertical, entry.center + horizontal - vertical,
                entry.center + horizontal + vertical, entry.center - horizontal + vertical}};
            const std::array<math::Vec4, 4> colors{{state.color, state.color, state.color, state.color}};
            if (append_quad(mesh, points, colors, model, *inverse_model, -forward, right, limits.coordinate_limit)) ++quads;
        }
        if (!description.trail.enabled || quads == available) continue;
        std::array<ParticleTrailPoint, particle_trail_points + 1> points;
        std::array<math::Vec3, particle_trail_points + 1> sides;
        std::array<double, particle_trail_points + 1> distances{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < particle.point_count; ++i) {
            auto point = particle.points[i];
            if (state.age - point.age > description.trail.lifetime) continue;
            point.position = math::transform_point(model, point.position);
            if (!bounded(point.position, limits.coordinate_limit)) { count = 0; break; }
            point.color.w *= math::lerp(description.trail.tail_opacity, 1.0,
                math::saturate(1.0 - (state.age - point.age) / description.trail.lifetime));
            points[count++] = point;
        }
        if (count > 0 && math::distance_squared(points[count - 1].position, entry.center) > 1.0e-18)
            points[count++] = {entry.center, state.color, state.age,
                description.trail.width * description.size_over_age.sample(state.age / state.lifetime)};
        if (count < 2) continue;
        for (std::size_t i = 0; i < count; ++i) {
            const auto tangent = points[std::min(i + 1, count - 1)].position - points[i == 0 ? 0 : i - 1].position;
            const auto toward_eye = camera.projection_mode == render::ProjectionMode::Orthographic ? -forward :
                math::normalized(camera.position - points[i].position, -forward);
            sides[i] = math::normalized(math::cross(tangent, toward_eye), right);
            if (i > 0) {
                if (math::dot(sides[i], sides[i - 1]) < 0.0) sides[i] = -sides[i];
                distances[i] = distances[i - 1] + math::distance(points[i].position, points[i - 1].position);
            }
        }
        const double length = distances[count - 1];
        if (length <= 1.0e-9) continue;
        for (std::size_t i = 1; i < count && quads < available; ++i) {
            if (distances[i] - distances[i - 1] <= 1.0e-9) continue;
            const auto previous_side = sides[i - 1] * (points[i - 1].width * 0.5);
            const auto next_side = sides[i] * (points[i].width * 0.5);
            if (points[i - 1].width <= 1.0e-9 && points[i].width <= 1.0e-9) continue;
            const std::array<math::Vec3, 4> corners{{points[i - 1].position - previous_side,
                points[i - 1].position + previous_side, points[i].position + next_side, points[i].position - next_side}};
            const std::array<math::Vec4, 4> colors{{points[i - 1].color, points[i - 1].color, points[i].color, points[i].color}};
            if (append_quad(mesh, corners, colors, model, *inverse_model, -forward, sides[i], limits.coordinate_limit,
                distances[i - 1] / length, distances[i] / length)) ++quads;
        }
    }
    const auto bounds = geometry::mesh_bounds(mesh);
    if (!meshes.replace(emitter.state.mesh, std::move(mesh))) {
        error = ParticleError::MissingMesh;
        hide(emitter);
        return 0;
    }
    if (!bounds || !scene.set_local_bounds(emitter.state.render_entity, *bounds)) { hide(emitter); return 0; }
    if (!meshes.assign(scene.registry(), emitter.state.render_entity, emitter.state.mesh) ||
        !materials.materials().assign(scene.registry(), emitter.state.render_entity, emitter.state.material)) {
        hide(emitter);
        return 0;
    }
    return quads;
}

std::size_t ParticleService::rebuild_meshes(const render::CameraView* camera) {
    auto& data = *impl_;
    if (!data.writable()) return 0;
    if (camera == nullptr || !bounded(camera->position, 1.0e12) ||
        !math::finite(camera->forward) || !math::finite(camera->up)) {
        for (auto& emitter : data.emitters) if (emitter) data.hide(*emitter);
        data.statistics.mesh_quads = 0;
        return 0;
    }
    for (auto& emitter : data.emitters) {
        if (emitter && data.binding_valid(*emitter)) (void)data.scene.set_local_transform(emitter->state.render_entity, {});
    }
    (void)data.scene.update();
    std::size_t quads = 0;
    for (std::size_t offset = 0; offset < data.emitters.size(); ++offset) {
        auto* emitter = data.emitters[(offset + data.mesh_cursor) % data.emitters.size()].get();
        if (emitter == nullptr) continue;
        if (!data.binding_valid(*emitter)) { data.retire(emitter->state.handle); continue; }
        if (!data.enabled(emitter->state.source) || quads == data.limits.mesh_quads_per_update) { data.hide(*emitter); continue; }
        if (!data.meshes.contains(emitter->state.mesh)) { data.error = ParticleError::MissingMesh; data.hide(*emitter); continue; }
        if (data.materials.materials().get(emitter->state.material) == nullptr) {
            data.error = ParticleError::MissingMaterial;
            data.hide(*emitter);
            continue;
        }
        try {
            quads += data.build(*emitter, *camera, data.limits.mesh_quads_per_update - quads);
        } catch (const std::exception&) {
            data.hide(*emitter);
            data.error = ParticleError::AllocationFailure;
            add_count(data.statistics.mesh_failures);
        }
    }
    (void)data.scene.update();
    data.mesh_cursor = (data.mesh_cursor + 1) % data.emitters.size();
    data.statistics.mesh_quads = quads;
    return quads;
}

bool ParticleService::shift_origin(math::Vec3 offset) {
    auto& data = *impl_;
    if (!data.writable()) return false;
    if (!bounded(offset, 1.0e12)) { data.error = ParticleError::InvalidOrigin; return false; }
    std::vector<scene::LocalTransformEdit> edits;
    try {
        edits.reserve(data.emitter_pool.size());
        for (const auto& emitter : data.emitters) {
            if (!emitter || emitter->state.space != ParticleSpace::World) continue;
            if (emitter->description.forces.radial_strength != 0.0 &&
                !bounded(emitter->description.forces.radial_center - offset, data.limits.coordinate_limit)) {
                data.error = ParticleError::InvalidOrigin;
                return false;
            }
            for (const auto handle : emitter->particles) {
                const auto& particle = data.particles[handle.index - 1];
                bool finite = bounded(particle.state.position - offset, data.limits.coordinate_limit) &&
                    bounded(particle.state.previous_position - offset, data.limits.coordinate_limit);
                for (std::size_t i = 0; i < particle.point_count; ++i)
                    finite = finite && bounded(particle.points[i].position - offset, data.limits.coordinate_limit);
                if (!finite) { data.error = ParticleError::InvalidOrigin; return false; }
            }
            if (data.binding_valid(*emitter)) edits.push_back({emitter->state.render_entity, {}});
        }
        if (!data.scene.apply_local_transforms(edits)) { data.error = ParticleError::SceneRejected; return false; }
    } catch (const std::exception&) {
        data.error = ParticleError::AllocationFailure;
        return false;
    }
    for (auto& emitter : data.emitters) {
        if (!emitter || emitter->state.space != ParticleSpace::World) continue;
        if (emitter->description.forces.radial_strength != 0.0) emitter->description.forces.radial_center -= offset;
        for (const auto handle : emitter->particles) {
            auto& particle = data.particles[handle.index - 1];
            particle.state.position -= offset;
            particle.state.previous_position -= offset;
            for (std::size_t i = 0; i < particle.point_count; ++i) particle.points[i].position -= offset;
        }
        data.hide(*emitter);
    }
    return true;
}

bool ParticleService::synchronize_origin(const scene::WorldPartition& partition) {
    auto& data = *impl_;
    if (!data.writable()) return false;
    const auto latest = partition.origin();
    if (!latest.consistent) {
        for (auto& emitter : data.emitters) if (emitter && emitter->state.space == ParticleSpace::World) data.clear(*emitter);
        data.error = ParticleError::InvalidOrigin;
        return false;
    }
    if (latest.epoch == data.statistics.origin_epoch) return true;
    const auto history = partition.shifts_since(data.statistics.origin_epoch, partition.limits().shift_history);
    if (history.history_lost) {
        for (auto& emitter : data.emitters) if (emitter && emitter->state.space == ParticleSpace::World) data.clear(*emitter);
        add_count(data.statistics.origin_resets);
        if (!shift_origin(history.latest.absolute_origin - data.absolute_origin)) return false;
        data.absolute_origin = history.latest.absolute_origin;
        data.statistics.origin_epoch = history.latest.epoch;
        return true;
    }
    for (const auto& shift : history.shifts) {
        if (shift.previous_epoch != data.statistics.origin_epoch || !shift_origin(shift.offset)) return false;
        data.absolute_origin = shift.new_origin;
        data.statistics.origin_epoch = shift.epoch;
    }
    return data.statistics.origin_epoch == latest.epoch;
}

void ParticleService::shutdown() {
    if (impl_->closed || !impl_->writable()) return;
    for (auto& emitter : impl_->emitters) if (emitter) impl_->retire(emitter->state.handle);
    (void)impl_->materials.materials().erase(impl_->shared_material);
    impl_->shared_material = {};
    impl_->closed = true;
}

bool ParticleService::contains(EmitterHandle handle) const noexcept { return impl_->get(handle) != nullptr; }
bool ParticleService::contains(ParticleHandle handle) const noexcept { return impl_->get(handle) != nullptr; }
ParticleError ParticleService::last_error() const noexcept { return impl_->error; }
const ParticleLimits& ParticleService::limits() const noexcept { return impl_->limits; }

EmitterHandle ParticleService::find(std::string_view name) const noexcept {
    for (const auto& emitter : impl_->emitters) if (emitter && emitter->state.name == name) return emitter->state.handle;
    return {};
}

std::optional<EmitterState> ParticleService::inspect(EmitterHandle handle) const {
    const auto* emitter = impl_->get(handle);
    return emitter == nullptr ? std::nullopt : std::optional<EmitterState>{emitter->state};
}

std::optional<EmitterDescription> ParticleService::description(EmitterHandle handle) const {
    const auto* emitter = impl_->get(handle);
    return emitter == nullptr ? std::nullopt : std::optional<EmitterDescription>{emitter->description};
}

std::optional<ParticleState> ParticleService::inspect(ParticleHandle handle) const noexcept {
    const auto* particle = impl_->get(handle);
    return particle == nullptr ? std::nullopt : std::optional<ParticleState>{particle->state};
}

std::vector<EmitterState> ParticleService::emitters() const {
    std::vector<EmitterState> result;
    result.reserve(impl_->emitter_pool.size());
    for (const auto& emitter : impl_->emitters) if (emitter) result.push_back(emitter->state);
    return result;
}

std::vector<ParticleState> ParticleService::particles(EmitterHandle handle, std::size_t limit) const {
    std::vector<ParticleState> result;
    const auto* emitter = impl_->get(handle);
    if (emitter == nullptr) return result;
    const auto count = std::min(limit, emitter->particles.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) result.push_back(impl_->particles[emitter->particles[i].index - 1].state);
    return result;
}

std::vector<ParticleTrailPoint> ParticleService::trail(ParticleHandle handle) const {
    const auto* particle = impl_->get(handle);
    if (particle == nullptr) return {};
    return {particle->points.begin(), particle->points.begin() + static_cast<std::ptrdiff_t>(particle->point_count)};
}

ParticleStatistics ParticleService::statistics() const noexcept {
    auto result = impl_->statistics;
    result.emitters = impl_->emitter_pool.size();
    result.particles = impl_->particle_pool.size();
    result.trail_points = 0;
    result.queued_particles = 0;
    for (const auto& emitter : impl_->emitters) {
        if (!emitter) continue;
        result.queued_particles += emitter->state.queued_particles;
        for (const auto handle : emitter->particles) result.trail_points += impl_->particles[handle.index - 1].point_count;
    }
    return result;
}

void register_particles(core::Kernel& kernel) {
    if (kernel.services().find<ParticleService>() != nullptr) return;
    auto* scene = kernel.services().find<scene::Scene>();
    auto* meshes = kernel.services().find<geometry::MeshLibrary>();
    auto* materials = kernel.services().find<render::MaterialService>();
    if (scene == nullptr || meshes == nullptr || materials == nullptr) {
        kernel.diagnose(core::Diagnostic::Severity::Warning, "effects.particles", "Scene, MeshLibrary and MaterialService are required");
        return;
    }
    ParticleLimits limits;
    const auto capacity = kernel.config().integer("particles.capacity", static_cast<std::int64_t>(limits.particles));
    const auto emitters = kernel.config().integer("particles.emitters", static_cast<std::int64_t>(limits.emitters));
    if (capacity < 1 || capacity > 65536 || emitters < 1 || emitters > 256)
        throw std::invalid_argument("particles.capacity or particles.emitters exceeds supported bounds");
    limits.particles = static_cast<std::uint32_t>(capacity);
    limits.emitters = static_cast<std::uint32_t>(emitters);
    auto& service = kernel.services().emplace<ParticleService>(*scene, *meshes, *materials, limits);
    if (!kernel.register_module({"effects.particles", "Bounded seeded particles and shared billboard/ribbon meshes", 1}) ||
        !kernel.add_shutdown("effects.particles.resources", [pointer = &service] { pointer->shutdown(); })) {
        service.shutdown();
        throw std::logic_error("particle module or shutdown registration conflict");
    }
    if (const auto* partition = kernel.services().find<scene::WorldPartition>()) (void)service.synchronize_origin(*partition);
    (void)kernel.add_system({"effects.particles.simulate", core::Phase::PostSimulation, 100, {}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& particles = host.services().get<ParticleService>();
            if (const auto* partition = host.services().find<scene::WorldPartition>(); partition && !particles.synchronize_origin(*partition)) {
                host.diagnose(core::Diagnostic::Severity::Warning, "effects.particles", std::string(describe(particles.last_error())));
                (void)particles.rebuild_meshes(nullptr);
                return;
            }
            particles.update(frame.delta_seconds);
            const auto state = particles.statistics();
            host.metric("particles.live", static_cast<double>(state.particles));
            host.metric("particles.emitters", static_cast<double>(state.emitters));
            host.metric("particles.emitted", static_cast<double>(state.emitted));
            host.metric("particles.dropped", static_cast<double>(state.dropped));
            host.metric("particles.trail_points", static_cast<double>(state.trail_points));
        });
    (void)kernel.add_system({"effects.particles.meshes", core::Phase::Visibility, -120, {"camera.update"}},
        [](core::Kernel& host, const core::Frame&) {
            auto& particles = host.services().get<ParticleService>();
            const auto* cameras = host.services().find<render::CameraService>();
            if (const auto* partition = host.services().find<scene::WorldPartition>(); partition && !particles.synchronize_origin(*partition)) {
                (void)particles.rebuild_meshes(nullptr);
                return;
            }
            const auto quads = particles.rebuild_meshes(cameras == nullptr ? nullptr : cameras->current_view());
            host.metric("particles.mesh_quads", static_cast<double>(quads));
            host.metric("particles.mesh_failures", static_cast<double>(particles.statistics().mesh_failures));
        });
    (void)kernel.order_after("geometry.mesh.bounds", "effects.particles.meshes");
    (void)kernel.add_command("particles.create", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 5) return {false, "usage: particles.create NAME [point|box|sphere|disk|cone] [local|world] [SOURCE_NAME|-] [trail]"};
        auto& particles = host.services().get<ParticleService>();
        EmitterDescription description;
        description.maximum_live = std::min(description.maximum_live, static_cast<std::size_t>(particles.limits().particles));
        if (args.size() >= 2) {
            if (args[1] == "point") description.shape.kind = EmissionShapeKind::Point;
            else if (args[1] == "box") description.shape.kind = EmissionShapeKind::Box;
            else if (args[1] == "sphere") description.shape.kind = EmissionShapeKind::Sphere;
            else if (args[1] == "disk") description.shape.kind = EmissionShapeKind::Disk;
            else if (args[1] == "cone") description.shape.kind = EmissionShapeKind::Cone;
            else return {false, "unknown emission shape"};
        }
        if (args.size() >= 3) {
            if (args[2] == "local") description.space = ParticleSpace::Local;
            else if (args[2] != "world") return {false, "space must be local or world"};
        }
        ecs::Entity source;
        if (args.size() >= 4 && args[3] != "-") {
            source = host.services().get<scene::Scene>().registry().find_name(args[3]);
            if (!source) return {false, "source entity name was not found"};
        }
        if (args.size() == 5) {
            if (args[4] != "trail") return {false, "the final option must be trail"};
            description.trail.enabled = true;
        }
        const auto handle = particles.create(args[0], description, source);
        return {handle.valid(), handle ? "emitter=" + handle_text(handle) : std::string(describe(particles.last_error()))};
    }, "Create a seeded Scene-bound emitter with shared material and mesh resources");
    (void)kernel.add_command("particles.burst", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) return {false, "usage: particles.burst EMITTER COUNT"};
        const auto count = number<std::uint32_t>(args[1]);
        if (!count) return {false, "count must be an unsigned integer"};
        auto& particles = host.services().get<ParticleService>();
        const auto accepted = particles.burst(resolve(particles, args[0]), *count);
        return {particles.last_error() == ParticleError::None, "queued=" + std::to_string(accepted) + " error=" + std::string(describe(particles.last_error()))};
    }, "Queue a bounded explicit burst, including on a stopped emitter");
    (void)kernel.add_command("particles.control", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) return {false, "usage: particles.control EMITTER play|pause|stop|restart|clear|destroy"};
        auto& particles = host.services().get<ParticleService>();
        const auto handle = resolve(particles, args[0]);
        bool accepted = false;
        if (args[1] == "play") accepted = particles.play(handle);
        else if (args[1] == "pause") accepted = particles.pause(handle);
        else if (args[1] == "stop") accepted = particles.stop(handle);
        else if (args[1] == "restart") accepted = particles.play(handle, true);
        else if (args[1] == "clear") accepted = particles.clear(handle);
        else if (args[1] == "destroy") accepted = particles.destroy(handle);
        else return {false, "unknown particle control"};
        return {accepted, accepted ? "emitter control applied" : std::string(describe(particles.last_error()))};
    }, "Control emission, freeze simulation, clear particles or retire owned resources");
    (void)kernel.add_command("particles.clear", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& particles = host.services().get<ParticleService>();
        if (args.empty()) {
            const auto count = particles.clear_all();
            return {particles.last_error() == ParticleError::None, "cleared=" + std::to_string(count)};
        }
        if (args.size() != 1) return {false, "usage: particles.clear [EMITTER]"};
        const bool accepted = particles.clear(resolve(particles, args[0]));
        return {accepted, accepted ? "particles cleared" : std::string(describe(particles.last_error()))};
    }, "Release active particle handles and queued bursts while retaining emitters");
    (void)kernel.add_command("particles.rate", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) return {false, "usage: particles.rate EMITTER PARTICLES_PER_SECOND"};
        const auto rate = number<double>(args[1]);
        if (!rate) return {false, "rate must be a finite number"};
        auto& particles = host.services().get<ParticleService>();
        const bool accepted = particles.set_rate(resolve(particles, args[0]), *rate);
        return {accepted, accepted ? "emission rate changed" : std::string(describe(particles.last_error()))};
    }, "Change continuous emission without clearing existing particles");
    (void)kernel.add_command("particles.force", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 5) return {false, "usage: particles.force EMITTER AX AY AZ DRAG"};
        auto& particles = host.services().get<ParticleService>();
        const auto handle = resolve(particles, args[0]);
        const auto description = particles.description(handle);
        const auto x = number<double>(args[1]);
        const auto y = number<double>(args[2]);
        const auto z = number<double>(args[3]);
        const auto drag = number<double>(args[4]);
        if (!description || !x || !y || !z || !drag) return {false, "invalid emitter or force arguments"};
        auto forces = description->forces;
        forces.acceleration = {*x, *y, *z};
        forces.drag = *drag;
        const bool accepted = particles.set_forces(handle, forces);
        return {accepted, accepted ? "forces changed" : std::string(describe(particles.last_error()))};
    }, "Set acceleration and drag in the emitter's simulation space");
    (void)kernel.add_command("particles.state", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        const auto& particles = host.services().get<ParticleService>();
        if (args.empty()) {
            const auto state = particles.statistics();
            return {true, "emitters=" + std::to_string(state.emitters) + " live=" + std::to_string(state.particles) +
                " trails=" + std::to_string(state.trail_points) + " emitted=" + std::to_string(state.emitted) +
                " dropped=" + std::to_string(state.dropped) + " quads=" + std::to_string(state.mesh_quads) +
                " origin_epoch=" + std::to_string(state.origin_epoch)};
        }
        if (args.size() != 1) return {false, "usage: particles.state [EMITTER]"};
        const auto state = particles.inspect(resolve(particles, args[0]));
        if (!state) return {false, "emitter is missing or stale"};
        const std::string playback = state->playback == ParticlePlayback::Playing ? "playing" :
            (state->playback == ParticlePlayback::Paused ? "paused" : "stopped");
        return {true, "emitter=" + handle_text(state->handle) + " source=" + handle_text(state->source) +
            " mesh=" + handle_text(state->mesh) + " state=" + playback + " live=" + std::to_string(state->live_particles) +
            " queued=" + std::to_string(state->queued_particles) + " elapsed=" + std::to_string(state->elapsed) +
            " emitted=" + std::to_string(state->emitted) + " dropped=" + std::to_string(state->dropped)};
    }, "Inspect live emitter state, mesh bindings, quotas and origin synchronization");
}

} // namespace ge::effects

