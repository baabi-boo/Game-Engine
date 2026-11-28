# Packaging and relocation

The binary distribution contains `ge_headless` and `ge_runtime`, cooked and authored
district fixtures, all 70 current public headers, the static core/content/world/engine
libraries, CMake package exports, a build manifest, and distribution documentation.
This is a pre-alpha headless engine foundation. Packaging checks delivery and
relocation; it does not qualify every subsystem, native graphics, performance,
crash durability, a supported platform matrix, or a stable binary ABI. Read the
[architecture](../architecture/overview.md) for the scope and
[project policy](../project-policy.md) for the pending owner license decision.

## Build, verify and archive

From the repository root, with Python 3.11+, CMake 3.25+, Ninja and a C++20 compiler:

```sh
python3 tools/ge.py package --preset release --jobs 2
```

The driver configures, builds, runs CTest, executes the relocation gate and then
invokes CPack. With the default presets, ZIP/TGZ archives and SHA-256 files appear
under `out/build/release/packages/`. The archives contain the install tree under
one top-level directory. Archive names identify project version, system and CMake's
`CMAKE_SYSTEM_PROCESSOR`; they are not complete target-architecture, ABI or
configuration identifiers. Preserve the producer's toolchain, target and
configuration in the accompanying validation record.

The raw equivalent is:

```sh
cmake --preset release
cmake --build --preset release --config Release --parallel 2
ctest --preset release --build-config Release --output-on-failure --no-tests=error
python3 tools/package_smoke.py --build-dir out/build/release --config Release --jobs 2
cpack --preset release -C Release
```

The gate accepts an existing build directory. It does not build or repair that
input tree, overwrite the caller's installation, fetch dependencies, or create an
archive itself. It fails if the cache/configuration/tools or required install
artifacts are absent. Reconfigure and build after changing install rules before
running it directly. `--config` accepts `Debug`, `Release`, `RelWithDebInfo`, and
`MinSizeRel`, with `Release` as the default; `--jobs` is a positive integer with
default `2`. A single-configuration cache must match the requested configuration.
For a multi-configuration generator, that configuration must be available and built.

The normal default/ALL build refreshes the manifest after both executables, the
four static libraries and the cooked fixture are ready. Building only one target
can leave an old manifest behind; use the normal build command above before
installing or packaging. The gate checks
the installed manifest against the requested configuration, configured project
version, and actual moved files. It does not regenerate metadata to repair a mismatch.

Check a development build independently with:

```sh
python3 tools/package_smoke.py --build-dir out/build/dev --config Debug
```

On Windows, substitute `py -3` for `python3` when necessary. Ninja/MSVC needs the
same compiler and SDK environment used for the original build. A Visual Studio
generator reuses its cached generator, platform, toolset and instance, and does
not require `cl.exe` or a Developer PowerShell on the caller's PATH. The consumer
also reuses a cached compiler, build tool, runtime/architecture settings and
configuration flags when present. It does not import the engine source project or
its toolchain file. Cross-target execution and arbitrary custom toolchain setups
are outside this native-host gate; no Windows or macOS execution is inferred from
these command definitions.

## What the relocation gate proves

The standard-library script performs these checks in a private temporary directory:

1. Validate the GameEngine cache, requested configuration, CMake version, compiler
   and generator's build tool. Require the documented `bin/` and `share/` layout;
   reject absolute or parent-traversing install destinations used by the project.
2. Run `cmake --install` with the selected configuration and a private prefix.
   A caller's `DESTDIR` does not redirect this installation.
3. Physically rename that installation to a different prefix containing spaces,
   Greek `δ`, and Chinese `街区` (district).
   Check that the original stage is absent, installed links stay inside the moved
   prefix, and exported CMake files refer to no original stage, build or checkout.
4. Read the moved `share/game-engine/build-manifest.json`. Require its schema,
   project name/version and requested configuration to match the configured build.
   Recompute SHA-256 and byte sizes for all seven moved artifacts: both executables,
   the four static libraries and the cooked fixture. Compare their filenames and
   content metadata with the manifest. Validate and report its recorded Git
   commit/dirty state or explicit unknown source status.
5. Run the installed `bin/ge_headless` (`.exe` on Windows) from an unrelated
   working directory with the moved `share/game-engine/assets/district.gepack`.
   Parse both `--inspect` and lifecycle JSON. Check matching package metadata,
   all required lifecycle checks, visits to every region, a bounded peak modeled
   reservation and `final_reserved_bytes == 0`.
6. Run the installed `bin/ge_runtime` from that unrelated directory, with a command
   file and report path containing spaces and Unicode. Create a box mesh and advance
   three frames. Check its mesh metrics, frame-system execution, render graph and
   diagnostics. This is a bounded runtime smoke check, not a populated-world test.
7. Create and build a fresh external CMake project that finds `GameEngine` only
   under the moved prefix and requests all four components: core, content, world
   and engine. It links `ge::engine`, which supplies the foundation libraries
   transitively. The consumer uses seven representative public headers: clock,
   identity, runtime, package, mesh, state and streaming. It exercises fixture
   loading/restoration, readiness policy, compiled geometry APIs and a kernel tick.
   Execute this newly linked consumer with the installed fixture from the unrelated
   working directory. The normal source build separately compiles every public
   header in isolation; this consumer does not exercise every installed API.
8. Verify that the original stage remains absent and remove only the script's own
   temporary directory, including its disposable consumer source and build.

Commands use argument arrays without a shell. Spaces in the build, install and
consumer paths remain individual arguments. On Windows, both installed executables
and the generated SDK consumer use `wmain` and construct `std::filesystem::path` directly
from native UTF-16 arguments; MinGW builds select the Unicode CRT entry with
`-municode`. On POSIX, they retain native `char` path arguments. The moved Unicode
prefix exercises executable launch, fixture loading, CMake package discovery, and
the SDK consumer's path boundary on the host running the gate. If a redirected log
uses an encoding that cannot display these characters, the script escapes them in
diagnostics while preserving the native paths passed to commands.

Every failed command, missing artifact, invalid JSON result, manifest mismatch,
broken installed local documentation link, or failed consumer contract produces a
nonzero exit. Successful output identifies the configuration, manifest content
checks and recorded source status, fixture counts, lifecycle checks and SDK
consumer completion.

This gate checks relocation of installation paths and an independent source consumer
on the producing host. It does not erase the engine checkout or prove compatibility
with another OS, architecture, runtime, compiler version or ABI. Debug information
inside binaries can still record compilation paths; those are distinct from a
runtime asset dependency or an exported include/library path. The gate rejects
checkout/build paths in the consumer's inherited toolchain arguments instead of
using those paths to satisfy a missing SDK dependency. Its temporary outputs are
removed after the run; retain command logs separately when recording evidence.

## Manifest evidence and limits

The installed manifest records the producer's build-time source observation,
project version, platform/processor, compiler, configuration, strict-warning and
sanitizer settings, package/state format versions, and hashes and byte sizes for
both executables, four static libraries and the cooked fixture. Its source record
is either a complete Git commit with a boolean dirty flag, or `unknown` with null
commit/dirty fields and a reason such as missing
Git metadata or an unavailable Git executable. Unknown status is never reported as
a clean checkout. The gate reports this record; it does not compare it with the
checkout's later state or independently establish which source produced the binary.

Matching SHA-256 values establish agreement between the installed files and the
manifest. The manifest is unsigned metadata, so these checks do not authenticate
the producer, prove source provenance, or establish reproducible executable bytes.
The gate checks the recorded project version against the configured CMake cache;
that agreement alone is not an independent verification of the binary's version.
Use a fresh normal build after source changes and retain the manifest with the
corresponding build and validation logs.

## Retain evidence and verify archive checksums

Keep a separate evidence directory for each acceptance run. From the repository
root, this example creates an unused `out/evidence/release-*` directory, captures
the unified command's stdout/stderr, records its exit status, and copies the
default Release products after success:

```sh
python3 - <<'PY'
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

base = Path("out/evidence")
base.mkdir(parents=True, exist_ok=True)
saved = Path(tempfile.mkdtemp(prefix="release-", dir=base))
with (saved / "package.log").open("wb") as log:
    result = subprocess.run(
        [sys.executable, "tools/ge.py", "package", "--preset", "release", "--jobs", "2"],
        stdout=log, stderr=subprocess.STDOUT)
(saved / "exit-status.txt").write_text(str(result.returncode) + "\n", encoding="utf-8")
print(f"Evidence: {saved}; package exit status: {result.returncode}")
if result.returncode == 0:
    sources = [Path("out/logs/ctest-release.xml"),
               Path("out/build/release/build-manifest/Release/build-manifest.json")]
    sources.extend(p for p in Path("out/build/release/packages").iterdir()
                   if p.is_file() and p.name.endswith((".zip", ".tar.gz", ".sha256")))
    for source in sources:
        shutil.copy2(source, saved / source.name)
sys.exit(result.returncode)
PY
```

In POSIX shells, read `$?` immediately after the block. For PowerShell, replace
the opening `python3 - <<'PY'` with `@'` and the closing `PY` with `'@ | py -3 -`,
then read `$LASTEXITCODE` immediately. The wrapper returns the package status;
a copy failure also exits nonzero. `exit-status.txt` preserves the package result.
For failed runs retain the log and any known-current failure diagnostics; do not
accept products left by an earlier run. A reused package directory can contain
older versions, so identify the accepted filenames and hashes in
[production verification](../production/verification.md).

Copy evidence before later commands replace JUnit reports, manifests or archives.
Download hosted archive **and diagnostic** artifacts before CI's 14-day expiry,
preserve them in the delivery's retained location, and record that location with
the revision, host and results. An `out/` path alone is temporary local evidence.

CPack sidecars contain a SHA-256 digest, two spaces, and the sibling archive
filename. Before extraction, substitute the actual sidecar path below. This
Python 3.11 check streams the archive through `hashlib.file_digest`:

```sh
python3 - path/to/archive.zip.sha256 <<'PY'
from pathlib import Path
import hashlib
import sys

sidecar = Path(sys.argv[1])
expected, name = sidecar.read_text(encoding="utf-8").rstrip("\r\n").split("  ", 1)
if len(expected) != 64 or Path(name).name != name:
    raise SystemExit("Invalid checksum sidecar")
with (sidecar.parent / name).open("rb") as archive:
    actual = hashlib.file_digest(archive, "sha256").hexdigest()
if actual != expected.lower():
    raise SystemExit("SHA-256 mismatch")
print("SHA-256 matches:", name)
PY
```

For PowerShell, use the same here-string adaptation, ending with
`'@ | py -3 - path/to/archive.zip.sha256`. Matching hashes establish agreement
with the supplied sidecar; they do not authenticate its producer or establish
reproducible archive bytes.

## Installed documentation and SDK use

The installation configures [DistributionReadme.md.in](../../cmake/DistributionReadme.md.in)
with CMake's project version as `share/doc/GameEngine/README.md`. It contains
commands relative to the extracted archive and the public source URL.
The source-development README is not copied into
the binary archive. `NOTICE.md` and its local policy/research destinations are
installed together. Source-only links within the installed policy point to the
public repository, so the package does not pretend to include developer test files.
The relocation gate checks installed Markdown file destinations offline.

Consumers use `find_package(GameEngine CONFIG REQUIRED)` and the exported
`ge::core`, `ge::content`, `ge::world`, and `ge::engine` targets. Link `ge::engine`
for domain APIs; its public dependencies supply the foundation libraries and thread
runtime. Set `CMAKE_PREFIX_PATH` to the extracted prefix; the CMake files and
include/library paths move with it. The
consumer must match the producer's architecture, configuration, compiler/runtime,
standard library, and ABI options. MSVC runtime selection and debug iterator ABI
are particularly sensitive to Debug/Release mismatches. Build from source when
those requirements cannot be matched. An archive contains no bundled C++ system
runtime, dependency installer, editor or asset-discovery service.

Record actual host, compiler, configuration, commands and results in
[production verification](../production/verification.md). A passing local run is
evidence for that environment only. See [quality gates](../quality-gates.md) for
the wider acceptance contract.
