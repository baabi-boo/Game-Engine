# Runtime source expansion

The original source campaign added C++20 engine implementation while preserving
the previous headless fixture and its historical evidence. That campaign excluded
builds and execution. The later owner-authorized
[integration audit](../production/integration-audit-2026-11-28.md) now exercises
and repairs those implementations. Its observed results qualify their recorded
source and scope; historical foundation evidence does not qualify new domains.

## Composition

`ge_core` owns identities, bounded allocation, binary codecs and the generic
runtime kernel. It depends only on the standard library and its thread runtime.
`ge_content` and `ge_world` retain their original dependency on core.
`ge_engine` compiles the new domain implementations and depends on those three
libraries. Public headers remain under each domain's `include/ge/` directory.
`ge_runtime` composes every installed domain through explicit registration calls
in `apps/runtime/modules.cpp`. Every source appears explicitly in CMake manifests.
The [module boundary policy](module-boundaries.md) declares public and private
imports separately. Public module dependencies are acyclic; existing private
integration dependencies remain explicit inside the combined engine target.
The repository checker rejects missing source/header manifest entries and
undeclared imports. The build compiles each public header independently.

The kernel owns stable typed services. Modules register commands and scheduled
frame functions that use those services. Phases order input, simulation,
animation, visibility, rendering, audio and frame retirement. Explicit system
dependencies are checked when scheduling. The bounded event bus is a consuming
queue per event type; independent consumers must use shared state or distinct
event channels. Kernel frame maintenance retains new events throughout the
following frame, then retires the old undrained prefix. Retirement and capacity
drops have separate counters. Durable backlogs belong in their owning services;
standalone event buses retain events until drained or explicitly maintained.
This prevents unused notifications from permanently starving input and filesystem
changes. Shutdown callbacks execute once in reverse registration order before
service destruction so worker threads can join while dependent services live;
recursive shutdown is harmless. A shutdown request closes command, registration
and schedule mutation admission immediately. Cleanup waits for active command,
queue-drain and frame callbacks to unwind, so services cannot be torn down below
their current consumers. Remaining queued commands and scheduled systems are
discarded. An interrupted tick returns false and does not retire its frame events.
The separate `request_stop()` operation still finishes the current frame.
Cleanup failures are counted without allocation and recorded as diagnostics when
possible; the CLI completes shutdown before emitting its final report and exit
status. A handler failure never prevents the remaining cleanup callbacks.

The command host supports startup settings, command scripts, interactive commands,
bounded frame advancement and JSON diagnostics. The command listing comes from
the actual installed services. It is not a windowed native graphics backend.

## Development accounting

The canonical task ledger retains unrun `dev` acceptance requirements. Source
checkpoints do not close those gates. The separate source campaign register
records actual delegated agent paths, assigned files and source delivery status;
planned work does not count as an executed agent. Source line counts exclude
blank lines, comments, tests, documentation, tooling, assets and vendored code.
Line totals measure size and do not demonstrate correctness or usefulness.
