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
disable leak detection. Hosted Linux, Windows and macOS results, relocatable package
checks, additional hardening reviews and final publication evidence will be recorded
at the final foundation checkpoint below.

## Tooling and distribution checkpoint

The unified `python3 tools/ge.py check --jobs 2` command passed all seven registered
CTest suites, including repository-policy checks and their 14 isolated regression
tests. `python3 tools/ge.py package --jobs 2` passed the Release suite, installed and
physically relocated the distribution, ran its host from an unrelated directory,
and compiled/executed a new consumer using all five public headers and all three
exported module targets. Both ZIP and TGZ archives and their SHA-256 files were
produced. Debug and Release relocation checks passed locally. Hosted results remain
a separate gate. Archive bit-for-bit reproducibility has not been claimed.

## Final foundation checkpoint

Pending the remaining setup waves and publishing validation. This section must be
updated with observed results before the completed setup is reported.
