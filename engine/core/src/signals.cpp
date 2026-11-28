#include <ge/core/signals.hpp>
#include <ge/core/runtime.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <deque>
#include <exception>
#include <list>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace ge::core {
namespace {

void increment(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}

std::uint64_t new_scope() {
    static std::atomic<std::uint64_t> next{1};
    auto candidate = next.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("signal hub identity exhausted");
        }
        if (next.compare_exchange_weak(candidate, candidate + 1,
                                       std::memory_order_relaxed)) {
            return candidate;
        }
    }
}

SignalLimits validate_limits(SignalLimits limits) {
    if (limits.max_channels > 65536 || limits.max_subscriptions > 1048576 ||
        limits.max_pending > 1048576 || limits.max_batch_events > 1048576 ||
        limits.max_history > 65536 || limits.max_nested_depth > 1024 ||
        limits.max_name_bytes == 0 || limits.max_name_bytes > 4096 ||
        limits.max_diagnostic_bytes > 4096 || limits.max_flush_events > 1048576 ||
        limits.max_flush_callbacks > 16777216) {
        throw std::invalid_argument("signal limits exceed supported bounds");
    }
    return limits;
}

bool valid_policy(SignalExceptionPolicy policy) noexcept {
    return policy == SignalExceptionPolicy::Continue ||
           policy == SignalExceptionPolicy::DisconnectListener ||
           policy == SignalExceptionPolicy::StopEvent ||
           policy == SignalExceptionPolicy::StopFlush ||
           policy == SignalExceptionPolicy::Rethrow;
}

std::string_view exception_text(const char* text, std::size_t limit) noexcept {
    if (text == nullptr) {
        return {};
    }
    std::size_t length{};
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return {text, length};
}

} // namespace

namespace detail {

struct SignalEnvelope {
    SignalChannelId channel;
    std::type_index type{typeid(void)};
    std::shared_ptr<const void> storage;
    const void* payload{};
    std::size_t bytes{};
    SignalPayloadKind kind{SignalPayloadKind::Owned};
    std::uint64_t sequence{};
    std::uint64_t parent{};
    std::uint32_t depth{};
};

struct SignalListener {
    SignalSubscription token;
    SignalChannelId channel;
    SignalListenerOptions options;
    std::function<void(const void*, const SignalContext&)> callback;
    std::uint64_t order{};
    bool connected{true};
};

struct SignalChannelRecord {
    SignalChannelId id;
    std::string name;
    std::type_index type{typeid(void)};
    std::size_t subscriptions{};
    std::size_t pending{};
};

struct SignalState {
    explicit SignalState(SignalLimits requested)
        : limits(validate_limits(requested)), scope(new_scope()),
          thread(std::this_thread::get_id()), pool(limits.max_subscriptions),
          slots(limits.max_subscriptions), traces(limits.max_history) {
        retired.reserve(limits.max_subscriptions);
        snapshot.reserve(limits.max_subscriptions);
        channels.reserve(limits.max_channels);
    }
    SignalLimits limits;
    std::uint64_t scope;
    std::thread::id thread;
    ge::GenerationPool pool;
    std::vector<std::shared_ptr<SignalListener>> slots;
    std::vector<ge::RuntimeHandle> retired;
    std::vector<ge::RuntimeHandle> snapshot;
    std::vector<SignalChannelRecord> channels;
    std::list<SignalEnvelope> queue;
    std::optional<SignalEnvelope> active;
    std::size_t cursor{};
    std::vector<SignalTrace> traces;
    std::size_t trace_head{};
    std::size_t trace_count{};
    std::uint64_t trace_ordinal{};
    std::uint64_t last_sequence{};
    std::uint64_t last_registration{};
    SignalStatistics totals;
    SignalExceptionPolicy policy{SignalExceptionPolicy::Continue};
    bool running{};
    bool closed{};

    [[nodiscard]] bool owner_thread() const noexcept {
        return thread == std::this_thread::get_id();
    }
    void require_thread() const {
        if (!owner_thread()) {
            throw std::logic_error("SignalHub requires its creating thread");
        }
    }
    [[nodiscard]] SignalChannelRecord* channel(SignalChannelId id) noexcept {
        if (id.scope != scope || id.index == 0 || id.index > channels.size()) {
            return nullptr;
        }
        return &channels[static_cast<std::size_t>(id.index - 1)];
    }
    [[nodiscard]] std::shared_ptr<SignalListener> listener(SignalSubscription token) const noexcept {
        if (token.scope != scope || !pool.alive(token.handle)) {
            return {};
        }
        const auto& entry = slots[static_cast<std::size_t>(token.handle.index - 1)];
        return entry && entry->connected ? entry : nullptr;
    }
    [[nodiscard]] SignalContext context(const SignalEnvelope& event,
                                         const SignalListener* target = nullptr) const noexcept {
        SignalContext result;
        result.channel = event.channel;
        result.sequence = event.sequence;
        result.parent_sequence = event.parent;
        result.depth = event.depth;
        result.payload_kind = event.kind;
        if (target != nullptr) {
            result.subscription = target->token;
            result.owner = target->options.owner;
        }
        return result;
    }
    void trace(SignalTraceKind kind, const SignalEnvelope& event,
               const SignalListener* target = nullptr, std::string_view message = {}) noexcept {
        if (traces.empty()) {
            return;
        }
        if (trace_ordinal == std::numeric_limits<std::uint64_t>::max()) {
            increment(totals.lost_history);
            return;
        }
        try {
            SignalTrace entry;
            entry.ordinal = trace_ordinal + 1;
            entry.kind = kind;
            entry.context = context(event, target);
            entry.accounted_bytes = event.kind == SignalPayloadKind::Borrowed ? 0 : event.bytes;
            entry.message.assign(message.substr(0, limits.max_diagnostic_bytes));
            const auto index = (trace_head + trace_count) % traces.size();
            traces[index] = std::move(entry);
            ++trace_ordinal;
            if (trace_count == traces.size()) {
                trace_head = (trace_head + 1) % traces.size();
                increment(totals.lost_history);
            } else {
                ++trace_count;
            }
        } catch (...) {
            increment(totals.lost_history);
        }
    }
    SignalReceipt reject(SignalStatus status, SignalChannelId id = {}) noexcept {
        increment(totals.rejected);
        SignalEnvelope event;
        event.channel = id;
        trace(SignalTraceKind::Rejected, event, nullptr, signal_status_name(status));
        return {status};
    }
    void reclaim() noexcept {
        while (!retired.empty()) {
            const auto handle = retired.back();
            retired.pop_back();
            auto garbage = std::move(slots[static_cast<std::size_t>(handle.index - 1)]);
            static_cast<void>(pool.release(handle));
        }
    }
    bool disconnect(SignalSubscription token) noexcept {
        if (!owner_thread() || closed) {
            return false;
        }
        auto entry = listener(token);
        if (!entry) {
            return false;
        }
        entry->connected = false;
        --totals.subscriptions;
        --channel(entry->channel)->subscriptions;
        retired.push_back(token.handle);
        if (!active && !running) {
            reclaim();
        }
        return true;
    }
    void prepare_snapshot(SignalChannelId id) noexcept {
        snapshot.clear();
        cursor = 0;
        for (const auto& entry : slots) {
            if (entry && entry->connected && entry->options.enabled && entry->channel == id) {
                snapshot.push_back(entry->token.handle);
            }
        }
        std::sort(snapshot.begin(), snapshot.end(), [this](auto left, auto right) {
            const auto& a = slots[static_cast<std::size_t>(left.index - 1)];
            const auto& b = slots[static_cast<std::size_t>(right.index - 1)];
            if (a->options.priority != b->options.priority) {
                return a->options.priority > b->options.priority;
            }
            return a->order < b->order;
        });
    }
    void uncharge(const SignalEnvelope& event) noexcept {
        --totals.pending;
        --channel(event.channel)->pending;
        if (event.kind != SignalPayloadKind::Borrowed) {
            totals.pending_accounted_bytes -= event.bytes;
        }
    }
    void finish(bool cancelled = false) noexcept {
        if (!active) {
            return;
        }
        auto completed = std::move(*active);
        active.reset();
        snapshot.clear();
        cursor = 0;
        uncharge(completed);
        if (cancelled) {
            increment(totals.cancelled);
            trace(SignalTraceKind::Cancelled, completed);
        } else {
            increment(totals.completed);
            trace(SignalTraceKind::Completed, completed);
        }
        reclaim();
    }
    SignalReceipt admit(std::list<SignalEnvelope>& entries, std::size_t bytes, bool batch) {
        if (!owner_thread()) {
            return {SignalStatus::WrongThread};
        }
        if (closed) {
            return reject(SignalStatus::Closed);
        }
        if (batch && entries.size() > limits.max_batch_events) {
            return reject(SignalStatus::BatchFull);
        }
        if (entries.size() > limits.max_pending - std::min(totals.pending, limits.max_pending)) {
            return reject(SignalStatus::QueueFull);
        }
        if (bytes > limits.max_accounted_bytes - totals.pending_accounted_bytes) {
            return reject(SignalStatus::AccountingLimit);
        }
        if (entries.size() > std::numeric_limits<std::uint64_t>::max() - last_sequence) {
            return reject(SignalStatus::SequenceExhausted);
        }
        const auto depth = running && active ? active->depth + 1U : 0U;
        const auto parent = running && active ? active->sequence : 0;
        if (depth > limits.max_nested_depth) {
            return reject(SignalStatus::NestingLimit);
        }
        std::vector<std::size_t> additions(channels.size(), 0);
        for (const auto& event : entries) {
            auto* record = channel(event.channel);
            if (record == nullptr || record->type != event.type) {
                return reject(SignalStatus::InvalidChannel, event.channel);
            }
            if (!event.payload || !event.storage) {
                return reject(SignalStatus::InvalidPayload, event.channel);
            }
            auto& added = additions[static_cast<std::size_t>(event.channel.index - 1)];
            if (record->pending >= limits.max_pending_per_channel ||
                added >= limits.max_pending_per_channel - record->pending) {
                return reject(SignalStatus::ChannelFull, event.channel);
            }
            ++added;
        }
        const auto count = entries.size();
        const auto first = count == 0 ? 0 : last_sequence + 1;
        for (auto& event : entries) {
            event.sequence = ++last_sequence;
            event.parent = parent;
            event.depth = depth;
            ++channel(event.channel)->pending;
        }
        totals.pending += count;
        totals.pending_accounted_bytes += bytes;
        increment(totals.admitted, static_cast<std::uint64_t>(count));
        if (batch) {
            increment(totals.committed_batches);
        }
        const auto first_added = entries.begin();
        queue.splice(queue.end(), entries);
        for (auto it = first_added; count != 0 && it != queue.end(); ++it) {
            trace(SignalTraceKind::Queued, *it);
        }
        return {SignalStatus::Accepted, count, first};
    }
    std::size_t discard(SignalChannelId filter) noexcept {
        std::list<SignalEnvelope> garbage;
        std::size_t removed{};
        for (auto it = queue.begin(); it != queue.end();) {
            auto current = it++;
            if (!filter || current->channel == filter) {
                uncharge(*current);
                trace(SignalTraceKind::Cancelled, *current);
                garbage.splice(garbage.end(), queue, current);
                ++removed;
            }
        }
        increment(totals.cancelled, static_cast<std::uint64_t>(removed));
        return removed;
    }
    void close() noexcept {
        if (closed) {
            return;
        }
        for (const auto& entry : slots) {
            if (entry && entry->connected) {
                entry->connected = false;
                retired.push_back(entry->token.handle);
            }
        }
        closed = true;
        totals.subscriptions = 0;
        for (auto& record : channels) {
            record.subscriptions = 0;
        }
        static_cast<void>(discard({}));
        if (!running) {
            finish(true);
            reclaim();
        }
    }
};

struct SignalBatchState {
    explicit SignalBatchState(std::weak_ptr<SignalState> value) : hub(std::move(value)) {}
    std::weak_ptr<SignalState> hub;
    std::list<SignalEnvelope> entries;
    std::size_t bytes{};
    bool committed{};
};

struct RunningGuard {
    explicit RunningGuard(SignalState& value) : state(value) {
        state.running = true;
    }
    ~RunningGuard() {
        if (state.closed) {
            state.finish(true);
        }
        if (!state.active) {
            state.reclaim();
        }
        state.running = false;
    }
    SignalState& state;
};

SignalDispatchReport dispatch(SignalState& state, std::size_t events, std::size_t callbacks) {
    SignalDispatchReport report;
    if (state.running) {
        report.busy = true;
        report.remaining = state.totals.pending;
        return report;
    }
    // Deferred captures can enqueue from their destructors. Finish the guard's
    // cleanup before sampling remaining work and exhaustion below.
    {
        RunningGuard guard(state);
        while (!state.closed && report.completed < events) {
            if (!state.active) {
                if (state.queue.empty()) {
                    break;
                }
                state.prepare_snapshot(state.queue.front().channel);
                state.active.emplace(std::move(state.queue.front()));
                state.queue.pop_front();
                state.trace(SignalTraceKind::Started, *state.active);
                ++report.started;
            }
            if (state.cursor == state.snapshot.size()) {
                state.finish();
                ++report.completed;
                continue;
            }
            auto entry = state.listener({state.scope, state.snapshot[state.cursor]});
            if (!entry || !entry->options.enabled) {
                ++state.cursor;
                continue;
            }
            if (report.callbacks == callbacks) {
                break;
            }
            ++state.cursor;
            const auto context = state.context(*state.active, entry.get());
            const auto policy = entry->options.exceptions.value_or(state.policy);
            if (entry->options.once) {
                static_cast<void>(state.disconnect(entry->token));
            }
            ++report.callbacks;
            increment(state.totals.callbacks);
            state.trace(SignalTraceKind::Callback, *state.active, entry.get(), entry->options.label);
            std::exception_ptr failure;
            try {
                entry->callback(state.active->payload, context);
            } catch (const std::exception& exception) {
                state.trace(SignalTraceKind::Exception, *state.active, entry.get(),
                            exception_text(exception.what(), state.limits.max_diagnostic_bytes));
                failure = std::current_exception();
            } catch (...) {
                state.trace(SignalTraceKind::Exception, *state.active, entry.get(), "non-standard exception");
                failure = std::current_exception();
            }
            if (failure) {
                ++report.exceptions;
                increment(state.totals.exceptions);
                if (policy == SignalExceptionPolicy::DisconnectListener) {
                    static_cast<void>(state.disconnect(entry->token));
                } else if (policy == SignalExceptionPolicy::StopEvent) {
                    state.cursor = state.snapshot.size();
                } else if (policy == SignalExceptionPolicy::StopFlush) {
                    report.stopped = true;
                } else if (policy == SignalExceptionPolicy::Rethrow) {
                    if (state.cursor == state.snapshot.size()) {
                        state.finish(state.closed);
                    }
                    std::rethrow_exception(failure);
                }
            }
            if (state.cursor == state.snapshot.size() || state.closed) {
                state.finish(state.closed);
                ++report.completed;
            }
            if (report.stopped) {
                break;
            }
        }
    }
    report.remaining = state.totals.pending;
    report.budget_exhausted = report.remaining != 0 && !report.stopped && !state.closed &&
        (report.completed == events || report.callbacks == callbacks);
    return report;
}

} // namespace detail

SignalConnection::SignalConnection(std::weak_ptr<detail::SignalState> state,
                                   SignalSubscription subscription) noexcept
    : state_(std::move(state)), subscription_(subscription) {}

SignalConnection::~SignalConnection() {
    static_cast<void>(disconnect());
}

SignalConnection::SignalConnection(SignalConnection&& other) noexcept
    : state_(std::move(other.state_)), subscription_(std::exchange(other.subscription_, {})) {}

SignalConnection& SignalConnection::operator=(SignalConnection&& other) noexcept {
    if (this != &other) {
        static_cast<void>(disconnect());
        state_ = std::move(other.state_);
        subscription_ = std::exchange(other.subscription_, {});
    }
    return *this;
}

bool SignalConnection::connected() const noexcept {
    const auto state = state_.lock();
    return state && state->owner_thread() && !state->closed &&
           static_cast<bool>(state->listener(subscription_));
}

bool SignalConnection::disconnect() noexcept {
    const auto state = state_.lock();
    if (state && !state->owner_thread()) {
        return false;
    }
    const auto token = std::exchange(subscription_, {});
    state_.reset();
    return state && state->disconnect(token);
}

SignalSubscription SignalConnection::token() const noexcept {
    return subscription_;
}

SignalSubscription SignalConnection::release() noexcept {
    state_.reset();
    return std::exchange(subscription_, {});
}

SignalBatch::SignalBatch() noexcept = default;
SignalBatch::~SignalBatch() = default;
SignalBatch::SignalBatch(SignalBatch&&) noexcept = default;
SignalBatch& SignalBatch::operator=(SignalBatch&&) noexcept = default;
SignalBatch::SignalBatch(std::weak_ptr<detail::SignalState> state)
    : state_(std::make_unique<detail::SignalBatchState>(std::move(state))) {}

SignalReceipt SignalBatch::add_erased(SignalChannelId channel, std::type_index type,
    std::shared_ptr<const void> payload, std::size_t bytes, SignalPayloadKind kind) {
    const auto hub = state_ ? state_->hub.lock() : nullptr;
    if (!hub) {
        return {SignalStatus::Closed};
    }
    if (!hub->owner_thread()) {
        return {SignalStatus::WrongThread};
    }
    if (state_->committed) {
        return hub->reject(SignalStatus::AlreadyCommitted, channel);
    }
    if (hub->closed) {
        return hub->reject(SignalStatus::Closed, channel);
    }
    const auto* record = hub->channel(channel);
    if (record == nullptr || record->type != type) {
        return hub->reject(SignalStatus::InvalidChannel, channel);
    }
    if (state_->entries.size() >= hub->limits.max_batch_events) {
        return hub->reject(SignalStatus::BatchFull, channel);
    }
    if (bytes > hub->limits.max_accounted_bytes - state_->bytes) {
        return hub->reject(SignalStatus::AccountingLimit, channel);
    }
    const auto* address = payload.get();
    state_->entries.push_back({channel, type, std::move(payload), address, bytes, kind});
    state_->bytes += bytes;
    return {SignalStatus::Accepted, 1, 0};
}

SignalReceipt SignalBatch::commit() {
    const auto hub = state_ ? state_->hub.lock() : nullptr;
    if (!hub) {
        return {SignalStatus::Closed};
    }
    if (!hub->owner_thread()) {
        return {SignalStatus::WrongThread};
    }
    if (state_->committed) {
        return hub->reject(SignalStatus::AlreadyCommitted);
    }
    auto receipt = hub->admit(state_->entries, state_->bytes, true);
    if (receipt) {
        state_->bytes = 0;
        state_->committed = true;
    }
    return receipt;
}

std::size_t SignalBatch::size() const noexcept {
    return state_ ? state_->entries.size() : 0;
}

std::size_t SignalBatch::accounted_bytes() const noexcept {
    return state_ ? state_->bytes : 0;
}

void SignalBatch::clear() noexcept {
    if (state_ && !state_->committed) {
        std::list<detail::SignalEnvelope> garbage;
        garbage.swap(state_->entries);
        state_->bytes = 0;
    }
}

SignalHub::SignalHub(SignalLimits limits) : state_(std::make_shared<detail::SignalState>(limits)) {}

SignalHub::~SignalHub() {
    close();
}

SignalChannelId SignalHub::channel_erased(std::string_view name, std::type_index type) {
    const auto state = state_;
    state->require_thread();
    if (state->closed) {
        return {};
    }
    if (name.size() > state->limits.max_name_bytes || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid signal channel name");
    }
    for (const auto& record : state->channels) {
        if (record.name == name && (!name.empty() || record.type == type)) {
            if (record.type != type) {
                throw std::invalid_argument("signal channel name already has a different payload type");
            }
            return record.id;
        }
    }
    if (state->channels.size() == state->limits.max_channels) {
        return {};
    }
    const SignalChannelId id{state->scope, static_cast<std::uint32_t>(state->channels.size() + 1)};
    state->channels.push_back({id, std::string(name), type, 0, 0});
    return id;
}

SignalSubscription SignalHub::subscribe_erased(SignalChannelId channel, std::type_index type,
    std::function<void(const void*, const SignalContext&)> function, SignalListenerOptions options) {
    const auto state = state_;
    state->require_thread();
    const auto* record = state->channel(channel);
    if (state->closed || record == nullptr || record->type != type || !function ||
        options.label.size() > state->limits.max_name_bytes ||
        (options.exceptions && !valid_policy(*options.exceptions)) ||
        state->last_registration == std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }
    auto entry = std::make_shared<detail::SignalListener>();
    entry->channel = channel;
    entry->options = std::move(options);
    entry->callback = std::move(function);
    const auto handle = state->pool.acquire();
    if (!handle) {
        return {};
    }
    entry->token = {state->scope, *handle};
    entry->order = ++state->last_registration;
    state->slots[static_cast<std::size_t>(handle->index - 1)] = entry;
    ++state->totals.subscriptions;
    ++state->channel(channel)->subscriptions;
    return entry->token;
}

SignalReceipt SignalHub::enqueue_erased(SignalChannelId channel, std::type_index type,
    std::shared_ptr<const void> payload, std::size_t bytes, SignalPayloadKind kind) {
    const auto state = state_;
    std::list<detail::SignalEnvelope> entries;
    const auto* address = payload.get();
    entries.push_back({channel, type, std::move(payload), address, bytes, kind});
    return state->admit(entries, bytes, false);
}

SignalReceipt SignalHub::borrowed_erased(SignalChannelId channel, std::type_index type,
                                       const void* payload, std::size_t bytes) {
    const auto state = state_;
    if (!state->owner_thread()) {
        return {SignalStatus::WrongThread};
    }
    if (state->closed) {
        return state->reject(SignalStatus::Closed, channel);
    }
    const auto* record = state->channel(channel);
    if (record == nullptr || record->type != type) {
        return state->reject(SignalStatus::InvalidChannel, channel);
    }
    if (state->running || state->totals.pending != 0) {
        return state->reject(SignalStatus::Busy, channel);
    }
    if (state->last_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return state->reject(SignalStatus::SequenceExhausted, channel);
    }
    const auto sequence = ++state->last_sequence;
    state->prepare_snapshot(channel);
    state->active = detail::SignalEnvelope{channel, type, {}, payload, bytes,
                                         SignalPayloadKind::Borrowed, sequence, 0, 0};
    ++state->totals.pending;
    ++state->channel(channel)->pending;
    increment(state->totals.admitted);
    state->trace(SignalTraceKind::Started, *state->active);
    try {
        static_cast<void>(detail::dispatch(*state, 1, state->limits.max_subscriptions));
    } catch (...) {
        state->finish(true);
        throw;
    }
    if (state->active) {
        state->finish(true);
    }
    return {SignalStatus::Accepted, 1, sequence};
}

SignalBatch SignalHub::batch() {
    state_->require_thread();
    return SignalBatch(state_);
}

SignalConnection SignalHub::connection(SignalSubscription subscription) {
    state_->require_thread();
    return connected(subscription) ? SignalConnection(state_, subscription) : SignalConnection{};
}

bool SignalHub::connected(SignalSubscription subscription) const noexcept {
    return state_->owner_thread() && !state_->closed &&
           static_cast<bool>(state_->listener(subscription));
}

bool SignalHub::disconnect(SignalSubscription subscription) noexcept {
    const auto state = state_;
    return state->disconnect(subscription);
}

std::size_t SignalHub::disconnect_owner(ge::PersistentId owner) noexcept {
    const auto state = state_;
    if (!state->owner_thread() || state->closed || !owner) {
        return 0;
    }
    std::size_t removed{};
    for (const auto& entry : state->slots) {
        if (entry && entry->connected && entry->options.owner == owner) {
            removed += state->disconnect(entry->token) ? 1U : 0U;
        }
    }
    return removed;
}

bool SignalHub::set_enabled(SignalSubscription subscription, bool enabled) noexcept {
    if (!state_->owner_thread() || state_->closed) {
        return false;
    }
    const auto entry = state_->listener(subscription);
    if (!entry) {
        return false;
    }
    entry->options.enabled = enabled;
    return true;
}

bool SignalHub::set_priority(SignalSubscription subscription, int priority) noexcept {
    if (!state_->owner_thread() || state_->closed) {
        return false;
    }
    const auto entry = state_->listener(subscription);
    if (!entry) {
        return false;
    }
    entry->options.priority = priority;
    return true;
}

SignalDispatchReport SignalHub::flush(std::size_t event_limit, std::size_t callback_limit) {
    const auto state = state_;
    state->require_thread();
    return detail::dispatch(*state, std::min(event_limit, state->limits.max_flush_events),
                            std::min(callback_limit, state->limits.max_flush_callbacks));
}

std::size_t SignalHub::discard_pending(SignalChannelId channel) {
    const auto state = state_;
    state->require_thread();
    if (state->running || (channel != SignalChannelId{} && state->channel(channel) == nullptr)) {
        return 0;
    }
    std::size_t removed{};
    if (state->active && (!channel || state->active->channel == channel)) {
        state->finish(true);
        ++removed;
    }
    return removed + state->discard(channel);
}

void SignalHub::close() noexcept {
    const auto state = state_;
    state->close();
}

void SignalHub::set_exception_policy(SignalExceptionPolicy policy) {
    state_->require_thread();
    if (!valid_policy(policy)) {
        throw std::invalid_argument("invalid signal exception policy");
    }
    state_->policy = policy;
}

SignalExceptionPolicy SignalHub::exception_policy() const {
    state_->require_thread();
    return state_->policy;
}

SignalLimits SignalHub::limits() const {
    state_->require_thread();
    return state_->limits;
}

SignalStatistics SignalHub::statistics() const {
    state_->require_thread();
    auto result = state_->totals;
    result.channels = state_->channels.size();
    result.dispatching = state_->running;
    result.closed = state_->closed;
    return result;
}

std::vector<SignalChannelInfo> SignalHub::channels() const {
    state_->require_thread();
    std::vector<SignalChannelInfo> result;
    result.reserve(state_->channels.size());
    for (const auto& channel : state_->channels) {
        result.push_back({channel.id, channel.name, channel.type.name(),
                          channel.subscriptions, channel.pending});
    }
    return result;
}

std::vector<SignalListenerInfo> SignalHub::listeners() const {
    state_->require_thread();
    std::vector<SignalListenerInfo> result;
    result.reserve(state_->totals.subscriptions);
    for (const auto& entry : state_->slots) {
        if (entry && entry->connected) {
            result.push_back({entry->token, entry->channel, entry->options, entry->order});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.registration_order < right.registration_order;
    });
    return result;
}

std::vector<SignalTrace> SignalHub::history(std::size_t limit) const {
    state_->require_thread();
    const auto count = std::min(limit, state_->trace_count);
    std::vector<SignalTrace> result;
    result.reserve(count);
    for (std::size_t i = state_->trace_count - count; i < state_->trace_count; ++i) {
        result.push_back(state_->traces[(state_->trace_head + i) % state_->traces.size()]);
    }
    return result;
}

void SignalHub::clear_history() {
    state_->require_thread();
    for (auto& entry : state_->traces) {
        entry.message.clear();
    }
    state_->trace_head = 0;
    state_->trace_count = 0;
}

std::uint64_t SignalHub::scope() const noexcept {
    return state_->scope;
}

std::string_view signal_status_name(SignalStatus status) noexcept {
    switch (status) {
    case SignalStatus::Accepted: return "accepted";
    case SignalStatus::Closed: return "closed";
    case SignalStatus::WrongThread: return "wrong thread";
    case SignalStatus::InvalidChannel: return "invalid channel";
    case SignalStatus::InvalidPayload: return "invalid payload";
    case SignalStatus::QueueFull: return "queue full";
    case SignalStatus::ChannelFull: return "channel full";
    case SignalStatus::BatchFull: return "batch full";
    case SignalStatus::AccountingLimit: return "accounting limit";
    case SignalStatus::NestingLimit: return "nesting limit";
    case SignalStatus::SequenceExhausted: return "sequence exhausted";
    case SignalStatus::Busy: return "busy";
    case SignalStatus::AlreadyCommitted: return "already committed";
    }
    return "unknown";
}

std::string_view signal_trace_name(SignalTraceKind kind) noexcept {
    switch (kind) {
    case SignalTraceKind::Queued: return "queued";
    case SignalTraceKind::Started: return "started";
    case SignalTraceKind::Callback: return "callback";
    case SignalTraceKind::Exception: return "exception";
    case SignalTraceKind::Completed: return "completed";
    case SignalTraceKind::Cancelled: return "cancelled";
    case SignalTraceKind::Rejected: return "rejected";
    }
    return "unknown";
}

namespace {

struct ConsoleSignalObservation {
    SignalContext context;
    std::string text;
};

struct ConsoleSignalLog {
    std::deque<ConsoleSignalObservation> observations;
};

struct ConsoleSignalService {
    std::shared_ptr<ConsoleSignalLog> log{std::make_shared<ConsoleSignalLog>()};
};

template<class T>
bool parse_signal_number(std::string_view text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

CommandResult receipt_result(SignalReceipt receipt) {
    return {static_cast<bool>(receipt), std::string(signal_status_name(receipt.status)) +
        " count=" + std::to_string(receipt.count) + " first=" + std::to_string(receipt.first_sequence)};
}

CommandResult dispatch_result(SignalDispatchReport report) {
    return {!report.busy, "completed=" + std::to_string(report.completed) +
        " callbacks=" + std::to_string(report.callbacks) + " exceptions=" +
        std::to_string(report.exceptions) + " pending=" + std::to_string(report.remaining) +
        (report.stopped ? " stopped" : "") + (report.budget_exhausted ? " budget reached" : "")};
}

CommandResult send_console_signal(Kernel& kernel, CommandArgs args, bool immediate) {
    if (args.size() != 2 || args[1].size() > 4096) {
        return {false, "usage: signals.emit|queue name text (text <= 4096 bytes)"};
    }
    auto& hub = kernel.services().get<SignalHub>();
    const auto signal = hub.channel<SignalMessage>(args[0]);
    const auto bytes = args[1].size();
    return receipt_result(immediate ? hub.emit(signal, SignalMessage{args[1]}, bytes)
                                    : hub.enqueue(signal, SignalMessage{args[1]}, bytes));
}

} // namespace

void register_signals(Kernel& kernel) {
    if (!kernel.register_module({"core.signals", "Typed transactional multicast signals", 1})) {
        return;
    }
    auto& hub = kernel.services().emplace<SignalHub>();
    static_cast<void>(kernel.services().emplace<ConsoleSignalService>());
    kernel.add_command("signals.status", [](Kernel& host, CommandArgs) {
        const auto stats = host.services().get<SignalHub>().statistics();
        return CommandResult{true, "channels=" + std::to_string(stats.channels) +
            " listeners=" + std::to_string(stats.subscriptions) + " pending=" +
            std::to_string(stats.pending) + " accounted_bytes=" +
            std::to_string(stats.pending_accounted_bytes) + " admitted=" +
            std::to_string(stats.admitted) + " callbacks=" + std::to_string(stats.callbacks) +
            " exceptions=" + std::to_string(stats.exceptions) + " rejected=" +
            std::to_string(stats.rejected) + (stats.closed ? " closed" : "")};
    }, "Report actual multicast subscriptions, queue accounting and dispatch totals");
    kernel.add_command("signals.channels", [](Kernel& host, CommandArgs) {
        std::ostringstream output;
        std::size_t shown{};
        for (const auto& channel : host.services().get<SignalHub>().channels()) {
            if (shown++ == 256) {
                break;
            }
            output << channel.id.index << ' ' << channel.name << " type=" << channel.payload_type
                   << " listeners=" << channel.subscriptions << " pending=" << channel.pending << '\n';
        }
        return CommandResult{true, output.str()};
    }, "List up to 256 actual typed signal channels");
    kernel.add_command("signals.listen", [](Kernel& host, CommandArgs args) {
        SignalListenerOptions options;
        if (args.empty() || args.size() > 3 ||
            (args.size() >= 2 && !parse_signal_number(args[1], options.priority)) ||
            (args.size() == 3 && args[2] != "once")) {
            return CommandResult{false, "usage: signals.listen name [priority] [once]"};
        }
        auto& signals = host.services().get<SignalHub>();
        options.once = args.size() == 3;
        options.label = "console";
        const auto signal = signals.channel<SignalMessage>(args[0]);
        const auto weak = std::weak_ptr<ConsoleSignalLog>(host.services().get<ConsoleSignalService>().log);
        const auto token = signals.subscribe(signal,
            [weak](const SignalMessage& message, const SignalContext& context) {
                if (const auto log = weak.lock()) {
                    if (log->observations.size() == 128) {
                        log->observations.pop_front();
                    }
                    log->observations.push_back({context, message.text.substr(0, 4096)});
                }
            }, std::move(options));
        return CommandResult{static_cast<bool>(token), "subscription=" +
            std::to_string(token.handle.index) + " generation=" + std::to_string(token.handle.generation)};
    }, "Subscribe a retained console observer; every listener records its own delivery");
    kernel.add_command("signals.unlisten", [](Kernel& host, CommandArgs args) {
        ge::RuntimeHandle handle;
        if (args.size() != 2 || !parse_signal_number(args[0], handle.index) ||
            !parse_signal_number(args[1], handle.generation)) {
            return CommandResult{false, "usage: signals.unlisten index generation"};
        }
        auto& signals = host.services().get<SignalHub>();
        const bool removed = signals.disconnect({signals.scope(), handle});
        return CommandResult{removed, removed ? "disconnected" : "stale or unknown subscription"};
    }, "Disconnect a generation-checked subscription");
    kernel.add_command("signals.emit", [](Kernel& host, CommandArgs args) {
        return send_console_signal(host, args, true);
    }, "Enqueue a named text signal and perform a bounded dispatch");
    kernel.add_command("signals.queue", [](Kernel& host, CommandArgs args) {
        return send_console_signal(host, args, false);
    }, "Enqueue a named text signal for scheduled dispatch");
    kernel.add_command("signals.batch", [](Kernel& host, CommandArgs args) {
        if (args.size() < 2 || args.size() > 65) {
            return CommandResult{false, "usage: signals.batch name text [text ...] (maximum 64)"};
        }
        auto& signals = host.services().get<SignalHub>();
        const auto signal = signals.channel<SignalMessage>(args[0]);
        auto batch = signals.batch();
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i].size() > 4096) {
                return CommandResult{false, "batch text exceeds 4096 bytes; no events committed"};
            }
            const auto staged = batch.add(signal, SignalMessage{args[i]}, args[i].size());
            if (!staged) {
                return receipt_result(staged);
            }
        }
        return receipt_result(batch.commit());
    }, "Atomically admit up to 64 text events without running any listeners");
    kernel.add_command("signals.flush", [](Kernel& host, CommandArgs args) {
        std::size_t events = 1024;
        std::size_t callbacks = 65536;
        if (args.size() > 2 || (!args.empty() && !parse_signal_number(args[0], events)) ||
            (args.size() == 2 && !parse_signal_number(args[1], callbacks))) {
            return CommandResult{false, "usage: signals.flush [events] [callbacks]"};
        }
        return dispatch_result(host.services().get<SignalHub>().flush(events, callbacks));
    }, "Resume ordered dispatch within event and callback budgets");
    kernel.add_command("signals.trace", [](Kernel& host, CommandArgs args) {
        std::size_t limit = 32;
        if (args.size() > 1 || (!args.empty() && !parse_signal_number(args[0], limit))) {
            return CommandResult{false, "usage: signals.trace [limit]"};
        }
        std::ostringstream output;
        for (const auto& event : host.services().get<SignalHub>().history(std::min(limit, std::size_t{256}))) {
            output << event.ordinal << ' ' << signal_trace_name(event.kind) << " event="
                   << event.context.sequence << " channel=" << event.context.channel.index
                   << " listener=" << event.context.subscription.handle.index
                   << " parent=" << event.context.parent_sequence << " depth=" << event.context.depth
                   << ' ' << event.message << '\n';
        }
        return CommandResult{true, output.str()};
    }, "Read the bounded admission, callback, exception and completion history");
    kernel.add_command("signals.messages", [](Kernel& host, CommandArgs) {
        std::ostringstream output;
        for (const auto& entry : host.services().get<ConsoleSignalService>().log->observations) {
            output << entry.context.sequence << '/' << entry.context.subscription.handle.index
                   << ':' << entry.context.subscription.handle.generation << ' ' << entry.text << '\n';
        }
        return CommandResult{true, output.str()};
    }, "Read the last 128 actual console listener deliveries");
    kernel.add_command("signals.clear", [](Kernel& host, CommandArgs) {
        const auto count = host.services().get<SignalHub>().discard_pending();
        return CommandResult{true, "cancelled=" + std::to_string(count)};
    }, "Cancel pending events while dispatch is idle");
    const bool scheduled = kernel.add_system({"core.signals.flush", Phase::EndFrame, 9000, {}},
        [](Kernel& host, const Frame&) {
            auto& signals = host.services().get<SignalHub>();
            const auto limits = signals.limits();
            const auto events = std::clamp(host.config().integer("signals.flush_events", 1024),
                std::int64_t{0}, static_cast<std::int64_t>(limits.max_flush_events));
            const auto callbacks = std::clamp(host.config().integer("signals.flush_callbacks", 65536),
                std::int64_t{0}, static_cast<std::int64_t>(limits.max_flush_callbacks));
            const auto report = signals.flush(static_cast<std::size_t>(events),
                                             static_cast<std::size_t>(callbacks));
            const auto stats = signals.statistics();
            host.metric("signals.pending", static_cast<double>(stats.pending));
            host.metric("signals.callbacks", static_cast<double>(stats.callbacks));
            host.metric("signals.exceptions", static_cast<double>(stats.exceptions));
            host.metric("signals.accounted_bytes", static_cast<double>(stats.pending_accounted_bytes));
            if (report.exceptions != 0) {
                host.diagnose(Diagnostic::Severity::Warning, "core.signals",
                    std::to_string(report.exceptions) + " listener failures; inspect signals.trace");
            }
        });
    if (!scheduled || !kernel.add_shutdown("core.signals.close", [pointer = &hub] { pointer->close(); })) {
        throw std::runtime_error("could not install signal dispatch lifecycle");
    }
}

} // namespace ge::core
