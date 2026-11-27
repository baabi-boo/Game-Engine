# Runtime source expansion

This owner-requested campaign adds original C++20 engine implementation while
preserving the previous headless fixture and its historical evidence. The owner
explicitly requested no builds, tests, engine execution or CI waits. New code is
therefore source implementation awaiting execution and qualification. Existing
quality gates have not been relaxed or reported passed.

## Composition

`ge_core` owns identities, bounded allocation, binary codecs and the generic
runtime kernel. It depends only on the standard library and its thread runtime.
`ge_content` and `ge_world` retain their original dependency on core.
`ge_engine` compiles the new domain implementations and depends on those three
libraries. Public headers remain under each domain's `include/ge/` directory.
`ge_runtime` composes every installed domain through explicit registration calls
in `apps/runtime/modules.cpp`. Every source appears explicitly in CMake manifests.

The kernel owns stable typed services. Modules register commands and scheduled
frame functions that use those services. Phases order input, simulation,
animation, visibility, rendering, audio and frame retirement. Explicit system
dependencies are checked when scheduling. The bounded event bus is a consuming
queue per event type; independent consumers must use shared state or distinct
event channels. Shutdown callbacks execute in reverse registration order before
service destruction so worker threads can join while dependent services live.

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
