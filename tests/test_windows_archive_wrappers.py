import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PLATFORM_ROOT = Path(__file__).resolve().parents[1]
PORTABLE_BIN = (
    PLATFORM_ROOT / ".tools" / "portable-jieli-windows" / "pi32" / "bin"
)
POWERSHELL = shutil.which("powershell.exe") or shutil.which("pwsh.exe")
CMD = shutil.which("cmd.exe")


@unittest.skipUnless(
    os.name == "nt"
    and POWERSHELL
    and CMD
    and (PORTABLE_BIN / "llvm-ar.exe").is_file()
    and (PORTABLE_BIN / "lto-ar.exe").is_file(),
    "Windows portable Jieli toolchain is required",
)
class WindowsArchiveWrapperTest(unittest.TestCase):
    def setUp(self):
        self.env = os.environ.copy()
        self.env.pop("JIELI_TOOL_DIR", None)

    def test_llvm_ar_wrapper_uses_portable_tree_without_environment(self):
        result = subprocess.run(
            [
                POWERSHELL,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(PLATFORM_ROOT / "jieli_llvm_ar.ps1"),
                "--version",
            ],
            env=self.env,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LLVM", result.stdout + result.stderr)

    def test_llvm_ar_wrapper_does_not_fall_back_for_invalid_explicit_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = self.env | {"JIELI_TOOL_DIR": temp_dir}
            result = subprocess.run(
                [
                    POWERSHELL,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PLATFORM_ROOT / "jieli_llvm_ar.ps1"),
                    "--version",
                ],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(temp_dir, result.stdout + result.stderr)

    def test_ranlib_wrapper_uses_portable_tree_without_environment(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = Path(temp_dir) / "empty.a"
            create = subprocess.run(
                [str(PORTABLE_BIN / "llvm-ar.exe"), "rcs", str(archive)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(create.returncode, 0, create.stdout + create.stderr)
            command = f"{PLATFORM_ROOT / 'jieli_ranlib.cmd'} {archive}"
            result = subprocess.run(
                [CMD, "/d", "/c", command],
                env=self.env,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ranlib_wrapper_does_not_fall_back_for_invalid_explicit_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            env = self.env | {"JIELI_TOOL_DIR": temp_dir}
            archive = Path(temp_dir) / "empty.a"
            command = f"{PLATFORM_ROOT / 'jieli_ranlib.cmd'} {archive}"
            result = subprocess.run(
                [CMD, "/d", "/c", command],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(temp_dir, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
