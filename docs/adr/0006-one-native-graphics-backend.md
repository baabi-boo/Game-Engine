# ADR 0006: Prove one native graphics backend before platform expansion

**Status:** Accepted sequencing decision; graphics backend selection pending.

## Context

The research lists several graphics and platform candidates, not an integrated
distribution. Every backend adds shader, capability, synchronization, resource
lifetime, diagnostics, and presentation tests. The headless compiler matrix can
check portable core code without establishing graphics support on those systems.
See [research platform scope](../research/open-world-blueprint.md#platforms-and-the-public-knowledge-boundary).

## Choice

Select one native desktop platform and one graphics backend for the first
playable character **or** vehicle route. Choose using the actual reference
machine, required features, available debugging tools, dependency terms, and
measured integration cost. Record that selection in a follow-up ADR; no Vulkan,
Direct3D, SDL GPU, or other backend is currently integrated or selected here.

The first renderer must connect cooked content, resource readiness, retirement,
camera/input, and collision-safe movement. Declare resource and frame contracts
before performance acceptance. Keep platform services and vendor APIs behind
boundaries justified by that integration, without building empty abstractions
for every future platform.

## Alternatives considered

- **Implement multiple backends together:** tests abstraction ideas early but
  multiplies work before one complete route establishes the requirements.
- **Choose an abstraction solely for its platform list:** does not demonstrate
  feature parity, lifetime behavior, or measured performance for this workload.
- **Make browser/console support a consequence of C++ portability:** overlooks
  separate loading, memory, storage, lifecycle, and qualification requirements.

## Consequences

The playable milestone has a bounded integration and measurement surface.
Portable headless tests remain useful, but their results must be stated separately
from playable support. A second backend/platform needs its own test matrix and
retained evidence. Browser delivery needs explicit asynchronous loading,
persistence completion, threading, graphics, and memory budgets. Console work
requires authorized platform access and its own qualification; public research
does not supply gated SDKs or establish console readiness.

Current boundary: [headless target](../../apps/headless/CMakeLists.txt).
Future acceptance: [playable milestone](../roadmap.md#m1-playable-feasibility-route)
and [expanded evidence gates](../quality-gates.md#after-the-headless-foundation).

## Revisit trigger

Select the backend when the first playable workload and reference platform are
declared. Expand only when a named product requirement justifies the additional
platform and resources exist to run its full integration and release gates.
