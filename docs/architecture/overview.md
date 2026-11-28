# Architecture and current scope

Game Engine is an independent C++20 engine foundation informed by
[public open-world research](../research/open-world-blueprint.md). Its composed
`ge_runtime` application is headless and pre-alpha. Current domain implementations
include Scene/ECS, jobs and filesystem I/O, asset streaming, CPU software rendering,
collision and dynamics, animation, navigation, audio processing, gameplay and
networking. These are development foundations, with bounded contracts and
integration tests; they do not constitute a production-qualified playable engine.

The retained `ge_headless` executable demonstrates the earlier synchronous
lifecycle fixture. Its modeled reservation counters and scalar-state checks have
a narrower scope than the composed runtime. The original source expansion excluded
execution; the later [integration audit](../production/integration-audit-2026-11-28.md)
and [stabilization review](../production/stabilization-2026-11-28.md) compile,
exercise and repair the implementations. Read the
[task ledger](../../project/agent-state.json) for source-specific execution receipts
and review status. A capability's source presence is separate from platform,
performance or production qualification.

## Dependency direction

| Target / area | Responsibility | Allowed dependencies |
| --- | --- | --- |
| `ge_core` / `ge::core` | Identity, bounded allocation, binary codecs, signals and the generic runtime kernel | C++ standard library and thread runtime |
| `ge_content` / `ge::content` | Strict versioned fixture package parsing | `ge_core`, C++ standard library |
| `ge_world` / `ge::world` | Readiness, reservation lifecycle and persistent scalar state | `ge_core`, C++ standard library, private OS file-publication call |
| `ge_engine` / `ge::engine` | Combined runtime domain implementations | Core, content, world, thread runtime and private platform adapters |
| `ge_headless` | Fixture composition and lifecycle evidence | Content, world and core |
| `ge_runtime` | Explicit domain installation, command host and frame advancement | `ge_engine` and its public dependencies |
| `tools` | Authoring validation, deterministic cooking, checks and packaging | Python standard library and declared build tools |
| `tests` | Contract, malformed-input and integration validation | Public module APIs and tool entry points |

Domains retain separate public include roots under `engine/<domain>/include/`;
the expanded implementations share the `ge_engine` target. The explicit
[build manifest](../../engine/runtime/modules.cmake) declares its source and header
inputs. The [module boundary policy](module-boundaries.md) keeps public dependencies
acyclic and declares private integration imports. Content and world remain separate
libraries and neither owns the other. Runtime domains cannot import application or
editor code; the editor selection backend consumes runtime APIs. The normal build
compiles every public header independently.

The [runtime composition contract](runtime-expansion.md) describes typed services,
frame phases, explicit scheduling dependencies, bounded event lifetimes and shutdown.
Registration order or numeric priority alone should not substitute for a required
producer/consumer dependency. New third-party APIs enter through adapters after a
specific workload requires them.

## Identity, time and authority

An authored persistent object ID is a nonzero 64-bit integer, unique within its world
baseline. It is independent of a resident handle and a cooked product digest. Handles
include a generation and cannot be used after destruction/reuse. ECS component
instance identities additionally distinguish replacement components on a live entity.
Ownership checks must preserve these distinctions; generation exhaustion must not
silently resurrect a stale handle.

The foundation's `FixedClock` is a separate bounded accumulator with integer
nanosecond accounting and observable catch-up drops. The composed kernel accepts
an explicit finite delta in `[0, 1]` seconds for each tick; `ge_runtime` selects a
positive constant step with `--step`. It does not automatically run the fixture's
accumulator. Wall-clock profiling, simulation ticks and game-calendar concepts
remain distinct. Cross-platform physics determinism is not claimed.

The lifecycle fixture stores authored positions as signed integer millimetres.
Runtime math uses metres with the intended right-handed, +Y-up, -Z-forward convention.
[World partition](../../engine/scene/include/ge/scene/partition.hpp) maintains an
absolute origin and coordinate epochs while Scene consumers use runtime coordinates.
Networking has explicit owner/server authority and handoff contracts; it does not
inherit the fixture's sole-writer assumption.

## Loading and publication

The foundation `StreamingPolicy` request carries a region ID, request generation,
content revision and coordinate epoch. Completion must match the whole current token.
Its visual, collision, navigation and logic readiness bits are manually supplied
signals in the lifecycle fixture. Passing that fixture does not prove readiness of
the corresponding runtime consumers.

Memory accounting includes pending, resident and retiring reservations. Admission
uses overflow-safe arithmetic. Cancellation or retirement keeps its reservation
charged until the matching acknowledgement. These fixture counters model budget
policy rather than physical memory, GPU fences or storage performance.

The separate [asset streamer](../../engine/assets/include/ge/assets/streaming.hpp)
dispatches bounded jobs through the virtual filesystem, tracks dependency revisions,
publishes results on its owner thread and retains explicitly pinned versions.
Its byte-resource path does not establish typed mesh, texture or audio import
adapters. World partition has runtime origin and producer-readiness contracts;
actual workload budgets, complete adapter coverage and traversal deadlines require
their own evidence. Terrain currently supplies height queries and render bindings;
there is no physics heightfield collision consumer.

## Content and state

The fixture authoring source is strict JSON. A standard-library Python cooker
validates it, canonicalizes ordering, and writes a bounded little-endian package
with source SHA-256 and CRC32. The native parser publishes only after full validation.
CRC32 detects accidental corruption and provides no authenticity guarantee. Exact
format and migration rules are documented in the
[content pipeline](../development/content-pipeline.md).

The foundation persists a scalar property for known persistent objects under an
explicit baseline. The expanded runtime also provides scene documents and a
[transaction journal](../../engine/persistence/include/ge/persistence/journal.hpp)
with bounded records, checkpoints and explicit recovery policies. Validation must
complete before replacing valid state. Filesystem replacement and successful
recovery tests do not establish universal power-loss durability.

## Gameplay integration boundary

The automatic interaction-to-mission bridge consumes successful `Activated` history.
Those terminal events currently have an empty name, so automatic `Interaction`
objectives use an empty `event_key` as a wildcard. A nonempty key requires explicit
`MissionInput`; a named Notify action does not define an activation key. The bridge
preserves the event's captured persistent identities when consuming delayed history.
See the [mission bridge](../../engine/gameplay/src/mission.cpp) and
[interaction dispatch](../../engine/gameplay/src/interaction.cpp) for this contract.

## Further development

Native window/GPU backends, an editor GUI, complete typed asset adapters and
production workload qualification remain further work. Extend a subsystem for a
measured scenario with an ownership contract, resource budget and acceptance
evidence; keep source delivery and observed behavior distinct.

The research district dimensions, population counts, 60 Hz goal and staffing
scenarios remain planning assumptions. Headless tests do not establish those
outcomes or support for an unexecuted OS, architecture, driver or device backend.
