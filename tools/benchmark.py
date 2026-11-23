#!/usr/bin/env python3
"""Measure complete headless process durations; Python 3.11+, stdlib only."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
TIMEOUT_SECONDS = 30
MAX_STDOUT = 32 * 1024 * 1024


def run_count(value: str) -> int:
    try:
        count = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("runs must be an integer from 1 through 1000") from error
    if not 1 <= count <= 1000:
        raise argparse.ArgumentTypeError("runs must be from 1 through 1000")
    return count


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_git() -> dict:
    result = {"revision": None, "dirty": None}
    # An archive inside another checkout must not inherit that checkout's HEAD.
    marker = ROOT / ".git"
    if not marker.exists() and not marker.is_symlink():
        return {**result, "unavailable_reason": "source Git metadata is absent"}
    # Query only this source checkout, without inherited repository/index/config
    # overrides, optional index writes, fsmonitor hooks, or partial-clone fetches.
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("GIT_")}
    environment.update(GIT_OPTIONAL_LOCKS="0", GIT_NO_LAZY_FETCH="1", GIT_TERMINAL_PROMPT="0")
    try:
        def git(*arguments: str) -> bytes:
            return subprocess.run(["git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                                   "-C", str(ROOT), *arguments], check=True,
                                  capture_output=True, env=environment, timeout=10).stdout.strip()
        result["revision"] = git("rev-parse", "--verify", "HEAD").decode("ascii")
        result["dirty"] = bool(git("status", "--porcelain=v1", "--untracked-files=normal",
                                   "--ignore-submodules=none"))
    except (OSError, UnicodeError, subprocess.SubprocessError) as error:
        result["unavailable_reason"] = str(error)
    return result


def check_output(output: Path, inputs: tuple[Path, Path]) -> None:
    if output.is_symlink() or (output.exists() and not output.is_file()):
        raise ValueError("output must be a regular file path, not a symlink or directory")
    for source in inputs:
        if output.resolve() == source or (output.exists() and output.samefile(source)):
            raise ValueError("output must not alias the binary or package")
    if not output.parent.is_dir():
        raise ValueError("output parent directory must already exist")


def invalid_constant(value: str) -> None:
    raise ValueError(f"non-JSON constant: {value}")


def validate(raw: bytes, mode: str) -> dict:
    if len(raw) > MAX_STDOUT:
        raise ValueError("stdout exceeds the 32 MiB validation limit")
    payload = json.loads(raw.decode("utf-8"), parse_constant=invalid_constant)
    expected = "inspect" if mode == "inspect" else "headless-policy-fixture"
    if not isinstance(payload, dict) or payload.get("mode") != expected:
        raise ValueError(f"expected a JSON object with mode {expected!r}")
    if type(payload.get("schema_version")) is not int or payload["schema_version"] != 1:
        raise ValueError("expected package schema_version 1")
    if mode == "inspect":
        if "status" in payload and payload["status"] != "ok":
            raise ValueError("inspection reports a non-ok status")
        if not isinstance(payload.get("regions"), list):
            raise ValueError("inspection is missing its regions array")
    else:
        checks = payload.get("checks")
        if payload.get("status") != "ok":
            raise ValueError("lifecycle must report status 'ok'")
        if not isinstance(checks, dict) or not checks or any(v is not True for v in checks.values()):
            raise ValueError("lifecycle checks must be present and all true")
    return {key: payload.get(key) for key in (
        "mode", "status", "schema_version", "region_count", "entity_count", "source_sha256")}


def measure(command: list[str], cwd: str, mode: str, index: int) -> dict:
    record = {"run": index, "duration_ns": None, "exit_code": None, "status": "failed"}
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        started = time.perf_counter_ns()
        try:
            process = subprocess.run(command, cwd=cwd, stdout=stdout, stderr=stderr,
                                     timeout=TIMEOUT_SECONDS, check=False)
            record["exit_code"] = process.returncode
        except (OSError, subprocess.SubprocessError) as error:
            record["error"] = str(error)
        record["duration_ns"] = time.perf_counter_ns() - started
        stdout.seek(0)
        raw = stdout.read(MAX_STDOUT + 1)
        stderr.seek(0)
        diagnostic = stderr.read(4097)
    try:
        if record["exit_code"] != 0 or "error" in record:
            raise ValueError(record.get("error", f"process exited {record['exit_code']}"))
        if diagnostic:
            raise ValueError("successful command unexpectedly wrote stderr")
        record["result"] = validate(raw, mode)
        record["status"] = "ok"
    except (ValueError, RecursionError) as error:
        record["error"] = str(error)
        record["stdout_excerpt"] = raw[:4096].decode("utf-8", errors="replace")
        record["stderr_excerpt"] = diagnostic[:4096].decode("utf-8", errors="replace")
    return record


def publish(output: Path, report: dict, inputs: tuple[Path, Path]) -> None:
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent,
                                         prefix=".ge-measure-", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write("\n")
        check_output(output, inputs)
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    # Keep human diagnostics printable in redirected legacy-encoded logs. JSON
    # remains ASCII-escaped, and subprocess arguments retain their Unicode paths.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--runs", type=run_count, default=5)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("inspect", "lifecycle"), default="inspect")
    options = parser.parse_args()
    try:
        binary, package = (path.expanduser().resolve(strict=True)
                           for path in (options.binary, options.package))
        if not binary.is_file() or not package.is_file():
            raise ValueError("binary and package must be regular files")
        output = options.output.expanduser().absolute()
        check_output(output, (binary, package))
        hashes = [sha256(binary), sha256(package)]
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    command = [str(binary), "--package", str(package)] + (["--inspect"] if options.mode == "inspect" else [])
    report = {"report_version": 1, "status": "failed", "mode": options.mode,
              "started_utc": datetime.now(timezone.utc).isoformat(), "command": command,
              "requested_runs": options.runs, "timeout_seconds": TIMEOUT_SECONDS,
              "measurement": "whole process wall clock; perf_counter_ns; OS cache uncontrolled",
              "platform": platform.platform(), "machine": platform.machine(),
              "python": sys.version, "source_git_at_start": source_git(),
              "binary": {"path": str(binary), "sha256": hashes[0]},
              "package": {"path": str(package), "sha256": hashes[1]},
              "runs": [], "summary": None}
    try:
        with tempfile.TemporaryDirectory(prefix="ge-measure-") as cwd:
            for index in range(1, options.runs + 1):
                report["runs"].append(measure(command, cwd, options.mode, index))
        if hashes != [sha256(binary), sha256(package)]:
            raise ValueError("binary or package changed during measurement")
        if any(run["status"] != "ok" for run in report["runs"]):
            raise ValueError("one or more runs failed validation")
        durations = [run["duration_ns"] for run in report["runs"]]
        report["summary"] = {"count": len(durations), "min_ns": min(durations),
                             "median_ns": statistics.median(durations), "max_ns": max(durations)}
        report["status"] = "ok"
        publish(output, report, (binary, package))
    except (OSError, ValueError, RuntimeError) as error:
        report.update(status="failed", summary=None, error=str(error))
        print(json.dumps(report, indent=2, allow_nan=False))
        print("benchmark: output preserved; failed report emitted to stdout", file=sys.stderr)
        return 1
    print(f"Wrote {len(durations)} validated process measurements to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
