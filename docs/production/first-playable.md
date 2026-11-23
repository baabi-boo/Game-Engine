# First playable: M1 execution plan

**Status: proposed work, no playable acceptance recorded.** The
[foundation](../architecture/overview.md) provides cooking, identity, symbolic
readiness/reservations, and scalar persistence. It does not provide graphics,
collision, or actual streaming. This plan implements
[M1](../roadmap.md#m1-playable-feasibility-route); it does not qualify the research
district, a complete game, or AAA production readiness.

## Route and decision gates

Build one native desktop route with a real window, keyboard/mouse input, camera,
one graphics backend, static collision, and one unrigged controllable actor.
Author one original indexed block mesh and reuse it for the floor, ramp, wall,
and actor. An interaction changes one existing persistent scalar; the route ends
with retirement, reload, save, and fresh-process restore of that value.

1. **Declare the experiment.** Assign reviewers through [ownership](ownership.md).
   Record the reference machine and route fields below; set movement speed,
   contact tolerances, memory caps, timing thresholds, and missed-frame policy
   before accepting results. Hardware and thresholds remain unselected.
2. **Measure then select the backend.** Follow [ADR 0006](../adr/0006-one-native-graphics-backend.md).
   Compare SDL GPU, Vulkan, or Direct3D 12 where supported on the declared
   host using window creation, indexed drawing, resize, upload/retirement,
   presentation, and capture. Record limitations and measured integration effort;
   select exactly one in a follow-up ADR. SDL3 and Jolt remain `evaluate` in the
   [register](../../third_party/dependencies.json). Evaluate their platform and
   collision adapters against that route; adoption requires the
   [dependency evidence](dependency-policy.md), including pinned inputs and offline
   packaging. This plan adopts no library.
3. **Connect authored content.** Extend the cooker with a bounded geometry/instance
   subset and explicit format-version decision; existing `GEPACK01` v1 has no mesh
   payload. Record original source provenance, units, winding, indices, limits,
   collision representation, settings, and product hashes. Invalid geometry must
   preserve the last valid product. A second contributor edits, cooks, inspects,
   and packages it without C++ edits.
4. **Integrate movement and lifecycle.** Implement the boundaries below together.
   Traverse the ramp and wall, turn abruptly, pause/resume, lose/regain focus,
   resize, interact, retire/reload, then restart from the save. Keep active support
   collision resident; a transition that cannot fit enters an explicit safe loading
   state. M1 may prepare resources synchronously; real asynchronous eviction and
   throughput acceptance remain M2.
5. **Accept the packaged route.** Run from a relocated installation with declared
   inputs. Retain route captures and failures in [verification](verification.md),
   alongside the existing [foundation gates](../quality-gates.md). A launch or mean
   FPS alone does not close M1.

## Required boundaries

These are implementation obligations, not declared placeholder APIs.

| Boundary | Data, authority, and completion |
| --- | --- |
| Platform → simulation | Convert events to owned action state; clear held actions on focus loss. Pause and fixed-step catch-up behavior are explicit. |
| Control → collision → presentation | Control requests movement; the collision step owns the resolved actor transform. Rendering reads completed poses. Use metres, right-handed axes, +Y up, −Z forward; convert authored millimetres at the boundary. |
| Cooked data → resources | Validate bounded products before publication. Keep vendor types private. Match the complete existing request token; signal visual readiness only after usable upload, collision readiness only after insertion. |
| Resource retirement → policy | Release render resources after GPU consumers finish and collision after actor/query users release it; only then acknowledge retirement. Cancellation alone releases nothing. |
| Interaction → persistence | Keep persistent IDs, runtime handles, revisions, and sequences distinct. Use the current scalar snapshot contract; do not serialize transient handles or imply pose persistence. |

## Evidence record

Record CPU model/core count, RAM, GPU/VRAM, storage model, OS/version, driver,
compiler/build configuration, backend/revision, source/content hashes, resolution,
quality settings, display refresh/presentation mode, route/input sequence, speed,
cache condition, instrumentation/overhead, reviewer, and artifact paths.

Separate cold launch, first traversal, and warmed runs. Retain CPU critical-path,
GPU and presentation timings with p50/p95/p99/worst, missed/consecutive frames;
allocation/residency peaks including staging and retirement; contact/authority
traces; and save/restart values. See the [risk gates](risk-register.md).
