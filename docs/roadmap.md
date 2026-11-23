# Roadmap and milestone gates

The current repository is a **native headless foundation**. The next product
milestone is a small playable route with one controllable character **or** vehicle,
one graphics backend, and real collision. Expansion follows retained evidence,
not calendar dates or module count.

This roadmap adapts the [public research blueprint](research/open-world-blueprint.md)
to the code that exists. Its proposed 1 km² district, 50 visible pedestrians,
20 traffic vehicles, and 1080p/60 Hz target are workload candidates. They become
acceptance criteria only after the reference platform, scene, quality settings,
budgets, and missed-frame policy are agreed and measured. The archived staffing
and 90-day schedule are research assumptions, not repository delivery promises.

## Milestone sequence

| Milestone | Deliverable | Exit evidence |
| --- | --- | --- |
| M0 — Native foundation | Headless identity, content, lifecycle, and scalar-state contracts | F01–F09 evidence at the explicitly limited foundation scope |
| M1 — Playable feasibility route | One character or vehicle, one backend, camera/input, and real collision | Repeatable playable route with owned transforms and measured CPU/GPU/resource behavior |
| M2 — Streamed and authored route | Actual asynchronous resource streaming, useful authoring, and persistent changes | Forced real eviction/reload, failure injection, independent editing, and save/restart evidence |
| M3 — Living district | Bounded population, navigation/traffic, a mission, weather, and coherent presentation | Integrated gameplay and quality routes under declared budgets |
| M4 — Production qualification | Reliable content delivery, diagnostics, migration, and justified platform expansion | Reproducible releases and the complete target-specific acceptance matrix |

These are integration gates. Authoring, packaging, and persistence work continue
through every milestone. An owner and retained evidence are required for each
exit decision; failed gates reduce scope or trigger a contract change before
world size or population increases.

## M0: Native headless foundation

**Current scope.** The runtime contains `ge_core`, `ge_content`, and `ge_world`,
composed by `ge_headless`. Source fixtures cook to `GEPACK01`; scalar snapshots
use `GESTATE1`. The host accepts `--package`, `--save-state`, and `--restore-state`
and emits lifecycle evidence. Contract, cooker, and process tests are defined.
See the [architecture](architecture/overview.md) and
[foundation matrix](quality-gates.md#foundation-acceptance-matrix).

**Exit gate.** Another contributor can configure, build, check, edit/cook, run,
restore in a fresh process, and package from declared inputs. Malformed content,
stale handles/tokens, incomplete readiness, over-budget admission, and incompatible
saves are rejected without publishing partial state. Retain command output,
revision/tool versions, content digests, and relocation evidence.

The readiness signals and byte counters model policy. A region's logical
retirement does not establish actual asset-memory eviction, GPU completion, or
physical collision safety. M0 has no playable, frame-time, or wall-clock
streaming acceptance claim. The latest executed results and remaining platform
limits belong in [verification](production/verification.md), not this plan.

## M1: Playable feasibility route

**Scope.** Declare the actual native reference machine and choose one graphics
backend through a follow-up to [ADR 0006](adr/0006-one-native-graphics-backend.md).
Add windows, input, a camera, and a small cooked scene. Integrate collision and
one instrumented controllable character **or** vehicle. A vehicle-first route can
defer character rigs, skeletal animation, and pedestrian navigation until needed.

Name transform authority for control and physics, and define the handoff at each
simulation phase. Connect visual/collision readiness to real prepared resources.
Capture CPU critical-path time, GPU frame time, presentation intervals, and
resource allocation with profiler/instrumentation integration. Select external
libraries only with provenance, pinned revisions, terms, and adapter checks.

**Exit gate.** A packaged build launches into controllable play on the declared
platform. A repeatable route exercises movement, contact, pause/resume, camera
behavior, and the chosen interaction without duplicate authority or missing
collision. Retain captures for startup and warmed runs, state/ownership traces,
and measured memory high-water marks. Agree the workload and thresholds before
tuning; a mean frame rate or a successful headless run cannot close this gate.

## M2: Actual streaming, authoring, and persistence

**Scope.** Replace supplied readiness signals with real asynchronous reads,
decoding, dependency preparation, uploads, collision insertion, and safe
publication. Bound queues and activation work. Make cancellation, content changes,
and coordinate epochs observable; keep every in-flight or retiring resource
charged until its actual consumers finish.

Create unique region content whose working set exceeds a deliberately constrained
residency allowance. Add enough authoring and inspection tooling for an independent
contributor to edit, diagnose invalid input, cook, inspect, and package the route.
Extend persistent state by explicit object policy and schema migrations. Define
ordered asynchronous snapshot publication and process-interruption recovery before
adding asynchronous saves; select and test any stronger durability contract separately.

**Exit gate.** Run a route at its prescribed wall-clock speed with reversals,
destination changes, teleports, and delayed/canceled work. Record actual allocation,
eviction, retirement, and reload of distinct resources. Required collision stays
available or the product enters a declared controlled loading transition. Admission
must not wait forever for an overlap that cannot fit.

Measure reserved, resident, transient, upload, and retiring memory against declared
caps. The research proposes a 30-minute traversal/soak as an initial test; choose
its route and thresholds before execution and retain each run. Establish bounded
growth rather than treating one finite soak as proof of indefinite stability.
Mutate a persistent object, leave its region, reload it, save, restart, and verify
the same logical state. Inject invalid content, stale completion, interrupted
publication, and failed writes; verify recovery of a valid committed state. Repeat
the edit–cook–package exercise with a contributor who did not implement the tool.

## M3: Living district and coherent presentation

**Scope.** Add a bounded simulated population with persistent identities and
explicit promotion/demotion between simulation tiers. Introduce authored roads,
navigation, limited traffic, schedules, and one mission. Add a game clock, time of
day, rain/weather transitions, materials, lighting, atmosphere, animation where
needed, spatial audio, and the player-facing controls/UI needed by the route.

Select population and scene density using M2 measurements. Visible decorative
actors and fully simulated actors need separate budgets. Possession or interaction
must preserve identity, pose, velocity, occupants, and one authoritative writer.
Mission reservations need owners and release paths; retries must not duplicate
completion rewards. Persistence must cover the chosen mission and significant
world changes, with migration fixtures as their meaning evolves.

**Exit gate.** A complete route joins traversal, interaction, interior transitions,
population changes, mission interruption/retry, save/restart, and return to a changed
location. Review contacts, motion, visual transitions, and sound under day/night
and rain conditions while measuring the integrated workload. Expand toward the
research district only when its selected density, quality, and timing contracts
pass together. Keep individual performance runs and observed failure cases.

## M4: Production qualification and targeted expansion

**Scope.** Harden crash diagnostics, retained symbols, content validation,
reproducible release manifests, migration/recovery fixtures, settings, accessibility,
and independent authoring throughput. Qualify the packaged product on its declared
hardware and software matrix. Add a second backend or native platform only for a
named product requirement and with the same lifecycle and performance evidence.

Browser delivery has its own loading, memory, persistence, threading, and graphics
constraints. Console work requires authorized access and target qualification.
Neither follows automatically from a C++ build. Broad destruction, large maps,
advanced physical reactions, and a public mod platform also require separately
bounded workloads and acceptance decisions.

If multiplayer becomes a product requirement, open the authority/replication
decision immediately and move a two-client interaction experiment into the earliest
affected milestone. Do not postpone authority until after single-player systems
depend on incompatible assumptions.

**Exit gate.** Every claimed target has retained clean-build, install/relocation,
gameplay, resource, recovery, and performance evidence. Every supported migration
has a reproducible fixture. The [eight expanded gates](quality-gates.md#after-the-headless-foundation)
remain the release review framework; selected features must satisfy them together.
