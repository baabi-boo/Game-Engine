#pragma once

#include <ge/assets/streaming.hpp>
#include <ge/scene/scene.hpp>
#include <ge/world/streaming.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ge::physics { class PhysicsWorld; }

namespace ge::scene {

struct PartitionLimits {
    std::size_t max_regions{4096};
    std::size_t max_sources{64};
    std::size_t max_assets_per_region{128};
    std::size_t max_roots_per_region{256};
    std::size_t max_total_bindings{65536};
    std::size_t admissions_per_update{8};
    std::size_t steps_per_update{128};
    std::size_t shift_history{128};
    std::size_t max_name_bytes{128};
    std::size_t max_message_bytes{512};
    std::uint64_t reservation_budget_bytes{512ULL * 1024ULL * 1024ULL};
    double coordinate_limit{1.0e12};
};

struct RegionRoot {
    // A live top-level Scene root, exclusively assigned to one region. Its
    // descendants keep their own enable flags when this root is gated.
    Entity entity{};
    bool active_enabled{true};
};

struct OriginState {
    math::Vec3 absolute_origin{};
    std::uint64_t epoch{};
    bool consistent{true};
};

// Runtime positions subtract offset. Authoring coordinates and demand centers
// remain absolute: absolute = runtime + absolute_origin. Each reader owns its
// history cursor; no event queue is consumed by another reader.
struct OriginShift {
    std::uint64_t previous_epoch{};
    std::uint64_t epoch{};
    math::Vec3 offset{};
    math::Vec3 old_origin{};
    math::Vec3 new_origin{};
    std::size_t scene_roots{};
    std::size_t physics_bodies{};
    std::size_t producer_rejections{};
    bool physics_shifted{};
};

struct OriginHistory {
    std::vector<OriginShift> shifts;
    OriginState latest;
    bool history_lost{};
};

enum class OriginShiftStatus : std::uint8_t {
    Applied,
    InvalidOffset,
    Busy,
    EpochExhausted,
    SceneRejected,
    PhysicsRejected,
    RollbackFailed,
    Indeterminate
};

struct OriginShiftResult {
    OriginShiftStatus status{OriginShiftStatus::InvalidOffset};
    OriginShift shift;
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == OriginShiftStatus::Applied;
    }
};

struct RegionContext {
    world::RequestToken token;
    OriginState coordinates;
    math::Aabb local_bounds;
    Scene& scene;
    ge::assets::AssetStreamer& streamer;
    ge::physics::PhysicsWorld* physics{};
    std::span<const RegionRoot> roots;
    std::span<const ge::assets::StreamPin> assets;
    bool shutting_down{};
};

struct ProducerReadiness {
    std::uint32_t ready_mask{};
    std::uint64_t coordinate_epoch{};
    bool failed{};
    std::string message;
};

// Callbacks execute synchronously on the partition owner thread, without
// reentering WorldPartition. prepare may poll a producer's own asynchronous
// operation, but must use the supplied pinned assets for imported content.
// Collision/navigation/visual bits require actual producer confirmation; loaded
// bytes alone never establish them. All three lifecycle callbacks are required.
// activate commits producer visibility; false triggers retirement. retire must
// disable and join/release every producer-owned resource before returning true.
// During shutdown it must finish synchronously. Callback destructors must not
// access borrowed services. Scene roots are borrowed, disabled on retirement,
// and never destroyed by this service. Consumers must not retain context spans.
struct RegionCallbacks {
    std::function<ProducerReadiness(const RegionContext&)> prepare;
    std::function<bool(const RegionContext&)> activate;
    std::function<bool(const RegionContext&)> retire;
    // Called after a committed shift. True confirms every producer-owned
    // spatial cache has moved to the new epoch; otherwise the region retires.
    std::function<bool(const RegionContext&, const OriginShift&)> shifted;
};

struct RegionDescriptor {
    std::uint32_t id{};
    std::string name;
    math::Aabb absolute_bounds;
    std::uint64_t content_revision{1};
    std::uint64_t reservation_bytes{1};
    std::uint32_t required_mask{world::readiness_logic};
    std::int32_t priority_bias{};
    std::vector<assets::AssetId> assets;
    std::vector<RegionRoot> roots;
    std::shared_ptr<const RegionCallbacks> callbacks;
};

enum class RegionDemandMode : std::uint8_t { Automatic, Hold, Suspended };

struct DemandSource {
    std::uint32_t id{};
    math::Vec3 absolute_position{};
    double radius{128.0};
    double hysteresis{32.0};
    std::int32_t priority{};
    bool enabled{true};
};

struct PartitionResult {
    bool success{};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

struct RegionSnapshot {
    std::uint32_t id{};
    std::string name;
    world::RegionStatus policy;
    RegionDemandMode demand_mode{RegionDemandMode::Automatic};
    assets::StreamConsumer consumer;
    std::size_t requested_assets{};
    std::size_t pinned_assets{};
    std::size_t roots{};
    std::int32_t effective_priority{};
    double nearest_distance{};
    std::uint64_t producer_epoch{};
    bool wanted{};
    bool faulted{};
    bool producer_started{};
    bool producer_released{};
    bool consumer_invalidated{};
    bool waiting_for_stream_jobs{};
    std::string message;
};

struct PartitionStatistics {
    std::size_t regions{};
    std::size_t sources{};
    std::size_t wanted{};
    std::size_t loading{};
    std::size_t active{};
    std::size_t retiring{};
    std::size_t faulted{};
    std::size_t pinned_assets{};
    std::uint64_t admissions{};
    std::uint64_t activations{};
    std::uint64_t retirements{};
    std::uint64_t rejected_admissions{};
    std::uint64_t producer_failures{};
    // Region reservations are planning estimates. Streamer reservations and
    // cache byte counts are independent global physical accounting; cancellation
    // does not reduce an in-flight job's reservation or evict cached payloads.
    world::MemoryUsage region_reservations;
    assets::StreamStatistics streamer;
    assets::CacheStatistics cache;
    OriginState coordinates;
    bool accepting{};
};

// Single writer. Destructor never touches borrowed Scene/streamer/cache/physics.
// shutdown must run while dependencies and producer callback owners are alive.
// Unloaded descriptors do not retain stream consumers. Retirement invalidates
// consumer epochs after producer cleanup. Regions with assets wait for global
// streamer jobs to drain: there is no per-consumer job completion fence.
class WorldPartition final {
public:
    WorldPartition(Scene& scene, assets::AssetStreamer& streamer,
                   assets::ResourceCache& cache, physics::PhysicsWorld* physics = nullptr,
                   PartitionLimits limits = {});
    ~WorldPartition();
    WorldPartition(const WorldPartition&) = delete;
    WorldPartition& operator=(const WorldPartition&) = delete;
    WorldPartition(WorldPartition&&) = delete;
    WorldPartition& operator=(WorldPartition&&) = delete;

    [[nodiscard]] const PartitionLimits& limits() const noexcept;
    [[nodiscard]] PartitionResult define_region(RegionDescriptor descriptor);
    [[nodiscard]] PartitionResult erase_region(std::uint32_t region);
    [[nodiscard]] PartitionResult set_demand_mode(std::uint32_t region, RegionDemandMode mode);
    [[nodiscard]] PartitionResult retry_region(std::uint32_t region);
    [[nodiscard]] PartitionResult upsert_source(const DemandSource& source);
    [[nodiscard]] bool erase_source(std::uint32_t source);
    [[nodiscard]] std::vector<DemandSource> sources() const;
    [[nodiscard]] std::optional<RegionDescriptor> descriptor(std::uint32_t region) const;
    [[nodiscard]] std::optional<RegionSnapshot> inspect(std::uint32_t region) const;
    [[nodiscard]] std::vector<RegionSnapshot> regions() const;
    [[nodiscard]] PartitionStatistics statistics() const;
    [[nodiscard]] OriginState origin() const noexcept;
    [[nodiscard]] std::optional<math::Vec3> to_local(math::Vec3 absolute) const noexcept;
    [[nodiscard]] std::optional<math::Vec3> to_absolute(math::Vec3 local) const noexcept;
    // Advance a cursor to the last returned shift, not latest.epoch when the
    // batch is limited. A lost history requires rebuilding the reader's cache.
    [[nodiscard]] OriginHistory shifts_since(std::uint64_t epoch, std::size_t limit = 128) const;

    // Roots are batch-translated first. Physics then writes its old cached body
    // poses minus offset, avoiding a second shift for nested bodies. Body caches
    // must already match Scene. A rejected physics operation restores saved root
    // matrices. Exceptions/failed restoration leave coordinates inconsistent and
    // block further work; cross-service mutations are not a general transaction.
    // Contact caches are cleared by physics. Other services consume shift history.
    [[nodiscard]] OriginShiftResult shift_origin(math::Vec3 offset);
    void update();
    [[nodiscard]] PartitionStatistics shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_partition(core::Kernel& kernel);

} // namespace ge::scene
