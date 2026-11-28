#!/usr/bin/env python3
"""Record the observed build settings, source revision and artifact content.

The build supplies CMake's project/toolchain metadata after compiling the host
and cooking the fixture. Only the Python standard library and optional local Git
are used. Hashes identify these files; they do not authenticate their producer or
promise that another compilation produces identical binaries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from typing import Any


MANIFEST_SCHEMA_VERSION = 1
# Version 1 retains the headless/fixture records. Expanded builds add these
# records without changing the existing record shape or foundation-only CLI.
SDK_ARTIFACT_ARGUMENTS = {
    "runtime": "runtime",
    "engine": "engine_library",
    "core": "core_library",
    "content": "content_library",
    "world": "world_library",
}
PACKAGE_HEADER = Path("engine/content/include/ge/content/package.hpp")
STATE_HEADER = Path("engine/world/include/ge/world/state.hpp")


def _metadata(value: str, label: str, *, empty: str | None = None) -> str:
    if not value and empty is not None:
        return empty
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+ -]{0,127}", value):
        raise ValueError(f"{label} must be a short name or version, without paths or control characters")
    return value


def _format_version(path: Path, symbol: str) -> int:
    # Read the native format declaration instead of maintaining another version
    # constant. Fail if its declaration changes so the producer is updated too.
    declaration = re.findall(
        rf"^\s*inline\s+constexpr\s+std::uint32_t\s+{symbol}\s*=\s*([0-9]+)[uU]?\s*;\s*$",
        path.read_text(encoding="utf-8"), re.MULTILINE,
    )
    if len(declaration) != 1 or not 1 <= int(declaration[0]) <= 0xFFFFFFFF:
        raise ValueError(f"cannot determine {symbol} from its native declaration")
    return int(declaration[0])


def _source_provenance(source: Path) -> dict[str, Any]:
    unknown = {"status": "unknown", "git_commit": None, "dirty": None}
    # An archive inside another checkout must not inherit that checkout's HEAD.
    marker = source / ".git"
    if not marker.exists() and not marker.is_symlink():
        return {**unknown, "reason": "git_metadata_absent"}
    git = shutil.which("git")
    if git is None:
        return {**unknown, "reason": "git_unavailable"}

    # Inherited Git overrides must not select another repository/index, execute
    # an fsmonitor hook, or cause a partial clone to fetch missing objects.
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("GIT_")}
    environment.update(GIT_OPTIONAL_LOCKS="0", GIT_NO_LAZY_FETCH="1", GIT_TERMINAL_PROMPT="0")
    command = [git, "--no-optional-locks", "-c", "core.fsmonitor=false",
               "-C", str(source)]

    def query(arguments: list[str]) -> bytes:
        try:
            result = subprocess.run([*command, *arguments], check=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    env=environment, timeout=30)
        except (OSError, subprocess.SubprocessError) as error:
            # Do not include Git's stderr/configuration in distributed metadata.
            raise ValueError("cannot read local Git provenance") from error
        return result.stdout

    commit = query(["rev-parse", "--verify", "HEAD"]).strip()
    if re.fullmatch(rb"[0-9a-f]{40}|[0-9a-f]{64}", commit) is None:
        raise ValueError("local Git HEAD is not a complete commit identifier")
    dirty = bool(query(["status", "--porcelain=v1", "--untracked-files=normal",
                        "--ignore-submodules=none"]))
    return {"status": "git", "git_commit": commit.decode("ascii"), "dirty": dirty}


def _artifact(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise ValueError(f"artifact is not a regular file: {path}")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError(f"artifact is not a regular file: {path}")
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
            size += len(block)
        after = os.fstat(stream.fileno())
    if (size != before.st_size or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns
            or before.st_ctime_ns != after.st_ctime_ns):
        raise ValueError(f"artifact changed while it was being hashed: {path}")
    return {"filename": path.name, "sha256": digest.hexdigest(), "size_bytes": size}


def _check_destination(output: Path, inputs: tuple[Path, ...]) -> None:
    destination = output.resolve()
    for path in inputs:
        if not path.is_file():
            raise ValueError(f"input is not a regular file: {path}")
        if (destination == path.resolve(strict=True)
                or (output.exists() and output.samefile(path))):
            raise ValueError("manifest output must not overwrite an input file")
    if output.is_symlink() or (output.exists() and not output.is_file()):
        raise ValueError("manifest output must be a regular file, not a link or directory")


def write_manifest(arguments: argparse.Namespace) -> dict[str, Any]:
    source = arguments.source_dir.resolve(strict=True)
    if not source.is_dir():
        raise ValueError("source directory must be a directory")
    output = arguments.output
    artifacts = {"headless": arguments.binary, "fixture": arguments.fixture}
    sdk_artifacts = {name: getattr(arguments, option) for name, option in SDK_ARTIFACT_ARGUMENTS.items()}
    if any(path is not None for path in sdk_artifacts.values()):
        if any(path is None for path in sdk_artifacts.values()):
            raise ValueError("runtime and all four library paths must be supplied together")
        artifacts.update(sdk_artifacts)
    inputs = (*artifacts.values(), source / PACKAGE_HEADER, source / STATE_HEADER)
    _check_destination(output, inputs)

    package_version = _format_version(source / PACKAGE_HEADER, "package_schema_version")
    state_version = _format_version(source / STATE_HEADER, "state_schema_version")
    with arguments.fixture.open("rb") as fixture:
        header = fixture.read(12)
    if len(header) != 12 or struct.unpack("<8sI", header) != (b"GEPACK01", package_version):
        raise ValueError("fixture header does not match the native package format version")

    manifest = {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "project": {
            "name": _metadata(arguments.project_name, "project name"),
            "version": _metadata(arguments.project_version, "project version"),
        },
        "source": _source_provenance(source),
        "build": {
            "platform": _metadata(arguments.platform, "platform", empty="unknown"),
            "system_processor": _metadata(arguments.system_processor, "system processor", empty="unknown"),
            "compiler": {
                "id": _metadata(arguments.compiler_id, "compiler ID", empty="unknown"),
                "version": _metadata(arguments.compiler_version, "compiler version", empty="unknown"),
            },
            "configuration": _metadata(arguments.configuration, "configuration", empty="unspecified"),
            "strict_warnings": arguments.strict_warnings,
            "sanitizers": arguments.sanitizers,
        },
        "format_versions": {"package": package_version, "state": state_version},
        "artifacts": {name: _artifact(path) for name, path in artifacts.items()},
    }
    content = (json.dumps(manifest, sort_keys=True, indent=2, ensure_ascii=True,
                          allow_nan=False) + "\n").encode("utf-8")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", dir=output.parent,
                                         prefix=".ge-build-manifest-", suffix=".tmp",
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        _check_destination(output, inputs)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return manifest


def _boolean(value: str) -> bool:
    if value not in ("0", "1"):
        raise argparse.ArgumentTypeError("expected 0 or 1")
    return value == "1"


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-dir", "binary", "fixture", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    for name in SDK_ARTIFACT_ARGUMENTS.values():
        parser.add_argument(f"--{name.replace('_', '-')}", type=Path,
                            help="expanded SDK artifact; supply runtime and all four libraries together")
    for name in ("project-name", "project-version", "platform", "system-processor",
                 "compiler-id", "compiler-version", "configuration"):
        parser.add_argument(f"--{name}", required=True)
    for name in ("strict-warnings", "sanitizers"):
        parser.add_argument(f"--{name}", type=_boolean, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    try:
        write_manifest(arguments)
    except (OSError, ValueError) as error:
        print(f"build-manifest: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
