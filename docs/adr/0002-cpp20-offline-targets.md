# ADR 0002: Use C++20, an offline default build, and explicit targets

**Status:** Accepted for the foundation.

## Context

The foundation needs a native executable, public contracts that can be tested
independently, and content tools another contributor can run. An undeclared
download or ambient editor installation would undermine a clean build and
relocatable delivery. Dependency boundaries should remain visible as the runtime
grows. See the [research implementation shortlist](../research/open-world-blueprint.md#a-practical-initial-implementation-stack).

## Choice

Use C++20 with CMake 3.25 or newer and a standard-library Python 3.11 or newer
tooling path. The default build downloads no dependencies. Build products belong
under `out/`; in-source builds are rejected.

Use target-scoped compile settings and public include roots:

| Target | Dependency boundary |
| --- | --- |
| `ge_core` | C++ standard library only |
| `ge_content` | `ge_core` and the standard library |
| `ge_world` | `ge_core`, standard library, and a private OS save-publication call |
| `ge_headless` | Compose the runtime targets; own application policy |

Content and world do not import each other. Runtime modules do not import tools,
tests, applications, or editor code. Adopt a third-party library only for a
demonstrated workload, with its exact revision, provenance, license, integration
checks, and a deliberate adapter boundary recorded first.

## Alternatives considered

- **One executable target:** reduces initial CMake work but hides reverse imports
  and makes reuse or independent contract testing harder.
- **Download dependencies during configure:** is convenient online but leaves
  clean builds dependent on external availability and mutable inputs.
- **Adopt the whole research shortlist immediately:** creates upgrade and license
  obligations before the prototype can test the integrations.

## Consequences

The default runtime is small and inspectable, and installed headers/libraries can
be consumed without application code. A compiler, CMake, Ninja, and Python still
must be provisioned. An offline configure is not a claim of bit-identical native
builds across toolchains. Platform success must be recorded per actual run.

Contracts: [root build](../../CMakeLists.txt), [presets](../../CMakePresets.json),
and [module boundaries](../architecture/overview.md#dependency-direction).
Checks: [quality gates F01 and F09](../quality-gates.md#foundation-acceptance-matrix).

## Revisit trigger

Revisit a boundary when a measured integration cannot fit it, or a new supported
toolchain/platform requires a documented exception. Revisit offline dependency
provisioning when a selected library is adopted; preserve explicit, reproducible
inputs rather than making configure silently fetch them.
