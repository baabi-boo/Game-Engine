#!/usr/bin/env python3
"""Regress the actual native GEPACK01 reader using only the Python stdlib.

Usage: python3 tests/test_native_package.py --binary PATH --fixture PATH [-v]
Remaining arguments are passed to unittest, so individual tests may be named.
Each invocation runs --package PATH --inspect in an isolated temporary directory.
Wire records are encoded independently of the cooker. Structural mutations get
a fresh CRC32, so checksum rejection cannot conceal a missing validation rule.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


BINARY: Path | None = None
FIXTURE: Path | None = None
HEADER = struct.Struct("<8sIIIIQ32s")
REGION = struct.Struct("<IIQII")
ENTITY = struct.Struct("<QIiiii")
CRC = struct.Struct("<I")
MAX_REGIONS = 4096
MAX_ENTITIES = 65536
MAX_PACKAGE_BYTES = 4 * 1024 * 1024
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1
REGION_FIELDS = ("id", "required_mask", "budget_bytes", "first_entity", "entity_count")


def with_crc(payload: bytes | bytearray) -> bytes:
    return bytes(payload) + CRC.pack(zlib.crc32(payload))


def wire_package(regions: list[tuple[int, ...]], entities: list[tuple[int, ...]],
                 *, baseline: int = 1, digest: bytes = bytes(range(32))) -> bytes:
    """Encode explicit records without normalizing or validating their values."""
    payload = HEADER.pack(b"GEPACK01", 1, len(regions), len(entities), 0, baseline, digest)
    payload += b"".join(REGION.pack(*record) for record in regions)
    payload += b"".join(ENTITY.pack(*record) for record in entities)
    return with_crc(payload)


def changed(raw: bytes, format_: str, offset: int, *values: int | bytes) -> bytes:
    payload = bytearray(raw[:-CRC.size])
    struct.pack_into(format_, payload, offset, *values)
    return with_crc(payload)


class NativePackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if BINARY is None or FIXTURE is None:
            raise RuntimeError("run this suite with --binary PATH --fixture PATH")
        if not BINARY.is_file() or not FIXTURE.is_file():
            raise RuntimeError(f"binary and fixture must exist: {BINARY}, {FIXTURE}")
        cls.fixture_bytes = FIXTURE.read_bytes()
        # IDs are ordered inside each region but deliberately interleave across
        # regions. This also gives every ownership/range mutation a known layout.
        cls.seed = wire_package(
            [(10, 1, 1, 0, 2), (20, 15, 64, 2, 2)],
            [(11, 10, -1, 1, 2, 3), (13, 10, 0, -1, -2, -3),
             (12, 20, 1, 0, 0, 0), (14, 20, 2, 4, 5, 6)],
        )
        cls.second_region = HEADER.size + REGION.size
        cls.entity_start = HEADER.size + 2 * REGION.size

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ge-native-package-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def invoke(self, raw: bytes) -> subprocess.CompletedProcess[str]:
        path = self.root / "candidate.gepack"
        path.write_bytes(raw)
        result = subprocess.run(
            [str(BINARY), "--package", str(path), "--inspect"],
            cwd=self.root, capture_output=True, text=True, timeout=30, check=False,
        )
        self.assertEqual(path.read_bytes(), raw, "inspection changed its input")
        self.assertEqual(set(self.root.iterdir()), {path}, "inspection created extra files")
        return result

    def assert_crc_valid(self, raw: bytes) -> None:
        self.assertGreaterEqual(len(raw), CRC.size)
        self.assertEqual(CRC.unpack_from(raw, len(raw) - CRC.size)[0],
                         zlib.crc32(raw[:-CRC.size]), "structural case has an invalid CRC")

    def assert_rejected(self, raw: bytes, *, valid_crc: bool = True
                        ) -> subprocess.CompletedProcess[str]:
        if valid_crc:
            self.assert_crc_valid(raw)
        result = self.invoke(raw)
        # A normal data error is distinct from a crash, timeout, or CLI error.
        self.assertEqual(result.returncode, 1, f"stdout={result.stdout!r}; stderr={result.stderr!r}")
        self.assertEqual(result.stdout, "", "rejected input emitted success metadata")
        self.assertIn("ge_headless:", result.stderr)
        return result

    def assert_accepted(self, raw: bytes) -> dict:
        self.assert_crc_valid(raw)
        result = self.invoke(raw)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        try:
            payload = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            self.fail(f"expected one inspection JSON object: {error}; output={result.stdout!r}")
        self.assertIsInstance(payload, dict)
        magic, version, regions, entities, reserved, baseline, digest = HEADER.unpack_from(raw)
        self.assertEqual((magic, version, reserved), (b"GEPACK01", 1, 0))
        self.assertEqual(payload["mode"], "inspect")
        self.assertEqual(payload["schema_version"], version)
        self.assertEqual(payload["baseline_id"], baseline)
        self.assertEqual(payload["region_count"], regions)
        self.assertEqual(payload["entity_count"], entities)
        self.assertEqual(payload["source_sha256"], digest.hex())
        expected_regions = [
            dict(zip(REGION_FIELDS, REGION.unpack_from(raw, HEADER.size + index * REGION.size)))
            for index in range(regions)
        ]
        self.assertEqual(payload["regions"], expected_regions)
        return payload

    def test_built_fixture_inspects_exact_metadata(self) -> None:
        self.assert_accepted(self.fixture_bytes)

    @unittest.skipUnless(os.name == "posix" and hasattr(os, "mkfifo"),
                         "POSIX FIFO support required")
    def test_fifo_is_rejected_without_waiting_for_a_writer(self) -> None:
        path = self.root / "package.fifo"
        try:
            os.mkfifo(path)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create a FIFO: {error}")
        try:
            result = subprocess.run(
                [str(BINARY), "--package", str(path), "--inspect"],
                cwd=self.root, capture_output=True, text=True, timeout=5, check=False,
            )
        except subprocess.TimeoutExpired:
            self.fail("native package reader waited for a FIFO writer instead of rejecting input")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("regular file", result.stderr)
        self.assertTrue(stat.S_ISFIFO(path.stat().st_mode))
        self.assertEqual(set(self.root.iterdir()), {path})

    def test_symlinks_to_regular_package_inputs_are_accepted(self) -> None:
        target = self.root / "regular.gepack"
        target.write_bytes(self.fixture_bytes)
        link = self.root / "linked.gepack"
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"host cannot create a symbolic link: {error}")
        result = subprocess.run(
            [str(BINARY), "--package", str(link), "--inspect"],
            cwd=self.root, capture_output=True, text=True, timeout=5, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(json.loads(result.stdout)["mode"], "inspect")
        self.assertEqual(target.read_bytes(), self.fixture_bytes)
        self.assertTrue(link.is_symlink())
        self.assertEqual(set(self.root.iterdir()), {target, link})

    def test_entity_ids_need_only_be_sorted_within_each_region(self) -> None:
        self.assert_accepted(self.seed)

    def test_magic_must_match_all_eight_bytes(self) -> None:
        for magic in (b"GEPACK00", b"\x00EPACK01", b"GESTATE1"):
            with self.subTest(magic=magic):
                self.assert_rejected(changed(self.seed, "<8s", 0, magic))

    def test_unsupported_schema_versions_are_rejected(self) -> None:
        for version in (0, 2, UINT32_MAX):
            with self.subTest(version=version):
                self.assert_rejected(changed(self.seed, "<I", 8, version))

    def test_reserved_header_bits_must_be_zero(self) -> None:
        for reserved in (1, 1 << 31, UINT32_MAX):
            with self.subTest(reserved=reserved):
                self.assert_rejected(changed(self.seed, "<I", 20, reserved))

    def test_header_counts_reject_zero_and_integer_maximum(self) -> None:
        for offset in (12, 16):
            for count in (0, UINT32_MAX):
                with self.subTest(offset=offset, count=count):
                    self.assert_rejected(changed(self.seed, "<I", offset, count))

    def test_in_range_counts_must_match_the_file_length(self) -> None:
        for offset, count in ((12, 1), (12, 3), (16, 3), (16, 5)):
            with self.subTest(offset=offset, count=count):
                self.assert_rejected(changed(self.seed, "<I", offset, count))

    def test_baseline_is_a_nonzero_unsigned_64_bit_value(self) -> None:
        self.assert_rejected(changed(self.seed, "<Q", 24, 0))
        for baseline in (1, 1 << 63, UINT64_MAX):
            with self.subTest(baseline=baseline):
                self.assert_accepted(changed(self.seed, "<Q", 24, baseline))

    def test_source_digest_is_carried_without_native_authentication(self) -> None:
        for digest in (bytes(32), b"\xff" * 32):
            with self.subTest(digest=digest.hex()):
                self.assert_accepted(changed(self.seed, "<32s", 32, digest))

    def test_region_ids_are_nonzero_unique_and_increasing(self) -> None:
        for index, region_id in ((0, 0), (1, 10), (1, 9)):
            with self.subTest(index=index, region_id=region_id):
                raw = changed(self.seed, "<I", HEADER.size + index * REGION.size, region_id)
                # Keep ownership consistent so that an ownership failure
                # cannot mask a missing region-ID validation rule.
                for entity_index in range(index * 2, index * 2 + 2):
                    raw = changed(raw, "<I", self.entity_start + entity_index * ENTITY.size + 8,
                                  region_id)
                self.assert_rejected(raw)

    def test_masks_allow_defined_bits_and_reject_empty_or_unknown_bits(self) -> None:
        for mask in (1, 2, 4, 8, 15):
            with self.subTest(valid_mask=mask):
                self.assert_accepted(changed(self.seed, "<I", HEADER.size + 4, mask))
        for mask in (0, 16, 17, 1 << 31, UINT32_MAX):
            with self.subTest(invalid_mask=mask):
                self.assert_rejected(changed(self.seed, "<I", HEADER.size + 4, mask))

    def test_region_budget_must_be_nonzero(self) -> None:
        self.assert_rejected(changed(self.seed, "<Q", self.second_region + 8, 0))

    def test_region_and_entity_integer_extremes_are_accepted(self) -> None:
        # --inspect has no runtime budget admission, so the full wire ranges
        # remain valid even when a lifecycle run could not admit these budgets.
        for region_id, budget, entity_id in ((1, 1, 1),
                                            (UINT32_MAX, UINT64_MAX, UINT64_MAX)):
            with self.subTest(region_id=region_id, budget=budget, entity_id=entity_id):
                self.assert_accepted(wire_package(
                    [(region_id, 15, budget, 0, 1)],
                    [(entity_id, region_id, INT32_MIN, INT32_MAX, INT32_MIN, -1)],
                    baseline=UINT64_MAX,
                ))

    def test_entity_ranges_start_at_zero_and_reject_gaps_and_overlaps(self) -> None:
        for offset, first in ((HEADER.size + 16, 1), (self.second_region + 16, 3),
                              (self.second_region + 16, 1),
                              (self.second_region + 16, UINT32_MAX)):
            with self.subTest(offset=offset, first=first):
                self.assert_rejected(changed(self.seed, "<I", offset, first))

    def test_each_region_requires_a_nonempty_entity_range(self) -> None:
        for empty_index in (0, 1):
            with self.subTest(empty_region=empty_index):
                # Adjust every count/range and owner to keep the remaining
                # table valid; emptiness must be the only structural defect.
                regions = ([(10, 1, 1, 0, 0), (20, 15, 64, 0, 2)] if empty_index == 0
                           else [(10, 1, 1, 0, 2), (20, 15, 64, 2, 0)])
                owner = 20 if empty_index == 0 else 10
                self.assert_rejected(wire_package(
                    regions, [(11, owner, 0, 0, 0, 0), (13, owner, 0, 0, 0, 0)],
                ))

    def test_entity_ranges_cannot_exceed_remaining_records_or_wrap(self) -> None:
        for offset, count in ((HEADER.size + 20, 5), (self.second_region + 20, 3),
                              (HEADER.size + 20, UINT32_MAX),
                              (self.second_region + 20, UINT32_MAX)):
            with self.subTest(offset=offset, count=count):
                self.assert_rejected(changed(self.seed, "<I", offset, count))

    def test_entity_ranges_must_cover_the_entire_entity_table(self) -> None:
        self.assert_rejected(changed(self.seed, "<I", self.second_region + 20, 1))

    def test_entity_ids_are_nonzero_and_increasing_inside_each_region(self) -> None:
        for offset, entity_id in ((self.entity_start, 0),
                                  (self.entity_start + ENTITY.size, 11),
                                  (self.entity_start + ENTITY.size, 10)):
            with self.subTest(offset=offset, entity_id=entity_id):
                self.assert_rejected(changed(self.seed, "<Q", offset, entity_id))

    def test_entity_ownership_matches_the_containing_region_range(self) -> None:
        for offset, owner in ((self.entity_start + 8, 0), (self.entity_start + 8, 20),
                              (self.entity_start + 8, UINT32_MAX),
                              (self.entity_start + 3 * ENTITY.size + 8, 10)):
            with self.subTest(offset=offset, owner=owner):
                self.assert_rejected(changed(self.seed, "<I", offset, owner))

    def test_entity_ids_are_globally_unique_across_regions(self) -> None:
        # Both cases preserve the second region's local ordering. Only the
        # global check can reject a duplicate of the first region's 11 or 13.
        for index, entity_id in ((2, 11), (3, 13)):
            with self.subTest(index=index, entity_id=entity_id):
                self.assert_rejected(changed(self.seed, "<Q",
                                             self.entity_start + index * ENTITY.size, entity_id))

    def test_trailing_payload_data_is_rejected_with_a_valid_checksum(self) -> None:
        for extra in (b"\x00", b"\x00" * ENTITY.size, self.seed):
            with self.subTest(extra_bytes=len(extra)):
                self.assert_rejected(with_crc(self.seed[:-CRC.size] + extra))

    def test_truncated_headers_and_records_are_rejected_with_a_valid_checksum(self) -> None:
        payload = self.seed[:-CRC.size]
        for length in (0, HEADER.size - 1, HEADER.size, HEADER.size + REGION.size - 1,
                       self.entity_start, self.entity_start + ENTITY.size - 1, len(payload) - 1):
            with self.subTest(payload_bytes=length):
                self.assert_rejected(with_crc(payload[:length]))

    def test_missing_or_corrupt_checksum_is_rejected(self) -> None:
        corrupted = bytearray(self.seed)
        corrupted[-1] ^= 1
        for name, raw in (("missing", self.seed[:-CRC.size]),
                          ("partial", self.seed[:-1]), ("corrupt", bytes(corrupted))):
            with self.subTest(case=name):
                self.assert_rejected(raw, valid_crc=False)

    def test_region_and_entity_count_boundaries(self) -> None:
        for regions, entities, accepted in ((MAX_REGIONS, MAX_ENTITIES, True),
                                            (MAX_REGIONS + 1, MAX_REGIONS + 1, False),
                                            (1, MAX_ENTITIES + 1, False)):
            with self.subTest(regions=regions, entities=entities):
                # The complete table and all ranges match the header even for
                # a count one above its limit; size errors cannot mask that cap.
                per_region = entities // regions
                self.assertEqual(per_region * regions, entities)
                raw = wire_package(
                    [(index + 1, 15, 1, index * per_region, per_region)
                     for index in range(regions)],
                    [(index + 1, index // per_region + 1, 0, 0, 0, 0)
                     for index in range(entities)],
                )
                self.assertLess(len(raw), MAX_PACKAGE_BYTES)
                if accepted:
                    self.assertEqual(len(raw), 1_933_380)
                    self.assert_accepted(raw)
                else:
                    self.assert_rejected(raw)

    def test_file_larger_than_four_mib_is_rejected_by_the_byte_limit(self) -> None:
        payload = self.seed[:-CRC.size]
        raw = with_crc(payload + bytes(MAX_PACKAGE_BYTES + 1 - CRC.size - len(payload)))
        self.assertEqual(len(raw), MAX_PACKAGE_BYTES + 1)
        result = self.assert_rejected(raw)
        # Valid v1 counts cannot fill 4 MiB. This coarse diagnostic category
        # distinguishes the independent file cap from a later count/size error.
        self.assertIn("limit", result.stderr.lower())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    BINARY = arguments.binary.resolve()
    FIXTURE = arguments.fixture.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
