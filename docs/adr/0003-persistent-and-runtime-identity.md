# ADR 0003: Separate baseline-scoped authored IDs from runtime generations

**Status:** Accepted for the foundation.

## Context

An authored object can outlive every resident instance used to represent it.
Saving a memory address or recycled slot would confuse persistent world state
with residency. Rebuilding content also must not accidentally create a new
persistent object. See [research identity and ownership](../research/open-world-blueprint.md#identity-and-ownership).

## Choice

Use a nonzero **64-bit authored persistent ID**, unique within its world baseline.
The identity is conceptually `(baseline_id, persistent_id)`. This foundation does
not use 128-bit UUIDs and does not claim globally unique IDs across independently
authored worlds. The cooker and state owner validate uniqueness; the ID value
type does not allocate identities.

Use a separate `RuntimeHandle` containing a one-based 32-bit slot index and a
nonzero 64-bit generation. Handles belong to one `GenerationPool` lifetime and
must not be exchanged across pools or serialized. Releasing a slot invalidates
the handle. Generation exhaustion retires the slot permanently instead of
wrapping a stale reference back into validity.

Keep the world baseline, authored identity, canonical-source digest, cooked
product revision, and streaming request generation distinct. A request token
also carries region ID, content revision, and coordinate epoch; every field must
match before publication. The present revision and epoch values are caller labels,
not a content dependency resolver or an origin-shifting implementation.

## Alternatives considered

- **128-bit UUIDs everywhere:** can support decentralized authoring but require an
  allocation/import policy and format change the small fixture does not need.
- **Pointers or slot indices as persistent IDs:** fail after process restart or
  slot reuse.
- **Content hashes as object IDs:** couple an object's identity to every asset edit.

## Consequences

The format is compact and deterministic, while save restore rejects incompatible
baselines and ID sets. Merging independently authored worlds needs an explicit
collision/remapping policy. Handles provide stale-reference detection, not pool
provenance, concurrent access control, or network authority.

Contracts: [identity](../../engine/core/include/ge/core/identity.hpp),
[request tokens](../../engine/world/include/ge/world/streaming.hpp), and
[persistent state](../../engine/world/include/ge/world/state.hpp).
Checks: [core](../../tests/core_tests.cpp), [streaming](../../tests/streaming_tests.cpp),
and [state](../../tests/state_tests.cpp) suites.

## Revisit trigger

Revisit ID allocation and encoding before collaborative world merging, external
content imports, distributed creation, or networking requires a wider uniqueness
scope. Preserve migrations and stable world identity when changing the format.
