#!/usr/bin/env python3
"""Exercise the agent workflow through public APIs and real CLI processes.

Run ``python3 tests/test_agent_state.py [-v]``. Fixtures are tiny source
archives; their allowlisted repository checker never configures an engine
build. Local Git tests skip if Git is unavailable.
"""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[1]
TOOL = REPOSITORY / "tools/agent_state.py"
STATE_PATH = "project/agent-state.json"
VIEWS = ("docs/agents/STATUS.md", "docs/agents/HANDOFF.md")
STAMP = "2026-01-02T03:04:05Z"
GIT = shutil.which("git")
_SPEC = importlib.util.spec_from_file_location("ge_agent_state_under_test", TOOL)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load agent workflow: {TOOL}")
workflow = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = workflow
_SPEC.loader.exec_module(workflow)

CHECKER = '''from pathlib import Path
import sys
import time

root = Path(__file__).resolve().parents[1]
out = root / "out"
out.mkdir(exist_ok=True)
print("fixture checker says success", flush=True)
if (out / "hold").exists():
    (out / "started").write_text("started", encoding="utf-8")
    deadline = time.monotonic() + 20
    while not (out / "release").exists():
        if time.monotonic() > deadline:
            sys.exit(19)
        time.sleep(0.01)
if (out / "mutate").exists():
    with (root / "README.md").open("a", encoding="utf-8") as stream:
        stream.write("Source changed during the check.\\n")
sys.exit(7 if (out / "fail").exists() else 0)
'''


def task(task_id: str = "TASK-001", *, paths: tuple[str, ...] = ("engine/world",),
         dependencies: tuple[str, ...] = (), checks: tuple[str, ...] = ("repository",),
         priority: int = 1) -> dict:
    return {
        "id": task_id, "title": f"Implement {task_id}", "milestone": "M0",
        "priority": priority, "description": "Make the scoped fixture change.",
        "depends_on": list(dependencies), "write_paths": list(paths),
        "acceptance": ["The scoped change is verified."], "required_checks": list(checks),
        "status": "planned", "owner": None, "claim_revision": None,
        "checkpoint": {"summary": "Not started.", "next_action": "Implement the fixture.",
                       "files": []},
        "blocker": None, "evidence": [], "completion": None,
    }


def definition(value: dict) -> dict:
    names = ("id", "title", "milestone", "priority", "description", "depends_on",
             "write_paths", "acceptance", "required_checks")
    return {**{name: copy.deepcopy(value[name]) for name in names},
            "next_action": value["checkpoint"]["next_action"]}


class AgentStateTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ge-agent-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "source archive"
        self.root.mkdir()
        (self.root / "tools").mkdir()
        (self.root / "out").mkdir()
        (self.root / "README.md").write_text("# Fixture engine\n", encoding="utf-8")
        (self.root / "tools/check_repo.py").write_text(CHECKER, encoding="utf-8")
        self.seed()

    def seed(self, tasks: list[dict] | None = None) -> dict:
        state = {
            "schema_version": 1, "revision": 1, "updated_at": STAMP,
            "project": {"name": "Fixture Engine", "objective": "Verify agent coordination.",
                        "current_milestone": "M0", "coordinator": "coordinator",
                        "truth_documents": ["README.md"], "known_limits": ["Fixture only."]},
            "source_snapshot": workflow.snapshot_source(self.root),
            "milestones": [
                {"id": name, "title": f"Milestone {name}", "objective": "Verify the work.",
                 "exit_criteria": ["Accepted evidence exists."]} for name in ("M0", "M1")
            ],
            "tasks": copy.deepcopy(tasks if tasks is not None else [task()]),
            "evidence": [],
            "history": [{"revision": 1, "at": STAMP, "actor": "coordinator",
                         "action": "bootstrap", "task": None, "summary": "Initial fixture."}],
        }
        self.install(state)
        return state

    def install(self, state: dict) -> None:
        workflow.validate_state(state)
        path = self.root / STATE_PATH
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        for name, content in workflow.render_documents(state).items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")

    def state(self) -> dict:
        return workflow.load_state(self.root)

    def tracked(self) -> dict[str, bytes]:
        return {name: (self.root / name).read_bytes() for name in (STATE_PATH, *VIEWS)}

    def environment(self, overrides: dict[str, str] | None = None) -> dict[str, str]:
        environment = {key: value for key, value in os.environ.items()
                       if not key.upper().startswith("GIT_")}
        environment.update(overrides or {})
        return environment

    def command(self, *arguments: str) -> list[str]:
        return [sys.executable, str(TOOL), "--root", str(self.root), *arguments]

    def cli(self, *arguments: str, cwd: Path | None = None,
            environment: dict[str, str] | None = None, timeout: int = 15) -> subprocess.CompletedProcess:
        return subprocess.run(self.command(*arguments), cwd=cwd or self.directory,
                              env=self.environment(environment), capture_output=True,
                              text=True, encoding="utf-8", errors="backslashreplace", timeout=timeout)

    def mutation(self, command: str, *arguments: str, actor: str = "worker",
                 revision: int | None = None, succeeds: bool = True,
                 cwd: Path | None = None, environment: dict[str, str] | None = None
                 ) -> subprocess.CompletedProcess:
        if revision is None:
            revision = self.state()["revision"]
        before = self.tracked()
        result = self.cli(command, *arguments, "--actor", actor, "--expect-revision", str(revision),
                          cwd=cwd, environment=environment)
        diagnostic = f"{result.stdout}\n{result.stderr}"
        if succeeds:
            self.assertEqual(result.returncode, 0, diagnostic)
            self.assertEqual(self.state()["revision"], revision + 1)
        else:
            self.assertNotEqual(result.returncode, 0, diagnostic)
            self.assertEqual(self.tracked(), before, diagnostic)
        return result

    def claim(self, task_id: str = "TASK-001", actor: str = "worker") -> None:
        self.mutation("claim", task_id, actor=actor)

    def run_check(self, *, succeeds: bool = True, environment: dict[str, str] | None = None
                  ) -> subprocess.CompletedProcess:
        revision = self.state()["revision"]
        result = self.cli("run-check", "TASK-001", "--check", "repository", "--actor", "worker",
                          "--expect-revision", str(revision), environment=environment)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.state()["revision"], revision + 1)
        return result

    def start_check(self) -> subprocess.Popen:
        (self.root / "out/hold").touch()
        process = subprocess.Popen(
            self.command("run-check", "TASK-001", "--check", "repository", "--actor", "worker",
                         "--expect-revision", str(self.state()["revision"])),
            cwd=self.directory, env=self.environment(), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8")

        def stop() -> None:
            (self.root / "out/release").touch()
            if process.poll() is None:
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)

        self.addCleanup(stop)
        deadline = time.monotonic() + 10
        while not (self.root / "out/started").exists():
            if process.poll() is not None:
                stdout, stderr = process.communicate()
                self.fail(f"checker exited before its handshake: {stdout}\n{stderr}")
            if time.monotonic() >= deadline:
                self.fail("checker did not reach its bounded handshake")
            time.sleep(0.01)
        return process

    def invoke(self, *arguments: str) -> tuple[int, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = workflow.main(["--root", str(self.root), *arguments])
        return result, stdout.getvalue() + stderr.getvalue()

    def git(self, *arguments: str, directory: Path | None = None) -> str:
        if GIT is None:
            self.skipTest("Git is unavailable; source archive behavior is tested separately")
        result = subprocess.run(
            [GIT, "-c", "core.fsmonitor=false", "-c", "commit.gpgsign=false",
             "-c", "user.name=Workflow Test", "-c", "user.email=workflow@example.invalid",
             "-C", str(directory or self.root), *arguments], env=self.environment(),
            check=True, capture_output=True, text=True, timeout=15)
        return result.stdout.strip()

    def test_schema_rejects_incoherent_types_references_and_task_states(self) -> None:
        original = self.state()
        mutations = {
            "boolean revision": lambda s: s.update(revision=True),
            "boolean schema version": lambda s: s.update(schema_version=True),
            "boolean priority": lambda s: s["tasks"][0].update(priority=True),
            "boolean source count": lambda s: s["source_snapshot"].update(file_count=True),
            "unknown field": lambda s: s.update(hidden_authority="admin"),
            "unknown check": lambda s: s["tasks"][0].update(required_checks=["shell"]),
            "duplicate task": lambda s: s["tasks"].append(copy.deepcopy(s["tasks"][0])),
            "missing dependency": lambda s: s["tasks"][0].update(depends_on=["TASK-404"]),
            "self dependency": lambda s: s["tasks"][0].update(depends_on=["TASK-001"]),
            "missing milestone": lambda s: s["tasks"][0].update(milestone="UNKNOWN"),
            "derived status persisted": lambda s: s["tasks"][0].update(status="ready"),
            "unowned active task": lambda s: s["tasks"][0].update(status="in_progress"),
            "unreviewed completion": lambda s: s["tasks"][0].update(status="done"),
            "empty acceptance": lambda s: s["tasks"][0].update(acceptance=[]),
            "empty handoff": lambda s: s["tasks"][0]["checkpoint"].update(next_action=""),
            "history gap": lambda s: s.update(revision=2),
            "control-bearing identity": lambda s: s["project"].update(coordinator="owner\nadmin"),
        }
        for name, mutate in mutations.items():
            with self.subTest(case=name):
                candidate = copy.deepcopy(original)
                mutate(candidate)
                with self.assertRaises(ValueError):
                    workflow.validate_state(candidate)
        cycle = copy.deepcopy(original)
        cycle["tasks"] = [task(dependencies=("TASK-002",)),
                          task("TASK-002", dependencies=("TASK-001",))]
        with self.assertRaises(ValueError):
            workflow.validate_state(cycle)

    def test_strict_json_rejects_duplicate_keys_nonfinite_numbers_and_wrong_root(self) -> None:
        valid = (self.root / STATE_PATH).read_text(encoding="utf-8")
        malformed = [valid.replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1'),
                     valid.replace('"priority": 1', '"priority": 1, "priority": 1'),
                     valid.replace('"priority": 1', '"priority": NaN'),
                     valid.replace('"priority": 1', '"priority": Infinity'), "[]", "null"]
        for content in malformed:
            with self.subTest(content=content[:90]):
                (self.root / STATE_PATH).write_text(content, encoding="utf-8")
                with self.assertRaises(ValueError):
                    workflow.load_state(self.root)
        (self.root / STATE_PATH).write_bytes(b"\xff invalid UTF-8")
        with self.assertRaises(ValueError):
            workflow.load_state(self.root)

    def test_task_paths_are_literal_relative_components(self) -> None:
        for path in ("../outside", "/absolute", "engine/../secret", "engine//world", "./engine",
                     "engine\\world", "C:/engine", "engine/*", "engine/[ab]", "engine/?.cpp"):
            with self.subTest(path=path):
                candidate = self.state()
                candidate["tasks"][0]["write_paths"] = [path]
                with self.assertRaises(ValueError):
                    workflow.validate_state(candidate)

    def test_ready_selection_dependencies_and_portable_component_overlap(self) -> None:
        self.seed([task(priority=2), task("TASK-002", paths=("engine/world2",), priority=1),
                   task("TASK-003", paths=("Engine/World/state.cpp",), priority=3),
                   task("TASK-004", paths=("docs",), dependencies=("TASK-001",), priority=0)])
        result = self.cli("next", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        ids = re.findall(r'"TASK-\d{3}"', result.stdout)
        self.assertEqual(list(dict.fromkeys(ids)), ['"TASK-002"', '"TASK-001"', '"TASK-003"'])
        self.claim()
        after = self.cli("next", "--json")
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertIn("TASK-002", after.stdout)
        self.assertNotIn("TASK-003", after.stdout)
        self.assertNotIn("TASK-004", after.stdout)
        self.mutation("claim", "TASK-003", actor="worker-2", succeeds=False)
        self.mutation("claim", "TASK-004", actor="worker-2", succeeds=False)
        self.mutation("block", "TASK-001", "--reason", "Reserved partial work.",
                      "--next-action", "Resolve the input dependency.")
        self.mutation("claim", "TASK-003", actor="worker-2", succeeds=False)
        self.mutation("resume", "TASK-001", "--summary", "Input dependency resolved.")
        self.claim("TASK-002", "worker-2")

    def test_checks_require_explicit_submit_and_coordinator_review(self) -> None:
        self.claim()
        self.mutation("submit", "TASK-001", "--summary", "Premature submission.", succeeds=False)
        self.run_check()
        state = self.state()
        self.assertEqual(state["tasks"][0]["status"], "in_progress")
        self.assertIsNone(state["tasks"][0]["completion"])
        receipt = state["evidence"][0]
        self.assertEqual(receipt["result"], "passed")
        self.assertEqual(receipt["exit_code"], 0)
        self.assertNotIn(str(self.directory), json.dumps(receipt))
        log = self.root / receipt["log"]["path"]
        self.assertEqual(receipt["log"]["sha256"], hashlib.sha256(log.read_bytes()).hexdigest())
        self.assertEqual(receipt["log"]["bytes"], log.stat().st_size)
        self.mutation("submit", "TASK-001", "--summary", "Ready for review.")
        self.mutation("accept", "TASK-001", "--summary", "Unauthorized review.",
                      actor="outsider", succeeds=False)
        self.mutation("accept", "TASK-001", "--summary", "Reviewed the evidence.", actor="coordinator")
        done = self.state()["tasks"][0]
        self.assertEqual(done["status"], "done")
        self.assertEqual(done["owner"], "worker")
        self.assertIsNone(done["claim_revision"])
        self.assertEqual(done["completion"]["reviewer"], "coordinator")
        self.assertEqual(done["completion"]["review_kind"], "peer")
        self.assertEqual(done["completion"]["evidence"], [receipt["id"]])
        self.mutation("cancel", "TASK-001", "--summary", "Cannot erase completed work.",
                      "--next-action", "Create a regression task.", actor="coordinator", succeeds=False)
        log.unlink()
        (self.root / "out/fail").touch()
        self.assertEqual(self.cli("check").returncode, 0, "fresh clones need no ignored check logs")
        (self.root / "README.md").write_text("# Later source revision\n", encoding="utf-8")
        self.mutation("reconcile", "--summary", "Record later source work.", actor="coordinator")
        self.assertEqual(self.state()["tasks"][0]["completion"], done["completion"])
        self.assertEqual(self.cli("check").returncode, 0)

    def test_coordinator_self_review_must_be_explicit(self) -> None:
        self.seed([task(checks=())])
        self.claim(actor="coordinator")
        self.mutation("submit", "TASK-001", "--summary", "Ready.", actor="coordinator")
        self.mutation("accept", "TASK-001", "--summary", "Implicit self review.",
                      actor="coordinator", succeeds=False)
        self.mutation("accept", "TASK-001", "--summary", "Explicit self review.",
                      "--self-review", actor="coordinator")
        self.assertEqual(self.state()["tasks"][0]["completion"]["review_kind"], "self")

    def test_every_declared_check_needs_a_current_passing_receipt(self) -> None:
        self.seed([task(checks=("repository", "workflow"))])
        self.claim()
        self.run_check()
        self.assertEqual(self.state()["evidence"][0]["check"], "repository")
        self.mutation("submit", "TASK-001", "--summary", "A second required gate is missing.",
                      succeeds=False)
        self.assertEqual(self.state()["tasks"][0]["status"], "in_progress")

    def test_checkpoint_block_resume_handoff_and_cancel_preserve_work(self) -> None:
        self.claim()
        self.mutation("checkpoint", "TASK-001", "--summary", "Wrong owner.",
                      "--next-action", "Continue.", actor="outsider", succeeds=False)
        (self.root / "README.md").write_text("# Preserved partial work\n", encoding="utf-8")
        self.mutation("checkpoint", "TASK-001", "--summary", "Partial implementation.",
                      "--next-action", "Verify the behavior.", "--file", "README.md")
        recorded = self.state()["tasks"][0]["checkpoint"]
        self.assertEqual(recorded["files"], ["README.md"])
        self.assertEqual(self.cli("check").returncode, 0)
        self.mutation("block", "TASK-001", "--reason", "Need fixture input.",
                      "--next-action", "Obtain the fixture input.")
        blocked = self.state()["tasks"][0]
        self.assertEqual(blocked["checkpoint"]["summary"], recorded["summary"])
        self.assertEqual(blocked["checkpoint"]["files"], recorded["files"])
        self.assertEqual(blocked["owner"], "worker")
        self.mutation("resume", "TASK-001", "--summary", "Wrong owner.",
                      actor="outsider", succeeds=False)
        self.mutation("resume", "TASK-001", "--summary", "Fixture supplied.", actor="coordinator")
        self.assertIsNone(self.state()["tasks"][0]["blocker"])
        self.mutation("handoff", "TASK-001", "--summary", "Preserve partial implementation.",
                      "--next-action", "Continue verification.")
        released = self.state()["tasks"][0]
        self.assertEqual(released["status"], "planned")
        self.assertIsNone(released["owner"])
        self.assertIsNone(released["claim_revision"])
        self.assertEqual(released["checkpoint"]["files"], recorded["files"])
        self.mutation("cancel", "TASK-001", "--summary", "No longer in scope.",
                      "--next-action", "Retain the partial work.", actor="coordinator")
        self.assertEqual(self.state()["tasks"][0]["status"], "cancelled")
        self.mutation("claim", "TASK-001", succeeds=False)

    def test_source_drift_requires_recorded_reconciliation_and_sync_cannot_hide_it(self) -> None:
        original = self.tracked()
        (self.root / "new-input.txt").write_text("new source\n", encoding="utf-8")
        result = self.cli("check")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.tracked(), original)
        self.mutation("claim", "TASK-001", succeeds=False)
        self.assertEqual(self.cli("sync").returncode, 0)
        self.assertEqual(self.tracked(), original)
        self.assertNotEqual(self.cli("check").returncode, 0)
        self.mutation("reconcile", "--summary", "Unauthorized source update.", succeeds=False)
        self.mutation("reconcile", "--summary", "Document the added input.", actor="coordinator")
        self.assertEqual(self.state()["source_snapshot"], workflow.snapshot_source(self.root))
        self.assertEqual(self.state()["tasks"][0]["status"], "planned")
        self.assertEqual(self.cli("check").returncode, 0)

    def test_latest_failed_receipt_blocks_an_older_pass_and_keeps_both_logs(self) -> None:
        self.claim()
        self.run_check()
        (self.root / "out/fail").touch()
        self.run_check(succeeds=False)
        evidence = self.state()["evidence"]
        self.assertEqual([record["result"] for record in evidence], ["passed", "failed"])
        self.assertEqual(evidence[-1]["exit_code"], 7)
        self.assertEqual(len({record["id"] for record in evidence}), 2)
        for record in evidence:
            self.assertTrue((self.root / record["log"]["path"]).is_file())
        self.mutation("submit", "TASK-001", "--summary", "Ignore the newer failure.", succeeds=False)
        (self.root / "out/fail").unlink()
        self.run_check()
        self.mutation("submit", "TASK-001", "--summary", "Latest gate passed.")
        self.assertEqual(len(self.state()["evidence"]), 3)

    def test_check_that_changes_source_is_invalidated_despite_zero_exit(self) -> None:
        self.claim()
        recorded = self.state()["source_snapshot"]
        (self.root / "out/mutate").touch()
        self.run_check(succeeds=False)
        state = self.state()
        receipt = state["evidence"][0]
        self.assertEqual(receipt["result"], "invalidated")
        self.assertEqual(receipt["exit_code"], 0)
        self.assertNotEqual(receipt["source_before"], receipt["source_after"])
        self.assertEqual(state["source_snapshot"], recorded)
        self.assertNotEqual(self.cli("check").returncode, 0)
        self.mutation("checkpoint", "TASK-001", "--summary", "Record source modified by the gate.",
                      "--next-action", "Rerun the gate.", "--file", "README.md")
        self.mutation("submit", "TASK-001", "--summary", "Invalidated is not a pass.", succeeds=False)

    def test_acceptance_rejects_previously_passing_evidence_for_changed_source(self) -> None:
        self.claim()
        self.run_check()
        self.mutation("submit", "TASK-001", "--summary", "Ready.")
        old_receipt = copy.deepcopy(self.state()["evidence"][0])
        (self.root / "README.md").write_text("# Source changed after review started\n", encoding="utf-8")
        self.mutation("reconcile", "--summary", "Record the intervening source change.", actor="coordinator")
        self.mutation("accept", "TASK-001", "--summary", "Stale evidence.",
                      actor="coordinator", succeeds=False)
        self.assertEqual(self.state()["evidence"][0], old_receipt)
        self.mutation("resume", "TASK-001", "--summary", "Review needs fresh evidence.", actor="coordinator")
        self.run_check()
        self.mutation("submit", "TASK-001", "--summary", "Fresh evidence attached.")
        self.mutation("accept", "TASK-001", "--summary", "Reviewed current source.", actor="coordinator")

    def test_nonrequired_check_cannot_invoke_an_arbitrary_command(self) -> None:
        self.claim()
        for check in ("workflow", "shell", "repository; touch pwned"):
            with self.subTest(check=check):
                self.mutation("run-check", "TASK-001", "--check", check, succeeds=False)
        self.assertFalse((self.root / "pwned").exists())

    def test_run_attaches_to_latest_revision_without_holding_the_ledger_lock(self) -> None:
        self.claim()
        process = self.start_check()
        self.mutation("focus", "M1", "--summary", "Unrelated planning update.", actor="coordinator")
        self.mutation("run-check", "TASK-001", "--check", "repository", succeeds=False)
        (self.root / "out/release").touch()
        stdout, stderr = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        state = self.state()
        self.assertEqual(state["revision"], 4)
        self.assertEqual(state["project"]["current_milestone"], "M1")
        self.assertEqual(state["tasks"][0]["claim_revision"], 2)
        self.assertEqual(len(state["evidence"]), 1)
        self.assertEqual([entry["revision"] for entry in state["history"]], [1, 2, 3, 4])

    def test_live_repeat_check_prevents_submission_and_retains_failure_when_blocked(self) -> None:
        self.claim()
        self.run_check()
        (self.root / "out/fail").touch()
        process = self.start_check()
        self.mutation("submit", "TASK-001", "--summary", "Cannot outrun the active repeat gate.",
                      succeeds=False)
        self.assertEqual(self.state()["tasks"][0]["status"], "in_progress")
        self.assertEqual(len(self.state()["evidence"]), 1)
        self.mutation("block", "TASK-001", "--reason", "Blocked while the gate finishes.",
                      "--next-action", "Inspect the completed gate before resuming.")
        (self.root / "out/release").touch()
        stdout, stderr = process.communicate(timeout=10)
        self.assertNotEqual(process.returncode, 0, stdout + stderr)
        self.assertEqual([receipt["result"] for receipt in self.state()["evidence"]],
                         ["passed", "failed"])
        self.assertEqual(self.state()["evidence"][-1]["exit_code"], 7)
        blocked = self.state()["tasks"][0]
        self.assertEqual(blocked["status"], "blocked")
        self.assertEqual(blocked["owner"], "worker")
        self.assertEqual(blocked["claim_revision"], 2)
        self.assertEqual(blocked["blocker"], "Blocked while the gate finishes.")
        self.mutation("resume", "TASK-001", "--summary", "Resume with the failed gate preserved.")
        self.mutation("submit", "TASK-001", "--summary", "The latest gate failed.", succeeds=False)

    def test_changed_claim_during_execution_keeps_log_but_never_attaches_receipt(self) -> None:
        self.claim()
        process = self.start_check()
        self.mutation("handoff", "TASK-001", "--summary", "Transfer preserved work.",
                      "--next-action", "Continue as the new owner.")
        self.claim(actor="worker-2")
        recorded = self.tracked()
        (self.root / "out/release").touch()
        stdout, stderr = process.communicate(timeout=10)
        self.assertNotEqual(process.returncode, 0, stdout + stderr)
        self.assertEqual(self.tracked(), recorded)
        self.assertEqual(self.state()["evidence"], [])
        self.assertEqual(self.state()["tasks"][0]["owner"], "worker-2")
        self.assertTrue(any(path.is_file() for path in (self.root / "out/agent-runs").rglob("*")))

    def test_concurrent_compare_and_swap_claims_have_exactly_one_winner(self) -> None:
        self.seed([task(), task("TASK-002", paths=("docs/manual",))])
        processes = [subprocess.Popen(
            self.command("claim", task_id, "--actor", actor, "--expect-revision", "1"),
            cwd=self.directory, env=self.environment(), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
            for task_id, actor in (("TASK-001", "worker-1"), ("TASK-002", "worker-2"))]
        results = []
        try:
            for process in processes:
                stdout, stderr = process.communicate(timeout=10)
                results.append((process.returncode, stdout + stderr))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        self.assertEqual(sum(code == 0 for code, _ in results), 1, results)
        state = self.state()
        self.assertEqual(state["revision"], 2)
        self.assertEqual(len(state["history"]), 2)
        self.assertEqual(sum(value["status"] == "in_progress" for value in state["tasks"]), 1)
        self.assertEqual(self.cli("check").returncode, 0)

    def test_existing_lock_is_reported_and_never_stolen_by_age_or_dead_pid(self) -> None:
        lock = self.root / "out/agent-state.lock"
        content = '{"pid": 99999999, "created_at": "1990-01-01T00:00:00Z"}\n'
        lock.write_text(content, encoding="utf-8")
        os.utime(lock, (1, 1))
        self.mutation("claim", "TASK-001", succeeds=False)
        info = self.cli("lock-info")
        self.assertEqual(info.returncode, 0, info.stdout + info.stderr)
        self.assertIn("agent-state.lock", info.stdout + info.stderr)
        self.assertEqual(lock.read_text(encoding="utf-8"), content)

    def test_rendering_is_pure_deterministic_and_escapes_markdown_data(self) -> None:
        state = self.state()
        state["tasks"][0]["title"] = "[unsafe](file:///private) | <script>bad</script>"
        state["tasks"][0]["description"] = "A paragraph\n# Injected heading"
        original = copy.deepcopy(state)
        with mock.patch("builtins.open", side_effect=AssertionError("render read a file")), \
                mock.patch("pathlib.Path.open", side_effect=AssertionError("render opened a path")), \
                mock.patch("subprocess.run", side_effect=AssertionError("render ran a command")), \
                mock.patch("time.time", side_effect=AssertionError("render read the clock")):
            first = workflow.render_documents(state)
            second = workflow.render_documents(copy.deepcopy(state))
        self.assertEqual(first, second)
        self.assertEqual(state, original)
        self.assertEqual(set(first), set(VIEWS))
        for document in first.values():
            self.assertTrue(document.endswith("\n"))
            self.assertNotIn("\r", document)
            self.assertNotIn("<script>", document)
            self.assertNotIn("[unsafe](file:///private)", document)
            self.assertNotIn("\n# Injected heading", document)
            self.assertNotIn(str(self.directory), document)

    def test_read_only_check_detects_view_drift_and_sync_repairs_only_views(self) -> None:
        canonical = (self.root / STATE_PATH).read_bytes()
        view = self.root / VIEWS[0]
        view.write_text("# Hand-edited status\n", encoding="utf-8")
        before = self.tracked()
        mtimes = {name: (self.root / name).stat().st_mtime_ns for name in before}
        self.assertNotEqual(self.cli("check").returncode, 0)
        self.assertEqual(self.tracked(), before)
        self.assertEqual({name: (self.root / name).stat().st_mtime_ns for name in before}, mtimes)
        self.assertEqual(self.cli("sync").returncode, 0)
        self.assertEqual((self.root / STATE_PATH).read_bytes(), canonical)
        expected = workflow.render_documents(self.state())
        for name in VIEWS:
            self.assertEqual((self.root / name).read_bytes(), expected[name].encode("utf-8"))
        self.assertEqual(self.cli("check").returncode, 0)
        fixed = self.tracked()
        self.assertEqual(self.cli("sync").returncode, 0)
        self.assertEqual(self.tracked(), fixed)

    def test_failure_before_json_commit_preserves_all_published_files(self) -> None:
        before = self.tracked()
        original_replace = os.replace

        def fail_state(source: object, destination: object, *args: object, **kwargs: object) -> None:
            if Path(destination).name == Path(STATE_PATH).name:
                raise OSError("injected canonical publication failure")
            original_replace(source, destination, *args, **kwargs)

        with mock.patch("os.replace", side_effect=fail_state):
            code, diagnostic = self.invoke("focus", "M1", "--summary", "Fail before commit.",
                                           "--actor", "coordinator", "--expect-revision", "1")
        self.assertNotEqual(code, 0, diagnostic)
        self.assertEqual(self.tracked(), before)
        self.assertEqual(self.cli("check").returncode, 0)
        self.mutation("focus", "M1", "--summary", "Retry after failed write.", actor="coordinator")

    def test_failure_after_json_commit_reports_drift_and_sync_recovers(self) -> None:
        original_replace = os.replace

        def fail_view(source: object, destination: object, *args: object, **kwargs: object) -> None:
            if Path(destination).name == Path(VIEWS[0]).name:
                raise OSError("injected derived publication failure")
            original_replace(source, destination, *args, **kwargs)

        with mock.patch("os.replace", side_effect=fail_view):
            code, diagnostic = self.invoke("focus", "M1", "--summary", "Commit before view failure.",
                                           "--actor", "coordinator", "--expect-revision", "1")
        self.assertNotEqual(code, 0, diagnostic)
        self.assertIn("sync", diagnostic.lower())
        self.assertEqual(self.state()["revision"], 2)
        self.assertEqual(self.state()["project"]["current_milestone"], "M1")
        committed = (self.root / STATE_PATH).read_bytes()
        self.assertNotEqual(self.cli("check").returncode, 0)
        self.assertEqual(self.cli("sync").returncode, 0)
        self.assertEqual((self.root / STATE_PATH).read_bytes(), committed)
        self.assertEqual(self.cli("check").returncode, 0)

    def test_add_and_amend_use_caller_relative_specs_and_preserve_operational_history(self) -> None:
        spec = definition(task("TASK-002", paths=("docs/manual",), dependencies=("TASK-001",)))
        draft = self.root / "out/draft.json"
        for invalid_id in ([], {}):
            with self.subTest(invalid_id=invalid_id):
                draft.write_text(json.dumps({**spec, "id": invalid_id}), encoding="utf-8")
                rejected = self.mutation("add", "--spec", "draft.json", actor="coordinator",
                                         cwd=draft.parent, succeeds=False)
                self.assertNotIn("Traceback", rejected.stdout + rejected.stderr)
                self.assertIn("agent-state:", rejected.stderr)
        draft.write_text(json.dumps(spec), encoding="utf-8")
        self.mutation("add", "--spec", "draft.json", actor="outsider", cwd=draft.parent, succeeds=False)
        self.mutation("add", "--spec", "draft.json", actor="coordinator", cwd=draft.parent)
        added = copy.deepcopy(self.state()["tasks"][1])
        spec.update(title="Refined future task", priority=0, next_action="Review the manual first.")
        draft.write_text(json.dumps(spec), encoding="utf-8")
        self.mutation("amend", "TASK-002", "--spec", "draft.json", "--summary", "Clarify scope.",
                      actor="coordinator", cwd=draft.parent)
        changed = self.state()["tasks"][1]
        self.assertEqual(changed["title"], spec["title"])
        self.assertEqual(changed["priority"], 0)
        for key in ("status", "owner", "claim_revision", "evidence", "completion"):
            self.assertEqual(changed[key], added[key])
        cycle = definition(self.state()["tasks"][0])
        cycle["depends_on"] = ["TASK-002"]
        draft.write_text(json.dumps(cycle), encoding="utf-8")
        self.mutation("amend", "TASK-001", "--spec", str(draft), "--summary", "Would create a cycle.",
                      actor="coordinator", succeeds=False)
        self.claim()
        cycle["depends_on"] = []
        draft.write_text(json.dumps(cycle), encoding="utf-8")
        self.mutation("amend", "TASK-001", "--spec", str(draft), "--summary", "Cannot rewrite active scope.",
                      actor="coordinator", succeeds=False)

    def test_source_snapshot_tracks_content_additions_deletions_and_canonical_newlines(self) -> None:
        initial = workflow.snapshot_source(self.root)
        readme = self.root / "README.md"
        readme.write_bytes(b"# Fixture engine\r\n")
        self.assertEqual(workflow.snapshot_source(self.root), initial)
        for name in ("out/ignored.bin", "build/ignored.bin", "__pycache__/ignored.pyc", ".git/HEAD"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"unrelated generated bytes")
        self.assertEqual(workflow.snapshot_source(self.root), initial)
        for name in (STATE_PATH, *VIEWS):
            (self.root / name).write_bytes(b"excluded derived content")
        self.assertEqual(workflow.snapshot_source(self.root), initial)
        asset = self.root / "asset.bin"
        asset.write_bytes(b"\0\r\n\xff")
        binary = workflow.snapshot_source(self.root)
        self.assertEqual(binary["file_count"], initial["file_count"] + 1)
        self.assertNotEqual(binary["sha256"], initial["sha256"])
        renamed = self.root / "renamed-asset.bin"
        asset.rename(renamed)
        self.assertNotEqual(workflow.snapshot_source(self.root)["sha256"], binary["sha256"])
        renamed.rename(asset)
        self.assertEqual(workflow.snapshot_source(self.root), binary)
        asset.write_bytes(b"\0\n\xff")
        self.assertNotEqual(workflow.snapshot_source(self.root)["sha256"], binary["sha256"])
        asset.unlink()
        self.assertEqual(workflow.snapshot_source(self.root), initial)
        prompt = self.root / "docs/agents/WORKER.md"
        prompt.write_text("# Manual agent instructions\n", encoding="utf-8")
        self.assertNotEqual(workflow.snapshot_source(self.root), initial)
        other = self.directory / "relocated source"
        shutil.copytree(self.root, other)
        os.utime(other / "README.md", (123456789, 123456789))
        self.assertEqual(workflow.snapshot_source(other), workflow.snapshot_source(self.root))

    def test_archive_does_not_inherit_an_unrelated_parent_repository(self) -> None:
        before = workflow.snapshot_source(self.root)
        self.git("init", "--quiet", directory=self.directory)
        self.git("add", self.root.name, directory=self.directory)
        self.git("commit", "--quiet", "-m", "Containing repository", directory=self.directory)
        self.assertEqual(workflow.snapshot_source(self.root), before)
        self.claim()
        self.run_check(environment={"GIT_DIR": str(self.directory / ".git"),
                                    "GIT_WORK_TREE": str(self.directory)})
        self.assertEqual(self.state()["evidence"][0]["git"],
                         {"status": "unknown", "commit": None, "dirty": None})

    def test_fingerprint_ignores_only_reserved_publication_temps_in_exact_directories(self) -> None:
        original = workflow.snapshot_source(self.root)
        reserved = (
            "project/.agent-state.json.fixture.tmp",
            "docs/agents/.STATUS.md.fixture.tmp",
            "docs/agents/.HANDOFF.md.fixture.tmp",
        )
        for name in reserved:
            with self.subTest(reserved=name):
                path = self.root / name
                path.write_bytes(b"incomplete publication\x00\xff")
                self.assertEqual(workflow.snapshot_source(self.root), original)
                path.write_bytes(b"publication still in progress")
                self.assertEqual(workflow.snapshot_source(self.root), original)
        manual_inputs = (
            "project/.agent-state.json.tmp",
            "project/notes.tmp",
            "docs/agents/.STATUS.md.tmp",
            "docs/agents/.HANDOFF.md.tmp",
            "docs/agents/.STATUS.md.fixture.tmp.backup",
            "docs/agents/HANDOFF.md.review.tmp",
            "docs/.STATUS.md.fixture.tmp",
            "project/nested/.agent-state.json.fixture.tmp",
            "docs/agents/nested/.HANDOFF.md.fixture.tmp",
            ".agent-state.json.fixture.tmp",
        )
        for name in manual_inputs:
            with self.subTest(manual=name):
                path = self.root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("Manual source input.\n", encoding="utf-8")
                try:
                    changed = workflow.snapshot_source(self.root)
                    self.assertNotEqual(changed["sha256"], original["sha256"])
                    self.assertEqual(changed["file_count"], original["file_count"] + 1)
                finally:
                    path.unlink()
                self.assertEqual(workflow.snapshot_source(self.root), original)

    def test_git_head_only_changes_and_inherited_overrides_do_not_change_fingerprint(self) -> None:
        before = workflow.snapshot_source(self.root)
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", "Source fixture")
        first_commit = self.git("rev-parse", "HEAD")
        self.git("commit", "--quiet", "--allow-empty", "-m", "Metadata only")
        commit = self.git("rev-parse", "HEAD")
        self.assertNotEqual(first_commit, commit)
        self.assertEqual(workflow.snapshot_source(self.root), before)
        self.claim()
        self.run_check(environment={"GIT_DIR": str(self.directory / "wrong.git"),
                                    "GIT_WORK_TREE": str(self.directory),
                                    "GIT_INDEX_FILE": str(self.directory / "wrong-index")})
        provenance = self.state()["evidence"][0]["git"]
        self.assertEqual(provenance["status"], "git")
        self.assertEqual(provenance["commit"], commit)

    def test_managed_file_symlinks_cannot_overwrite_external_targets(self) -> None:
        external = self.directory / "outside.md"
        external.write_bytes(b"outside sentinel\n")
        for name in (STATE_PATH, *VIEWS):
            with self.subTest(path=name):
                path = self.root / name
                original = path.read_bytes()
                path.unlink()
                try:
                    path.symlink_to(external)
                except (OSError, NotImplementedError) as error:
                    path.write_bytes(original)
                    self.skipTest(f"host cannot create symbolic links: {error}")
                try:
                    result = self.cli("sync")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(external.read_bytes(), b"outside sentinel\n")
                    self.assertTrue(path.is_symlink())
                finally:
                    path.unlink()
                    path.write_bytes(original)

    def test_symlink_components_and_source_symlinks_fail_without_following_them(self) -> None:
        external = self.directory / "outside"
        external.mkdir()
        (external / "secret.txt").write_text("private fixture", encoding="utf-8")
        link = self.root / "source-link"
        try:
            link.symlink_to(external, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create symbolic links: {error}")
        with self.assertRaises(ValueError):
            workflow.snapshot_source(self.root)
        link.unlink()
        agents = self.root / "docs/agents"
        saved = self.root / "docs/agents-original"
        agents.rename(saved)
        agents.symlink_to(external, target_is_directory=True)
        before = (self.root / STATE_PATH).read_bytes()
        result = self.cli("sync")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / STATE_PATH).read_bytes(), before)
        self.assertFalse((external / "STATUS.md").exists())
        self.assertFalse((external / "HANDOFF.md").exists())

    def test_managed_hardlink_alias_is_rejected_without_changing_input(self) -> None:
        view = self.root / VIEWS[0]
        view.unlink()
        try:
            view.hardlink_to(self.root / "README.md")
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create hard links: {error}")
        before = self.tracked()
        source = (self.root / "README.md").read_bytes()
        result = self.cli("focus", "M1", "--summary", "Unsafe alias.", "--actor", "coordinator",
                          "--expect-revision", "1")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.tracked(), before)
        self.assertEqual((self.root / "README.md").read_bytes(), source)
        self.assertTrue(view.samefile(self.root / "README.md"))

    @unittest.skipUnless(hasattr(os, "mkfifo"), "named pipes require POSIX")
    def test_nonregular_managed_state_is_rejected_without_waiting_for_a_writer(self) -> None:
        path = self.root / STATE_PATH
        path.unlink()
        os.mkfifo(path)
        result = self.cli("check", timeout=4)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Traceback", result.stderr)

    def test_unicode_paths_and_legacy_output_encoding_report_errors_without_crashing(self) -> None:
        unicode_root = self.directory / "source-世界-😀"
        self.root.rename(unicode_root)
        self.root = unicode_root
        result = self.cli("check", environment={"PYTHONIOENCODING": "cp1252:strict"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (self.root / STATE_PATH).unlink()
        error = self.cli("check", environment={"PYTHONIOENCODING": "cp1252:strict"})
        self.assertNotEqual(error.returncode, 0)
        self.assertNotIn("UnicodeEncodeError", error.stdout + error.stderr)
        self.assertNotIn("Traceback", error.stdout + error.stderr)
        self.assertIn("agent-state.json", error.stdout + error.stderr)

    def test_default_root_is_script_location_even_from_an_unrelated_caller(self) -> None:
        copied_tool = self.root / "tools/agent_state.py"
        shutil.copyfile(TOOL, copied_tool)
        self.seed()
        result = subprocess.run([sys.executable, str(copied_tool), "check"], cwd=self.directory,
                                env=self.environment(), capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
