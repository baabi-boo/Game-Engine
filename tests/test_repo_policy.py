#!/usr/bin/env python3
"""Exercise repository policy failures in isolated, untracked source trees."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).resolve().parents[1] / "tools" / "check_repo.py"
PIN = "a" * 40
WORKFLOW = f"""name: Fixture
on:
  pull_request:
  workflow_dispatch:
permissions:
  contents: read
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@{PIN}
      - run: |
          echo 'uses: this is a shell string, not an action'
"""


class RepositoryPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ge-repo-policy-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\nproject(Fixture LANGUAGES CXX)\n")
        self.write("CMakePresets.json", '{"version": 6}\n')
        self.write(".github/workflows/ci.yml", WORKFLOW)
        self.write("engine/core/include/ge/core/identity.hpp", "#pragma once\n#include <cstdint>\n")
        self.write("engine/content/include/ge/content/package.hpp", "#pragma once\n#include <ge/core/identity.hpp>\n")
        self.write("engine/world/include/ge/world/state.hpp", "#pragma once\n#include <ge/core/identity.hpp>\n")
        self.write("README.md", "# Fixture\n\n[Core](engine/core/include/ge/core/identity.hpp)\n")

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_check(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CHECKER), "--root", str(self.root)],
            cwd=self.root, text=True, capture_output=True, timeout=30, check=False,
        )

    def assert_passes(self) -> None:
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Repository checks passed", result.stdout)

    def assert_rejected(self, message: str) -> subprocess.CompletedProcess[str]:
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        return result

    def test_valid_untracked_checkout_passes(self) -> None:
        self.assertFalse((self.root / ".git").exists())
        self.assert_passes()

    def test_default_root_is_relative_to_script_not_working_directory(self) -> None:
        script = self.root / "tools" / "check_repo.py"
        script.parent.mkdir()
        shutil.copyfile(CHECKER, script)
        with tempfile.TemporaryDirectory(prefix="ge-repo-cwd-") as elsewhere:
            result = subprocess.run(
                [sys.executable, str(script)], cwd=elsewhere,
                text=True, capture_output=True, timeout=30, check=False,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reverse_and_sibling_module_includes_are_rejected(self) -> None:
        cases = (("core", "world", "state.hpp"), ("content", "world", "state.hpp"),
                 ("world", "content", "package.hpp"))
        for module, dependency, header in cases:
            with self.subTest(module=module, dependency=dependency):
                relative = f"engine/{module}/src/negative.cpp"
                self.write(relative, f"#include <ge/{dependency}/{header}>\n")
                self.assert_rejected(f"{module} cannot include")
                (self.root / relative).unlink()

    def test_application_headers_and_parent_path_bypasses_are_rejected(self) -> None:
        self.write("apps/headless/application.hpp", "#pragma once\n")
        for header in ("apps/headless/application.hpp", "../../../apps/headless/application.hpp"):
            with self.subTest(header=header):
                self.write("engine/core/src/negative.cpp", f'#include "{header}"\n')
                self.assert_rejected("undeclared header" if ".." not in header else "bypasses declared include roots")

    def test_macro_include_cannot_hide_dependency(self) -> None:
        self.write("engine/core/src/negative.cpp", '#define HEADER "ge/world/state.hpp"\n#include HEADER\n')
        self.assert_rejected("use a literal #include")

    def test_reviewed_platform_adapter_does_not_leak_into_public_headers(self) -> None:
        self.write("engine/world/src/state.cpp", "#include <ge/world/state.hpp>\n#ifdef _WIN32\n#include <windows.h>\n#endif\n")
        self.assert_passes()
        self.write("engine/world/include/ge/world/state.hpp", "#include <windows.h>\n")
        self.assert_rejected("undeclared header 'windows.h'")

    def test_unpinned_remote_action_is_rejected(self) -> None:
        self.write(".github/workflows/ci.yml", WORKFLOW.replace(PIN, "v4"))
        self.assert_rejected("pinned to a full 40-hex commit SHA")

    def test_privileged_workflows_and_permissions_are_rejected(self) -> None:
        for workflow, expected in (
            (WORKFLOW.replace("pull_request:", "pull_request_target:"), "outside the unprivileged CI policy"),
            (WORKFLOW.replace("pull_request:", "workflow_run:"), "outside the unprivileged CI policy"),
            (WORKFLOW.replace("contents: read", "contents: write"), "may grant only contents: read"),
            (WORKFLOW.replace("permissions:\n  contents: read\n", ""), "declare top-level permissions explicitly"),
        ):
            with self.subTest(expected=expected):
                self.write(".github/workflows/ci.yml", workflow)
                self.assert_rejected(expected)

    def test_broken_local_file_links_are_rejected(self) -> None:
        for link in ("[Missing](docs/missing.md#section)", "[guide]: docs/missing.md"):
            with self.subTest(link=link):
                self.write("README.md", f"# Fixture\n\n{link}\n")
                self.assert_rejected("Markdown file destination does not exist: 'docs/missing.md")

    def test_generated_outputs_fences_and_external_links_are_not_local_sources(self) -> None:
        self.write("out/bad.json", "not JSON and no newline")
        self.write(".git/bad.md", "[missing](missing.md)")
        self.write("tools/__pycache__/cache.py", "invalid cache  ")
        self.write("README.md", """# Fixture

[External](https://example.invalid/not-checked)
[Anchor](#a-heading)
[Generated report](out/reports/not-yet-generated.json)

```markdown
[Example](does-not-exist.md)
```
""")
        self.assert_passes()

    def test_cmake_download_and_reverse_link_are_rejected(self) -> None:
        original = (self.root / "CMakeLists.txt").read_text(encoding="utf-8")
        for command, expected in (
            ('file(DOWNLOAD "https://example.invalid/dependency.zip" dependency.zip)\n', "CMake dependency download/bootstrap is forbidden"),
            ("target_link_libraries(ge_core INTERFACE ge_world)\n", "undeclared link dependency"),
        ):
            with self.subTest(command=command):
                self.write("CMakeLists.txt", original + command)
                self.assert_rejected(expected)

    def test_malformed_json_and_missing_required_input_fail(self) -> None:
        self.write("CMakePresets.json", '{"version": }\n')
        self.assert_rejected("invalid JSON")
        (self.root / "CMakePresets.json").unlink()
        self.assert_rejected("required build configuration is missing")

    def test_text_hygiene_is_checked_in_untracked_source(self) -> None:
        self.write("tools/new_source.py", "value = 1  \n")
        self.assert_rejected("remove trailing whitespace")
        self.write("tools/new_source.py", "value = 1")
        self.assert_rejected("text file must end with a newline")

    def test_private_material_is_reported_without_echoing_secret(self) -> None:
        token = "gh" + "p_" + "a" * 36
        self.write("private.txt", token + "\n")
        result = self.assert_rejected("possible GitHub token")
        self.assertNotIn(token, result.stderr + result.stdout)
        self.write("private.txt", "https://chatgpt.com/" + "library?file_id=example\n")
        self.assert_rejected("possible private Library link")


if __name__ == "__main__":
    unittest.main()
