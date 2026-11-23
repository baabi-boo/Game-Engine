# Native package golden fixture

`package-v1.hex` is an original, frozen `GEPACK01` version 1 package represented
as whitespace-separated hexadecimal bytes. It contains one region and one
entity. It was constructed independently from the documented
[wire layout](../../docs/development/content-pipeline.md#gepack01-binary-layout)
using only Python's standard-library `struct` and `zlib`; no production cooker
code, external source, or external asset was used.

The direct C++ [content tests](../content_tests.cpp) read these static bytes,
write an isolated temporary binary, and call `ge::read_package`. They compare
every returned field, including all coordinates and the complete source digest,
with fixed expectations. The tests also decode the ordinary cooked
`assets/fixtures/district.world.json` fixture and check all three regions and
four entities. All checks use `test_support.hpp` and remain active with `NDEBUG`.

## Frozen values

| Property | Value |
| --- | --- |
| Binary size | `120` bytes: 64-byte header, 24-byte region, 28-byte entity, 4-byte CRC32 |
| Schema / reserved / region count / entity count | `1` / `0` / `1` / `1` |
| Baseline ID | `0xFEDCBA9876543210` |
| Source digest | Exactly the 32 bytes `00 01 02 ... 1f` |
| Region ID and entity owner | `0x89ABCDEF` |
| Readiness mask | `13` (visual, navigation, logic) |
| Region budget | `0x8877665544332211` |
| Region first entity / count | `0` / `1` |
| Entity ID | `0xF1E2D3C4B5A69788` |
| Initial scalar | `-2147483648` (`INT32_MIN`) |
| Position `[x, y, z]`, millimetres | `[2147483647, 0, -123456789]` |
| CRC32 of bytes 0–115 | `0xB7FDD399` (`3086865305`) |
| CRC32 bytes at offsets 116–119 | `99 d3 fd b7` |
| SHA-256 of the complete binary | `80063659873f09df4c2aa8bb8cd5387592531374046eb36e015d54dda5679a5d` |

The high bits of the 64-bit IDs and budget and the 32-bit region ID exercise
unsigned decoding. The signed scalar and distinct coordinates exercise
two's-complement decoding, coordinate order, and the signed limits. The digest
is deliberately a synthetic byte pattern, not the SHA-256 of an authored JSON
file. Version 1 carries that digest without authenticating or recomputing it.

## Creation record and compatibility policy

The following one-time construction records the independent origin of the
fixture. It is documentation, not a build or test generation step:

```python
import struct
import zlib

payload = (
    struct.pack("<8sIIIIQ32s", b"GEPACK01", 1, 1, 1, 0,
                0xFEDCBA9876543210, bytes(range(32)))
    + struct.pack("<IIQII", 0x89ABCDEF, 13, 0x8877665544332211, 0, 1)
    + struct.pack("<QIiiii", 0xF1E2D3C4B5A69788, 0x89ABCDEF,
                  -2147483648, 2147483647, 0, -123456789)
)
package = payload + struct.pack("<I", zlib.crc32(payload))
```

Keep this v1 fixture and its expectations fixed when the cooker or reader is
refactored. Do not regenerate the golden from the implementation under test to
make a failure pass. A new format version needs a separate versioned fixture
and an explicit compatibility decision: retain v1 decoding, test a migration,
or test deliberate v1 rejection. Preserve these bytes as evidence of the old
format; changing the existing fixture is not a migration.

The test executable takes both paths explicitly. Pass absolute paths to run it
from any working directory; this relative-path example starts at the repository
root:

```sh
ge_content_tests --golden tests/fixtures/package-v1.hex --fixture out/build/dev/assets/district.gepack
```
