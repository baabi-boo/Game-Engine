# Existing engine stabilization — 2026-11-28

## Objective and baseline

The owner requested 100 actual agents to inspect, debug and connect the existing
engine, verify compilation, and leave a maintainable base for further work.
This continuation starts at `876ec252119d575a110f69c3f9b6140642a3f3c2`.
The earlier [integration register](integration-audit-register.json) remains a
historical checkpoint, including its unfinished worker record. It is not counted
as part of this new set of 100 runs.

The [new actual-worker register](stabilization-register-2026-11-28.json) records
canonical agent IDs, assigned scopes, completed reports and actual progress.
Only successful delegated runs are counted; planned roles and repeated turns do
not create additional agents. Runtime capacity is three simultaneous workers.

## Status

The requested review has 100 completed workers out of 100 distinct
canonical agent IDs. The actual-worker register contains every assigned scope and
completed report. Acceptance of `AUDIT-20261128` is recorded in the canonical ledger. The canonical ledger records acceptance against a full source
fingerprint; historical checks do not qualify later changes. Read the delivery
commit together with that task's latest source-matching receipts and completion
note. This review qualifies a maintainable headless engine foundation for further
development within the observed environment, not a production or platform guarantee.

Reviewers worked in batches of at most three, with one coordinator integrating
released paths and running frozen-source checks. Read-only reviews count only when
an actual agent inspected its assigned contract and delivered findings. Earlier
campaign registers and repeated agent turns are not part of the 100-worker count.

## First checkpoint changes

- Input device removal preserves unrelated held-action state. Captured/consumed
  input cancels timed interactions without fabricating a physical release.
- Scene document validation charges complete section and archive budgets, rejects
  unsavable limits, preserves last-good outputs and identifies parent changes.
- Asset-stream dependency refresh survives delayed catalog events and replaces
  obsolete closures at capacity while retaining other consumers and active work.
- Kernel shutdown closes admission, waits for active callbacks to unwind, skips
  pending work and reports cleanup failures in the CLI's final status.
- Collision and triangle queries preserve small valid geometry. Dynamics retains
  small position edits far from the origin, and character control preserves Scene
  teleports while discarding contacts from the previous pose.
- Vehicle one-shot torques do not survive zero-time updates; obsolete AI expiry
  state is cleared on authority transfer. Constraints detect lost endpoints before
  applying another cached velocity impulse.
- Implicit mesh ranges obey the configured submesh limit.
- Camera selection, stale-view publication, ownership and capacity recovery are
  repaired. Camera and visibility share stable far-origin matrix/frustum updates.
- The composed-runtime regression explicitly requires skinning and checks final
  pose evaluation before skinning and mesh-bound publication.

Read-only review found no definite defect in the assigned partition, spatial
index, image, material or rasterizer contracts. Those reviews are bounded source
inspections, not exhaustive correctness proofs. Worker 022 reproduced a batching
cache lifetime defect; worker 023 supplied the integrated repair.

## Second checkpoint changes

- Mesh libraries expose an opaque lifetime token shared by render batching and
  lighting caches. Reconstructed libraries cannot reuse stale geometry or shadows
  merely because their address, pool handles and revisions match.
- Imported render-graph depth surfaces obey dimension and pixel budgets.
- Lighting uses the shared stable camera matrix path and releases renderer shadow
  callbacks only when both function and owner still match.
- Postprocess and decal services reject detach requests for a different graph,
  preserving the active graph's callback and accurate attachment state.
- Bound skeleton animation advances only when its scene transform batch succeeds.
  Curve markers allocate before transform publication, preserving player/scene
  agreement when payload allocation fails.
- Additive rotations extrapolate the shortest angular delta accurately. Quaternion
  curves retain small rotations and their velocity, and the shared direction
  rotation helper preserves small and nearly opposite IK corrections.
- Root extraction strips newly refreshed poses at zero playback speed or zero frame
  delta while retaining the paused playback baseline.

Mixer and spatial-audio source reviews found no definite defect in their assigned
contracts. The component-address reuse issue identified during these reviews is now
resolved by the shared ECS identity and consumer changes below.

## Third checkpoint changes

- ECS exposes a component instance identity distinct from entity generation and
  change revision. It survives ordinary edits and changes on replacement, allowing
  skinning, IK, blend, retarget and particles to reject stale ownership even when
  an allocator reuses the same address. Skinning additionally checks mesh-library
  lifetime before touching output meshes.
- Navmesh movement can cross multiple connected edges at a shared vertex within
  its existing work budget. Winding normalization retains the authored height-fan
  anchor for nonplanar polygons.
- Heightfield sampling accepts exact upper endpoints on fractional grids. Terrain
  and vegetation enforce Scene and mesh-library ownership. A shared terrain Scene
  predicate also validates vegetation's initial binding.
- Particle cleanup and material operations preserve nodes after their binding is
  removed or replaced.
- Script scheduling gives a costly host call the first slice of the next frame
  when it cannot fit the current frame's remaining budget, preventing starvation.
- Settings imports reject aliases targeting the same setting atomically, and
  exports check that the format header fits the configured line budget.

The terrain review covers height queries and render bindings; the current physics
module has no terrain heightfield collision consumer. That feature is not implied
by passing terrain tests.

## Fourth checkpoint changes

- UTF-16 conversion errors identify the input code-unit position when UTF-8 output
  exceeds its budget, including supplementary characters.
- Reliable networking retains bounded retry identities so delayed ACKs release
  the correct message and fragment reservations. Raw transport queues age packets
  from the first valid pump even when send budgets defer their first attempt.
- Replication distinguishes evicted unread frames from an already-consumed cursor
  frame, avoiding false history-loss reports and unnecessary prediction resyncs.
- Server prediction refreshes live collision layers before processing inputs, so
  simulation and captured authoritative metadata agree.
- Journal replay rejects transaction IDs that cannot form a valid checkpoint
  manifest, preserving strict recovery and reopen behavior.
- Behavior execution aborts and cleans up when a callback invalidates its
  attachment, and component replacement cannot silently inherit an attachment.
- Perception rotates priority under shared query budgets. Road edits budget the
  resources they retain after connector retirement, and partial-lane routing uses
  a consistent remaining-time bound.

## Fifth checkpoint changes

- Traffic treats busy updates/shutdown as retryable without losing AI ownership or
  leaving orphan components. Arrival checks use chassis speed magnitude.
- Interaction enrollment follows component instance identity through queues,
  callbacks and cleanup, preserving replacement components and allowing reattach.
- Inventory retires containers after persistent actor identity changes. Equipment
  integration tests verify reservation, modifier and restore cleanup in the
  existing inventory-before-equipment update order.
- Rejected combat transactions report no applied damage or defeat and preserve
  ammunition, cooldowns, target state and history.
- Dialogue retains pending provider cancellation and retries it after session
  release until completion. Quest-owned mission cancellation uses UTF-8-safe
  diagnostics bounded by the receiving mission service's limit.
- Editor pick acceptance synchronizes permitted Registry edits before validating
  its cached Scene revision, preserving selection/history when results are stale.

## Sixth checkpoint changes

- Executor waits avoid destruction of user captures under the state mutex and
  reject blocking waits from synchronous cancellation callbacks.
- File-watch polling publishes snapshots after assembling the successful result,
  preserving events if a later watch scan fails.
- Bit-writer copy assignment preserves destination metadata when allocation fails.
- Signal dispatch accounts for close during a throwing final callback and for
  events queued by deferred listener cleanup.
- Asset catalogs retain the type paired with their last good revision, protecting
  fallback resources during failed imports after a type change.
- Repository checks reject file symlinks that bypass module boundaries. Benchmark
  JSON rejects non-finite exponent values, and malformed build-cache encoding uses
  the driver's normal error boundary.
- Package install directories reject anchored Windows paths and embedded NUL. The
  installed README describes both executables, all SDK targets and the manifest.

The package gate initially identified a checkout-local build-tool dependency. The
build tools were relocated outside the source tree and the unchanged relocation
check then passed, including a separate installed-SDK consumer. No gate was relaxed.

## Integration and independent-review follow-up

- CLI validation rejects empty script/report paths and malformed UTF-8 scripts,
  reports output failures, and emits valid JSON for malformed diagnostic bytes.
- Blended final poses now reach CPU skinning even when blending owns a disabled
  base animator. The populated runtime regression checks deformed geometry,
  spatial bounds and rendered pixels together.
- Animator sampling publishes an explicit successful-pose serial. Root capture
  strips fresh paused same-time seeks, resumed clamped endpoints and zero-duration
  samples once; failed sampling and rejected Scene publication retain the prior
  extraction baseline. Caller assignment/restoration of a different Animator
  requires `reset_root()` as documented.
- Skeletal animation stages each entity's sampled Animator and event payload
  allocations before publishing its Scene pose or playback state. A failed entity
  remains retryable. This is a per-entity guarantee: an allocation exception can
  leave earlier entities committed and returns no event batch, as the API states.
- Interaction history captures persistent actor and target identities when the
  event occurs. Delayed mission consumption cannot credit a replacement identity.
- Network authority and owner handoffs publish the accepted pose and velocity
  before prediction suspends, then release prediction ownership.
- Terrain checks translated/scaled bounds in world coordinates before inverse
  sampling, preserving inclusive endpoints and strict rejection one representable
  value outside. Populated vegetation tests cover rebasing and cleanup.
- Asset streaming adopts external dependency-free fallback versions using the
  type paired with their last-good revision, preserving the bytes across type
  changes, insufficient replacement budgets and decoder failures. Historical
  dependency graphs are not inferred.
- A refused executor join from an external cancellation callback now reaches the
  kernel cleanup-failure counter and diagnostics. The service remains alive for
  an explicit owner-thread cleanup after that callback returns.
- Allocation-failure regression instrumentation measures the standard library
  allocation it intends to reject and remains active in optimized builds. It
  retains the original transform-publication failure assertion.
- Developer setup, architecture and packaging guides describe the implemented
  engine and current SDK. They distinguish the lifecycle fixture from the composed
  runtime and identify the automatic interaction-to-mission key limitation.

Independent follow-up reviewers check repairs against consumers, resource ownership,
transaction boundaries, frame scheduling, numerical limits, build manifests and
platform-specific source paths. Their exact reviewed scopes and executed evidence
are retained in the actual-worker register. No-defect findings are bounded reviews,
not proofs that every possible bug has been eliminated.

## Verification scope

The local toolchain is Linux x86-64, GCC 14.2, CMake 4.4.5, Ninja 1.13.2 and
Python 3.12.14. The original checkout passed strict Debug compilation, standalone
public-header compilation and all 25 existing CTest suites. The final Linux test
manifest registers 84 suites, including 73 native suites, and independent
compilation of all 70 public headers. Every one of the 67 engine translation units
is listed explicitly; all 61 runtime registrations are called by the real installer. A 600-frame empty
runtime run installed 61 modules and 78 systems without reported system failures.
The populated runtime integration suite separately exercised simulation and pixels.

Focused repair regressions retain before/after evidence where applicable.
Current frozen-checkpoint development, release and package qualification must be
read from the task receipts; earlier runs do not qualify later source changes.

An intermediate ASan/UBSan native run passed 49 suites on the unchanged source
recorded in the worker register. The associated Debug run passed all 59 suites.
Later source changes require fresh qualification. The final native sanitizer result,
its exact options and source fingerprint are recorded in the accepted task
completion note; the earlier midpoint result alone does not qualify delivery. LeakSanitizer aborts under
this execution environment's process tracing. Local instrumented runs therefore
use `ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` with raw CTest. Repository presets
and hosted sanitizer settings are unchanged. Leak detection is not qualified here.

A final sanitizer preflight caught mismatched allocation hooks in the new skeletal
regression: standard-library scratch allocation used a nothrow path. The test now
pairs that path with its replacement deallocator. The original failure log is
retained, and allocation-mismatch diagnostics remain enabled. Delivery
qualification uses the repaired test and the final source fingerprint.

No Windows, macOS, native graphical window, GPU, performance or production
qualification is inferred from these Linux checks. The runtime remains an engine
foundation; byte streaming alone does not establish typed asset import adapters.


## Development entry points

```sh
python3 tools/ge.py doctor
python3 tools/ge.py check --preset dev --jobs 2
out/build/dev/bin/ge_runtime --ticks 3 --step 0.125
python3 tools/ge.py package --preset release --jobs 2
```

`tools/ge.py run` runs the retained lifecycle fixture. `ge_runtime` installs the
composed engine. Public APIs remain under each domain's `include/ge/` root; the
combined implementations export through `ge::engine`. See
[developer setup](../development/setup.md), [runtime composition](../architecture/runtime-expansion.md)
and [packaging](../development/packaging.md) for the contracts and commands.
