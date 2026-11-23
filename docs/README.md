# Documentation

Start with [developer setup](development/setup.md) to build and run the headless
foundation, then read the architecture before changing a module. This repository
implements contract and tooling fixtures; playable-engine features and performance
targets remain roadmap work.

| Document | Use it for |
| --- | --- |
| [Developer setup](development/setup.md) | Tools, CLI commands, presets, raw CMake, output paths, and troubleshooting. |
| [Agent continuation prompt](agents/CONTINUE.md) | Copy-ready instructions for the next coordinating agent, including review and authorized publication. |
| [Agent workflow](agents/WORKFLOW.md) | Canonical task state, automatic view updates, evidence checks, concurrency and recovery. |
| [Agent worker prompt](agents/WORKER.md) | Precise delegated assignment, ownership, acceptance and handoff contract. |
| [Live project status](agents/STATUS.md) and [handoff](agents/HANDOFF.md) | Generated task status, blockers, next actions and retained evidence. |
| [Task ledger](../project/agent-state.json) | Authoritative machine-readable milestone and task coordination state. |
| [Architecture and scope](architecture/overview.md) | Module boundaries, ownership, identity, loading, and current capability limits. |
| [Architecture decisions](adr/README.md) | Accepted choices and the conditions for revisiting them. |
| [Roadmap](roadmap.md) | Milestones, dependencies, and the evidence required before expanding scope. |
| [Quality gates](quality-gates.md) | Acceptance contracts and focused checks. |
| [Content pipeline](development/content-pipeline.md) | Edit/cook/inspect/save workflow, strict authored JSON, package/state formats, and compatibility. |
| [Diagnostics](development/diagnostics.md) | Interpreting lifecycle reports, state, budgets, errors, and their limits. |
| [Packaging](development/packaging.md) | Installed runtime, relocatable SDK, archive creation, and delivery checks. |
| [Process measurements](../benchmarks/README.md) | Repeatable headless process timing with input and executable hashes. |
| [First playable milestone](production/first-playable.md) | Bounded rendering, input, collision, content, and persistence acceptance for M1. |
| [Risk register](production/risk-register.md) | Eight integration risks, experiments, owners, and acceptance conditions. |
| [Research](research/README.md) | Attributed public design input and the archived open-world blueprint. |
| [Production verification](production/verification.md) | Dated executed results, environments, retained evidence, and unverified platform coverage. |
| [Implementation and review register](production/implementation-register.md) | The 60 actual assignments across ten waves, outcomes, and evidence boundaries. |
| [Project policy](project-policy.md) | Provenance, dependency adoption, and the pending project license decision. |

Quality gates say what must be demonstrated; the verification record says what
actually ran. A design proposal, test definition, or CI configuration is not
execution evidence. Contributor workflow lives in [CONTRIBUTING.md](../CONTRIBUTING.md)
and shared-checkout rules in [AGENTS.md](../AGENTS.md).

## Dependencies and review ownership

- [Dependency and provenance policy](production/dependency-policy.md)
- [Machine-readable dependency register](../third_party/dependencies.json)
- [Responsibilities and review handoffs](production/ownership.md)
