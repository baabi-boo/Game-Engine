#!/usr/bin/env python3
"""Check private install destinations without configuring or building CMake."""

from __future__ import annotations

import importlib.util
from pathlib import Path, PurePosixPath, PureWindowsPath
import unittest
from unittest import mock


TOOL = Path(__file__).resolve().parents[1] / "tools/package_smoke.py"
SPEC = importlib.util.spec_from_file_location("ge_package_contract_audit", TOOL)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load package smoke checker: {TOOL}")
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


class PackageContractAuditTests(unittest.TestCase):
    def test_windows_destinations_cannot_discard_the_private_prefix(self) -> None:
        prefix = PureWindowsPath("D:/private/moved")
        for value in (r"\outside", "C:outside", "C:/outside", r"\\server\share\outside"):
            with self.subTest(value=value), mock.patch.object(smoke, "Path", PureWindowsPath):
                self.assertFalse((prefix / value).is_relative_to(prefix))
                with self.assertRaisesRegex(smoke.SmokeError, "inside the installation prefix"):
                    smoke._relative_install_dir({"CMAKE_INSTALL_LIBDIR": value},
                                                "CMAKE_INSTALL_LIBDIR", "lib")

    def test_relative_install_layouts_remain_supported(self) -> None:
        for path_type, value in ((PurePosixPath, "lib/x86_64-linux-gnu"),
                                 (PureWindowsPath, r"lib\nested")):
            with self.subTest(value=value), mock.patch.object(smoke, "Path", path_type):
                self.assertEqual(smoke._relative_install_dir({"CMAKE_INSTALL_LIBDIR": value},
                                                            "CMAKE_INSTALL_LIBDIR", "lib"),
                                 path_type(value))

    def test_invalid_destinations_fail_before_filesystem_operations(self) -> None:
        for path_type in (PurePosixPath, PureWindowsPath):
            for value in ("../outside", "/outside", "lib;outside", "lib\0outside"):
                with self.subTest(path_type=path_type.__name__, value=value), \
                        mock.patch.object(smoke, "Path", path_type):
                    with self.assertRaisesRegex(smoke.SmokeError, "inside the installation prefix"):
                        smoke._relative_install_dir({"CMAKE_INSTALL_LIBDIR": value},
                                                    "CMAKE_INSTALL_LIBDIR", "lib")

    def test_checkout_build_tool_is_still_rejected(self) -> None:
        checkout = Path("/engine-checkout")
        arguments = smoke._consumer_configuration({
            "CMAKE_GENERATOR": "Ninja",
            "CMAKE_MAKE_PROGRAM": str(checkout / "out/toolchain/bin/ninja"),
        }, "Release")
        with self.assertRaisesRegex(smoke.SmokeError, "original source/build/staging path"):
            smoke._reject_checkout_paths("\n".join(arguments), (checkout,),
                                         "consumer toolchain configuration")


if __name__ == "__main__":
    unittest.main()
