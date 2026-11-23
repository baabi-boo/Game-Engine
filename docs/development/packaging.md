# Packaging and relocation

The binary distribution contains the installed headless host, cooked and authored
district fixtures, all five public headers, static content/world libraries, CMake
package exports, and distribution documentation. This is a pre-alpha headless
contract prototype. Packaging does not establish graphical capability, real asset
streaming, performance, crash durability, a supported platform matrix, or a stable
binary ABI. Read the [architecture](../architecture/overview.md) for the scope and
[project policy](../project-policy.md) for the pending owner license decision.

## Build, verify and archive

From the repository root, with Python 3.11+, CMake 3.25+, Ninja and a C++20 compiler:

```sh
python3 tools/ge.py package --preset release --jobs 2
```

The driver configures, builds, runs CTest, executes the relocation gate and then
invokes CPack. With the default presets, ZIP/TGZ archives and SHA-256 files appear
under `out/build/release/packages/`. The archives contain the install tree under
one top-level directory. Archive names identify project version, system and
architecture; they do not identify every ABI setting. Preserve the producer's
toolchain and configuration in the accompanying validation record.

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
3. Physically rename that installation to a different prefix containing spaces.
   Check that the original stage is absent, installed links stay inside the moved
   prefix, and exported CMake files refer to no original stage, build or checkout.
4. Run the installed `bin/ge_headless` (`.exe` on Windows) from an unrelated
   working directory with the moved `share/game-engine/assets/district.gepack`.
   Parse both `--inspect` and lifecycle JSON. Check matching package metadata,
   all required lifecycle checks, visits to every region, a bounded peak modeled
   reservation and `final_reserved_bytes == 0`.
5. Create and build a fresh external CMake project that finds `GameEngine` only
   under the moved prefix and links `ge::core`, `ge::content`, and `ge::world`.
   It includes and uses every public header: clock and identity, package loading,
   state encode/decode/restore, and readiness/retirement policy. Execute this newly
   linked consumer with the installed fixture from the unrelated working directory.
6. Verify that the original stage remains absent and remove only the script's own
   temporary directory, including its disposable consumer source and build.

Commands use argument arrays without a shell. Spaces in the build, install and
consumer paths remain individual arguments. Every failed command, missing artifact,
invalid JSON result, broken installed local documentation link, or failed consumer
contract produces a nonzero exit. Successful output identifies the configuration,
fixture counts, lifecycle checks and SDK consumer completion.

This gate checks relocation of installation paths and an independent source consumer
on the producing host. It does not erase the engine checkout or prove compatibility
with another OS, architecture, runtime, compiler version or ABI. Debug information
inside binaries can still record compilation paths; those are distinct from a
runtime asset dependency or an exported include/library path. The gate rejects
checkout/build paths in the consumer's inherited toolchain arguments instead of
using those paths to satisfy a missing SDK dependency. Its temporary outputs are
removed after the run; retain command logs separately when recording evidence.

## Installed documentation and SDK use

The installation renames [DistributionReadme.md](../../cmake/DistributionReadme.md)
to `share/doc/GameEngine/README.md`. It contains commands relative to the extracted
archive and the public source URL. The source-development README is not copied into
the binary archive. `NOTICE.md` and its local policy/research destinations are
installed together. Source-only links within the installed policy point to the
public repository, so the package does not pretend to include developer test files.
The relocation gate checks installed Markdown file destinations offline.

Consumers use `find_package(GameEngine CONFIG REQUIRED)` and the exported
`ge::core`, `ge::content`, and `ge::world` targets. Set `CMAKE_PREFIX_PATH` to the
extracted prefix; the CMake files and include/library paths move with it. The
consumer must match the producer's architecture, configuration, compiler/runtime,
standard library, and ABI options. MSVC runtime selection and debug iterator ABI
are particularly sensitive to Debug/Release mismatches. Build from source when
those requirements cannot be matched. An archive contains no bundled C++ system
runtime, dependency installer, editor or asset-discovery service.

Record actual host, compiler, configuration, commands and results in
[production verification](../production/verification.md). A passing local run is
evidence for that environment only. See [quality gates](../quality-gates.md) for
the wider acceptance contract.
