#!/usr/bin/env python3
"""Check source policies offline with Python 3.11's standard library.

This is a deliberately scoped lexical check, not a C++ preprocessor, CMake
interpreter, YAML validator, or complete secret scanner. It checks the current
module contracts, known dependency downloads, explicit workflow permissions and
action revisions, text hygiene, JSON syntax, and local Markdown destinations.
Untracked source is included; generated output and local caches are excluded.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
MODULES = {"core": {"core"}, "content": {"content", "core"}, "world": {"world", "core"}}
# These domains are compiled together in ge_engine. The base utility domains
# retain narrower boundaries, and no core/content/world reverse edge is added.
ENGINE_DOMAINS = {
    "math", "ecs", "jobs", "io", "scene", "input", "assets", "geometry",
    "render", "spatial", "physics", "animation", "audio", "navigation",
    "terrain", "effects", "script", "gameplay", "config", "text", "network",
    "persistence", "ai", "traffic", "editor", "environment", "procedural",
    "cinematic", "diagnostics", "platform", "ui", "runtime",
}
MODULES.update({name: ENGINE_DOMAINS | {"core", "content", "world"}
                for name in ENGINE_DOMAINS})
MODULES.update({
    "math": {"math", "core"},
    "ecs": {"ecs", "math", "core"},
    "jobs": {"jobs", "core"},
    "io": {"io", "jobs", "core"},
})
IGNORED_DIRS = {
    ".git", ".venv", "venv", "__pycache__", ".pytest_cache", ".mypy_cache",
    ".ruff_cache", ".cache", ".idea", ".vscode", "node_modules", "CMakeFiles",
}
ROOT_OUTPUTS = {"out", "build", "Testing"}
IGNORED_FILES = {".git", ".DS_Store", "Thumbs.db", "CMakeUserPresets.json"}
ROOT_GENERATED = {
    "CMakeCache.txt", "cmake_install.cmake", "install_manifest.txt", "compile_commands.json",
}
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".tpp",
    ".py", ".json", ".md", ".txt", ".yaml", ".yml", ".cmake", ".in",
    ".sh", ".ps1", ".toml", ".ini", ".cfg",
}
TEXT_NAMES = {"CODEOWNERS", ".gitignore", ".gitattributes", ".editorconfig", ".clang-format"}
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".tpp"}
STANDARD_HEADERS = set("""
algorithm any array atomic barrier bit bitset cassert ccomplex cctype cerrno cfenv
cfloat charconv chrono cinttypes ciso646 climits clocale cmath codecvt compare complex
concepts condition_variable coroutine csetjmp csignal cstdalign cstdarg cstdbool
cstddef cstdint cstdio cstdlib cstring ctgmath ctime cuchar cwchar cwctype deque
exception execution filesystem format forward_list fstream functional future
initializer_list iomanip ios iosfwd iostream istream iterator latch limits list
locale map memory memory_resource mutex new numbers numeric optional ostream queue
random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location
span sstream stack stdexcept stop_token streambuf string string_view strstream
syncstream system_error thread tuple type_traits typeindex typeinfo unordered_map
unordered_set utility valarray variant vector version
assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h
math.h setjmp.h signal.h stdalign.h stdarg.h stdbool.h stddef.h stdint.h stdio.h
stdlib.h string.h tgmath.h time.h uchar.h wchar.h wctype.h
""".split())
PRIVATE_PLATFORM_HEADERS = {
    "engine/world/src/state.cpp": {"windows.h"},
    "engine/network/src/transport.cpp": {
        "winsock2.h", "ws2tcpip.h", "windows.h", "sys/socket.h", "sys/types.h",
        "netinet/in.h", "arpa/inet.h", "netdb.h", "unistd.h", "fcntl.h", "poll.h",
    },
}
RESEARCH_SNAPSHOT = "docs/research/open-world-blueprint.md"

SECRET_PATTERNS = (
    (r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----", "private key material"),
    (r"\bgh[pousr]_[A-Za-z0-9]{36,}\b|\bgithub_pat_[A-Za-z0-9_]{60,}\b", "GitHub token"),
    (r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b", "AWS access key identifier"),
    (r"\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}\b", "API secret token"),
    (r"\bxox[baprs]-[0-9A-Za-z-]{20,}\b", "Slack token"),
    (r"https?://[^\s/:@]+:[^\s/@]+@", "credential-bearing URL"),
    (r"https?://(?:www\.)?chatgpt\.com/library(?:[/?#]|\b)", "private Library link"),
    (r"(?:oai-library|library-file|mcp-resource):|\bsandbox:/", "private workspace link"),
    (r"/(?:workspace/scratch|mnt/data)/|/(?:[h]ome|[U]sers)/[A-Za-z0-9_.-]+/|[A-Za-z]:\\[U]sers\\[^\\\s]+\\", "personal workspace path"),
)
DOWNLOAD_PATTERNS = (
    r"\b(?:FetchContent_(?:Declare|MakeAvailable|Populate)|ExternalProject_Add)\s*\(",
    r"\binclude\s*\(\s*\"?(?:FetchContent|ExternalProject)\b",
    r"\bfile\s*\(\s*(?:DOWNLOAD|UPLOAD)\b",
    r"\b(?:curl|wget)(?:\.exe)?(?:\s|\")",
    r"\bgit(?:\.exe)?\s+(?:clone|fetch|pull)\b",
    r"\b(?:Invoke-WebRequest|Invoke-RestMethod|Start-BitsTransfer)\b",
    r"\b(?:pip3?|npm|pnpm|yarn|conan|vcpkg|nuget)\s+(?:install|add|update)\b",
)
YAML_KEY = re.compile(r"^(?:-\s+)?[\"']?([A-Za-z_][A-Za-z0-9_-]*)[\"']?\s*:\s*(.*)$")
INLINE_LINK = re.compile(r"!?\[[^\]\n]*\]\(\s*(?:<([^>\n]+)>|((?:\\.|[^\s()\\]|\([^()\n]*\))+))")
REFERENCE_LINK = re.compile(r"^ {0,3}\[[^\]\n]+\]:\s*(?:<([^>\n]+)>|(\S+))", re.MULTILINE)


def masked(text: str) -> str:
    """Remove content while retaining offsets and line numbers."""
    return re.sub(r"[^\n]", " ", text)


def reject_json_constant(value: str) -> None:
    raise ValueError(f"non-JSON number {value}")


def without_comments(text: str, language: str) -> str:
    if language == "cpp":
        pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
        return re.sub(pattern, lambda m: masked(m[0]) if m[0].startswith(("//", "/*")) else m[0], text)
    pattern = r'"(?:\\.|[^"\\])*"|#\[(=*)\[[\s\S]*?\]\1\]|#[^\n]*'
    return re.sub(pattern, lambda m: masked(m[0]) if m[0].startswith("#") else m[0], text)


def yaml_lines(text: str) -> list[tuple[int, int, str]]:
    """Read block-style workflow declarations, excluding comments and scalar bodies."""
    result = []
    scalar_indent = None
    for number, line in enumerate(text.splitlines(), 1):
        indent = len(line) - len(line.lstrip())
        if scalar_indent is not None:
            if not line.strip() or indent > scalar_indent:
                continue
            scalar_indent = None
        # Quoted '#' characters are values, not comments.
        clean = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\'\'|[^\'])*\'|\s+#.*$|^\s*#.*$',
                       lambda m: "" if m[0].lstrip().startswith("#") else m[0], line).strip()
        if not clean:
            continue
        result.append((number, indent, clean))
        if re.search(r":\s*[|>][+-]?[1-9]?\s*$", clean):
            scalar_indent = indent
    return result


class RepositoryCheck:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.errors: list[str] = []
        self.text_count = 0
        self.workflow_count = 0

    def error(self, path: Path, line: int, message: str) -> None:
        relative = path.relative_to(self.root).as_posix()
        self.errors.append(f"{relative}:{line}: {message}")

    def source_files(self):
        def walk_error(error: OSError) -> None:
            self.errors.append(f"{error.filename}: cannot inspect source directory: {error.strerror}")

        for directory, dirs, files in os.walk(self.root, onerror=walk_error, followlinks=False):
            parent = Path(directory)
            dirs[:] = sorted(name for name in dirs if name not in IGNORED_DIRS
                             and not (parent == self.root and name in ROOT_OUTPUTS))
            for name in dirs[:]:
                path = parent / name
                if path.is_symlink():
                    self.error(path, 1, "cannot inspect a symlinked source directory; use ordinary source directories")
                    dirs.remove(name)
            for name in sorted(files):
                if name in IGNORED_FILES or name.endswith((".pyc", ".pyo")):
                    continue
                if parent == self.root and name in ROOT_GENERATED:
                    continue
                path = parent / name
                if path.is_symlink() and not path.resolve().is_relative_to(self.root):
                    self.error(path, 1, "source symlink points outside the repository")
                    continue
                yield path

    def check_text(self, path: Path, text: str) -> None:
        relative = path.relative_to(self.root).as_posix()
        if text and not text.endswith("\n"):
            self.error(path, len(text.splitlines()) or 1, "text file must end with a newline")
        for number, line in enumerate(text.splitlines(), 1):
            if line != line.rstrip(" \t"):
                hard_break = relative == RESEARCH_SNAPSHOT and line.endswith("  ") and "\t" not in line[len(line.rstrip(" \t")):]
                if not hard_break:
                    self.error(path, number, "remove trailing whitespace")
        for pattern, category in SECRET_PATTERNS:
            for match in re.finditer(pattern, text):
                self.error(path, text.count("\n", 0, match.start()) + 1,
                           f"possible {category}; remove private data from public source")

    def check_includes(self, path: Path, text: str, module: str) -> None:
        # Join preprocessor continuation lines before stripping comments.
        logical, line_numbers = [], []
        pending = ""
        start = 1
        for number, line in enumerate(text.splitlines(), 1):
            if not pending:
                start = number
            if line.endswith("\\"):
                pending += line[:-1]
            else:
                logical.append(pending + line)
                line_numbers.append(start)
                pending = ""
        if pending:
            logical.append(pending)
            line_numbers.append(start)
        clean = without_comments("\n".join(logical), "cpp")
        for number, line in zip(line_numbers, clean.split("\n")):
            directive = re.match(r"^\s*#\s*(include(?:_next)?|import)\b(.*)$", line)
            if not directive:
                continue
            literal = re.fullmatch(r'\s*([<"])([^>"\n]+)[>"]\s*', directive[2])
            if directive[1] != "include" or not literal:
                self.error(path, number, "use a literal #include so module dependencies can be checked")
                continue
            header = literal[2]
            parts = header.replace("\\", "/").split("/")
            if ".." in parts or "\\" in header or header.startswith("/") or re.match(r"^[A-Za-z]:", header):
                self.error(path, number, f"include '{header}' bypasses declared include roots; no absolute or parent paths")
                continue
            if parts[0] == "ge":
                dependency = parts[1] if len(parts) > 1 else ""
                if dependency not in MODULES[module]:
                    allowed = ", ".join(f"ge/{name}" for name in sorted(MODULES[module]))
                    self.error(path, number, f"{module} cannot include '{header}'; allowed module roots: {allowed}")
                elif not (self.root / "engine" / dependency / "include" / header).is_file():
                    self.error(path, number, f"declared public header does not exist: '{header}'")
                continue
            if header in STANDARD_HEADERS:
                continue
            if header in PRIVATE_PLATFORM_HEADERS.get(path.relative_to(self.root).as_posix(), set()):
                continue
            local = (path.parent / header).resolve()
            if literal[1] == '"' and local.is_relative_to(self.root / "engine" / module) and local.is_file():
                continue
            self.error(path, number, f"undeclared header '{header}' in {module}; use its own/core public headers or the standard library")

    def check_cmake(self, path: Path, text: str) -> None:
        clean = without_comments(text, "cmake")
        for pattern in DOWNLOAD_PATTERNS:
            for match in re.finditer(pattern, clean, re.IGNORECASE):
                self.error(path, clean.count("\n", 0, match.start()) + 1,
                           "CMake dependency download/bootstrap is forbidden; keep provisioning explicit and the build offline")
        for match in re.finditer(r'\b(?i:target_link_libraries)\s*\(\s*"?(ge_core|ge_content|ge_world)"?\s+([^)]*)\)', clean):
            module = match[1].removeprefix("ge_")
            allowed = {"PRIVATE", "PUBLIC", "INTERFACE"}
            if module == "core":
                # CMake's platform support for the C++ standard thread library.
                allowed.add("Threads::Threads")
            allowed.update(f"ge_{name}" for name in MODULES[module] if name != module)
            allowed.update(f"ge::{name}" for name in MODULES[module] if name != module)
            for dependency in re.findall(r'"[^"]*"|\S+', match[2]):
                if dependency.strip('"') not in allowed:
                    self.error(path, clean.count("\n", 0, match.start()) + 1,
                               f"{match[1]} has undeclared link dependency '{dependency}'; preserve module dependency direction")

    def check_workflow(self, path: Path, text: str, workflow: bool) -> None:
        lines = yaml_lines(text)
        top_permissions = False
        top_events = False
        for index, (number, indent, line) in enumerate(lines):
            mapping = YAML_KEY.match(line)
            if not mapping:
                if re.search(r"[\"']?(?:uses|permissions)[\"']?\s*:", line):
                    self.error(path, number, "use block mappings for action uses and permissions so policy checks are explicit")
                continue
            key, value = mapping.groups()
            value = value.strip().strip('"\'')
            if key not in {"uses", "permissions"} and value.startswith(("{", "[")) and re.search(r"[\"']?(?:uses|permissions)[\"']?\s*:", value):
                self.error(path, number, "use block mappings for action uses and permissions so policy checks are explicit")
            children = []
            for child in lines[index + 1:]:
                if child[1] <= indent:
                    break
                children.append(child)
            if key == "uses":
                if value.startswith("./"):
                    local = (self.root / value).resolve()
                    if not local.is_relative_to(self.root) or not local.exists():
                        self.error(path, number, "local action/workflow path must exist inside the repository")
                elif not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_./-]+)?@[0-9a-fA-F]{40}", value):
                    self.error(path, number, "remote action/workflow uses must be pinned to a full 40-hex commit SHA")
            elif key == "permissions":
                top_permissions |= indent == 0
                if value == "{}":
                    continue
                if value:
                    self.error(path, number, "permissions must be {} or an explicit block granting at most contents: read")
                    continue
                if not children:
                    self.error(path, number, "empty permissions declaration; use {} to grant no permissions")
                for child_number, _, child_line in children:
                    permission = YAML_KEY.fullmatch(child_line)
                    level = permission[2].strip('"\'') if permission else ""
                    if not permission or not (level == "none" or (permission[1], level) == ("contents", "read")):
                        self.error(path, child_number, "workflow permissions may grant only contents: read; other scopes must be none")
            elif workflow and key == "on" and indent == 0:
                top_events = True
                events = []
                if value:
                    events = [(number, event.strip().strip('"\'')) for event in value.strip("[]").split(",")]
                elif children:
                    first_indent = min(child[1] for child in children)
                    for child_number, child_indent, child_line in children:
                        if child_indent == first_indent:
                            event = YAML_KEY.match(child_line)
                            events.append((child_number, event[1] if event else child_line.removeprefix("- ")))
                if not events:
                    self.error(path, number, "workflow must declare explicit event triggers")
                for event_number, event in events:
                    if event not in {"push", "pull_request", "workflow_dispatch"}:
                        self.error(path, event_number, f"workflow trigger '{event}' is outside the unprivileged CI policy")
        if workflow:
            self.workflow_count += 1
            if not top_permissions:
                self.error(path, 1, "declare top-level permissions explicitly (contents: read or {})")
            if not top_events:
                self.error(path, 1, "declare workflow triggers explicitly using on:")

    def check_markdown(self, path: Path, text: str) -> None:
        visible = []
        fence = None
        for line in text.splitlines(keepends=True):
            marker = re.match(r"^ {0,3}(`{3,}|~{3,})", line)
            if fence:
                visible.append(masked(line))
                if marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence) and not line[marker.end():].strip():
                    fence = None
            elif marker:
                fence = marker[1]
                visible.append(masked(line))
            else:
                visible.append(line)
        clean = re.sub(r"<!--[\s\S]*?-->|(`+)[^`]*?\1", lambda m: masked(m[0]), "".join(visible))
        for pattern in (INLINE_LINK, REFERENCE_LINK):
            for match in pattern.finditer(clean):
                target = match[1] or match[2]
                if target.startswith(("#", "//")) or re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target):
                    continue
                filename = unquote(re.split(r"[?#]", target, maxsplit=1)[0])
                filename = re.sub(r"\\([\\`*{}\[\]()#+.! _-])", r"\1", filename)
                if not filename:
                    continue
                destination = (self.root / filename.lstrip("/") if filename.startswith("/") else path.parent / filename).resolve()
                number = clean.count("\n", 0, match.start()) + 1
                if not destination.is_relative_to(self.root):
                    self.error(path, number, f"Markdown destination leaves the repository: '{target}'")
                elif destination.relative_to(self.root).parts[:1] == ("out",):
                    continue
                elif not destination.exists():
                    self.error(path, number, f"Markdown file destination does not exist: '{target}'")

    def run(self) -> list[str]:
        for required in ("CMakeLists.txt", "CMakePresets.json"):
            if not (self.root / required).is_file():
                self.error(self.root / required, 1, "required build configuration is missing")
        for path in self.source_files():
            try:
                data = path.read_bytes()
            except OSError as error:
                self.error(path, 1, f"cannot read source file: {error.strerror}")
                continue
            try:
                text = data.decode("utf-8")
                if "\0" in text:
                    raise UnicodeError("NUL in text")
            except UnicodeError:
                if path.suffix.lower() in TEXT_SUFFIXES or path.name in TEXT_NAMES:
                    self.error(path, 1, "source text must be valid UTF-8 without NUL bytes")
                continue
            self.text_count += 1
            self.check_text(path, text)
            relative = path.relative_to(self.root)
            if path.suffix.lower() == ".json":
                try:
                    json.loads(text, parse_constant=reject_json_constant)
                except (json.JSONDecodeError, ValueError) as error:
                    self.error(path, getattr(error, "lineno", 1), f"invalid JSON: {error}")
            if path.suffix.lower() in CPP_SUFFIXES and len(relative.parts) >= 3 and relative.parts[0] == "engine":
                module = relative.parts[1]
                if module not in MODULES:
                    self.error(path, 1, f"module '{module}' has no declared dependency policy")
                else:
                    self.check_includes(path, text, module)
            if path.name == "CMakeLists.txt" or ".cmake" in path.suffixes:
                self.check_cmake(path, text)
            workflow = relative.parts[:2] == (".github", "workflows") and path.suffix.lower() in {".yaml", ".yml"}
            if workflow or path.name in {"action.yml", "action.yaml"}:
                self.check_workflow(path, text, workflow)
            if path.suffix.lower() == ".md":
                self.check_markdown(path, text)
        if self.workflow_count == 0:
            self.error(self.root / ".github/workflows", 1, "no workflow found; required CI policy checks cannot run")
        return self.errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this script's parent repository)")
    args = parser.parse_args()
    if not args.root.is_dir():
        parser.error(f"repository root is not a directory: {args.root}")
    checker = RepositoryCheck(args.root)
    errors = checker.run()
    for error in errors:
        print(f"ERROR {error}", file=sys.stderr)
    if errors:
        print(f"Repository checks failed: {len(errors)} error(s), {checker.text_count} source text files checked.", file=sys.stderr)
        return 1
    print(f"Repository checks passed: {checker.text_count} source text files, {checker.workflow_count} workflow(s); offline lexical policy checks.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
