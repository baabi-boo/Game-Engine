#!/usr/bin/env python3
"""Exercise the native application in fresh processes using only the stdlib.

Usage: python3 tests/test_headless.py --binary PATH --fixture PATH [-v]
The remaining arguments are passed to unittest, so individual tests may be named.
Every invocation uses an isolated working directory and explicit package paths.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


BINARY: Path | None = None
FIXTURE: Path | None = None
REQUIRED_CHECKS = {
    "incomplete_readiness_rejected",
    "retry_issued_new_generation",
    "stale_token_rejected",
    "stale_handle_rejected",
    "retirement_retained_reservation",
    "retirement_ack_released_reservation",
    "all_regions_visited",
    "mutation_survived_reload",
    "snapshot_restored",
    "all_values_restored",
    "budget_respected",
    "budget_admission_matches_capacity",
    "runtime_handles_released",
}


def next_value(value: int) -> int:
    return value - 1 if value == (1 << 31) - 1 else value + 1


def checksummed(data: bytes | bytearray) -> bytes:
    output = bytearray(data)
    struct.pack_into("<I", output, len(output) - 4, zlib.crc32(output[:-4]))
    return bytes(output)


class HeadlessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if BINARY is None or FIXTURE is None:
            raise RuntimeError("run this suite with --binary PATH --fixture PATH")
        if not BINARY.is_file() or not FIXTURE.is_file():
            raise RuntimeError(f"binary and fixture must exist: {BINARY}, {FIXTURE}")
        cls.package_bytes = FIXTURE.read_bytes()
        magic, version, cls.region_count, cls.entity_count, reserved, cls.baseline, digest = (
            struct.unpack_from("<8sIIIIQ32s", cls.package_bytes)
        )
        if (magic, version, reserved) != (b"GEPACK01", 1, 0):
            raise RuntimeError("integration fixture has an unexpected package header")
        if len(cls.package_bytes) != 64 + cls.region_count * 24 + cls.entity_count * 28 + 4:
            raise RuntimeError("integration fixture has an unexpected package length")
        cls.source_digest = digest.hex()
        cls.regions = [struct.unpack_from("<IIQII", cls.package_bytes, 64 + index * 24)
                       for index in range(cls.region_count)]
        entity_start = 64 + cls.region_count * 24
        cls.entities = [struct.unpack_from("<QIiiii", cls.package_bytes, entity_start + index * 28)
                        for index in range(cls.entity_count)]
        cls.changed_id = cls.entities[0][0]
        cls.authored_value = cls.entities[0][2]

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ge-headless-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def invoke(self, *arguments: str | Path, package: Path | None = None) -> subprocess.CompletedProcess:
        command = [str(BINARY), "--package", str(package if package is not None else FIXTURE)]
        command.extend(str(argument) for argument in arguments)
        return subprocess.run(command, cwd=self.root, capture_output=True, text=True,
                              encoding="utf-8", timeout=30, check=False)

    def success(self, *arguments: str | Path, package: Path | None = None) -> dict:
        result = self.invoke(*arguments, package=package)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        try:
            payload = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            self.fail(f"expected one stdout JSON object: {error}; output={result.stdout!r}")
        self.assertIsInstance(payload, dict)
        return payload

    def assert_failure(self, result: subprocess.CompletedProcess) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout, "", "failed data command must not emit success JSON")
        self.assertIn("ge_headless:", result.stderr)

    def assert_package_metadata(self, payload: dict) -> None:
        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(payload["region_count"], self.region_count)
        self.assertEqual(payload["entity_count"], self.entity_count)
        self.assertEqual(payload["baseline_id"], self.baseline)
        self.assertEqual(payload["source_sha256"], self.source_digest)

    def assert_policy_result(self, payload: dict, *, budget: int, restored: int | None = None) -> None:
        self.assertEqual(payload["mode"], "headless-policy-fixture")
        self.assert_package_metadata(payload)
        self.assertEqual(payload["budget_bytes"], budget)
        self.assertEqual(payload["initial_value"], self.authored_value)
        self.assertEqual(payload["restored_value"], restored)
        before = self.authored_value if restored is None else restored
        self.assertEqual(payload["value_before_mutation"], before)
        self.assertEqual(payload["final_value"], next_value(before))
        self.assertEqual(payload["persistent_id"], self.changed_id)
        self.assertGreater(payload["peak_reserved_bytes"], 0)
        self.assertLessEqual(payload["peak_reserved_bytes"], budget)
        self.assertEqual(payload["final_reserved_bytes"], 0)
        checks = payload["checks"]
        self.assertIsInstance(checks, dict)
        self.assertTrue(REQUIRED_CHECKS <= checks.keys(), REQUIRED_CHECKS - checks.keys())
        for name, passed in checks.items():
            self.assertIs(passed, True, f"lifecycle check did not pass: {name}")

        expected_regions = [region[0] for region in self.regions]
        self.assertEqual(payload["visited_regions"], expected_regions)
        self.assertEqual(payload["activation_order"], expected_regions + expected_regions[:1])
        # Independently inspect emitted reservation evidence as well as the
        # app's named checks; retirement must remain charged until acknowledgement.
        events = payload["events"]
        self.assertTrue(events)
        actions = {event["action"] for event in events}
        self.assertTrue({"incomplete_activation_rejected", "stale_completion_rejected",
                         "cancel", "acknowledge_cancellation", "activate", "retire",
                         "acknowledge_retirement"} <= actions)
        observed_peak = 0
        for event in events:
            memory = event["memory"]
            for amount in memory.values():
                self.assertIs(type(amount), int)
                self.assertGreaterEqual(amount, 0)
            reserved = memory["pending_bytes"] + memory["resident_bytes"] + memory["retiring_bytes"]
            self.assertEqual(memory["capacity_bytes"], budget)
            self.assertEqual(memory["reserved_bytes"], reserved)
            self.assertEqual(memory["available_bytes"], budget - reserved)
            self.assertLessEqual(reserved, budget)
            observed_peak = max(observed_peak, reserved)
            if event["action"] in ("cancel", "retire"):
                self.assertEqual(event["state"], "Retiring")
                self.assertGreater(memory["retiring_bytes"], 0)
            elif event["action"] in ("acknowledge_cancellation", "acknowledge_retirement"):
                self.assertEqual(event["state"], "Unloaded")
                self.assertEqual(reserved, 0)
            elif event["action"] in ("incomplete_activation_rejected", "stale_completion_rejected"):
                self.assertIs(event["accepted"], False)
        self.assertEqual(observed_peak, payload["peak_reserved_bytes"])
        self.assertEqual(events[-1]["memory"]["reserved_bytes"], 0)

    def read_saved_values(self, path: Path, *, changed_value: int) -> tuple[bytes, int]:
        raw = path.read_bytes()
        self.assertGreaterEqual(len(raw), 44)
        magic, version, reserved, baseline, sequence, count, reserved2 = struct.unpack_from(
            "<8sIIQQII", raw
        )
        self.assertEqual((magic, version, reserved, baseline, count, reserved2),
                         (b"GESTATE1", 1, 0, self.baseline, self.entity_count, 0))
        self.assertGreater(sequence, 0)
        self.assertEqual(len(raw), 40 + count * 12 + 4)
        self.assertEqual(struct.unpack_from("<I", raw, len(raw) - 4)[0], zlib.crc32(raw[:-4]))
        records = [struct.unpack_from("<Qi", raw, 40 + index * 12) for index in range(count)]
        expected = sorted((entity[0], changed_value if entity[0] == self.changed_id else entity[2])
                          for entity in self.entities)
        self.assertEqual(records, expected)
        return raw, sequence

    def test_inspection_reports_the_loaded_package(self) -> None:
        payload = self.success("--inspect")
        self.assertEqual(payload["mode"], "inspect")
        self.assert_package_metadata(payload)
        expected = [dict(zip(("id", "required_mask", "budget_bytes", "first_entity", "entity_count"),
                             region)) for region in self.regions]
        self.assertEqual(payload["regions"], expected)
        self.assertEqual(list(self.root.iterdir()), [])

    @unittest.skipUnless(os.name == "posix" and Path("/dev/full").is_char_device(),
                         "requires POSIX /dev/full")
    def test_output_write_errors_return_failure(self) -> None:
        commands = (
            ("inspect", ("--package", str(FIXTURE), "--inspect")),
            ("lifecycle", ("--package", str(FIXTURE))),
            ("help", ("--help",)),
            ("version", ("--version",)),
        )
        # Small responses can remain buffered until process shutdown. Check
        # both those and the larger lifecycle report against a failing sink.
        with Path("/dev/full").open("wb") as output:
            for mode, arguments in commands:
                with self.subTest(mode=mode):
                    result = subprocess.run([str(BINARY), *arguments], cwd=self.root,
                                            stdout=output, stderr=subprocess.PIPE,
                                            text=True, encoding="utf-8", timeout=30, check=False)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("ge_headless: failed to write command output", result.stderr)

    def test_default_lifecycle_mutates_retires_and_restores(self) -> None:
        payload = self.success()
        self.assert_policy_result(payload, budget=96)
        self.assertIs(payload["save_state_written"], False)
        self.assertIs(payload["budget_pressure_observed"], True)
        self.assertGreater(payload["budget_denials"], 0)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_budget_override_changes_actual_admission(self) -> None:
        for capacity, pressure in ((64, True), (128, False)):
            with self.subTest(capacity=capacity):
                payload = self.success("--budget-bytes", str(capacity))
                self.assert_policy_result(payload, budget=capacity)
                self.assertIs(payload["budget_pressure_observed"], pressure)
                if pressure:
                    self.assertGreater(payload["budget_denials"], 0)
                else:
                    self.assertEqual(payload["budget_denials"], 0)
                    self.assertEqual(payload["peak_reserved_bytes"], 128)

    def test_saved_state_restores_in_a_new_process(self) -> None:
        first_path = self.root / "first.gestate"
        first = self.success("--save-state", first_path)
        self.assert_policy_result(first, budget=96)
        self.assertIs(first["save_state_written"], True)
        self.assertIs(first["checks"]["save_round_trip"], True)
        first_bytes, first_sequence = self.read_saved_values(first_path, changed_value=first["final_value"])
        self.assertEqual(first["state_sequence"], first_sequence)

        second_path = self.root / "second.gestate"
        # success() always starts a new OS process; no in-memory store can leak
        # between these invocations and stand in for on-disk restoration.
        second = self.success("--restore-state", first_path, "--save-state", second_path)
        self.assert_policy_result(second, budget=96, restored=first["final_value"])
        self.assertIs(second["checks"]["save_round_trip"], True)
        _, second_sequence = self.read_saved_values(second_path, changed_value=second["final_value"])
        self.assertEqual(second["restored_sequence"], first_sequence)
        self.assertEqual(second["state_sequence"], second_sequence)
        self.assertGreater(second_sequence, first_sequence)
        self.assertEqual(first_path.read_bytes(), first_bytes)

    def test_unicode_package_and_save_paths_restore_in_a_new_process(self) -> None:
        # Include multiple scripts, a non-BMP character and spaces: these cannot
        # round-trip through a typical Windows ANSI code page. The working
        # directory also stands in for a Unicode caller path or user directory.
        self.root = self.root / "\u73a9\u5bb6 caf\u00e9 \U0001f30d"
        self.root.mkdir()
        package = self.root / "\u4e16\u754c \U0001f3ae.gepack"
        package.write_bytes(self.package_bytes)
        save = self.root / "\u72b6\u614b \U0001f4be.gestate"

        inspected = self.success("--inspect", package=package)
        self.assertEqual(inspected["mode"], "inspect")
        self.assert_package_metadata(inspected)

        first = self.success("--save-state", save, package=package)
        self.assert_policy_result(first, budget=96)
        self.assertIs(first["save_state_written"], True)
        self.assertIs(first["checks"]["save_round_trip"], True)
        _, first_sequence = self.read_saved_values(save, changed_value=first["final_value"])
        self.assertEqual(first["state_sequence"], first_sequence)

        # A fresh process restores and replaces the same file using relative
        # paths, exercising both Unicode argument handling and native publication.
        second = self.success("--restore-state", save.name, "--save-state", save.name,
                              package=Path(package.name))
        self.assert_policy_result(second, budget=96, restored=first["final_value"])
        self.assertIs(second["save_state_written"], True)
        self.assertIs(second["checks"]["save_round_trip"], True)
        _, second_sequence = self.read_saved_values(save, changed_value=second["final_value"])
        self.assertEqual(second["restored_sequence"], first_sequence)
        self.assertEqual(second["state_sequence"], second_sequence)
        self.assertGreater(second_sequence, first_sequence)
        self.assertEqual(package.read_bytes(), self.package_bytes)
        self.assertEqual(set(self.root.iterdir()), {package, save})

    def test_unicode_arguments_preserve_strict_cli_errors(self) -> None:
        unknown = "--\u672a\u77e5-\U0001f30d"
        result = self.invoke(unknown)
        self.assert_failure(result)
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown argument: " + unknown, result.stderr)
        # Decimal parsing remains ASCII-only, even when native arguments are wide.
        result = self.invoke("--budget-bytes", "\uff19\uff16")
        self.assert_failure(result)
        self.assertEqual(result.returncode, 2)
        self.assertIn("positive uint64 decimal integer", result.stderr)

    def test_corrupt_packages_are_rejected_before_creating_save(self) -> None:
        bad_crc = bytearray(self.package_bytes)
        bad_crc[-1] ^= 1
        bad_version = bytearray(self.package_bytes)
        struct.pack_into("<I", bad_version, 8, 2)
        bad_reserved = bytearray(self.package_bytes)
        struct.pack_into("<I", bad_reserved, 20, 1)
        bad_count = bytearray(self.package_bytes)
        struct.pack_into("<I", bad_count, 16, self.entity_count + 1)
        variants = {
            "truncated": self.package_bytes[:20],
            "checksum": bytes(bad_crc),
            "trailing": self.package_bytes + b"\x00",
            "version": checksummed(bad_version),
            "reserved": checksummed(bad_reserved),
            "count": checksummed(bad_count),
        }
        destination = self.root / "never-written.gestate"
        for name, raw in variants.items():
            with self.subTest(case=name):
                broken = self.root / f"{name}.gepack"
                broken.write_bytes(raw)
                self.assert_failure(self.invoke("--save-state", destination, package=broken))
                self.assertFalse(destination.exists())
                self.assertEqual(broken.read_bytes(), raw)
        # Inspection uses the same strict reader, and failure preserves a
        # pre-existing destination even when --save-state was requested.
        self.assert_failure(self.invoke("--inspect", package=broken))
        destination.write_bytes(b"preserve existing save")
        self.assert_failure(self.invoke("--save-state", destination, package=broken))
        self.assertEqual(destination.read_bytes(), b"preserve existing save")

    def test_corrupt_and_incompatible_restores_preserve_existing_save(self) -> None:
        valid = self.root / "valid.gestate"
        initial = self.success("--save-state", valid)
        original, _ = self.read_saved_values(valid, changed_value=initial["final_value"])
        invalid_crc = bytearray(original)
        invalid_crc[-1] ^= 1
        wrong_baseline = bytearray(original)
        struct.pack_into("<Q", wrong_baseline, 16, self.baseline + 1)
        for name, raw in (("checksum", bytes(invalid_crc)),
                          ("baseline", checksummed(wrong_baseline))):
            with self.subTest(case=name):
                broken = self.root / f"{name}.gestate"
                broken.write_bytes(raw)
                # A previously absent output detects acceptance independently
                # of the save API's protection for an existing destination.
                absent = self.root / "absent.gestate"
                self.assert_failure(self.invoke("--restore-state", broken, "--save-state", absent))
                self.assertFalse(absent.exists())
                self.assert_failure(self.invoke("--restore-state", broken, "--save-state", valid))
                self.assertEqual(valid.read_bytes(), original)
                self.assertEqual(broken.read_bytes(), raw)

    def test_invalid_existing_save_and_package_output_alias_are_preserved(self) -> None:
        invalid_save = self.root / "invalid.gestate"
        invalid_save.write_bytes(b"invalid existing save must survive")
        self.assert_failure(self.invoke("--save-state", invalid_save))
        self.assertEqual(invalid_save.read_bytes(), b"invalid existing save must survive")
        package_copy = self.root / "district.gepack"
        package_copy.write_bytes(self.package_bytes)
        self.assert_failure(self.invoke("--save-state", package_copy, package=package_copy))
        self.assertEqual(package_copy.read_bytes(), self.package_bytes)

    def test_invalid_budgets_fail_without_clobbering_save(self) -> None:
        destination = self.root / "preserved.gestate"
        destination.write_bytes(b"previous save")
        for value in ("0", "-1", "63", "1.5", "many", str(1 << 64)):
            with self.subTest(budget=value):
                self.assert_failure(self.invoke("--budget-bytes", value, "--save-state", destination))
                self.assertEqual(destination.read_bytes(), b"previous save")

    def test_missing_inputs_and_invalid_cli_combinations_fail(self) -> None:
        missing = self.root / "missing"
        for arguments in (("--inspect", "--budget-bytes", "96"),
                          ("--inspect", "--save-state", str(missing)),
                          ("--inspect", "--restore-state", str(missing)),
                          ("--unknown-option",), ("--budget-bytes",),
                          ("--restore-state", str(missing))):
            with self.subTest(arguments=arguments):
                self.assert_failure(self.invoke(*arguments))
        self.assert_failure(self.invoke(package=missing))
        no_package = subprocess.run([str(BINARY)], cwd=self.root, capture_output=True,
                                    text=True, timeout=30, check=False)
        self.assert_failure(no_package)
        self.assertFalse(missing.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    BINARY = arguments.binary.resolve()
    FIXTURE = arguments.fixture.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
