#!/usr/bin/env python3
"""Check published provenance/content and failure preservation without a compiler.

Run with ``python3 tests/test_build_manifest.py [-v]``. Local temporary Git
repositories exercise real clean/dirty behavior when Git is installed.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[1]
TOOL = REPOSITORY / "tools/build_manifest.py"
GIT = shutil.which("git")
_SPEC = importlib.util.spec_from_file_location("ge_build_manifest_under_test", TOOL)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load build manifest producer: {TOOL}")
producer = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(producer)


class BuildManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with tempfile.TemporaryDirectory(prefix="ge-manifest-fixture-") as directory:
            output = Path(directory) / "district.gepack"
            subprocess.run([sys.executable, str(REPOSITORY / "tools/cook.py"),
                            str(REPOSITORY / "assets/fixtures/district.world.json"),
                            "--output", str(output)], check=True, capture_output=True)
            cls.fixture_bytes = output.read_bytes()

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ge-manifest-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source archive with spaces"
        self.source.mkdir()
        for header in ("engine/content/include/ge/content/package.hpp",
                       "engine/world/include/ge/world/state.hpp"):
            target = self.source / header
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPOSITORY / header, target)
        self.binary = self.root / "ge_headless"
        self.binary.write_bytes(b"fixture host bytes\0\x01\x02\xff")
        self.fixture = self.root / "district.gepack"
        self.fixture.write_bytes(self.fixture_bytes)
        self.output = self.root / "build-manifest.json"

    def arguments(self, **changes: object) -> list[str]:
        values = {
            "source-dir": self.source, "binary": self.binary,
            "fixture": self.fixture, "output": self.output,
            "project-name": "GameEngine", "project-version": "7.8.9",
            "platform": "TestOS", "system-processor": "test64",
            "compiler-id": "TestCompiler", "compiler-version": "12.3.4",
            "configuration": "Release", "strict-warnings": "1", "sanitizers": "0",
        }
        values.update({key.replace("_", "-"): value for key, value in changes.items()})
        return [argument for name, value in values.items() for argument in (f"--{name}", str(value))]

    def run_tool(self, *, environment: dict[str, str] | None = None,
                 **changes: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(TOOL), *self.arguments(**changes)],
                              capture_output=True, text=True, env=environment, timeout=60)

    def generate(self, **changes: object) -> dict:
        result = self.run_tool(**changes)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(Path(changes.get("output", self.output)).read_text(encoding="utf-8"))

    def git(self, *arguments: str, directory: Path | None = None) -> str:
        if GIT is None:
            self.skipTest("Git is unavailable; archive provenance is still checked")
        environment = {key: value for key, value in os.environ.items()
                       if not key.upper().startswith("GIT_")}
        result = subprocess.run(
            [GIT, "-c", "core.fsmonitor=false", "-c", "commit.gpgsign=false",
             "-c", "user.name=Manifest Test", "-c", "user.email=manifest@example.invalid",
             "-C", str(directory or self.source), *arguments],
            capture_output=True, text=True, check=True, env=environment, timeout=30,
        )
        return result.stdout.strip()

    def commit_source(self) -> str:
        self.git("init", "--quiet")
        (self.source / ".gitignore").write_text("out/\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", "Local provenance fixture")
        return self.git("rev-parse", "HEAD")

    def assert_previous_survives(self, **changes: object) -> None:
        previous = self.output.read_bytes()
        result = self.run_tool(**changes)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build-manifest:", result.stderr)
        self.assertEqual(self.output.read_bytes(), previous)
        self.assertEqual(list(self.root.rglob(".ge-build-manifest-*.tmp")), [])

    def test_hashes_sizes_versions_and_typed_build_settings_describe_inputs(self) -> None:
        manifest = self.generate()
        self.assertEqual(manifest["project"], {"name": "GameEngine", "version": "7.8.9"})
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["format_versions"], {"package": 1, "state": 1})
        for key, path in (("headless", self.binary), ("fixture", self.fixture)):
            record = manifest["artifacts"][key]
            self.assertEqual(record["filename"], path.name)
            self.assertEqual(record["size_bytes"], len(path.read_bytes()))
            self.assertEqual(record["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(manifest["build"]["configuration"], "Release")
        self.assertIs(manifest["build"]["strict_warnings"], True)
        self.assertIs(manifest["build"]["sanitizers"], False)
        self.assertEqual(manifest["build"]["compiler"], {"id": "TestCompiler", "version": "12.3.4"})
        changed = self.generate(configuration="Debug", strict_warnings="0", sanitizers="1")
        self.assertIs(changed["build"]["strict_warnings"], False)
        self.assertIs(changed["build"]["sanitizers"], True)
        self.assertEqual(changed["build"]["configuration"], "Debug")
        self.assertEqual(self.generate(configuration="")["build"]["configuration"], "unspecified")

    def test_identical_content_is_stable_across_paths_mtimes_and_repeated_runs(self) -> None:
        self.generate()
        original = self.output.read_bytes()
        os.utime(self.binary, (1234567890, 1234567890))
        os.utime(self.fixture, (1234567000, 1234567000))
        destination = self.root / "another absolute location"
        destination.mkdir()
        shutil.copytree(self.source, destination / "source")
        shutil.copyfile(self.binary, destination / self.binary.name)
        shutil.copyfile(self.fixture, destination / self.fixture.name)
        other_output = destination / "nested" / "manifest.json"
        self.generate(source_dir=destination / "source", binary=destination / self.binary.name,
                      fixture=destination / self.fixture.name, output=other_output)
        self.assertEqual(other_output.read_bytes(), original)
        self.generate()
        self.assertEqual(self.output.read_bytes(), original)
        self.assertNotIn(str(self.root).encode(), original)

    def test_changed_artifact_is_rehashed_even_when_size_is_unchanged(self) -> None:
        before = self.generate()
        raw = bytearray(self.binary.read_bytes())
        raw[-1] ^= 1
        self.binary.write_bytes(raw)
        after = self.generate()
        self.assertNotEqual(before["artifacts"]["headless"]["sha256"],
                            after["artifacts"]["headless"]["sha256"])
        self.assertEqual(before["artifacts"]["headless"]["size_bytes"],
                         after["artifacts"]["headless"]["size_bytes"])
        self.assertEqual(before["artifacts"]["fixture"], after["artifacts"]["fixture"])

    def test_clean_dirty_untracked_and_ignored_git_sources(self) -> None:
        commit = self.commit_source()
        clean = self.generate()["source"]
        self.assertEqual(clean, {"status": "git", "git_commit": commit, "dirty": False})
        ignored = self.source / "out"
        ignored.mkdir()
        (ignored / "build-output").write_bytes(b"ignored product")
        self.assertEqual(self.generate()["source"], clean)
        untracked = self.source / "new-source.cpp"
        untracked.write_text("// source not committed\n", encoding="utf-8")
        self.assertIs(self.generate()["source"]["dirty"], True)
        untracked.unlink()
        with (self.source / ".gitignore").open("a", encoding="utf-8") as stream:
            stream.write("extra-ignored/\n")
        dirty = self.generate()["source"]
        self.assertEqual(dirty["git_commit"], commit)
        self.assertIs(dirty["dirty"], True)

    def test_archive_provenance_is_unknown_even_inside_another_repository(self) -> None:
        archive = self.generate()["source"]
        self.assertEqual(archive["status"], "unknown")
        self.assertIsNone(archive["git_commit"])
        self.assertIsNone(archive["dirty"])
        self.assertEqual(archive["reason"], "git_metadata_absent")
        self.git("init", "--quiet", directory=self.root)
        self.git("add", self.source.name, directory=self.root)
        self.git("commit", "--quiet", "-m", "Unrelated containing repository", directory=self.root)
        self.assertEqual(self.generate()["source"], archive)

    def test_missing_git_tool_has_explicit_unknown_provenance(self) -> None:
        (self.source / ".git").mkdir()
        environment = dict(os.environ, PATH=str(self.root / "no-programs"))
        result = self.run_tool(environment=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        source = json.loads(self.output.read_text(encoding="utf-8"))["source"]
        self.assertEqual(source, {"status": "unknown", "git_commit": None,
                                  "dirty": None, "reason": "git_unavailable"})

    def test_inherited_git_overrides_do_not_select_another_repository(self) -> None:
        commit = self.commit_source()
        environment = dict(os.environ, GIT_DIR=str(self.root / "wrong-repository"),
                           GIT_WORK_TREE=str(self.root), GIT_INDEX_FILE=str(self.root / "wrong-index"))
        result = self.run_tool(environment=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.output.read_text(encoding="utf-8"))["source"],
                         {"status": "git", "git_commit": commit, "dirty": False})

    def test_missing_invalid_inputs_and_broken_git_preserve_previous_manifest(self) -> None:
        self.generate()
        for changes in ({"binary": self.root / "missing-host"},
                        {"fixture": self.root / "missing-fixture"},
                        {"binary": self.root}, {"compiler_id": "/private/workspace/compiler"}):
            with self.subTest(changes=changes):
                self.assert_previous_survives(**changes)
        self.fixture.write_bytes(b"not a package")
        self.assert_previous_survives()
        self.fixture.write_bytes(self.fixture_bytes)
        state_header = self.source / "engine/world/include/ge/world/state.hpp"
        state_header.unlink()
        self.assert_previous_survives()
        shutil.copyfile(REPOSITORY / "engine/world/include/ge/world/state.hpp", state_header)
        if GIT is not None:
            (self.source / ".git").write_text("not valid git metadata\n", encoding="utf-8")
            self.assert_previous_survives()

    def test_input_paths_cannot_be_used_as_output(self) -> None:
        for path in (self.binary, self.fixture,
                     self.source / "engine/world/include/ge/world/state.hpp"):
            with self.subTest(path=path):
                before = path.read_bytes()
                result = self.run_tool(output=path)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("must not overwrite an input", result.stderr)
                self.assertEqual(path.read_bytes(), before)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "named pipes require POSIX")
    def test_nonregular_input_fails_without_waiting_for_a_writer(self) -> None:
        self.generate()
        previous = self.output.read_bytes()
        pipe = self.root / "input-pipe"
        os.mkfifo(pipe)
        for name in ("binary", "fixture"):
            with self.subTest(input=name):
                result = subprocess.run([sys.executable, str(TOOL), *self.arguments(**{name: pipe})],
                                        capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("not a regular file", result.stderr)
                self.assertEqual(self.output.read_bytes(), previous)

    def test_input_hard_link_alias_cannot_be_used_as_output(self) -> None:
        try:
            self.output.hardlink_to(self.binary)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create hard links: {error}")
        before = self.binary.read_bytes()
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.binary.read_bytes(), before)
        self.assertTrue(self.output.samefile(self.binary))

    def test_input_symbolic_link_alias_cannot_be_used_as_output(self) -> None:
        try:
            self.output.symlink_to(self.fixture)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create symbolic links: {error}")
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.fixture.read_bytes(), self.fixture_bytes)
        self.assertTrue(self.output.is_symlink())

    def test_publication_failures_leave_previous_manifest_and_no_temporary_file(self) -> None:
        self.generate()
        previous = self.output.read_bytes()
        arguments = producer.parse_arguments(self.arguments())
        self.binary.write_bytes(b"new host bytes to publish")
        for operation in ("fsync", "replace"):
            with self.subTest(operation=operation):
                with mock.patch.object(producer.os, operation, side_effect=OSError("injected I/O failure")):
                    with self.assertRaises(OSError):
                        producer.write_manifest(arguments)
                self.assertEqual(self.output.read_bytes(), previous)
                self.assertEqual(list(self.root.rglob(".ge-build-manifest-*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
