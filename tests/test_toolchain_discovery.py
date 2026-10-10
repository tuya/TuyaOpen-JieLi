import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.jieli_build.errors import BuildError
from tools.jieli_build.toolchain import resolve_tool_dir


_REQUIRED_TOOLS = (
    "clang.exe",
    "lto-wrapper.exe",
    "lto-ar.exe",
    "objdump.exe",
    "objsizedump.exe",
)


def _make_tool_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    for name in _REQUIRED_TOOLS:
        (path / name).touch()
    return path


class PortableToolchainDiscoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.platform_root = self.root / "platform" / "JIELI"
        self.platform_root.mkdir(parents=True)
        self.sdk_root = self.root / "sdk"
        self.sdk_root.mkdir()
        self.portable_bin = (
            self.platform_root
            / ".tools"
            / "portable-jieli-windows"
            / "pi32"
            / "bin"
        )

    def tearDown(self):
        self.temp_dir.cleanup()

    @unittest.skipUnless(os.name == "nt", "Windows portable toolchain discovery")
    def test_discovers_platform_portable_tree(self):
        _make_tool_dir(self.portable_bin)

        result = resolve_tool_dir(
            self.sdk_root, environ={}, module_root=self.platform_root
        )

        self.assertEqual(result, self.portable_bin)

    @unittest.skipUnless(os.name == "nt", "Windows portable toolchain discovery")
    def test_explicit_tool_dir_overrides_portable_tree(self):
        _make_tool_dir(self.portable_bin)
        explicit = _make_tool_dir(self.root / "custom" / "bin")

        result = resolve_tool_dir(
            self.sdk_root,
            environ={"JIELI_TOOL_DIR": str(explicit)},
            module_root=self.platform_root,
        )

        self.assertEqual(result, explicit)

    @unittest.skipUnless(os.name == "nt", "Windows portable toolchain discovery")
    def test_invalid_explicit_tool_dir_does_not_fall_back(self):
        _make_tool_dir(self.portable_bin)
        explicit = self.root / "missing" / "bin"

        with self.assertRaises(BuildError) as caught:
            resolve_tool_dir(
                self.sdk_root,
                environ={"JIELI_TOOL_DIR": str(explicit)},
                module_root=self.platform_root,
            )

        self.assertIn("JIELI_TOOL_DIR", str(caught.exception))
        self.assertIn(str(explicit), str(caught.exception))
        self.assertIn("does not contain the required tools", str(caught.exception))

    @unittest.skipUnless(os.name == "nt", "Windows portable toolchain discovery")
    def test_incomplete_portable_tree_reports_missing_tools(self):
        self.portable_bin.mkdir(parents=True)
        for name in _REQUIRED_TOOLS[:-1]:
            (self.portable_bin / name).touch()

        with self.assertRaises(BuildError) as caught:
            resolve_tool_dir(
                self.sdk_root, environ={}, module_root=self.platform_root
            )

        self.assertIn("objsizedump", str(caught.exception))

    @unittest.skipUnless(
        os.name == "nt" and shutil.which("cmake"),
        "Windows CMake toolchain selection",
    )
    def test_cmake_selects_same_portable_tree_as_python(self):
        _make_tool_dir(self.portable_bin)
        sdk_root = self.root / "vendor-sdk"
        probe = sdk_root / "apps" / "demo" / "demo_hello" / "board" / "wl82"
        probe.mkdir(parents=True)
        (probe / "Makefile").touch()
        script = self.root / "select-toolchain.cmake"
        toolchain_file = Path(__file__).resolve().parents[1] / "toolchain_file.cmake"
        script.write_text(
            "\n".join(
                (
                    'set(CONFIG_CHIP_CHOICE "wl82")',
                    f'set(PLATFORM_PATH "{self.platform_root.as_posix()}")',
                    f'include("{toolchain_file.as_posix()}")',
                    'message("SELECTED_TOOL_DIR=${JIELI_TOOL_DIR}")',
                    'message("SELECTED_C_COMPILER=${CMAKE_C_COMPILER}")',
                )
            ),
            encoding="utf-8",
        )
        def run_cmake(tool_dir=None):
            env = os.environ.copy()
            env.pop("JIELI_TOOL_DIR", None)
            env["JIELI_SDK_ROOT"] = str(sdk_root)
            if tool_dir is not None:
                env["JIELI_TOOL_DIR"] = str(tool_dir)
            return subprocess.run(
                [shutil.which("cmake"), "-P", str(script)],
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )

        result = run_cmake()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            f"SELECTED_TOOL_DIR={self.portable_bin.as_posix()}",
            result.stdout + result.stderr,
        )

        explicit = _make_tool_dir(self.root / "custom" / "bin")
        explicit_result = run_cmake(explicit)
        self.assertEqual(
            explicit_result.returncode,
            0,
            explicit_result.stdout + explicit_result.stderr,
        )
        self.assertIn(
            f"SELECTED_TOOL_DIR={explicit.as_posix()}",
            explicit_result.stdout + explicit_result.stderr,
        )
        alias_explicit = self.root / "custom-alias" / "bin"
        alias_explicit.mkdir(parents=True)
        (alias_explicit / "clang").touch()
        for name in _REQUIRED_TOOLS[1:]:
            (alias_explicit / name).touch()
        alias_result = run_cmake(alias_explicit)
        self.assertEqual(alias_result.returncode, 0, alias_result.stdout + alias_result.stderr)
        self.assertIn(
            f"SELECTED_C_COMPILER={alias_explicit.as_posix()}/clang",
            alias_result.stdout + alias_result.stderr,
        )

        invalid = self.root / "missing" / "bin"
        invalid.mkdir(parents=True)
        (invalid / "clang.exe").touch()
        invalid_result = run_cmake(invalid)
        self.assertNotEqual(invalid_result.returncode, 0)
        self.assertIn(
            "JIELI_TOOL_DIR is set",
            invalid_result.stdout + invalid_result.stderr,
        )
        for missing_tool in ("lto-wrapper", "lto-ar", "objdump", "objsizedump"):
            self.assertIn(missing_tool, invalid_result.stdout + invalid_result.stderr)

        # A partial portable tree must be skipped just like Python's resolver.
        for name in _REQUIRED_TOOLS[1:]:
            (self.portable_bin / name).unlink()
        sdk_tool_dir = (
            self.root
            / "ipc_ac7916a"
            / "toolchain"
            / "jieli-linux-toolchains"
            / "pi32v2"
            / "bin"
        )
        _make_tool_dir(sdk_tool_dir)
        fallback_result = run_cmake()
        expected = resolve_tool_dir(
            sdk_root, environ={}, module_root=self.platform_root
        )
        self.assertEqual(fallback_result.returncode, 0, fallback_result.stdout + fallback_result.stderr)
        output = fallback_result.stdout + fallback_result.stderr
        selected = output.split("SELECTED_TOOL_DIR=", 1)[1].splitlines()[0].strip()
        self.assertEqual(Path(selected).resolve(), expected.resolve())


if __name__ == "__main__":
    unittest.main()
