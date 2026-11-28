#include <ge/scene/partition.hpp>

#include <ge/core/runtime.hpp>
#include <ge/physics/dynamics.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ge::scene {
namespace {

bool bounded(math::Vec3 point, double limit) noexcept {
    return math::finite(point) && std::abs(point.x) <= limit &&
        std::abs(point.y) <= limit && std::abs(point.z) <= limit;
}

void increment(std::uint64_t& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) {
        ++counter;
    }
}

double distance_to(const math::Aabb& bounds, math::Vec3 point) noexcept {
    const double x = std::max({bounds.min.x - point.x, 0.0, point.x - bounds.max.x});
    const double y = std::max({bounds.min.y - point.y, 0.0, point.y - bounds.max.y});
    const double z = std::max({bounds.min.z - point.z, 0.0, point.z - bounds.max.z});
    return std::hypot(x, y, z);
}

std::int32_t combine_priority(std::int32_t first, std::int32_t second) noexcept {
    const auto sum = static_cast<std::int64_t>(first) + static_cast<std::int64_t>(second);
    return static_cast<std::int32_t>(std::clamp(sum,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

jobs::Priority stream_priority(std::int32_t priority) noexcept {
    if (priority > 0) {
        return jobs::Priority::High;
    }
    if (priority < -100) {
        return jobs::Priority::Background;
    }
    return priority < 0 ? jobs::Priority::Low : jobs::Priority::Normal;
}

bool equal_matrix(const math::Mat4& first, const math::Mat4& second) noexcept {
    for (std::size_t index = 0; index < first.data.size(); ++index) {
        const double tolerance = 1.0e-8 + 64.0 * std::numeric_limits<double>::epsilon() *
            std::max(1.0, std::abs(first.data[index]));
        if (std::abs(first.data[index] - second.data[index]) > tolerance) {
            return false;
        }
    }
    return math::finite(first) && math::finite(second);
}

class BusyScope final {
public:
    explicit BusyScope(bool& flag) noexcept : flag_(flag) { flag_ = true; }
    ~BusyScope() { flag_ = false; }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;
private:
    bool& flag_;
};

template<class Number>
std::optional<Number> number(std::string_view value) noexcept {
    Number result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

std::optional<math::Vec3> vector_argument(core::CommandArgs arguments, std::size_t offset) {
    if (offset > arguments.size() || arguments.size() - offset < 3) {
        return std::nullopt;
    }
    const auto x = number<double>(arguments[offset]);
    const auto y = number<double>(arguments[offset + 1]);
    const auto z = number<double>(arguments[offset + 2]);
    if (!x || !y || !z) {
        return std::nullopt;
    }
    const math::Vec3 result{*x, *y, *z};
    return math::finite(result) ? std::optional{result} : std::nullopt;
}

core::CommandResult command_result(PartitionResult result) {
    return {result.success, std::move(result.message)};
}

} // namespace

struct WorldPartition::Impl {
    struct Record {
        RegionDescriptor definition;
        world::RequestToken token;
        RegionDemandMode mode{RegionDemandMode::Automatic};
        assets::StreamConsumer consumer;
        std::vector<assets::StreamTicket> tickets;
        std::vector<assets::StreamPin> pins;
        std::int32_t priority{};
        double distance{std::numeric_limits<double>::infinity()};
        std::uint64_t producer_epoch{};
        bool wanted{};
        bool faulted{};
        bool producer_started{};
        bool producer_released{};
        bool consumer_invalidated{};
        bool waiting_jobs{};
        std::string message;
    };

    Impl(Scene& shared_scene, assets::AssetStreamer& shared_streamer,
         assets::ResourceCache& shared_cache, ge::physics::PhysicsWorld* shared_physics,
         PartitionLimits configuration)
        : scene(shared_scene), streamer(shared_streamer), cache(shared_cache),
          physics(shared_physics), limits(configuration), policy(limits.reservation_budget_bytes) {
        if (limits.max_regions == 0 || limits.max_regions > 65536 ||
            limits.max_sources == 0 || limits.max_sources > 1024 ||
            limits.max_assets_per_region == 0 || limits.max_assets_per_region > 4096 ||
            limits.max_roots_per_region == 0 || limits.max_roots_per_region > 4096 ||
            limits.max_total_bindings == 0 || limits.max_total_bindings > 1048576 ||
            limits.admissions_per_update == 0 || limits.steps_per_update == 0 ||
            limits.shift_history == 0 || limits.shift_history > 65536 ||
            limits.max_name_bytes == 0 || limits.max_name_bytes > 4096 ||
            limits.max_message_bytes == 0 || limits.max_message_bytes > 4096 ||
            limits.reservation_budget_bytes == 0 || !std::isfinite(limits.coordinate_limit) ||
            limits.coordinate_limit <= 0.0 || limits.coordinate_limit > 1.0e12) {
            throw std::invalid_argument("invalid world partition limits");
        }
        if (physics != nullptr && &physics->scene() != &scene) {
            throw std::invalid_argument("partition and physics must share the same Scene");
        }
        shift_log.reserve(limits.shift_history);
    }

    bool editable() const noexcept {
        return accepting && !busy && coordinates.consistent && !scene.registry().iterating() &&
            (physics == nullptr || !physics->stepping());
    }

    void message(Record& record, std::string_view text) {
        record.message.assign(text.substr(0, limits.max_message_bytes));
    }

    world::RegionState state(const Record& record) const noexcept {
        return policy.state(record.definition.id);
    }

    RegionContext context(const Record& record) {
        const auto& bounds = record.definition.absolute_bounds;
        return {record.token, coordinates,
            {bounds.min - coordinates.absolute_origin, bounds.max - coordinates.absolute_origin},
            scene, streamer, physics, record.definition.roots, record.pins, shutting_down};
    }

    bool valid_roots(const Record& record) const noexcept {
        for (const auto& root : record.definition.roots) {
            if (!scene.registry().alive(root.entity) || !scene.contains(root.entity) ||
                scene.parent(root.entity)) {
                return false;
            }
        }
        return true;
    }

    bool gate(Record& record, bool active) {
        bool complete = true;
        for (const auto& root : record.definition.roots) {
            if (scene.registry().alive(root.entity)) {
                complete = scene.registry().set_enabled(root.entity,
                    active && root.active_enabled) && complete;
            } else if (active) {
                complete = false;
            }
        }
        const auto refreshed = scene.update();
        complete = refreshed.error == SceneError::None && complete;
        if (physics != nullptr) {
            const auto synchronized = physics->collision().sync(scene);
            complete = !synchronized.scene_busy && complete;
        }
        return complete;
    }

    void retire(Record& record, std::string_view reason) {
        const auto previous = state(record);
        if (previous == world::RegionState::Unloaded || previous == world::RegionState::Retiring) {
            return;
        }
        const bool accepted = previous == world::RegionState::Active ?
            policy.retire(record.token) : policy.cancel(record.token);
        if (!accepted) {
            throw std::logic_error("partition policy rejected its current region token");
        }
        message(record, reason);
        try {
            (void)gate(record, false);
        } catch (...) {
            message(record, "roots disabled; Scene/collider synchronization will retry during retirement");
        }
    }

    void fail(Record& record, std::string_view reason) {
        if (!record.faulted) {
            increment(counters.producer_failures);
        }
        record.faulted = true;
        message(record, reason);
        retire(record, reason);
    }

    void evaluate_demand(Record& record) {
        const bool retained = record.wanted && state(record) != world::RegionState::Unloaded;
        record.wanted = record.mode == RegionDemandMode::Hold;
        record.priority = record.definition.priority_bias;
        record.distance = std::numeric_limits<double>::infinity();
        if (record.mode != RegionDemandMode::Automatic) {
            return;
        }
        bool first = true;
        for (const auto& [identity, source] : demand_sources) {
            (void)identity;
            if (!source.enabled) {
                continue;
            }
            const double separation = distance_to(record.definition.absolute_bounds, source.absolute_position);
            const double radius = source.radius + (retained ? source.hysteresis : 0.0);
            if (separation > radius) {
                continue;
            }
            record.wanted = true;
            record.distance = std::min(record.distance, separation);
            const auto priority = combine_priority(source.priority, record.definition.priority_bias);
            record.priority = first ? priority : std::max(record.priority, priority);
            first = false;
        }
    }

    void begin(Record& record) {
        const auto requested = policy.begin(record.definition.id, record.definition.content_revision,
            coordinates.epoch, record.definition.required_mask, record.definition.reservation_bytes);
        if (!requested) {
            increment(counters.rejected_admissions);
            message(record, "region reservation budget unavailable or request generations exhausted");
            return;
        }
        record.token = *requested;
        record.producer_started = false;
        record.producer_released = false;
        record.consumer_invalidated = false;
        record.waiting_jobs = false;
        record.producer_epoch = coordinates.epoch;
        record.message.clear();
        increment(counters.admissions);
        try {
            record.tickets.reserve(record.definition.assets.size());
            record.pins.reserve(record.definition.assets.size());
            if (!record.definition.assets.empty()) {
                record.consumer = streamer.create_consumer();
            }
            assets::StreamRequestOptions options;
            options.priority = stream_priority(record.priority);
            options.attempts = std::min<std::uint32_t>(3, streamer.limits().maximum_attempts);
            for (const auto asset : record.definition.assets) {
                record.tickets.push_back(streamer.request(asset, record.consumer, options));
            }
        } catch (const std::exception& error) {
            fail(record, error.what());
        } catch (...) {
            fail(record, "unknown exception while admitting asset requests");
        }
    }

    bool acquire_assets(Record& record) {
        for (const auto ticket : record.tickets) {
            const auto held = std::find_if(record.pins.begin(), record.pins.end(),
                [ticket](const assets::StreamPin& pin) { return pin.request == ticket; });
            if (held != record.pins.end()) {
                continue;
            }
            const auto snapshot = streamer.inspect(ticket);
            if (!snapshot || snapshot->state == assets::StreamState::failed ||
                snapshot->state == assets::StreamState::cancelled) {
                fail(record, snapshot ? snapshot->error : "asset request disappeared");
                return false;
            }
            (void)streamer.reprioritize(ticket, stream_priority(record.priority));
            if (snapshot->state != assets::StreamState::ready || snapshot->last_good ||
                snapshot->available_revision != snapshot->desired_revision) {
                continue;
            }
            if (const auto pinned = streamer.pin(ticket, false)) {
                record.pins.push_back(*pinned);
            }
        }
        return record.pins.size() == record.definition.assets.size();
    }

    void load(Record& record) {
        if (!valid_roots(record)) {
            fail(record, "bound region roots were destroyed or reparented");
            return;
        }
        if (!acquire_assets(record)) {
            return;
        }
        std::uint32_t ready = record.definition.roots.empty() ? 0U : world::readiness_logic;
        const auto& callbacks = record.definition.callbacks;
        if (callbacks) {
            record.producer_started = true;
            const auto progress = callbacks->prepare(context(record));
            message(record, progress.message);
            if (progress.failed || (progress.ready_mask & ~world::readiness_all) != 0U) {
                fail(record, progress.message.empty() ? "producer rejected region preparation" : progress.message);
                return;
            }
            if (progress.coordinate_epoch != coordinates.epoch) {
                message(record, "producer readiness belongs to an earlier coordinate epoch");
                return;
            }
            ready |= progress.ready_mask;
        }
        if ((ready & record.definition.required_mask) != record.definition.required_mask) {
            return;
        }
        record.producer_epoch = coordinates.epoch;
        if (!policy.mark_ready(record.token, ready)) {
            fail(record, "readiness token rejected by region policy");
            return;
        }
        if (callbacks && !callbacks->activate(context(record))) {
            fail(record, "producer activation rejected; resources retained for retirement");
            return;
        }
        if (!valid_roots(record) || !gate(record, true) || !policy.activate(record.token)) {
            fail(record, "region root activation or policy activation failed");
            return;
        }
        record.message.clear();
        increment(counters.activations);
    }

    void release_consumer(Record& record) {
        for (const auto& pin : record.pins) {
            (void)streamer.release(pin);
        }
        record.pins.clear();
        if (record.consumer && streamer.valid(record.consumer)) {
            const auto stream_state = streamer.statistics();
            if (stream_state.accepting && record.consumer.epoch != std::numeric_limits<std::uint64_t>::max()) {
                record.consumer = streamer.advance_consumer(record.consumer);
            }
            if (!streamer.destroy_consumer(record.consumer)) {
                throw std::logic_error("partition consumer could not be destroyed");
            }
        }
        record.consumer = {};
        record.tickets.clear();
        record.consumer_invalidated = true;
    }

    void finish_retirement(Record& record) {
        if (!gate(record, false)) {
            message(record, "scene or collider deactivation is not yet complete");
            return;
        }
        if (!record.producer_released) {
            const auto& callbacks = record.definition.callbacks;
            if (record.producer_started && callbacks && !callbacks->retire(context(record))) {
                message(record, shutting_down ? "producer did not finish required shutdown cleanup" :
                    "waiting for producer resources to become quiescent");
                return;
            }
            record.producer_released = true;
        }
        if (!record.consumer_invalidated) {
            release_consumer(record);
        }
        record.waiting_jobs = !record.definition.assets.empty() && streamer.statistics().jobs != 0;
        if (record.waiting_jobs) {
            message(record, "consumer invalidated; global streamer jobs still retain their own budgets");
            return;
        }
        if (!policy.acknowledge_retirement(record.token)) {
            throw std::logic_error("region retirement acknowledgement rejected");
        }
        record.token = {};
        record.producer_started = false;
        increment(counters.retirements);
        if (!record.faulted) {
            record.message.clear();
        }
    }

    void process(Record& record) {
        try {
            switch (state(record)) {
            case world::RegionState::Loading:
                load(record);
                break;
            case world::RegionState::Active:
                if (!valid_roots(record)) {
                    fail(record, "active region lost a bound Scene root");
                }
                break;
            case world::RegionState::Retiring:
                finish_retirement(record);
                break;
            case world::RegionState::Ready:
                fail(record, "uncommitted producer activation requires retirement");
                break;
            case world::RegionState::Unloaded:
                break;
            }
        } catch (const std::exception& error) {
            fail(record, error.what());
        } catch (...) {
            fail(record, "unknown exception from region producer or resource service");
        }
    }

    RegionSnapshot snapshot(const Record& record) const {
        RegionSnapshot result;
        result.id = record.definition.id;
        result.name = record.definition.name;
        if (const auto current = policy.status(result.id)) {
            result.policy = *current;
        } else {
            result.policy.required_mask = record.definition.required_mask;
            result.policy.budget_bytes = record.definition.reservation_bytes;
        }
        result.demand_mode = record.mode;
        result.consumer = record.consumer;
        result.requested_assets = record.tickets.size();
        result.pinned_assets = record.pins.size();
        result.roots = record.definition.roots.size();
        result.effective_priority = record.priority;
        result.nearest_distance = record.distance;
        result.producer_epoch = record.producer_epoch;
        result.wanted = record.wanted;
        result.faulted = record.faulted;
        result.producer_started = record.producer_started;
        result.producer_released = record.producer_released;
        result.consumer_invalidated = record.consumer_invalidated;
        result.waiting_for_stream_jobs = record.waiting_jobs;
        result.message = record.message;
        return result;
    }

    Scene& scene;
    assets::AssetStreamer& streamer;
    assets::ResourceCache& cache;
    ge::physics::PhysicsWorld* physics;
    PartitionLimits limits;
    world::StreamingPolicy policy;
    std::map<std::uint32_t, Record> records;
    std::map<std::uint32_t, DemandSource> demand_sources;
    std::vector<OriginShift> shift_log;
    OriginState coordinates;
    PartitionStatistics counters;
    std::size_t bindings{};
    std::uint32_t step_cursor{};
    bool accepting{true};
    bool busy{};
    bool shutting_down{};
};

WorldPartition::WorldPartition(Scene& scene, assets::AssetStreamer& streamer,
    assets::ResourceCache& cache, physics::PhysicsWorld* physics, PartitionLimits limits)
    : impl_(std::make_unique<Impl>(scene, streamer, cache, physics, limits)) {}

WorldPartition::~WorldPartition() = default;

const PartitionLimits& WorldPartition::limits() const noexcept {
    return impl_->limits;
}

PartitionResult WorldPartition::define_region(RegionDescriptor descriptor_value) {
    auto& data = *impl_;
    if (!data.editable()) {
        return {false, "partition is stopped, busy, or has inconsistent coordinates"};
    }
    const auto& configuration = data.limits;
    if (descriptor_value.id == 0 || descriptor_value.name.size() > configuration.max_name_bytes ||
        descriptor_value.name.find('\0') != std::string::npos ||
        !math::valid(descriptor_value.absolute_bounds) ||
        !bounded(descriptor_value.absolute_bounds.min, configuration.coordinate_limit) ||
        !bounded(descriptor_value.absolute_bounds.max, configuration.coordinate_limit) ||
        !world::valid_readiness_mask(descriptor_value.required_mask) ||
        descriptor_value.reservation_bytes == 0 ||
        descriptor_value.reservation_bytes > configuration.reservation_budget_bytes ||
        descriptor_value.assets.size() > configuration.max_assets_per_region ||
        descriptor_value.roots.size() > configuration.max_roots_per_region) {
        return {false, "invalid region identity, bounds, readiness, reservation, or binding count"};
    }
    const auto& callbacks = descriptor_value.callbacks;
    if (callbacks && (!callbacks->prepare || !callbacks->activate || !callbacks->retire)) {
        return {false, "producer requires prepare, activate, and retire callbacks"};
    }
    if (!callbacks && (descriptor_value.roots.empty() ||
        descriptor_value.required_mask != world::readiness_logic)) {
        return {false, "non-logic readiness requires an explicit producer; logic requires Scene roots"};
    }
    const auto existing = data.records.find(descriptor_value.id);
    if (existing != data.records.end() && data.state(existing->second) != world::RegionState::Unloaded) {
        return {false, "region descriptor can only change after retirement acknowledgement"};
    }
    if (existing == data.records.end() && data.records.size() >= configuration.max_regions) {
        return {false, "region descriptor capacity exhausted"};
    }
    const auto old_bindings = existing == data.records.end() ? 0 :
        existing->second.definition.assets.size() + existing->second.definition.roots.size();
    const auto new_bindings = descriptor_value.assets.size() + descriptor_value.roots.size();
    if (new_bindings > configuration.max_total_bindings - (data.bindings - old_bindings)) {
        return {false, "partition binding capacity exhausted"};
    }
    std::set<assets::AssetId> unique_assets;
    for (const auto asset : descriptor_value.assets) {
        if (!asset || !unique_assets.insert(asset).second) {
            return {false, "asset bindings must have unique nonzero identities"};
        }
    }
    std::set<Entity> unique_roots;
    for (const auto& root : descriptor_value.roots) {
        if (!data.scene.contains(root.entity) || data.scene.parent(root.entity) ||
            !data.scene.registry().alive(root.entity) || !unique_roots.insert(root.entity).second) {
            return {false, "each bound entity must be a unique, live Scene root"};
        }
        for (const auto& [identity, record] : data.records) {
            if (identity == descriptor_value.id) {
                continue;
            }
            for (const auto& owned : record.definition.roots) {
                if (owned.entity == root.entity) {
                    return {false, "Scene root already belongs to another region"};
                }
            }
        }
    }
    Impl::Record prepared;
    prepared.definition = std::move(descriptor_value);
    prepared.message.reserve(configuration.max_message_bytes);
    const auto identity = prepared.definition.id;
    if (existing == data.records.end()) {
        data.records.emplace(identity, std::move(prepared));
    } else {
        existing->second = std::move(prepared);
    }
    data.bindings = data.bindings - old_bindings + new_bindings;
    (void)data.gate(data.records.at(identity), false);
    return {true, "region defined with borrowed roots disabled until activation"};
}

PartitionResult WorldPartition::erase_region(std::uint32_t region) {
    auto& data = *impl_;
    const auto found = data.records.find(region);
    if (!data.editable() || found == data.records.end()) {
        return {false, "region is unknown or partition is not editable"};
    }
    if (data.state(found->second) != world::RegionState::Unloaded) {
        return {false, "suspend the region and wait for retirement before erasing it"};
    }
    data.bindings -= found->second.definition.assets.size() + found->second.definition.roots.size();
    data.records.erase(found);
    return {true, "region descriptor erased; borrowed Scene roots remain disabled"};
}

PartitionResult WorldPartition::set_demand_mode(std::uint32_t region, RegionDemandMode mode) {
    auto& data = *impl_;
    const auto found = data.records.find(region);
    if (!data.editable() || found == data.records.end() ||
        (mode != RegionDemandMode::Automatic && mode != RegionDemandMode::Hold &&
         mode != RegionDemandMode::Suspended)) {
        return {false, "invalid region, demand mode, or busy partition"};
    }
    found->second.mode = mode;
    data.evaluate_demand(found->second);
    if (!found->second.wanted) {
        data.retire(found->second, "region demand suspended or outside every source radius");
    }
    return {true, "region demand mode updated"};
}

PartitionResult WorldPartition::retry_region(std::uint32_t region) {
    auto& data = *impl_;
    const auto found = data.records.find(region);
    if (!data.editable() || found == data.records.end() ||
        data.state(found->second) != world::RegionState::Unloaded) {
        return {false, "retry requires an editable, fully retired region"};
    }
    found->second.faulted = false;
    found->second.message.clear();
    return {true, "region fault cleared; current demand determines the next admission"};
}

PartitionResult WorldPartition::upsert_source(const DemandSource& source) {
    auto& data = *impl_;
    if (!data.editable() || source.id == 0 ||
        !bounded(source.absolute_position, data.limits.coordinate_limit) ||
        !std::isfinite(source.radius) || !std::isfinite(source.hysteresis) ||
        source.radius < 0.0 || source.hysteresis < 0.0 ||
        source.radius > data.limits.coordinate_limit ||
        source.hysteresis > data.limits.coordinate_limit - source.radius) {
        return {false, "invalid demand identity, absolute center, radius, or hysteresis"};
    }
    if (!data.demand_sources.contains(source.id) && data.demand_sources.size() >= data.limits.max_sources) {
        return {false, "demand source capacity exhausted"};
    }
    data.demand_sources.insert_or_assign(source.id, source);
    return {true, "absolute-coordinate demand source updated"};
}

bool WorldPartition::erase_source(std::uint32_t source) {
    return impl_->editable() && impl_->demand_sources.erase(source) != 0;
}

std::vector<DemandSource> WorldPartition::sources() const {
    std::vector<DemandSource> result;
    result.reserve(impl_->demand_sources.size());
    for (const auto& [identity, source] : impl_->demand_sources) {
        (void)identity;
        result.push_back(source);
    }
    return result;
}

std::optional<RegionDescriptor> WorldPartition::descriptor(std::uint32_t region) const {
    const auto found = impl_->records.find(region);
    return found == impl_->records.end() ? std::nullopt : std::optional{found->second.definition};
}

std::optional<RegionSnapshot> WorldPartition::inspect(std::uint32_t region) const {
    const auto found = impl_->records.find(region);
    return found == impl_->records.end() ? std::nullopt : std::optional{impl_->snapshot(found->second)};
}

std::vector<RegionSnapshot> WorldPartition::regions() const {
    std::vector<RegionSnapshot> result;
    result.reserve(impl_->records.size());
    for (const auto& [identity, record] : impl_->records) {
        (void)identity;
        result.push_back(impl_->snapshot(record));
    }
    return result;
}

PartitionStatistics WorldPartition::statistics() const {
    auto result = impl_->counters;
    result.regions = impl_->records.size();
    result.sources = impl_->demand_sources.size();
    result.region_reservations = impl_->policy.memory_usage();
    result.streamer = impl_->streamer.statistics();
    result.cache = impl_->cache.statistics();
    result.coordinates = impl_->coordinates;
    result.accepting = impl_->accepting;
    for (const auto& [identity, record] : impl_->records) {
        (void)identity;
        result.wanted += record.wanted ? 1U : 0U;
        result.faulted += record.faulted ? 1U : 0U;
        result.pinned_assets += record.pins.size();
        const auto current = impl_->state(record);
        result.loading += current == world::RegionState::Loading || current == world::RegionState::Ready ? 1U : 0U;
        result.active += current == world::RegionState::Active ? 1U : 0U;
        result.retiring += current == world::RegionState::Retiring ? 1U : 0U;
    }
    return result;
}

OriginState WorldPartition::origin() const noexcept {
    return impl_->coordinates;
}

std::optional<math::Vec3> WorldPartition::to_local(math::Vec3 absolute) const noexcept {
    const auto result = absolute - impl_->coordinates.absolute_origin;
    return impl_->coordinates.consistent && bounded(absolute, impl_->limits.coordinate_limit) &&
        bounded(result, impl_->limits.coordinate_limit) ? std::optional{result} : std::nullopt;
}

std::optional<math::Vec3> WorldPartition::to_absolute(math::Vec3 local) const noexcept {
    const auto result = local + impl_->coordinates.absolute_origin;
    return impl_->coordinates.consistent && bounded(local, impl_->limits.coordinate_limit) &&
        bounded(result, impl_->limits.coordinate_limit) ? std::optional{result} : std::nullopt;
}

OriginHistory WorldPartition::shifts_since(std::uint64_t epoch, std::size_t limit) const {
    OriginHistory result;
    result.latest = impl_->coordinates;
    result.history_lost = epoch > result.latest.epoch || (!impl_->shift_log.empty() &&
        epoch < impl_->shift_log.front().previous_epoch);
    const auto count = std::min(limit, impl_->limits.shift_history);
    result.shifts.reserve(count);
    for (const auto& shift : impl_->shift_log) {
        if (shift.epoch > epoch && result.shifts.size() < count) {
            result.shifts.push_back(shift);
        }
    }
    return result;
}

void WorldPartition::update() {
    auto& data = *impl_;
    if (!data.editable()) {
        return;
    }
    BusyScope scope(data.busy);
    if (data.scene.update().error != SceneError::None) {
        return;
    }
    for (auto& [identity, record] : data.records) {
        (void)identity;
        data.evaluate_demand(record);
        if (!record.wanted) {
            data.retire(record, "no demand source remains within the retention radius");
        }
    }
    auto next = data.records.upper_bound(data.step_cursor);
    const auto steps = std::min(data.records.size(), data.limits.steps_per_update);
    for (std::size_t index = 0; index < steps; ++index) {
        if (next == data.records.end()) {
            next = data.records.begin();
        }
        data.step_cursor = next->first;
        data.process(next->second);
        ++next;
    }
    std::vector<Impl::Record*> candidates;
    candidates.reserve(data.records.size());
    for (auto& [identity, record] : data.records) {
        (void)identity;
        if (record.wanted && !record.faulted && data.state(record) == world::RegionState::Unloaded) {
            candidates.push_back(&record);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto* first, const auto* second) {
        if (first->priority != second->priority) {
            return first->priority > second->priority;
        }
        if (first->distance != second->distance) {
            return first->distance < second->distance;
        }
        return first->definition.id < second->definition.id;
    });
    std::size_t admitted{};
    for (auto* record : candidates) {
        if (admitted >= data.limits.admissions_per_update) {
            break;
        }
        if (record->definition.reservation_bytes > data.policy.memory_usage().available_bytes) {
            increment(data.counters.rejected_admissions);
            data.message(*record, "region planning reservation has insufficient capacity");
            continue;
        }
        data.begin(*record);
        ++admitted;
    }
}

OriginShiftResult WorldPartition::shift_origin(math::Vec3 offset) {
    auto& data = *impl_;
    OriginShiftResult result;
    if (!data.editable()) {
        result.status = OriginShiftStatus::Busy;
        result.message = "partition, Registry, or physics is busy, stopped, or inconsistent";
        return result;
    }
    const auto next_origin = data.coordinates.absolute_origin + offset;
    if (!bounded(offset, data.limits.coordinate_limit) || !bounded(next_origin, data.limits.coordinate_limit)) {
        result.message = "origin offset or resulting absolute origin is out of bounds";
        return result;
    }
    if (data.coordinates.epoch == std::numeric_limits<std::uint64_t>::max()) {
        result.status = OriginShiftStatus::EpochExhausted;
        result.message = "coordinate epoch exhausted";
        return result;
    }
    BusyScope scope(data.busy);
    result.shift = {data.coordinates.epoch, data.coordinates.epoch + 1, offset,
        data.coordinates.absolute_origin, next_origin, 0, 0, 0, false};
    const auto refreshed = data.scene.update();
    if (refreshed.error != SceneError::None || refreshed.invalid != 0) {
        result.status = OriginShiftStatus::SceneRejected;
        result.message = "Scene must have valid synchronized transforms before shifting";
        return result;
    }
    std::vector<WorldMatrixEdit> previous;
    std::vector<WorldMatrixEdit> shifted;
    const auto roots = data.scene.roots();
    previous.reserve(roots.size());
    shifted.reserve(roots.size());
    for (const auto root : roots) {
        const auto matrix = data.scene.world_matrix(root);
        if (!matrix) {
            result.status = OriginShiftStatus::SceneRejected;
            result.message = "Scene root lacks a valid world matrix";
            return result;
        }
        auto translated = *matrix;
        translated(0, 3) -= offset.x;
        translated(1, 3) -= offset.y;
        translated(2, 3) -= offset.z;
        const auto decomposed = math::decompose(translated);
        if (!decomposed || !bounded(decomposed->position, data.limits.coordinate_limit)) {
            result.status = OriginShiftStatus::SceneRejected;
            result.message = "shifted root is out of bounds or cannot be represented by Scene";
            return result;
        }
        previous.push_back({root, *matrix});
        shifted.push_back({root, translated});
    }
    result.shift.scene_roots = roots.size();
    if (data.physics != nullptr) {
        const auto bodies = data.physics->bodies();
        result.shift.physics_bodies = bodies.size();
        for (const auto handle : bodies) {
            const auto* body = data.physics->get(handle);
            const auto matrix = body ? data.scene.world_matrix(body->entity) : std::nullopt;
            if (body == nullptr || !matrix || !data.scene.world_transform_exact(body->entity) ||
                !equal_matrix(*matrix, math::matrix(body->pose)) ||
                !bounded(body->pose.position - offset, data.limits.coordinate_limit) ||
                !bounded(body->previous_pose.position - offset, data.limits.coordinate_limit)) {
                result.status = OriginShiftStatus::PhysicsRejected;
                result.message = "physics body cache is stale or shifted body position is out of bounds";
                return result;
            }
        }
    }
    try {
        const auto applied = data.scene.apply_world_matrices(shifted);
        if (!applied) {
            result.status = OriginShiftStatus::SceneRejected;
            result.message = std::string(describe(applied.error));
            return result;
        }
        if (data.physics != nullptr && !data.physics->shift_origin(offset)) {
            const auto restored = data.scene.apply_world_matrices(previous);
            const auto synchronized = data.scene.update();
            const bool complete = restored && synchronized.error == SceneError::None;
            data.coordinates.consistent = complete;
            result.status = complete ? OriginShiftStatus::PhysicsRejected : OriginShiftStatus::RollbackFailed;
            result.message = complete ? "physics rejected shift; saved Scene root matrices restored" :
                "physics rejected shift and Scene restoration failed; external repair is required";
            return result;
        }
        result.shift.physics_shifted = data.physics != nullptr;
        if (data.scene.update().error != SceneError::None) {
            throw std::runtime_error("Scene synchronization failed after origin mutation");
        }
    } catch (const std::exception& error) {
        data.coordinates.consistent = false;
        result.status = OriginShiftStatus::Indeterminate;
        result.message = std::string("cross-service shift may be partially committed: ") + error.what();
        return result;
    } catch (...) {
        data.coordinates.consistent = false;
        result.status = OriginShiftStatus::Indeterminate;
        result.message = "cross-service shift raised an unknown exception; coordinate repair is required";
        return result;
    }
    data.coordinates.absolute_origin = next_origin;
    data.coordinates.epoch = result.shift.epoch;
    if (data.shift_log.size() == data.limits.shift_history) {
        data.shift_log.erase(data.shift_log.begin());
    }
    data.shift_log.push_back(result.shift);
    for (auto& [identity, record] : data.records) {
        (void)identity;
        if (!record.definition.callbacks) {
            record.producer_epoch = data.coordinates.epoch;
        }
        if (!record.producer_started || record.producer_released || !record.definition.callbacks ||
            data.state(record) == world::RegionState::Retiring) {
            continue;
        }
        bool acknowledged{};
        try {
            const auto& callback = record.definition.callbacks->shifted;
            acknowledged = callback && callback(data.context(record), result.shift);
        } catch (...) {
            acknowledged = false;
        }
        if (acknowledged) {
            record.producer_epoch = data.coordinates.epoch;
        } else {
            ++result.shift.producer_rejections;
            data.retire(record, "producer did not acknowledge the new coordinate epoch; reload required");
        }
    }
    data.shift_log.back().producer_rejections = result.shift.producer_rejections;
    result.status = OriginShiftStatus::Applied;
    result.message = "origin shifted; other spatial consumers must advance their independent history cursors";
    return result;
}

PartitionStatistics WorldPartition::shutdown() {
    auto& data = *impl_;
    if (data.busy || data.scene.registry().iterating() ||
        (data.physics != nullptr && data.physics->stepping())) {
        throw std::logic_error("partition shutdown requires an idle owner thread");
    }
    BusyScope scope(data.busy);
    data.accepting = false;
    data.shutting_down = true;
    data.demand_sources.clear();
    for (auto& [identity, record] : data.records) {
        (void)identity;
        record.wanted = false;
        data.retire(record, "partition shutdown requested");
        if (data.state(record) == world::RegionState::Retiring) {
            data.process(record);
        }
    }
    return statistics();
}

void register_partition(core::Kernel& kernel) {
    if (kernel.services().contains<WorldPartition>()) {
        return;
    }
    auto* scene = kernel.services().find<Scene>();
    auto* streamer = kernel.services().find<assets::AssetStreamer>();
    auto* cache = kernel.services().find<assets::ResourceCache>();
    auto* physics_world = kernel.services().find<physics::PhysicsWorld>();
    if (scene == nullptr || streamer == nullptr || cache == nullptr) {
        throw std::logic_error("scene.partition requires Scene, AssetStreamer, and ResourceCache");
    }
    PartitionLimits limits;
    const auto budget = kernel.config().integer("partition.reservation_bytes",
        static_cast<std::int64_t>(limits.reservation_budget_bytes));
    if (budget <= 0) {
        throw std::invalid_argument("partition.reservation_bytes must be positive");
    }
    limits.reservation_budget_bytes = static_cast<std::uint64_t>(budget);
    auto& partition = kernel.services().emplace<WorldPartition>(*scene, *streamer, *cache, physics_world, limits);
    if (!kernel.register_module({"scene.partition", "Asset-backed region activation and coordinate history", 1}) ||
        !kernel.add_shutdown("scene.partition", [service = &partition, host = &kernel] {
            const auto result = service->shutdown();
            if (result.retiring != 0) {
                host->diagnose(core::Diagnostic::Severity::Warning, "scene.partition",
                    "retiring regions remain charged; producers or shared streamer jobs are still live");
            }
        })) {
        (void)partition.shutdown();
        throw std::logic_error("partition module or shutdown registration conflict");
    }
    (void)kernel.add_system({"scene.partition.update", core::Phase::PreSimulation, -8800,
        {"scene.transforms.pre"}}, [](core::Kernel& host, const core::Frame&) {
        auto& service = host.services().get<WorldPartition>();
        service.update();
        const auto state = service.statistics();
        host.metric("partition.active", static_cast<double>(state.active));
        host.metric("partition.retiring", static_cast<double>(state.retiring));
        host.metric("partition.region_reserved_bytes", static_cast<double>(state.region_reservations.reserved_bytes));
        host.metric("partition.stream_inflight_bytes", static_cast<double>(state.streamer.inflight_reserved_bytes));
        host.metric("partition.cache_reserved_bytes", static_cast<double>(state.cache.reserved_bytes()));
        host.metric("partition.origin_epoch", static_cast<double>(state.coordinates.epoch));
    });
    (void)kernel.add_command("partition.region", [](core::Kernel& host, core::CommandArgs args) {
        auto& service = host.services().get<WorldPartition>();
        if (args.size() < 8 || args.size() - 8 > service.limits().max_assets_per_region) {
            return core::CommandResult{false,
                "usage: partition.region id name x y z half_extent reservation_bytes root_entity [asset_id ...]"};
        }
        const auto identity = number<std::uint32_t>(args[0]);
        const auto center = vector_argument(args, 2);
        const auto extent = number<double>(args[5]);
        const auto reservation = number<std::uint64_t>(args[6]);
        const auto root = ecs::parse_entity(args[7]);
        if (!identity || !center || !extent || !reservation || !root || *extent < 0.0) {
            return core::CommandResult{false, "invalid region number, extent, budget, or entity handle"};
        }
        RegionDescriptor definition;
        definition.id = *identity;
        definition.name = args[1];
        const math::Vec3 half{*extent, *extent, *extent};
        definition.absolute_bounds = {*center - half, *center + half};
        definition.reservation_bytes = *reservation;
        definition.roots.push_back({*root, true});
        for (std::size_t index = 8; index < args.size(); ++index) {
            const auto asset = number<std::uint64_t>(args[index]);
            if (!asset) {
                return core::CommandResult{false, "invalid asset identity"};
            }
            definition.assets.push_back({*asset});
        }
        return command_result(service.define_region(std::move(definition)));
    }, "Define an absolute region bound to a real Scene root and optional imported assets");
    (void)kernel.add_command("partition.demand", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 7) {
            return core::CommandResult{false, "usage: partition.demand id x y z radius hysteresis priority"};
        }
        const auto identity = number<std::uint32_t>(args[0]);
        const auto position = vector_argument(args, 1);
        const auto radius = number<double>(args[4]);
        const auto hysteresis = number<double>(args[5]);
        const auto priority = number<std::int32_t>(args[6]);
        if (!identity || !position || !radius || !hysteresis || !priority) {
            return core::CommandResult{false, "invalid demand values"};
        }
        return command_result(host.services().get<WorldPartition>().upsert_source(
            {*identity, *position, *radius, *hysteresis, *priority, true}));
    }, "Create or move a demand source in absolute authoring coordinates");
    (void)kernel.add_command("partition.mode", [](core::Kernel& host, core::CommandArgs args) {
        if (args.size() != 2) {
            return core::CommandResult{false, "usage: partition.mode region_id auto|hold|off"};
        }
        const auto identity = number<std::uint32_t>(args[0]);
        const auto mode = args[1] == "auto" ? RegionDemandMode::Automatic :
            args[1] == "hold" ? RegionDemandMode::Hold : RegionDemandMode::Suspended;
        if (!identity || (args[1] != "auto" && args[1] != "hold" && args[1] != "off")) {
            return core::CommandResult{false, "invalid region or demand mode"};
        }
        return command_result(host.services().get<WorldPartition>().set_demand_mode(*identity, mode));
    }, "Follow demand, hold a region loaded, or suspend it with safe retirement");
    (void)kernel.add_command("partition.erase", [](core::Kernel& host, core::CommandArgs args) {
        const auto identity = args.size() == 1 ? number<std::uint32_t>(args[0]) : std::nullopt;
        return identity ? command_result(host.services().get<WorldPartition>().erase_region(*identity)) :
            core::CommandResult{false, "usage: partition.erase region_id"};
    }, "Erase a fully retired descriptor without destroying borrowed entities");
    (void)kernel.add_command("partition.retry", [](core::Kernel& host, core::CommandArgs args) {
        const auto identity = args.size() == 1 ? number<std::uint32_t>(args[0]) : std::nullopt;
        return identity ? command_result(host.services().get<WorldPartition>().retry_region(*identity)) :
            core::CommandResult{false, "usage: partition.retry region_id"};
    }, "Clear a retired region fault and permit a new generation of asset requests");
    (void)kernel.add_command("partition.source.remove", [](core::Kernel& host, core::CommandArgs args) {
        const auto identity = args.size() == 1 ? number<std::uint32_t>(args[0]) : std::nullopt;
        const bool removed = identity && host.services().get<WorldPartition>().erase_source(*identity);
        return core::CommandResult{removed, removed ? "demand source removed" : "usage: partition.source.remove existing_id"};
    }, "Remove a source and allow its regions to retire on the next partition update");
    (void)kernel.add_command("partition.shift", [](core::Kernel& host, core::CommandArgs args) {
        const auto offset = args.size() == 3 ? vector_argument(args, 0) : std::nullopt;
        if (!offset) {
            return core::CommandResult{false, "usage: partition.shift runtime_offset_x y z"};
        }
        const auto result = host.services().get<WorldPartition>().shift_origin(*offset);
        return core::CommandResult{static_cast<bool>(result), result.message +
            " epoch=" + std::to_string(result.shift.epoch)};
    }, "Subtract an offset from Scene and physics coordinates and publish origin history");
    (void)kernel.add_command("partition.origin", [](core::Kernel& host, core::CommandArgs args) {
        const auto epoch = args.empty() ? std::optional<std::uint64_t>{0} :
            args.size() == 1 ? number<std::uint64_t>(args[0]) : std::nullopt;
        if (!epoch) {
            return core::CommandResult{false, "usage: partition.origin [since_epoch]"};
        }
        const auto history = host.services().get<WorldPartition>().shifts_since(*epoch, 16);
        std::ostringstream text;
        text << "epoch=" << history.latest.epoch << " origin=" << history.latest.absolute_origin.x
            << ',' << history.latest.absolute_origin.y << ',' << history.latest.absolute_origin.z
            << " consistent=" << history.latest.consistent << " history_lost=" << history.history_lost;
        for (const auto& shift : history.shifts) {
            text << '\n' << shift.previous_epoch << "->" << shift.epoch << " offset="
                << shift.offset.x << ',' << shift.offset.y << ',' << shift.offset.z
                << " producer_rejections=" << shift.producer_rejections;
        }
        return core::CommandResult{true, text.str()};
    }, "Inspect coordinate state and replay bounded origin-shift history independently");
    (void)kernel.add_command("partition.state", [](core::Kernel& host, core::CommandArgs args) {
        auto& service = host.services().get<WorldPartition>();
        std::ostringstream text;
        if (args.empty()) {
            const auto state = service.statistics();
            text << "regions=" << state.regions << " wanted=" << state.wanted << " active=" << state.active
                << " loading=" << state.loading << " retiring=" << state.retiring << " faulted=" << state.faulted
                << " region_reservation_bytes=" << state.region_reservations.reserved_bytes
                << " global_stream_job_bytes=" << state.streamer.inflight_reserved_bytes
                << " global_cache_bytes=" << state.cache.reserved_bytes();
        } else {
            const auto identity = args.size() == 1 ? number<std::uint32_t>(args[0]) : std::nullopt;
            const auto state = identity ? service.inspect(*identity) : std::nullopt;
            if (!state) {
                return core::CommandResult{false, "usage: partition.state [existing_region_id]"};
            }
            text << state->id << ' ' << state->name << " state=" << world::state_name(state->policy.state)
                << " generation=" << state->policy.token.generation << " ready_mask=" << state->policy.ready_mask
                << " pins=" << state->pinned_assets << " wanted=" << state->wanted
                << " consumer_invalidated=" << state->consumer_invalidated
                << " waiting_stream_jobs=" << state->waiting_for_stream_jobs << " message=" << state->message;
        }
        return core::CommandResult{true, text.str()};
    }, "Inspect logical region reservations separately from global physical resource accounting");
}

} // namespace ge::scene
