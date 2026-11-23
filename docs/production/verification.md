# Foundation verification

This record distinguishes executed checks from configured coverage. It concerns the
headless foundation and does not qualify a renderer, physical collision, asynchronous
asset streaming, frame rate, or production crash durability.

## First integrated checkpoint — 23 November 2026

Coordinator verification used Linux x86-64, GCC 14.2.0, CMake 4.4.5, Ninja and
Python 3.12.14. Debug, Release and ASan/UBSan builds configured and compiled with
the repository's strict warnings-as-errors policy. Each passed all five CTest suites:
core, streaming, state, cooker and headless integration.

At this checkpoint the native suites contained 25 named cases and 1,776 active checks;
the Python suites contained 34 unittest methods. The integration suite starts a new
process to restore persisted state and parses saved records independently.

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
cmake --preset asan
cmake --build --preset asan --parallel 2
```

The local traced execution environment prevents LeakSanitizer from operating. The
ASan/UBSan test run therefore used this explicit environment-specific command:

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir out/build/asan --output-on-failure --no-tests=error
```

This does not claim local leak checking. The committed sanitizer CI job does not
disable leak detection. Hosted results, relocatable package checks, subsequent hardening, and exact
published-source evidence are recorded in the final foundation checkpoint below.

## Tooling and distribution checkpoint

The unified `python3 tools/ge.py check --jobs 2` command passed all seven registered
CTest suites, including repository-policy checks and their 14 isolated regression
tests. `python3 tools/ge.py package --jobs 2` passed the Release suite, installed and
physically relocated the distribution, ran its host from an unrelated directory,
and compiled/executed a new consumer using all five public headers and all three
exported module targets. Both ZIP and TGZ archives and their SHA-256 files were
produced. Debug and Release relocation checks passed locally. Hosted results remain
a separate gate. Archive bit-for-bit reproducibility has not been claimed.

## Final foundation checkpoint — 23 November 2026

**Accepted implementation:** [`1694f3c87f52`](https://github.com/baabi-boo/Game-Engine/commit/1694f3c87f5227842d157d5becc718b2bba03d85). The coordinator
integrated **60 distinct agent assignments in ten thematic waves**, recorded in
[the implementation and review register](implementation-register.md). This ledger
is a documentation follow-up to that implementation revision. The structured
[verification snapshot](verification.json) records the same results and hashes.

### Source identity and executed gates

The full Debug, Release and ASan/UBSan gate ran on the prepared working tree before
publication. Those manifests correctly recorded base commit `d4175381616a57588fdebb93b7f9ba0ea8cc3c9a`
with `dirty=true`. The independent source export and final source comparison found
no differences in implementation, build or test inputs; subsequent changes were
README/navigation and diagnostics documentation plus the new assignment register.

After publishing `1694f3c87f52`, the coordinator fetched it, confirmed a clean
checkout, and ran this gate again from the published revision:

```sh
python3 tools/ge.py package --preset release --jobs 2
```

The command passed repository checks, configuration, and a Release build using
up-to-date native targets and a refreshed manifest. It then passed **all 12 CTest
suites**, moved-install manifest/hash checks, installed inspection
and lifecycle execution, a new external SDK consumer, and ZIP/TGZ creation.
The resulting installed manifest records the exact accepted commit with
**`dirty=false`**. The checkout remained clean throughout this release run.

| Local gate | Executed result |
| --- | --- |
| Debug, strict warnings | 12/12 suites; zero failed or skipped suites |
| Release, strict warnings | 12/12 suites; zero failed or skipped suites |
| ASan + UBSan, strict warnings | 12/12 suites; zero failed or skipped suites; local leak detection disabled as explained above |
| Clean published Release package | 12/12 suites, manifest agreement, relocated runtime/SDK, and archive sidecars passed |
| Fresh source export, same host | 88 exported source files; fresh Release configure/build and 12/12 suites; relocated SDK passed |

The host was Linux x86-64 with GNU C++ 14.2.0, CMake 4.4.5,
Ninja 1.13.2.git.kitware.jobserver-pipe-1, and Python 3.12.14. The clean export had
no Git metadata or previous build products, and its manifest correctly reported
unknown source identity. It was executed on the same host. Separately observed hosted builds are recorded
below; the export check itself is not second-machine reproduction. Local testing did not
execute CMake 3.25 itself; minimum-version review checked the constructs against
local CMake documentation.

### Retained regression coverage

Each complete local configuration executed these native cases:

| Native suite | Named cases | Check evaluations |
| --- | --- | --- |
| Core | 9 | 350 |
| Content | 3 | 299 |
| Streaming | 10 | 622 |
| State | 6 | 807 |
| Linux state I/O faults | 4 | 207 |
| **Total** | **32** | **2,285** |

Release builds define `NDEBUG`; the test harness still evaluates its checks and
returns failure when a case fails. Evaluation totals include setup and loop checks,
not 2,285 independent scenarios. The six Python unittest suites executed **96 test
methods**: cooker 25, headless 12, native package 27, repository policy 14, build
manifest 13, and tool boundaries 5. The repository scan is an additional CTest suite.

Coverage includes strict/valid-CRC malformed inputs, maximum counts, golden field
decoding, FIFO/NUL rejection, native Unicode paths, failing stdout, stale handles and
tokens, reservation lifetime, baseline/sequence compatibility, and partial-write,
flush, close and replacement failures. The latter retain old state and allow retry;
they do not establish process-termination or power-loss durability. A save can be
committed before a later stdout failure, as documented and exercised.

An independent authoring exercise changed ID 1001's scalar to 143 using only JSON.
The first run saved 144 at sequence 2; a fresh process restored it and saved 145 at
sequence 4. An intervening invalid duplicate ID was rejected, preserved the last
good package, and produced byte-identical output after correction and recooking.

### Accepted Release artifacts

The moved installation used spaces, Greek and Chinese characters in its prefix.
The installed host completed all **30 default lifecycle checks** with zero final
modeled reservations. A fresh external consumer compiled and executed using all
**five public headers** and `ge::core`, `ge::content`, and `ge::world`.

The archive audit found matching ZIP/TGZ payloads: **19 regular files**, including
runtime, fixtures, headers, libraries, exports, manifest and required documentation.
Executable permissions were 0755; no traversal, absolute member path, special file,
external link or source/build-output junk was found. Clean published artifacts
passed their regenerated SHA-256 sidecars.

| Accepted file | Bytes | SHA-256 |
| --- | --- | --- |
| `GameEngine-0.1.0-Linux-x86_64.tar.gz` | 139,289 | `216cac7cf6e3390e512cb97d0a9d3128a6dce1bd932ca9f13c5f26841bf26192` |
| `GameEngine-0.1.0-Linux-x86_64.zip` | 149,757 | `f545e5f983c4e7b76f9dee30fde0dca2ad032f7070cbe516abfc96624e7fc6be` |

The Release executable is 137,200 bytes,
SHA-256 `a38f80bfb47437fc9da4dcd28adcdf1a7667ca870065ba30fc42efc1d8a865d4`.
The 252-byte cooked fixture is
`c40f4dd023f8ca5f4d57034abdfff73bdb22145a39694a56df3c4155ce8efb5d`.
These hashes identify this observed build. They do not authenticate a producer or
establish bit-for-bit executable/archive reproducibility.

### Hosted results are revision-specific

[First-checkpoint run 45165611612](https://github.com/baabi-boo/Game-Engine/actions/runs/45165611612)
for `d4175381616a57588fdebb93b7f9ba0ea8cc3c9a` completed with overall **failure**:

| Hosted job | Observed conclusion |
| --- | --- |
| Repository policy | Success |
| GCC Release, including relocation and archives | Success |
| GCC ASan/UBSan | Success |
| MSVC Release, including relocation and archives | Success |
| AppleClang Release / macOS | Cancelled with zero executed steps |

No macOS cancellation cause was established; no job log was available. These
successful jobs qualify only that earlier checkpoint. They do not verify the
subsequent hardening or establish macOS execution.

[Implementation run 45173950724](https://github.com/baabi-boo/Game-Engine/actions/runs/45173950724)
for accepted commit `1694f3c87f5227842d157d5becc718b2bba03d85` subsequently produced
these observed job results on 23 November 2026:

| Hosted job for the accepted implementation | Observed result |
| --- | --- |
| [Repository policy](https://github.com/baabi-boo/Game-Engine/actions/runs/45173950724/job/137690251161) | Success |
| [GCC 13.3 Release](https://github.com/baabi-boo/Game-Engine/actions/runs/45173950724/job/137690251293) | 12/12 suites; relocated runtime/SDK and archives passed |
| [GCC 13.3 ASan/UBSan](https://github.com/baabi-boo/Game-Engine/actions/runs/45173950724/job/137690250668) | 12/12 suites passed; the local leak-disabling override was not used |
| [MSVC 19.44 Release](https://github.com/baabi-boo/Game-Engine/actions/runs/45173950724/job/137690251142) | 11/11 applicable suites; relocated runtime/SDK and archives passed |
| [AppleClang Release / macOS](https://github.com/baabi-boo/Game-Engine/actions/runs/45173950724/job/137690251015) | Queued; no executed steps observed |

Windows omits the explicitly Linux-qualified linker fault-injection suite; individual
POSIX-only regressions remain conditional on their platform requirements. Its job
log confirms the moved Unicode/space-containing installation, all 30 host lifecycle
checks, a freshly compiled SDK consumer using all five headers and three targets,
and an installed manifest naming the accepted commit with `dirty=false`.

These observed Linux and Windows successes qualify the accepted implementation's
hosted gates. macOS remains unverified, and no overall successful workflow is claimed.
Subsequent macOS outcomes and runs triggered by documentation follow-ups are visible
in the [CI workflow](https://github.com/baabi-boo/Game-Engine/actions/workflows/ci.yml).

### Evidence locations and remaining scope

The committed structured snapshot retains source identity, gate results and hashes.
Full local logs, JUnit files, manifests and artifacts were retained under
`out/evidence/foundation-x2cat5qm`, `out/evidence/clean-release-o_d53gvd`, and
`out/w09-a54`. These paths belong to the verification workspace, are ignored by
Git, and are not downloadable files in a clone. Follow the
[evidence-preservation instructions](../development/packaging.md#retain-evidence-and-verify-archive-checksums)
to retain a new run and preserve hosted artifacts before their 14-day expiry.

This acceptance concerns the bounded synchronous foundation. Rendering, physical
collision, asynchronous asset I/O, actual resource eviction, frame-rate targets,
crash durability, stable binary ABI and production qualification remain outside
these results. The [first playable plan](first-playable.md) and
[risk register](risk-register.md) define the next evidence required. Project license
selection, any branch-enforcement settings, and a confirmed confidential reporting
channel remain owner decisions documented in the project policies.
