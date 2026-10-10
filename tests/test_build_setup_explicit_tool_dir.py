import contextlib
import io
import os
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import build_setup
from tools.jieli_build.errors import BuildError


@unittest.skipUnless(os.name == "nt", "Windows installer fallback behavior")
class BuildSetupExplicitToolDirTest(unittest.TestCase):
    def setUp(self):
        self.sdk_root = Path("C:/fake-jieli-sdk")
        self.resolver = patch.object(
            build_setup,
            "resolve_tool_dir",
            side_effect=BuildError("invalid explicit tool directory"),
        )
        self.resolver.start()
        self.addCleanup(self.resolver.stop)
        self.chip = patch.object(
            build_setup, "resolve_chip", return_value=SimpleNamespace(name="wl83")
        )
        self.chip.start()
        self.addCleanup(self.chip.stop)
        self.sdk = patch.object(build_setup, "resolve_sdk_root", return_value=self.sdk_root)
        self.sdk.start()
        self.addCleanup(self.sdk.stop)

    def test_empty_explicit_path_does_not_download_installer(self):
        self._assert_explicit_path_does_not_download("")

    def test_invalid_explicit_path_does_not_download_installer(self):
        self._assert_explicit_path_does_not_download("C:/missing/pi32/bin")

    def _assert_explicit_path_does_not_download(self, value):
        with patch.dict(os.environ, {"JIELI_TOOL_DIR": value}):
            with patch.object(build_setup, "download_windows_toolchain_installer") as download:
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    result = build_setup.main(
                        ["build_setup.py", "switch_demo", "JIELI", "base", "wl83"]
                    )

        self.assertEqual(result, 1)
        download.assert_not_called()
        self.assertIn("invalid explicit tool directory", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
