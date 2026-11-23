#!/usr/bin/env python3
"""Strict, deterministic JSON authoring cooker for the GEPACK01 fixture format.

Python 3.11+ and the standard library are sufficient. ``normalize_world`` and
``cook_bytes`` accept decoded JSON objects and never mutate them. ``cook`` reads
bounded UTF-8 JSON, rejects duplicate keys, then replaces one output file only
after validation and a complete temporary write. Its returned metadata describes
the resulting package; it is not a second, atomically published manifest.

CRC32 detects accidental corruption, not malicious changes. The source SHA256
identifies canonical authored content; neither field authenticates a package.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import secrets
import stat
import struct
import sys
from typing import Any, Iterator
import zlib


MAGIC = b"GEPACK01"
SCHEMA_VERSION = 1
MAX_SOURCE_BYTES = 8 * 1024 * 1024
MAX_PACKAGE_BYTES = 4 * 1024 * 1024
MAX_REGIONS = 4096
MAX_ENTITIES = 65536
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1
READINESS_BITS = {"visual": 1, "collision": 2, "navigation": 4, "logic": 8}

_HEADER = struct.Struct("<8sIIIIQ32s")
_REGION = struct.Struct("<IIQII")
_ENTITY = struct.Struct("<QIiiii")
_CRC = struct.Struct("<I")
_WORLD_FIELDS = frozenset(("schema_version", "baseline_id", "regions"))
_REGION_FIELDS = frozenset(("id", "budget_bytes", "required", "entities"))
_ENTITY_FIELDS = frozenset(("id", "initial_value", "position_mm"))


def _short(value: object) -> str:
    text = repr(value)
    return text if len(text) <= 100 else text[:97] + "..."


def _object(value: object, fields: frozenset[str], location: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise ValueError(f"{location} must be an object")
    if any(type(key) is not str for key in value):
        raise ValueError(f"{location} keys must be strings")
    actual = set(value)
    missing, unknown = fields - actual, actual - fields
    if missing:
        raise ValueError(f"{location} is missing fields: {', '.join(sorted(missing))}")
    if unknown:
        raise ValueError(f"{location} has unknown fields: {_short(sorted(unknown))}")
    return value


def _integer(value: object, minimum: int, maximum: int, location: str) -> int:
    # bool is an int subclass, but it is never an authoring integer.
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{location} must be an integer in [{minimum}, {maximum}]")
    return value


def _array(value: object, minimum: int, maximum: int, location: str) -> list[Any]:
    if type(value) is not list or not minimum <= len(value) <= maximum:
        raise ValueError(f"{location} must be an array of {minimum}..{maximum} items")
    return value


def normalize_world(data: object) -> dict[str, Any]:
    """Validate exact v1 fields and return an independent canonical-order object.

    Regions and each region's entities are sorted by ID. Required readiness names
    are ordered visual, collision, navigation, logic. Integer bounds, global
    entity uniqueness and count limits are checked before any bytes are emitted.
    Invalid authoring data raises ``ValueError``.
    """
    world = _object(data, _WORLD_FIELDS, "world")
    _integer(world["schema_version"], SCHEMA_VERSION, SCHEMA_VERSION, "schema_version")
    baseline = _integer(world["baseline_id"], 1, UINT64_MAX, "baseline_id")
    regions = _array(world["regions"], 1, MAX_REGIONS, "regions")
    region_ids: set[int] = set()
    entity_ids: set[int] = set()
    normalized_regions: list[dict[str, Any]] = []
    entity_count = 0

    for region_index, value in enumerate(regions):
        location = f"regions[{region_index}]"
        region = _object(value, _REGION_FIELDS, location)
        region_id = _integer(region["id"], 1, UINT32_MAX, f"{location}.id")
        if region_id in region_ids:
            raise ValueError(f"duplicate region ID {region_id}")
        region_ids.add(region_id)
        budget = _integer(region["budget_bytes"], 1, UINT64_MAX, f"{location}.budget_bytes")
        required = _array(region["required"], 1, len(READINESS_BITS), f"{location}.required")
        names: set[str] = set()
        for name in required:
            if type(name) is not str or name not in READINESS_BITS:
                raise ValueError(f"{location}.required has unknown readiness name {_short(name)}")
            if name in names:
                raise ValueError(f"{location}.required repeats {_short(name)}")
            names.add(name)
        entities = _array(region["entities"], 1, MAX_ENTITIES, f"{location}.entities")
        entity_count += len(entities)
        if entity_count > MAX_ENTITIES:
            raise ValueError(f"world exceeds {MAX_ENTITIES} entities")
        normalized_entities: list[dict[str, Any]] = []
        for entity_index, entity_value in enumerate(entities):
            entity_location = f"{location}.entities[{entity_index}]"
            entity = _object(entity_value, _ENTITY_FIELDS, entity_location)
            entity_id = _integer(entity["id"], 1, UINT64_MAX, f"{entity_location}.id")
            if entity_id in entity_ids:
                raise ValueError(f"duplicate entity ID {entity_id}")
            entity_ids.add(entity_id)
            initial = _integer(entity["initial_value"], INT32_MIN, INT32_MAX,
                               f"{entity_location}.initial_value")
            position = _array(entity["position_mm"], 3, 3, f"{entity_location}.position_mm")
            coordinates = [
                _integer(coordinate, INT32_MIN, INT32_MAX, f"{entity_location}.position_mm[{axis}]")
                for axis, coordinate in enumerate(position)
            ]
            normalized_entities.append({"id": entity_id, "initial_value": initial,
                                        "position_mm": coordinates})
        normalized_entities.sort(key=lambda entity: entity["id"])
        normalized_regions.append({
            "id": region_id,
            "budget_bytes": budget,
            "required": [name for name in READINESS_BITS if name in names],
            "entities": normalized_entities,
        })

    size = _HEADER.size + len(regions) * _REGION.size + entity_count * _ENTITY.size + _CRC.size
    if size > MAX_PACKAGE_BYTES:
        raise ValueError(f"cooked package exceeds {MAX_PACKAGE_BYTES} bytes")
    normalized_regions.sort(key=lambda region: region["id"])
    return {"schema_version": SCHEMA_VERSION, "baseline_id": baseline, "regions": normalized_regions}


def _encode_world(world: dict[str, Any]) -> bytes:
    canonical = json.dumps(world, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True, allow_nan=False).encode("ascii")
    regions = world["regions"]
    entity_count = sum(len(region["entities"]) for region in regions)
    size = _HEADER.size + len(regions) * _REGION.size + entity_count * _ENTITY.size + _CRC.size
    if size > MAX_PACKAGE_BYTES:
        raise ValueError(f"cooked package exceeds {MAX_PACKAGE_BYTES} bytes")
    output = bytearray(size)
    _HEADER.pack_into(output, 0, MAGIC, SCHEMA_VERSION, len(regions), entity_count, 0,
                      world["baseline_id"], hashlib.sha256(canonical).digest())
    region_offset = _HEADER.size
    entity_offset = _HEADER.size + len(regions) * _REGION.size
    first_entity = 0
    for region in regions:
        count = len(region["entities"])
        mask = sum(READINESS_BITS[name] for name in region["required"])
        _REGION.pack_into(output, region_offset, region["id"], mask,
                          region["budget_bytes"], first_entity, count)
        region_offset += _REGION.size
        for entity in region["entities"]:
            _ENTITY.pack_into(output, entity_offset, entity["id"], region["id"],
                              entity["initial_value"], *entity["position_mm"])
            entity_offset += _ENTITY.size
        first_entity += count
    _CRC.pack_into(output, size - _CRC.size, zlib.crc32(memoryview(output)[:-_CRC.size]))
    return bytes(output)


def cook_bytes(data: object) -> bytes:
    """Validate decoded authoring data and return the exact GEPACK01 bytes."""
    return _encode_world(normalize_world(data))


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {_short(key)}")
        result[key] = value
    return result


def _parse_integer(text: str) -> int:
    # No accepted field needs more than 20 decimal digits. Bound conversion even
    # when the embedding interpreter has disabled its own integer-string limit.
    if len(text.removeprefix("-")) > 20:
        raise ValueError("JSON integer exceeds the supported 64-bit range")
    return int(text)


def _reject_number(text: str) -> Any:
    raise ValueError(f"JSON numbers must be finite integers: {_short(text)}")


def _decode_world(raw: bytes) -> object:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object,
                          parse_int=_parse_integer, parse_float=_reject_number,
                          parse_constant=_reject_number)
    except UnicodeDecodeError as error:
        raise ValueError("authoring source must be UTF-8 JSON") from error
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid JSON at line {error.lineno}, column {error.colno}: {error.msg}") from error
    except RecursionError as error:
        raise ValueError("authoring JSON is nested too deeply") from error


def _is_link(info: os.stat_result) -> bool:
    # Windows junctions and other reparse points must not redirect a write either.
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    )


_DIRECTORY_FDS = (
    hasattr(os, "O_DIRECTORY") and hasattr(os, "O_NOFOLLOW")
    and all(function in os.supports_dir_fd
            for function in (os.open, os.stat, os.mkdir, os.unlink, os.rename))
)


def _fallback_parent(path: Path, *, create: bool) -> None:
    """Check every parent component on platforms without directory descriptors."""
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            if not create:
                raise
            current.mkdir(exist_ok=True)
            info = current.lstat()
        if _is_link(info) or not stat.S_ISDIR(info.st_mode):
            raise ValueError(f"parent must be a directory without symbolic links: {current}")


@contextmanager
def _open_parent(path: Path, *, create: bool = False) -> Iterator[int | None]:
    if not _DIRECTORY_FDS:
        _fallback_parent(path.parent, create=create)
        yield None
        return
    # Parent aliases have already been resolved. Anchor subsequent traversal and
    # publication to descriptors where supported; this is not a hostile sandbox.
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    descriptor = os.open(path.anchor, flags)
    try:
        for part in path.parent.parts[1:]:
            try:
                child = os.open(part, flags, dir_fd=descriptor)
            except FileNotFoundError:
                if not create:
                    raise
                try:
                    os.mkdir(part, dir_fd=descriptor)
                except FileExistsError:
                    pass
                child = os.open(part, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        yield descriptor
    finally:
        os.close(descriptor)


def _entry(path: Path, parent_fd: int | None) -> str | Path:
    return path.name if parent_fd is not None else path


def _identity(info: os.stat_result) -> tuple[int, int]:
    return info.st_dev, info.st_ino


def _parent_identity(path: Path, parent_fd: int | None) -> tuple[int, int]:
    return _identity(os.fstat(parent_fd) if parent_fd is not None else path.parent.stat())


def _read_source(source: Path, parent_fd: int | None) -> tuple[bytes, tuple[int, int]]:
    entry = _entry(source, parent_fd)
    before = os.stat(entry, dir_fd=parent_fd, follow_symlinks=False)
    if _is_link(before) or not stat.S_ISREG(before.st_mode):
        raise ValueError(f"source must be a regular file without symbolic links: {source}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    flags |= getattr(os, "O_NONBLOCK", 0)
    descriptor = os.open(entry, flags, dir_fd=parent_fd)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or _identity(info) != _identity(before):
            raise ValueError("source changed while it was being opened")
        if info.st_size > MAX_SOURCE_BYTES:
            raise ValueError(f"authoring source exceeds {MAX_SOURCE_BYTES} bytes")
        raw = stream.read(MAX_SOURCE_BYTES + 1)
        if len(raw) > MAX_SOURCE_BYTES:
            raise ValueError(f"authoring source exceeds {MAX_SOURCE_BYTES} bytes")
        return raw, _identity(info)


def _check_destination(output: Path, output_fd: int | None, source: Path,
                       source_parent: tuple[int, int], source_file: tuple[int, int]) -> None:
    if output_fd is None:
        _fallback_parent(output.parent, create=False)
    if output.name == source.name and _parent_identity(output, output_fd) == source_parent:
        raise ValueError("source and output must be different files")
    try:
        info = os.stat(_entry(output, output_fd), dir_fd=output_fd, follow_symlinks=False)
    except FileNotFoundError:
        return
    if _is_link(info) or not stat.S_ISREG(info.st_mode):
        raise ValueError(f"output must be a regular file without symbolic links: {output}")
    if _identity(info) == source_file:
        raise ValueError("source and output must not refer to the same file")


def _publish(package: bytes, output: Path, output_fd: int | None, source: Path,
             source_parent: tuple[int, int], source_file: tuple[int, int]) -> None:
    _check_destination(output, output_fd, source, source_parent, source_file)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    for _ in range(16):
        temporary = output.parent / f".ge-cook-{secrets.token_hex(16)}.tmp"
        try:
            descriptor = os.open(_entry(temporary, output_fd), flags, 0o600, dir_fd=output_fd)
            break
        except FileExistsError:
            continue
    else:
        raise OSError("could not create a unique temporary output")
    published = False
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(package)
            stream.flush()
            os.fsync(stream.fileno())
        _check_destination(output, output_fd, source, source_parent, source_file)
        os.replace(_entry(temporary, output_fd), _entry(output, output_fd),
                   src_dir_fd=output_fd, dst_dir_fd=output_fd)
        published = True
    finally:
        if not published:
            try:
                os.unlink(_entry(temporary, output_fd), dir_fd=output_fd)
            except FileNotFoundError:
                pass


def _canonical_file(path: Path) -> Path:
    absolute = Path(path).absolute()
    if absolute.name in ("", ".", ".."):
        raise ValueError("source and output must name files")
    try:
        # Resolve only the parent: the final entry must remain visible to lstat
        # so a source/output symlink cannot silently become its target file.
        return absolute.parent.resolve(strict=False) / absolute.name
    except RuntimeError as error:
        raise ValueError(f"cannot resolve parent directory: {absolute.parent}") from error


def cook(source: Path, output: Path) -> dict[str, Any]:
    """Cook a source into one safely replaced output and return derived metadata.

    Invalid source data raises ``ValueError``; filesystem failures raise
    ``OSError``. Final-entry symlinks, nonregular files and source/output identity
    aliases are rejected. Caller-selected parent directories are trusted: their
    aliases are resolved once, and missing directories are created only after
    source validation. Directory descriptors anchor publication where supported;
    callers must exclude hostile concurrent namespace changes. Replacement is
    not a universal power-loss durability guarantee.
    """
    source, output = _canonical_file(source), _canonical_file(output)
    with _open_parent(source) as source_fd:
        raw, source_file = _read_source(source, source_fd)
        world = normalize_world(_decode_world(raw))
        package = _encode_world(world)
        metadata = {
            "schema_version": SCHEMA_VERSION,
            "baseline_id": world["baseline_id"],
            "source_sha256": package[32:64].hex(),
            "package_sha256": hashlib.sha256(package).hexdigest(),
            "crc32": _CRC.unpack_from(package, len(package) - _CRC.size)[0],
            "region_count": len(world["regions"]),
            "entity_count": sum(len(region["entities"]) for region in world["regions"]),
            "size_bytes": len(package),
        }
        source_parent = _parent_identity(source, source_fd)
        with _open_parent(output, create=True) as output_fd:
            _publish(package, output, output_fd, source, source_parent, source_file)
    return metadata


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("source", type=Path, help="UTF-8 v1 world authoring JSON")
    parser.add_argument("--output", type=Path, required=True, help="destination GEPACK01 file")
    arguments = parser.parse_args(argv)
    try:
        metadata = cook(arguments.source, arguments.output)
    except (ValueError, OSError) as error:
        print(f"cook: error: {error}", file=sys.stderr)
        return 1
    print(json.dumps(metadata, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
