# ADR 0007: Coordinate agents with explicit state and source-bound evidence

**Status:** Accepted operational design; execution results belong in task receipts
and the [verification record](../production/verification.md).

## Context

Multiple contributors can lose conversational context, edit overlapping files,
reuse stale test results, or describe planned work as complete. The repository's
original [60-assignment setup register](../production/implementation-register.md)
records a completed effort; it is not a live queue for future agents. The project
needs one inspectable current task ledger and precise resumption instructions.

The default toolchain remains offline and standard-library-only. Coordination
should work in a normal checkout or source archive without a hosted agent service.
It must preserve the user objective and existing authorization, protect unrelated
work, and distinguish historical evidence from current acceptance.

## Choice

Use [project/agent-state.json](../../project/agent-state.json) as canonical task
state, managed by [tools/agent_state.py](../../tools/agent_state.py). Explicit
semantic commands validate and advance one revision, append history, and regenerate
[STATUS.md](../agents/STATUS.md) and [HANDOFF.md](../agents/HANDOFF.md). Generated
views are deterministic from stored state. Checks observe freshness without repairing
tracked files; `sync` repairs only derived views.

Tasks declare dependencies, literal write scopes, acceptance criteria, required
checks, owners, and actionable checkpoints. Planned work becomes claimed work;
submission requires current evidence; the configured coordinator accepts review
explicitly. Failed checks remain recorded. A matching actor/owner requires explicit
self-review. Completed work remains historical; regressions become new tasks.

Use compare-and-swap revisions and a cooperative local transaction lock to reject
stale writes. A separate run lock serializes allowlisted check execution against
shared output trees. Claim scopes compare path components with case folding.
These controls coordinate cooperating agents in one checkout. They do not
authenticate identities, enforce source-file permissions, or provide distributed locks.

Fingerprint regular source inputs, including tests, tooling, manual documentation,
and agent prompts. Exclude the ledger, generated views, and documented local/build
outputs. Normalize CRLF to LF for UTF-8 NUL-free text; preserve binary bytes. Capture
source before and after each fixed-argv check. A changed source tree invalidates
the result; a latest failure cannot be hidden by an earlier pass.

Retain compact execution receipts in the ledger and full logs under ignored
`out/agent-runs/`. Clones need not contain ignored logs to inspect historical
metadata. Receipts describe actual local execution; they are not signed provenance
or proof that a test semantically establishes every acceptance criterion.

Publish JSON atomically, then the two derived files. All three replacements are
not one atomic transaction. If derived publication fails, JSON remains authoritative
and `sync` repairs the views. Never remove locks automatically based on age or PID;
inspect ownership, preserve work, stop the confirmed process, and recover explicitly.

Use [WORKFLOW.md](../agents/WORKFLOW.md), the
[coordinator prompt](../agents/CONTINUE.md), and
[worker prompt](../agents/WORKER.md) as the operational contract. No background
daemon, automatic task completion, CI auto-commit, or extra approval policy is introduced.

## Alternatives considered

- **Conversation-only handoffs:** quick initially, but omit ownership, failed runs,
  and next actions when context is lost or a different agent resumes.
- **Manually edited status documents:** simple to write, but create conflicting
  descriptions and allow evidence to drift independently of source changes.
- **Hosted database or autonomous orchestration service:** can support broader
  coordination, but adds credentials, dependencies, availability requirements,
  and authority concerns beyond the present repository need.
- **Automatically finish tasks after passing tests:** removes review of acceptance
  semantics, scope, compatibility, and the limits of test coverage.
- **Treat Git HEAD alone as freshness:** misses uncommitted inputs and changes
  identity on a content-equivalent commit. Source fingerprints describe tested inputs.

## Consequences

New agents can recover current scope and checkpoints from inspectable files.
State mutations update handoff documents in the same operation, and ordinary
checks can reject stale source or generated views. Check execution and acceptance
remain separate. The existing engine lifecycle and provenance rules still apply.

The whole-tree fingerprint is deliberately conservative: unrelated source edits
invalidate current task evidence. Contributors must coordinate a stable source
window for integration checks. Local locks do not coordinate separate clones,
prevent manual edits, or establish trustworthy review identity. The coordinator
must integrate changes, evaluate evidence, preserve user authorization, and publish.

Logs are local unless deliberately retained elsewhere; stored digests cannot
reconstruct missing logs. The CLI cannot prove semantic correctness, hosted CI
success, platform qualification, or AAA production readiness. The historical
foundation baseline must not be relabeled as a newly executed check.

## Revisit trigger

Revisit this decision when concurrent clones need shared ownership, the ledger
becomes too large to review, required retention exceeds Git/local logs, or a real
product requirement calls for authenticated attestations or hosted orchestration.
Document migration, identity/authorization boundaries, failure recovery, and
independent checks before replacing the current contract. Preserve existing
history and the original setup register through any migration.
