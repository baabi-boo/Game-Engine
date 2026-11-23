#!/usr/bin/env python3
"""Repository driver for Python 3.11+ and the standard library.

Commands always execute from the checkout root. Explicit file arguments are
relative to the caller's working directory; CMake -D values retain CMake's own
semantics. Existing build caches and the caller's environment are preserved.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Sequence


ROOT = Path(__file__).resolve().parent.parent
PRESETS = ("dev", "release", "asan")
MIN_PYTHON = (3, 11)
MIN_CMAKE = (3, 25, 0)


class DriverError(Exception):
    """An actionable driver error that does not need a traceback."""


def _message(message: str) -> None:
    print(f"ge: {message}", file=sys.stderr, flush=True)


def _program(name: str) -> str:
    executable = shutil.which(name)
    if executable is None:
        requirement = "CMake 3.25+" if name in {"cmake", "ctest", "cpack"} else name
        raise DriverError(
            f"{name} is not on PATH. Install {requirement}, reopen your terminal, "
            "and run 'python3 tools/ge.py doctor'."
        )
    # PATH may contain relative directories; subprocesses change cwd to ROOT.
    return str(Path(executable).absolute())


def _command_text(command: Sequence[str]) -> str:
    return subprocess.list2cmdline(command) if os.name == "nt" else shlex.join(command)


def _run(command: Sequence[str]) -> None:
    # Display quoting is informational only; no command is passed to a shell.
    _message(f"$ {_command_text(command)}")
    subprocess.run(list(command), cwd=ROOT, check=True)


def _file(path: Path, purpose: str) -> Path:
    if not path.is_file():
        raise DriverError(f"{purpose} is missing: {path}")
    return path


def _user_path(value: str) -> Path:
    # Do not resolve the final component: the cooker must still see and reject
    # a source/output symlink instead of silently receiving its target.
    return Path(os.path.abspath(os.path.expanduser(value)))


def _positive_integer(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def _definition(value: str) -> str:
    name, separator, _ = value.partition("=")
    if not separator or not name or name.startswith("-"):
        raise argparse.ArgumentTypeError("expected NAME=VALUE or NAME:TYPE=VALUE")
    return value


def _build_dir(preset: str) -> Path:
    return ROOT / "out" / "build" / preset


def _cache(preset: str) -> dict[str, str]:
    path = _file(_build_dir(preset) / "CMakeCache.txt",
                 f"configured {preset} build; run 'tools/ge.py configure --preset {preset}'")
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        key, separator, value = line.partition("=")
        if separator and ":" in key:
            values[key.rsplit(":", 1)[0]] = value
    source = values.get("CMAKE_HOME_DIRECTORY")
    if not source or Path(source).resolve() != ROOT:
        raise DriverError(f"{path} belongs to a different or invalid source tree; "
                          "use a build directory configured for this checkout.")
    return values


def _enabled(value: str) -> bool:
    upper = value.upper()
    return (upper not in {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}
            and not upper.endswith("-NOTFOUND"))


def _configuration(preset: str) -> str:
    configuration = _cache(preset).get("CMAKE_BUILD_TYPE", "")
    if not configuration:
        raise DriverError(f"{preset} has no CMAKE_BUILD_TYPE in its cache; configure "
                          "with -D CMAKE_BUILD_TYPE=Debug or Release.")
    return configuration


def _configure(preset: str, definitions: Sequence[str] = ()) -> None:
    _run([_program("cmake"), "--preset", preset,
          *(f"-D{definition}" for definition in definitions)])
    _cache(preset)


def _build(preset: str, jobs: int) -> None:
    if not (_build_dir(preset) / "CMakeCache.txt").is_file():
        _configure(preset)
    else:
        _cache(preset)
    _run([_program("cmake"), "--build", "--preset", preset,
          "--parallel", str(jobs), "--config", _configuration(preset)])


def _test(preset: str, jobs: int) -> None:
    cache = _cache(preset)
    if not _enabled(cache.get("BUILD_TESTING", "")):
        raise DriverError(f"BUILD_TESTING is disabled for {preset}; configure with "
                          "-D BUILD_TESTING=ON before requesting tests.")
    _file(_build_dir(preset) / "CTestTestfile.cmake", "CTest test manifest")
    logs = ROOT / "out" / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    report = logs / f"ctest-{preset}.xml"
    _run([_program("ctest"), "--preset", preset,
          "--build-config", _configuration(preset), "--parallel", str(jobs),
          "--output-on-failure", "--no-tests=error", "--output-junit", str(report)])
    _file(report, "CTest JUnit report")
    _message(f"JUnit report: {report}")


def _doctor(_: argparse.Namespace) -> None:
    print(f"Python {sys.version.split()[0]}: OK ({sys.executable})", flush=True)
    errors: list[str] = []
    programs: dict[str, str] = {}
    for name in ("cmake", "ninja", "ctest", "cpack"):
        try:
            executable = _program(name)
            result = subprocess.run([executable, "--version"], cwd=ROOT, check=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, errors="replace", timeout=15)
            version_line = next((line for line in result.stdout.splitlines() if line.strip()), "")
            if name == "cmake":
                match = re.search(r"\bcmake version (\d+)\.(\d+)\.(\d+)", result.stdout)
                if not match or tuple(map(int, match.groups())) < MIN_CMAKE:
                    raise DriverError(f"CMake 3.25+ is required; found {version_line!r}. "
                                      "Install a supported CMake and update PATH.")
            programs[name] = executable
            print(f"{name}: OK ({version_line}; {executable})", flush=True)
        except (DriverError, OSError, subprocess.SubprocessError) as error:
            errors.append(str(error))
            print(f"{name}: ERROR: {error}", flush=True)

    if "cmake" in programs and "ninja" in programs:
        doctor_dir = ROOT / "out" / "doctor"
        doctor_dir.mkdir(parents=True, exist_ok=True)
        # CMake handles CXX, CXXFLAGS and CMAKE_TOOLCHAIN_FILE using its normal
        # platform rules. Do not guess a compiler or replace explicit overrides.
        try:
            with tempfile.TemporaryDirectory(prefix="cxx20-", dir=doctor_dir) as temporary:
                source = Path(temporary)
                (source / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.25)\n"
                    "project(GEDoctor LANGUAGES CXX)\n"
                    "add_executable(ge_doctor main.cpp)\n"
                    "set_target_properties(ge_doctor PROPERTIES CXX_STANDARD 20 "
                    "CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)\n", encoding="utf-8")
                (source / "main.cpp").write_text(
                    "#include <array>\n#include <concepts>\n#include <span>\n"
                    "static_assert(std::integral<int>);\n"
                    "int main() { std::array<int, 2> a{1, 2}; "
                    "std::span<int> s(a); return s[0] - 1; }\n", encoding="utf-8")
                build = source / "build"
                for command in (
                    [programs["cmake"], "-S", str(source), "-B", str(build), "-G", "Ninja"],
                    [programs["cmake"], "--build", str(build), "--parallel", "2"],
                ):
                    _message(f"$ {_command_text(command)}")
                    result = subprocess.run(command, cwd=ROOT, check=False,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, errors="replace", timeout=120)
                    if result.returncode:
                        print(result.stdout, file=sys.stderr, end="", flush=True)
                        raise DriverError("the active C++ toolchain cannot compile and link "
                                          "the C++20 probe; see diagnostics above.")
                print("C++20 compiler and standard library: OK (compile/link probe)", flush=True)
        except (DriverError, OSError, subprocess.SubprocessError) as error:
            errors.append(str(error))
            print(f"C++20: ERROR: {error}", flush=True)
            _message("Install a C++20 compiler and its development libraries. On Windows, "
                     "use a Visual Studio Developer PowerShell with the C++ workload and "
                     "Windows SDK installed. Check CXX, CXXFLAGS and CMAKE_TOOLCHAIN_FILE "
                     "if you set them; this driver does not replace those settings.")
    else:
        errors.append("C++20 validation requires working CMake and Ninja.")
        print("C++20: ERROR: install/fix CMake and Ninja, then rerun doctor.", flush=True)
    if errors:
        raise DriverError(f"doctor found {len(errors)} problem(s); fix the errors above and rerun.")
    print("Doctor passed. Run 'python3 tools/ge.py check' to validate the repository.", flush=True)


def _configure_command(arguments: argparse.Namespace) -> None:
    _configure(arguments.preset, arguments.define)


def _build_command(arguments: argparse.Namespace) -> None:
    _build(arguments.preset, arguments.jobs)


def _test_command(arguments: argparse.Namespace) -> None:
    _test(arguments.preset, arguments.jobs)


def _check(arguments: argparse.Namespace) -> None:
    checker = _file(ROOT / "tools" / "check_repo.py", "required repository checker")
    _run([sys.executable, str(checker)])
    _configure(arguments.preset, arguments.define)
    _build(arguments.preset, arguments.jobs)
    _test(arguments.preset, arguments.jobs)
    _message(f"check passed for {arguments.preset}")


def _run_command(arguments: argparse.Namespace) -> None:
    _build(arguments.preset, arguments.jobs)
    executable = _build_dir(arguments.preset) / "bin" / (
        "ge_headless.exe" if os.name == "nt" else "ge_headless")
    package = arguments.package or _build_dir(arguments.preset) / "assets" / "district.gepack"
    command = [str(_file(executable, "headless executable")), "--package", str(package)]
    if arguments.inspect:
        command.append("--inspect")
    if arguments.budget_bytes is not None:
        command.extend(["--budget-bytes", str(arguments.budget_bytes)])
    for option in ("restore_state", "save_state"):
        if (path := getattr(arguments, option)) is not None:
            command.extend(["--" + option.replace("_", "-"), str(path)])
    _run(command)


def _cook(arguments: argparse.Namespace) -> None:
    cooker = _file(ROOT / "tools" / "cook.py", "asset cooker")
    _run([sys.executable, str(cooker), str(arguments.source), "--output", str(arguments.output)])


def _package(arguments: argparse.Namespace) -> None:
    smoke = _file(ROOT / "tools" / "package_smoke.py", "required package relocation check")
    _configure(arguments.preset, arguments.define)
    _build(arguments.preset, arguments.jobs)
    _test(arguments.preset, arguments.jobs)
    if not _enabled(_cache(arguments.preset).get("GE_ENABLE_PACKAGING", "")):
        raise DriverError("GE_ENABLE_PACKAGING is disabled; configure with "
                          "-D GE_ENABLE_PACKAGING=ON before requesting a package.")
    build = _build_dir(arguments.preset)
    configuration = _configuration(arguments.preset)
    cpack_config = _file(build / "CPackConfig.cmake", "CPack configuration")
    _run([sys.executable, str(smoke), "--build-dir", str(build), "--config", configuration,
          "--jobs", str(arguments.jobs)])
    _run([_program("cpack"), "--config", str(cpack_config), "-C", configuration])
    archives = sorted(path for path in (build / "packages").iterdir()
                      if path.is_file() and (path.name.endswith(".zip") or path.name.endswith(".tar.gz")))
    if not archives:
        raise DriverError(f"CPack reported success but no ZIP/TGZ archives exist in {build / 'packages'}")
    print("Package artifacts:", flush=True)
    for archive in archives:
        print(f"  {archive}", flush=True)
        checksum = archive.with_name(archive.name + ".sha256")
        if checksum.is_file():
            print(f"  {checksum}", flush=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build, test, run and package the headless engine foundation.",
        epilog="Requires Python 3.11+, CMake 3.25+, Ninja and a C++20 compiler. "
               "Commands run from the repository root; explicit file paths are relative "
               "to your current directory. No dependencies are downloaded.")
    commands = parser.add_subparsers(dest="command", required=True)
    specifications = (
        ("doctor", "Validate the active build tools and compile/link a C++20 probe", _doctor),
        ("configure", "Configure a preset, applying explicit -D cache definitions", _configure_command),
        ("build", "Build a preset; configure first only when its cache is absent", _build_command),
        ("test", "Run CTest on an existing build and write out/logs/ctest-PRESET.xml", _test_command),
        ("check", "Check repository policy, configure, build, and run CTest", _check),
        ("run", "Build and run the headless lifecycle fixture", _run_command),
        ("cook", "Cook authored JSON: SOURCE --output OUTPUT", _cook),
        ("package", "Configure, build, test, check relocation, and create CPack archives", _package),
    )
    for name, description, handler in specifications:
        subparser = commands.add_parser(name, help=description, description=description)
        subparser.set_defaults(handler=handler)
        if name not in {"doctor", "cook"}:
            subparser.add_argument("--preset", choices=PRESETS,
                                   default="release" if name == "package" else "dev",
                                   help="CMake preset (default: %(default)s)")
        if name in {"build", "test", "check", "run", "package"}:
            subparser.add_argument("--jobs", "-j", type=_positive_integer, default=2,
                                   help="maximum build/test parallel jobs (default: %(default)s)")
        if name in {"configure", "check", "package"}:
            subparser.add_argument("-D", dest="define", action="append", type=_definition,
                                   default=[], metavar="NAME[:TYPE]=VALUE",
                                   help="CMake cache definition; repeat as needed, quote values with spaces")
        if name == "run":
            subparser.add_argument("--package", type=_user_path, metavar="PATH",
                                   help="use this cooked package instead of the built district fixture")
            subparser.add_argument("--inspect", action="store_true", help="print package metadata only")
            subparser.add_argument("--budget-bytes", type=_positive_integer, metavar="N",
                                   help="modeled reservation capacity (application default: 96)")
            subparser.add_argument("--restore-state", type=_user_path, metavar="PATH")
            subparser.add_argument("--save-state", type=_user_path, metavar="PATH")
        if name == "cook":
            subparser.add_argument("source", type=_user_path, help="UTF-8 v1 world authoring JSON")
            subparser.add_argument("--output", type=_user_path, required=True,
                                   help="destination GEPACK01 file")
    return parser


def main(argv: list[str] | None = None) -> int:
    # Redirected logs may use a legacy encoding. Escape diagnostic display only;
    # subprocess argument lists keep their original native Unicode paths.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    if sys.version_info < MIN_PYTHON:
        _message(f"Python 3.11+ is required; running {sys.version.split()[0]}. "
                 "Install Python 3.11+ and use that interpreter (Windows: py -3.11).")
        return 2
    arguments = _parser().parse_args(argv)
    try:
        arguments.handler(arguments)
    except subprocess.CalledProcessError as error:
        status = error.returncode if error.returncode > 0 else 128 - error.returncode
        _message(f"command failed with exit code {status}: {_command_text(error.cmd)}")
        return status
    except (DriverError, OSError) as error:
        _message(f"error: {error}")
        return 1
    except KeyboardInterrupt:
        _message("interrupted")
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
