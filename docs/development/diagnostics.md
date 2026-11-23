# Headless diagnostics

The current executable reports a synchronous policy fixture. Readiness signals
and byte reservations are symbolic; they establish no rendering, collision,
asynchronous I/O, frame-time, or allocated-memory result. The implementation is
[`apps/headless/main.cpp`](../../apps/headless/main.cpp), with policy semantics in
the [streaming API](../../engine/world/include/ge/world/streaming.hpp).

## Capture a report

Run from the repository root. Build through the supported driver, then invoke
the executable directly to keep build messages out of captured JSON. On Windows
use `py -3` as needed and append `.exe` to direct executable commands. The Python
capture selects the executable name automatically.

```sh
python3 tools/ge.py build --preset dev
python3 -c "from pathlib import Path; Path('out/diagnostics').mkdir(parents=True, exist_ok=True)"
out/build/dev/bin/ge_headless --package out/build/dev/assets/district.gepack --inspect
python3 -c "from pathlib import Path; import os, subprocess; host = Path('out/build/dev/bin') / ('ge_headless.exe' if os.name == 'nt' else 'ge_headless'); report = subprocess.run([str(host), '--package', 'out/build/dev/assets/district.gepack'], check=True, stdout=subprocess.PIPE); Path('out/diagnostics/report.json').write_bytes(report.stdout)"
python3 -m json.tool out/diagnostics/report.json
```

The capture keeps stdout as bytes, avoiding shell-dependent text conversion, and
checks the host's exit status before writing the report. Check the capture command's
exit status before parsing its output; formatting JSON does not check lifecycle
correctness. A successful data command prints one JSON
object. `--inspect` validates the package and reports metadata plus `regions`
(`id`, readiness mask, reservation cost, first entity index, entity count). It
does not run the lifecycle and accepts no budget, restore, or save options.

The optional [lifecycle report schema](../../schemas/headless-report-v1.schema.json)
describes the current required fields and types, allowing additional object
fields. It covers `mode: "headless-policy-fixture"`, not inspection or error
output. It is documentation for consumers, **not an authoritative validation
engine**; the build downloads no schema validator. Critically, report
`schema_version: 1` is the **package format version**, not a separately versioned
diagnostics protocol. Preserve full 64-bit integers when decoding reports.

## Read the result

| Fields | Interpretation |
| --- | --- |
| `mode`, `status` | A completed lifecycle emits `headless-policy-fixture` and `ok`. A failed required check throws before the report is returned. |
| `checks` | Named booleans accumulated across repeated operations. Every emitted check is true on success. Names vary: partial readiness, neighbor probes, and save checks depend on the package/options. Missing optional checks do not demonstrate a failure or a capability. |
| `events` | Ordered snapshots after recorded policy operations, not timestamps or a complete log of every state/save operation. `accepted: false` is often the successful outcome of a rejection probe. |
| `visited_regions`, `activation_order` | Sorted distinct activated region IDs versus chronological activations. The scenario visits every region and then reloads the first. |
| `persistent_id`, scalar values | The first entity in the first package region is mutated. `initial_value` is authored; `value_before_mutation` includes any restore; `final_value` normally increments it, or decrements at `INT32_MAX`. `restored_value` is null without `--restore-state`. |
| `baseline_id`, `source_sha256` | Explicit save-compatibility baseline versus the carried canonical-source digest. Neither is a runtime request generation. |

The unedited district at the default capacity of 96 reports three regions, four
entities, `visited_regions: [1,2,3]`, `activation_order: [1,2,3,1]`, three budget
denials, peak reservations of 64, and final reservations of zero. Its first
entity is `1001`; a fresh run changes its scalar from 0 to 1. These are fixture
expectations, not universal values for other packages.

`source_sha256` hashes canonical authored meaning, not the raw JSON file or the
complete package bytes. Native loading carries that digest through without
recomputing it from source. Restore instead requires the same baseline and exact
known persistent ID set, so a recook can change the source digest while still
accepting a save. See [content identity and formats](content-pipeline.md).

## Follow tokens, readiness, and reservations

Each event includes its attempted `token` (or null if no token was issued), the
region's resulting `state`, its current masks, and global `memory`. A stale-token
event therefore carries the old token alongside the current request's state;
it does not say the stale token owns that state. An acknowledgement retains its
attempted token in the event while reporting `Unloaded` and zero masks.

Tokens contain `region_id`, `generation`, `content_revision`, and
`coordinate_epoch`. The policy compares all four. Successful begins issue new
generations across the policy instance; rejection does not consume one.
Generations do not wrap and tokens are meaningful only to their issuing policy.
The fixture supplies revision and epoch labels of 1; these do not implement
origin shifting. Runtime handle generations are separate and appear only in
named checks, not as handle objects in the report.

Readiness bits are `visual=1`, `collision=2`, `navigation=4`, and `logic=8`.
Activation requires `(ready_mask & required_mask) == required_mask` and a `Ready`
state. In the reference first region, required mask 11 and partial mask 10 leave
the request `Loading`; `incomplete_activation_rejected` is expected.

| State | Charged bucket | Exit from that bucket |
| --- | --- | --- |
| `Loading`, `Ready` | `pending_bytes` | Activate to resident, or cancel to retiring. |
| `Active` | `resident_bytes` | Retire to retiring. |
| `Retiring` | `retiring_bytes` | Matching retirement acknowledgement releases the reservation. |
| `Unloaded` | None | A successful begin reserves pending bytes. |

For every event, `reserved_bytes = pending_bytes + resident_bytes + retiring_bytes`
and `available_bytes = capacity_bytes - reserved_bytes`. Capacity equals the
run's `budget_bytes`; reservations never exceed it. Cancellation alone frees
nothing. `peak_reserved_bytes` is the maximum recorded reservation total and
`final_reserved_bytes` is zero on successful completion. These numbers exclude
actual heap, process, storage, and GPU usage.

`budget_denials` counts only neighbor capacity probes, not every rejected event.
The reference neighbor needs 64 while only 32 remain, so probes during pending,
resident, and retiring phases are all denied. This is expected pressure:
`budget_pressure_observed` means `budget_denials != 0`, not a failed lifecycle.
With capacity 128 the neighbor probes are admitted, canceled, and acknowledged;
denials become zero and the recorded peak becomes 128:

```sh
out/build/dev/bin/ge_headless --package out/build/dev/assets/district.gepack --budget-bytes 128
```

## Saves and failure handling

Ordering is package validation, budget-fit preflight, optional restore, mutation
and complete retirement, in-memory capture/restore verification, optional save
and read-back verification, then report output. `restored_sequence` is zero when
no file was restored. Capture advances the sequence by one; optional saving
advances it once more. Thus a fresh saved run reports snapshot sequence 1 and
state sequence 2; restoring that save and saving again reports 3 and 4.

Choose an unused save path for the first command, with an existing parent:

```sh
out/build/dev/bin/ge_headless --package out/build/dev/assets/district.gepack --save-state out/diagnostics/session.gestate
out/build/dev/bin/ge_headless --package out/build/dev/assets/district.gepack --restore-state out/diagnostics/session.gestate --save-state out/diagnostics/session.gestate
```

A save sequence must exceed both live state and any existing compatible
destination; rerunning the first command alone against its sequence-2 save
fails. Restore it first or choose a new destination. Invalid or incompatible
destinations are not overwritten. Publication uses a checked sibling temporary
file and replacement; there is no promised power-loss durability. Publication
precedes read-back verification and stdout, so an error after publication does
not imply that no save was written.

| Native exit status | Meaning and next action |
| --- | --- |
| `0` | Data command completed; inspect `mode` before consuming the report. `--help` and `--version` also succeed but print text. |
| `1` | Package/save validation or I/O error, insufficient capacity for an individual region, failed lifecycle check, or output error. Read the `ge_headless:` stderr message; inspect/recook the input, restore a compatible save, or correct the path/capacity as indicated. |
| `2` | CLI error: missing/unknown/duplicate option, prohibited option combination, or invalid budget syntax/range. Use `ge_headless --help` alone. |

For example, `--budget-bytes 0` is a CLI error (2), while `--budget-bytes 63`
is valid syntax but cannot fit a reference 64-byte region (1). Invalid package
or save bytes are data errors (1). Such failures emit stderr rather than an error
JSON object. An output-write failure can leave incomplete stdout; inspect the
exit status even if a report appears present. For a targeted reproduction use:

```sh
python3 tests/test_headless.py --binary out/build/dev/bin/ge_headless --fixture out/build/dev/assets/district.gepack HeadlessTests.test_default_lifecycle_mutates_retires_and_restores -v
```

## Future observability requirements

Before asynchronous streaming or performance claims, add explicit timestamps
and correlation for I/O, decode and publication, measured resource ownership
and release completion, and subsystem readiness provenance. Frame/GPU timings,
queue latency, percentiles, and real memory high-water marks require those
systems and suitable measurement scenarios. None is supplied by today's ordered
symbolic events; keep any future report-version contract distinct from the
package's existing `schema_version`.
