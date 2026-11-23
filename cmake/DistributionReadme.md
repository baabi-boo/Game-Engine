# Game Engine binary distribution

**Version 0.1.0 · headless foundation / pre-alpha**

This archive contains an independent C++20 headless contract prototype, its small
original district fixture, and a static-library SDK. Its lifecycle demonstrates
package validation, symbolic readiness, modeled reservation accounting, persistent
identity, scalar mutation, retirement and restoration. It has no renderer, physical
collision system, asynchronous asset I/O pipeline, or production save protocol.
Counters are modeled reservations, not measured resource memory or performance.

Source and development instructions:
[github.com/baabi-boo/Game-Engine](https://github.com/baabi-boo/Game-Engine).
The archive is built for its producer's operating system, architecture, compiler,
standard library and configuration. Version 0.1.0 does not establish a supported
production release or a stable binary ABI.

## Run the installed fixture

Extract the whole archive, keeping its directory structure. From the extracted
prefix on a compatible POSIX host:

```sh
./bin/ge_headless --package ./share/game-engine/assets/district.gepack --inspect
./bin/ge_headless --package ./share/game-engine/assets/district.gepack
```

For an archive built on Windows, use PowerShell:

```powershell
.\bin\ge_headless.exe --package .\share\game-engine\assets\district.gepack --inspect
.\bin\ge_headless.exe --package .\share\game-engine\assets\district.gepack
```

These are host-specific commands, not a claim that every platform has been verified.
The executable requires an explicit `--package` path. From another working directory,
use quoted absolute paths to both the executable and the installed `.gepack`; the
working directory does not supply assets. `--help` describes the optional budget and
save/restore paths. Inspect mode and the lifecycle each emit one JSON object and
return nonzero on failure. The lifecycle reports `status: "ok"`, its individual
`checks`, and `final_reserved_bytes: 0` on success.

The host needs its compatible platform C++ runtime; runtimes and system libraries
are not bundled. Python, Ninja and the source checkout are unnecessary to run this
already built fixture. Build tools are required to compile an SDK consumer.

## Consume the SDK

Use CMake 3.25+ with a C++20 compiler, architecture, standard library and runtime
compatible with the producer. Match the archive's Debug, Release, RelWithDebInfo or
MinSizeRel configuration. In particular, preserve MSVC platform/toolset, runtime
selection and debug iterator settings; mixing a Debug SDK and Release consumer can
be ABI-incompatible. A matching package version alone does not establish ABI
compatibility. Rebuild from source when the toolchain or ABI settings differ.

For a project containing `main.cpp`, the consumer `CMakeLists.txt` can be:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyConsumer LANGUAGES CXX)
find_package(GameEngine CONFIG REQUIRED COMPONENTS core content world)
add_executable(my_consumer main.cpp)
target_link_libraries(my_consumer PRIVATE ge::core ge::content ge::world)
```

Configure a Release consumer with the extracted prefix, substituting its actual
absolute path and the generator used by your compatible toolchain:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/absolute/path/to/extracted prefix" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

With a Visual Studio generator, select the corresponding generator, architecture
and toolset using CMake's `-G`, `-A` and `-T` options; `--config Release` selects the
build configuration. Visual Studio generators discover their installed toolchain
without requiring a Developer PowerShell. Ninja with MSVC needs the configured
compiler/SDK environment. This example does not download or install build tools.

The exported targets carry the installed include directory and C++20 requirement.
`ge::core` provides `ge/core/clock.hpp` and `ge/core/identity.hpp`.
`ge::content` provides `ge/content/package.hpp`; `ge::world` provides
`ge/world/state.hpp` and `ge/world/streaming.hpp`. Pass the fixture path to your
consumer at runtime; there is no source-checkout asset path in the SDK interface.

## Contents and notices

The default layout is `bin/` for the host, `include/ge/` for public headers,
`lib/` for static libraries and `lib/cmake/GameEngine/` for CMake exports.
`share/game-engine/assets/` contains `district.gepack` and its authored JSON source.
Some toolchains use `lib64/` or a platform-specific library directory.

This README lives in `share/doc/GameEngine/` together with `NOTICE.md`,
`docs/project-policy.md` and `docs/research/open-world-blueprint.md`.
Source-development references in the installed policy link to the public repository;
the notice's policy and research references remain local and readable offline.

**Project license: pending owner selection.** The notice and this archive introduce
no additional software license grant. Consult the included policy before external
reuse or distribution. The project is independent, is not Rockstar's RAGE, and is
not Rockstar-affiliated. No proprietary game source, game assets or console SDKs
are included. Research attribution does not imply endorsement or integration of a
referenced component. No vendored third-party runtime source or binary assets are
included in this foundation; platform tools and standard libraries retain their
own terms.
