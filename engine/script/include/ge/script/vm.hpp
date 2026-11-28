#pragma once

#include <ge/ecs/registry.hpp>
#include <ge/math/math.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ge::core { class Kernel; struct Frame; }

namespace ge::script {

enum class ValueType : std::uint8_t {
    Null, Boolean, Integer, Number, String, Vector, Entity, Any = 255
};

struct Value {
    using Storage = std::variant<std::monostate, bool, std::int64_t, double,
                                 std::string, math::Vec3, ecs::Entity>;
    Storage data;
    Value() = default;
    explicit Value(bool value) : data(value) {}
    explicit Value(std::int64_t value) : data(value) {}
    explicit Value(double value) : data(value) {}
    explicit Value(std::string value) : data(std::move(value)) {}
    explicit Value(math::Vec3 value) : data(value) {}
    explicit Value(ecs::Entity value) : data(value) {}
    [[nodiscard]] ValueType type() const noexcept;
    [[nodiscard]] std::string display() const;
    [[nodiscard]] bool operator==(const Value&) const = default;
};

struct VmLimits {
    std::size_t max_source_bytes{1024U * 1024U};
    std::size_t max_binary_bytes{8U * 1024U * 1024U};
    std::size_t max_string_bytes{4096};
    std::uint32_t max_programs{128};
    std::uint32_t max_instructions{65536};
    std::uint32_t max_constants{8192};
    std::uint32_t max_globals{256};
    std::uint32_t max_functions{256};
    std::uint32_t max_imports{128};
    std::uint32_t max_parameters{32};
    std::uint32_t max_locals{256};
    std::uint32_t max_local_slots{4096};
    std::uint32_t max_stack{512};
    std::uint32_t max_call_depth{32};
    std::uint32_t max_instances{256};
    std::size_t max_verifier_values{1024U * 1024U};
    std::uint64_t max_total_gas{10000000};
    std::uint64_t slice_gas{2048};
    std::uint64_t frame_gas{65536};
    std::uint32_t max_host_calls_per_slice{64};
    std::uint64_t max_sleep_frames{1000000};
};

struct SourceLocation {
    std::uint32_t line{};
    std::uint32_t column{};
};

struct ScriptError {
    std::string message;
    SourceLocation source{};
    std::uint32_t instruction{};
};

enum class Op : std::uint8_t {
    Nop, Constant, LoadLocal, StoreLocal, LoadGlobal, StoreGlobal,
    Pop, Duplicate, Add, Subtract, Multiply, Divide, Remainder, Negate,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, Not,
    Jump, JumpFalse, Call, Host, Return, Yield, Sleep, Halt
};

struct Instruction {
    Op op{Op::Nop};
    std::uint32_t operand{};
    SourceLocation source{};
};

struct Function {
    std::string name;
    std::uint32_t begin{};
    std::uint32_t end{};
    std::uint32_t parameters{};
    std::uint32_t locals{};
    bool returns_value{};
};

struct HostSignature {
    std::string name;
    std::vector<ValueType> parameters;
    ValueType result{ValueType::Null};
    bool operator==(const HostSignature&) const = default;
};

struct ProgramData {
    std::vector<Value> constants;
    std::vector<Value> globals;
    std::vector<Instruction> instructions;
    std::vector<Function> functions;
    std::vector<HostSignature> imports;
    std::uint32_t entry{};
};

class Program;
struct CompileResult {
    std::shared_ptr<const Program> program;
    ScriptError error;
    [[nodiscard]] explicit operator bool() const noexcept { return program != nullptr; }
};

// Only verified, privately owned program storage is published. Instances retain
// their original program version when a named program is subsequently replaced.
class Program final {
public:
    [[nodiscard]] const ProgramData& data() const noexcept;
    [[nodiscard]] static CompileResult create(ProgramData data, VmLimits limits = {});
private:
    explicit Program(ProgramData data);
    ProgramData data_;
};

[[nodiscard]] std::optional<ScriptError> verify(const ProgramData& data,
                                               const VmLimits& limits = {});
// Assembly directives: .global/.const TYPE VALUE, .func NAME ARGS LOCALS
// RETURNS(0|1), .entry NAME, .end. Labels are local to a function. Instructions
// include push TYPE VALUE, const/load/store/gload/gstore INDEX, call/host NAME,
// jmp/jfalse LABEL, arithmetic, ret, yield, sleep, halt. Sleep pops integer frames.
[[nodiscard]] CompileResult assemble(std::string_view source,
                                     std::span<const HostSignature> hosts = {},
                                     VmLimits limits = {});
[[nodiscard]] std::vector<std::byte> encode_program(const Program& program,
                                                   VmLimits limits = {});
[[nodiscard]] CompileResult decode_program(std::span<const std::byte> bytes,
                                           VmLimits limits = {});

struct InstanceHandle {
    std::uint32_t index{};
    std::uint64_t generation{};
    [[nodiscard]] explicit operator bool() const noexcept {
        return index != 0 && generation != 0;
    }
    auto operator<=>(const InstanceHandle&) const = default;
};

enum class ExecutionState : std::uint8_t {
    Ready, Running, Yielded, Sleeping, Paused, Completed, Faulted, Stopped
};

[[nodiscard]] std::string_view state_name(ExecutionState state) noexcept;

struct InstanceInfo {
    InstanceHandle handle{};
    ecs::Entity owner{};
    std::string program;
    ExecutionState state{ExecutionState::Ready};
    std::uint32_t instruction{};
    std::size_t stack_size{};
    std::size_t call_depth{};
    std::uint64_t gas_used{};
    std::uint64_t wake_tick{};
    Value result;
    ScriptError error;
};

struct ScriptComponent {
    std::string program;
    InstanceHandle instance{};
    bool enabled{true};
    bool auto_start{true};
    std::string last_error;
};

struct HostContext {
    core::Kernel& kernel;
    const core::Frame& frame;
    ecs::Entity owner{};
    InstanceHandle instance{};
};

struct HostResult {
    bool success{true};
    Value value;
    std::string error;
};

struct HostFunction {
    HostSignature signature;
    std::uint64_t gas_cost{1};
    std::function<HostResult(HostContext&, std::span<const Value>)> callback;
};

struct VmStatistics {
    std::size_t programs{};
    std::size_t instances{};
    std::size_t runnable{};
    std::uint64_t frame_gas{};
    std::uint64_t completed{};
    std::uint64_t faulted{};
};

// Single writer; callbacks run synchronously, must be bounded, and may not
// reenter mutating ScriptService operations. Native callbacks are trusted engine
// code, not a security sandbox. Registrations own their function objects; running
// instances pin each imported callback until release, even after unregistering.
// Argument spans and HostContext references are borrowed only for the callback.
class ScriptService final {
public:
    explicit ScriptService(core::Kernel& kernel, VmLimits limits = {});
    ~ScriptService();
    ScriptService(const ScriptService&) = delete;
    ScriptService& operator=(const ScriptService&) = delete;
    [[nodiscard]] const VmLimits& limits() const noexcept;
    bool register_host(HostFunction function, std::string* error = nullptr);
    bool unregister_host(std::string_view name);
    [[nodiscard]] std::vector<HostSignature> host_signatures() const;
    [[nodiscard]] CompileResult load(std::string name, std::string_view source);
    bool install(std::string name, std::shared_ptr<const Program> program,
                 std::string* error = nullptr);
    bool unload(std::string_view name);
    [[nodiscard]] std::shared_ptr<const Program> program(std::string_view name) const;
    [[nodiscard]] InstanceHandle start(std::string_view name, ecs::Entity owner = {},
                                       std::span<const Value> arguments = {},
                                       std::string* error = nullptr);
    bool attach(ecs::Entity entity, std::string_view program, bool auto_start = true);
    bool detach(ecs::Entity entity);
    bool pause(InstanceHandle handle);
    bool resume(InstanceHandle handle);
    bool stop(InstanceHandle handle);
    bool release(InstanceHandle handle);
    void stop_all() noexcept;
    [[nodiscard]] std::uint64_t step(InstanceHandle handle, std::uint64_t gas = 1);
    // Frame gas is shared in round-robin slices. An instruction that cannot fit
    // the final partial slice receives the first slice of the next frame.
    void advance(const core::Frame& frame);
    [[nodiscard]] std::optional<InstanceInfo> inspect(InstanceHandle handle) const;
    [[nodiscard]] std::vector<InstanceInfo> instances() const;
    [[nodiscard]] std::optional<Value> global(InstanceHandle handle, std::size_t index) const;
    bool set_global(InstanceHandle handle, std::size_t index, Value value);
    [[nodiscard]] VmStatistics statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void register_vm(core::Kernel& kernel);

} // namespace ge::script
