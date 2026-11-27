#pragma once

#include <ge/physics/dynamics.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::core { class Kernel; }
namespace ge::input { class InputSystem; }
namespace ge::render { class CameraService; }

namespace ge::physics {

enum class CharacterAuthority : std::uint8_t { Manual, Player, Navigation };
enum class CharacterMotion : std::uint8_t { Disabled, Grounded, Rising, Falling, Sliding };
enum class CharacterError : std::uint8_t {
    None,
    InvalidEntity,
    InvalidSettings,
    InvalidInput,
    AlreadyOwned,
    Capacity,
    MissingCharacter,
    StaleOwnership,
    Busy,
    BodyRejected,
    ColliderRejected,
    QueryBudget,
    QueryOverflow,
    UnsupportedGeometry,
    ClearanceBlocked,
    CameraRejected,
    WrongAuthority
};

[[nodiscard]] std::string_view describe(CharacterError error) noexcept;
[[nodiscard]] std::string_view describe(CharacterMotion motion) noexcept;

struct CharacterResult {
    CharacterError error{CharacterError::None};
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == CharacterError::None;
    }
};

// Metres/seconds, +Y up, -Z forward. The body origin is the feet; a unit world
// scale is required. Inertia uses a containing box approximation, with COM at
// half the current capsule height. CharacterSystem owns both physical handles.
struct CharacterController {
    double radius{0.3};
    double standing_height{1.8};
    double crouching_height{1.1};
    double mass{80.0};
    double walk_speed{4.0};
    double sprint_speed{7.0};
    double crouch_speed{2.0};
    double ground_acceleration{32.0};
    double ground_deceleration{40.0};
    double air_acceleration{8.0};
    double air_deceleration{1.0};
    double jump_speed{5.5};
    double released_jump_multiplier{0.45};
    double jump_buffer_seconds{0.12};
    double coyote_seconds{0.1};
    double jump_support_delay{0.1};
    double gravity_scale{1.0};
    double falling_gravity_multiplier{1.5};
    double terminal_speed{45.0};
    double maximum_slope{math::radians(50.0)};
    double ground_probe{0.04};
    double ground_snap{0.12};
    double ground_stick_speed{0.5};
    double step_height{0.3};
    double maximum_assist_distance{0.3};
    double maximum_depenetration{0.1};
    double skin{0.003};
    double turn_speed{math::radians(720.0)};
    bool face_movement{true};
    bool inherit_support_velocity{true};
    CollisionFilter filter{};
};

[[nodiscard]] bool valid_character(const CharacterController& controller) noexcept;
[[nodiscard]] math::Vec3 character_direction(math::Vec2 movement,
                                             math::Vec3 camera_forward) noexcept;

struct CharacterInput {
    math::Vec2 movement{};
    math::Vec3 forward{0.0, 0.0, -1.0};
    bool jump_pressed{};
    bool jump_held{};
    bool crouch{};
    bool sprint{};
};

struct CharacterSupport {
    ecs::Entity entity{};
    ColliderHandle collider{};
    math::Vec3 normal{0.0, 1.0, 0.0};
    math::Vec3 point{};
    math::Vec3 velocity{};
    double distance{};
    double penetration{};
    bool valid{};
};

// Snapshot for gameplay and animation; writes belong to CharacterSystem.
struct CharacterState {
    ecs::Entity entity{};
    CharacterMotion motion{CharacterMotion::Falling};
    CharacterAuthority authority{CharacterAuthority::Manual};
    CharacterError last_error{CharacterError::None};
    CharacterSupport support{};
    math::Vec3 position{};
    math::Vec3 velocity{};
    math::Vec3 desired_velocity{};
    math::Vec3 facing{0.0, 0.0, -1.0};
    double height{1.8};
    double horizontal_speed{};
    double vertical_speed{};
    double airborne_seconds{};
    double coyote_remaining{};
    double jump_buffer_remaining{};
    double landing_speed{};
    double distance_travelled{};
    std::uint64_t tick{};
    bool enabled{true};
    bool grounded{};
    bool crouched{};
    bool ceiling_blocked{};
    bool movement_blocked{};
    bool query_incomplete{};
};

struct CharacterComponent {
    BodyHandle body{};
    ColliderHandle collider{};
    CharacterController controller{};
    CharacterState state{};
};

enum class CharacterEventKind : std::uint8_t {
    Attached, Detached, Landed, LeftGround, Jumped, Crouched, Stood,
    StandBlocked, Stepped, Depenetrated, Disabled, Enabled, OwnershipLost
};

struct CharacterEvent {
    CharacterEventKind kind{CharacterEventKind::Attached};
    ecs::Entity entity{};
    ecs::Entity support{};
    math::Vec3 position{};
    math::Vec3 normal{0.0, 1.0, 0.0};
    double value{};
    std::uint64_t tick{};
};

struct CharacterLimits {
    std::size_t characters{1024};
    std::size_t events{4096};
    std::size_t query_candidates{256};
    std::size_t contacts{16384};
    std::size_t queries_per_frame{16384};
    std::uint32_t assist_samples{12};
    std::uint32_t depenetration_iterations{3};
};

struct CharacterStatistics {
    std::size_t characters{};
    std::size_t grounded{};
    std::size_t crouched{};
    std::size_t disabled{};
    std::size_t queries{};
    std::size_t incomplete_queries{};
    std::size_t removed_stale{};
    std::size_t dropped_events{};
    std::size_t rejected_writes{};
    double simulated_delta{};
    bool invalid_delta{};
    bool contacts_truncated{};
};

// Single writer borrowing PhysicsWorld and its Registry/CollisionWorld. Attach
// requires a Scene entity without an existing body or collider. Controller data
// copied by Registry cloning never transfers ownership. Assistance uses bounded
// overlap samples, not continuous sweeps; unsupported primitive pairs fail closed.
// Run control before PhysicsWorld::step and sample afterward for each frame.
// Call shutdown before borrowed services die. The destructor performs no borrowed
// service access; the dynamics callback holds only weak ownership of this service.
class CharacterSystem final {
public:
    explicit CharacterSystem(PhysicsWorld& physics, CharacterLimits limits = {});
    ~CharacterSystem();
    CharacterSystem(const CharacterSystem&) = delete;
    CharacterSystem& operator=(const CharacterSystem&) = delete;
    CharacterSystem(CharacterSystem&&) = delete;
    CharacterSystem& operator=(CharacterSystem&&) = delete;

    [[nodiscard]] CharacterResult attach(ecs::Entity entity,
        const CharacterController& controller = {},
        CharacterAuthority authority = CharacterAuthority::Manual);
    bool detach(ecs::Entity entity);
    bool clear();
    bool shutdown();
    [[nodiscard]] const CharacterComponent* get(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::optional<CharacterState> state(ecs::Entity entity) const noexcept;
    [[nodiscard]] std::vector<ecs::Entity> entities() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const CharacterLimits& limits() const noexcept;
    [[nodiscard]] const CharacterStatistics& statistics() const noexcept;

    bool set_input(ecs::Entity entity, const CharacterInput& input);
    bool request_jump(ecs::Entity entity, bool held = true);
    bool request_crouch(ecs::Entity entity, bool crouched);
    bool set_enabled(ecs::Entity entity, bool enabled);
    bool set_authority(ecs::Entity entity, CharacterAuthority authority);
    // Selecting a player grants exclusive player-input authority. With cameras,
    // the chosen active camera becomes Follow and its prior controller is saved.
    bool select_player(ecs::Entity entity, render::CameraService* cameras = nullptr,
                       ecs::Entity camera = {});
    bool release_player();
    [[nodiscard]] ecs::Entity player() const noexcept;

    void control(double delta_seconds, std::uint64_t tick,
                 input::InputSystem* input = nullptr,
                 const render::CameraService* cameras = nullptr);
    void sample(std::uint64_t tick);
    [[nodiscard]] std::span<const CharacterEvent> events() const noexcept;
    void clear_events() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

void register_character(core::Kernel& kernel);

} // namespace ge::physics
