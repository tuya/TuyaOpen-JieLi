import tempfile
import unittest
from pathlib import Path, PureWindowsPath

from tools.jieli_build.chip_profiles import JIELI_CHIPS
from tools.jieli_build.sdk_overlay import (
    build_make_command,
    clean_staging_tree,
    drop_vendor_example_sources,
    read_adapter_sources,
    stage_sdk_inputs,
)


class JieliSdkOverlayTest(unittest.TestCase):
    def _make_sdk(self, root: Path) -> Path:
        sdk = root / "sdk"
        files = {
            "include_lib/vendor.h": "header",
            "lib/vendor.a": "library",
            "tools/utils/fixbat.exe": "tool",
            "apps/common/movable/section.c": "generated source",
            "apps/common/movable/section.txt": "stale generated sections",
            "apps/common/net/wifi.c": "common source",
            "apps/demo/demo_hello/app_main.c": "demo source",
            "cpu/wl82/sdk_ld.c": "linker source",
            "cpu/wl82/sdk.ld": "stale generated linker script",
            "cpu/wl82/liba/cpu.a": "archive",
            "cpu/wl82/tools/download.c": "postbuild source",
            "cpu/wl82/tools/app.bin": "stale image",
        }
        for name, content in files.items():
            target = sdk / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        return sdk

    def test_staging_copies_writable_sdk_paths_and_links_read_only_inputs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            sdk = self._make_sdk(root)
            build_root = root / "staging" / "build"

            stage_sdk_inputs(sdk, build_root, JIELI_CHIPS["wl82"])

            self.assertEqual(
                (build_root / "cpu/wl82/sdk_ld.c").read_text(encoding="utf-8"),
                "linker source",
            )
            self.assertFalse((build_root / "cpu/wl82/sdk.ld").exists())
            self.assertFalse((build_root / "cpu/wl82/tools/app.bin").exists())
            self.assertFalse((build_root / "apps/common/movable/section.txt").exists())
            self.assertEqual(
                (build_root / "cpu/wl82/tools/download.c").read_text(encoding="utf-8"),
                "postbuild source",
            )
            self.assertEqual(
                (build_root / "apps/common/movable/section.c").read_text(encoding="utf-8"),
                "generated source",
            )
            self.assertEqual(
                (build_root / "apps/common/net/wifi.c").read_text(encoding="utf-8"),
                "common source",
            )
            self.assertTrue((build_root / "include_lib/vendor.h").is_file())
            self.assertEqual((build_root / "tools/utils/fixbat.exe").read_text(encoding="utf-8"), "tool")
            self.assertEqual(
                (build_root / "cpu/wl82/liba/cpu.a").read_text(encoding="utf-8"),
                "archive",
            )
            self.assertEqual((sdk / "cpu/wl82/sdk.ld").read_text(encoding="utf-8"), "stale generated linker script")
            self.assertEqual((sdk / "cpu/wl82/tools/app.bin").read_text(encoding="utf-8"), "stale image")
            self.assertEqual(
                (sdk / "apps/common/movable/section.txt").read_text(encoding="utf-8"),
                "stale generated sections",
            )

    def test_clean_removes_only_the_staging_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            sdk = self._make_sdk(root)
            staging = root / "jieli-staging"
            stage_sdk_inputs(sdk, staging / "build", JIELI_CHIPS["wl82"])

            clean_staging_tree(staging)

            self.assertFalse(staging.exists())
            self.assertEqual((sdk / "cpu/wl82/tools/app.bin").read_text(encoding="utf-8"), "stale image")
            self.assertEqual((sdk / "cpu/wl82/sdk.ld").read_text(encoding="utf-8"), "stale generated linker script")

    def test_build_outputs_and_clean_stay_inside_the_overlay(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            sdk = self._make_sdk(root)
            staging = root / "jieli-staging"
            build_root = staging / "build"
            stage_sdk_inputs(sdk, build_root, JIELI_CHIPS["wl82"])

            def snapshot(directory: Path) -> dict[str, bytes]:
                return {
                    path.relative_to(directory).as_posix(): path.read_bytes()
                    for path in directory.rglob("*")
                    if path.is_file()
                }

            before = snapshot(sdk)
            generated = (
                "cpu/wl82/sdk.ld",
                "cpu/wl82/sdk_used_list.used",
                "cpu/wl82/tools/sdk.elf",
                "cpu/wl82/tools/sdk.map",
                "cpu/wl82/tools/download.sh",
                "cpu/wl82/tools/isd_config.ini",
                "cpu/wl82/tools/isd_config_loader.ini",
                "cpu/wl82/tools/app.bin",
                "apps/common/movable/section.txt",
            )
            for relative in generated:
                output = build_root / relative
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(b"generated by build")

            self.assertEqual(snapshot(sdk), before)
            clean_staging_tree(staging)

            self.assertFalse(staging.exists())
            self.assertEqual(snapshot(sdk), before)

    def test_adapter_manifest_has_no_unnecessary_chip_helper_source(self):
        platform_root = Path(__file__).resolve().parents[1]

        wl82_sources = read_adapter_sources(platform_root, "wl82")
        wl83_sources = read_adapter_sources(platform_root, "wl83")

        self.assertNotIn("src/chip/wl82/jieli_chip_wl82.c", wl82_sources)
        self.assertNotIn("src/chip/wl83/jieli_chip_wl83.c", wl83_sources)
        self.assertFalse(any("src/chip/" in source for source in wl82_sources))
        self.assertFalse(any("src/chip/" in source for source in wl83_sources))
        self.assertEqual(wl82_sources, wl83_sources)
        self.assertIn("src/driver/tkl_wifi.c", wl82_sources)
        self.assertIn("src/driver/tkl_wifi.c", wl83_sources)
        self.assertNotIn("src/misc/jieli_dev_mac.c", wl82_sources)
        self.assertNotIn("src/misc/jieli_dev_mac.c", wl83_sources)
        self.assertNotIn("src/driver/jieli_bluetooth_backend.c", wl82_sources)
        self.assertNotIn("src/driver/jieli_bluetooth_backend.c", wl83_sources)

    def test_queue_uses_public_tkl_semaphore_contract(self):
        platform_root = Path(__file__).resolve().parents[1]
        source = (platform_root / "tuyaos/tuyaos_adapter/src/system/tkl_queue.c").read_text(encoding="utf-8")

        required_contract = (
            '#include "tkl_semaphore.h"',
            "TKL_SEM_HANDLE free_slots",
            "TKL_SEM_HANDLE filled",
            "tkl_semaphore_wait(",
            "tkl_semaphore_post(",
            "tkl_semaphore_release(",
        )
        self.assertTrue(all(symbol in source for symbol in required_contract), "queue must use the public TKL semaphore API")
        self.assertNotIn("jieli_tkl_sem_wait", source)

    def test_vendor_semaphore_wait_helper_has_internal_linkage(self):
        platform_root = Path(__file__).resolve().parents[1]
        source = (platform_root / "tuyaos/tuyaos_adapter/src/system/tkl_semaphore.c").read_text(encoding="utf-8")

        self.assertIn("static int jieli_tkl_sem_wait(", source)

    def test_flash_uid_selection_is_local_to_wifi_tkl(self):
        platform_root = Path(__file__).resolve().parents[1]
        source = (platform_root / "tuyaos/tuyaos_adapter/src/driver/tkl_wifi.c").read_text(encoding="utf-8")
        wl82_sources = read_adapter_sources(platform_root, "wl82")
        wl83_sources = read_adapter_sources(platform_root, "wl83")

        required_contract = (
            '#include "asm/sfc_norflash_api.h"',
            "static int jieli_read_flash_uid",
            "#if defined(JIELI_SELECTED_CHIP_WL82)",
            "get_norflash_uuid()",
            "#elif defined(JIELI_SELECTED_CHIP_WL83)",
            "get_norflash_uuid(0)",
        )
        self.assertTrue(all(symbol in source for symbol in required_contract), "Wi-Fi TKL must select the matching chip UID API")
        wl82_branch = source.split("#if defined(JIELI_SELECTED_CHIP_WL82)", 1)[1].split(
            "#elif defined(JIELI_SELECTED_CHIP_WL83)", 1
        )[0]
        wl83_branch = source.split("#elif defined(JIELI_SELECTED_CHIP_WL83)", 1)[1].split("#else", 1)[0]
        self.assertIn("sdk_uid = get_norflash_uuid();", wl82_branch)
        self.assertIn("sdk_uid = get_norflash_uuid(0);", wl83_branch)
        self.assertNotIn("extern int jieli_chip_get_flash_uid", source)
        self.assertFalse(any("src/chip/" in path for path in wl82_sources))
        self.assertFalse(any("src/chip/" in path for path in wl83_sources))

    def test_adapter_manifest_rejects_unknown_chip(self):
        platform_root = Path(__file__).resolve().parents[1]
        with self.assertRaisesRegex(Exception, "unsupported Jieli chip"):
            read_adapter_sources(platform_root, "wl99")

    def test_windows_toolchain_path_uses_make_safe_slashes(self):
        command = build_make_command(
            PureWindowsPath("D:/sdk"),
            PureWindowsPath("C:/JL/pi32/bin"),
            chip=JIELI_CHIPS["wl82"],
        )

        self.assertIn("TOOL_DIR=C:/JL/pi32/bin", command)

    def test_vendor_example_sources_are_dropped_for_wl83(self):
        content = (
            "c_SRC_FILES := \\\n"
            "\t../../../../../apps/common/example/peripherals/uart/uart_test.c \\\n"
            "\t../../../../../apps/common/example/system/os/os_test.c \\\n"
            "\t../../../../../apps/demo/demo_hello/app_main.c \\\n"
        )

        result = drop_vendor_example_sources(content, expects_examples=True)

        self.assertNotIn("apps/common/example/", result)
        self.assertIn("apps/demo/demo_hello/app_main.c", result)

    def test_wl82_demo_makefile_without_example_sources_is_left_alone(self):
        # The wl82 vendor demo Makefile lists no apps/common/example sources at
        # all. That is a valid layout, not a Makefile-format error: raising here
        # failed every AC791 build in staging.
        content = (
            "c_SRC_FILES := \\\n"
            "\t../../../../../apps/demo/demo_hello/app_main.c \\\n"
        )

        self.assertEqual(drop_vendor_example_sources(content), content)

    def test_a_chip_that_should_list_examples_still_fails_loudly(self):
        # wl83 does list them, so finding none means the Makefile moved and the
        # strip silently stopped working - which would quietly put ~45% of the
        # vendor compile back. That guard has to survive.
        content = (
            "c_SRC_FILES := \\\n"
            "\t../../../../../apps/demo/demo_hello/app_main.c \\\n"
        )

        with self.assertRaisesRegex(Exception, "expected to list"):
            drop_vendor_example_sources(content, expects_examples=True)


if __name__ == "__main__":
    unittest.main()
