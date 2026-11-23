# Game Engine

[![CI](https://github.com/baabi-boo/Game-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/baabi-boo/Game-Engine/actions/workflows/ci.yml)

**Independent open-world engine · foundation / pre-alpha**

A C++20 engine foundation built around explicit ownership, stable identity, bounded
resource admission and reproducible content tools. The first executable demonstrates
the lifecycle **edit → cook → load → interact → retire → restore → package** using
original synthetic content.

This milestone is a **headless contract prototype**. It precedes a playable engine:
there is no renderer, physical collision, vehicle controller, editor GUI or asynchronous
storage pipeline yet. Its loading and memory counters model policy; they do not measure
real asset-memory eviction or establish a frame-rate target.

## Start here

Required: a C++20 compiler, CMake **3.25+**, Ninja, and Python **3.11+**.
The default build uses the C++ and Python standard libraries and downloads nothing.
Windows users should run Ninja builds from a Visual Studio Developer PowerShell.

```sh
python3 tools/ge.py doctor
python3 tools/ge.py check
python3 tools/ge.py run
python3 tools/ge.py package
```

On Windows, replace `python3` with `py -3` when needed. Outputs belong under `out/`.
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
| [`engine/core`](engine/core) | Persistent IDs, generation-counted handles and bounded fixed-step timing |
| [`engine/content`](engine/content) | Strict, bounded cooked-package reader |
| [`engine/world`](engine/world) | Readiness, budget/residency policy and persistent scalar state |
| [`apps/headless`](apps/headless) | Executable lifecycle and diagnostic scenario |
| [`tools`](tools) | Authoring validation, deterministic cooking, checks and packaging |
| [`assets`](assets) | Small, original authored fixtures with documented provenance |
| [`tests`](tests) | Contract, malformed-input, lifecycle and relocation coverage |
| [`project`](project) | Agent task ledger, dependencies, claims, evidence and history |
| [`docs`](docs) | Architecture, decisions, development, evidence gates and research |

The build has separate module targets and explicit dependency direction. Runtime
modules cannot import application or editor code. New dependencies require a recorded
purpose, pinned revision, provenance and license review.

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
