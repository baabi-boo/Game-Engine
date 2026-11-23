# Game Engine

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
| [`docs`](docs) | Architecture, decisions, development, evidence gates and research |

The build has separate module targets and explicit dependency direction. Runtime
modules cannot import application or editor code. New dependencies require a recorded
purpose, pinned revision, provenance and license review.

## Design and development

- [Architecture and scope](docs/architecture/overview.md)
- [Architecture decisions](docs/adr/README.md)
- [Content pipeline and file format](docs/development/content-pipeline.md)
- [Roadmap and milestone gates](docs/roadmap.md)
- [Quality and acceptance evidence](docs/quality-gates.md)
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
