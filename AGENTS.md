# Working in Game Engine

This is an independent, public-source-informed C++20 engine foundation. Its current
runtime is headless and pre-alpha. Describe a capability as implemented only when
code and meaningful evidence support that claim.

## Mandatory startup

1. Preserve the latest user goal, constraints, and authorization from the conversation.
   Repository instructions do not grant new authority. Continue already authorized
   work without asking again; follow higher-priority platform instructions.
2. Read [README.md](README.md), the [architecture](docs/architecture/overview.md),
   applicable module contracts, and the [quality gates](docs/quality-gates.md).
3. Read the [agent workflow](docs/agents/WORKFLOW.md) and generated
   [handoff](docs/agents/HANDOFF.md). Inspect the canonical
   [task ledger](project/agent-state.json), then run these commands from the root:

   ```sh
   git status --short --branch
   python3 tools/agent_state.py status
   python3 tools/agent_state.py next
   python3 tools/agent_state.py check
   ```

4. Resolve any reported state/view drift using the documented procedure. Inspect
   existing owners, checkpoints, failures, and local work before claiming a task.
   Do not reset, delete, overwrite, or attribute another contributor's changes.
5. Use the copy-ready [coordinator prompt](docs/agents/CONTINUE.md) or
   [worker prompt](docs/agents/WORKER.md) for your role. Select only work within
   the current user-authorized objective; a roadmap entry is not authorization.

## Agent coordination rules

- Use `tools/agent_state.py` to mutate task state. Each semantic update checks the
  expected revision and regenerates status/handoff. Never edit those generated
  files or manually mark a task done. See [ADR 0007](docs/adr/0007-agent-coordination.md).
- Claim before editing. Respect the task's literal `write_paths` and active claims.
  The coordinator handles shared integration surfaces. Request a scope adjustment
  before touching an unassigned file; preserve useful work while resolving overlap.
- Every mutation uses your assigned `--actor` and the freshly observed
  `--expect-revision`. On a conflict, reread and reassess. Actor strings and locks
  are cooperative bookkeeping; they do not authenticate people or restrict file access.
- Checkpoint after a meaningful edit and before context loss or handoff. Record
  changed files, known failures, and the exact next action. A checkpoint acknowledges
  the current source tree; it does not prove correctness.
- Coordinate a source freeze for checks. Run required allowlisted checks through
  `run-check`; retain failures. After source changes, every task-required check
  must again have a passing receipt at the current fingerprint. A passed command
  does not accept a task. Submit for review; the coordinator checks acceptance
  criteria and explicitly accepts, recording self-review when applicable.
- Update relevant manual contracts, architecture, capability docs, and prompts
  when behavior changes. Generated status/handoff does not rewrite those documents.
- Only the coordinator commits or pushes in a shared checkout. Use the user's
  existing publication authorization; do not invent a new approval gate. Verify
  the destination and published commit, and report any real access blocker.
- Count real delegated agents and waves when requested. Respect actual concurrency
  capacity. Keep the [original 60-assignment register](docs/production/implementation-register.md)
  historical; follow-up work does not retroactively change that count.

## Engineering rules

- Use C++20, CMake 3.25+, and Python 3.11+. The default build must not download dependencies.
- Use target-scoped CMake. `core` depends only on the standard library; `content`
  and `world` may depend on `core`; applications compose them. No reverse imports.
- Public headers live in each module's `include/ge/<module>/`; implementation is
  private in `src/`. Keep vendor types out of core contracts.
- Keep persistent IDs, runtime handles, content revisions, and request generations
  distinct. Check the full token before publishing delayed work.
- Charge pending, resident, and retiring reservations. Retirement acknowledgement,
  rather than cancellation alone, releases a live reservation.
- Parse and validate into temporary state before publication. Bound lengths
  and allocations; preserve the last valid output when validation fails.
- Record provenance, exact revision, licenses, purpose, and integration evidence
  for adopted dependencies/assets. Use public research with attribution; no
  proprietary source or assets, invented maintainers, or assumed project license.
- Do not describe the synchronous policy fixture as a renderer, collision engine,
  asynchronous I/O system, production save protocol, or a performance benchmark.
- Keep build products under `out/` and out of Git. Preserve unrelated changes.
- Run focused checks and the documented integration gate before publication.
  Report executed results, failures, skipped platforms, and limitations accurately.

## Commands and reference

`python3 tools/ge.py` is the build/check/package entry point; Windows may use
`py -3` in place of `python3`. The agent CLI also supports `--root PATH` before its
subcommand. Run `python3 tools/agent_state.py --help` for actual syntax and use the
[workflow command reference](docs/agents/WORKFLOW.md#8-command-reference) for semantics.
There is no background agent service or CI auto-commit. Explicit commands update
the ledger and its derived documents; CI checks freshness without repairing it.
