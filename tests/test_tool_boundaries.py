#!/usr/bin/env python3
"""Check tool provenance, redirected diagnostics, and report publication.

Usage: python3 tests/test_tool_boundaries.py --binary PATH --fixture PATH [-v]
Remaining arguments are passed to unittest. Native checks reuse the supplied
headless executable and district fixture; no build or timing threshold is used.
"""

from __future__ import annotations

import argparse
import importlib.util
import io
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
BINARY: Path | None = None
FIXTURE: Path | None = None


def load_tool(name: str):
    path = REPOSITORY / "tools" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"ge_{name}_boundary_tests", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load tool: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


benchmark = load_tool("benchmark")
driver = load_tool("ge")


class ToolBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if BINARY is None or FIXTURE is None:
            raise RuntimeError("run this suite with --binary PATH --fixture PATH")
        if not BINARY.is_file() or not FIXTURE.is_file():
            raise RuntimeError(f"binary and fixture must exist: {BINARY}, {FIXTURE}")
        cls.fixture_bytes = FIXTURE.read_bytes()

    def setUp(self) -> None:
        # An archive here is deliberately inside the containing checkout.
        products = REPOSITORY / "out"
        products.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="ge-tool-boundaries-", dir=products)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.unicode = self.root / "\u8def\u5f84\U0001f697"
        self.unicode.mkdir()
        self.environment = dict(os.environ, PYTHONIOENCODING="cp1252:strict")

    def invoke(self, tool: str, *arguments: str | Path) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [sys.executable, str(REPOSITORY / "tools" / f"{tool}.py"),
             *(str(argument) for argument in arguments)],
            cwd=self.root, env=self.environment, capture_output=True, timeout=45,
        )

    def test_archive_does_not_inherit_containing_checkout_or_git_overrides(self) -> None:
        archive = self.root / "source archive"
        archive.mkdir()
        overrides = {"GIT_DIR": str(REPOSITORY / ".git"), "GIT_WORK_TREE": str(REPOSITORY)}
        for environment in ({}, overrides):
            with self.subTest(overrides=bool(environment)), \
                    mock.patch.object(benchmark, "ROOT", archive), \
                    mock.patch.dict(os.environ, environment):
                provenance = benchmark.source_git()
                self.assertIsNone(provenance["revision"])
                self.assertIsNone(provenance["dirty"])
                self.assertIn("unavailable_reason", provenance)

    def test_checkout_ignores_inherited_git_overrides(self) -> None:
        git = shutil.which("git")
        if git is None or not (REPOSITORY / ".git").exists():
            self.skipTest("local Git checkout unavailable; archive behavior is still checked")
        environment = {key: value for key, value in os.environ.items()
                       if not key.upper().startswith("GIT_")}
        expected = subprocess.run(
            [git, "-C", str(REPOSITORY), "rev-parse", "--verify", "HEAD"],
            capture_output=True, check=True, env=environment, timeout=10,
        ).stdout.decode("ascii").strip()
        overrides = {
            "GIT_DIR": str(self.root / "not-a-repository"),
            "GIT_WORK_TREE": str(self.root),
            "GIT_INDEX_FILE": str(self.root / "missing" / "index"),
        }
        with mock.patch.dict(os.environ, overrides):
            provenance = benchmark.source_git()
        self.assertEqual(provenance["revision"], expected)
        self.assertIs(type(provenance["dirty"]), bool)
        self.assertNotIn("unavailable_reason", provenance)

    def test_driver_escapes_legacy_stdout_before_tool_discovery(self) -> None:
        output = io.BytesIO()
        diagnostics = io.BytesIO()
        with io.TextIOWrapper(output, encoding="cp1252", errors="strict") as stdout, \
                io.TextIOWrapper(diagnostics, encoding="cp1252", errors="strict") as stderr:
            with mock.patch.object(sys, "stdout", stdout), \
                    mock.patch.object(sys, "stderr", stderr), \
                    mock.patch.object(sys, "executable", str(self.unicode / "python")), \
                    mock.patch.object(driver, "_program", side_effect=driver.DriverError(
                        "deliberately unavailable for this diagnostic check")) as discover:
                status = driver.main(["doctor"])
            self.assertEqual(status, 1)
            self.assertEqual(discover.call_count, 4)
            stdout.flush()
            self.assertIn(b"\\u8def\\u5f84\\U0001f697", output.getvalue())
            self.assertNotIn(b"Traceback", diagnostics.getvalue())

    def test_driver_cook_preserves_unicode_arguments_with_legacy_logs(self) -> None:
        source = self.unicode / "district \u96ea.json"
        source.write_bytes((REPOSITORY / "assets/fixtures/district.world.json").read_bytes())
        output = self.unicode / "cooked \u96ea.gepack"
        result = self.invoke("ge", "cook", source, "--output", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), self.fixture_bytes)
        self.assertEqual(json.loads(result.stdout)["schema_version"], 1)
        self.assertIn(b"\\u8def\\u5f84\\U0001f697", result.stderr)
        self.assertNotIn(b"Traceback", result.stderr)

    def test_driver_reports_invalid_cache_encoding_without_a_traceback(self) -> None:
        cache = self.root / "out" / "build" / "dev" / "CMakeCache.txt"
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b"CMAKE_HOME_DIRECTORY:INTERNAL=\xff\n")
        diagnostics = io.StringIO()
        with mock.patch.object(driver, "ROOT", self.root), \
                mock.patch.object(sys, "stderr", diagnostics):
            status = driver.main(["test"])
        self.assertEqual(status, 1)
        self.assertIn("ge: error:", diagnostics.getvalue())
        self.assertIn("decode", diagnostics.getvalue())
        self.assertNotIn("Traceback", diagnostics.getvalue())

    def test_benchmark_overflow_retains_a_serializable_failure_report(self) -> None:
        binary = self.root / "binary"
        binary.write_bytes(b"mock executable")
        package = self.root / "district.gepack"
        package.write_bytes(self.fixture_bytes)
        output = self.root / "measurements.json"
        previous = b"last validated report"
        output.write_bytes(previous)
        arguments = ["benchmark.py", "--binary", str(binary), "--package", str(package),
                     "--runs", "1", "--output", str(output)]
        for number in ("1e999", "-1e999"):
            raw = (f'{{"mode":"inspect","schema_version":1,"regions":[],\n'
                   f'"region_count":{number}}}').encode("ascii")

            def emit_output(command: list[str], **kwargs) -> subprocess.CompletedProcess:
                kwargs["stdout"].write(raw)
                return subprocess.CompletedProcess(command, 0)

            stdout, stderr = io.StringIO(), io.StringIO()
            with self.subTest(number=number), \
                    mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(sys, "stdout", stdout), \
                    mock.patch.object(sys, "stderr", stderr), \
                    mock.patch.object(benchmark.platform, "platform", return_value="test host"), \
                    mock.patch.object(benchmark, "source_git", return_value={}), \
                    mock.patch.object(benchmark.subprocess, "run", side_effect=emit_output):
                self.assertEqual(benchmark.main(), 1)
                report = json.loads(stdout.getvalue())
                self.assertEqual(report["status"], "failed")
                self.assertIsNone(report["summary"])
                self.assertEqual(report["runs"][0]["status"], "failed")
                self.assertIn("finite", report["runs"][0]["error"])
                self.assertIn(number, report["runs"][0]["stdout_excerpt"])
                self.assertEqual(output.read_bytes(), previous)
                self.assertIn("output preserved", stderr.getvalue())
                self.assertEqual(list(self.root.glob(".ge-measure-*")), [])

    def test_benchmark_unicode_success_and_failure_preserve_publication(self) -> None:
        package = self.unicode / "district \u96ea.gepack"
        package.write_bytes(self.fixture_bytes)
        output = self.unicode / "inspect \u96ea.json"
        arguments = ["--binary", BINARY, "--package", package, "--runs", "1",
                     "--mode", "inspect", "--output", output]
        result = self.invoke("benchmark", *arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b"")
        self.assertIn(b"\\u8def\\u5f84\\U0001f697", result.stdout)
        previous = output.read_bytes()
        report = json.loads(previous)
        self.assertEqual(report["status"], "ok")
        self.assertEqual(report["summary"]["count"], 1)
        self.assertEqual(report["runs"][0]["result"]["mode"], "inspect")
        self.assertIn(str(package), report["command"])

        package.write_bytes(b"invalid package")
        failed = self.invoke("benchmark", *arguments)
        self.assertEqual(failed.returncode, 1, failed.stderr)
        self.assertEqual(output.read_bytes(), previous)
        failure = json.loads(failed.stdout)
        self.assertEqual(failure["status"], "failed")
        self.assertIsNone(failure["summary"])
        self.assertEqual(failure["runs"][0]["status"], "failed")
        self.assertNotEqual(failure["runs"][0]["exit_code"], 0)
        self.assertIn(b"output preserved", failed.stderr)
        self.assertNotIn(b"Traceback", failed.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    BINARY = options.binary.resolve(strict=True)
    FIXTURE = options.fixture.resolve(strict=True)
    unittest.main(argv=[sys.argv[0], *remaining])
