# Quality gates and acceptance evidence

This document specifies **what must be checked**, not which runs have passed.
The [foundation verification record](production/verification.md) retains historical
execution results, source revisions, environments, failures and artifacts for that
milestone. Current runtime stabilization is described in the
[stabilization report](production/stabilization-2026-11-28.md), with source-specific
check receipts and acceptance in the [canonical task ledger](../project/agent-state.json).
A test definition or CI workflow is not a passing result. Hosted jobs require
observed execution before their platforms can be described as verified.

F01–F09 below refine the archived blueprint's
[eight expansion gates](research/open-world-blueprint.md#eight-gates-before-expanding-scope)
into a smaller headless foundation contract. They are repository acceptance IDs,
not additional claims made by the research. Their foundation scope is defined by
[ADR 0001](adr/0001-headless-foundation.md).

## Command contract

Run commands from the repository root with Python 3.11 or newer. Substitute
`py -3` on Windows where appropriate. Existing CMake/CTest commands are:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

CTest registers native, content, integration and repository-policy suites in
[tests/CMakeLists.txt](../tests/CMakeLists.txt). The focused commands
below assume this configured and built `dev` tree. The headless integration suite
launches fresh processes with explicit input paths and isolated working directories.

The unified tooling entry points are:

```sh
python3 tools/ge.py check
python3 tools/ge.py run
python3 tools/ge.py package
```

`check` runs repository checks, configuration, compilation and CTest. `run` builds
and executes the cooked lifecycle fixture. `package` builds, tests, verifies a moved
installation and independent SDK consumer, and creates archives with checksums.
The tools require their dependencies and checks to exist; failures remain nonzero.
Keep the focused checks below available for diagnosing failures. Execution results
are recorded separately in the verification record.

## Foundation acceptance matrix

Owners here are responsible module areas; assign a named reviewer when accepting
a gate. “Present” describes code and checks, with execution status in the verification
record. Each row requires retained evidence for its stated scope.

| ID / owner | Contract and current code | Exact check / retained evidence | Remaining acceptance and research trace |
| --- | --- | --- | --- |
| **F01 — Rebuild from declared inputs** / Build | The [root build](../CMakeLists.txt) and [presets](../CMakePresets.json) declare tool requirements and offline module builds. | Run the three CMake/CTest commands above; unified gate: `python3 tools/ge.py check`. Retain revision, tool versions, configure/build/test logs, configuration, and content/product hashes. | The build-time [manifest](../tools/build_manifest.py) records the observed source state, configuration, format versions and completed artifact hashes; `ge.build_manifest` checks publication and provenance behavior. A fresh checkout and independent environment must reproduce the deliverable. Manifest hashes do not establish authenticity or bit-for-bit binary reproducibility. Maps to research gate 1, reproduce the package. |
| **F02 — Independent authoring** / Content and tools | [Source fixture](../assets/fixtures/district.world.json), [cooker](../tools/cook.py), and [cooker checks](../tests/test_cooker.py) support strict source validation and cooking. | `python3 tests/test_cooker.py`; then use the edit/cook/run commands below. Retain the source change, invalid-record diagnostic, package hash, and run output. | An independent contributor must perform the documented workflow without C++ edits. Automated tests alone do not establish usability. Maps to research gate 8 and the asset production/editor section. |
| **F03 — Identity boundaries** / Core and world | [Persistent IDs/handles](../engine/core/include/ge/core/identity.hpp) separate baseline-scoped 64-bit identity from pool-local generations. [State](../engine/world/include/ge/world/state.hpp) keeps logical identity across reload/restart. | `ctest --preset dev -R '^ge[.]core$'`; `ctest --preset dev -R '^ge[.]state$'`; `ctest --preset dev -R '^ge[.]headless$'`. Retain stale-handle, generation-exhaustion, baseline, and restart evidence. | Present contract covers the fixture; world merging and distributed identity remain future scope. Maps to identity/ownership and research gate 7. |
| **F04 — Readiness publication** / World | [StreamingPolicy](../engine/world/include/ge/world/streaming.hpp) requires all requested readiness bits and checks region ID, request generation, content revision, and coordinate epoch. | `ctest --preset dev -R '^ge[.]streaming$'`; `ctest --preset dev -R '^ge[.]headless$'`. Retain rejected incomplete/stale transitions and unchanged-state evidence. | Present **symbolic** policy. Actual collision/resource readiness, asynchronous completion, and traversal deadlines require M1/M2. Maps to research gates 3 and 6. |
| **F05 — Bounded admission** / World | [Reservation accounting](../engine/world/src/streaming.cpp) tracks pending, resident, retiring, reserved, and available bytes. Cancellation keeps reservations until matching retirement acknowledgement. | `ctest --preset dev -R '^ge[.]streaming$'`; `ctest --preset dev -R '^ge[.]headless$'`. Retain capacity boundaries, overflow cases, denied admission, and charged retirement events. | Present **modeled byte** contract. M1 must establish actual allocator/process/GPU measurements and consumer retirement completion. M2 adds constrained capacity enforcement and asynchronous streaming stress against those measurements. Maps to research gates 4 and 6. |
| **F06 — Lifecycle and restoration** / Headless host | The [host](../apps/headless/main.cpp) visits distinct authored regions, releases resident handles, records policy transitions, and restores a scalar mutation. The package and state tables remain in process. | `ctest --preset dev -R '^ge[.]headless$'`; scenario entry: `python3 tools/ge.py run`. Retain event JSON, activation order, budgets, mutation/reload state, and released-handle checks. | Present logical lifecycle only. Actual unique resource working sets must exceed an enforced allowance and prove eviction/reload in M2. This row does **not** close research gate 2; it prepares gates 2 and 7. |
| **F07 — Safe deterministic cooking** / Content and tools | [Cooker](../tools/cook.py) canonicalizes semantic source and publishes complete output; [native reader](../engine/content/src/package.cpp) rejects malformed packages before returning state. | `python3 tests/test_cooker.py`; `ctest --preset dev -R '^ge[.](content|native_package|headless)$'`. Retain golden-byte/equivalent-source comparisons, native field decoding, valid-CRC structural rejection, boundary counts, and the previous product after a failed cook. | Present fixture checks; retain cooker revision and format/settings with reproducibility evidence. Real asset dependencies and incremental cooking need later gates. Maps to research gate 8 and the asset pipeline. |
| **F08 — Persistence boundary** / World and host | [Immutable snapshots and save/restore](../engine/world/include/ge/world/state.hpp) use `GESTATE1`, exact baseline/ID compatibility, sequence ordering, and checked replacement. | `ctest --preset dev -R '^ge[.]state$'`; `ctest --preset dev -R '^ge[.]headless$'`. Retain save bytes, fresh-process restored values, malformed/truncated rejection, and preservation after rejected save attempts. | Existing checks cover validation, ordering, restart, and file error paths. On supported Linux linkers, `ge.state_io_faults` injects partial-write, flush, close, and replacement failures and checks preservation and retry. Process termination, schema migration, and asynchronous publication remain planned. No power-loss durability claim. Maps to research gate 7. |
| **F09 — Relocatable delivery** / Build and release | [Install targets](../CMakeLists.txt) and [CPack configuration](../cmake/Packaging.cmake) define runtime, fixture, headers, libraries, and CMake exports. The CLI requires explicit package paths. | Release/install commands below are present. Automated gate: `python3 tools/package_smoke.py --build-dir out/build/release --config Release`; unified entry: `python3 tools/ge.py package`. Retain moved-tree runtime output, independent SDK-consumer build, manifest, and archive checksums. | Relocation checks exist; retain observed execution for each accepted platform. Test from a different directory without source-tree/editor-cache dependencies. Maps to research gate 1. |

## Reproducible authoring and process checks

To exercise the existing tool boundary, copy the documented source fixture under
`out/`, edit its authored values, then cook and run the copy:

```sh
python3 -c "from pathlib import Path; import shutil; Path('out/authoring').mkdir(parents=True, exist_ok=True); shutil.copyfile('assets/fixtures/district.world.json', 'out/authoring/district.world.json')"
python3 tools/cook.py out/authoring/district.world.json --output out/authoring/district.gepack
out/build/dev/bin/ge_headless --package out/authoring/district.gepack --inspect
out/build/dev/bin/ge_headless --package out/authoring/district.gepack
```

Use `ge_headless.exe` on Windows. The first command prepares a disposable copy;
the actual authoring exercise requires a deliberate edit, a diagnosed invalid
record, a successful recook, and evidence that the native reader used that product.
Do not count an unchanged copy as independent authoring acceptance.

For repeatable fresh-process save/restore and malformed-input coverage, run:

```sh
python3 tests/test_headless.py --binary out/build/dev/bin/ge_headless --fixture out/build/dev/assets/district.gepack
```

This suite covers `--save-state` and `--restore-state` using isolated destinations.
The CLI mutates the scenario value after restoring it; compare the reported
`restored_value` and `value_before_mutation` as well as `final_value` when assessing
restart evidence. A successful scenario JSON does not claim an actual renderer,
collision system, asynchronous I/O pipeline, or physical eviction.

## Release and relocation checks

The raw release/build/install/archive path is:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install out/build/release --config Release --prefix out/install/release
cpack --preset release
```

Acceptance additionally requires moving the install tree, running its executable
from an unrelated working directory with its installed fixture, and compiling a
small consumer against its exported CMake package. Merely creating an archive does
not establish F09. The `package_smoke.py` command in the matrix automates
these checks. Inspect [CI](../.github/workflows/ci.yml) for the declared GCC,
AppleClang, MSVC, and sanitizer jobs; report observed platform results only in the
verification record. Workflow definitions alone do not establish hosted execution.

## Evidence retention and acceptance

For each accepted gate, record the reviewer, source revision, exact commands,
toolchain/OS, configuration, fixture and package hashes, exit status, and artifact
locations. Preserve failure logs as well as the eventual passing run. Distinguish
local execution from hosted CI and native correctness from platform qualification.
Preserve historical foundation results in
[foundation verification](production/verification.md). Record current runtime checks
and acceptance against source fingerprints in the
[canonical task ledger](../project/agent-state.json), with scope and limitations in
the [stabilization report](production/stabilization-2026-11-28.md). This document
retains the foundation acceptance contract.

For lifecycle evidence, retain raw JSON and save/package fixtures. Describe its
counters as **modeled reservations**, including pending and retiring bytes.
`peak_reserved_bytes` is not allocator usage or process resident memory, and
`final_reserved_bytes == 0` does not establish release of GPU or physical assets.
The synchronous fixture cannot demonstrate race freedom or real completion latency.

## After the headless foundation

The [roadmap](roadmap.md) must add executable checks as the relevant integration
lands; no test command exists yet for the following playable gates:

| Research gate | Required later evidence |
| --- | --- |
| Reproduce the package | Independent machine builds/launches with retained inputs and no undeclared cache dependency. |
| Prove eviction | Distinct regional resource working sets exceed the allowance; allocation/retirement traces demonstrate actual eviction and reload. |
| Meet readiness deadlines | Prescribed wall-clock traversal, reversals, and destination preparation preserve required collision or enter a declared loading transition. |
| Bound peak memory | Measured CPU/GPU residency, in-flight, transient, upload, and retiring memory stay within declared caps and stabilize across repeated routes. |
| Report the frame contract | Each run records CPU critical path, GPU time, presentation intervals, p50/p95/p99/worst timings, missed-frame counts, and consecutive misses against agreed thresholds. |
| Survive asynchronous failure | Delays, cancellation, content changes, origin changes when implemented, and GPU delays cannot publish stale state, free live buffers, leak ownership pins, or deadlock admission. |
| Restore persistent state | World/mission/ownership changes survive unload and restart; interrupted writes recover a committed generation; migration and reward idempotence have fixtures. |
| Enable independent authoring | Another contributor imports/edits, diagnoses invalid content, cooks, inspects, and packages without changing engine code. |

Before accepting performance, fix the actual hardware, OS/driver, build/content
hashes, resolution, quality settings, route, cache conditions, instrumentation
overhead, and presentation mode. Keep cold launch, first traversal, and warmed
runs separate. A percentile or short soak does not establish every-frame deadlines
or indefinite stability. These are future measurement requirements, with no claimed
hardware result or calendar commitment.
