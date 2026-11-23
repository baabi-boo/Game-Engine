#!/usr/bin/env python3
"""Standard-library tests for the authored JSON and GEPACK01 contracts.

Run directly with ``python3 tests/test_cooker.py [-v]`` or through CTest.
Temporary files are isolated; the committed fixture is only read.
"""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib


REPOSITORY = Path(__file__).resolve().parents[1]
COOKER_PATH = REPOSITORY / "tools" / "cook.py"
FIXTURE_PATH = REPOSITORY / "assets" / "fixtures" / "district.world.json"
FIXTURE_SHA256 = "c40f4dd023f8ca5f4d57034abdfff73bdb22145a39694a56df3c4155ce8efb5d"

_SPEC = importlib.util.spec_from_file_location("ge_cooker_under_test", COOKER_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load cooker: {COOKER_PATH}")
cooker = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(cooker)


def small_world() -> dict:
    return {
        "schema_version": 1,
        "baseline_id": 1,
        "regions": [{
            "id": 1,
            "budget_bytes": 64,
            "required": ["visual", "logic"],
            "entities": [{"id": 1001, "initial_value": 0, "position_mm": [0, 0, 0]}],
        }],
    }


def replace_value(data: dict, path: tuple, value: object) -> None:
    cursor = data
    for key in path[:-1]:
        cursor = cursor[key]
    cursor[path[-1]] = value


class CookerTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ge-cooker-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "world.json"
        self.output = self.root / "world.gepack"
        self.source.write_text(json.dumps(small_world()), encoding="utf-8")

    def assert_no_temporary_outputs(self) -> None:
        self.assertEqual(list(self.root.rglob(".ge-cook-*.tmp")), [])

    def assert_invalid_source_preserves_output(self, raw: bytes) -> None:
        self.source.write_bytes(raw)
        previous = b"previous valid output must survive"
        self.output.write_bytes(previous)
        with self.assertRaises(ValueError):
            cooker.cook(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), previous)
        self.assertEqual(self.source.read_bytes(), raw)
        self.assert_no_temporary_outputs()

    def symlink(self, target: Path, link: Path, *, directory: bool = False) -> None:
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create symbolic links: {error}")

    def test_fixture_golden_bytes_and_independent_layout(self) -> None:
        authored = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
        package = cooker.cook_bytes(authored)
        self.assertEqual(hashlib.sha256(package).hexdigest(), FIXTURE_SHA256)
        self.assertEqual(len(package), 252)
        magic, version, regions, entities, reserved, baseline, digest = struct.unpack_from(
            "<8sIIIIQ32s", package
        )
        self.assertEqual((magic, version, regions, entities, reserved, baseline),
                         (b"GEPACK01", 1, 3, 4, 0, 1))
        canonical = json.dumps(authored, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=True, allow_nan=False).encode("ascii")
        self.assertEqual(digest, hashlib.sha256(canonical).digest())
        self.assertEqual(
            [struct.unpack_from("<IIQII", package, 64 + index * 24) for index in range(3)],
            [(1, 11, 64, 0, 2), (2, 15, 64, 2, 1), (3, 9, 64, 3, 1)],
        )
        self.assertEqual(
            [struct.unpack_from("<QIiiii", package, 136 + index * 28) for index in range(4)],
            [(1001, 1, 0, 0, 0, 0), (1002, 1, 7, 1000, 0, -1000),
             (2001, 2, 0, 100000, 0, 0), (3001, 3, 3, 200000, 0, 0)],
        )
        self.assertEqual(struct.unpack_from("<I", package, len(package) - 4)[0],
                         zlib.crc32(package[:-4]))

    def test_order_and_json_whitespace_do_not_change_package(self) -> None:
        authored = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
        reordered = dict(reversed(list(authored.items())))
        reordered["regions"] = list(reversed(copy.deepcopy(authored["regions"])))
        for region in reordered["regions"]:
            region["required"].reverse()
            region["entities"].reverse()
        self.source.write_text(json.dumps(reordered, indent=4), encoding="utf-8")
        first = cooker.cook(self.source, self.output)
        expected = cooker.cook_bytes(authored)
        self.assertEqual(self.output.read_bytes(), expected)
        self.source.write_text(json.dumps(authored, separators=(",", ":")), encoding="utf-8")
        second = cooker.cook(self.source, self.output)
        self.assertEqual(first, second)
        self.assertEqual(self.output.read_bytes(), expected)

    def test_normalization_is_independent_and_never_mutates_input(self) -> None:
        authored = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
        authored["regions"].reverse()
        for region in authored["regions"]:
            region["required"].reverse()
            region["entities"].reverse()
        original = copy.deepcopy(authored)
        normalized = cooker.normalize_world(authored)
        cooker.cook_bytes(authored)
        self.assertEqual(authored, original)
        self.assertEqual([region["id"] for region in normalized["regions"]], [1, 2, 3])
        normalized["regions"][0]["entities"][0]["position_mm"][0] = 999
        normalized["regions"][0]["required"].append("navigation")
        self.assertEqual(authored, original)
        authored["regions"][-1]["entities"][-1]["id"] = 0
        invalid_original = copy.deepcopy(authored)
        with self.assertRaises(ValueError):
            cooker.cook_bytes(authored)
        self.assertEqual(authored, invalid_original)

    def test_returned_metadata_describes_published_bytes(self) -> None:
        metadata = cooker.cook(FIXTURE_PATH, self.output)
        package = self.output.read_bytes()
        self.assertEqual(metadata, {
            "schema_version": 1,
            "baseline_id": 1,
            "source_sha256": package[32:64].hex(),
            "package_sha256": FIXTURE_SHA256,
            "crc32": zlib.crc32(package[:-4]),
            "region_count": 3,
            "entity_count": 4,
            "size_bytes": 252,
        })
        self.assert_no_temporary_outputs()

    def test_exact_object_fields_and_types(self) -> None:
        objects = [(), ("regions", 0), ("regions", 0, "entities", 0)]
        for path in objects:
            for alteration in ("missing", "unknown", "non_string_key", "array", "null"):
                with self.subTest(path=path, alteration=alteration):
                    data = small_world()
                    target = data
                    for key in path:
                        target = target[key]
                    if alteration == "missing":
                        del target[next(iter(target))]
                    elif alteration == "unknown":
                        target["unexpected"] = 1
                    elif alteration == "non_string_key":
                        target[2] = 1
                    elif path:
                        replace_value(data, path, [] if alteration == "array" else None)
                    else:
                        data = [] if alteration == "array" else None
                    with self.assertRaises(ValueError):
                        cooker.cook_bytes(data)

    def test_integer_types_and_ranges_are_strict(self) -> None:
        fields = [
            (("schema_version",), 1, 1),
            (("baseline_id",), 1, (1 << 64) - 1),
            (("regions", 0, "id"), 1, (1 << 32) - 1),
            (("regions", 0, "budget_bytes"), 1, (1 << 64) - 1),
            (("regions", 0, "entities", 0, "id"), 1, (1 << 64) - 1),
            (("regions", 0, "entities", 0, "initial_value"), -(1 << 31), (1 << 31) - 1),
            (("regions", 0, "entities", 0, "position_mm", 0), -(1 << 31), (1 << 31) - 1),
        ]
        for path, minimum, maximum in fields:
            for value in (True, False, 1.0, "1", None, minimum - 1, maximum + 1):
                with self.subTest(path=path, value=value):
                    data = small_world()
                    replace_value(data, path, value)
                    with self.assertRaises(ValueError):
                        cooker.cook_bytes(data)

    def test_integer_extremes_encode_without_truncation(self) -> None:
        data = small_world()
        data["baseline_id"] = (1 << 64) - 1
        region = data["regions"][0]
        region["id"] = (1 << 32) - 1
        region["budget_bytes"] = (1 << 64) - 1
        entity = region["entities"][0]
        entity.update(id=(1 << 64) - 1, initial_value=-(1 << 31),
                      position_mm=[-(1 << 31), 0, (1 << 31) - 1])
        package = cooker.cook_bytes(data)
        self.assertEqual(struct.unpack_from("<Q", package, 24)[0], (1 << 64) - 1)
        self.assertEqual(struct.unpack_from("<IIQII", package, 64),
                         ((1 << 32) - 1, 9, (1 << 64) - 1, 0, 1))
        self.assertEqual(struct.unpack_from("<QIiiii", package, 88),
                         ((1 << 64) - 1, (1 << 32) - 1, -(1 << 31),
                          -(1 << 31), 0, (1 << 31) - 1))

    def test_arrays_and_readiness_names_are_strict(self) -> None:
        cases = [
            (("regions",), []), (("regions",), ()),
            (("regions", 0, "entities"), []), (("regions", 0, "entities"), {}),
            (("regions", 0, "required"), []),
            (("regions", 0, "required"), ["visual"] * 5),
            (("regions", 0, "required"), ["visual", "visual"]),
            (("regions", 0, "required"), ["Visual"]),
            (("regions", 0, "required"), [True]),
            (("regions", 0, "required"), "visual"),
            (("regions", 0, "entities", 0, "position_mm"), [0, 0]),
            (("regions", 0, "entities", 0, "position_mm"), [0, 0, 0, 0]),
            (("regions", 0, "entities", 0, "position_mm"), (0, 0, 0)),
        ]
        for path, value in cases:
            with self.subTest(path=path, value=value):
                data = small_world()
                replace_value(data, path, value)
                with self.assertRaises(ValueError):
                    cooker.cook_bytes(data)

    def test_duplicate_region_and_global_entity_ids_are_rejected(self) -> None:
        for duplicate in ("region", "entity_in_region", "entity_across_regions"):
            with self.subTest(duplicate=duplicate):
                data = small_world()
                region = data["regions"][0]
                if duplicate == "entity_in_region":
                    region["entities"].append(copy.deepcopy(region["entities"][0]))
                else:
                    other = copy.deepcopy(region)
                    if duplicate == "region":
                        other["entities"][0]["id"] += 1
                    else:
                        other["id"] += 1
                    data["regions"].append(other)
                with self.assertRaises(ValueError):
                    cooker.cook_bytes(data)

    def test_region_count_boundary(self) -> None:
        data = small_world()
        data["regions"] = [
            {"id": index, "budget_bytes": 1, "required": ["logic"],
             "entities": [{"id": index, "initial_value": 0, "position_mm": [0, 0, 0]}]}
            for index in range(1, 4097)
        ]
        package = cooker.cook_bytes(data)
        self.assertEqual(struct.unpack_from("<II", package, 12), (4096, 4096))
        data["regions"].append(data["regions"][0])
        with self.assertRaisesRegex(ValueError, "regions.*4096"):
            cooker.cook_bytes(data)

    def test_per_region_and_global_entity_count_boundaries(self) -> None:
        data = small_world()
        entities = [{"id": index, "initial_value": 0, "position_mm": [0, 0, 0]}
                    for index in range(1, 65537)]
        data["regions"][0]["entities"] = entities
        package = cooker.cook_bytes(data)
        self.assertEqual(struct.unpack_from("<I", package, 16)[0], 65536)
        self.assertEqual(len(package), 64 + 24 + 65536 * 28 + 4)
        entities.append({"id": 65537, "initial_value": 0, "position_mm": [0, 0, 0]})
        with self.assertRaisesRegex(ValueError, "entities.*65536"):
            cooker.cook_bytes(data)
        data["regions"][0]["entities"] = entities[:32768]
        data["regions"].append({"id": 2, "budget_bytes": 1, "required": ["logic"],
                                "entities": entities[32768:]})
        with self.assertRaisesRegex(ValueError, "world exceeds 65536 entities"):
            cooker.cook_bytes(data)

    def test_package_size_guard_preserves_output(self) -> None:
        # The v1 count caps fit under 4 MiB. Lower this independent size bound to
        # exercise its failure path with a small, otherwise valid world.
        expected_size = 64 + 24 + 28 + 4
        self.output.write_bytes(b"keep this package")
        with mock.patch.object(cooker, "MAX_PACKAGE_BYTES", expected_size - 1):
            with self.assertRaisesRegex(ValueError, "cooked package exceeds"):
                cooker.cook(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"keep this package")
        self.assert_no_temporary_outputs()

    def test_source_size_limit_accepts_boundary_and_rejects_one_more_byte(self) -> None:
        source = self.source.read_bytes()
        self.source.write_bytes(source + b" " * (8 * 1024 * 1024 - len(source)))
        cooker.cook(self.source, self.output)
        previous = self.output.read_bytes()
        with self.source.open("ab") as stream:
            stream.write(b" ")
        with self.assertRaisesRegex(ValueError, "authoring source exceeds"):
            cooker.cook(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), previous)
        self.assert_no_temporary_outputs()

    def test_duplicate_json_keys_at_each_object_level_are_rejected(self) -> None:
        raw = json.dumps(small_world(), separators=(",", ":"))
        duplicates = [
            raw.replace('"schema_version":1', '"schema_version":1,"schema_version":1', 1),
            raw.replace('"id":1,', '"id":1,"id":1,', 1),
            raw.replace('"id":1001,', '"id":1001,"id":1001,', 1),
        ]
        for duplicate in duplicates:
            with self.subTest(source=duplicate):
                self.assert_invalid_source_preserves_output(duplicate.encode("utf-8"))

    def test_malformed_json_utf8_and_noninteger_json_numbers_are_rejected(self) -> None:
        raw = json.dumps(small_world(), separators=(",", ":"))
        cases = {
            "truncated": b'{"schema_version":',
            "trailing": raw.encode("utf-8") + b" false",
            "comment": b"//comment\n" + raw.encode("utf-8"),
            "utf8": b"\xff",
            "bom": b"\xef\xbb\xbf" + raw.encode("utf-8"),
            "null": b"null",
            "nesting": b"[" * 1200 + b"0" + b"]" * 1200,
        }
        for literal in ("0.0", "1e1", "NaN", "Infinity", "-Infinity", "1" * 21):
            cases[literal] = raw.replace('"initial_value":0',
                                        f'"initial_value":{literal}', 1).encode("utf-8")
        for name, invalid in cases.items():
            with self.subTest(case=name):
                self.assert_invalid_source_preserves_output(invalid)

    def test_invalid_source_does_not_create_destination_directories(self) -> None:
        self.source.write_text("{}", encoding="utf-8")
        destination = self.root / "missing" / "nested" / "world.gepack"
        with self.assertRaises(ValueError):
            cooker.cook(self.source, destination)
        self.assertFalse(destination.parent.parent.exists())

    def test_valid_source_creates_parents_and_replaces_complete_output(self) -> None:
        destination = self.root / "new" / "nested" / "world.gepack"
        cooker.cook(self.source, destination)
        expected = cooker.cook_bytes(small_world())
        self.assertEqual(destination.read_bytes(), expected)
        destination.write_bytes(b"old output")
        cooker.cook(self.source, destination)
        self.assertEqual(destination.read_bytes(), expected)
        self.assert_no_temporary_outputs()

    def test_publication_failure_preserves_output_and_cleans_temporary_file(self) -> None:
        self.output.write_bytes(b"last good output")
        original = self.source.read_bytes()
        for operation in ("fsync", "replace"):
            with self.subTest(operation=operation):
                with mock.patch.object(cooker.os, operation, side_effect=OSError("injected I/O failure")):
                    with self.assertRaises(OSError):
                        cooker.cook(self.source, self.output)
                self.assertEqual(self.output.read_bytes(), b"last good output")
                self.assertEqual(self.source.read_bytes(), original)
                self.assert_no_temporary_outputs()

    def test_source_and_output_identity_aliases_are_rejected(self) -> None:
        original = self.source.read_bytes()
        (self.root / "child").mkdir()
        aliases = [self.source, self.root / "child" / ".." / self.source.name]
        for alias in aliases:
            with self.subTest(alias=alias):
                with self.assertRaises(ValueError):
                    cooker.cook(self.source, alias)
                self.assertEqual(self.source.read_bytes(), original)
        self.assert_no_temporary_outputs()

    def test_hardlinked_source_output_is_rejected(self) -> None:
        try:
            os.link(self.source, self.output)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create hard links: {error}")
        original = self.source.read_bytes()
        with self.assertRaises(ValueError):
            cooker.cook(self.source, self.output)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(self.output.read_bytes(), original)
        self.assertTrue(os.path.samefile(self.source, self.output))

    def test_final_source_symlink_is_rejected(self) -> None:
        alias = self.root / "source-link.json"
        self.symlink(self.source, alias)
        self.output.write_bytes(b"keep output")
        with self.assertRaises(ValueError):
            cooker.cook(alias, self.output)
        self.assertTrue(alias.is_symlink())
        self.assertEqual(self.output.read_bytes(), b"keep output")

    def test_final_output_symlink_is_rejected(self) -> None:
        target = self.root / "keep.gepack"
        target.write_bytes(b"keep target")
        self.symlink(target, self.output)
        with self.assertRaises(ValueError):
            cooker.cook(self.source, self.output)
        self.assertTrue(self.output.is_symlink())
        self.assertEqual(target.read_bytes(), b"keep target")
        self.assert_no_temporary_outputs()

    def test_trusted_symlink_parent_is_accepted_and_identity_is_checked(self) -> None:
        real_parent = self.root / "real"
        real_parent.mkdir()
        alias_parent = self.root / "parent-alias"
        self.symlink(real_parent, alias_parent, directory=True)
        real_source = real_parent / "source.json"
        original = self.source.read_bytes()
        real_source.write_bytes(original)
        output = alias_parent / "nested" / "world.gepack"
        cooker.cook(alias_parent / "source.json", output)
        self.assertEqual((real_parent / "nested" / "world.gepack").read_bytes(),
                         cooker.cook_bytes(small_world()))
        with self.assertRaises(ValueError):
            cooker.cook(real_source, alias_parent / "source.json")
        self.assertEqual(real_source.read_bytes(), original)

    def test_nonregular_source_and_destination_are_rejected(self) -> None:
        directory = self.root / "directory"
        directory.mkdir()
        with self.assertRaises(ValueError):
            cooker.cook(directory, self.output)
        with self.assertRaises(ValueError):
            cooker.cook(self.source, directory)
        self.assertTrue(directory.is_dir())
        self.assertFalse(self.output.exists())

    def test_cli_reports_metadata_and_returns_failure_without_clobbering(self) -> None:
        result = subprocess.run(
            [sys.executable, str(COOKER_PATH), str(FIXTURE_PATH), "--output", str(self.output)],
            cwd=self.root, capture_output=True, text=True, timeout=30, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        metadata = json.loads(result.stdout)
        self.assertEqual(metadata["package_sha256"], FIXTURE_SHA256)
        previous = self.output.read_bytes()
        self.source.write_text('{"baseline_id":1,"baseline_id":2}', encoding="utf-8")
        failed = subprocess.run(
            [sys.executable, str(COOKER_PATH), str(self.source), "--output", str(self.output)],
            cwd=self.root, capture_output=True, text=True, timeout=30, check=False,
        )
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(failed.stdout, "")
        self.assertIn("duplicate JSON key", failed.stderr)
        self.assertNotIn("Traceback", failed.stderr)
        self.assertEqual(self.output.read_bytes(), previous)
        self.assert_no_temporary_outputs()


if __name__ == "__main__":
    unittest.main()
