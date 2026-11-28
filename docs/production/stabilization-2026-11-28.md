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

Work is in progress. The first 22 distinct workers have completed their assigned
reviews or repairs. Further subsystem review, repairs and independent integration
reviews remain. The active `AUDIT-20261128` task and its source-matching receipts
are the authoritative acceptance record. A published intermediate checkpoint is
not completion of the 100-worker request or production qualification.

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
cache lifetime defect; its repair is queued for the next checkpoint.

## Verification scope

The local toolchain is Linux x86-64, GCC 14.2, CMake 4.4.5, Ninja 1.13.2 and
Python 3.12.14. The original checkout passed strict Debug compilation, standalone
public-header compilation and all 25 existing CTest suites. A 600-frame empty
runtime run installed 61 modules and 78 systems without reported system failures.
The populated runtime integration suite separately exercised simulation and pixels.

Focused repair regressions retain before/after evidence where applicable.
Current frozen-checkpoint development, release and package qualification must be
read from the task receipts; earlier runs do not qualify later source changes.

An exploratory ASan/UBSan native run passed 21 suites. LeakSanitizer aborts under
this execution environment's process tracing. Local instrumented runs therefore
use `ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` with raw CTest. Repository presets
and hosted sanitizer settings are unchanged. Leak detection is not qualified here.

No Windows, macOS, native graphical window, GPU, performance or production
qualification is inferred from these Linux checks. The runtime remains an engine
foundation; byte streaming alone does not establish typed asset import adapters.
