#include "test_support.hpp"

#include <ge/core/runtime.hpp>
#include <ge/script/vm.hpp>

#include <cstdint>
#include <span>

namespace {

void deferred_host_receives_a_full_frame_budget() {
    ge::core::Kernel kernel;
    ge::script::VmLimits limits;
    limits.max_instances = 2;
    limits.slice_gas = 4;
    limits.frame_gas = 4;
    limits.max_total_gas = 64;
    ge::script::ScriptService service(kernel, limits);

    std::uint32_t host_calls = 0;
    CHECK(service.register_host({{"expensive", {}, ge::script::ValueType::Null}, 4,
        [&](ge::script::HostContext&, std::span<const ge::script::Value>) {
            ++host_calls;
            return ge::script::HostResult{};
        }}));
    CHECK(service.load("loop", R"(
.func main 0 0 0
again:
yield
jmp again
.end
)"));
    CHECK(service.load("host", R"(
.func main 0 0 0
host expensive
pop
ret
.end
)"));

    const auto first = service.start("loop");
    const auto second = service.start("host");
    CHECK(first);
    CHECK(second);
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        ge::core::Frame frame{};
        frame.tick = tick;
        service.advance(frame);
        CHECK(service.statistics().frame_gas <= limits.frame_gas);
    }

    CHECK(host_calls == 1);
    const auto completed = service.inspect(second);
    CHECK(completed);
    CHECK(completed->state == ge::script::ExecutionState::Completed);
    CHECK(completed->gas_used == 6);
    CHECK(service.inspect(first)->state == ge::script::ExecutionState::Yielded);
}

} // namespace

int main() {
    return ge::test::run("script_vm_audit", {
        {"deferred_host_receives_a_full_frame_budget", deferred_host_receives_a_full_frame_budget},
    });
}
