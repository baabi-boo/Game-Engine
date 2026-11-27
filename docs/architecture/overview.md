# Architecture and current scope

The [runtime source expansion](runtime-expansion.md) describes the new, explicitly
untested implementation campaign. The verified foundation scope described below
remains historical; it is not evidence for the new subsystems.

Game Engine is an independent C++20 engine foundation informed by
[public open-world research](../research/open-world-blueprint.md). It is currently
a **headless contract prototype preceding the playable feasibility milestone**.
The code demonstrates selected lifecycle and tooling contracts. It does not yet
render a world, simulate collision, perform asynchronous streaming, or provide an editor.

## Dependency direction

| Target / area | Responsibility | Allowed dependencies |
| --- | --- | --- |
| `ge_core` | Persistent identity, generation-counted handles, bounded simulation clock | C++ standard library |
| `ge_content` | Strict versioned fixture package parsing | `ge_core`, C++ standard library |
| `ge_world` | Readiness, reservation lifecycle and persistent scalar state | `ge_core`, C++ standard library, private OS file-publication call |
| `ge_headless` | Executable composition and lifecycle evidence | `ge_content`, `ge_world`, `ge_core` |
| `tools` | Authoring validation, deterministic cooking, checks, packaging | Python standard library and declared build tools |
| `tests` | Contract, malformed-input and integration validation | Public module APIs and tool entry points |

Each native module has its own public include root and target. Neither content nor
world owns the other. The application converts authored definitions into runtime
requests and initial persistent state. Runtime code does not import editor or tools code.
Future third-party APIs enter through adapters after a specific workload requires them.

## Identity, time and authority

An authored persistent object ID is a nonzero 64-bit integer, unique within its world
baseline. It is independent of a resident handle and a cooked product digest. Handles
include a generation and cannot be used after destruction/reuse. Generation exhaustion
must never silently resurrect a stale handle.

The foundation uses a bounded fixed simulation clock with integer nanosecond accounting.
Wall-clock profiling, simulation ticks and future game-calendar time are separate concepts.
Clamping/catch-up drops are observable. No cross-platform physics determinism is claimed.

The headless host is the sole writer of state. It represents authored positions as signed
integer millimetres. The intended spatial convention is metres in runtime math, right-handed,
+Y up and -Z forward; actual graphics/physics conversions are deferred to their adapters.
Coordinate epochs are carried by request tokens; this does not implement origin shifting.

## Loading and publication

A request carries a region ID, request generation, content revision and coordinate epoch.
Completion must match the whole current token. Visual, collision, navigation and logic
readiness are independent bits. In this fixture these bits are manually supplied signals,
not evidence that those systems exist. Activation requires every requested bit.

Memory accounting includes pending, resident and retiring reservations. Admission uses
overflow-safe arithmetic. Canceling or retiring a request keeps its reservation charged
until its matching acknowledgement. A request that cannot fit fails explicitly; no waiting
loop can silently deadlock a pinned-source transition. This models budget policy, not actual
allocated-memory usage, GPU fences, storage performance, threads or file eviction.

## Content and state

The authoring source is strict JSON. A standard-library Python cooker validates it, canonicalizes
ordering, and writes a bounded little-endian package with source SHA-256 and CRC32. The
native parser publishes a result only after full validation. CRC32 detects accidental
corruption and provides no authenticity guarantee. Exact format and migration rules are
documented with the content pipeline.

The prototype persists a scalar property for known persistent objects, scoped to an explicit
baseline. Saves and cooked outputs are versioned. Validation must complete before replacing
valid state. Filesystem replacement is not a universal power-loss durability protocol.

## Expansion rule

Rendering, physics, animation, navigation, audio, player input, gameplay scripting, GUI
authoring, networked play, browser targets and console targets remain roadmap work.
Add a subsystem when the next measured scenario needs it, with an ADR, ownership contract,
resource budget and acceptance evidence. Do not add empty production interfaces solely to
make the repository appear larger.

The research district dimensions, population counts, 60 Hz goal and staffing scenarios
remain planning assumptions. Passing headless tests does not establish those outcomes.
