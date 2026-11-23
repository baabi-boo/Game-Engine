#!/usr/bin/env python3
"""Maintain the repository's explicit, cooperative agent work ledger.

Only explicit commands change state. The JSON ledger is authoritative; the two
Markdown views are deterministic projections. This module has no third-party
dependencies, executes only named repository checks, and never commits or pushes.
See docs/agents/WORKFLOW.md for the operating contract and recovery procedures.
"""

from __future__ import annotations

import argparse
import codecs
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Iterator
from urllib.parse import quote, urlsplit
import uuid


_STATE = "project/agent-state.json"
_VIEWS = ("docs/agents/STATUS.md", "docs/agents/HANDOFF.md")
_STATE_LOCK = "out/agent-state.lock"
_RUN_LOCK = "out/agent-run.lock"
_ID = re.compile(r"[A-Za-z][A-Za-z0-9_-]{0,63}\Z")
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
_UTC = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?(?:Z|\+00:00)\Z")
_STATUSES = {"planned", "in_progress", "blocked", "review", "done", "cancelled"}
_ACTIVE = {"in_progress", "blocked", "review"}
_RESULTS = {"passed", "failed", "invalidated", "interrupted", "timeout"}
_DEFINITION = (
    "id", "title", "milestone", "priority", "description", "depends_on",
    "write_paths", "acceptance", "required_checks",
)
_CHECKS: dict[str, tuple[list[str], int]] = {
    "repository": (["python", "tools/check_repo.py"], 120),
    "workflow": (["python", "tools/agent_state.py", "check"], 120),
    "workflow-tests": (["python", "tests/test_agent_state.py"], 120),
    "dev": (["python", "tools/ge.py", "check", "--preset", "dev", "--jobs", "2"], 1800),
    "release": (["python", "tools/ge.py", "check", "--preset", "release", "--jobs", "2"], 1800),
    "package": (["python", "tools/ge.py", "package", "--preset", "release", "--jobs", "2"], 1800),
}
_IGNORED_DIRS = {".git", ".venv", ".idea", ".vscode", "__pycache__"}
_ROOT_OUTPUTS = {
    "out", "build", "Testing", "CMakeFiles", "CMakeCache.txt",
    "cmake_install.cmake", "install_manifest.txt", "compile_commands.json",
    "CMakeUserPresets.json",
}
_TEMP_PREFIXES = {
    "project": ".agent-state.json.",
    "docs/agents": (".STATUS.md.", ".HANDOFF.md."),
}
_MAX_JSON_BYTES = 32 * 1024 * 1024


class WorkflowError(ValueError):
    """A validation, coordination, or publication error safe to show to users."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise WorkflowError(message)


def _keys(value: Any, expected: set[str], label: str) -> None:
    _require(type(value) is dict, f"{label} must be an object")
    actual = set(value)
    _require(actual == expected,
             f"{label} has invalid fields (missing={sorted(expected - actual)}, "
             f"extra={sorted(actual - expected, key=str)})")


def _integer(value: Any, label: str, minimum: int = 0) -> None:
    _require(type(value) is int and value >= minimum,
             f"{label} must be an integer >= {minimum}")


def _text(value: Any, label: str, *, empty: bool = False) -> None:
    _require(isinstance(value, str), f"{label} must be a string")
    _require(len(value) <= 65536, f"{label} is too long")
    _require(empty or bool(value.strip()), f"{label} must not be empty")
    _require(not any((ord(c) < 32 and c not in "\n\t") or ord(c) == 127
                     or 0xD800 <= ord(c) <= 0xDFFF for c in value),
             f"{label} contains unsupported control or surrogate characters")


def _identifier(value: Any, label: str) -> None:
    _require(isinstance(value, str) and _ID.fullmatch(value) is not None,
             f"{label} must match [A-Za-z][A-Za-z0-9_-]{{0,63}}")


def _timestamp(value: Any, label: str) -> None:
    _require(isinstance(value, str) and _UTC.fullmatch(value) is not None,
             f"{label} must be a UTC ISO8601 timestamp")
    try:
        datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise WorkflowError(f"{label} is not a valid timestamp") from error


def _strings(value: Any, label: str, *, nonempty: bool = False,
             unique: bool = True, identifiers: bool = False) -> None:
    _require(type(value) is list, f"{label} must be an array")
    _require(not nonempty or bool(value), f"{label} must not be empty")
    for item in value:
        (_identifier if identifiers else _text)(item, label)
    _require(not unique or len(set(value)) == len(value), f"{label} contains duplicates")


def _path(value: Any, label: str, *, directory: bool = False) -> tuple[str, ...]:
    _text(value, label)
    _require(not any(c in value for c in "\\:*?[]\n\t"),
             f"{label} must be a literal portable repository-relative path: {value!r}")
    normalized = value[:-1] if directory and value.endswith("/") else value
    parts = normalized.split("/")
    _require(all(part and part not in {".", ".."} for part in parts),
             f"{label} contains an absolute, empty, or traversal component: {value!r}")
    _require(not any(part.endswith((" ", ".")) for part in parts),
             f"{label} has a nonportable trailing dot or space: {value!r}")
    return tuple(part.casefold() for part in parts)


def _paths(value: Any, label: str, *, directory: bool = False) -> None:
    _require(type(value) is list, f"{label} must be an array")
    normalized = [_path(item, label, directory=directory) for item in value]
    _require(len(set(normalized)) == len(normalized), f"{label} contains duplicate paths")


def _snapshot(value: Any, label: str) -> None:
    _keys(value, {"sha256", "file_count"}, label)
    _require(isinstance(value["sha256"], str) and _SHA.fullmatch(value["sha256"]) is not None,
             f"{label}.sha256 must be 64 lowercase hexadecimal characters")
    _integer(value["file_count"], f"{label}.file_count")


def _overlap(left: str, right: str) -> bool:
    a = _path(left, "scope", directory=True)
    b = _path(right, "scope", directory=True)
    return a[:len(b)] == b or b[:len(a)] == a


def _claimed(task: dict) -> bool:
    return task["status"] in _ACTIVE and task["owner"] is not None


def _conflicts(state: dict, candidate: dict) -> list[str]:
    return [
        other["id"] for other in state["tasks"]
        if other["id"] != candidate["id"] and _claimed(other)
        and any(_overlap(a, b) for a in candidate["write_paths"] for b in other["write_paths"])
    ]


def _tasks_by_id(state: dict) -> dict[str, dict]:
    return {task["id"]: task for task in state["tasks"]}


def _dependencies_done(state: dict, task: dict) -> bool:
    tasks = _tasks_by_id(state)
    return all(tasks[name]["status"] == "done" for name in task["depends_on"])


def _latest_receipts(state: dict, task: dict) -> dict[str, dict]:
    receipts = {receipt["id"]: receipt for receipt in state["evidence"]}
    latest: dict[str, dict] = {}
    for name in task["evidence"]:
        receipt = receipts[name]
        latest[receipt["check"]] = receipt
    return latest


def validate_state(state: dict) -> None:
    """Validate the complete schema and cross-record invariants without I/O."""
    _keys(state, {"schema_version", "revision", "updated_at", "project",
                  "source_snapshot", "milestones", "tasks", "evidence", "history"}, "state")
    _require(type(state["schema_version"]) is int and state["schema_version"] == 1,
             "unsupported schema_version; expected integer 1")
    _integer(state["revision"], "revision", 1)
    _timestamp(state["updated_at"], "updated_at")
    _snapshot(state["source_snapshot"], "source_snapshot")
    project = state["project"]
    _keys(project, {"name", "objective", "current_milestone", "coordinator",
                    "truth_documents", "known_limits"}, "project")
    for name in ("name", "objective"):
        _text(project[name], f"project.{name}")
    for name in ("current_milestone", "coordinator"):
        _identifier(project[name], f"project.{name}")
    _paths(project["truth_documents"], "project.truth_documents")
    _require(bool(project["truth_documents"]), "project.truth_documents must not be empty")
    _strings(project["known_limits"], "project.known_limits")
    _require(type(state["milestones"]) is list and bool(state["milestones"]),
             "milestones must be a nonempty array")
    milestones: set[str] = set()
    for milestone in state["milestones"]:
        _keys(milestone, {"id", "title", "objective", "exit_criteria"}, "milestone")
        _identifier(milestone["id"], "milestone.id")
        _require(milestone["id"] not in milestones, f"duplicate milestone {milestone['id']}")
        milestones.add(milestone["id"])
        for field in ("title", "objective"):
            _text(milestone[field], f"milestone.{field}")
        _strings(milestone["exit_criteria"], "milestone.exit_criteria", nonempty=True)
    _require(project["current_milestone"] in milestones, "current_milestone does not exist")
    _require(type(state["tasks"]) is list, "tasks must be an array")
    tasks: dict[str, dict] = {}
    baseline_count = 0
    for task in state["tasks"]:
        _keys(task, set(_DEFINITION) | {"status", "owner", "claim_revision", "checkpoint",
                                      "blocker", "evidence", "completion"}, "task")
        name = task["id"]
        _identifier(name, "task.id")
        _require(name not in tasks, f"duplicate task {name}")
        tasks[name] = task
        for field in ("title", "description"):
            _text(task[field], f"{name}.{field}")
        _identifier(task["milestone"], f"{name}.milestone")
        _require(task["milestone"] in milestones, f"{name}: unknown milestone")
        _integer(task["priority"], f"{name}.priority")
        _strings(task["depends_on"], f"{name}.depends_on", identifiers=True)
        _paths(task["write_paths"], f"{name}.write_paths", directory=True)
        _strings(task["acceptance"], f"{name}.acceptance", nonempty=True)
        _strings(task["required_checks"], f"{name}.required_checks", identifiers=True)
        _require(all(check in _CHECKS for check in task["required_checks"]),
                 f"{name}: unknown required check")
        _strings(task["evidence"], f"{name}.evidence", identifiers=True)
        _require(isinstance(task["status"], str) and task["status"] in _STATUSES,
                 f"{name}: invalid status")
        if task["owner"] is not None:
            _identifier(task["owner"], f"{name}.owner")
        if task["claim_revision"] is not None:
            _integer(task["claim_revision"], f"{name}.claim_revision", 1)
            _require(task["claim_revision"] <= state["revision"],
                     f"{name}: claim_revision is in the future")
        checkpoint = task["checkpoint"]
        _keys(checkpoint, {"summary", "next_action", "files"}, f"{name}.checkpoint")
        _text(checkpoint["summary"], f"{name}.checkpoint.summary")
        _text(checkpoint["next_action"], f"{name}.checkpoint.next_action")
        _paths(checkpoint["files"], f"{name}.checkpoint.files")
        if task["blocker"] is not None:
            _text(task["blocker"], f"{name}.blocker")
        status = task["status"]
        if status == "planned":
            _require(task["owner"] is None and task["claim_revision"] is None,
                     f"{name}: planned tasks must be unowned")
        elif status in {"in_progress", "review"}:
            _require(task["owner"] is not None and task["claim_revision"] is not None,
                     f"{name}: active tasks require an owner and claim revision")
        elif status == "blocked":
            _require((task["owner"] is None) == (task["claim_revision"] is None),
                     f"{name}: blocked owner and claim must both exist or both be null")
        else:
            _require(task["claim_revision"] is None, f"{name}: finished tasks cannot hold claims")
        _require((task["blocker"] is not None) == (status == "blocked"),
                 f"{name}: only blocked tasks must have a blocker")
        completion = task["completion"]
        if status != "done":
            _require(completion is None, f"{name}: unfinished/cancelled task has completion")
        else:
            _require(type(completion) is dict, f"{name}: done requires explicit completion")
            if completion.get("kind") == "baseline":
                _keys(completion, {"kind", "reference", "notes", "at"}, f"{name}.completion")
                _text(completion["reference"], f"{name}.completion.reference")
                try:
                    reference = urlsplit(completion["reference"])
                except ValueError as error:
                    raise WorkflowError(f"{name}: invalid baseline URL") from error
                _require(reference.scheme == "https" and bool(reference.netloc)
                         and not any(c.isspace() for c in completion["reference"]),
                         f"{name}: baseline reference must be an HTTPS URL")
                _require(not task["required_checks"],
                         f"{name}: historical baseline cannot claim new required checks")
                baseline_count += 1
            else:
                _keys(completion, {"kind", "reviewer", "review_kind", "notes", "source_sha256",
                                   "evidence", "at"}, f"{name}.completion")
                _require(completion["kind"] == "reviewed", f"{name}: invalid completion kind")
                _identifier(completion["reviewer"], f"{name}.completion.reviewer")
                _require(completion["reviewer"] == project["coordinator"],
                         f"{name}: completion reviewer must be coordinator")
                _require(task["owner"] is not None, f"{name}: reviewed task must retain owner")
                expected_kind = "self" if task["owner"] == completion["reviewer"] else "peer"
                _require(completion["review_kind"] == expected_kind,
                         f"{name}: review_kind does not match owner/reviewer")
                _require(isinstance(completion["source_sha256"], str)
                         and _SHA.fullmatch(completion["source_sha256"]) is not None,
                         f"{name}: invalid completion source hash")
                _strings(completion["evidence"], f"{name}.completion.evidence", identifiers=True)
            _text(completion["notes"], f"{name}.completion.notes")
            _timestamp(completion["at"], f"{name}.completion.at")
    _require(baseline_count <= 1, "at most one historical baseline completion is allowed")
    for name, task in tasks.items():
        _require(all(dependency in tasks for dependency in task["depends_on"]),
                 f"{name}: unknown dependency")
        _require(name not in task["depends_on"], f"{name}: task depends on itself")
    # Iterative topological traversal keeps validation bounded by the graph size.
    indegree = {name: len(task["depends_on"]) for name, task in tasks.items()}
    children: dict[str, list[str]] = {name: [] for name in tasks}
    for name, task in tasks.items():
        for dependency in task["depends_on"]:
            children[dependency].append(name)
    queue = [name for name in tasks if indegree[name] == 0]
    visited = 0
    while queue:
        name = queue.pop()
        visited += 1
        for child in children[name]:
            indegree[child] -= 1
            if indegree[child] == 0:
                queue.append(child)
    _require(visited == len(tasks), "task dependency graph contains a cycle")
    for task in tasks.values():
        if _claimed(task) or task["status"] == "done":
            _require(_dependencies_done(state, task), f"{task['id']}: dependencies are not done")
        if _claimed(task):
            conflicts = _conflicts(state, task)
            _require(not conflicts, f"{task['id']}: active scope conflicts with {', '.join(conflicts)}")
    _require(type(state["evidence"]) is list, "evidence must be an array")
    receipts: dict[str, dict] = {}
    for index, receipt in enumerate(state["evidence"], 1):
        _keys(receipt, {"id", "task", "check", "actor", "argv", "started_at", "finished_at",
                        "result", "exit_code", "source_before", "source_after", "git",
                        "platform", "log", "note"}, "receipt")
        for field in ("id", "task", "check", "actor"):
            _identifier(receipt[field], f"receipt.{field}")
        _require(receipt["id"] == f"RUN-{index:06d}", "receipt IDs must be contiguous RUN counters")
        _require(receipt["id"] not in receipts, "duplicate receipt ID")
        receipts[receipt["id"]] = receipt
        _require(receipt["task"] in tasks, f"{receipt['id']}: unknown task")
        _require(receipt["check"] in _CHECKS, f"{receipt['id']}: unknown check")
        _strings(receipt["argv"], "receipt.argv", nonempty=True, unique=False)
        _require(receipt["argv"] == _CHECKS[receipt["check"]][0],
                 f"{receipt['id']}: argv must match the portable check allowlist")
        for field in ("started_at", "finished_at"):
            _timestamp(receipt[field], f"receipt.{field}")
        _require(isinstance(receipt["result"], str) and receipt["result"] in _RESULTS,
                 "invalid receipt result")
        _require(receipt["exit_code"] is None or type(receipt["exit_code"]) is int,
                 "receipt.exit_code must be integer or null")
        for field in ("source_before", "source_after"):
            _snapshot(receipt[field], f"receipt.{field}")
        same_source = receipt["source_before"] == receipt["source_after"]
        if receipt["result"] == "passed":
            _require(receipt["exit_code"] == 0 and same_source,
                     f"{receipt['id']}: passing receipt requires exit 0 and unchanged source")
        elif receipt["result"] == "failed":
            _require(receipt["exit_code"] != 0 and same_source,
                     f"{receipt['id']}: failed receipt requires a failure and unchanged source")
        elif receipt["result"] == "invalidated":
            _require(not same_source, f"{receipt['id']}: invalidated receipt requires changed source")
        provenance = receipt["git"]
        _keys(provenance, {"status", "commit", "dirty"}, "receipt.git")
        if provenance["status"] == "unknown":
            _require(provenance["commit"] is None and provenance["dirty"] is None,
                     "unknown Git provenance must use null commit and dirty")
        else:
            _require(provenance["status"] == "git"
                     and isinstance(provenance["commit"], str)
                     and _COMMIT.fullmatch(provenance["commit"]) is not None
                     and type(provenance["dirty"]) is bool, "invalid Git provenance")
        _text(receipt["platform"], "receipt.platform")
        _text(receipt["note"], "receipt.note", empty=True)
        log = receipt["log"]
        _keys(log, {"path", "sha256", "bytes"}, "receipt.log")
        log_parts = _path(log["path"], "receipt.log.path")
        _require(len(log_parts) >= 3 and log_parts[:2] == ("out", "agent-runs"),
                 "receipt logs must be under out/agent-runs/")
        _require(isinstance(log["sha256"], str) and _SHA.fullmatch(log["sha256"]) is not None,
                 "invalid receipt log SHA256")
        _integer(log["bytes"], "receipt.log.bytes")
    used_receipts: set[str] = set()
    positions = {name: index for index, name in enumerate(receipts)}
    for task in tasks.values():
        _require(all(name in receipts for name in task["evidence"]),
                 f"{task['id']}: unknown evidence reference")
        _require(all(receipts[name]["task"] == task["id"] for name in task["evidence"]),
                 f"{task['id']}: evidence belongs to another task")
        _require(task["evidence"] == sorted(task["evidence"], key=positions.__getitem__),
                 f"{task['id']}: evidence references must preserve receipt order")
        used_receipts.update(task["evidence"])
        completion = task["completion"]
        if completion is not None and completion["kind"] == "reviewed":
            _require(all(name in task["evidence"] for name in completion["evidence"]),
                     f"{task['id']}: completion references missing task evidence")
            selected = [receipts[name] for name in completion["evidence"]]
            _require(len(selected) == len(task["required_checks"])
                     and {item["check"] for item in selected} == set(task["required_checks"]),
                     f"{task['id']}: completion must cover each required check exactly once")
            _require(all(item["result"] == "passed"
                         and item["source_after"]["sha256"] == completion["source_sha256"]
                         for item in selected), f"{task['id']}: invalid completion evidence")
    _require(used_receipts == set(receipts), "every receipt must be attached to its task")
    _require(type(state["history"]) is list and len(state["history"]) == state["revision"],
             "history must contain every revision from 1 through the current revision")
    for index, event in enumerate(state["history"], 1):
        _keys(event, {"revision", "at", "actor", "action", "task", "summary"}, "history event")
        _integer(event["revision"], "history.revision", 1)
        _require(event["revision"] == index, "history revisions must be contiguous")
        _timestamp(event["at"], "history.at")
        _identifier(event["actor"], "history.actor")
        _identifier(event["action"], "history.action")
        _text(event["summary"], "history.summary")
        if event["task"] is not None:
            _identifier(event["task"], "history.task")
            _require(event["task"] in tasks, "history references unknown task")
    _require(state["history"][-1]["at"] == state["updated_at"],
             "updated_at must equal the latest history event time")


def _root(path: Path) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise WorkflowError(f"cannot resolve repository root {path}: {error}") from error
    _require(resolved.is_dir(), f"repository root is not a directory: {resolved}")
    return resolved


def _is_link(info: os.stat_result) -> bool:
    """Include Windows junctions and other reparse entries, not just POSIX symlinks."""
    return (stat.S_ISLNK(info.st_mode)
            or bool(getattr(info, "st_file_attributes", 0)
                    & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)))


def _managed(root: Path, relative: str, *, create_parents: bool = False,
             required: bool = False) -> Path:
    """Reject existing symlinks, special files, and hard-link aliases on a fixed path."""
    _path(relative, "managed path")
    parts = relative.split("/")
    current = root
    for part in parts[:-1]:
        current = current / part
        try:
            info = current.lstat()
        except FileNotFoundError:
            if not create_parents:
                _require(not required, f"missing managed path parent: {current}")
                return root / relative
            try:
                current.mkdir()
            except FileExistsError:
                pass
            info = current.lstat()
        _require(stat.S_ISDIR(info.st_mode) and not _is_link(info),
                 f"managed path parent must be a real directory: {current}")
    target = root / relative
    try:
        info = target.lstat()
    except FileNotFoundError:
        _require(not required, f"missing managed file: {target}")
        return target
    _require(stat.S_ISREG(info.st_mode) and not _is_link(info),
             f"managed file must be regular, not a symlink or special file: {target}")
    _require(info.st_nlink == 1, f"managed file must not have hard-link aliases: {target}")
    return target


def _read_regular(path: Path, *, maximum: int | None = None,
                  single_link: bool = False) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    # O_NONBLOCK avoids a FIFO race between lstat and open on POSIX.
    flags |= getattr(os, "O_NONBLOCK", 0)
    try:
        before = path.lstat()
        _require(stat.S_ISREG(before.st_mode) and not _is_link(before),
                 f"expected a regular file: {path}")
        if single_link:
            _require(before.st_nlink == 1, f"hard-link aliases are unsupported: {path}")
        with os.fdopen(os.open(path, flags), "rb") as stream:
            opened = os.fstat(stream.fileno())
            _require(stat.S_ISREG(opened.st_mode) and not _is_link(opened)
                     and (opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino),
                     f"file changed while opening: {path}")
            if single_link:
                _require(opened.st_nlink == 1, f"hard-link aliases are unsupported: {path}")
            _require(maximum is None or opened.st_size <= maximum, f"file is too large: {path}")
            data = stream.read() if maximum is None else stream.read(maximum + 1)
            _require(maximum is None or len(data) <= maximum, f"file is too large: {path}")
            return data
    except OSError as error:
        raise WorkflowError(f"cannot read {path}: {error}") from error


def _strict_json(data: bytes, label: str) -> Any:
    def pairs(items: list[tuple[str, Any]]) -> dict:
        result: dict = {}
        for key, value in items:
            _require(key not in result, f"{label}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    def constant(value: str) -> None:
        raise WorkflowError(f"{label}: nonfinite JSON number {value}")

    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise WorkflowError(f"{label}: invalid UTF-8 JSON: {error}") from error


def _truth_documents(root: Path, state: dict) -> None:
    for relative in state["project"]["truth_documents"]:
        _managed(root, relative, required=True)


def load_state(root: Path) -> dict:
    """Load strict UTF-8 JSON and validate state, paths, and truth-document references."""
    root = _root(Path(root))
    path = _managed(root, _STATE, required=True)
    state = _strict_json(_read_regular(path, maximum=_MAX_JSON_BYTES, single_link=True), str(path))
    validate_state(state)
    _truth_documents(root, state)
    return state


def _ignored(relative: str, name: str) -> bool:
    if relative in {_STATE, *_VIEWS}:
        return True
    if name in _IGNORED_DIRS or name in {".DS_Store", "Thumbs.db"}:
        return True
    if "/" not in relative and name in _ROOT_OUTPUTS:
        return True
    if name.endswith((".pyc", ".pyo", ".pyd")):
        return True
    parent = relative.rpartition("/")[0]
    prefixes = _TEMP_PREFIXES.get(parent)
    if isinstance(prefixes, str):
        prefixes = (prefixes,)
    return bool(prefixes and name.endswith(".tmp")
                and any(name.startswith(prefix) and len(name) > len(prefix) + len(".tmp")
                        for prefix in prefixes))


def _content_digest(path: Path) -> bytes:
    """Hash bytes, using CRLF canonicalization only for valid UTF-8 NUL-free text."""
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    flags |= getattr(os, "O_NONBLOCK", 0)
    before = path.lstat()
    _require(stat.S_ISREG(before.st_mode) and not _is_link(before),
             f"source must be a regular file: {path}")
    binary_hash = hashlib.sha256()
    text_hash = hashlib.sha256()
    decoder = codecs.getincrementaldecoder("utf-8")("strict")
    text = True
    pending = b""
    with os.fdopen(os.open(path, flags), "rb") as stream:
        opened = os.fstat(stream.fileno())
        _require(stat.S_ISREG(opened.st_mode) and not _is_link(opened)
                 and (opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino),
                 f"source changed while opening: {path}")
        while block := stream.read(1024 * 1024):
            binary_hash.update(block)
            if text:
                try:
                    decoder.decode(block, final=False)
                    if b"\0" in block:
                        text = False
                except UnicodeDecodeError:
                    text = False
                if text:
                    joined = pending + block
                    pending = b"\r" if joined.endswith(b"\r") else b""
                    if pending:
                        joined = joined[:-1]
                    text_hash.update(joined.replace(b"\r\n", b"\n"))
        after = os.fstat(stream.fileno())
    _require((opened.st_size, opened.st_mtime_ns, opened.st_ctime_ns)
             == (after.st_size, after.st_mtime_ns, after.st_ctime_ns),
             f"source changed while hashing: {path}; retry after source edits stop")
    if text:
        try:
            decoder.decode(b"", final=True)
        except UnicodeDecodeError:
            text = False
        text_hash.update(pending)
    return text_hash.digest() if text else binary_hash.digest()


def snapshot_source(root: Path) -> dict:
    """Fingerprint explicit source inputs independently of Git, mtimes, or location."""
    root = _root(Path(root))
    files: list[tuple[str, Path]] = []
    pending = [root]
    try:
        while pending:
            directory = pending.pop()
            with os.scandir(directory) as entries:
                for entry in entries:
                    path = directory / entry.name
                    relative = path.relative_to(root).as_posix()
                    if _ignored(relative, entry.name):
                        continue
                    info = entry.stat(follow_symlinks=False)
                    _require(not _is_link(info),
                             f"source symlinks and reparse entries are unsupported: {path}")
                    if stat.S_ISDIR(info.st_mode):
                        pending.append(path)
                    else:
                        _require(stat.S_ISREG(info.st_mode),
                                 f"source special files are unsupported: {path}")
                        files.append((relative, path))
        digest = hashlib.sha256(b"ge-agent-source-v1\0")
        for relative, path in sorted(files):
            name = relative.encode("utf-8")
            digest.update(len(name).to_bytes(8, "big"))
            digest.update(name)
            digest.update(_content_digest(path))
        return {"sha256": digest.hexdigest(), "file_count": len(files)}
    except (OSError, UnicodeError) as error:
        raise WorkflowError(f"cannot fingerprint source under {root}: {error}") from error


def _markdown(value: Any) -> str:
    text = html.escape(str(value), quote=False).replace("\r", " ").replace("\n", " ").replace("\t", " ").strip()
    for character in ("\\", chr(96), "*", "_", "{", "}", "[", "]", "(", ")", "#", "+", "-", "!", "|", ">"):
        text = text.replace(character, "\\" + character)
    return text


def _ready(state: dict) -> list[dict]:
    return sorted((task for task in state["tasks"] if task["status"] == "planned"
                   and _dependencies_done(state, task) and not _conflicts(state, task)),
                  key=lambda task: (task["priority"], task["id"]))


def _summary_task(task: dict) -> dict:
    return {name: task[name] for name in
            ("id", "title", "priority", "milestone", "status", "owner", "blocker", "write_paths")}


def render_documents(state: dict) -> dict[str, str]:
    """Render both committed views from state alone, without filesystem/time/Git reads."""
    validate_state(state)
    project = state["project"]
    milestones = {item["id"]: item for item in state["milestones"]}
    milestone = milestones[project["current_milestone"]]
    ready = _ready(state)
    heading = [
        "<!-- Generated by tools/agent_state.py. Edit the canonical ledger through its CLI. -->",
        "",
    ]
    metadata = [
        f"- **Project:** {_markdown(project['name'])}",
        f"- **Ledger revision:** {state['revision']}",
        f"- **Recorded update:** {_markdown(state['updated_at'])}",
        f"- **Coordinator:** {_markdown(project['coordinator'])}",
        f"- **Source fingerprint:** {state['source_snapshot']['sha256']}",
        f"- **Source files:** {state['source_snapshot']['file_count']}",
        "",
        "This view records explicit operations. Source freshness is checked by the CLI; "
        "a generated view alone is not proof that the current checkout is unchanged.",
        "",
    ]
    status = heading + ["# Agent project status", ""] + metadata
    status += [
        "## Objective and current milestone", "",
        _markdown(project["objective"]), "",
        f"**{_markdown(milestone['id'])}: {_markdown(milestone['title'])}**", "",
        _markdown(milestone["objective"]), "",
        "Exit criteria:", "",
        *[f"- {_markdown(item)}" for item in milestone["exit_criteria"]], "",
        "Milestone focus is planning context. Acceptance of its work remains explicit.", "",
        "## Known limits", "",
        *([f"- {_markdown(item)}" for item in project["known_limits"]] or ["- None recorded."]), "",
        "## Task board", "",
        "| Task | Priority | Milestone | Status | Owner | Dependencies | Next action |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    ready_ids = {task["id"] for task in ready}
    for task in sorted(state["tasks"], key=lambda item: (item["priority"], item["id"])):
        label = task["status"] + (" (ready)" if task["id"] in ready_ids else "")
        status.append("| " + " | ".join((
            _markdown(f"{task['id']}: {task['title']}"), str(task["priority"]),
            _markdown(task["milestone"]), _markdown(label), _markdown(task["owner"] or "unowned"),
            _markdown(", ".join(task["depends_on"]) or "none"),
            _markdown(task["checkpoint"]["next_action"]),
        )) + " |")
    status += ["", "## Active claims and blockers", ""]
    active = [task for task in state["tasks"] if _claimed(task) or task["status"] == "blocked"]
    if not active:
        status.append("No active claims or blockers are recorded.")
    for task in active:
        status += [
            f"### {_markdown(task['id'])}", "",
            f"- Status: {_markdown(task['status'])}",
            f"- Owner: {_markdown(task['owner'] or 'unowned')}",
            f"- Claim revision: {task['claim_revision'] if task['claim_revision'] is not None else 'none'}",
            f"- Write scope: {_markdown(', '.join(task['write_paths']) or 'review only')}",
            f"- Blocker: {_markdown(task['blocker'] or 'none')}",
            f"- Preserved work: {_markdown(task['checkpoint']['summary'])}",
            f"- Next action: {_markdown(task['checkpoint']['next_action'])}", "",
        ]
    status += ["", "## Evidence observations", "",
               "These are historical observations. A pass neither completes a task nor "
               "remains current after source changes. Ignored local logs may be absent in a clone.", "",
               "| Receipt | Task | Check | Result | Source | Recorded at |",
               "| --- | --- | --- | --- | --- | --- |"]
    for receipt in state["evidence"]:
        source = receipt["source_after"]["sha256"]
        current = "recorded snapshot" if receipt["source_after"] == state["source_snapshot"] else "historical"
        status.append("| " + " | ".join((
            receipt["id"], _markdown(receipt["task"]), _markdown(receipt["check"]),
            _markdown(receipt["result"]), f"{source[:16]} ({current})",
            _markdown(receipt["finished_at"]),
        )) + " |")
    if not state["evidence"]:
        status.append("| None | — | — | No check receipts recorded | — | — |")
    status += ["", "## Completion records", ""]
    completed = [task for task in state["tasks"] if task["status"] == "done"]
    if not completed:
        status.append("No completed tasks are recorded.")
    for task in completed:
        completion = task["completion"]
        if completion["kind"] == "baseline":
            status += [
                f"- {_markdown(task['id'])}: historical baseline; "
                f"{_markdown(completion['notes'])} "
                f"Reference: {_markdown(completion['reference'])}. "
                "This is not a newly executed check.",
            ]
        else:
            status.append(
                f"- {_markdown(task['id'])}: {_markdown(completion['review_kind'])} review by "
                f"{_markdown(completion['reviewer'])}; {_markdown(completion['notes'])} "
                f"Source: {completion['source_sha256']}.")
    status += ["", "## Recent operations", ""]
    status += [
        f"- Revision {event['revision']} — {_markdown(event['at'])} — "
        f"{_markdown(event['actor'])}: {_markdown(event['action'])}"
        f"{' ' + _markdown(event['task']) if event['task'] else ''}. {_markdown(event['summary'])}"
        for event in state["history"][-20:]
    ]
    status += ["", "The canonical ledger retains the complete operation history.", ""]
    handoff = heading + ["# Agent continuation and handoff", ""] + metadata
    handoff += [
        "## Start here", "",
        "Read [the coordinator prompt](CONTINUE.md), [the workflow guide](WORKFLOW.md), "
        "and [the worker prompt](WORKER.md). Run the read-only status and check commands "
        "before selecting work. Current user scope and authorization remain controlling.", "",
        f"Current milestone: **{_markdown(milestone['id'])}: {_markdown(milestone['title'])}**.", "",
        "Source-of-truth documents:", "",
    ]
    handoff += [
        f"- [{_markdown(relative)}](../../{quote(relative, safe='/._-')})"
        for relative in project["truth_documents"]
    ]
    handoff += ["", "## Ready work", ""]
    if ready:
        for task in ready:
            handoff.append(
                f"- {_markdown(task['id'])} (priority {task['priority']}): "
                f"{_markdown(task['title'])}. Next: {_markdown(task['checkpoint']['next_action'])}")
    else:
        handoff.append("No planned task currently has both completed dependencies and a clear write scope.")
    handoff += [
        "",
        "Ready is derived from dependencies and active claims. A new claim also requires "
        "a current source fingerprint and the revision observed by the claimant.",
        "",
        "## Preserved task context", "",
    ]
    unfinished = [task for task in state["tasks"] if task["status"] not in {"done", "cancelled"}]
    for task in sorted(unfinished, key=lambda item: (item["priority"], item["id"])):
        latest = _latest_receipts(state, task)
        handoff += [
            f"### {_markdown(task['id'])}: {_markdown(task['title'])}", "",
            _markdown(task["description"]), "",
            f"- State / owner: {_markdown(task['status'])} / {_markdown(task['owner'] or 'unowned')}",
            f"- Write scope: {_markdown(', '.join(task['write_paths']) or 'review only')}",
            f"- Dependencies: {_markdown(', '.join(task['depends_on']) or 'none')}",
            f"- Blocker: {_markdown(task['blocker'] or 'none')}",
            f"- Checkpoint: {_markdown(task['checkpoint']['summary'])}",
            f"- Files: {_markdown(', '.join(task['checkpoint']['files']) or 'none recorded')}",
            f"- Next action: {_markdown(task['checkpoint']['next_action'])}",
            f"- Required checks: {_markdown(', '.join(task['required_checks']) or 'none')}",
            "",
            "Acceptance:", "",
            *[f"- {_markdown(item)}" for item in task["acceptance"]], "",
        ]
        for check in task["required_checks"]:
            receipt = latest.get(check)
            if receipt is None:
                handoff.append(f"- {_markdown(check)}: no receipt.")
            else:
                fresh = receipt["source_after"] == state["source_snapshot"]
                handoff.append(
                    f"- {_markdown(check)}: {receipt['id']} / {_markdown(receipt['result'])} / "
                    f"{'recorded source snapshot' if fresh else 'historical source snapshot'}.")
        if task["required_checks"]:
            handoff.append("")
    if not unfinished:
        handoff += ["No unfinished tasks are recorded. Create explicitly scoped future work when authorized.", ""]
    handoff += [
        "## Recovery rules", "",
        "- Preserve source edits and checkpoint context before releasing ownership.",
        "- Never steal a claim or a local lock based on age. Inspect lock-info and identify the original process.",
        "- A stale source fingerprint needs an explicit checkpoint or coordinator reconcile.",
        "- A partially published view needs sync. Sync does not refresh source evidence or advance tasks.",
        "- Submit and accept remain explicit decisions; only the latest current passing receipts qualify.",
        "- Record real checks and actual publication outcomes. This ledger never grants commit or push authority.", "",
    ]
    return {_VIEWS[0]: "\n".join(status).rstrip() + "\n",
            _VIEWS[1]: "\n".join(handoff).rstrip() + "\n"}


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")


@contextmanager
def _lock(root: Path, relative: str, actor: str, *, wait_seconds: float = 0,
          **details: str) -> Iterator[dict]:
    path = _managed(root, relative, create_parents=True)
    metadata = {"token": uuid.uuid4().hex, "actor": actor, "pid": os.getpid(),
                "hostname": socket.gethostname(), "created_at": _now(), **details}
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
    deadline = time.monotonic() + wait_seconds
    while True:
        try:
            descriptor = os.open(path, flags, 0o600)
            break
        except FileExistsError as error:
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(0.05, remaining))
                _managed(root, relative)
                continue
            raise WorkflowError(
                f"local lock already exists: {relative}; run lock-info, inspect its owner and preserved "
                "work, stop the original process, then manually remove only that exact lock if safe. "
                "Locks are never stolen by age or PID.") from error
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(_json_bytes(metadata))
            stream.flush()
            os.fsync(stream.fileno())
        yield metadata
    finally:
        # Never delete a different actor's replacement lock.
        try:
            existing = _strict_json(_read_regular(path, maximum=65536, single_link=True), relative)
            if isinstance(existing, dict) and existing.get("token") == metadata["token"]:
                path.unlink()
        except (OSError, WorkflowError):
            pass


def _lock_info(root: Path) -> dict:
    locks = {}
    for relative in (_STATE_LOCK, _RUN_LOCK):
        path = _managed(root, relative)
        if path.exists():
            data = _read_regular(path, maximum=65536, single_link=True)
            try:
                locks[relative] = _strict_json(data, relative)
            except WorkflowError:
                locks[relative] = {"unparsed_metadata": data.decode("utf-8", errors="backslashreplace")}
        else:
            locks[relative] = None
    return locks


def _publish(root: Path, state: dict, *, ledger: bool) -> None:
    validate_state(state)
    _truth_documents(root, state)
    documents = render_documents(state)
    outputs = ({_STATE: _json_bytes(state)} if ledger else {})
    outputs.update({relative: text.encode("utf-8") for relative, text in documents.items()})
    # Validate all final targets before creating temporary files or publishing anything.
    for relative in (_STATE, *_VIEWS):
        _managed(root, relative, required=relative == _STATE)
    temporary: dict[str, Path] = {}
    committed = False
    try:
        for relative, data in outputs.items():
            target = _managed(root, relative, create_parents=True)
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{target.name}.", suffix=".tmp", dir=target.parent)
            temporary[relative] = Path(temporary_name)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            # Repository records are ordinary read-only-to-other-users text, not secrets.
            temporary[relative].chmod(0o644)
        for relative in outputs:
            target = _managed(root, relative, required=relative == _STATE)
            os.replace(temporary[relative], target)
            if relative == _STATE:
                committed = True
    except (OSError, KeyboardInterrupt) as error:
        if committed:
            raise WorkflowError(
                f"ledger published at revision {state['revision']}, but generated views may be stale; "
                f"run sync before continuing. Publication error: {error}") from error
        raise WorkflowError(f"publication failed before ledger commit; state was preserved: {error}") from error
    except WorkflowError as error:
        if committed:
            raise WorkflowError(
                f"ledger published at revision {state['revision']}, but generated views may be stale; "
                f"run sync before continuing. {error}") from error
        raise
    finally:
        for temporary_path in temporary.values():
            try:
                temporary_path.unlink(missing_ok=True)
            except OSError:
                pass


def _current_source(root: Path, state: dict) -> dict:
    snapshot = snapshot_source(root)
    _require(snapshot == state["source_snapshot"],
             "source fingerprint is stale; preserve edits, then checkpoint the owning task "
             "or use coordinator reconcile with a factual summary")
    return snapshot


def _task(state: dict, name: str) -> dict:
    _identifier(name, "task ID")
    task = _tasks_by_id(state).get(name)
    _require(task is not None, f"unknown task: {name}")
    return task


def _owner(task: dict, actor: str) -> None:
    _require(task["owner"] == actor, f"{task['id']}: only the recorded owner may perform this action")


def _coordinator(state: dict, actor: str) -> None:
    _require(actor == state["project"]["coordinator"], "only the recorded coordinator may perform this action")


def _claimable(state: dict, task: dict) -> None:
    _require(_dependencies_done(state, task), f"{task['id']}: dependencies are not done")
    conflicts = _conflicts(state, task)
    _require(not conflicts, f"{task['id']}: write scope conflicts with {', '.join(conflicts)}")


def _advance(state: dict, actor: str, action: str, task: str | None, summary: str) -> None:
    _text(summary, "summary")
    state["revision"] += 1
    state["updated_at"] = _now()
    state["history"].append({"revision": state["revision"], "at": state["updated_at"],
                             "actor": actor, "action": action, "task": task, "summary": summary})


def _evidence_for_completion(state: dict, task: dict, snapshot: dict) -> list[str]:
    latest = _latest_receipts(state, task)
    selected = []
    for check in task["required_checks"]:
        receipt = latest.get(check)
        _require(receipt is not None, f"{task['id']}: required check {check} has no receipt")
        _require(receipt["result"] == "passed",
                 f"{task['id']}: latest {check} receipt is {receipt['result']}")
        _require(receipt["source_before"] == snapshot and receipt["source_after"] == snapshot,
                 f"{task['id']}: latest {check} receipt is from an older source snapshot")
        selected.append(receipt["id"])
    return selected


def _spec(path: str) -> dict:
    # Resolve relative to the caller, not the repository root.
    source = Path(path).absolute()
    value = _strict_json(_read_regular(source, maximum=_MAX_JSON_BYTES), str(source))
    _keys(value, set(_DEFINITION) | {"next_action"}, "task spec")
    _identifier(value["id"], "task spec.id")
    _text(value["next_action"], "task spec.next_action")
    return value


def _cas(state: dict, args: argparse.Namespace) -> None:
    _identifier(args.actor, "actor")
    _integer(args.expect_revision, "expect-revision", 1)
    _require(state["revision"] == args.expect_revision,
             f"stale revision: expected {args.expect_revision}, current {state['revision']}; "
             "reload status and reassess before retrying")


def _mutation(root: Path, args: argparse.Namespace) -> int:
    _identifier(args.actor, "actor")
    _integer(args.expect_revision, "expect-revision", 1)
    with _lock(root, _STATE_LOCK, args.actor):
        state = load_state(root)
        _cas(state, args)
        command = args.command
        task = _task(state, args.task) if hasattr(args, "task") else None
        summary = getattr(args, "summary", None)
        if command in {"add", "amend", "focus", "reconcile", "accept", "cancel"}:
            _coordinator(state, args.actor)
        if command in {"submit", "accept"}:
            # A pending rerun may invalidate older evidence. Do not make it
            # ineligible to attach merely by advancing the task into review.
            run_path = _managed(root, _RUN_LOCK)
            if run_path.exists():
                run = _strict_json(_read_regular(run_path, maximum=65536, single_link=True), _RUN_LOCK)
                _require(isinstance(run, dict) and run.get("task") != task["id"],
                         f"{task['id']}: a check is still running; wait for its receipt before {command}")
        if command == "claim":
            _require(task["status"] == "planned", f"{task['id']}: claim requires planned status")
            _current_source(root, state)
            _claimable(state, task)
            task.update(status="in_progress", owner=args.actor, claim_revision=state["revision"] + 1)
            summary = f"Claimed {task['id']} with its recorded write scope."
        elif command == "checkpoint":
            _owner(task, args.actor)
            _require(task["status"] in {"in_progress", "blocked"}, "checkpoint requires in_progress or blocked")
            _text(args.next_action, "next-action")
            _paths(args.file, "checkpoint files")
            task["checkpoint"] = {"summary": args.summary, "next_action": args.next_action, "files": args.file}
            state["source_snapshot"] = snapshot_source(root)
        elif command == "block":
            _owner(task, args.actor)
            _require(task["status"] in {"in_progress", "review"}, "block requires in_progress or review")
            _text(args.reason, "reason")
            _text(args.next_action, "next-action")
            task.update(status="blocked", blocker=args.reason)
            task["checkpoint"]["next_action"] = args.next_action
            summary = args.reason
        elif command == "resume":
            _require(task["status"] in {"blocked", "review"}, "resume requires blocked or review")
            _require(args.actor in {task["owner"], state["project"]["coordinator"]},
                     "only the owner or coordinator may resume")
            _claimable(state, task)
            if task["owner"] is None:
                task["owner"] = args.actor
                task["claim_revision"] = state["revision"] + 1
            task.update(status="in_progress", blocker=None, completion=None)
            task["checkpoint"]["summary"] = args.summary
        elif command == "handoff":
            _require(task["status"] in _ACTIVE and task["owner"] is not None,
                     "handoff requires an unfinished owned task")
            _require(args.actor in {task["owner"], state["project"]["coordinator"]},
                     "only the owner or coordinator may hand off")
            _text(args.next_action, "next-action")
            task["checkpoint"].update(summary=args.summary, next_action=args.next_action)
            task.update(status="planned", owner=None, claim_revision=None, blocker=None)
        elif command == "submit":
            _owner(task, args.actor)
            _require(task["status"] == "in_progress", "submit requires in_progress")
            snapshot = _current_source(root, state)
            _claimable(state, task)
            _evidence_for_completion(state, task, snapshot)
            _require(task["blocker"] is None, "blocked work cannot be submitted")
            task["status"] = "review"
            task["checkpoint"]["summary"] = args.summary
        elif command == "accept":
            _require(task["status"] == "review", "accept requires review")
            snapshot = _current_source(root, state)
            _claimable(state, task)
            evidence = _evidence_for_completion(state, task, snapshot)
            self_review = task["owner"] == args.actor
            _require(not self_review or args.self_review,
                     "coordinator-owned work requires explicit --self-review")
            task.update(status="done", claim_revision=None)
            task["completion"] = {
                "kind": "reviewed", "reviewer": args.actor,
                "review_kind": "self" if self_review else "peer",
                "notes": args.summary, "source_sha256": snapshot["sha256"],
                "evidence": evidence, "at": _now(),
            }
            task["checkpoint"]["summary"] = args.summary
        elif command == "cancel":
            _require(task["status"] not in {"done", "cancelled"}, "cancel requires unfinished work")
            _text(args.next_action, "next-action")
            task["checkpoint"].update(summary=args.summary, next_action=args.next_action)
            task.update(status="cancelled", claim_revision=None, blocker=None, completion=None)
        elif command in {"add", "amend"}:
            definition = _spec(args.spec)
            if command == "add":
                _require(definition["id"] not in _tasks_by_id(state), "task ID already exists")
                task = {name: definition[name] for name in _DEFINITION}
                task.update(status="planned", owner=None, claim_revision=None, blocker=None,
                            checkpoint={"summary": "Not started.", "next_action": definition["next_action"],
                                        "files": []}, evidence=[], completion=None)
                state["tasks"].append(task)
                summary = f"Added planned task {task['id']}: {task['title']}."
            else:
                _require(task["status"] == "planned"
                         or (task["status"] == "blocked" and task["owner"] is None),
                         "amend requires planned or unowned blocked work")
                _require(definition["id"] == task["id"], "amend spec ID must match task ID")
                task.update({name: definition[name] for name in _DEFINITION})
                task["checkpoint"]["next_action"] = definition["next_action"]
        elif command == "focus":
            _require(args.milestone in {item["id"] for item in state["milestones"]}, "unknown milestone")
            state["project"]["current_milestone"] = args.milestone
        elif command == "reconcile":
            state["source_snapshot"] = snapshot_source(root)
        else:
            raise WorkflowError(f"unsupported mutation: {command}")
        _advance(state, args.actor, command, task["id"] if task is not None else None, summary)
        _publish(root, state, ledger=True)
    print(f"{command}: published ledger revision {state['revision']} and both generated views.")
    return 0


def _git_metadata(root: Path) -> dict:
    unknown = {"status": "unknown", "commit": None, "dirty": None}
    # Never discover a containing repository or inherit a caller's Git overrides.
    if not (root / ".git").exists():
        return unknown
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    environment.update(GIT_TERMINAL_PROMPT="0", GIT_NO_LAZY_FETCH="1")
    argv = ["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-C", str(root)]
    try:
        top = subprocess.run(argv + ["rev-parse", "--show-toplevel"], env=environment,
                             capture_output=True, timeout=10, check=True)
        if Path(os.fsdecode(top.stdout).strip()).resolve() != root:
            return unknown
        commit = subprocess.run(argv + ["rev-parse", "HEAD"], env=environment,
                                capture_output=True, timeout=10, check=True).stdout.decode("ascii").strip()
        if _COMMIT.fullmatch(commit) is None:
            return unknown
        dirty = bool(subprocess.run(argv + ["status", "--porcelain", "--untracked-files=normal"],
                                    env=environment, capture_output=True, timeout=10,
                                    check=True).stdout)
        return {"status": "git", "commit": commit, "dirty": dirty}
    except (OSError, UnicodeError, subprocess.SubprocessError):
        return unknown


def _log_metadata(path: Path, relative: str) -> dict:
    """Digest exact log bytes using bounded memory, without text normalization."""
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    flags |= getattr(os, "O_NONBLOCK", 0)
    before = path.lstat()
    _require(stat.S_ISREG(before.st_mode) and not _is_link(before) and before.st_nlink == 1,
             f"check log is no longer an unaliased regular file: {relative}")
    digest = hashlib.sha256()
    size = 0
    with os.fdopen(os.open(path, flags), "rb") as stream:
        opened = os.fstat(stream.fileno())
        _require(stat.S_ISREG(opened.st_mode) and not _is_link(opened)
                 and opened.st_nlink == 1
                 and (opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino),
                 f"check log changed while opening: {relative}")
        while block := stream.read(1024 * 1024):
            digest.update(block)
            size += len(block)
        after = os.fstat(stream.fileno())
    _require((opened.st_size, opened.st_mtime_ns, opened.st_ctime_ns)
             == (after.st_size, after.st_mtime_ns, after.st_ctime_ns),
             f"check log changed while hashing: {relative}; inspect descendant processes")
    return {"path": relative, "sha256": digest.hexdigest(), "bytes": size}


def _run_check(root: Path, args: argparse.Namespace) -> int:
    _identifier(args.actor, "actor")
    _identifier(args.task, "task ID")
    _require(args.check in _CHECKS, f"unknown check: {args.check}")
    with _lock(root, _RUN_LOCK, args.actor, task=args.task, check=args.check) as run_lock:
        with _lock(root, _STATE_LOCK, args.actor):
            state = load_state(root)
            _cas(state, args)
            task = _task(state, args.task)
            _owner(task, args.actor)
            _require(task["status"] == "in_progress", "run-check requires in_progress")
            _require(args.check in task["required_checks"], "check is not required by this task")
            before = _current_source(root, state)
            claim_revision = task["claim_revision"]
            receipt_id = f"RUN-{len(state['evidence']) + 1:06d}"
            display_argv, timeout = _CHECKS[args.check]
            # Validate the actual script before execution; it must remain inside the source root.
            script = _managed(root, display_argv[1], required=True)
        log_relative = f"out/agent-runs/{receipt_id}-{run_lock['token']}.log"
        log_path = _managed(root, log_relative, create_parents=True)
        argv = [sys.executable, str(script), *display_argv[2:]]
        started = _now()
        provenance = _git_metadata(root)
        result = "failed"
        code: int | None = None
        note = ""
        process: subprocess.Popen | None = None
        with log_path.open("xb") as log:
            try:
                process = subprocess.Popen(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                                           stdin=subprocess.DEVNULL, shell=False)
                code = process.wait(timeout=timeout)
                result = "passed" if code == 0 else "failed"
                note = "Process exited successfully." if code == 0 else f"Process exited with status {code}."
            except subprocess.TimeoutExpired:
                result = "timeout"
                note = (f"Check exceeded {timeout} seconds; the direct child was stopped. "
                        "Inspect possible descendant processes before reusing build outputs.")
                if process is not None:
                    process.kill()
                    code = process.wait(timeout=10)
            except KeyboardInterrupt:
                result = "interrupted"
                note = ("Operator interrupted the check; the direct child was stopped. "
                        "Inspect possible descendant processes before reusing build outputs.")
                if process is not None:
                    process.kill()
                    code = process.wait(timeout=10)
            except OSError as error:
                result = "failed"
                note = f"Check process failed to start or finish: {type(error).__name__} (errno {error.errno})."
                log.write((f"{note}\n{error}\n").encode("utf-8", errors="backslashreplace"))
            finally:
                log.flush()
                os.fsync(log.fileno())
        try:
            after = snapshot_source(root)
        except WorkflowError as error:
            raise WorkflowError(f"check log preserved at {log_relative}; source could not be fingerprinted "
                                f"after execution, so no receipt was attached: {error}") from error
        if after != before:
            result = "invalidated"
            note += " Source changed during execution; this receipt cannot qualify as passing evidence."
        finished = _now()
        try:
            log_metadata = _log_metadata(log_path, log_relative)
            with _lock(root, _STATE_LOCK, args.actor, wait_seconds=5):
                current = load_state(root)
                task = _task(current, args.task)
                _require(task["owner"] == args.actor and task["claim_revision"] == claim_revision
                         and task["status"] in {"in_progress", "blocked"},
                         "claim changed during execution; no receipt attached")
                receipt = {
                    "id": f"RUN-{len(current['evidence']) + 1:06d}", "task": args.task, "check": args.check,
                    "actor": args.actor, "argv": list(display_argv), "started_at": started, "finished_at": finished,
                    "result": result, "exit_code": code, "source_before": before, "source_after": after,
                    "git": provenance, "platform": f"{platform.system()} {platform.machine()} / Python {platform.python_version()}",
                    "log": log_metadata, "note": note,
                }
                current["evidence"].append(receipt)
                task["evidence"].append(receipt["id"])
                _advance(current, args.actor, "run-check", args.task,
                         f"{args.check}: {result}; receipt {receipt['id']}; {note}")
                _publish(root, current, ledger=True)
        except (WorkflowError, OSError) as error:
            raise WorkflowError(f"{error}. Check log preserved at {log_relative}.") from error
        print(f"{receipt['id']}: {args.check} {result}; ledger revision {current['revision']}; log {log_relative}")
        return 0 if result == "passed" else (130 if result == "interrupted" else 124 if result == "timeout" else 1)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1],
                        help="repository root (defaults to the tool's own checkout)")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "next"):
        command = commands.add_parser(name, help=f"read-only {name} view")
        command.add_argument("--json", action="store_true", help="print structured JSON")
    commands.add_parser("check", help="validate state, source freshness, and generated views without writing")
    commands.add_parser("sync", help="repair only generated views; preserve ledger bytes and revision")
    commands.add_parser("lock-info", help="inspect local locks; never remove or steal them")
    descriptions = {
        "claim": "claim planned work with a nonconflicting write scope",
        "checkpoint": "preserve work and refresh the recorded source fingerprint",
        "block": "record an explicit blocker while retaining ownership",
        "resume": "resume blocked work or return review to implementation",
        "handoff": "preserve context and release unfinished ownership",
        "submit": "submit current verified work for review",
        "accept": "record explicit coordinator review and completion",
        "cancel": "preserve context and cancel unfinished work",
        "add": "add one planned task from an exact JSON definition",
        "amend": "refine a planned or unowned blocked task definition",
        "focus": "select an existing milestone without declaring it accepted",
        "reconcile": "explicitly record the actual source fingerprint",
        "run-check": "execute an allowlisted required check and append its actual result",
    }
    task_commands = {"claim", "checkpoint", "block", "resume", "handoff", "submit",
                     "accept", "cancel", "amend", "run-check"}
    summary_commands = {"checkpoint", "resume", "handoff", "submit", "accept", "cancel",
                        "amend", "focus", "reconcile"}
    for name, help_text in descriptions.items():
        command = commands.add_parser(name, help=help_text)
        if name in task_commands:
            command.add_argument("task", help="existing task ID")
        if name == "focus":
            command.add_argument("milestone", help="existing milestone ID")
        if name in summary_commands:
            command.add_argument("--summary", required=True, help="factual operation summary")
        if name in {"checkpoint", "block", "handoff", "cancel"}:
            command.add_argument("--next-action", required=True, help="concrete continuation or recovery step")
        if name == "checkpoint":
            command.add_argument("--file", action="append", default=[], help="preserved repo-relative file; repeatable")
        if name == "block":
            command.add_argument("--reason", required=True, help="concrete blocking condition")
        if name in {"add", "amend"}:
            command.add_argument("--spec", required=True, help="JSON definition; relative paths use caller directory")
        if name == "accept":
            command.add_argument("--self-review", action="store_true", help="explicitly record coordinator self review")
        if name == "run-check":
            command.add_argument("--check", required=True, choices=sorted(_CHECKS))
        command.add_argument("--actor", required=True, help="cooperative actor identity, not authentication")
        command.add_argument("--expect-revision", required=True, type=int, help="revision last read by the actor")
    return parser


def _safe_streams() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(errors="backslashreplace")
            except (OSError, ValueError):
                pass


def main(argv: list[str] | None = None) -> int:
    """Run one explicit command; return a process exit status."""
    _safe_streams()
    parser = _parser()
    try:
        args = parser.parse_args(argv)
        root = _root(args.root)
        if args.command == "lock-info":
            print(json.dumps(_lock_info(root), ensure_ascii=False, indent=2))
            print("Recovery: inspect the recorded owner and preserve work; stop the original process "
                  "before manually removing only its exact lock path. Never infer abandonment from age or PID.")
            return 0
        if args.command == "sync":
            with _lock(root, _STATE_LOCK, "sync"):
                state = load_state(root)
                _publish(root, state, ledger=False)
            print(f"Synchronized both generated views at ledger revision {state['revision']}; ledger unchanged.")
            return 0
        if args.command in {"status", "next", "check"}:
            state = load_state(root)
            if args.command == "next":
                ready = [_summary_task(task) for task in _ready(state)]
                if args.json:
                    print(json.dumps(ready, ensure_ascii=False, indent=2))
                else:
                    print("\n".join(f"{task['id']} [priority {task['priority']}]: {task['title']}"
                                    for task in ready) or "No eligible planned tasks.")
                return 0
            actual = snapshot_source(root)
            fresh = actual == state["source_snapshot"]
            if args.command == "status":
                result = {
                    "revision": state["revision"], "updated_at": state["updated_at"],
                    "current_milestone": state["project"]["current_milestone"],
                    "source_current": fresh, "recorded_source": state["source_snapshot"],
                    "actual_source": actual, "ready_tasks": [_summary_task(task) for task in _ready(state)],
                    "tasks": [_summary_task(task) for task in state["tasks"]],
                }
                if args.json:
                    print(json.dumps(result, ensure_ascii=False, indent=2))
                else:
                    print(f"Revision {state['revision']}; milestone {state['project']['current_milestone']}; "
                          f"source {'current' if fresh else 'STALE'}.")
                    for task in state["tasks"]:
                        print(f"{task['id']}: {task['status']}; owner={task['owner'] or 'unowned'}"
                              + (f"; blocker={task['blocker']}" if task["blocker"] else ""))
                    print("Ready tasks: " + (", ".join(task["id"] for task in _ready(state)) or "none"))
                return 0
            problems = []
            if not fresh:
                problems.append("source fingerprint is stale; checkpoint or coordinator reconcile is required")
            for relative, expected in render_documents(state).items():
                try:
                    path = _managed(root, relative, required=True)
                    actual_bytes = _read_regular(path, single_link=True)
                    if actual_bytes != expected.encode("utf-8"):
                        problems.append(f"{relative} differs from its generated view; run sync")
                except WorkflowError as error:
                    problems.append(str(error))
            _require(not problems, "\n".join(problems))
            print(f"Agent workflow valid: revision {state['revision']}, {actual['file_count']} source files, "
                  "source fingerprint current, generated views current.")
            return 0
        if args.command == "run-check":
            return _run_check(root, args)
        return _mutation(root, args)
    except (WorkflowError, OSError) as error:
        print(f"agent-state: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("agent-state: interrupted; inspect lock-info and preserve work before retrying.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
