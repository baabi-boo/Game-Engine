#pragma once

#include <ge/core/identity.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace ge::core {

class Kernel;
class SignalHub;
namespace detail {
struct SignalState;
struct SignalBatchState;
}

struct SignalChannelId {
    std::uint64_t scope{};
    std::uint32_t index{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return scope != 0 && index != 0;
    }
    auto operator<=>(const SignalChannelId&) const = default;
};

template<class T>
struct Signal {
    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);
    SignalChannelId id;
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(id);
    }
    auto operator<=>(const Signal&) const = default;
};

struct SignalSubscription {
    std::uint64_t scope{};
    ge::RuntimeHandle handle;
    [[nodiscard]] explicit operator bool() const noexcept {
        return scope != 0 && handle.valid();
    }
    auto operator<=>(const SignalSubscription&) const = default;
};

enum class SignalStatus : std::uint8_t {
    Accepted, Closed, WrongThread, InvalidChannel, InvalidPayload,
    QueueFull, ChannelFull, BatchFull, AccountingLimit, NestingLimit,
    SequenceExhausted, Busy, AlreadyCommitted
};

enum class SignalExceptionPolicy : std::uint8_t {
    Continue, DisconnectListener, StopEvent, StopFlush, Rethrow
};

enum class SignalPayloadKind : std::uint8_t { Owned, Shared, Borrowed };
enum class SignalTraceKind : std::uint8_t {
    Queued, Started, Callback, Exception, Completed, Cancelled, Rejected
};

struct SignalReceipt {
    SignalStatus status{SignalStatus::Accepted};
    std::size_t count{};
    std::uint64_t first_sequence{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SignalStatus::Accepted;
    }
};

struct SignalLimits {
    std::size_t max_channels{256};
    std::uint32_t max_subscriptions{4096};
    std::size_t max_pending{8192};
    std::size_t max_pending_per_channel{2048};
    std::size_t max_batch_events{1024};
    std::size_t max_accounted_bytes{16U * 1024U * 1024U};
    std::uint32_t max_nested_depth{32};
    std::size_t max_flush_events{1024};
    std::size_t max_flush_callbacks{65536};
    std::size_t max_history{1024};
    std::size_t max_name_bytes{128};
    std::size_t max_diagnostic_bytes{256};
};

struct SignalListenerOptions {
    int priority{};
    ge::PersistentId owner;
    bool once{};
    bool enabled{true};
    std::optional<SignalExceptionPolicy> exceptions;
    std::string label;
};

struct SignalContext {
    SignalChannelId channel;
    SignalSubscription subscription;
    ge::PersistentId owner;
    std::uint64_t sequence{};
    std::uint64_t parent_sequence{};
    std::uint32_t depth{};
    SignalPayloadKind payload_kind{SignalPayloadKind::Owned};
};

struct SignalDispatchReport {
    std::size_t started{};
    std::size_t completed{};
    std::size_t callbacks{};
    std::size_t exceptions{};
    std::size_t remaining{};
    bool busy{};
    bool budget_exhausted{};
    bool stopped{};
};

struct SignalStatistics {
    std::size_t channels{};
    std::size_t subscriptions{};
    std::size_t pending{};
    std::size_t pending_accounted_bytes{};
    std::uint64_t admitted{};
    std::uint64_t completed{};
    std::uint64_t callbacks{};
    std::uint64_t exceptions{};
    std::uint64_t rejected{};
    std::uint64_t cancelled{};
    std::uint64_t committed_batches{};
    std::uint64_t lost_history{};
    bool dispatching{};
    bool closed{};
};

struct SignalChannelInfo {
    SignalChannelId id;
    std::string name;
    std::string payload_type;
    std::size_t subscriptions{};
    std::size_t pending{};
};

struct SignalListenerInfo {
    SignalSubscription subscription;
    SignalChannelId channel;
    SignalListenerOptions options;
    std::uint64_t registration_order{};
};

struct SignalTrace {
    std::uint64_t ordinal{};
    SignalTraceKind kind{SignalTraceKind::Queued};
    SignalContext context;
    std::size_t accounted_bytes{};
    std::string message;
};

// Connections hold a weak reference, never a Kernel or a borrowed hub pointer.
// All operations, including connection destruction, belong to the creating
// thread. After hub destruction disconnect is a harmless false result.
class SignalConnection final {
public:
    SignalConnection() noexcept = default;
    ~SignalConnection();
    SignalConnection(SignalConnection&& other) noexcept;
    SignalConnection& operator=(SignalConnection&& other) noexcept;
    SignalConnection(const SignalConnection&) = delete;
    SignalConnection& operator=(const SignalConnection&) = delete;
    [[nodiscard]] bool connected() const noexcept;
    bool disconnect() noexcept;
    [[nodiscard]] SignalSubscription token() const noexcept;
    [[nodiscard]] SignalSubscription release() noexcept;
private:
    friend class SignalHub;
    SignalConnection(std::weak_ptr<detail::SignalState> state,
                     SignalSubscription subscription) noexcept;
    std::weak_ptr<detail::SignalState> state_;
    SignalSubscription subscription_;
};

// Staging owns payloads but does not reserve queue capacity. Commit validates
// the complete batch and splices it into the queue without invoking listeners.
// A rejected commit retains its staged payloads and can be retried. Allocation
// failures propagate; existing queued events and batch entries remain intact.
class SignalBatch final {
public:
    SignalBatch() noexcept;
    ~SignalBatch();
    SignalBatch(SignalBatch&&) noexcept;
    SignalBatch& operator=(SignalBatch&&) noexcept;
    SignalBatch(const SignalBatch&) = delete;
    SignalBatch& operator=(const SignalBatch&) = delete;
    template<class T>
    SignalReceipt add(Signal<T> signal, T value, std::size_t dynamic_bytes = 0) {
        if (dynamic_bytes > std::numeric_limits<std::size_t>::max() - sizeof(T)) {
            return {SignalStatus::AccountingLimit};
        }
        return add_erased(signal.id, typeid(T), std::make_shared<T>(std::move(value)),
                          sizeof(T) + dynamic_bytes, SignalPayloadKind::Owned);
    }
    template<class T>
    SignalReceipt add_shared(Signal<T> signal, std::type_identity_t<std::shared_ptr<const T>> value,
                             std::size_t dynamic_bytes = 0) {
        if (!value) {
            return {SignalStatus::InvalidPayload};
        }
        if (dynamic_bytes > std::numeric_limits<std::size_t>::max() - sizeof(T)) {
            return {SignalStatus::AccountingLimit};
        }
        return add_erased(signal.id, typeid(T), std::move(value),
                          sizeof(T) + dynamic_bytes, SignalPayloadKind::Shared);
    }
    [[nodiscard]] SignalReceipt commit();
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t accounted_bytes() const noexcept;
    void clear() noexcept;
private:
    friend class SignalHub;
    explicit SignalBatch(std::weak_ptr<detail::SignalState> state);
    SignalReceipt add_erased(SignalChannelId channel, std::type_index type,
                            std::shared_ptr<const void> payload, std::size_t bytes,
                            SignalPayloadKind kind);
    std::unique_ptr<detail::SignalBatchState> state_;
};

// Single-writer multicast dispatcher, independent of EventBus consumption.
// All accesses require the constructing thread. User allocation failures can
// propagate. Name/type mismatches throw invalid_argument. Channels never retire.
// Higher priorities run first, with registration order breaking ties. Each
// event snapshots eligible listeners: new/enabled listeners and priority edits
// take effect on the next event; disconnect suppresses a pending invocation.
// Owned nested emissions queue FIFO, with an explicit causal depth limit.
// Byte limits account sizeof(T) plus caller-declared dynamic bytes per envelope;
// they do not bound arbitrary heap graphs, shared storage, or callback captures.
// In-flight owned/shared events retain their queue charge until completion.
// Shared payloads must remain immutable while admitted. Each staged batch has
// its own size bound; caller-held batches do not reserve the pending queue.
class SignalHub final {
public:
    explicit SignalHub(SignalLimits limits = {});
    ~SignalHub();
    SignalHub(const SignalHub&) = delete;
    SignalHub& operator=(const SignalHub&) = delete;
    SignalHub(SignalHub&&) = delete;
    SignalHub& operator=(SignalHub&&) = delete;
    template<class T>
    [[nodiscard]] Signal<T> channel(std::string_view name = {}) {
        return {channel_erased(name, typeid(T))};
    }
    template<class T, class F>
    [[nodiscard]] SignalSubscription subscribe(Signal<T> signal, F&& function,
                                               SignalListenerOptions options = {}) {
        using Callable = std::decay_t<F>;
        static_assert(std::is_invocable_v<Callable&, const T&, const SignalContext&> ||
                      std::is_invocable_v<Callable&, const T&>);
        if constexpr (std::is_pointer_v<Callable> || std::is_member_pointer_v<Callable>) {
            if (function == nullptr) {
                return {};
            }
        }
        auto callable = std::make_shared<Callable>(std::forward<F>(function));
        auto erased = [callable = std::move(callable)](const void* payload,
                                                     const SignalContext& context) {
            const auto& value = *static_cast<const T*>(payload);
            if constexpr (std::is_invocable_v<Callable&, const T&, const SignalContext&>) {
                std::invoke(*callable, value, context);
            } else {
                std::invoke(*callable, value);
            }
        };
        return subscribe_erased(signal.id, typeid(T), std::move(erased), std::move(options));
    }
    template<class T, class F>
    [[nodiscard]] SignalConnection connect(Signal<T> signal, F&& function,
                                           SignalListenerOptions options = {}) {
        return connection(subscribe(signal, std::forward<F>(function), std::move(options)));
    }
    template<class T>
    SignalReceipt enqueue(Signal<T> signal, T value, std::size_t dynamic_bytes = 0) {
        if (dynamic_bytes > std::numeric_limits<std::size_t>::max() - sizeof(T)) {
            return {SignalStatus::AccountingLimit};
        }
        return enqueue_erased(signal.id, typeid(T), std::make_shared<T>(std::move(value)),
                              sizeof(T) + dynamic_bytes, SignalPayloadKind::Owned);
    }
    template<class T>
    SignalReceipt enqueue_shared(Signal<T> signal, std::type_identity_t<std::shared_ptr<const T>> value,
                                 std::size_t dynamic_bytes = 0) {
        if (!value) {
            return {SignalStatus::InvalidPayload};
        }
        if (dynamic_bytes > std::numeric_limits<std::size_t>::max() - sizeof(T)) {
            return {SignalStatus::AccountingLimit};
        }
        return enqueue_erased(signal.id, typeid(T), std::move(value),
                              sizeof(T) + dynamic_bytes, SignalPayloadKind::Shared);
    }
    template<class T>
    SignalReceipt emit(Signal<T> signal, T value, std::size_t dynamic_bytes = 0) {
        auto receipt = enqueue(signal, std::move(value), dynamic_bytes);
        if (receipt) {
            static_cast<void>(flush());
        }
        return receipt;
    }
    // Borrowed emission requires an idle, empty dispatcher; nested owned work
    // remains queued. Borrowed payload bytes are not charged to queue accounting.
    // The borrowed reference never survives this call. A
    // StopFlush exception policy stops this event, and Rethrow discards its tail.
    template<class T>
    SignalReceipt emit_borrowed(Signal<T> signal, const T& value) {
        return borrowed_erased(signal.id, typeid(T), std::addressof(value), sizeof(T));
    }
    [[nodiscard]] SignalBatch batch();
    [[nodiscard]] SignalConnection connection(SignalSubscription subscription);
    [[nodiscard]] bool connected(SignalSubscription subscription) const noexcept;
    bool disconnect(SignalSubscription subscription) noexcept;
    std::size_t disconnect_owner(ge::PersistentId owner) noexcept;
    bool set_enabled(SignalSubscription subscription, bool enabled) noexcept;
    bool set_priority(SignalSubscription subscription, int priority) noexcept;
    [[nodiscard]] SignalDispatchReport flush(
        std::size_t event_limit = std::numeric_limits<std::size_t>::max(),
        std::size_t callback_limit = std::numeric_limits<std::size_t>::max());
    std::size_t discard_pending(SignalChannelId channel = {});
    void close() noexcept;
    void set_exception_policy(SignalExceptionPolicy policy);
    [[nodiscard]] SignalExceptionPolicy exception_policy() const;
    [[nodiscard]] SignalLimits limits() const;
    [[nodiscard]] SignalStatistics statistics() const;
    [[nodiscard]] std::vector<SignalChannelInfo> channels() const;
    [[nodiscard]] std::vector<SignalListenerInfo> listeners() const;
    [[nodiscard]] std::vector<SignalTrace> history(std::size_t limit = 128) const;
    void clear_history();
    [[nodiscard]] std::uint64_t scope() const noexcept;
private:
    SignalChannelId channel_erased(std::string_view name, std::type_index type);
    SignalSubscription subscribe_erased(SignalChannelId channel, std::type_index type,
        std::function<void(const void*, const SignalContext&)> function,
        SignalListenerOptions options);
    SignalReceipt enqueue_erased(SignalChannelId channel, std::type_index type,
        std::shared_ptr<const void> payload, std::size_t bytes, SignalPayloadKind kind);
    SignalReceipt borrowed_erased(SignalChannelId channel, std::type_index type,
                                  const void* payload, std::size_t bytes);
    std::shared_ptr<detail::SignalState> state_;
};

struct SignalMessage {
    std::string text;
};

[[nodiscard]] std::string_view signal_status_name(SignalStatus status) noexcept;
[[nodiscard]] std::string_view signal_trace_name(SignalTraceKind kind) noexcept;
void register_signals(Kernel& kernel);

} // namespace ge::core
