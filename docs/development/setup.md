# Developer setup

This guide builds the composed headless runtime, the retained lifecycle fixture,
and their contract and integration tests. Read the
[architecture overview](../architecture/overview.md) for the implemented scope.
The host/toolchain combinations below are development targets. See
[production verification](../production/verification.md) for historical results and
the [stabilization review](../production/stabilization-2026-11-28.md) and
[task ledger](../../project/agent-state.json) for source-specific review receipts.
A CI job definition alone does not establish that its platform passed.

## Provision the tools

Install the following tools before building, and make them available in the same
terminal. The repository does not install them or download dependencies.

| Requirement | What must be available |
| --- | --- |
| C++20 compiler | Compiler, compatible standard library, linker, and platform development headers/SDK. |
| CMake 3.25+ | `cmake`, `ctest`, and `cpack` on `PATH`. |
| Ninja | `ninja` on `PATH` for every checked-in preset and the CLI's doctor probe. |
| Python 3.11+ | `python3`, or an explicitly selected supported interpreter. Tools use the Python standard library; no pip packages are required. |

| Host intent | Compiler environment |
| --- | --- |
| Linux | GCC with its C++ development libraries; select `g++` before first configure, for example with `CXX=g++`. |
| macOS | AppleClang from Xcode or its command line tools, with the matching SDK; select `clang++` before first configure. |
| Windows | Visual Studio 2022/MSVC with the C++ workload and Windows SDK. Run the Ninja/CLI workflow from a Visual Studio Developer PowerShell or developer command prompt for the desired architecture. |

Commands below assume the repository root. On Windows, replace `python3` with
`py -3` if appropriate; check that it selects Python 3.11 or newer. CMake may find
a different Python interpreter from the one running the CLI; use an explicit
`-D Python3_EXECUTABLE=...` when selecting it matters.

“Offline build” means that configure, compile, cook, test, and package do not
fetch dependencies once these tools and the checkout are present. It does not
provision compilers/SDKs offline or promise identical native binaries across
different toolchains. See [ADR 0002](../adr/0002-cpp20-offline-targets.md).

## First run

```sh
python3 tools/ge.py doctor
python3 tools/ge.py check
python3 tools/ge.py run --inspect
python3 tools/ge.py run
out/build/dev/bin/ge_runtime --ticks 3 --step 0.125
```

`doctor` checks tool discovery and compiles/links a temporary C++20 probe under
`out/doctor/`. It uses the active environment, including `CXX`, `CXXFLAGS`, and
`CMAKE_TOOLCHAIN_FILE`; it does not rewrite your compiler choice or inspect an
existing preset's compiler cache. `check` validates the actual repository build.

`tools/ge.py run` launches `ge_headless`, which prints package metadata or lifecycle
JSON. Its counters describe modeled reservations and synchronous policy transitions,
not measured asset
memory or graphics performance. `ge_runtime` runs the installed engine services
and emits a separate JSON report. Use `ge_runtime.exe` on Windows. The driver
also prints build output and commands.

| CLI command | Behavior |
| --- | --- |
| `doctor` | Check tools and compile/link the C++20 probe. |
| `configure` | Configure the chosen preset, applying any explicit `-D` definitions. |
| `build` | Build; configure first only if the preset's cache does not exist. |
| `test` | Run CTest against an existing build; does not build first. Write `out/logs/ctest-PRESET.xml`. |
| `check` | Run repository policy checks, configure, build, and run CTest. |
| `run` | Build and launch the headless fixture; accepts a custom package, inspection, budget, and save/restore options. |
| `cook` | Validate authored JSON and publish a cooked package; requires a source and `--output`. |
| `package` | Configure, build, test, check installed runtime/SDK relocation, and create CPack archives. |

Use `python3 tools/ge.py --help` and, for example,
`python3 tools/ge.py run --help` for the complete options. Commands that build or
test accept `--jobs N` (or `-j N`), defaulting to two jobs. Preset-aware commands
default to `dev`, except `package`, which defaults to `release`:

```sh
python3 tools/ge.py check --preset release --jobs 2
python3 tools/ge.py package
```

See [quality gates](../quality-gates.md) for required acceptance evidence and
[packaging](packaging.md) for the installed runtime and SDK workflow.

### Composed runtime commands

After the normal build, inspect the commands registered by the actual modules:

```sh
out/build/dev/bin/ge_runtime --commands
out/build/dev/bin/ge_runtime --help
```

Use `--script PATH` for a UTF-8 command file, `--interactive` for standard-input
commands, and `--report PATH` to write the final JSON report. For example:

```sh
python3 -c "from pathlib import Path; Path('out/runtime.commands').write_text('mesh.create demo box 2\ntick\n', encoding='utf-8')"
out/build/dev/bin/ge_runtime --script out/runtime.commands --ticks 2 --step 0.125 --report out/runtime.json
```

This creates a box mesh and advances three frames: one from the script's `tick`
and two from `--ticks`. Runtime commands do not require the district `.gepack`.
Creating a mesh alone does not create a camera or a renderable scene node; use
the populated [runtime integration test](../../tests/runtime_integration_tests.cpp)
as a composition example. The host is headless and does not open a graphical window.

## Presets, overrides, and cached settings

The checked-in [presets](../../CMakePresets.json) use Ninja and separate build trees:

| Preset | Build directory | Build type | Sanitizers |
| --- | --- | --- | --- |
| `dev` | `out/build/dev` | Debug | Off |
| `release` | `out/build/release` | Release | Off |
| `asan` | `out/build/asan` | Debug | AddressSanitizer and UndefinedBehaviorSanitizer |

All three enable tests (`BUILD_TESTING`), strict warnings treated as errors
(`GE_STRICT_WARNINGS`), archive packaging (`GE_ENABLE_PACKAGING`), and compile
command export. `GE_ENABLE_SANITIZERS` needs a compatible GCC/Clang driver and
both sanitizer runtimes; the combined option rejects MSVC/clang-cl.

Only `configure`, `check`, and `package` accept repeated `-D NAME[:TYPE]=VALUE`
arguments. Explicit definitions take precedence for that configure invocation:

```sh
python3 tools/ge.py configure --preset dev -D CMAKE_BUILD_TYPE=RelWithDebInfo
python3 tools/ge.py build --preset dev
python3 tools/ge.py test --preset dev
```

`build`, `run`, and `test` reuse the existing cache. `configure`, `check`, and
`package` invoke CMake's preset again, which reapplies values declared by that
preset. Repeat an override when a later command reconfigures:

```sh
python3 tools/ge.py check --preset dev -D CMAKE_BUILD_TYPE=RelWithDebInfo
```

Cache values not declared by the preset retain CMake's normal behavior. Changing
`CXX` after the first configure does not replace a cached compiler. Inspect
`out/build/PRESET/CMakeCache.txt` when the active environment and build disagree;
use a fresh build tree when changing compiler, generator, architecture, or
toolchain. Keep experimental overrides separate from recorded acceptance runs.

### Paths and quoting

Quote each argument containing spaces, including an entire cache definition,
such as `-D "Python3_EXECUTABLE=/path with spaces/python3"`. That path is an
example to replace with your interpreter's real location.

Explicit CLI file arguments (`cook` input/output, `--package`, `--save-state`,
and `--restore-state`) are relative to the directory where you invoke the
driver. Its child commands run from the checkout root. `-D` values retain normal
CMake semantics rather than being rewritten as caller-relative file paths;
prefer absolute paths for toolchain and interpreter overrides.

```sh
python3 tools/ge.py cook assets/fixtures/district.world.json --output "out/content demo/district.gepack"
python3 tools/ge.py run --package "out/content demo/district.gepack" --inspect
```

The cooker creates output parent directories. Save-state destinations require an
existing parent directory. Keep authored JSON distinct from cooked binaries; see
the [edit, cook, save, and restart walkthrough](content-pipeline.md#copy-edit-cook-inspect-save-and-restart).

## Raw CMake workflows

The CLI is a convenience layer over standard CMake tools. The development stages
can be run separately:

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
```

`cmake --workflow --preset dev` runs those three stages together. The `release`
and `asan` workflow presets do the same for their configurations. To investigate
one existing suite, use, for example, `ctest --preset dev -R '^ge[.]state$'`.
CTest presets treat an empty test set as an error. Use the CLI `check` command
for its explicit repository-policy step and retained JUnit report.

### Windows: Visual Studio multi-configuration build

The CLI presets use Ninja. The [Windows CI definition](../../.github/workflows/ci.yml)
instead uses the Visual Studio 2022 generator, which locates the installed MSVC
toolchain without a separate developer-shell setup action. Its raw equivalent
is the following, run from PowerShell:

```powershell
cmake -S . -B out/build/msvc -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON -DGE_STRICT_WARNINGS=ON -DGE_ENABLE_SANITIZERS=OFF -DGE_ENABLE_PACKAGING=ON
cmake --build out/build/msvc --config Release
ctest --test-dir out/build/msvc -C Release --output-on-failure --no-tests=error
```

This generator chooses Debug/Release at build, test, and install time; do not use
`CMAKE_BUILD_TYPE` to choose its configuration. The Release executables are
`out/build/msvc/bin/Release/ge_headless.exe` and `ge_runtime.exe` in the same
directory. Keep this tree separate from the Ninja preset trees; `tools/ge.py` does
not drive arbitrary raw build directories.

### Linux sanitizer diagnostics

Use the normal sanitizer workflow on a compatible host:

```sh
cmake --workflow --preset asan
```

If LeakSanitizer reports that it cannot operate under tracing/ptrace, the
[local verification record](../production/verification.md) documents this restricted
environment workaround after configuring and building the `asan` tree:

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir out/build/asan --output-on-failure --no-tests=error
```

Use `--test-dir` for that workaround: the `asan` test preset supplies its own
`ASAN_OPTIONS`. Record the restriction with the result; this run does not check
leaks. The committed sanitizer CI configuration does not disable leak detection.

## Output layout and SDK use

| Path | Contents |
| --- | --- |
| `out/build/PRESET/` | CMake cache, build files, native libraries, tests, and `compile_commands.json`. |
| `out/build/PRESET/bin/ge_headless` | Ninja-built host; filename ends in `.exe` on Windows. |
| `out/build/PRESET/bin/ge_runtime` | Ninja-built composed runtime and command host; `.exe` on Windows. |
| `out/build/PRESET/assets/district.gepack` | Cooked reference fixture. |
| `out/build/PRESET/packages/` | ZIP/TGZ archives and SHA-256 sidecar files from CPack. |
| `out/install/PRESET/` | Preset's default destination for an explicit CMake install. |
| `out/logs/` | CLI CTest JUnit reports and retained diagnostics. |
| `out/doctor/` | Parent directory for temporary toolchain probes. |

An explicit Release install is:

```sh
cmake --install out/build/release --config Release --prefix out/install/release
```

Build that configuration first. The installed tree contains both executables in `bin/`,
the fixture in `share/game-engine/assets/`, public headers, static libraries, and
CMake package exports under the configured library directory. Consumers use
`find_package(GameEngine CONFIG REQUIRED)` and the targets `ge::core`,
`ge::content`, `ge::world`, and `ge::engine`, with the install prefix on
`CMAKE_PREFIX_PATH`. Link `ge::engine` for domain APIs; it brings the three
foundation libraries and declared thread dependency transitively.
Match compiler ABI, architecture, runtime, and configuration; an instrumented
SDK also requires compatible sanitizer runtimes. A moved install tree is not
automatically a portable binary SDK for another platform or toolchain.

Keep generated packages, saves, logs, and build/install trees under `out/`.
In-source CMake builds are rejected. Do not commit generated output or copy it
into source directories; commit authored inputs and the tools that reproduce it.

## Troubleshooting

| Symptom | Next action |
| --- | --- |
| Doctor cannot compile/link C++20, or the build uses the wrong compiler | Check the compiler's SDK/standard-library installation and `CXX`, `CXXFLAGS`, and `CMAKE_TOOLCHAIN_FILE`. On Windows reopen the correct Visual Studio developer shell. Compare `CMakeCache.txt` with the intended compiler; a changed environment does not replace a cached toolchain. Configure a fresh output tree. |
| `ninja` is missing | Install/provision Ninja, add it to `PATH`, reopen the terminal, and rerun `doctor`. The preset generator is Ninja even on Windows. The raw Visual Studio workflow is a separate option. |
| No tests found, or `BUILD_TESTING` is disabled | Run `python3 tools/ge.py configure --preset dev -D BUILD_TESTING=ON`, then build and test. `ctest --preset dev -N` lists registrations. For a raw Visual Studio tree, use its actual `--test-dir` and `-C Release`; do not test an unrelated or unconfigured directory. |
| JSON validation or cooked-package CRC/format error | Read the diagnostic against the strict [content contract](content-pipeline.md). Correct authored JSON and recook, then use `run --package PATH --inspect`. Do not edit the binary to bypass validation. A failed cook preserves a previous valid output, so check that you inspected the intended path. |
| Budget denial, or bytes remain charged after cancellation | Each region must fit the chosen `--budget-bytes N`. Pending, resident, and retiring reservations all count; a matching retirement acknowledgement releases a retiring reservation. The default fixture deliberately probes denials and retries while retiring. Inspect lifecycle events before interpreting those expected rejections as a failure. |
| Save rejected as not newer than its destination | A new process starts its own sequence unless it restores a save. Continue with `--restore-state PATH --save-state PATH`, or use a fresh destination. Ensure the parent exists and the baseline/ID set is compatible. Do not delete a valid save just to hide an ordering error. |

For a failed test, retain its output and `out/build/PRESET/Testing/Temporary/`
logs along with the source revision and toolchain. Follow the
[quality gates](../quality-gates.md) for acceptance, and record platform limitations
in [production verification](../production/verification.md).

## Editing conventions

[`.editorconfig`](../../.editorconfig) defines text conventions and
[`.clang-format`](../../.clang-format) describes the C++ style (four-space
indentation, attached braces, and a 100-column target). Use them with your editor
and match neighboring code. There is no enforced clang-format check in the
current gate; `tools/check_repo.py` performs repository policy/text/link checks,
and compiler warnings remain a separate build gate. See
[contributing](../../CONTRIBUTING.md) before publishing a change.
