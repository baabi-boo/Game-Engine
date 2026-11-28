#!/usr/bin/env python3
"""Install, move, run, and consume a GameEngine build using only the stdlib.

The input must already be configured and built. All disposable files belong to
one private TemporaryDirectory; no caller-owned install tree is removed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Sequence


CONFIGURATIONS = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
MIN_CMAKE = (3, 25, 0)

CONSUMER_CMAKE = """cmake_minimum_required(VERSION 3.25)
project(GameEnginePackageConsumer LANGUAGES CXX)
find_package(GameEngine CONFIG REQUIRED COMPONENTS core content world engine
    PATHS "${GE_PACKAGE_PREFIX}" NO_DEFAULT_PATH)
add_executable(ge_package_consumer main.cpp)
target_link_libraries(ge_package_consumer PRIVATE ge::engine)
if(MINGW)
    target_link_options(ge_package_consumer PRIVATE -municode)
endif()
set_target_properties(ge_package_consumer PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
"""

CONSUMER_CPP = r"""#include <ge/core/clock.hpp>
#include <ge/core/identity.hpp>
#include <ge/core/runtime.hpp>
#include <ge/content/package.hpp>
#include <ge/geometry/mesh.hpp>
#include <ge/world/state.hpp>
#include <ge/world/streaming.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    try {
        require(argc == 2, "expected the installed fixture path");
        const auto package = ge::read_package(std::filesystem::path{argv[1]});
        require(!package.regions.empty() && !package.entities.empty(), "empty fixture");

        ge::FixedClock clock{std::chrono::milliseconds{10}, 2};
        const auto tick = clock.advance(std::chrono::milliseconds{15});
        require(tick.steps == 1 && tick.remainder == std::chrono::milliseconds{5},
                "installed fixed clock failed");

        const ge::PersistentId id{package.entities.front().id};
        require(id.valid(), "invalid installed persistent ID");
        ge::GenerationPool handles{1};
        const auto old_handle = handles.acquire();
        require(old_handle.has_value() && handles.alive(*old_handle), "handle acquire failed");
        require(handles.release(*old_handle), "handle release failed");
        const auto new_handle = handles.acquire();
        require(new_handle.has_value() && handles.alive(*new_handle) &&
                    !handles.alive(*old_handle) && *old_handle != *new_handle,
                "stale handle was accepted");
        require(handles.release(*new_handle), "new handle release failed");

        std::vector<ge::world::StateEntry> initial;
        for (const auto& entity : package.entities) {
            initial.push_back({ge::PersistentId{entity.id}, entity.initial_value});
        }
        ge::world::PersistentState state{package.baseline_id, initial};
        const auto original = package.entities.front().initial_value;
        const auto changed = original == std::numeric_limits<std::int32_t>::max()
            ? original - 1 : original + 1;
        require(state.set(id, changed), "state mutation failed");
        const auto snapshot = state.capture(1);
        const auto encoded = ge::world::encode_state(snapshot);
        const auto decoded = ge::world::decode_state(encoded);
        ge::world::PersistentState restored{package.baseline_id, initial};
        restored.restore(decoded);
        require(restored.get(id) == changed && restored.sequence() == 1 &&
                    restored.size() == package.entities.size(), "state restore failed");

        const auto& region = package.regions.front();
        ge::world::StreamingPolicy policy{region.budget_bytes};
        const auto token = policy.begin(region.id, 1, 1, region.required_mask,
                                        region.budget_bytes);
        require(token.has_value(), "region admission failed");
        require(policy.mark_ready(*token, region.required_mask) && policy.activate(*token),
                "region activation failed");
        require(policy.state(region.id) == ge::world::RegionState::Active &&
                    policy.retire(*token) &&
                    policy.memory_usage().retiring_bytes == region.budget_bytes,
                "region retirement released the reservation early");
        require(policy.acknowledge_retirement(*token) &&
                    policy.memory_usage().reserved_bytes == 0 && policy.live_regions() == 0,
                "retirement acknowledgement failed");

        // Exercise compiled ge_engine and ge_core symbols through the installed
        // engine target, including its transitive foundation/Threads dependencies.
        ge::core::Kernel kernel;
        ge::geometry::register_mesh(kernel);
        auto& meshes = kernel.services().get<ge::geometry::MeshLibrary>();
        const auto mesh = meshes.create(ge::geometry::make_box({2.0, 4.0, 6.0}), "sdk-box");
        const auto info = meshes.inspect(mesh);
        require(info.has_value() && info->triangles == 12 &&
                    info->bounds.min == ge::math::Vec3{-1.0, -2.0, -3.0} &&
                    info->bounds.max == ge::math::Vec3{1.0, 2.0, 3.0},
                "installed compiled geometry API failed");
        require(kernel.tick(0.125) && kernel.frame().tick == 1,
                "installed kernel/domain frame failed");
        require(meshes.destroy(mesh) && !meshes.contains(mesh),
                "installed mesh retirement failed");

        std::cout << "{\"status\":\"ok\",\"baseline_id\":" << package.baseline_id
                  << ",\"region_count\":" << package.regions.size()
                  << ",\"entity_count\":" << package.entities.size()
                  << ",\"headers_used\":7,\"components\":[\"core\",\"content\",\"world\",\"engine\"]"
                  << ",\"compiled_mesh_triangles\":" << info->triangles
                  << ",\"runtime_ticks\":" << kernel.frame().tick
                  << ",\"final_reserved_bytes\":0}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "package consumer: " << error.what() << '\n';
        return 1;
    }
}
"""


class SmokeError(Exception):
    """A relocation failure with a concise, actionable diagnostic."""


def _message(message: str) -> None:
    print(f"package-smoke: {message}", flush=True)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise SmokeError(message)


def _positive_integer(value: str) -> int:
    try:
        result = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if result < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def _file(path: Path, purpose: str) -> Path:
    _require(path.is_file(), f"{purpose} is missing: {path}")
    return path


def _program(value: str, purpose: str) -> str:
    executable = shutil.which(value)
    _require(executable is not None, f"{purpose} is unavailable: {value!r}; restore the build toolchain")
    return str(Path(executable).resolve())


def _run(command: Sequence[str], cwd: Path, *, capture: bool = False) -> str:
    display = subprocess.list2cmdline(command) if os.name == "nt" else shlex.join(command)
    _message(f"$ {display}")
    environment = os.environ.copy()
    # The cache, rather than a newly changed caller environment, chooses the
    # consumer toolchain. Keep PATH/INCLUDE/LIB for configured native toolchains.
    # In particular, an environment toolchain file must not load another project
    # and DESTDIR must not redirect this private installation.
    for key in ("DESTDIR", "CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR",
                "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET",
                "CMAKE_GENERATOR_INSTANCE", "CC", "CXX", "CFLAGS", "CXXFLAGS",
                "CPPFLAGS", "LDFLAGS", "CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH",
                "LIBRARY_PATH"):
        environment.pop(key, None)
    result = subprocess.run(list(command), cwd=cwd, env=environment, check=False,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace")
    if result.stdout and (not capture or result.returncode):
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n", flush=True)
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, list(command))
    return result.stdout


def _json_run(command: Sequence[str], cwd: Path) -> dict[str, Any]:
    output = _run(command, cwd, capture=True)
    try:
        result = json.loads(output)
    except json.JSONDecodeError as error:
        raise SmokeError(f"expected a single JSON object from {command[0]}: {output!r}") from error
    _require(isinstance(result, dict), f"expected JSON object from {command[0]}")
    return result


def _relative_install_dir(cache: dict[str, str], key: str, default: str) -> Path:
    value = cache.get(key) or default
    path = Path(value)
    _require(not path.is_absolute() and ".." not in path.parts and ";" not in value,
             f"{key} must stay inside the installation prefix, found {value!r}")
    return path


def _cache(build: Path, config: str) -> tuple[dict[str, str], str]:
    path = _file(build / "CMakeCache.txt", "configured build cache; configure and build first")
    cache: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("//", "#")):
            continue
        key, separator, value = line.partition("=")
        if separator and ":" in key:
            cache[key.rsplit(":", 1)[0]] = value
    _require(cache.get("CMAKE_PROJECT_NAME") == "GameEngine",
             f"{path} is not a GameEngine build cache")
    _require(bool(cache.get("CMAKE_PROJECT_VERSION")), f"{path} has no project version")
    _require(bool(cache.get("CMAKE_HOME_DIRECTORY")), f"{path} has no source directory")
    _require(Path(cache.get("CMAKE_CACHEFILE_DIR", "")).resolve() == build,
             "the build directory has moved since configuration; reconfigure and build it first")
    _file(build / "cmake_install.cmake", "CMake install script; configure and build first")
    configurations = cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";")
    if any(configurations):
        _require(config in configurations,
                 f"configuration {config} is not available; configured choices: {configurations}")
    else:
        _require(cache.get("CMAKE_BUILD_TYPE") == config,
                 f"requested {config}, but the build cache is {cache.get('CMAKE_BUILD_TYPE')!r}; "
                 "pass the matching --config or select a matching build directory")

    _require(bool(cache.get("CMAKE_GENERATOR")), "build cache is missing CMAKE_GENERATOR")
    visual_studio = cache["CMAKE_GENERATOR"].startswith("Visual Studio ")
    for key, purpose in (("CMAKE_CXX_COMPILER", "configured C++ compiler"),
                         ("CMAKE_MAKE_PROGRAM", "configured build tool")):
        if cache.get(key) and (not visual_studio or Path(cache[key]).is_absolute()):
            _program(cache[key], purpose)
        elif not cache.get(key) and not visual_studio:
            raise SmokeError(f"build cache is missing {key}; reconfigure the build")
    # Visual Studio locates its compiler and MSBuild from the recorded generator,
    # platform, toolset and instance. cl.exe need not be on the caller's PATH.
    cmake = _program(cache.get("CMAKE_COMMAND") or "cmake", "CMake 3.25+")
    version = _run([cmake, "--version"], build, capture=True)
    match = re.search(r"cmake version (\d+)\.(\d+)\.(\d+)", version)
    _require(match is not None and tuple(map(int, match.groups())) >= MIN_CMAKE,
             "CMake 3.25+ is required")

    bindir = _relative_install_dir(cache, "CMAKE_INSTALL_BINDIR", "bin")
    dataroot = _relative_install_dir(cache, "CMAKE_INSTALL_DATAROOTDIR", "share")
    datadir = _relative_install_dir(cache, "CMAKE_INSTALL_DATADIR", str(dataroot))
    _require(bindir == Path("bin") and datadir == Path("share"),
             "this distribution gate expects CMAKE_INSTALL_BINDIR=bin and CMAKE_INSTALL_DATADIR=share")
    _relative_install_dir(cache, "CMAKE_INSTALL_INCLUDEDIR", "include")
    _relative_install_dir(cache, "CMAKE_INSTALL_LIBDIR", "lib")
    _relative_install_dir(cache, "CMAKE_INSTALL_DOCDIR", "share/doc/GameEngine")
    return cache, cmake


def _consumer_configuration(cache: dict[str, str], config: str) -> list[str]:
    arguments = ["-G", cache["CMAKE_GENERATOR"], f"-DCMAKE_BUILD_TYPE:STRING={config}"]
    for key, option in (("CMAKE_GENERATOR_PLATFORM", "-A"), ("CMAKE_GENERATOR_TOOLSET", "-T")):
        if cache.get(key):
            arguments.extend([option, cache[key]])
    # Preserve the chosen compiler and ABI-relevant settings. Do not load the
    # engine's project, presets, build targets, or a source-tree toolchain file.
    keys = (
        "CMAKE_GENERATOR_INSTANCE", "CMAKE_CONFIGURATION_TYPES", "CMAKE_MAKE_PROGRAM", "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_COMPILER_ARG1", "CMAKE_CXX_COMPILER_TARGET",
        "CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN", "CMAKE_SYSROOT",
        "CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_DEPLOYMENT_TARGET", "CMAKE_OSX_SYSROOT",
        "CMAKE_MSVC_RUNTIME_LIBRARY", "CMAKE_CXX_FLAGS", f"CMAKE_CXX_FLAGS_{config.upper()}",
        "CMAKE_EXE_LINKER_FLAGS", f"CMAKE_EXE_LINKER_FLAGS_{config.upper()}",
    )
    for key in keys:
        if cache.get(key):
            arguments.append(f"-D{key}:STRING={cache[key]}")
    return arguments


def _reject_checkout_paths(text: str, forbidden: Sequence[Path], purpose: str) -> None:
    normalized = text.replace("\\", "/").casefold()
    for path in forbidden:
        _require(path.as_posix().casefold() not in normalized,
                 f"{purpose} refers to an original source/build/staging path: {path}")


def _check_install(prefix: Path, stage: Path, build: Path, cache: dict[str, str]) -> None:
    _require(not stage.exists() and not stage.is_symlink(), "the original installation stage still exists")
    forbidden = (stage, build, Path(cache["CMAKE_HOME_DIRECTORY"]).resolve())
    for path in prefix.rglob("*"):
        _require(path.resolve().is_relative_to(prefix), f"installed link escapes the moved prefix: {path}")
        if path.is_file() and path.suffix == ".cmake":
            _reject_checkout_paths(path.read_text(encoding="utf-8"), forbidden, str(path))

    docdir = prefix / _relative_install_dir(cache, "CMAKE_INSTALL_DOCDIR", "share/doc/GameEngine")
    for name in ("README.md", "NOTICE.md", "docs/project-policy.md", "docs/research/open-world-blueprint.md"):
        _file(docdir / name, "installed distribution documentation")
    # This intentionally checks file destinations, not remote URLs or anchors.
    for path in docdir.rglob("*.md"):
        for link in re.findall(r"\]\(([^\s)]+)\)", path.read_text(encoding="utf-8")):
            if link.startswith("#") or re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", link):
                continue
            destination = (path.parent / link.split("#", 1)[0]).resolve()
            _require(destination.is_relative_to(prefix) and destination.exists(),
                     f"installed documentation has a broken local link: {path}: {link}")
    _message("original install stage is absent; moved CMake exports and documentation are self-contained")


def _check_manifest(path: Path, host: Path, fixture: Path,
                    cache: dict[str, str], config: str, *,
                    runtime: Path | None = None, library_dir: Path | None = None) -> None:
    try:
        manifest = json.loads(_file(path, "installed build manifest").read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SmokeError(f"installed build manifest is invalid JSON: {path}") from error
    _require(isinstance(manifest, dict) and type(manifest.get("schema_version")) is int and
             manifest["schema_version"] == 1, "installed build manifest must use schema version 1")
    project = manifest.get("project")
    _require(isinstance(project, dict) and project.get("name") == cache["CMAKE_PROJECT_NAME"] and
             project.get("version") == cache["CMAKE_PROJECT_VERSION"],
             "installed manifest project name/version does not match the configured build")
    settings = manifest.get("build")
    _require(isinstance(settings, dict) and settings.get("configuration") == config,
             f"installed manifest does not describe requested configuration {config}")

    artifacts = manifest.get("artifacts")
    _require(isinstance(artifacts, dict), "installed manifest has no artifact records")
    installed = {"headless": host, "fixture": fixture}
    # Retain foundation-only v1 verification for its original callers. The full
    # relocation gate always supplies both paths and requires every SDK record.
    if runtime is not None or library_dir is not None:
        _require(runtime is not None and library_dir is not None,
                 "expanded manifest verification requires the runtime and library directory")
        installed["runtime"] = runtime
        for name in ("engine", "core", "content", "world"):
            record = artifacts.get(name)
            filename = record.get("filename") if isinstance(record, dict) else None
            _require(isinstance(filename, str) and filename not in ("", ".", "..") and
                     not any(character in filename for character in "/\\:\0"),
                     f"installed manifest has an invalid {name} library filename")
            installed[name] = library_dir / filename
    for name, artifact in installed.items():
        record = artifacts.get(name)
        _require(isinstance(record, dict) and record.get("filename") == artifact.name,
                 f"installed manifest has an invalid {name} filename")
        _require(type(record.get("size_bytes")) is int and record["size_bytes"] >= 0 and
                 isinstance(record.get("sha256"), str) and
                 re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) is not None,
                 f"installed manifest has invalid {name} size/SHA-256 metadata")
        digest = hashlib.sha256()
        size = 0
        with _file(artifact, f"installed {name} artifact").open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
                size += len(block)
        _require(record["size_bytes"] == size,
                 f"installed manifest size mismatch for {name}; run a normal build before installing")
        _require(record["sha256"] == digest.hexdigest(),
                 f"installed manifest SHA-256 mismatch for {name}; run a normal build before installing")

    # This is the producer's build-time observation, not a claim about the current
    # checkout or authentication of the source or hashed artifacts.
    source = manifest.get("source")
    _require(isinstance(source, dict), "installed manifest has no source status")
    if source.get("status") == "git":
        commit = source.get("git_commit")
        _require(isinstance(commit, str) and
                 re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit) is not None and
                 type(source.get("dirty")) is bool,
                 "installed manifest has invalid recorded Git source status")
        source_status = f"git commit={commit}, dirty={str(source['dirty']).lower()}"
    else:
        _require(source.get("status") == "unknown" and
                 "git_commit" in source and source["git_commit"] is None and
                 "dirty" in source and source["dirty"] is None and
                 source.get("reason") in ("git_metadata_absent", "git_unavailable"),
                 "installed manifest has invalid unknown source status")
        source_status = f"unknown ({source['reason']})"
    _message(f"installed manifest matches {project['name']} {project['version']} / {config}; "
             f"all {len(installed)} moved artifact SHA-256/byte sizes verified")
    _message(f"manifest source record: {source_status}; build-time metadata, not authenticated provenance")


def _check_host(inspect: dict[str, Any], lifecycle: dict[str, Any]) -> None:
    _require(inspect.get("mode") == "inspect" and inspect.get("schema_version") == 1,
             "installed host did not inspect a v1 package")
    for key in ("baseline_id", "region_count", "entity_count"):
        _require(type(inspect.get(key)) is int and inspect[key] > 0, f"invalid inspect field: {key}")
    _require(isinstance(inspect.get("source_sha256"), str) and
             re.fullmatch(r"[0-9a-f]{64}", inspect["source_sha256"]) is not None,
             "inspect output is missing its source digest")
    regions = inspect.get("regions")
    _require(isinstance(regions, list) and len(regions) == inspect["region_count"] and
             all(isinstance(region, dict) and type(region.get("id")) is int for region in regions),
             "inspect output has an invalid region list")
    _require(lifecycle.get("mode") == "headless-policy-fixture" and lifecycle.get("status") == "ok",
             "installed headless lifecycle did not report success")
    for key in ("schema_version", "baseline_id", "region_count", "entity_count", "source_sha256"):
        _require(lifecycle.get(key) == inspect[key], f"lifecycle and inspect disagree on {key}")
    checks = lifecycle.get("checks")
    required_checks = {
        "all_regions_visited", "budget_respected", "final_resources_retired",
        "incomplete_readiness_rejected", "mutation_survived_reload", "snapshot_restored",
        "retirement_retained_reservation", "retirement_ack_released_reservation",
        "stale_handle_rejected", "stale_token_rejected",
    }
    _require(isinstance(checks, dict) and required_checks.issubset(checks) and
             all(value is True for value in checks.values()), "installed lifecycle checks failed or are absent")
    _require(lifecycle.get("final_reserved_bytes") == 0, "installed lifecycle left modeled reservations live")
    _require(type(lifecycle.get("peak_reserved_bytes")) is int and
             type(lifecycle.get("budget_bytes")) is int and
             0 < lifecycle["peak_reserved_bytes"] <= lifecycle["budget_bytes"],
             "installed lifecycle reported an invalid peak reservation")
    _require(lifecycle.get("visited_regions") == sorted(region["id"] for region in regions),
             "installed lifecycle did not visit every installed region")
    _message(f"installed host passed inspect and lifecycle: baseline={inspect['baseline_id']}, "
             f"regions={inspect['region_count']}, entities={inspect['entity_count']}, "
             f"checks={len(checks)}, final_reserved_bytes=0")


def _check_runtime(runtime: Path, cwd: Path) -> None:
    script = cwd / "runtime commands \u03b4 \u8857\u533a.txt"
    report_path = cwd / "runtime report \u03b4 \u8857\u533a.json"
    script.write_bytes(b"mesh.create relocated-box box 2\r\n\t tick \r\n")
    _run([str(runtime), "--ticks", "2", "--step", "0.125",
          "--script", str(script), "--report", str(report_path)], cwd)
    try:
        report = json.loads(_file(report_path, "installed runtime report").read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SmokeError("installed runtime report is invalid JSON") from error
    _require(isinstance(report, dict) and report.get("success") is True and
             type(report.get("tick")) is int and report["tick"] == 3 and
             report.get("elapsed_seconds") == 0.375,
             "installed runtime did not complete its three-frame scenario")
    modules = report.get("modules")
    _require(isinstance(modules, list) and
             any(isinstance(module, dict) and module.get("name") == "geometry.mesh" for module in modules),
             "installed runtime did not register its compiled geometry module")
    systems = report.get("systems")
    _require(isinstance(systems, list) and systems and
             all(isinstance(system, dict) and isinstance(system.get("name"), str) and
                 system.get("failures") == 0 and
                 ((system.get("enabled") is True and system.get("calls") == 3) or
                  (system.get("name") == "render.rasterizer" and
                   system.get("enabled") is False and system.get("calls") == 0))
                 for system in systems),
             "installed runtime systems did not complete every frame")
    # The graph owns rendering and intentionally disables its standalone driver.
    _require({"core.scratch", "geometry.mesh.bounds", "render.graph.frame"}.issubset(
                 system["name"] for system in systems),
             "installed runtime omitted core or geometry frame systems")
    metrics = report.get("metrics")
    _require(isinstance(metrics, dict) and metrics.get("mesh.count") == 1 and
             metrics.get("mesh.triangles") == 12,
             "installed runtime did not process its command-created mesh")
    _require(metrics.get("render.graph.enabled") == 1 and metrics.get("render.graph.error") == 0,
             "installed runtime did not execute its render graph")
    _require(report.get("diagnostics") == [], "installed runtime reported unexpected diagnostics")
    _message("installed runtime passed a command-created mesh and three frames from the moved prefix")


def _smoke(build: Path, config: str, jobs: int) -> None:
    cache, cmake = _cache(build, config)
    with tempfile.TemporaryDirectory(prefix="ge-package-smoke-") as temporary:
        work = Path(temporary).resolve()
        _require(not work.is_relative_to(build) and
                 not work.is_relative_to(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve()),
                 "the temporary directory must be outside the engine checkout/build; "
                 "choose an external TMPDIR or TEMP directory")
        stage = work / "original install stage"
        prefix = work / "relocated district \u03b4 \u8857\u533a with spaces"
        cwd = work / "unrelated working directory"
        source = work / "independent consumer source"
        consumer_build = work / "independent consumer build"
        cwd.mkdir()
        source.mkdir()
        _run([cmake, "--install", str(build), "--config", config, "--prefix", str(stage)], cwd)
        _require(stage.is_dir(), "CMake did not create the private installation stage")
        stage.rename(prefix)
        _check_install(prefix, stage, build, cache)

        suffix = ".exe" if os.name == "nt" else ""
        host = _file(prefix / "bin" / f"ge_headless{suffix}", "installed headless executable")
        runtime = _file(prefix / "bin" / f"ge_runtime{suffix}", "installed runtime executable")
        fixture = _file(prefix / "share/game-engine/assets/district.gepack", "installed cooked fixture")
        library_dir = prefix / _relative_install_dir(cache, "CMAKE_INSTALL_LIBDIR", "lib")
        _check_manifest(prefix / "share/game-engine/build-manifest.json", host, fixture, cache, config,
                        runtime=runtime, library_dir=library_dir)
        command = [str(host), "--package", str(fixture)]
        inspect = _json_run([*command, "--inspect"], cwd)
        lifecycle = _json_run(command, cwd)
        _check_host(inspect, lifecycle)
        _check_runtime(runtime, cwd)

        (source / "CMakeLists.txt").write_text(CONSUMER_CMAKE, encoding="utf-8")
        (source / "main.cpp").write_text(CONSUMER_CPP, encoding="utf-8")
        configuration = _consumer_configuration(cache, config)
        forbidden = (build, Path(cache["CMAKE_HOME_DIRECTORY"]).resolve(), stage)
        _reject_checkout_paths("\n".join(configuration), forbidden, "consumer toolchain configuration")
        _run([cmake, "-S", str(source), "-B", str(consumer_build), *configuration,
              f"-DGE_PACKAGE_PREFIX:PATH={prefix}",
              "-DCMAKE_FIND_USE_PACKAGE_REGISTRY:BOOL=OFF",
              "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY:BOOL=OFF"], cwd)
        _run([cmake, "--build", str(consumer_build), "--config", config,
              "--parallel", str(jobs)], cwd)
        consumer = _file(consumer_build / "bin" / config / f"ge_package_consumer{suffix}",
                         "newly built SDK consumer")
        result = _json_run([str(consumer), str(fixture)], cwd)
        _require(result.get("status") == "ok" and result.get("headers_used") == 7 and
                 result.get("components") == ["core", "content", "world", "engine"] and
                 result.get("compiled_mesh_triangles") == 12 and result.get("runtime_ticks") == 1 and
                 result.get("final_reserved_bytes") == 0, "installed SDK consumer failed its contracts")
        for key in ("baseline_id", "region_count", "entity_count"):
            _require(result.get(key) == inspect[key], f"SDK consumer used a different fixture: {key}")
        _require(not stage.exists() and not stage.is_symlink(), "original stage reappeared during the gate")
        _message(f"new {config} consumer passed seven public headers and compiled ge::engine APIs")
    _message(f"relocation gate passed for {config}; private temporary files removed")


def main(argv: list[str] | None = None) -> int:
    # Redirected Windows logs can use a legacy encoding. Escape only diagnostic
    # display when needed; subprocess arguments retain their native Unicode paths.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True, metavar="DIR",
                        help="existing, configured and built GameEngine directory")
    parser.add_argument("--config", choices=CONFIGURATIONS, default="Release",
                        help="installed/consumer configuration (default: %(default)s)")
    parser.add_argument("--jobs", type=_positive_integer, default=2,
                        help="parallel consumer build jobs (default: %(default)s)")
    arguments = parser.parse_args(argv)
    if sys.version_info < (3, 11):
        print("package-smoke: Python 3.11+ is required", file=sys.stderr)
        return 2
    try:
        _smoke(arguments.build_dir.expanduser().resolve(), arguments.config, arguments.jobs)
    except subprocess.CalledProcessError as error:
        status = error.returncode if error.returncode > 0 else 128 - error.returncode
        print(f"package-smoke: command failed with exit code {status}", file=sys.stderr)
        return status
    except (SmokeError, OSError, UnicodeError) as error:
        print(f"package-smoke: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("package-smoke: interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
