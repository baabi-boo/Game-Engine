#!/usr/bin/env python3
"""Exercise the composed runtime CLI in isolated, bounded subprocesses.

Usage: python3 tests/test_runtime_cli.py --binary PATH [-v]
The remaining arguments are passed to unittest.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


BINARY: Path | None = None


class RuntimeCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if BINARY is None or not BINARY.is_file():
            raise RuntimeError("run this suite with --binary PATH to the built ge_runtime")

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ge-runtime-cli-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def invoke(self, *arguments: str | Path, input_text: str | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(BINARY), *(str(argument) for argument in arguments)],
            cwd=self.root, input=input_text, capture_output=True, text=True,
            encoding="utf-8", timeout=15, check=False,
        )

    def read_report(self, path: Path) -> dict:
        self.assertTrue(path.is_file(), f"missing runtime report: {path}")
        payload = json.loads(path.read_text(encoding="utf-8"))
        self.assertIsInstance(payload, dict)
        return payload

    def assert_success(self, result: subprocess.CompletedProcess) -> None:
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    def assert_failed_report(self, result: subprocess.CompletedProcess, path: Path) -> dict:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertTrue(result.stderr, "failed commands must explain the failure")
        report = self.read_report(path)
        self.assertIs(report["success"], False)
        self.assertTrue(report["diagnostics"], "the failure must be visible in the report")
        return report

    def test_help_and_command_discovery(self) -> None:
        result = self.invoke("--help")
        self.assert_success(result)
        for option in ("--ticks", "--step", "--script", "--set", "--report", "--interactive"):
            self.assertIn(option, result.stdout)
        result = self.invoke("--commands")
        self.assert_success(result)
        commands = [line.partition("\t")[0] for line in result.stdout.splitlines()]
        self.assertEqual(len(commands), len(set(commands)))
        self.assertTrue({"engine.stop", "engine.set", "engine.report", "content.hash", "tick"}
                        .issubset(commands))

    def test_ticks_produce_composed_report(self) -> None:
        path = self.root / "report.json"
        result = self.invoke("--ticks", "3", "--step", "0.125", "--report", path)
        self.assert_success(result)
        self.assertEqual(result.stdout, "")
        report = self.read_report(path)
        self.assertIs(report["success"], True)
        self.assertEqual(report["tick"], 3)
        self.assertAlmostEqual(report["elapsed_seconds"], 0.375)
        self.assertTrue(report["modules"])
        self.assertGreater(report["metrics"]["kernel.services"], 2)
        systems = {system["name"]: system for system in report["systems"]}
        self.assertEqual(systems["core.scratch"]["calls"], 3)
        self.assertTrue(all(system["failures"] == 0 for system in systems.values()))

    def test_default_step_and_stdout_report(self) -> None:
        result = self.invoke("--ticks", "2")
        self.assert_success(result)
        report = json.loads(result.stdout)
        self.assertIs(report["success"], True)
        self.assertEqual(report["tick"], 2)
        self.assertAlmostEqual(report["elapsed_seconds"], 2.0 / 60.0)

    def test_script_crlf_whitespace_and_unicode_paths(self) -> None:
        directory = self.root / "\u0394\u4e16\u754c runtime"
        directory.mkdir()
        script = directory / "\u547d\u4ee4.txt"
        report_path = directory / "\u03b1\u62a5\u544a.json"
        script.write_bytes(b" # comment\r\n\t tick \r\n \t\r\n tick\t# comment\r\n\t tick")
        result = self.invoke("--ticks", "0", "--step", "0.125", "--script", script,
                             "--report", report_path)
        self.assert_success(result)
        report = self.read_report(report_path)
        self.assertIs(report["success"], True)
        self.assertEqual(report["tick"], 3)
        self.assertAlmostEqual(report["elapsed_seconds"], 0.375)

    def test_invalid_script_commands_are_reported(self) -> None:
        script = self.root / "commands.txt"
        report_path = self.root / "report.json"
        for command in ("not.a.command", "tick 2", "content.hash", 'tick "'):
            with self.subTest(command=command):
                script.write_text(command + "\n", encoding="utf-8")
                result = self.invoke("--ticks", "0", "--script", script, "--report", report_path)
                report = self.assert_failed_report(result, report_path)
                self.assertEqual(report["tick"], 0)

    def test_command_failure_survives_later_successful_ticks(self) -> None:
        script = self.root / "commands.txt"
        script.write_text("not.a.command\ntick\n", encoding="utf-8")
        report_path = self.root / "report.json"
        result = self.invoke("--ticks", "1", "--script", script, "--report", report_path)
        report = self.assert_failed_report(result, report_path)
        self.assertEqual(report["tick"], 2)
        self.assertTrue(any("unknown command" in item["message"] for item in report["diagnostics"]))

    def test_invalid_steps_and_overrides_preserve_existing_report(self) -> None:
        report_path = self.root / "report.json"
        previous = b"previous valid report\n"
        for option in ("--step", "--set"):
            for value in ("", "0", "-0.1", "1.01", "nan", "inf", "nonsense", "1e999"):
                with self.subTest(option=option, value=value):
                    report_path.write_bytes(previous)
                    argument = f"runtime.step={value}" if option == "--set" else value
                    result = self.invoke("--ticks", "0", option, argument, "--report", report_path)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, "")
                    self.assertIn("step", result.stderr)
                    self.assertEqual(report_path.read_bytes(), previous)

    def test_valid_override_and_script_setting_control_timestep(self) -> None:
        result = self.invoke("--ticks", "2", "--step", "0.5", "--set", "runtime.step=0.125")
        self.assert_success(result)
        report = json.loads(result.stdout)
        self.assertAlmostEqual(report["elapsed_seconds"], 0.25)
        script = self.root / "commands.txt"
        script.write_text("engine.set runtime.step 0.25\ntick\n", encoding="utf-8")
        report_path = self.root / "report.json"
        result = self.invoke("--ticks", "1", "--step", "0.125", "--script", script,
                             "--report", report_path)
        self.assert_success(result)
        report = self.read_report(report_path)
        self.assertIs(report["success"], True)
        self.assertEqual(report["tick"], 2)
        self.assertAlmostEqual(report["elapsed_seconds"], 0.5)

    def test_invalid_script_step_preserves_last_valid_step(self) -> None:
        script = self.root / "commands.txt"
        report_path = self.root / "report.json"
        for value in ("0", "nan", "nonsense", "2"):
            with self.subTest(value=value):
                script.write_text(f"engine.set runtime.step {value}\ntick\n", encoding="utf-8")
                result = self.invoke("--ticks", "1", "--step", "0.125", "--script", script,
                                     "--report", report_path)
                report = self.assert_failed_report(result, report_path)
                self.assertEqual(report["tick"], 2)
                self.assertAlmostEqual(report["elapsed_seconds"], 0.25)
                self.assertIn("runtime.step", result.stderr)

    def test_interactive_tick_and_stop(self) -> None:
        report_path = self.root / "report.json"
        result = self.invoke("--ticks", "0", "--interactive", "--report", report_path,
                             input_text="  tick \r\nengine.stop\r\n tick\r\n")
        self.assert_success(result)
        report = self.read_report(report_path)
        self.assertIs(report["success"], True)
        self.assertEqual(report["tick"], 1)

    def test_script_line_limit_is_enforced(self) -> None:
        script = self.root / "commands.txt"
        script.write_bytes(b" " * 65537 + b"\n")
        report_path = self.root / "report.json"
        result = self.invoke("--ticks", "0", "--script", script, "--report", report_path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("65536", result.stderr)
        self.assertFalse(report_path.exists())

    def test_invalid_arguments_and_missing_script_fail(self) -> None:
        for arguments in (("--unknown-option",), ("--ticks",), ("--ticks", "-1"),
                          ("--set", "runtime.step"), ("--set", "=0.125"),
                          ("--script", self.root / "missing.txt")):
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--binary", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    BINARY = arguments.binary.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
