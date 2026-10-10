import os
import tempfile
import unittest
from pathlib import Path

from platform_flash_bridge import (
    _FLASH_CHIPS,
    _default_flash_command,
    _resolve_flash_chip,
    _staging_tools_dir,
)

# isd_config.ini / uboot.boot / cfg_tool.bin are produced by the vendor build
# into the app's staging tree; only isd_download.exe ships in the SDK checkout.
_GENERATED = ("isd_config.ini", "uboot.boot", "cfg_tool.bin")

# The staging tree mirrors the SDK layout, so the sub-path differs per chip
# (wl82: build/cpu/wl82/tools, wl83: build/sdk/cpu/wl83/tools). Take it from the
# same table the production lookup uses instead of restating it, so this test
# cannot drift away from the code it is meant to check.
_CHIPS = sorted(_FLASH_CHIPS)


class StagingToolsDirTest(unittest.TestCase):
    def _staging_tools(self, root: Path, chip: str) -> Path:
        return root / ".build" / "jieli-staging" / "build" / _FLASH_CHIPS[chip][1]

    def _make_app(self, root: Path, chip: str) -> Path:
        tools = self._staging_tools(root, chip)
        tools.mkdir(parents=True)
        for name in _GENERATED:
            (tools / name).write_text("x", encoding="utf-8")
        image = root / "dist" / "proj_1.0.0" / "proj_QIO_1.0.0.bin"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"\0")
        return image

    def test_every_chip_resolves_from_a_dist_image(self):
        for chip in _CHIPS:
            with self.subTest(chip=chip), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                image = self._make_app(root, chip)
                self.assertEqual(_staging_tools_dir(image, chip),
                                 self._staging_tools(root, chip))

    def test_returns_none_when_the_build_never_ran(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / "dist" / "proj_1.0.0" / "proj_QIO_1.0.0.bin"
            image.parent.mkdir(parents=True)
            image.write_bytes(b"\0")
            self.assertIsNone(_staging_tools_dir(image, "wl82"))

    def test_unknown_chip_is_not_a_crash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = self._make_app(root, "wl82")
            self.assertIsNone(_staging_tools_dir(image, "wl99"))

    def test_a_temp_copy_still_resolves_through_its_origin(self):
        # A project name longer than 15 characters is flashed from a temporary
        # app.bin, so the staging tree can only be found via the original path.
        for chip in _CHIPS:
            with self.subTest(chip=chip), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                image = self._make_app(root, chip)
                tools = self._staging_tools(root, chip)
                (tools / "isd_download.exe").write_bytes(b"\0")

                resolved = _resolve_flash_chip(chip, image)
                self.assertIsNotNone(resolved)
                self.assertEqual(resolved[1], tools)

                if os.name == "nt":
                    with tempfile.TemporaryDirectory() as other:
                        copy = Path(other) / "app.bin"
                        copy.write_bytes(b"\0")
                        command = _default_flash_command(copy, chip, origin=image)
                        self.assertIsNotNone(command)
                        argv, working_dir = command
                        self.assertEqual(working_dir, tools)
                        self.assertIn(str(copy), argv)
                        self.assertIn(str(tools / "isd_config.ini"), argv)
                        self.assertIn(chip, argv)


if __name__ == "__main__":
    unittest.main()
