# Content pipeline and persistence formats

The implemented pipeline accepts bounded world JSON, cooks deterministic
`GEPACK01` packages, validates them in native code, and saves scalar object values
as `GESTATE1` snapshots. The supplied content is an original synthetic fixture.
Mesh, texture, animation, audio, and scene-file importers are not implemented.
Readiness names are symbolic policy signals; a `collision` bit does not imply a
collision engine, and `budget_bytes` models reservation cost rather than measured
resource allocation.

The executable contracts are the [cooker](../../tools/cook.py),
[package header](../../engine/content/include/ge/content/package.hpp),
[package reader](../../engine/content/src/package.cpp),
[state header](../../engine/world/include/ge/world/state.hpp), and
[state implementation](../../engine/world/src/state.cpp). The
[JSON Schema](../../schemas/world-v1.schema.json) documents the authored shape;
the cooker remains the authoritative validator. The compatibility decision is
recorded in [ADR 0005](../adr/0005-content-and-state-formats.md).

## Copy, edit, cook, inspect, save, and restart

Run these commands from the repository root with Python 3.11+, CMake 3.25+, Ninja,
and a C++20 compiler available. This example uses the development preset and its
native executable path. On Windows, use a Visual Studio Developer PowerShell,
replace `python3` with `py -3` if needed, and append `.exe` to `ge_headless`.

```sh
cmake --preset dev
cmake --build --preset dev --target ge_headless
```

Create a working copy under `out/` and change object `1001`'s initial scalar value
from `0` to `41`. The commands use Python for copying and editing so they do not
depend on a particular shell's file-copy syntax. The source fixture remains the
reference for the golden package described below.

```sh
python3 -c "from pathlib import Path; import shutil; Path('out/content-demo').mkdir(parents=True, exist_ok=True); shutil.copyfile('assets/fixtures/district.world.json', 'out/content-demo/district.world.json')"
python3 -c "from pathlib import Path; import json; p = Path('out/content-demo/district.world.json'); w = json.loads(p.read_text(encoding='utf-8')); w['regions'][0]['entities'][0]['initial_value'] = 41; p.write_text(json.dumps(w, indent=2) + '\n', encoding='utf-8')"
python3 tools/cook.py out/content-demo/district.world.json --output out/content-demo/district.gepack
out/build/dev/bin/ge_headless --package out/content-demo/district.gepack --inspect
```

The cooker prints one JSON metadata object after publishing the package. Native
`--inspect` fully validates the binary and prints its baseline, carried source
digest, counts, and region records. It accepts only `--package` with `--inspect`;
save, restore, and budget options belong to the lifecycle run.

Use fresh save filenames for the first run. Each following command launches a
new process; the second restores the first process's saved values before making
the next mutation:

```sh
out/build/dev/bin/ge_headless --package out/content-demo/district.gepack --save-state out/content-demo/session-1.gestate
out/build/dev/bin/ge_headless --package out/content-demo/district.gepack --restore-state out/content-demo/session-1.gestate --save-state out/content-demo/session-2.gestate
```

For this edited fixture, the first run reports `persistent_id: 1001`,
`value_before_mutation: 41`, `final_value: 42`, and `state_sequence: 2`. The fresh
process reports `restored_value: 42`, `restored_sequence: 2`, `final_value: 43`, and
`state_sequence: 4`. Each run takes an in-memory snapshot and then publishes a
newer save, accounting for the two sequence advances. The default reservation
capacity is 96 modeled bytes; each fixture region requests 64. Increasing an
authored budget above 96 requires an adequate `--budget-bytes N` for a lifecycle
run. Inspection does not perform budget admission.

The native save API requires its parent directory to exist; the copy step above
creates it. Reusing an existing save path is allowed only with a compatible,
strictly newer sequence. To continue the same save in another process, restore
it and save back to that path:

```sh
out/build/dev/bin/ge_headless --package out/content-demo/district.gepack --restore-state out/content-demo/session-2.gestate --save-state out/content-demo/session-2.gestate
```

This continuation reports `final_value: 44` and `state_sequence: 6`. Repeating the
first command without restoring an existing sequence-2 destination is rejected;
choose a new destination or restore the existing compatible save.

### Reference fixture

Cook the unedited fixture separately to reproduce the reference product:

```sh
python3 tools/cook.py assets/fixtures/district.world.json --output out/content-demo/default.gepack
out/build/dev/bin/ge_headless --package out/content-demo/default.gepack --inspect
```

| Property | Reference value |
| --- | --- |
| Authored path | `assets/fixtures/district.world.json` |
| Baseline | `1` |
| Regions / entities | `3` / `4` |
| Cooked size | `252` bytes |
| Canonical `source_sha256` | `4548f9d6cd41044f88c6fd343b9167db1e61bf74813f9766f71d9c35bac40763` |
| Complete `package_sha256` | `c40f4dd023f8ca5f4d57034abdfff73bdb22145a39694a56df3c4155ce8efb5d` |
| Stored CRC32, unsigned decimal | `621285848` |

The package golden SHA-256 covers the complete 252-byte product, including its
CRC32. It is not the source hash. The fixture's region masks are `11`, `15`, and
`9`, with entity ranges `(0, 2)`, `(2, 1)`, and `(3, 1)` respectively. Region records
begin at byte 64, entity records at byte 136, and the CRC32 at byte 248.

## Authored version 1 JSON

Every field below is required, and every object rejects unknown keys. There are
no implicit defaults or extra extension fields. In particular, `$schema` is not
an authored field; associate the schema in an editor without inserting it into
the source document.

| Object | Exact keys | Constraints |
| --- | --- | --- |
| World | `schema_version`, `baseline_id`, `regions` | Version is integer `1`; baseline is nonzero `u64`; 1–4,096 regions. |
| Region | `id`, `budget_bytes`, `required`, `entities` | ID is nonzero `u32`; budget is nonzero `u64`; 1–4 distinct readiness names; 1–65,536 entities in the region. |
| Entity | `id`, `initial_value`, `position_mm` | ID is nonzero `u64`; initial value is `i32`; position is exactly three `i32` coordinates in `[x, y, z]` order, in millimetres. |

| Integer class | Inclusive minimum | Inclusive maximum |
| --- | ---: | ---: |
| Nonzero `u32` | `1` | `4294967295` |
| Nonzero `u64` | `1` | `18446744073709551615` |
| `i32` | `-2147483648` | `2147483647` |

Region IDs are unique within the world. Entity IDs are unique across the entire
world, including across regions, and the sum of all entity counts is at most
65,536. Every region contains at least one entity. These are content constraints,
not a promise that the entire authored world can be resident under a chosen
runtime budget.

The only readiness names and their binary bit values are:

| Name | Value | Bit |
| --- | ---: | ---: |
| `visual` | `1` | `0` |
| `collision` | `2` | `1` |
| `navigation` | `4` | `2` |
| `logic` | `8` | `3` |

An authored `required` list contains each selected name once. Its binary mask is
the sum of those values: any nonzero mask from `1` through `15` is valid. Zero,
unknown bits, unknown names, and repeated names are rejected. The list order is
not significant.

The file decoder accepts UTF-8 JSON without a byte-order mark, with a maximum
source length of 8 MiB (`8,388,608` bytes). Duplicate object keys, including keys
whose escape sequences decode to the same text, are rejected. Comments, trailing
commas, numeric strings, booleans used as integers, floating-point/exponent
number tokens such as `1.0` or `1e0`, and non-finite numbers are invalid. Integer
tokens are limited to 20 decimal digits excluding a leading minus sign, then
checked against each field's range. Keep 64-bit IDs exact when editing them;
tools that coerce numbers to floating point may change their value.

The JSON Schema captures required keys, value ranges, readiness uniqueness, and
per-array lengths. It does not establish unique `id` properties across objects,
the sum of entity counts, the original byte length, or absence of duplicate JSON
keys. JSON Schema's mathematical `integer` type may also accept integral decimal
or exponent notation that the cooker rejects. See the
[schema notes](../../schemas/README.md). No external schema-validation dependency
is needed to cook or build.

## Canonicalization and digests

The cooker first validates and creates an independent normalized object. It
sorts regions by numeric region ID, sorts each region's entities by numeric
entity ID, and orders readiness names as `visual`, `collision`, `navigation`,
`logic`. Coordinates retain their authored order. The normalized object is
serialized for hashing exactly as follows:

```python
canonical = json.dumps(
    normalized_world,
    sort_keys=True,
    separators=(",", ":"),
    ensure_ascii=True,
    allow_nan=False,
).encode("ascii")
source_sha256 = hashlib.sha256(canonical).digest()
```

Thus `source_sha256` is a semantic canonical-source digest, not SHA-256 of the raw
input file. Whitespace, indentation, object-key order, region order, entity order
within a region, and readiness-list order do not change either digest or product
bytes. Moving an entity to another region, changing a coordinate or scalar, or
changing a budget, readiness set, ID, baseline, or schema version changes the
authored meaning. Identical valid meaning produces identical package bytes for
the same format and cooker implementation.

The header stores the 32 digest bytes. Metadata renders them as lowercase hex in
`source_sha256`. The native reader validates the package structure and CRC32 but
does not recompute or verify this source digest against authored JSON. The
cooker's separate `package_sha256` is SHA-256 of all final package bytes and is
reported in metadata, not stored in the package. Keep the cooker revision and
declared inputs with build evidence; the header is not a complete build manifest.

## `GEPACK01` binary layout

All multi-byte integers are little-endian. Signed `i32` values use their 32-bit
two's-complement representation. There is no alignment padding and no native
C++ structure serialization. Let `R` be the region count and `E` the total entity
count. The exact file size is:

```text
64 + 24 * R + 28 * E + 4 bytes
```

The reader rejects a file larger than 4 MiB (`4,194,304` bytes) before allocating
its file buffer, and rejects sizes inconsistent with the header counts. The
count limits are 1–4,096 regions and 1–65,536 entities. Under all version 1 count
constraints the largest possible package is 1,933,380 bytes; the 4 MiB file cap
is an additional independent bound. The smallest valid package is 120 bytes.

### Header: 64 bytes

| Absolute offset | Bytes | Encoding | Field / rule |
| ---: | ---: | --- | --- |
| `0` | `8` | ASCII bytes | Exactly `GEPACK01` |
| `8` | `4` | `u32` | Schema version, exactly `1` |
| `12` | `4` | `u32` | Region count `R` |
| `16` | `4` | `u32` | Entity count `E` |
| `20` | `4` | `u32` | Reserved, exactly `0` |
| `24` | `8` | `u64` | Nonzero baseline ID |
| `32` | `32` | Raw bytes | Canonical source SHA-256 |

### Region record: 24 bytes

Region `i` begins at absolute offset `64 + 24 * i`, with `0 <= i < R`.

| Offset within record | Bytes | Encoding | Field / rule |
| ---: | ---: | --- | --- |
| `0` | `4` | `u32` | Nonzero region ID |
| `4` | `4` | `u32` | Required readiness mask, nonzero, with no bits outside `15` |
| `8` | `8` | `u64` | Nonzero modeled budget in bytes |
| `16` | `4` | `u32` | First entity's zero-based index in the entity table |
| `20` | `4` | `u32` | Nonzero entity count for this region |

Region IDs are strictly increasing. Entity ranges start at index zero, are
nonempty and contiguous in region order, and cover the entire entity table
exactly once. There are no gaps, overlaps, or unused records.

### Entity record: 28 bytes

Entity `j` begins at absolute offset `64 + 24 * R + 28 * j`, with `0 <= j < E`.

| Offset within record | Bytes | Encoding | Field / rule |
| ---: | ---: | --- | --- |
| `0` | `8` | `u64` | Nonzero persistent entity ID |
| `8` | `4` | `u32` | Owning region ID, matching the containing region range |
| `12` | `4` | `i32` | Initial scalar value |
| `16` | `4` | `i32` | X position in millimetres |
| `20` | `4` | `i32` | Y position in millimetres |
| `24` | `4` | `i32` | Z position in millimetres |

Entity IDs are strictly increasing within each region and globally unique.
Because regions own contiguous ranges, the whole entity table need not be
globally sorted by entity ID.

### Trailer: 4 bytes

At absolute offset `64 + 24 * R + 28 * E`, a little-endian `u32` stores the reflected
IEEE CRC32 of every preceding byte. It matches Python's `zlib.crc32(payload)`;
the reflected polynomial is `0xEDB88320`, with initial and final XOR values of
`0xFFFFFFFF`. The CRC itself is excluded from its input. Trailing bytes are
rejected. CRC32 detects accidental corruption and does not authenticate content.

## Baseline, product identity, and persistent identity

These identifiers have separate purposes:

| Identifier | Meaning | Persistence behavior |
| --- | --- | --- |
| `baseline_id` | Authored compatibility contract for a world's saved values | A restore requires the same baseline. A new incompatible world needs a new baseline or an explicit migration. |
| Persistent entity `id` | Stable identity of one logical object | A save records the ID and value. Restore requires exactly the store's known ID set. Reusing an ID for a different object changes its meaning. |
| Region `id` | Ownership and loading-policy identity | Present in the package; region membership is not stored in scalar snapshots. |
| `source_sha256` | Canonical authored-content identity | Provenance carried by the package; it is not a save compatibility key. |
| `package_sha256` | Identity of the complete cooked binary | Useful for build evidence and byte comparison; it is not a stable object ID or baseline. |
| Runtime handle | A resident object's slot and generation | Transient; reconstructed as objects become resident and never serialized in these snapshots. |
| Snapshot sequence | Ordering of snapshots for one state store | Nonzero on disk, monotonically advanced by capture/save; independent of IDs and digests. |

The current restore check compares baseline and the exact known persistent ID
set, not a product hash. Recooking with the same baseline and IDs can change
positions, budgets, readiness, or initial values and still accept an old save.
Saved scalar values then replace the new initial values. Authors must decide
whether those semantics remain compatible; matching integers alone do not prove
that an old save is meaningful. Changing, adding, or removing a persistent ID
requires an explicit compatibility decision and currently makes the old ID set
incompatible. Neither changing a digest nor rewriting a baseline performs a
migration.

## `GESTATE1` snapshot layout and rules

A snapshot owns an immutable copy of a baseline, sequence, and sorted scalar
records. It is independent of the originating store's lifetime or later edits.
It contains no positions, region records, runtime handles, readiness, resource
reservations, or arbitrary components.

All integers are little-endian with the same `i32` representation as packages.
Let `N` be the entry count. The exact file size is `40 + 12 * N + 4` bytes. The
entry limit is 0–65,536 and the independent file limit is 4 MiB (`4,194,304`
bytes). Empty snapshots are valid and occupy 44 bytes; at the entry-count limit
the file occupies 786,476 bytes. A package-backed headless store starts with all
of that nonempty package's entity IDs, so an empty snapshot cannot restore it.

### Header: 40 bytes

| Absolute offset | Bytes | Encoding | Field / rule |
| ---: | ---: | --- | --- |
| `0` | `8` | ASCII bytes | Exactly `GESTATE1` |
| `8` | `4` | `u32` | State schema version, exactly `1` |
| `12` | `4` | `u32` | Reserved, exactly `0` |
| `16` | `8` | `u64` | Nonzero baseline ID |
| `24` | `8` | `u64` | Nonzero snapshot sequence |
| `32` | `4` | `u32` | Entry count `N` |
| `36` | `4` | `u32` | Reserved, exactly `0` |

### Entry: 12 bytes; trailer: 4 bytes

Entry `k` begins at absolute offset `40 + 12 * k`, with `0 <= k < N`.

| Offset within entry | Bytes | Encoding | Field / rule |
| ---: | ---: | --- | --- |
| `0` | `8` | `u64` | Nonzero persistent ID, strictly increasing across the snapshot |
| `8` | `4` | `i32` | Current scalar value |

The little-endian CRC32 is at absolute offset `40 + 12 * N`. It uses the same
algorithm and coverage rule as the package trailer. Unknown magic or version,
nonzero reserved fields, zero baseline/sequence/IDs, duplicate or unordered IDs,
out-of-bound counts/lengths, CRC mismatches, and trailing bytes are errors.
CRC32 supplies no authenticity guarantee for saves either.

### State operations

`PersistentState` starts at sequence zero with an immutable set of known IDs.
Construction copies and sorts those initial records, rejecting zero or duplicate
IDs and invalid bounds. `set` edits a known scalar without changing the sequence;
unknown IDs do not create records. The caller serializes all access to the store
and its save paths. The implementation is synchronous, has no thread-safety or
multiwriter protocol, and does not merge concurrent snapshots.

`capture(sequence)` requires a nonzero sequence strictly newer than the live
sequence. It copies a complete snapshot and advances the live sequence only on
success. It does not publish a file. `restore` first validates the entire
snapshot, matching baseline and every known ID exactly once. It rejects an older
sequence but accepts an equal one so a snapshot can restore values after local
edits. Failed validation or allocation preserves all previous values and the
live sequence. Sequences range from `1` through `18446744073709551615` and never
wrap; exhaustion prevents any further newer capture or save.

`save(path, sequence)` requires a sequence strictly newer than the live store
and any existing compatible destination. Existing corrupt or incompatible saves
are rejected without replacement. An existing destination must be a regular
file, not a directory or final-entry symlink. The caller supplies an existing
parent directory and excludes competing writers or hostile namespace changes;
the API is not a filesystem sandbox.

The writer creates a sibling temporary file exclusively, checks the complete
write, flush, and close, and publishes with checked replacement: POSIX rename
or Windows `MoveFileExW` with `REPLACE_EXISTING`. It never deletes the old
destination first. A failure before successful publication preserves the old
destination and live sequence; temporary-file cleanup is best effort. Sequence
advances only after publication. This save path performs no `fsync` or directory
sync and makes no crash-recovery or power-loss durability guarantee.

## Cooker publication and native loading

`cook(source, output)` reads the bounded source, rejects invalid input, and
builds the entire validated product in memory before creating missing output
parents. Final source/output entries must be regular files when present;
final-entry symlinks and source/output aliases, including hard links to the same
file, are rejected. Parent aliases are resolved once. Caller-chosen parent
directories are trusted; descriptor-based traversal anchors operations where
supported, but the cooker is not a hostile-filesystem sandbox.

The cooker creates an exclusive sibling temporary file, writes and flushes the
complete bytes, syncs that file with `fsync`, and uses `os.replace` only after a
second destination check. A validation failure or failed operation before
replacement preserves a previous output. It attempts to remove an unpublished
temporary file on failure. The metadata returned or printed after publication describes
the resulting package; it is not a second atomically published manifest. The
parent directory is not synced, and replacement is not a universal crash or
power-loss durability guarantee.

The native package reader bounds input before allocating, checks the complete
format and CRC, and assembles temporary package state. It returns a package only
after every record and global uniqueness check succeeds. No partially validated
package is exposed. These checks do not make the package or source digest an
authenticated artifact.

## Version and migration policy

Authored schema version, package schema version, and state schema version are
separate contracts, each currently `1`. The two magic strings remain distinct.
Current readers reject unsupported versions, unknown authored keys, nonzero
reserved fields, and invalid/trailing data. No automatic migration or fallback
parser is implemented.

An incompatible authored or wire-format change requires a documented version
decision and updated readers/writers with explicit acceptance or rejection
tests. A content change that breaks saved-value meaning requires a new baseline
or a separately implemented migration with a defined old-ID-to-new-ID mapping
and value policy. Preserve old input until migration has been validated; do not
silently reinterpret an old version or reuse its baseline to bypass checks.
The product SHA-256 can change on an ordinary compatible rebuild while stable
IDs and the baseline remain meaningful.

Changes to canonicalization or versioned encodings must update the documented
contract and golden evidence deliberately. Current coverage is in the
[standard-library cooker tests](../../tests/test_cooker.py),
[native state tests](../../tests/state_tests.cpp), and
[fresh-process headless tests](../../tests/test_headless.py). Passing these
fixture checks establishes the documented bounded behavior, not production
asset import, asynchronous storage, authentication, or durable save recovery.
