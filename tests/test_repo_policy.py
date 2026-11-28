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

    def engine_fixture(self, *modules: str) -> None:
        """Declare a small fixed runtime build; later additions must be listed."""
        for module in modules:
            self.write(f"engine/{module}/include/ge/{module}/api.hpp", "#pragma once\n")
            self.write(f"engine/{module}/src/impl.cpp", f"#include <ge/{module}/api.hpp>\n")
        lists = {
            "GE_ENGINE_MODULES": list(modules),
            "GE_ENGINE_INCLUDE_ROOTS": [f"${{PROJECT_SOURCE_DIR}}/engine/{name}/include" for name in modules],
            "GE_ENGINE_SOURCES": [f"${{PROJECT_SOURCE_DIR}}/engine/{name}/src/impl.cpp" for name in modules],
            "GE_ENGINE_HEADERS": [f"${{PROJECT_SOURCE_DIR}}/engine/{name}/include/ge/{name}/api.hpp" for name in modules],
        }
        self.write("engine/runtime/modules.cmake", "".join(
            f"set({name}\n" + "".join(f'    "{value}"\n' for value in values) + ")\n"
            for name, values in lists.items()
        ))

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

    def test_private_integration_does_not_authorize_public_reverse_dependency(self) -> None:
        self.engine_fixture("scene", "physics")
        self.write("engine/scene/src/impl.cpp", "#include <ge/physics/api.hpp>\n")
        self.assert_passes()
        self.write("engine/scene/include/ge/scene/api.hpp", "#include <ge/physics/api.hpp>\n")
        result = self.assert_rejected("scene cannot include 'ge/physics/api.hpp'")
        self.assertIn("allowed public module roots", result.stderr)

    def test_public_dependency_cycle_reports_the_cross_module_chain(self) -> None:
        self.engine_fixture("assets", "jobs", "scene")
        self.write("engine/assets/include/ge/assets/api.hpp", "#include <ge/jobs/api.hpp>\n")
        self.write("engine/scene/include/ge/scene/api.hpp", "#include <ge/assets/api.hpp>\n")
        self.assert_passes()
        self.write("engine/jobs/include/ge/jobs/api.hpp", "#include <ge/scene/api.hpp>\n")
        result = self.assert_rejected("public module dependency cycle: assets -> jobs -> scene -> assets")
        self.assertIn("jobs cannot include", result.stderr)

    def test_unreviewed_private_engine_dependency_is_rejected(self) -> None:
        self.engine_fixture("config", "physics")
        self.write("engine/config/src/impl.cpp", "#include <ge/physics/api.hpp>\n")
        result = self.assert_rejected("config cannot include 'ge/physics/api.hpp'")
        self.assertIn("allowed private module roots", result.stderr)

    def test_runtime_cannot_import_editor_from_public_or_private_code(self) -> None:
        self.engine_fixture("render", "editor")
        for relative in ("engine/render/include/ge/render/api.hpp", "engine/render/src/impl.cpp"):
            with self.subTest(relative=relative):
                self.write(relative, "#include <ge/editor/api.hpp>\n")
                self.assert_rejected("render cannot include 'ge/editor/api.hpp'")
                self.write(relative, "// Restored fixture.\n")

    def test_core_cannot_import_engine_domains(self) -> None:
        self.engine_fixture("math")
        self.write("engine/core/src/negative.cpp", "#include <ge/math/api.hpp>\n")
        self.assert_rejected("core cannot include 'ge/math/api.hpp'")

    def test_comment_and_continuation_handling_preserves_dependency_checks(self) -> None:
        self.engine_fixture("math", "ecs")
        self.write("engine/math/include/ge/math/api.hpp", "/* #include <ge/ecs/api.hpp> */\n")
        self.assert_passes()
        self.write("engine/math/include/ge/math/api.hpp", "#include \\\n<ge/ecs/api.hpp> // Reverse edge.\n")
        self.assert_rejected("math cannot include 'ge/ecs/api.hpp'")

    def test_new_engine_source_and_header_must_be_in_explicit_manifest(self) -> None:
        self.engine_fixture("math")
        for relative, variable in (
            ("engine/math/src/new.cpp", "GE_ENGINE_SOURCES"),
            ("engine/math/include/ge/math/new.hpp", "GE_ENGINE_HEADERS"),
        ):
            with self.subTest(relative=relative):
                self.write(relative, "// New source input.\n")
                self.assert_rejected(f"{variable} omits '${{PROJECT_SOURCE_DIR}}/{relative}'")
                (self.root / relative).unlink()

    def test_engine_manifest_comments_do_not_declare_sources(self) -> None:
        self.engine_fixture("math")
        path = self.root / "engine/runtime/modules.cmake"
        text = path.read_text(encoding="utf-8")
        source = '"${PROJECT_SOURCE_DIR}/engine/math/src/impl.cpp"'
        for replacement in ("# " + source, "#[=[ " + source + " ]=]"):
            with self.subTest(replacement=replacement):
                self.write("engine/runtime/modules.cmake", text.replace(source, replacement))
                self.assert_rejected("GE_ENGINE_SOURCES omits")

    def test_engine_manifest_accepts_literal_formatting_and_comments(self) -> None:
        self.engine_fixture("math")
        path = self.root / "engine/runtime/modules.cmake"
        text = path.read_text(encoding="utf-8").replace("set(", "SET ( ")
        text = text.replace('"math"', "math # Module name\n")
        self.write("engine/runtime/modules.cmake", "#[=[ Manifest comment. ]=]\n" + text)
        self.assert_passes()

    def test_engine_manifest_rejects_missing_duplicate_and_stale_entries(self) -> None:
        self.engine_fixture("math")
        path = self.root / "engine/runtime/modules.cmake"
        text = path.read_text(encoding="utf-8")
        source = '"${PROJECT_SOURCE_DIR}/engine/math/src/impl.cpp"'
        for changed, expected in (
            (text.replace(source, source + "\n    " + source), "GE_ENGINE_SOURCES duplicates"),
            (text.replace(source, '"${PROJECT_SOURCE_DIR}/engine/math/src/missing.cpp"'), "nonexistent or misplaced entry"),
            (text.replace('    "math"\n', ""), "GE_ENGINE_MODULES omits 'math'"),
            (text.replace('    "${PROJECT_SOURCE_DIR}/engine/math/include"\n', ""), "GE_ENGINE_INCLUDE_ROOTS omits"),
        ):
            with self.subTest(expected=expected):
                self.write("engine/runtime/modules.cmake", changed)
                self.assert_rejected(expected)
        path.unlink()
        self.assert_rejected("cannot read explicit engine manifest")

    def test_engine_manifest_rejects_dynamic_or_unrelated_declarations(self) -> None:
        self.engine_fixture("math")
        path = self.root / "engine/runtime/modules.cmake"
        text = path.read_text(encoding="utf-8")
        for changed in (
            text.replace("set(GE_ENGINE_SOURCES", "set(OTHER_SOURCES"),
            text + "list(APPEND GE_ENGINE_SOURCES ${OTHER_SOURCES})\n",
            text + "set(GE_ENGINE_SOURCES)\n",
        ):
            with self.subTest(changed=changed):
                self.write("engine/runtime/modules.cmake", changed)
                self.assert_rejected("engine manifest")

    def test_application_headers_and_parent_path_bypasses_are_rejected(self) -> None:
        self.write("apps/headless/application.hpp", "#pragma once\n")
        for header in ("apps/headless/application.hpp", "../../../apps/headless/application.hpp"):
            with self.subTest(header=header):
                self.write("engine/core/src/negative.cpp", f'#include "{header}"\n')
                self.assert_rejected("undeclared header" if ".." not in header else "bypasses declared include roots")

    def test_macro_include_cannot_hide_dependency(self) -> None:
        self.write("engine/core/src/negative.cpp", '#define HEADER "ge/world/state.hpp"\n#include HEADER\n')
        self.assert_rejected("use a literal #include")

    def test_source_symlinks_cannot_alias_modules_or_loop_during_resolution(self) -> None:
        alias = self.root / "engine/core/include/ge/core/alias.hpp"
        self.write("engine/core/src/negative.cpp", "#include <ge/core/alias.hpp>\n")
        for target in (
            self.root / "engine/world/include/ge/world/state.hpp",
            Path("missing.hpp"),
            Path("alias.hpp"),
        ):
            with self.subTest(target=target.name):
                try:
                    alias.symlink_to(target)
                except (OSError, NotImplementedError) as error:
                    self.skipTest(f"source symlink fixture unavailable: {error}")
                try:
                    result = self.assert_rejected("cannot inspect a symlinked source file")
                    self.assertNotIn("Traceback", result.stderr)
                finally:
                    alias.unlink()

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
