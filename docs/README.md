# Documentation

Start with [developer setup](development/setup.md) to build and run the headless
foundation, then read the architecture before changing a module. This repository
implements contract and tooling fixtures; playable-engine features and performance
targets remain roadmap work.

| Document | Use it for |
| --- | --- |
| [Developer setup](development/setup.md) | Tools, CLI commands, presets, raw CMake, output paths, and troubleshooting. |
| [Architecture and scope](architecture/overview.md) | Module boundaries, ownership, identity, loading, and current capability limits. |
| [Architecture decisions](adr/README.md) | Accepted choices and the conditions for revisiting them. |
| [Roadmap](roadmap.md) | Milestones, dependencies, and the evidence required before expanding scope. |
| [Quality gates](quality-gates.md) | Acceptance contracts and focused checks. |
| [Content pipeline](development/content-pipeline.md) | Edit/cook/inspect/save workflow, strict authored JSON, package/state formats, and compatibility. |
| [Packaging](development/packaging.md) | Installed runtime, relocatable SDK, archive creation, and delivery checks. |
| [Research](research/README.md) | Attributed public design input and the archived open-world blueprint. |
| [Production verification](production/verification.md) | Dated executed results, environments, retained evidence, and unverified platform coverage. |
| [Project policy](project-policy.md) | Provenance, dependency adoption, and the pending project license decision. |

Quality gates say what must be demonstrated; the verification record says what
actually ran. A design proposal, test definition, or CI configuration is not
execution evidence. Contributor workflow lives in [CONTRIBUTING.md](../CONTRIBUTING.md)
and shared-checkout rules in [AGENTS.md](../AGENTS.md).

## Dependencies and review ownership

- [Dependency and provenance policy](production/dependency-policy.md)
- [Machine-readable dependency register](../third_party/dependencies.json)
- [Responsibilities and review handoffs](production/ownership.md)
