# Game Engine

[![CI](https://github.com/baabi-boo/Game-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/baabi-boo/Game-Engine/actions/workflows/ci.yml)

**Independent open-world engine · foundation / pre-alpha**

## Current runtime and verification

The current branch contains a **C++20 headless engine runtime** in `engine/`
and the `ge_runtime` application. It connects subsystems through
a shared service registry, ordered frame systems, bounded event queues and
runtime commands. The existing `ge_headless` lifecycle fixture is retained.

The runtime includes CPU software rendering, collision and dynamics, animation,
navigation, audio processing, asynchronous asset work, and gameplay/network services.
It remains pre-alpha: there is no native graphical window, GPU renderer or editor GUI,
and the implemented services do not establish a complete playable game or a
performance target. See [runtime composition](docs/architecture/runtime-expansion.md)
for ownership and scheduling contracts.

The original [source campaign](docs/production/source-campaign.json) excluded builds
and test execution. The later [integration audit](docs/production/integration-audit-2026-11-28.md)
and current [stabilization review](docs/production/stabilization-2026-11-28.md)
compile, exercise and repair that source. The
[actual-worker register](docs/production/stabilization-register-2026-11-28.json)
records review coverage; the [task ledger](project/agent-state.json) records checks
against source fingerprints. Passing results qualify their recorded revision,
environment and scope. They do not establish production readiness.

A C++20 engine foundation built around explicit ownership, stable identity, bounded
resource admission and reproducible content tools. The first executable demonstrates
the lifecycle **edit → cook → load → interact → retire → restore → package** using
original synthetic content.

The retained `ge_headless` executable is a **lifecycle contract fixture**. Its
loading and memory counters model policy; they do not measure real asset-memory
eviction or graphics performance. Its historical evidence is separate from the
composed runtime's subsystem and integration checks.

## Start here

Required: a C++20 compiler, CMake **3.25+**, Ninja, and Python **3.11+**.
The default build uses the C++ and Python standard libraries and downloads nothing.
Windows users should run Ninja builds from a Visual Studio Developer PowerShell.

```sh
python3 tools/ge.py doctor
python3 tools/ge.py check
python3 tools/ge.py run
out/build/dev/bin/ge_runtime --ticks 3 --step 0.125
python3 tools/ge.py package
```

`tools/ge.py run` launches the lifecycle fixture; run `ge_runtime` directly to
exercise the composed engine. On Windows, replace `python3` with `py -3` when
needed and use `ge_runtime.exe`. Outputs belong under `out/`.
See [developer setup](docs/development/setup.md) for compiler details, raw CMake
commands, diagnostics and platform verification limits.

## Continue with agents

Give a coordinating agent the [continuation prompt](docs/agents/CONTINUE.md), then
have it inspect the [live status](docs/agents/STATUS.md) and
[handoff](docs/agents/HANDOFF.md). The [workflow guide](docs/agents/WORKFLOW.md)
defines task claims, checkpoints, review, source freshness and recovery. Use the
[worker prompt](docs/agents/WORKER.md) for bounded delegated assignments.

```sh
python3 tools/agent_state.py status
python3 tools/agent_state.py next
python3 tools/agent_state.py check
```

The [task ledger](project/agent-state.json) is authoritative. Explicit workflow
commands update it and regenerate status and handoff documents; build and CI checks
reject stale source snapshots or generated views. Completion requires fresh recorded
checks and an explicit review. This is cooperative coordination within the checkout;
it does not launch agents or run unattended work. Future engine features remain
planned until their acceptance criteria have been demonstrated.

## What is in the repository

| Area | Purpose |
| --- | --- |
| [`engine/core`](engine/core) | Identity, bounded allocation, binary codecs, signals and the runtime kernel |
| [`engine/content`](engine/content) | Strict, bounded cooked-package reader |
| [`engine/world`](engine/world) | Readiness, budget/residency policy and persistent scalar state |
| [`engine/runtime`](engine/runtime) | Explicit build manifest for the combined `ge_engine` domain library |
| [`engine/`](engine) | Scene/ECS, jobs/I/O, assets, rendering, physics, animation, audio and gameplay domains |
| [`apps/headless`](apps/headless) | Executable lifecycle and diagnostic scenario |
| [`apps/runtime`](apps/runtime) | Domain registration, command host, frame advancement and JSON reports |
| [`tools`](tools) | Authoring validation, deterministic cooking, checks and packaging |
| [`assets`](assets) | Small, original authored fixtures with documented provenance |
| [`tests`](tests) | Contract, malformed-input, lifecycle and relocation coverage |
| [`project`](project) | Agent task ledger, dependencies, claims, evidence and history |
| [`docs`](docs) | Architecture, decisions, development, evidence gates and research |

The build exports `ge::core`, `ge::content`, `ge::world` and `ge::engine`.
Domain headers have separate include roots; their implementations share the
`ge_engine` library. The [module boundary policy](docs/architecture/module-boundaries.md)
declares allowed imports and keeps public dependencies acyclic. Runtime modules
cannot import application code or depend on the editor domain. New dependencies
require a recorded purpose, pinned revision, provenance and license review.

## Design and development

- [Architecture and scope](docs/architecture/overview.md)
- [Architecture decisions](docs/adr/README.md)
- [Content pipeline and file format](docs/development/content-pipeline.md)
- [Lifecycle diagnostics](docs/development/diagnostics.md)
- [Roadmap and milestone gates](docs/roadmap.md)
- [First playable milestone](docs/production/first-playable.md) and [integration risks](docs/production/risk-register.md)
- [Quality and acceptance evidence](docs/quality-gates.md)
- [Observed verification results](docs/production/verification.md)
- [60-agent implementation and review register](docs/production/implementation-register.md)
- [Dependency and provenance policy](docs/production/dependency-policy.md)
- [Contributing](CONTRIBUTING.md) and [agent guidance](AGENTS.md)
- [Original public research blueprint](docs/research/open-world-blueprint.md)

The blueprint is design input, with its own earlier 63-assignment research register
and 153 reference URLs. This repository setup uses a separate ten-wave implementation
and review program. The proposed district size, population, 1080p/60 Hz target, staffing
and schedule remain assumptions to test, not achieved engine capabilities.

## Project status and provenance

The code and fixture content are original. Public descriptions of open-world techniques
inform the design; this is not Rockstar's RAGE, a reconstruction of its private internals,
or a Rockstar-affiliated project. No proprietary game code, assets or console SDKs are
included.

No project software license has been selected by the owner. This setup introduces no
additional license grant. See [project policy](docs/project-policy.md) before external
reuse or distribution. Dependency and platform expansion follow the documented gates.
