# ADR 0005: Version deterministic fixture packages and logical snapshots

**Status:** Accepted for the bounded fixture formats.

## Context

Independent editing and repeatable restore need a stable boundary between source
content, cooked products, and live state. Malformed or incompatible data must not
partially replace valid data. The first format should prove this boundary without
pretending to be a complete asset database or production save service. See the
research [asset pipeline](../research/open-world-blueprint.md#asset-production-and-editor)
and [persistence](../research/open-world-blueprint.md#missions-and-persistence) goals.

## Choice

Author strict JSON and cook bounded, little-endian `GEPACK01` version 1 packages.
Canonical ordering makes semantically equivalent source produce identical bytes
for the same cooker implementation and format. The source SHA-256 records
canonical source provenance; the cooker's reported package SHA-256 identifies the
product. Record the cooker revision and declared inputs with delivery evidence;
the package header does not embed a complete build manifest.

Validate field names, types, ranges, counts, identity uniqueness, lengths, and
reserved/version fields. Cook into a temporary file and replace the destination
only after the complete product is ready. Native loading returns only a fully
validated package. CRC32 detects accidental corruption; it does not authenticate
either content or saves, and the native reader does not recompute the source hash.

Persist `GESTATE1` version 1 as an immutable logical snapshot: baseline, monotonic
sequence, and sorted `(persistent_id, scalar_value)` records. Restore validates
the entire snapshot, baseline, and exact known ID set before replacing live values.
Synchronous saves require a newer sequence and preserve the destination on rejected
publication. The caller serializes access to the state and save path.

## Alternatives considered

- **Serialize the live object graph:** captures transient handles and resource
  lifetimes that should be reconstructed.
- **Unversioned JSON for runtime products:** postpones compatibility rules and
  leaves canonicalization and bounds ambiguous.
- **General database/asset service immediately:** introduces transactions, schema,
  and operational scope before the fixture establishes its requirements.

## Consequences

Golden bytes, malformed input, failure preservation, and fresh-process restore are
testable. Schema migration, arbitrary component persistence, multiwriter saves,
and ordered asynchronous publication remain future work. Filesystem replacement
alone provides no universal crash or power-loss durability guarantee; the native
save protocol does not sync the file and directory for that guarantee.

Contracts/checks: [cooker](../../tools/cook.py), [package reader](../../engine/content/include/ge/content/package.hpp),
[state API](../../engine/world/include/ge/world/state.hpp), [cooker tests](../../tests/test_cooker.py),
[state tests](../../tests/state_tests.cpp), and [process tests](../../tests/test_headless.py).

## Revisit trigger

Revise the formats when real assets, incremental cooking, content migration,
mission state, or asynchronous save ordering require additional semantics. Add
versioned migration/rejection fixtures and explicit recovery tests before adopting
the new schema or durability claim.
