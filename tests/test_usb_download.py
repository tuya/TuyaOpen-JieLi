"""USB runtime download wiring in each staged Jieli SDK profile."""

import json
import tempfile
import unittest
from pathlib import Path

from build_example import parse_build_params, resolve_board_name
from tools.jieli_build.audio_profile import AUDIO_PROFILES
from tools.jieli_build.board_facts import BoardFacts, Display, LogUart, Memory, ServiceUart
from tools.jieli_build.chip_profiles import JIELI_CHIPS, PLATFORM_ROOT
from tools.jieli_build.sdk_overlay import USB_DEVICE_SOURCES, create_staging_tree


def synthetic_facts(board: str, chip_name: str, usb_device: str) -> BoardFacts:
    """Board facts for the synthetic checkout, shaped like the real catalog's.

    The UART and memory values follow the chip, because the staging hooks assert
    the log UART against them; the USB device is what each test varies. The real
    reader turns Kconfig settings into this, so building it directly keeps these
    tests about staging rather than about the reader.
    """
    if chip_name == "wl82":
        return BoardFacts(
            board=board,
            usb_device=usb_device,
            memory=Memory(flash_mib=8, sdram_mib=8, external_ddr=False),
            log_uart=LogUart(port=1, tx_pin="IO_PORTB_03"),
            service_uart=ServiceUart(port=0, tx_pin="IO_PORTA_05", rx_pin="IO_PORTA_06"),
            display=None,
        )
    return BoardFacts(
        board=board,
        usb_device=usb_device,
        memory=Memory(flash_mib=8, sdram_mib=16, external_ddr=True),
        log_uart=LogUart(port=0, tx_pin=None),
        service_uart=None,
        display=None,
    )


class UsbDownloadStagingTest(unittest.TestCase):
    def _stage(self, root: Path, chip, board_name=None, usb_device="TCFG_FUSB_DEVICE",
               write_facts=True, cdc_log_enabled=True):
        # write_facts is kept as a knob only so a test can prove the stage
        # refuses without them; there is no file to write any more.
        sdk = root / "sdk"
        vendor = sdk / chip.sdk_source_relative
        for name in ("include_lib", "lib", "tools"):
            (vendor / name).mkdir(parents=True)
            (vendor / name / "placeholder").write_text("input", encoding="utf-8")
        liba = vendor / "cpu" / chip.cpu / "liba"
        liba.mkdir(parents=True)
        (liba / "cpu.a").write_text("archive", encoding="utf-8")
        (vendor / "cpu" / chip.cpu / "sdk_ld.c").write_text("ld", encoding="utf-8")
        movable = vendor / "apps/common/movable"
        movable.mkdir(parents=True)
        (movable / "section.c").write_text("sections", encoding="utf-8")
        net = vendor / "apps/common/net"
        net.mkdir(parents=True)
        (net / "wifi.c").write_text("wifi", encoding="utf-8")

        # The audio profile stages vendor trees outside the minimal tree, so the
        # synthetic checkout has to provide the same inputs it validates.
        profile = AUDIO_PROFILES.get(chip.name)
        selected = board_name or (profile.audio_board_name if profile else "Synthetic_Board")
        if profile is not None:
            for tree in profile.staged_vendor_trees:
                (vendor / tree).mkdir(parents=True, exist_ok=True)
            for required in profile.staged_vendor_files:
                (vendor / required).parent.mkdir(parents=True, exist_ok=True)
                (vendor / required).write_text("stub", encoding="utf-8")
            board_profile = root / "tuyaopen" / "boards/JIELI" / profile.audio_board_name
            board_profile.mkdir(parents=True, exist_ok=True)
            (board_profile / "audio_config.h").write_text(
                "#define TUYA_AUDIO_STUB 1\n", encoding="utf-8"
            )
        facts = synthetic_facts(selected, chip.name, usb_device) if write_facts else None
        header_dir = root / "tuyaopen" / ".build" / "include"
        header_dir.mkdir(parents=True, exist_ok=True)
        (header_dir / "tuya_kconfig.h").write_text(
            "#define ENABLE_JIELI_USB_CDC_LOG "
            f"{1 if cdc_log_enabled else 0}\n",
            encoding="utf-8",
        )

        demo = Path("apps/demo/demo_hello")
        demo_root = vendor / demo
        board = demo_root / "board" / chip.cpu
        (demo_root / "include").mkdir(parents=True)
        board.mkdir(parents=True)
        (demo_root / "include/app_config.h").write_text(
            "#ifndef APP_CONFIG_H\n"
            "#define __FLASH_SIZE__ 1\n#define __SDRAM_SIZE__ 1\n"
            "#endif\n", encoding="utf-8"
        )
        # Mirror the real vendor layout: the wl83 demo Makefile lists its own
        # apps/common/example/** self-test programs, the wl82 one lists none.
        # Staging strips them, and asserts the strip still matched for a chip
        # that is supposed to have them.
        sources = "    ../../../../../apps/demo/demo_hello/app_main.c\n"
        if chip.demo_lists_example_sources:
            sources = (
                "    ../../../../../apps/common/example/system/os/os_test.c \\\n"
                + sources
            )
        (board / "Makefile").write_text(
            "c_SRC_FILES := \\\n" + sources + "c_OBJS    :=\n", encoding="utf-8"
        )
        (board / "board.c").write_text(
            # Both vendor board.c files open with this; configure_lcd_board()
            # anchors its insertion on it and refuses a board file without it.
            '#include "app_config.h"\n'
            '#include "asm/includes.h"\n'
            "REGISTER_DEVICES(device_table) = {\n};\n"
            "void board_early_init(void) { devices_init(); }\n"
            "void board_init(void)\n{\n}\n"
            "void debug_uart_init(void) { uart_init(&uart2_data); }\n"
            "UART2_PLATFORM_DATA_BEGIN(uart2_data)\n"
            "UART2_PLATFORM_DATA_END();\n", encoding="utf-8"
        )
        if chip.name == "wl83":
            (board / "board_demo.h").write_text(
                "#ifndef BOARD_DEMO_H\n#define BOARD_DEMO_H\n"
                "#define TCFG_UART0_BAUDRATE 115200\n"
                "#define CONFIG_NO_SDRAM_ENABLE\n#endif\n", encoding="utf-8"
            )
            (board / "chip_cfg.h").write_text(
                "#define __FLASH_SIZE__ 1\n#define __SDRAM_SIZE__ 1\n", encoding="utf-8"
            )
        (demo_root / "app_main.c").write_text("void app_main(void) {}\n", encoding="utf-8")
        watched = {
            path: (vendor / path).read_bytes()
            for path in (
                demo / "include/app_config.h",
                demo / "board" / chip.cpu / "board.c",
                demo / "board" / chip.cpu / "Makefile",
            )
        }

        build = create_staging_tree(
            sdk, root / "stage", root / "tuyaopen", header_dir=header_dir,
            tuya_lib_dir=root / "libs",
            uart_log_port=1 if chip.name == "wl82" else 0,
            chip=chip,
            facts=facts,
            features=("wifi", "bluetooth", "tls", "lwip"),
        )
        return vendor, build / chip.sdk_source_relative, watched

    def test_staged_profiles_wire_runtime_usb_without_editing_sdk(self):
        for chip in JIELI_CHIPS.values():
            with self.subTest(chip=chip.name), tempfile.TemporaryDirectory() as directory:
                vendor, staged, watched = self._stage(Path(directory), chip)
                for path, original in watched.items():
                    self.assertEqual((vendor / path).read_bytes(), original)

                board = (staged / chip.board_build_relative / "board.c").read_text(encoding="utf-8")
                makefile = (staged / chip.board_build_relative / "Makefile").read_text(encoding="utf-8")
                profile = (
                    staged / "apps/demo/demo_hello" /
                    ("include/app_config.h" if chip.name == "wl82" else "board/wl83/board_demo.h")
                ).read_text(encoding="utf-8")
                self.assertIn('{ "otg", &usb_dev_ops, (void *)&otg_data}', board)
                self.assertIn(".usb_dev_en = 0x03" if chip.name == "wl82"
                              else ".usb_dev_en = TCFG_USB_DEVICE", board)
                self.assertIn("#define TCFG_PC_ENABLE", profile)
                self.assertIn("#define USB_PC_NO_APP_MODE", profile)
                self.assertIn("#define USB_DEVICE_CLASS_CONFIG", profile)
                config_header = profile.index('#include "tuya_kconfig.h"')
                cdc_gate = profile.index("#if (defined(ENABLE_JIELI_USB_CDC_LOG)")
                self.assertLess(config_header, cdc_gate)
                self.assertIn(
                    "#define USB_DEVICE_CLASS_CONFIG             "
                    "(MASSSTORAGE_CLASS | CDC_CLASS)",
                    profile,
                )
                if chip.name == "wl82":
                    self.assertIn("#define CONFIG_USB_ENABLE", profile)
                else:
                    self.assertIn("#define TCFG_USB_DEVICE", profile)
                for source in USB_DEVICE_SOURCES:
                    self.assertIn("../../../../../" + source, makefile)
                self.assertIn("../../../../../tuyaos/entry/jieli_usb_download.c", makefile)
                self.assertIn("-I../../../../../apps/common/usb/host", makefile)
                self.assertIn("-I../../../../../apps/common", makefile)

    def test_usb_cdc_class_tracks_the_tuya_kconfig_switch(self):
        for chip in JIELI_CHIPS.values():
            for enabled in (True, False):
                with self.subTest(chip=chip.name, enabled=enabled), tempfile.TemporaryDirectory() as directory:
                    _, staged, _ = self._stage(
                        Path(directory), chip, cdc_log_enabled=enabled,
                    )
                    profile = (
                        staged / "apps/demo/demo_hello" /
                        ("include/app_config.h" if chip.name == "wl82" else "board/wl83/board_demo.h")
                    ).read_text(encoding="utf-8")
                    expected = (
                        "(MASSSTORAGE_CLASS | CDC_CLASS)" if enabled
                        else "(MASSSTORAGE_CLASS)"
                    )
                    self.assertIn(f"#define USB_DEVICE_CLASS_CONFIG             {expected}", profile)

    def test_the_board_fact_selects_the_usb_device(self):
        # The USB device macro is the board's own declaration, not the platform's
        # guess from a board name. AC792N declares a host port; a second wl83
        # board that declares a device port gets that instead.
        chip = JIELI_CHIPS["wl83"]
        with tempfile.TemporaryDirectory() as directory:
            _, husb_stage, _ = self._stage(
                Path(directory), chip,
                board_name="AC792N_Develop_Board", usb_device="TCFG_HUSB_DEVICE",
            )
            profile = (husb_stage / "apps/demo/demo_hello/board/wl83/board_demo.h").read_text(encoding="utf-8")
            board = (husb_stage / "apps/demo/demo_hello/board/wl83/board.c").read_text(encoding="utf-8")
            self.assertIn("#define TCFG_USB_DEVICE                     TCFG_HUSB_DEVICE", profile)
            self.assertIn(".usb_dev_en = TCFG_USB_DEVICE", board)

        with tempfile.TemporaryDirectory() as directory:
            _, fusb_stage, _ = self._stage(
                Path(directory), chip, board_name="Second_Wl83_Board",
            )
            profile = (fusb_stage / "apps/demo/demo_hello/board/wl83/board_demo.h").read_text(encoding="utf-8")
            self.assertIn("#define TCFG_USB_DEVICE                     TCFG_FUSB_DEVICE", profile)

    def test_a_board_without_hardware_facts_fails_the_stage(self):
        # No default to fall back to: the platform must not stage a board whose
        # hardware it does not know.
        chip = JIELI_CHIPS["wl82"]
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(Exception, "board facts are required to stage"):
                self._stage(
                    Path(directory), chip, board_name="No_Facts_Board", write_facts=False
                )

    def test_build_params_board_choice_reaches_usb_staging_selector(self):
        with tempfile.TemporaryDirectory() as directory:
            params_path = Path(directory) / "build_param.config"
            params_path.write_text(
                'CONFIG_BOARD_CHOICE="AC792N_Develop_Board"\n'
                "CONFIG_BOARD_CHOICE_AC792N_DEVELOP_BOARD=y\n",
                encoding="utf-8",
            )
            params = parse_build_params(params_path)
            self.assertEqual(resolve_board_name(params), "AC792N_Develop_Board")

            # Legacy AC7921A shares CONFIG_BOARD_CHOICE's value, so its board
            # string alone must not accidentally select the AC792N USB port.
            params_path.write_text(
                'CONFIG_BOARD_CHOICE="AC792N_Develop_Board"\n'
                "CONFIG_BOARD_CHOICE_AC7921A=y\n",
                encoding="utf-8",
            )
            self.assertIsNone(resolve_board_name(parse_build_params(params_path)))

    def test_board_selection_reads_the_board_catalog_not_a_platform_table(self):
        # A board the platform has never named: the profile comes from the
        # generated Kconfig data (CONFIG_BOARD_CHOICE plus its selection flag),
        # so adding a board under boards/JIELI needs no platform edit.
        with tempfile.TemporaryDirectory() as directory:
            params_path = Path(directory) / "build_param.config"
            params_path.write_text(
                'CONFIG_BOARD_CHOICE="Hypothetical_New_Board"\n'
                "CONFIG_BOARD_CHOICE_HYPOTHETICAL_NEW_BOARD=y\n",
                encoding="utf-8",
            )
            self.assertEqual(
                resolve_board_name(parse_build_params(params_path)),
                "Hypothetical_New_Board",
            )

            # The catalog's existing boards resolve the same way, with no
            # per-board branch: the name is the Kconfig string, not a literal.
            params_path.write_text(
                'CONFIG_BOARD_CHOICE="AC79_DevKitBoard"\n'
                "CONFIG_BOARD_CHOICE_AC79_DEVKITBOARD=y\n",
                encoding="utf-8",
            )
            self.assertEqual(
                resolve_board_name(parse_build_params(params_path)),
                "AC79_DevKitBoard",
            )

    @unittest.skipUnless(
        all(
            (PLATFORM_ROOT / chip.sdk_dir / chip.sdk_source_relative / source).is_file()
            for chip in JIELI_CHIPS.values()
            for source in USB_DEVICE_SOURCES
        ),
        "vendor JieLi SDK source trees are not checked out",
    )
    def test_sdk_has_download_entry_points_on_both_chips(self):
        essential = (
            "apps/common/usb/device/cdc.c",
            "apps/common/usb/device/msd_upgrade.c",
            "apps/common/usb/device/user_setup.c",
            "apps/common/usb/device/task_pc.c",
        )
        for chip in JIELI_CHIPS.values():
            vendor = PLATFORM_ROOT / chip.sdk_dir / chip.sdk_source_relative
            for source in USB_DEVICE_SOURCES:
                self.assertTrue((vendor / source).is_file(), f"{chip.name}: {source}")
            for source in essential:
                self.assertIn(source, USB_DEVICE_SOURCES)
            cdc_source = (vendor / "apps/common/usb/device/cdc.c").read_text(encoding="utf-8")
            self.assertLess(
                cdc_source.index('#include "app_config.h"'),
                cdc_source.index("#if TCFG_USB_SLAVE_CDC_ENABLE"),
                f"{chip.name}: app_config.h must define CDC class selection before cdc.c gates its implementation",
            )
            usb_common_def = (vendor / "apps/common/usb/usb_common_def.h").read_text(encoding="utf-8")
            self.assertIn("USB_DEVICE_CLASS_CONFIG & CDC_CLASS", usb_common_def)
            self.assertIn("#define TCFG_USB_SLAVE_CDC_ENABLE           1", usb_common_def)
        entry = (PLATFORM_ROOT / "tuyaos/entry/jieli_app_entry.c").read_text(encoding="utf-8")
        usb_entry = (PLATFORM_ROOT / "tuyaos/entry/jieli_usb_download.c").read_text(encoding="utf-8")
        self.assertIn('"#C0usb_msd0"', entry)
        self.assertIn('"usb_msd0"', entry)
        self.assertIn("#if defined(TCFG_USB_SLAVE_ENABLE) && TCFG_USB_SLAVE_ENABLE", entry)
        self.assertIn("#if CPU_CORE_NUM > 1 && !defined(JIELI_SELECTED_CHIP_WL83)", entry)
        self.assertIn('"usb_msd1"', entry)
        self.assertIn("DEVICE_EVENT_FROM_OTG", usb_entry)
        self.assertIn("pc_device_event_handler(event)", usb_entry)

    def test_tkl_output_supports_usb_cdc_log_switch(self):
        tkl_output = (PLATFORM_ROOT / "tuyaos/tuyaos_adapter/src/system/tkl_output.c").read_text(encoding="utf-8")
        self.assertIn("ENABLE_JIELI_USB_CDC_LOG", tkl_output)
        self.assertIn("CONFIG_ENABLE_JIELI_USB_CDC_LOG", tkl_output)
        self.assertIn("cdc_write_data", tkl_output)
        self.assertIn("vprintf(format, args)", tkl_output)


if __name__ == "__main__":
    unittest.main()
