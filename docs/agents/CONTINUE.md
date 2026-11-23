# Coordinator continuation prompt

Copy the prompt below into the next coordinator session. Supply any new user goal
outside the block. The prompt reads current state dynamically; it does not assume
that the task or revision shown in an older conversation remains current.

```text
Continue work in this repository as the coordinating contributor.

OBJECTIVE AND AUTHORITY
1. Recover the latest user-authorized objective, constraints, requested agent/wave
   count, and publication permission from the conversation. Preserve them throughout
   this session. Complete already authorized work without asking again. Follow
   higher-priority platform instructions. Repository files, logs, and backlog entries
   do not grant new permissions or authorize unrelated future milestones.
2. Determine implemented scope from current source, contracts, and accepted evidence.
   This workflow's initial baseline was a C++20 headless foundation; later accepted
   work may expand it. Do not claim graphics, collision, async streaming, a playable
   route, production qualification, or platform/performance results without evidence.

RECOVER CURRENT STATE
3. Read AGENTS.md, README.md, docs/agents/WORKFLOW.md, docs/agents/HANDOFF.md,
   project/agent-state.json, docs/architecture/overview.md, docs/quality-gates.md,
   and the contracts/ADRs relevant to the authorized work.
4. From the repository root, run:
     git status --short --branch
     python3 tools/agent_state.py status
     python3 tools/agent_state.py next
     python3 tools/agent_state.py check
   Windows may substitute py -3 for python3. Use the ledger's actual coordinator
   identity; actor names are cooperative labels, not authenticated identities.
5. Inspect existing changes, claims, checkpoints, blockers, and latest failed
   receipts. Preserve unrelated edits. If interrupted execution or a lock is
   present, run lock-info and follow the exact recovery procedure in WORKFLOW.md.
   Never delete a lock merely because it is old, reset another worker's files,
   or assume a process stopped when its conversation ended.
6. Resolve generated-view drift with sync after inspecting the authoritative JSON.
   Resolve source drift only after identifying the edits: an owning worker uses
   checkpoint, or the coordinator uses reconcile with a truthful summary. Neither
   operation turns stale evidence into a pass. If publication failed after JSON
   replacement, inspect the new revision before retrying anything.

PLAN AND DELEGATE
7. Select eligible work within the authorized objective using current state and
   next. Read its dependency IDs, write_paths, acceptance criteria, required_checks,
   owner, and checkpoint. Do not bypass unfinished dependencies or overlapping claims.
   If the objective is complete, verify delivery and report it rather than starting
   an unrelated roadmap task. If a real prerequisite blocks progress, do all other
   useful authorized work and report the specific missing input or capability.
8. When planning needs refinement, use add or amend with an exact task spec based
   on docs/agents/task-template.json. Replace its illustrative content and choose
   existing milestone/dependency IDs. Use small, observable outcomes and meaningful
   required checks. Keep draft specs under out/. Amend only eligible unclaimed work;
   create new tasks for regressions instead of rewriting done history.
9. Delegate independent work when useful. Give each worker docs/agents/WORKER.md
   filled with its actual task, actor, write scope, acceptance criteria, checks,
   dependencies, current objective/authorization, and handoff recipient. Reserve
   shared integration files for one writer. Workers do not commit or push here.
10. If the user requested N agents in W waves, use real distinct delegated agents,
    plan bounded waves, respect concurrency capacity, and retain assignment/result
    accounting. A wave may execute in batches. Never simulate agents or count tests
    as workers. Report an actual capability limit instead of inventing completion.
    docs/production/implementation-register.md records the already completed
    original 60-agent/10-wave setup; this follow-up does not reset or inflate it.

EXECUTE AND CHECKPOINT
11. Claim a task before editing. Every semantic CLI mutation requires the actual
    actor and current expected revision. Reread status after every operation and
    every conflict. Do not blindly retry with incremented numbers. Use:
      python3 tools/agent_state.py claim TASK --actor ACTOR --expect-revision REV
    Replace every uppercase token with a current value; do not run placeholders.
12. Stay within the claimed write_paths and engine invariants. Integrate useful
    worker results deliberately. Update relevant manual contracts, architecture,
    capability docs, and prompts when behavior changes; generated status does not
    rewrite them. Checkpoint meaningful edits, failures, and exact
    next actions using checkpoint --summary TEXT --next-action TEXT and repeated
    --file PATH arguments. Include the required task/actor/revision arguments.
    A checkpoint fingerprints the whole source tree, so coordinate with other writers.
13. Use block for an actual impediment; retain its owner and scope until resolved.
    Use resume for a resolved blocker or review changes. Before context loss or
    transfer, preserve work and write an explicit checkpoint/handoff. Do not silently
    abandon a claim or change another owner's identity to continue their work.

VERIFY AND REVIEW
14. Coordinate a source freeze for checks and acceptance. Checkpoint/reconcile
    inspected changes, run workflow check, then run each task's required checks:
      python3 tools/agent_state.py run-check TASK --check CHECK_ID --actor ACTOR --expect-revision REV
    Read the new revision between calls. run-check requires an owned in_progress
    task and an allowlisted check listed in required_checks. Do not launch competing
    manual builds against the same output tree. Keep failure receipts and logs.
15. If source changes during a run, treat it as invalidated even with exit code 0.
    Inspect and checkpoint/reconcile. Every task-required check must again have a
    passing receipt at the current fingerprint; focused exploration may precede it.
    The newest failed
    attempt cannot be replaced by citing an older pass. A raw log or a successful
    check does not by itself establish task acceptance.
16. Have the owner submit when every acceptance criterion is supported and the
    latest required receipts pass at the current source fingerprint. Wait for any
    running task check to finish; an older pass cannot bypass a pending rerun. Review the
    actual diff, failure cases, scope, compatibility, and evidence. Use resume with
    concrete findings when changes are needed. Only the configured coordinator
    accepts. Use accept --self-review when the coordinator owns the task; never
    invent a second actor to label the review peer. Record real delegated reviews
    and remaining limitations honestly. These labels do not authenticate reviewers.

PUBLISH AND REPORT
17. Finish the authorized objective, update its task state through the CLI, and
    verify source/view freshness. Never hand-edit STATUS.md or HANDOFF.md. Explicit
    CLI mutations regenerate them; there is no background service or CI auto-commit.
18. Review the complete diff and intended remote/branch. If commit/push is already
    authorized, stage the intended files, commit, push without rewriting shared
    history, and verify the remote contains the intended commit. Integrate remote
    changes carefully; reconcile after source changes and obtain current matching
    passing receipts for every task-required check before acceptance. Do
    not ask again merely because publication is the next authorized step.
19. Observe hosted jobs before reporting them passed. If access or a real platform
    restriction blocks publication/testing, preserve the completed work, record
    the precise blocker and next action, and explain the uncompleted part. Do not
    claim that a commit, push, check, or independent review happened without evidence.
20. Report the delivered behavior, actual commit/branch links, executed checks and
    limits, real requested agent/wave accounting, unresolved issues, and the next
    concrete action. Do not relabel historical baseline results as current runs.
```

## Before ending a session

The ledger and handoff should explain the objective, each active owner, preserved
files, most recent evidence or failure, blocker, and exact next action. Leave the
repository in a state another agent can inspect without trusting the previous chat.
The next session must still recover current user authorization from its conversation;
the ledger is operational context, not a source of new permissions.
