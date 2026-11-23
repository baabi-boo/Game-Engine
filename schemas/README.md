# Authored world schema

[`world-v1.schema.json`](world-v1.schema.json) is a JSON Schema 2020-12 description
of the version 1 authoring objects consumed by [`tools/cook.py`](../tools/cook.py).
It records exact required keys, integer ranges, readiness names, and per-array
length limits. An editor may associate `*.world.json` with this schema. Do not add
a `$schema` key to a world file: the cooker rejects every unknown authored key.

The schema is documentation and optional editor support. The build and cooker
use Python's standard library and do not install a JSON Schema validator or fetch
the schema's declared meta-schema. Run the authoritative validation and cooking
step from the repository root:

```sh
python3 tools/cook.py assets/fixtures/district.world.json --output out/schema-check/district.gepack
```

The cooker creates missing output parents after source validation. A failure
before publication preserves an existing output. See the
[content pipeline](../docs/development/content-pipeline.md) for the complete
authoring contract, byte layouts, identity rules, and save/restart example.

## Checks beyond JSON Schema

Passing a generic schema validator is not sufficient to accept a world. The
cooker also checks the following:

| Rule | Why the schema alone is insufficient |
| --- | --- |
| No duplicate JSON object keys | A generic decoder may discard a repeated key before validation. The cooker rejects it while decoding. |
| Integer JSON number syntax only | JSON Schema's `integer` describes a mathematical value and may accept `1.0` or `1e0`. The cooker rejects decimal/exponent numeric tokens, even if their value is integral; booleans and numeric strings are also rejected. |
| Region IDs are unique | Equality of complete region objects is different from uniqueness of their `id` properties. |
| Entity IDs are globally unique | IDs cannot repeat within one region or across different regions. |
| At most 65,536 entities in the whole world | Each region array has its own schema limit; the cooker additionally sums all region counts. |
| Source is at most 8,388,608 bytes of UTF-8 JSON | Schema validation sees decoded data, not the source's encoding and byte length. |
| Cooked package is at most 4,194,304 bytes | The cooker validates the computed binary size before publication. |

The cooker also bounds integer-token parsing to at most 20 decimal digits,
excluding a leading minus sign, and rejects non-finite numbers. Its field bounds
then determine which integers are valid. Editors or validators must preserve
64-bit integer values exactly; converting all numbers to floating point can lose
precision before validation.

Ordering of authored regions, entities within a region, readiness names, and
object keys is unrestricted. The cooker canonicalizes those orders. The three
coordinates in `position_mm` retain their `[x, y, z]` meaning and order.

There is no schema migration in version 1. Unsupported versions and unknown keys
are errors. The binary `GEPACK01` and `GESTATE1` layouts are documented separately
in the content pipeline; this JSON Schema does not describe either binary file.
