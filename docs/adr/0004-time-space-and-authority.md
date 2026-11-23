# ADR 0004: Separate clocks, declare coordinates, and use one state writer

**Status:** Accepted; spatial adapter conventions apply to future integrations.

## Context

Streaming, movement, save capture, and presentation must agree about when a value
is valid and who can change it. Wall-clock elapsed time, simulation time, and
world-calendar time serve different purposes. Coordinate conversions must become
explicit before graphics or collision begins. See the research
[time](../research/open-world-blueprint.md#time-and-execution) and
[coordinate](../research/open-world-blueprint.md#coordinates) contracts.

## Choice

Use `FixedClock` as a caller-driven, single-threaded fixed-step accumulator with
integer nanosecond accounting. Its configured catch-up limit bounds scheduled
steps; excess whole steps are reported as dropped time and a fractional remainder
is retained. It does not read the wall clock, execute jobs, or prescribe a global
simulation frequency. Future profiling/deadlines use monotonic elapsed time;
game-calendar time will have a separate pause and catch-up policy.

The current package stores signed integer millimetres. Adopt metres for future
runtime spatial math, right-handed coordinates, +Y up, and -Z forward; perform
conversions at asset and subsystem boundaries. A coordinate epoch invalidates
delayed results computed in an older space. Carrying that label does not perform
origin shifting, and no global-position precision scheme is selected yet.

The headless host is the sole state writer. Future workers may prepare results,
but an owning simulation phase validates and publishes them. Capture immutable
save snapshots at a defined boundary. Controller, physics, animation, and
possession integrations must name one authoritative transform writer and an
explicit handoff before they are added.

## Alternatives considered

- **Variable-step authoritative updates:** directly couples simulation behavior
  to rendering and pause duration.
- **Unbounded catch-up:** can turn one delay into sustained overload.
- **Concurrent mutation without phase ownership:** obscures ordering and snapshot
  consistency; locks alone do not define authority.
- **Planet-scale coordinates immediately:** adds conversion and history-management
  work without a measured precision requirement.

## Consequences

Clock accounting and scalar state transitions have testable boundaries. A fixed
clock does not prove deterministic physics across platforms, and single-writer
ownership does not require a global lock around future independent work. Actual
threading, spatial transforms, and origin-shift fan-out remain integration work.

Contracts/checks: [clock](../../engine/core/include/ge/core/clock.hpp),
[core tests](../../tests/core_tests.cpp), [state](../../engine/world/include/ge/world/state.hpp),
and [headless composition](../../apps/headless/main.cpp).

## Revisit trigger

Revisit step frequency, phase order, and coordinate precision using the first
playable route's movement, collision, and timing measurements. Require coordinated
invalidation tests before introducing asynchronous publication or origin shifts.
