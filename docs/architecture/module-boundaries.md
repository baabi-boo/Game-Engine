# Engine module boundaries

The domains compiled into `ge_engine` share a target, but they do not share an
unrestricted include policy. [The repository checker](../../tools/check_repo.py)
declares direct public dependencies in `PUBLIC_DEPENDENCIES` and additional
implementation dependencies in `PRIVATE_DEPENDENCIES`. Each module may include
its own headers and the C++ standard library. An allowed dependency is direct;
transitive reachability does not grant another include permission.

Public headers under `engine/<module>/include/` form an acyclic module graph.
Runtime domains cannot include editor or application code. Editor code consumes
runtime APIs. Core remains standard-library-only; content and world may consume
core, and neither may consume the other or the expanded engine domains.

## Reviewed direct dependencies

The implementation column lists additions to the public column. An em dash
means there are no cross-module dependencies in that column.

| Module | Public headers may include | Additional implementation dependencies |
| --- | --- | --- |
| core | — | — |
| content | core | — |
| world | core | — |
| math | — | — |
| ecs | core, math | — |
| jobs | core | — |
| io | — | core |
| scene | assets, ecs, math, world | core, io, physics |
| input | math | core |
| assets | io, jobs | core |
| geometry | core, math | ecs, scene |
| render | core, ecs, geometry, math | input, physics, scene, spatial |
| spatial | ecs, math | core, scene |
| physics | ecs, math, scene, spatial | core, input, navigation, render |
| animation | core, ecs, geometry, math | physics, scene |
| audio | ecs, math | core, physics, render, scene |
| navigation | core, ecs, geometry, math | physics, scene |
| terrain | core, geometry, math, render | ecs, scene |
| effects | ecs, geometry, render | core, scene |
| script | ecs, math | core, io, physics, scene |
| gameplay | core, ecs, math, physics, text | ai, input, persistence, render, scene, script, spatial |
| config | core | io |
| text | core | config, io |
| network | core, ecs, math | input, physics, scene |
| persistence | scene | core, io, world |
| ai | ecs, math | core, navigation, physics, scene, script, spatial |
| traffic | animation, geometry, navigation, physics, render | core, scene |
| editor | ecs, geometry, math, render | core, scene, spatial |

Private integration currently includes mutual relationships, such as scene and
physics. These allowances describe existing implementation coupling; they do
not authorize public cycles or establish separately linkable domain libraries.
Change a dependency only with its consumer and contract reviewed together.

## Explicit build inputs

[The engine manifest](../../engine/runtime/modules.cmake) contains exactly one
literal `set(...)` declaration for each of `GE_ENGINE_MODULES`,
`GE_ENGINE_INCLUDE_ROOTS`, `GE_ENGINE_SOURCES`, and `GE_ENGINE_HEADERS`. Values
may be quoted or unquoted; ordinary CMake comments and whitespace are supported.
Source and header paths use the `${PROJECT_SOURCE_DIR}/` prefix. Dynamic list
construction, conditionals, globs, duplicate declarations, and unrelated commands
are outside this deliberately small manifest format.

The checker compares these lists with domain translation units and public
headers in the checkout, including untracked files. Missing inputs, stale paths,
duplicates, missing modules, and missing include roots fail the gate. Core,
content, and world retain their separate build declarations.

The checker is lexical: it reads literal includes, including conditional ones,
and does not evaluate preprocessor conditions, C++ types, or general CMake.
Compilation, standalone public-header checks, linking, and runtime tests remain
necessary evidence. Run its isolated regressions with
`python3 tests/test_repo_policy.py` and the checkout gate with
`python3 tools/check_repo.py`.
