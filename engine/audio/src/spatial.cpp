#include <ge/audio/spatial.hpp>

#include <ge/core/runtime.hpp>
#include <ge/physics/collision.hpp>
#include <ge/physics/dynamics.hpp>
#include <ge/render/camera.hpp>
#include <ge/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ge::audio {
namespace {

bool range(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

bool bounded(math::Vec3 value) noexcept {
    return math::finite(value) && std::abs(value.x) <= 1.0e12 &&
           std::abs(value.y) <= 1.0e12 && std::abs(value.z) <= 1.0e12;
}

bool direction(math::Vec3 value) noexcept {
    return bounded(value) && math::length_squared(value) > 1.0e-18;
}

bool valid_velocity_source(VelocitySource source) noexcept {
    return source == VelocitySource::Transform || source == VelocitySource::Physics ||
           source == VelocitySource::Explicit;
}

bool valid_base(float gain, float pitch, float pan) noexcept {
    return range(gain, 0.0, 16.0) && range(pitch, 0.03125, 8.0) && range(pan, -1.0, 1.0);
}

bool valid_sample(const AcousticSample& sample) noexcept {
    return range(sample.obstruction, 0.0, 1.0) && range(sample.occlusion, 0.0, 1.0) &&
           range(sample.high_frequency_gain, 0.0, 1.0);
}

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) { ++value; }
}

double smooth(double previous, double target, double seconds, double delta) noexcept {
    if (seconds <= 0.0) { return target; }
    const double amount = -std::expm1(-delta / seconds);
    return previous + (target - previous) * amount;
}

bool valid_config(const SpatialAudioConfig& value) noexcept {
    return value.max_emitters > 0 && value.max_emitters <= 65536 &&
           value.max_listeners > 0 && value.max_listeners <= 256 &&
           value.max_tracked_voices > 0 && value.max_tracked_voices <= 4096 &&
           value.max_voices_per_emitter > 0 && value.max_voices_per_emitter <= 64 &&
           value.max_collision_candidates > 0 && value.max_collision_candidates <= 8192 &&
           value.max_acoustic_queries_per_update <= 4096 &&
           value.max_primitive_tests_per_update <= 4194304 && value.camera_slot < 256 &&
           range(value.speed_of_sound, 1.0, 100000.0) && range(value.doppler_factor, 0.0, 4.0) &&
           range(value.minimum_doppler, 0.03125, 1.0) && range(value.maximum_doppler, 1.0, 8.0) &&
           range(value.maximum_velocity, 0.0, 100000.0) && range(value.teleport_distance, 0.001, 1.0e9) &&
           range(value.maximum_delta_seconds, 0.001, 1.0) &&
           range(value.velocity_smoothing_seconds, 0.0, 10.0) &&
           range(value.control_smoothing_seconds, 0.0, 10.0) && range(value.gain_ramp_seconds, 0.0, 60.0);
}

bool basis(math::Vec3 forward, math::Vec3 up, SpatialListenerState& state) noexcept {
    if (!direction(forward) || !direction(up)) { return false; }
    forward = math::normalized(forward);
    const auto right = math::cross(forward, math::normalized(up));
    if (!direction(right)) { return false; }
    state.forward = forward;
    state.right = math::normalized(right);
    state.up = math::normalized(math::cross(state.right, forward));
    return true;
}

bool inside_shape(const physics::CollisionShape& shape, math::Vec3 point) noexcept {
    return std::visit([point](const auto& primitive) {
        using T = std::decay_t<decltype(primitive)>;
        if constexpr (std::is_same_v<T, math::Sphere>) {
            return math::contains(primitive, point);
        } else if constexpr (std::is_same_v<T, math::Obb>) {
            return math::length_squared(math::closest_point(primitive, point) - point) <= 1.0e-16;
        } else if constexpr (std::is_same_v<T, math::Capsule>) {
            return math::distance_squared(math::Segment{primitive.a, primitive.b}, point) <=
                   primitive.radius * primitive.radius;
        } else if constexpr (std::is_same_v<T, math::Plane>) {
            return math::signed_distance(primitive, point) < -1.0e-6;
        } else {
            return false;
        }
    }, shape);
}

bool intersects(const physics::CollisionShape& shape, const math::Ray& ray, double maximum) noexcept {
    if (inside_shape(shape, ray.origin)) { return true; }
    return std::visit([&](const auto& primitive) {
        return math::intersect(ray, primitive, 1.0e-5, maximum).has_value();
    }, shape);
}

template<class T>
std::optional<T> number(std::string_view text) noexcept {
    if (text.empty() || text.size() > 96) { return std::nullopt; }
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) { return std::nullopt; }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) { return std::nullopt; }
    }
    return value;
}

} // namespace

bool valid(const DistanceAttenuation& value) noexcept {
    const bool model = value.model == DistanceModel::None || value.model == DistanceModel::Linear ||
                       value.model == DistanceModel::Inverse || value.model == DistanceModel::Exponential;
    return model && range(value.reference_distance, 0.0001, 1.0e9) &&
           range(value.maximum_distance, value.reference_distance, 1.0e12) &&
           range(value.rolloff, 0.0, 16.0) && range(value.minimum_gain, 0.0, 1.0) &&
           range(value.maximum_gain, value.minimum_gain, 1.0);
}

bool valid(const EmissionCone& value) noexcept {
    return range(value.inner_angle, 0.0, math::pi * 2.0) &&
           range(value.outer_angle, value.inner_angle, math::pi * 2.0) &&
           range(value.outer_gain, 0.0, 1.0);
}

bool valid(const AcousticSettings& value) noexcept {
    return range(value.aperture_radius, 0.0, 1000.0) && range(value.refresh_seconds, 0.0, 60.0) &&
           range(value.attack_seconds, 0.0, 60.0) && range(value.release_seconds, 0.0, 60.0) &&
           range(value.obstruction_gain, 0.0, 1.0) && range(value.occlusion_gain, 0.0, 1.0) &&
           (value.ray_count == 1U || value.ray_count == 3U || value.ray_count == 5U);
}

bool valid(const AudioEmitterComponent& value) noexcept {
    return bounded(value.local_position) && direction(value.local_forward) && bounded(value.velocity) &&
           valid_velocity_source(value.velocity_source) && valid(value.distance) && valid(value.cone) &&
           valid(value.acoustics) && range(value.gain, 0.0, 16.0) && range(value.pitch, 0.03125, 8.0) &&
           range(value.spatial_blend, 0.0, 1.0) && range(value.stereo_spread, 0.0, 1.0) &&
           range(value.doppler_scale, 0.0, 4.0);
}

bool valid(const AudioListenerComponent& value) noexcept {
    return bounded(value.local_position) && direction(value.local_forward) && direction(value.local_up) &&
           direction(math::cross(math::normalized(value.local_forward), math::normalized(value.local_up))) &&
           bounded(value.velocity) && valid_velocity_source(value.velocity_source) && range(value.gain, 0.0, 16.0);
}

bool valid(const AcousticMaterialComponent& value) noexcept {
    return range(value.obstruction, 0.0, 1.0) && range(value.occlusion, 0.0, 1.0) &&
           range(value.high_frequency_absorption, 0.0, 1.0);
}

double distance_gain(const DistanceAttenuation& value, double metres) noexcept {
    if (!valid(value) || !range(metres, 0.0, 1.0e13)) { return 0.0; }
    if (value.model == DistanceModel::None) { return value.maximum_gain; }
    if (value.silence_beyond_maximum && metres > value.maximum_distance) { return 0.0; }
    const double distance = std::clamp(metres, value.reference_distance, value.maximum_distance);
    double result = 1.0;
    if (value.model == DistanceModel::Linear) {
        const double width = value.maximum_distance - value.reference_distance;
        if (width > 0.0) { result -= value.rolloff * (distance - value.reference_distance) / width; }
    } else if (value.model == DistanceModel::Inverse) {
        result = value.reference_distance / (value.reference_distance + value.rolloff *
                 (distance - value.reference_distance));
    } else {
        result = std::pow(distance / value.reference_distance, -value.rolloff);
    }
    return std::clamp(result, value.minimum_gain, value.maximum_gain);
}

double cone_gain(const EmissionCone& value, math::Vec3 forward, math::Vec3 toward_listener) noexcept {
    if (!valid(value) || !direction(forward) || !bounded(toward_listener)) { return 0.0; }
    if (!direction(toward_listener) || value.inner_angle >= math::pi * 2.0) { return 1.0; }
    const double cosine = std::clamp(math::dot(math::normalized(forward),
                                             math::normalized(toward_listener)), -1.0, 1.0);
    const double angle = 2.0 * std::acos(cosine);
    if (angle <= value.inner_angle) { return 1.0; }
    if (angle >= value.outer_angle) { return value.outer_gain; }
    const double amount = (angle - value.inner_angle) / (value.outer_angle - value.inner_angle);
    return 1.0 + (value.outer_gain - 1.0) * amount;
}

double doppler_ratio(math::Vec3 listener_velocity, math::Vec3 emitter_velocity,
                     math::Vec3 toward_emitter, double speed_of_sound,
                     double factor, double minimum, double maximum) noexcept {
    if (!bounded(listener_velocity) || !bounded(emitter_velocity) || !direction(toward_emitter) ||
        !range(speed_of_sound, 1.0, 100000.0) || !range(factor, 0.0, 16.0) ||
        !range(minimum, 0.03125, 1.0) || !range(maximum, 1.0, 8.0)) { return 1.0; }
    const auto axis = math::normalized(toward_emitter);
    const double limit = speed_of_sound * 0.95;
    const double listener = std::clamp(math::dot(listener_velocity, axis) * factor, -limit, limit);
    const double emitter = std::clamp(math::dot(emitter_velocity, axis) * factor, -limit, limit);
    return std::clamp((speed_of_sound + listener) / (speed_of_sound + emitter), minimum, maximum);
}

struct SpatialAudio::Impl {
    struct Motion {
        math::Vec3 previous{};
        math::Vec3 velocity{};
        bool initialized{};
    };
    struct TrackedVoice {
        SpatialVoiceState state{};
        std::uint32_t initial_ramp{};
        std::uint64_t observed_frames{};
    };
    struct Emitter {
        ecs::Entity entity{};
        Motion motion{};
        SpatialEmitterState state{};
        AcousticSample target{};
        double until_query{};
        bool controls_ready{};
        std::vector<TrackedVoice> voices;
    };
    struct Listener {
        ecs::Entity entity{};
        Motion motion{};
        SpatialListenerState state{};
    };
    AudioMixer& mixer;
    scene::Scene& scene;
    ecs::Registry& registry;
    SpatialAudioConfig config;
    std::vector<Emitter> emitters;
    std::vector<Listener> listeners;
    std::vector<physics::ColliderHandle> candidates;
    SpatialAudioStatistics stats{};
    SpatialListenerState selected{};
    ecs::Entity preferred{};
    Motion camera_motion{};
    ecs::Entity previous_camera{};
    std::size_t query_cursor{};
    AcousticQuery query{};
    void* query_user{};
    SpatialVoiceObserver observer{};
    void* observer_user{};
    std::string_view error;
    bool busy{};
    bool stopped{};

    Impl(AudioMixer& audio, scene::Scene& world, SpatialAudioConfig settings)
        : mixer(audio), scene(world), registry(world.registry()), config(settings) {
        if (!valid_config(config)) { throw std::invalid_argument("invalid spatial audio limits or parameters"); }
        emitters.reserve(config.max_emitters);
        listeners.reserve(config.max_listeners);
        candidates.resize(config.max_collision_candidates);
    }

    bool reject(std::string_view message) noexcept {
        error = message;
        increment(stats.rejected_requests);
        return false;
    }

    bool editable() noexcept {
        return (!busy && !stopped) || reject("spatial audio is updating or shut down");
    }

    Emitter* find_emitter(ecs::Entity entity) noexcept {
        const auto found = std::find_if(emitters.begin(), emitters.end(),
                                       [entity](const Emitter& item) { return item.entity == entity; });
        return found == emitters.end() ? nullptr : &*found;
    }

    const Emitter* find_emitter(ecs::Entity entity) const noexcept {
        const auto found = std::find_if(emitters.begin(), emitters.end(),
                                       [entity](const Emitter& item) { return item.entity == entity; });
        return found == emitters.end() ? nullptr : &*found;
    }

    TrackedVoice* find_voice(VoiceHandle handle) noexcept {
        for (auto& item : emitters) {
            for (auto& voice : item.voices) {
                if (voice.state.voice == handle) { return &voice; }
            }
        }
        return nullptr;
    }

    std::size_t voice_count() const noexcept {
        std::size_t count{};
        for (const auto& item : emitters) { count += item.voices.size(); }
        return count;
    }

    bool enabled(ecs::Entity entity) const noexcept {
        if (!scene.contains(entity)) { return false; }
        std::size_t depth{};
        while (entity && depth < scene.limits().max_depth) {
            if (!registry.enabled(entity)) { return false; }
            entity = scene.parent(entity);
            ++depth;
        }
        return !entity;
    }

    std::uint32_t frames(double seconds) const noexcept {
        return static_cast<std::uint32_t>(std::ceil(seconds * static_cast<double>(mixer.config().sample_rate)));
    }

    void restore(const SpatialVoiceState& state, bool stop_voice) noexcept {
        if (stop_voice) { (void)mixer.stop(state.voice); return; }
        const auto current = mixer.voice(state.voice);
        if (!current || current->stopping) { return; }
        (void)mixer.set_gain(state.voice, state.base_gain, frames(config.gain_ramp_seconds));
        (void)mixer.set_pitch(state.voice, state.base_pitch);
        (void)mixer.set_pan(state.voice, state.base_pan);
    }

    void retire(Emitter& item) noexcept {
        for (const auto& voice : item.voices) { restore(voice.state, voice.state.owned); }
        item.voices.clear();
    }

    void prune() {
        for (auto& item : emitters) {
            if (!scene.contains(item.entity) || !registry.has<AudioEmitterComponent>(item.entity)) {
                retire(item);
                continue;
            }
            std::erase_if(item.voices, [&](const TrackedVoice& voice) {
                if (mixer.voice(voice.state.voice)) { return false; }
                increment(stats.expired_voices);
                return true;
            });
        }
        std::erase_if(emitters, [&](const Emitter& item) {
            return !scene.contains(item.entity) || !registry.has<AudioEmitterComponent>(item.entity);
        });
        std::erase_if(listeners, [&](const Listener& item) {
            return !scene.contains(item.entity) || !registry.has<AudioListenerComponent>(item.entity);
        });
    }

    math::Vec3 velocity(Motion& history, ecs::Entity entity, math::Vec3 position,
                        VelocitySource source, math::Vec3 explicit_velocity, double delta,
                        const physics::PhysicsWorld* physics) noexcept {
        const bool previous = history.initialized;
        const auto displacement = position - history.previous;
        history.previous = position;
        history.initialized = true;
        if (delta <= 1.0e-8 || delta > config.maximum_delta_seconds ||
            (previous && math::length(displacement) > config.teleport_distance)) {
            history.velocity = {};
            return {};
        }
        math::Vec3 target = previous ? displacement / delta : math::Vec3{};
        if (source == VelocitySource::Explicit) {
            target = explicit_velocity;
        } else if (source == VelocitySource::Physics && physics != nullptr) {
            if (const auto* body = physics->get(entity); body && body->enabled && body->scene_enabled) {
                target = ge::physics::point_velocity(*body, position);
            }
        }
        target = bounded(target) ? math::clamp_length(target, config.maximum_velocity) : math::Vec3{};
        const double amount = previous ? smooth(0.0, 1.0, config.velocity_smoothing_seconds, delta) : 1.0;
        history.velocity = history.velocity + (target - history.velocity) * amount;
        return history.velocity;
    }

    void select(double delta, const render::CameraService* cameras, const physics::PhysicsWorld* physics) {
        SpatialListenerState next;
        std::int32_t priority = std::numeric_limits<std::int32_t>::min();
        for (auto& item : listeners) {
            item.state.valid = false;
            const auto* settings = registry.get<AudioListenerComponent>(item.entity);
            const auto* matrix = scene.cached_world_matrix(item.entity);
            if (!settings || !valid(*settings)) { increment(stats.invalid_components); item.motion = {}; continue; }
            if (!matrix || !settings->enabled || !enabled(item.entity)) { item.motion = {}; continue; }
            auto& state = item.state;
            state.entity = item.entity;
            state.position = math::transform_point(*matrix, settings->local_position);
            if (!bounded(state.position) || !basis(math::transform_vector(*matrix, settings->local_forward),
                                                  math::transform_vector(*matrix, settings->local_up), state)) {
                item.motion = {};
                continue;
            }
            state.velocity = velocity(item.motion, item.entity, state.position, settings->velocity_source,
                                      settings->velocity, delta, physics);
            state.gain = settings->gain;
            state.valid = true;
            if (next.entity == preferred && preferred) { continue; }
            if (item.entity == preferred || !next.valid || settings->priority > priority ||
                (settings->priority == priority && item.entity < next.entity)) {
                next = state;
                priority = settings->priority;
            }
        }
        if (!next.valid && config.camera_listener && cameras != nullptr) {
            const auto* view = cameras->view(config.camera_slot);
            if (view && !view->stale && bounded(view->position) && basis(view->forward, view->up, next)) {
                if (previous_camera != view->entity) { camera_motion = {}; }
                previous_camera = view->entity;
                next.entity = view->entity;
                next.position = view->position;
                next.velocity = velocity(camera_motion, view->entity, view->position,
                                          VelocitySource::Transform, {}, delta, nullptr);
                next.valid = true;
                next.from_camera = true;
            }
        }
        if (!next.from_camera) {
            camera_motion = {};
            previous_camera = {};
        }
        if (selected.entity != next.entity || selected.from_camera != next.from_camera || selected.valid != next.valid) {
            for (auto& item : emitters) { item.until_query = 0.0; item.target = {}; item.controls_ready = false; }
        }
        selected = next;
    }

    void locate_emitters(double delta, const physics::PhysicsWorld* physics) {
        for (auto& item : emitters) {
            auto& state = item.state;
            state.entity = item.entity;
            state.voices = item.voices.size();
            state.valid = false;
            const auto* settings = registry.get<AudioEmitterComponent>(item.entity);
            const auto* matrix = scene.cached_world_matrix(item.entity);
            if (!settings || !valid(*settings)) { increment(stats.invalid_components); item.motion = {}; continue; }
            if (!matrix || !settings->enabled || !enabled(item.entity)) { item.motion = {}; continue; }
            state.position = math::transform_point(*matrix, settings->local_position);
            const auto forward = math::transform_vector(*matrix, settings->local_forward);
            if (!bounded(state.position) || !direction(forward)) { item.motion = {}; continue; }
            state.forward = math::normalized(forward);
            state.velocity = velocity(item.motion, item.entity, state.position, settings->velocity_source,
                                      settings->velocity, delta, physics);
            state.valid = true;
        }
    }

    bool trace(const AcousticPath& path, const physics::CollisionWorld& collision, AcousticSample& sample) {
        auto bounds = math::include(math::Aabb{}, path.origin);
        bounds = math::include(bounds, path.destination);
        bounds = math::expanded(bounds, path.settings.aperture_radius + 0.0001);
        physics::CollisionFilter filter;
        filter.layers = ~std::uint64_t{0};
        filter.mask = path.settings.collision_mask;
        physics::CollisionQueryResult result;
        try {
            result = collision.query(bounds, candidates, filter, path.emitter);
        } catch (...) {
            return false;
        }
        if (result.invalid_input) { return false; }
        sample.incomplete = result.truncated;
        const double radius = path.settings.aperture_radius;
        const std::array<math::Vec3, 5> offsets{{{}, path.right * radius, path.right * -radius,
                                               path.up * radius, path.up * -radius}};
        double total_occlusion{};
        double total_high_frequency{};
        for (std::size_t ray_index = 0; ray_index < static_cast<std::size_t>(path.settings.ray_count); ++ray_index) {
            const auto displacement = path.destination + offsets[ray_index] - path.origin;
            const double distance = math::length(displacement);
            double obstruction = 1.0;
            double occlusion = 1.0;
            double high_frequency = 1.0;
            std::array<ecs::Entity, 16> encountered{};
            std::size_t encountered_count{};
            if (distance > 0.0001) {
                const math::Ray ray{path.origin, displacement / distance};
                for (std::size_t index = 0; index < result.written; ++index) {
                    const auto* collider = collision.get(candidates[index]);
                    if (!collider || !collider->active || collider->description.trigger ||
                        collider->entity == path.listener || collider->entity == path.emitter) { continue; }
                    bool duplicate{};
                    for (std::size_t seen = 0; seen < encountered_count; ++seen) {
                        if (encountered[seen] == collider->entity) { duplicate = true; break; }
                    }
                    if (duplicate) { continue; }
                    if (stats.primitive_tests >= config.max_primitive_tests_per_update) {
                        sample.incomplete = true;
                        obstruction = occlusion = high_frequency = 0.0;
                        break;
                    }
                    ++stats.primitive_tests;
                    if (!intersects(collider->world_shape, ray, distance - 0.00001)) { continue; }
                    if (encountered_count == encountered.size()) {
                        sample.incomplete = true;
                        obstruction = occlusion = high_frequency = 0.0;
                        break;
                    }
                    encountered[encountered_count++] = collider->entity;
                    AcousticMaterialComponent material;
                    if (const auto* authored = registry.get<AcousticMaterialComponent>(collider->entity);
                        authored && valid(*authored)) { material = *authored; }
                    obstruction *= 1.0 - material.obstruction;
                    occlusion *= 1.0 - material.occlusion;
                    high_frequency *= 1.0 - material.high_frequency_absorption;
                }
            }
            if (ray_index == 0) { sample.obstruction = 1.0 - obstruction; }
            total_occlusion += 1.0 - occlusion;
            total_high_frequency += high_frequency;
        }
        const double count = static_cast<double>(path.settings.ray_count);
        sample.occlusion = total_occlusion / count;
        sample.high_frequency_gain = total_high_frequency / count;
        if (result.truncated) {
            sample.obstruction = sample.occlusion = 1.0;
            sample.high_frequency_gain = 0.0;
        }
        return true;
    }

    void acoustics(double delta, const physics::CollisionWorld* collision) {
        if (emitters.empty()) { query_cursor = 0; return; }
        query_cursor %= emitters.size();
        std::size_t next_cursor = (query_cursor + 1) % emitters.size();
        bool budget_exhausted{};
        for (std::size_t offset = 0; offset < emitters.size(); ++offset) {
            const std::size_t index = (query_cursor + offset) % emitters.size();
            auto& item = emitters[index];
            const auto* settings = registry.get<AudioEmitterComponent>(item.entity);
            if (!settings || !item.state.valid || !selected.valid || item.voices.empty()) { continue; }
            item.until_query = std::max(0.0, item.until_query - delta);
            if (!settings->acoustics.enabled || settings->spatial_blend == 0.0 || (!query && !collision)) {
                item.target = {};
                continue;
            }
            if (item.until_query > 0.0) { continue; }
            if (stats.acoustic_queries >= config.max_acoustic_queries_per_update ||
                (!query && stats.primitive_tests >= config.max_primitive_tests_per_update)) {
                ++stats.deferred_queries;
                if (!budget_exhausted) { next_cursor = index; budget_exhausted = true; }
                continue;
            }
            const AcousticPath path{item.entity, selected.entity, selected.position, item.state.position,
                                    selected.right, selected.up, settings->acoustics};
            AcousticSample sample;
            ++stats.acoustic_queries;
            const bool sampled = query ? query(path, sample, query_user) : trace(path, *collision, sample);
            if (sampled && valid_sample(sample)) {
                item.target = sample;
                if (sample.incomplete) { increment(stats.incomplete_queries); }
            } else {
                increment(stats.incomplete_queries);
            }
            item.until_query = settings->acoustics.refresh_seconds;
        }
        query_cursor = next_cursor;
    }

    void apply(Emitter& item, double delta) noexcept {
        auto& state = item.state;
        const auto* settings = registry.get<AudioEmitterComponent>(item.entity);
        const bool audible = state.valid && selected.valid && settings != nullptr;
        double attenuation{};
        if (audible) {
            const auto relative = state.position - selected.position;
            state.distance = math::length(relative);
            state.distance_gain = distance_gain(settings->distance, state.distance);
            state.cone_gain = cone_gain(settings->cone, state.forward, -relative);
            auto& sample = state.acoustics;
            const auto& acoustic = settings->acoustics;
            const auto blend = [&](double old, double target, bool rising_is_attack) {
                const bool attack = rising_is_attack ? target > old : target < old;
                return smooth(old, target, attack ? acoustic.attack_seconds : acoustic.release_seconds, delta);
            };
            sample.obstruction = blend(sample.obstruction, item.target.obstruction, true);
            sample.occlusion = blend(sample.occlusion, item.target.occlusion, true);
            sample.high_frequency_gain = blend(sample.high_frequency_gain, item.target.high_frequency_gain, false);
            sample.incomplete = item.target.incomplete;
            state.acoustic_gain = (1.0 - sample.obstruction * (1.0 - acoustic.obstruction_gain)) *
                                  (1.0 - sample.occlusion * (1.0 - acoustic.occlusion_gain));
            const double denominator = std::max(state.distance, settings->distance.reference_distance);
            const double pan = std::clamp(math::dot(relative, selected.right) / denominator, -1.0, 1.0) *
                               (1.0 - settings->stereo_spread);
            const double doppler = doppler_ratio(selected.velocity, state.velocity, relative, config.speed_of_sound,
                                                 config.doppler_factor * settings->doppler_scale,
                                                 config.minimum_doppler, config.maximum_doppler);
            state.pan = item.controls_ready ? smooth(state.pan, pan, config.control_smoothing_seconds, delta) : pan;
            state.doppler = item.controls_ready ? smooth(state.doppler, doppler, config.control_smoothing_seconds, delta) : doppler;
            attenuation = 1.0 + (state.distance_gain * state.cone_gain * state.acoustic_gain - 1.0) * settings->spatial_blend;
            attenuation *= settings->gain * selected.gain;
            item.controls_ready = true;
        } else {
            item.controls_ready = false;
        }
        for (auto& tracked : item.voices) {
            auto& voice = tracked.state;
            const auto current = mixer.voice(voice.voice);
            if (!current || current->stopping) { continue; }
            const auto rendered = mixer.statistics().rendered_frames;
            const auto elapsed = rendered >= tracked.observed_frames ? rendered - tracked.observed_frames : rendered;
            tracked.observed_frames = rendered;
            if (current->state == VoiceState::Playing) {
                tracked.initial_ramp -= static_cast<std::uint32_t>(std::min<std::uint64_t>(tracked.initial_ramp, elapsed));
            }
            const float gain = static_cast<float>(std::clamp(static_cast<double>(voice.base_gain) * attenuation, 0.0, 16.0));
            const double doppler = audible ? 1.0 + (state.doppler - 1.0) * settings->spatial_blend : 1.0;
            const double pitch_scale = audible ? settings->pitch * doppler : 1.0;
            const float pitch = static_cast<float>(std::clamp(static_cast<double>(voice.base_pitch) * pitch_scale, 0.03125, 8.0));
            const double pan = audible ? voice.base_pan + (state.pan - voice.base_pan) * settings->spatial_blend : voice.base_pan;
            const float balance = static_cast<float>(std::clamp(pan, -1.0, 1.0));
            const auto ramp = std::max(frames(config.gain_ramp_seconds), tracked.initial_ramp);
            if (gain != voice.applied_gain && mixer.set_gain(voice.voice, gain, ramp)) {
                voice.applied_gain = gain;
            }
            if (mixer.set_pitch(voice.voice, pitch)) { voice.applied_pitch = pitch; }
            if (mixer.set_pan(voice.voice, balance)) { voice.applied_pan = balance; }
            if (state.valid) { (void)mixer.set_position(voice.voice, state.position); }
            voice.high_frequency_gain = audible ? 1.0 + (state.acoustics.high_frequency_gain - 1.0) * settings->spatial_blend : 1.0;
            if (gain > 0.00001F && current->state == VoiceState::Playing) { ++stats.audible_voices; }
            if (observer) { observer(voice, observer_user); }
        }
    }
};

SpatialAudio::SpatialAudio(AudioMixer& mixer, scene::Scene& scene, SpatialAudioConfig configuration)
    : impl_(std::make_unique<Impl>(mixer, scene, configuration)) {}

SpatialAudio::~SpatialAudio() = default;

bool SpatialAudio::attach_emitter(ecs::Entity entity, AudioEmitterComponent settings) {
    auto& owner = *impl_;
    if (!owner.editable()) { return false; }
    if (!valid(settings) || !owner.scene.contains(entity) || owner.registry.iterating()) {
        return owner.reject("invalid emitter, missing scene entity, or registry iteration");
    }
    if (owner.find_emitter(entity)) { return set_emitter(entity, settings); }
    if (owner.emitters.size() >= owner.config.max_emitters) { return owner.reject("emitter capacity exhausted"); }
    Impl::Emitter item;
    item.entity = entity;
    item.state.entity = entity;
    item.voices.reserve(owner.config.max_voices_per_emitter);
    owner.registry.emplace_or_replace<AudioEmitterComponent>(entity, settings);
    owner.emitters.push_back(std::move(item));
    owner.error = {};
    return true;
}

bool SpatialAudio::detach_emitter(ecs::Entity entity) {
    auto& owner = *impl_;
    if (!owner.editable() || owner.registry.iterating()) { return false; }
    auto* item = owner.find_emitter(entity);
    if (!item) { return owner.reject("emitter is not attached"); }
    (void)owner.registry.remove<AudioEmitterComponent>(entity);
    owner.retire(*item);
    std::erase_if(owner.emitters, [entity](const Impl::Emitter& value) { return value.entity == entity; });
    return true;
}

bool SpatialAudio::attach_listener(ecs::Entity entity, AudioListenerComponent settings) {
    auto& owner = *impl_;
    if (!owner.editable()) { return false; }
    if (!valid(settings) || !owner.scene.contains(entity) || owner.registry.iterating()) {
        return owner.reject("invalid listener, missing scene entity, or registry iteration");
    }
    for (const auto& item : owner.listeners) {
        if (item.entity == entity) { return set_listener(entity, settings); }
    }
    if (owner.listeners.size() >= owner.config.max_listeners) { return owner.reject("listener capacity exhausted"); }
    owner.registry.emplace_or_replace<AudioListenerComponent>(entity, settings);
    Impl::Listener item;
    item.entity = entity;
    owner.listeners.push_back(item);
    owner.error = {};
    return true;
}

bool SpatialAudio::detach_listener(ecs::Entity entity) {
    auto& owner = *impl_;
    if (!owner.editable() || owner.registry.iterating()) { return false; }
    const auto removed = std::erase_if(owner.listeners, [entity](const Impl::Listener& item) { return item.entity == entity; });
    if (removed == 0) { return owner.reject("listener is not attached"); }
    (void)owner.registry.remove<AudioListenerComponent>(entity);
    if (owner.preferred == entity) { owner.preferred = {}; }
    if (owner.selected.entity == entity) { owner.selected = {}; }
    return true;
}

bool SpatialAudio::set_emitter(ecs::Entity entity, const AudioEmitterComponent& settings) {
    auto& owner = *impl_;
    if (!owner.editable()) { return false; }
    auto* item = owner.find_emitter(entity);
    auto* current = owner.registry.get<AudioEmitterComponent>(entity);
    if (!item || !current || !valid(settings)) { return owner.reject("invalid emitter settings or ownership"); }
    if (current->local_position != settings.local_position || current->velocity_source != settings.velocity_source) {
        item->motion = {};
    }
    *current = settings;
    item->until_query = 0.0;
    (void)owner.registry.mark_changed<AudioEmitterComponent>(entity);
    owner.error = {};
    return true;
}

bool SpatialAudio::set_listener(ecs::Entity entity, const AudioListenerComponent& settings) {
    auto& owner = *impl_;
    if (!owner.editable()) { return false; }
    for (auto& item : owner.listeners) {
        if (item.entity != entity) { continue; }
        auto* current = owner.registry.get<AudioListenerComponent>(entity);
        if (!current || !valid(settings)) { return owner.reject("invalid listener settings"); }
        if (current->local_position != settings.local_position || current->velocity_source != settings.velocity_source) {
            item.motion = {};
        }
        *current = settings;
        (void)owner.registry.mark_changed<AudioListenerComponent>(entity);
        owner.error = {};
        return true;
    }
    return owner.reject("listener is not attached");
}

bool SpatialAudio::select_listener(ecs::Entity entity) noexcept {
    if (!impl_->editable()) { return false; }
    if (entity && std::none_of(impl_->listeners.begin(), impl_->listeners.end(),
                              [entity](const Impl::Listener& item) { return item.entity == entity; })) {
        return impl_->reject("preferred listener is not attached");
    }
    impl_->preferred = entity;
    return true;
}

bool SpatialAudio::set_camera_listener(bool enabled, std::size_t slot) noexcept {
    if (!impl_->editable()) { return false; }
    if (slot >= 256) { return impl_->reject("camera view slot exceeds spatial limit"); }
    impl_->config.camera_listener = enabled;
    impl_->config.camera_slot = slot;
    impl_->camera_motion = {};
    impl_->previous_camera = {};
    return true;
}

VoiceHandle SpatialAudio::play(ecs::Entity entity, SoundHandle sound, const VoiceOptions& options) {
    auto& owner = *impl_;
    if (!owner.editable()) { return {}; }
    auto* item = owner.find_emitter(entity);
    if (!item || !owner.scene.contains(entity) || !owner.registry.has<AudioEmitterComponent>(entity) ||
        !valid_base(options.gain, options.pitch, options.pan) ||
        item->voices.size() >= owner.config.max_voices_per_emitter ||
        owner.voice_count() >= owner.config.max_tracked_voices) {
        (void)owner.reject("invalid emitter or voice parameters, or spatial voice capacity exhausted");
        return {};
    }
    VoiceOptions admitted = options;
    admitted.gain = 0.0F;
    admitted.position = item->state.position;
    const auto handle = owner.mixer.play(sound, admitted);
    if (!handle) { (void)owner.reject("mixer rejected spatial voice"); return {}; }
    Impl::TrackedVoice voice;
    voice.state.emitter = entity;
    voice.state.voice = handle;
    voice.state.base_gain = options.gain;
    voice.state.base_pitch = voice.state.applied_pitch = options.pitch;
    voice.state.base_pan = voice.state.applied_pan = options.pan;
    voice.state.owned = true;
    voice.initial_ramp = options.fade_in_frames;
    voice.observed_frames = owner.mixer.statistics().rendered_frames;
    item->voices.push_back(voice);
    item->until_query = 0.0;
    owner.error = {};
    return handle;
}

bool SpatialAudio::bind_voice(ecs::Entity entity, VoiceHandle handle, float base_gain, float base_pitch, float base_pan) {
    auto& owner = *impl_;
    if (!owner.editable()) { return false; }
    auto* item = owner.find_emitter(entity);
    const auto current = owner.mixer.voice(handle);
    if (!item || !owner.scene.contains(entity) || !owner.registry.has<AudioEmitterComponent>(entity) ||
        !current || current->stopping || owner.find_voice(handle) || !valid_base(base_gain, base_pitch, base_pan) ||
        item->voices.size() >= owner.config.max_voices_per_emitter || owner.voice_count() >= owner.config.max_tracked_voices) {
        return owner.reject("invalid, already bound, stopping, or over-budget voice");
    }
    if (!owner.mixer.set_gain(handle, 0.0F)) { return owner.reject("mixer refused voice binding"); }
    Impl::TrackedVoice voice;
    voice.state.emitter = entity;
    voice.state.voice = handle;
    voice.state.base_gain = base_gain;
    voice.state.base_pitch = base_pitch;
    voice.state.base_pan = base_pan;
    voice.state.applied_pitch = current->pitch;
    voice.state.applied_pan = current->pan;
    item->voices.push_back(voice);
    item->until_query = 0.0;
    owner.error = {};
    return true;
}

bool SpatialAudio::release_voice(VoiceHandle handle, bool stop_voice) noexcept {
    auto& owner = *impl_;
    if (!owner.editable()) { return false; }
    for (auto& item : owner.emitters) {
        for (std::size_t index = 0; index < item.voices.size(); ++index) {
            if (item.voices[index].state.voice != handle) { continue; }
            owner.restore(item.voices[index].state, stop_voice);
            item.voices.erase(item.voices.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
    }
    return owner.reject("voice is not spatially controlled");
}

bool SpatialAudio::set_voice_base(VoiceHandle handle, float gain, float pitch, float pan) noexcept {
    if (!impl_->editable()) { return false; }
    auto* voice = impl_->find_voice(handle);
    if (!voice || !valid_base(gain, pitch, pan)) { return impl_->reject("invalid authored voice controls"); }
    voice->state.base_gain = gain;
    voice->state.base_pitch = pitch;
    voice->state.base_pan = pan;
    return true;
}

std::size_t SpatialAudio::stop(ecs::Entity entity, double fade_seconds) noexcept {
    if (!impl_->editable()) { return 0; }
    auto* item = impl_->find_emitter(entity);
    if (!item || !range(fade_seconds, 0.0, 60.0)) { (void)impl_->reject("invalid emitter or fade duration"); return 0; }
    std::size_t count{};
    for (const auto& voice : item->voices) {
        if (impl_->mixer.stop(voice.state.voice, impl_->frames(fade_seconds))) { ++count; }
    }
    return count;
}

std::size_t SpatialAudio::pause(ecs::Entity entity, bool paused) noexcept {
    if (!impl_->editable()) { return 0; }
    auto* item = impl_->find_emitter(entity);
    if (!item) { (void)impl_->reject("emitter is not attached"); return 0; }
    std::size_t count{};
    for (const auto& voice : item->voices) {
        if (impl_->mixer.pause(voice.state.voice, paused)) { ++count; }
    }
    return count;
}

bool SpatialAudio::set_acoustic_query(AcousticQuery query, void* user) noexcept {
    if (!impl_->editable()) { return false; }
    impl_->query = query;
    impl_->query_user = user;
    for (auto& item : impl_->emitters) { item.until_query = 0.0; }
    return true;
}

bool SpatialAudio::set_voice_observer(SpatialVoiceObserver observer, void* user) noexcept {
    if (!impl_->editable()) { return false; }
    impl_->observer = observer;
    impl_->observer_user = user;
    return true;
}

bool SpatialAudio::reset_motion(ecs::Entity entity) noexcept {
    if (!impl_->editable()) { return false; }
    bool changed{};
    for (auto& item : impl_->emitters) {
        if (!entity || item.entity == entity) { item.motion = {}; item.until_query = 0.0; changed = true; }
    }
    for (auto& item : impl_->listeners) {
        if (!entity || item.entity == entity) { item.motion = {}; changed = true; }
    }
    if (!entity || entity == impl_->previous_camera) { impl_->camera_motion = {}; changed = true; }
    return changed;
}

void SpatialAudio::update(double delta_seconds, const render::CameraService* cameras,
                          const physics::CollisionWorld* collision, const physics::PhysicsWorld* physics) {
    auto& owner = *impl_;
    if (!owner.editable()) { return; }
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) { (void)owner.reject("invalid spatial delta"); return; }
    struct UpdateScope {
        bool& flag;
        explicit UpdateScope(bool& value) : flag(value) { flag = true; }
        ~UpdateScope() { flag = false; }
    } scope(owner.busy);
    owner.stats.acoustic_queries = owner.stats.primitive_tests = owner.stats.deferred_queries = 0;
    owner.stats.audible_voices = 0;
    owner.prune();
    owner.select(delta_seconds, cameras, physics);
    owner.locate_emitters(delta_seconds, physics);
    const double admitted_delta = std::min(delta_seconds, owner.config.maximum_delta_seconds);
    owner.acoustics(admitted_delta, collision);
    for (auto& item : owner.emitters) { owner.apply(item, admitted_delta); }
    owner.stats.listener_available = owner.selected.valid;
    increment(owner.stats.updates);
    owner.error = {};
}

void SpatialAudio::shutdown() noexcept {
    if (impl_->busy || impl_->stopped) { return; }
    for (auto& item : impl_->emitters) { impl_->retire(item); }
    impl_->emitters.clear();
    impl_->listeners.clear();
    impl_->selected = {};
    impl_->query = nullptr;
    impl_->observer = nullptr;
    impl_->stopped = true;
}

std::optional<SpatialEmitterState> SpatialAudio::emitter(ecs::Entity entity) const noexcept {
    const auto* item = impl_->find_emitter(entity);
    if (!item) { return std::nullopt; }
    auto result = item->state;
    result.voices = item->voices.size();
    return result;
}

std::optional<SpatialVoiceState> SpatialAudio::voice(VoiceHandle handle) const noexcept {
    const auto* item = impl_->find_voice(handle);
    return item ? std::optional{item->state} : std::nullopt;
}

std::size_t SpatialAudio::copy_emitters(std::span<SpatialEmitterState> output) const noexcept {
    const auto count = std::min(output.size(), impl_->emitters.size());
    for (std::size_t index = 0; index < count; ++index) {
        output[index] = impl_->emitters[index].state;
        output[index].voices = impl_->emitters[index].voices.size();
    }
    return count;
}

std::size_t SpatialAudio::copy_voices(std::span<SpatialVoiceState> output) const noexcept {
    std::size_t count{};
    for (const auto& item : impl_->emitters) {
        for (const auto& voice : item.voices) {
            if (count == output.size()) { return count; }
            output[count++] = voice.state;
        }
    }
    return count;
}

const SpatialListenerState& SpatialAudio::listener() const noexcept { return impl_->selected; }
const SpatialAudioConfig& SpatialAudio::config() const noexcept { return impl_->config; }
std::string_view SpatialAudio::last_error() const noexcept { return impl_->error; }

SpatialAudioStatistics SpatialAudio::statistics() const noexcept {
    auto result = impl_->stats;
    result.emitters = impl_->emitters.size();
    result.listeners = impl_->listeners.size();
    result.voices = impl_->voice_count();
    result.audible_voices = std::min(result.audible_voices, result.voices);
    result.listener_available = impl_->selected.valid;
    return result;
}

void register_spatial(core::Kernel& kernel) {
    auto* mixer = kernel.services().find<AudioMixer>();
    auto* world = kernel.services().find<scene::Scene>();
    if (!mixer || !world) {
        kernel.diagnose(core::Diagnostic::Severity::Error, "audio.spatial", "Scene and AudioMixer are required");
        return;
    }
    if (!kernel.register_module({"audio.spatial", "Entity spatial voices, listeners and bounded acoustic attenuation", 1})) { return; }
    SpatialAudioConfig config;
    config.max_emitters = static_cast<std::size_t>(std::clamp(kernel.config().integer("audio.spatial.emitters", 256),
                                                            std::int64_t{1}, std::int64_t{65536}));
    config.max_tracked_voices = std::min(config.max_tracked_voices, mixer->config().max_voices);
    config.camera_listener = kernel.config().boolean("audio.spatial.camera_listener", true);
    const double sound_speed = kernel.config().number("audio.spatial.speed_of_sound", 343.0);
    if (range(sound_speed, 1.0, 100000.0)) { config.speed_of_sound = sound_speed; }
    auto& service = kernel.services().emplace<SpatialAudio>(*mixer, *world, config);
    (void)world->registry().register_component<AudioEmitterComponent>("audio.emitter");
    (void)world->registry().register_component<AudioListenerComponent>("audio.listener");
    (void)world->registry().register_component<AcousticMaterialComponent>("audio.acoustic_material");
    (void)kernel.add_system({"audio.spatial", core::Phase::Audio, 0, {"scene.transforms.post"}},
        [](core::Kernel& host, const core::Frame& frame) {
            auto& spatial = host.services().get<SpatialAudio>();
            spatial.update(frame.delta_seconds, host.services().find<render::CameraService>(),
                           host.services().find<physics::CollisionWorld>(), host.services().find<physics::PhysicsWorld>());
            const auto stats = spatial.statistics();
            host.metric("audio.spatial.voices", static_cast<double>(stats.voices));
            host.metric("audio.spatial.audible", static_cast<double>(stats.audible_voices));
            host.metric("audio.spatial.queries", static_cast<double>(stats.acoustic_queries));
            host.metric("audio.spatial.primitive_tests", static_cast<double>(stats.primitive_tests));
            host.metric("audio.spatial.deferred", static_cast<double>(stats.deferred_queries));
            host.metric("audio.spatial.listener", stats.listener_available ? 1.0 : 0.0);
        });
    (void)kernel.order_after("audio.mixer", "audio.spatial");
    (void)kernel.add_shutdown("audio.spatial", [owner = &service] { owner->shutdown(); });
    (void)kernel.add_command("audio.emitter", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 2) { return {false, "usage: audio.emitter attach|detach entity"}; }
        const auto entity = ecs::parse_entity(args[1]);
        if (!entity) { return {false, "invalid entity token"}; }
        auto& spatial = host.services().get<SpatialAudio>();
        const bool result = args[0] == "attach" ? spatial.attach_emitter(*entity) :
                            args[0] == "detach" && spatial.detach_emitter(*entity);
        return {result, result ? "emitter updated" : std::string(spatial.last_error())};
    }, "Attach a spatial emitter to an existing scene entity or release its voices");
    (void)kernel.add_command("audio.listener", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        auto& spatial = host.services().get<SpatialAudio>();
        if (args.size() == 1 && args[0] == "auto") { return {spatial.select_listener(), "automatic listener priority enabled"}; }
        if (args.size() != 2) { return {false, "usage: audio.listener attach|detach|select entity; auto; camera slot|off"}; }
        if (args[0] == "camera") {
            if (args[1] == "off") { return {spatial.set_camera_listener(false), "camera fallback disabled"}; }
            const auto slot = number<std::size_t>(args[1]);
            return {slot && spatial.set_camera_listener(true, *slot), "camera fallback selection"};
        }
        const auto entity = ecs::parse_entity(args[1]);
        if (!entity) { return {false, "invalid entity token"}; }
        bool result{};
        if (args[0] == "attach") { result = spatial.attach_listener(*entity); }
        else if (args[0] == "detach") { result = spatial.detach_listener(*entity); }
        else if (args[0] == "select") { result = spatial.select_listener(*entity); }
        return {result, result ? "listener updated" : std::string(spatial.last_error())};
    }, "Select attached listeners by entity, automatic priority, or camera fallback");
    (void)kernel.add_command("audio.spatial.play", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() < 2 || args.size() > 5) { return {false, "usage: audio.spatial.play entity sound [gain] [pitch] [loop]"}; }
        const auto entity = ecs::parse_entity(args[0]);
        auto& audio = host.services().get<AudioMixer>();
        const auto sound = audio.sounds().find(args[1]);
        const auto gain = args.size() > 2 ? number<float>(args[2]) : std::optional{1.0F};
        const auto pitch = args.size() > 3 ? number<float>(args[3]) : std::optional{1.0F};
        if (!entity || !sound || !gain || !pitch) { return {false, "invalid entity, sound, gain, or pitch"}; }
        VoiceOptions options;
        options.gain = *gain;
        options.pitch = *pitch;
        if (args.size() == 5) {
            if (args[4] != "loop") { return {false, "final argument must be loop"}; }
            const auto buffer = audio.sounds().acquire(sound);
            if (!buffer) { return {false, "sound no longer available"}; }
            options.loop = LoopRegion{0, static_cast<std::uint64_t>(buffer->frame_count()), infinite_repeats};
        }
        auto& spatial = host.services().get<SpatialAudio>();
        const auto voice = spatial.play(*entity, sound, options);
        return {static_cast<bool>(voice), voice ? std::to_string(voice.slot) + ":" + std::to_string(voice.generation) :
                                               std::string(spatial.last_error())};
    }, "Play an existing mixer sound through an attached emitter");
    (void)kernel.add_command("audio.spatial.stop", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.empty() || args.size() > 2) { return {false, "usage: audio.spatial.stop entity [fade_seconds]"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const auto fade = args.size() == 2 ? number<double>(args[1]) : std::optional{0.0};
        if (!entity || !fade || !range(*fade, 0.0, 60.0)) { return {false, "invalid entity or fade"}; }
        auto& spatial = host.services().get<SpatialAudio>();
        if (!spatial.emitter(*entity)) { return {false, "emitter is not attached"}; }
        return {true, std::to_string(spatial.stop(*entity, *fade)) + " voices stopping"};
    }, "Fade or stop only the voices retained by one emitter");
    (void)kernel.add_command("audio.spatial.distance", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (args.size() != 5) { return {false, "usage: audio.spatial.distance entity none|linear|inverse|exponential reference maximum rolloff"}; }
        const auto entity = ecs::parse_entity(args[0]);
        const auto reference = number<double>(args[2]);
        const auto maximum = number<double>(args[3]);
        const auto rolloff = number<double>(args[4]);
        if (!entity || !reference || !maximum || !rolloff) { return {false, "invalid distance arguments"}; }
        auto& scene_service = host.services().get<scene::Scene>();
        const auto* component = scene_service.registry().get<AudioEmitterComponent>(*entity);
        if (!component) { return {false, "emitter component missing"}; }
        auto settings = *component;
        if (args[1] == "none") { settings.distance.model = DistanceModel::None; }
        else if (args[1] == "linear") { settings.distance.model = DistanceModel::Linear; }
        else if (args[1] == "inverse") { settings.distance.model = DistanceModel::Inverse; }
        else if (args[1] == "exponential") { settings.distance.model = DistanceModel::Exponential; }
        else { return {false, "unknown distance model"}; }
        settings.distance.reference_distance = *reference;
        settings.distance.maximum_distance = *maximum;
        settings.distance.rolloff = *rolloff;
        auto& spatial = host.services().get<SpatialAudio>();
        const bool changed = spatial.set_emitter(*entity, settings);
        return {changed, changed ? "distance attenuation updated" : std::string(spatial.last_error())};
    }, "Edit finite distance attenuation parameters without changing authored voice gain");
    (void)kernel.add_command("audio.spatial.status", [](core::Kernel& host, core::CommandArgs args) -> core::CommandResult {
        if (!args.empty()) { return {false, "usage: audio.spatial.status"}; }
        const auto& spatial = host.services().get<SpatialAudio>();
        const auto stats = spatial.statistics();
        return {true, "emitters=" + std::to_string(stats.emitters) + " voices=" + std::to_string(stats.voices) +
                      " audible=" + std::to_string(stats.audible_voices) + " listener=" + ecs::format_entity(spatial.listener().entity) +
                      " queries=" + std::to_string(stats.acoustic_queries) + " deferred=" + std::to_string(stats.deferred_queries)};
    }, "Inspect retained voices, selected listener, and acoustic work budgets");
}

} // namespace ge::audio
