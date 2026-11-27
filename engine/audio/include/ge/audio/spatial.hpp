#pragma once

#include <ge/audio/mixer.hpp>
#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ge::core { class Kernel; }
namespace ge::scene { class Scene; }
namespace ge::render { class CameraService; }
namespace ge::physics { class CollisionWorld; class PhysicsWorld; }

namespace ge::audio {

enum class DistanceModel : std::uint8_t { None, Linear, Inverse, Exponential };
enum class VelocitySource : std::uint8_t { Transform, Physics, Explicit };

struct DistanceAttenuation {
    DistanceModel model{DistanceModel::Inverse};
    double reference_distance{1.0};
    double maximum_distance{100.0};
    double rolloff{1.0};
    double minimum_gain{};
    double maximum_gain{1.0};
    bool silence_beyond_maximum{};
};

struct EmissionCone {
    double inner_angle{math::pi * 2.0};
    double outer_angle{math::pi * 2.0};
    double outer_gain{1.0};
};

struct AcousticSettings {
    bool enabled{true};
    std::uint64_t collision_mask{~std::uint64_t{0}};
    double aperture_radius{0.2};
    double refresh_seconds{0.05};
    double attack_seconds{0.03};
    double release_seconds{0.15};
    double obstruction_gain{0.7};
    double occlusion_gain{0.25};
    std::uint32_t ray_count{5}; // Supported finite aperture samples: 1, 3, or 5.
};

// Authored values stay independent of the attenuated mixer controls. Attach
// through SpatialAudio; copying a component never duplicates a playing voice.
struct AudioEmitterComponent {
    math::Vec3 local_position{};
    math::Vec3 local_forward{0.0, 0.0, -1.0};
    math::Vec3 velocity{}; // World metres/second when Explicit is selected.
    VelocitySource velocity_source{VelocitySource::Physics};
    DistanceAttenuation distance{};
    EmissionCone cone{};
    AcousticSettings acoustics{};
    double gain{1.0};
    double pitch{1.0};
    double spatial_blend{1.0};
    double stereo_spread{};
    double doppler_scale{1.0};
    bool enabled{true};
};

struct AudioListenerComponent {
    math::Vec3 local_position{};
    math::Vec3 local_forward{0.0, 0.0, -1.0};
    math::Vec3 local_up{0.0, 1.0, 0.0};
    math::Vec3 velocity{};
    VelocitySource velocity_source{VelocitySource::Physics};
    double gain{1.0};
    std::int32_t priority{};
    bool enabled{true};
};

// Optional material on collider entities. Compound colliders on one entity
// count once per acoustic ray. These are attenuation hints, not DSP filters.
struct AcousticMaterialComponent {
    double obstruction{1.0};
    double occlusion{1.0};
    double high_frequency_absorption{0.8};
};

struct AcousticPath {
    ecs::Entity emitter{};
    ecs::Entity listener{};
    math::Vec3 origin{};
    math::Vec3 destination{};
    math::Vec3 right{1.0, 0.0, 0.0};
    math::Vec3 up{0.0, 1.0, 0.0};
    AcousticSettings settings{};
};

struct AcousticSample {
    double obstruction{};
    double occlusion{};
    double high_frequency_gain{1.0};
    bool incomplete{};
};

// Called on the serialized control thread. Return false to preserve the last
// sample. Callbacks must not mutate this service, its mixer, or borrowed worlds.
using AcousticQuery = bool (*)(const AcousticPath&, AcousticSample&, void*) noexcept;

struct SpatialListenerState {
    ecs::Entity entity{};
    math::Vec3 position{};
    math::Vec3 forward{0.0, 0.0, -1.0};
    math::Vec3 up{0.0, 1.0, 0.0};
    math::Vec3 right{1.0, 0.0, 0.0};
    math::Vec3 velocity{};
    double gain{1.0};
    bool valid{};
    bool from_camera{};
};

struct SpatialEmitterState {
    ecs::Entity entity{};
    math::Vec3 position{};
    math::Vec3 forward{0.0, 0.0, -1.0};
    math::Vec3 velocity{};
    AcousticSample acoustics{};
    double distance{};
    double distance_gain{1.0};
    double cone_gain{1.0};
    double acoustic_gain{1.0};
    double doppler{1.0};
    double pan{};
    std::size_t voices{};
    bool valid{};
};

struct SpatialVoiceState {
    ecs::Entity emitter{};
    VoiceHandle voice{};
    float base_gain{1.0F};
    float base_pitch{1.0F};
    float base_pan{};
    float applied_gain{}; // Last accepted target; the mixer owns the sample ramp.
    float applied_pitch{1.0F};
    float applied_pan{};
    double high_frequency_gain{1.0};
    bool owned{};
};

// Receives real applied controls for integration with a later per-voice DSP
// owner. The reference expires on return; callback mutation is forbidden.
using SpatialVoiceObserver = void (*)(const SpatialVoiceState&, void*) noexcept;

struct SpatialAudioConfig {
    std::size_t max_emitters{256};
    std::size_t max_listeners{16};
    std::size_t max_tracked_voices{256};
    std::size_t max_voices_per_emitter{8};
    std::size_t max_collision_candidates{128};
    std::size_t max_acoustic_queries_per_update{32};
    std::size_t max_primitive_tests_per_update{8192};
    double speed_of_sound{343.0};
    double doppler_factor{1.0};
    double minimum_doppler{0.5};
    double maximum_doppler{2.0};
    double maximum_velocity{1000.0};
    double teleport_distance{100.0};
    double maximum_delta_seconds{0.25};
    double velocity_smoothing_seconds{0.03};
    double control_smoothing_seconds{0.025};
    double gain_ramp_seconds{0.01};
    std::size_t camera_slot{};
    bool camera_listener{true};
};

struct SpatialAudioStatistics {
    std::size_t emitters{};
    std::size_t listeners{};
    std::size_t voices{};
    std::size_t audible_voices{};
    std::size_t acoustic_queries{};
    std::size_t primitive_tests{};
    std::size_t deferred_queries{};
    std::uint64_t updates{};
    std::uint64_t expired_voices{};
    std::uint64_t rejected_requests{};
    std::uint64_t invalid_components{};
    std::uint64_t incomplete_queries{};
    bool listener_available{};
};

[[nodiscard]] bool valid(const DistanceAttenuation& value) noexcept;
[[nodiscard]] bool valid(const EmissionCone& value) noexcept;
[[nodiscard]] bool valid(const AcousticSettings& value) noexcept;
[[nodiscard]] bool valid(const AudioEmitterComponent& value) noexcept;
[[nodiscard]] bool valid(const AudioListenerComponent& value) noexcept;
[[nodiscard]] bool valid(const AcousticMaterialComponent& value) noexcept;
[[nodiscard]] double distance_gain(const DistanceAttenuation& value, double metres) noexcept;
[[nodiscard]] double cone_gain(const EmissionCone& value, math::Vec3 forward,
                               math::Vec3 toward_listener) noexcept;
[[nodiscard]] double doppler_ratio(math::Vec3 listener_velocity, math::Vec3 emitter_velocity,
                                   math::Vec3 toward_emitter, double speed_of_sound,
                                   double factor, double minimum, double maximum) noexcept;

// Borrowed services must outlive use. Destruction never touches them: call
// shutdown() while they live. Owned voices stop; externally bound voices are
// restored to their explicit base values. Controls require one serialized writer.
// Update after Scene/camera synchronization and before mixer output. Missing
// listeners mute retained voices while their source timelines continue. Optional
// collision/physics services must share this Scene's registry; collision queries
// read its synchronized shapes and do not mutate contacts or consume events.
class SpatialAudio final {
public:
    explicit SpatialAudio(AudioMixer& mixer, scene::Scene& scene,
                          SpatialAudioConfig configuration = {});
    ~SpatialAudio();
    SpatialAudio(const SpatialAudio&) = delete;
    SpatialAudio& operator=(const SpatialAudio&) = delete;
    bool attach_emitter(ecs::Entity entity, AudioEmitterComponent settings = {});
    bool detach_emitter(ecs::Entity entity);
    bool attach_listener(ecs::Entity entity, AudioListenerComponent settings = {});
    bool detach_listener(ecs::Entity entity);
    bool set_emitter(ecs::Entity entity, const AudioEmitterComponent& settings);
    bool set_listener(ecs::Entity entity, const AudioListenerComponent& settings);
    bool select_listener(ecs::Entity entity = {}) noexcept;
    bool set_camera_listener(bool enabled, std::size_t slot = 0) noexcept;
    [[nodiscard]] VoiceHandle play(ecs::Entity entity, SoundHandle sound,
                                    const VoiceOptions& options = {});
    bool bind_voice(ecs::Entity entity, VoiceHandle voice, float base_gain,
                    float base_pitch = 1.0F, float base_pan = 0.0F);
    bool release_voice(VoiceHandle voice, bool stop = false) noexcept;
    bool set_voice_base(VoiceHandle voice, float gain, float pitch, float pan = 0.0F) noexcept;
    std::size_t stop(ecs::Entity entity, double fade_seconds = 0.0) noexcept;
    std::size_t pause(ecs::Entity entity, bool paused = true) noexcept;
    bool set_acoustic_query(AcousticQuery query, void* user = nullptr) noexcept;
    bool set_voice_observer(SpatialVoiceObserver observer, void* user = nullptr) noexcept;
    bool reset_motion(ecs::Entity entity = {}) noexcept;
    void update(double delta_seconds, const render::CameraService* cameras = nullptr,
                const physics::CollisionWorld* collision = nullptr,
                const physics::PhysicsWorld* physics = nullptr);
    void shutdown() noexcept;
    [[nodiscard]] std::optional<SpatialEmitterState> emitter(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::optional<SpatialVoiceState> voice(VoiceHandle handle) const noexcept;
    [[nodiscard]] std::size_t copy_emitters(std::span<SpatialEmitterState> output) const noexcept;
    [[nodiscard]] std::size_t copy_voices(std::span<SpatialVoiceState> output) const noexcept;
    [[nodiscard]] const SpatialListenerState& listener() const noexcept;
    [[nodiscard]] const SpatialAudioConfig& config() const noexcept;
    [[nodiscard]] SpatialAudioStatistics statistics() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_spatial(ge::core::Kernel& kernel);

} // namespace ge::audio
