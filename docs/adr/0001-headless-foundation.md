# ADR 0001: Establish a headless foundation before playable integration

**Status:** Accepted for the foundation.

## Context

The research proposes an edit–cook–stream–interact–evict–restore–package loop.
A playable version requires graphics, collision, input, real resource lifetimes,
and an authoring workflow to agree on identity and ownership. Introducing those
systems together would make basic contract failures difficult to isolate.
The repository therefore starts below the blueprint's playable feasibility core.
See [research scope levels](../research/open-world-blueprint.md#three-levels-of-scope).

## Choice

Build a native headless executable that composes `ge_core`, `ge_content`, and
`ge_world`. Exercise generation-checked handles, a bounded clock, strict package
loading, readiness publication, reservation retirement, and persistent scalar
state using original synthetic content. Expose observable events and explicit
failure results through the CLI.

Readiness bits are supplied fixture signals. Pending, resident, and retiring
bytes are modeled reservations. Logical unload and handle retirement do not
demonstrate physical asset eviction. There is no integrated renderer, physics
solver, asynchronous storage path, or job scheduler in this milestone.

## Alternatives considered

- **Begin with the whole district:** provides an early visual demonstration but
  combines too many unproven lifetime and tooling contracts.
- **Start with a rendering showcase:** tests visual techniques while leaving the
  essential state, authoring, and resource lifecycle unresolved.
- **Create interfaces for every planned subsystem:** adds maintenance and suggests
  unsupported capabilities before a workload can validate their boundaries.

## Consequences

The foundation can test failure paths without a GPU or third-party runtime.
Its evidence supports only the narrower [F01–F09 contracts](../quality-gates.md).
It cannot establish collision safety, real memory limits, traversal deadlines,
visual quality, or a frame-rate claim. The next milestone must produce a playable
character **or** vehicle with one backend and real collision.

Implementation: [headless host](../../apps/headless/main.cpp).
Checks: [native contract suites](../../tests/CMakeLists.txt) and
[process integration suite](../../tests/test_headless.py).

## Revisit trigger

Reassess this milestone boundary when the foundation gates have retained evidence
and a bounded playable route is ready to drive graphics and collision integration.
Do not keep extending symbolic simulations in place of those integrations.
