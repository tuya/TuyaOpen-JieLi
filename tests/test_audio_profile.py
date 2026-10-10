import pathlib
import tempfile
import unittest

from tools.jieli_build.audio_profile import AUDIO_PROFILES, AudioProfile, apply_audio_profile
from tools.jieli_build.board_config import configure_audio_board
from tools.jieli_build.chip_profiles import JIELI_CHIPS, PLATFORM_ROOT
from tools.jieli_build.sdk_overlay import create_staging_tree


class AudioProfileTest(unittest.TestCase):
    def test_both_chips_have_a_profile(self):
        self.assertEqual(set(AUDIO_PROFILES), {"wl82", "wl83"})

    def test_profiles_are_plain_data(self):
        for name, profile in AUDIO_PROFILES.items():
            with self.subTest(chip=name):
                self.assertIsInstance(profile, AudioProfile)
                self.assertTrue(all(s.endswith(".c") for s in profile.vendor_sources))
                self.assertTrue(all(lib.endswith(".a") for lib in profile.link_libs))

    def test_wl82_enables_the_decoder_symbols_its_used_list_gates(self):
        # The vendor's sdk_used_list.c gates pcm_decoder_ops on this macro, and
        # the LTO plugin pulls archive members only when the used-symbol list
        # names them. The staged app inherits demo_hello's app_config.h, which
        # defines no decoder macros, so the profile has to supply them.
        # CONFIG_AUDIO_ENABLE is deliberately absent: sdk_overlay already writes
        # it globally, so listing it here would never reach the compiler.
        defines = AUDIO_PROFILES["wl82"].defines
        self.assertIn("CONFIG_PCM_DEC_ENABLE", defines)
        self.assertIn("CONFIG_PCM_ENC_ENABLE", defines)
        self.assertNotIn("CONFIG_AUDIO_ENABLE", defines)

    def test_wl83_pulls_the_audio_runtime_our_adapter_calls(self):
        sources = AUDIO_PROFILES["wl83"].vendor_sources
        for needed in (
            "audio/common/audio_volume_mixer.c",
            "audio/framework/plugs/source/adc_file.c",
            "audio/cpu/wl83/audio_setup.c",
        ):
            self.assertIn(needed, sources)


import tempfile

from tools.jieli_build.audio_profile import apply_audio_profile

MAKEFILE = (
    "c_OBJS    :=\n"
    "\n"
    "LFLAGS += \\\n"
    "    --start-group \\\n"
    "    ../../../../../cpu/wl83/liba/cpu.a \\\n"
    "    --end-group\n"
)
APP_MAIN = (
    "const struct task_info task_info_table[] = {\n"
    '    {"app_core", 15, 2048, 1024 },\n'
    "    {0, 0, 0, 0 },\n"
    "};\n"
)


class ApplyAudioProfileTest(unittest.TestCase):
    def _stage(self, tmp, chip):
        makefile = pathlib.Path(tmp) / "Makefile"
        app_main = pathlib.Path(tmp) / "app_main.c"
        makefile.write_text(MAKEFILE, encoding="utf-8")
        app_main.write_text(APP_MAIN, encoding="utf-8")
        apply_audio_profile(makefile, app_main, chip)
        return makefile, app_main

    def test_injects_every_vendor_source_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            makefile, _ = self._stage(tmp, "wl83")
            content = makefile.read_text(encoding="utf-8")
            for source in AUDIO_PROFILES["wl83"].vendor_sources:
                # Per-object flag lines embed the source path ("objs/<source>.o"),
                # so anchor the count to the injected source entry itself.
                entry = f"c_SRC_FILES += ../../../../../{source}"
                self.assertEqual(content.count(entry), 1, source)

    def test_adds_the_link_libraries_to_the_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            makefile, _ = self._stage(tmp, "wl83")
            content = makefile.read_text(encoding="utf-8")
            for lib in AUDIO_PROFILES["wl83"].link_libs:
                self.assertIn(f"/liba/{lib}", content)

    def test_adds_the_audio_tasks_before_the_sentinel(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, app_main = self._stage(tmp, "wl83")
            app = app_main.read_text(encoding="utf-8")
            self.assertIn('"tuya_audio_capture"', app)
            self.assertLess(app.index('"tuya_audio_capture"'), app.index("{0, 0, 0, 0 }"))

    def test_wl82_does_not_add_the_wl83_capture_task(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, app_main = self._stage(tmp, "wl82")
            self.assertNotIn('"tuya_audio_capture"', app_main.read_text(encoding="utf-8"))

    def test_adds_the_decoder_defines_the_used_list_gates(self):
        # cpu/wl82/sdk_used_list.c only references pcm_decoder_ops under
        # CONFIG_PCM_DEC_ENABLE, and the LTO plugin only pulls an archive member
        # that the used-symbol list references. Without the define the
        # .audio_decoder section links empty and AUDIO_DEC_OPEN returns -14.
        with tempfile.TemporaryDirectory() as tmp:
            makefile, _ = self._stage(tmp, "wl82")
            content = makefile.read_text(encoding="utf-8")
            self.assertIn("-DCONFIG_PCM_DEC_ENABLE", content)
            self.assertIn("-DCONFIG_PCM_ENC_ENABLE", content)

    def test_wl83_keeps_its_own_define_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            makefile, _ = self._stage(tmp, "wl83")
            self.assertNotIn("-DCONFIG_PCM_DEC_ENABLE", makefile.read_text(encoding="utf-8"))

    def test_a_longer_flag_does_not_count_as_the_define_being_present(self):
        # A substring test would read -DCONFIG_PCM_DEC_ENABLE_LEGACY as proof
        # that CONFIG_PCM_DEC_ENABLE is already set, skip the append, and leave
        # .audio_decoder empty again with no build error.
        with tempfile.TemporaryDirectory() as tmp:
            makefile, _ = self._stage(tmp, "wl82")
            content = makefile.read_text(encoding="utf-8")
            makefile.write_text(
                content + "\nDEFINES += -DCONFIG_PCM_DEC_ENABLE_LEGACY\n",
                encoding="utf-8",
            )
            apply_audio_profile(makefile, self._stage(tmp, "wl82")[1], "wl82")
            self.assertIn("-DCONFIG_PCM_DEC_ENABLE ", makefile.read_text(encoding="utf-8"))

    def test_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            makefile, app_main = self._stage(tmp, "wl83")
            makefile_once = makefile.read_text(encoding="utf-8")
            app_once = app_main.read_text(encoding="utf-8")
            apply_audio_profile(makefile, app_main, "wl83")
            self.assertEqual(makefile.read_text(encoding="utf-8"), makefile_once)
            self.assertEqual(app_main.read_text(encoding="utf-8"), app_once)


class MissingAudioIncludesTest(unittest.TestCase):
    def test_reports_only_the_includes_the_makefile_lacks(self):
        from tools.jieli_build.audio_profile import missing_audio_includes

        staged = "INCLUDES += -I../../../../../include_lib/media\n"
        self.assertEqual(missing_audio_includes(staged, "wl82"), ())
        self.assertEqual(
            missing_audio_includes("", "wl82"),
            ("-I../../../../../include_lib/media",),
        )
        with self.assertRaises(Exception):
            missing_audio_includes("", "wl99")


BOARD_FILE = (
    '#include "asm/includes.h"\n'
    '#include "audio_config.h"\n'
    "\n"
    "REGISTER_DEVICES(device_table) = {\n"
    '    {"uart0", &uart_dev_ops, (void *)&uart0_data },\n'
    "};\n"
    "\n"
    "void board_early_init(void)\n"
    "{\n"
    "    devices_init();\n"
    "}\n"
    "\n"
    "void board_init(void)\n"
    "{\n"
    "    board_power_init();\n"
    "}\n"
)


class ConfigureAudioBoardTest(unittest.TestCase):
    def _stage(self, tmp, chip):
        board = pathlib.Path(tmp) / "board.c"
        header = pathlib.Path(tmp) / "audio_config.h"
        board.write_text(BOARD_FILE, encoding="utf-8")
        header.write_text("#define JIELI_AUDIO_PA_MUTE_PORT 1\n", encoding="utf-8")
        configure_audio_board(board, header, AUDIO_PROFILES[chip])
        return board

    def test_wl82_registers_the_audio_device_and_platform_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            board = self._stage(tmp, "wl82")
            content = board.read_text(encoding="utf-8")
            self.assertIn('{"audio", &audio_dev_ops, (void *)&tuya_audio_data },', content)
            self.assertIn("static const struct audio_platform_data tuya_audio_data", content)
            self.assertIn("dac_early_init(0, JIELI_AUDIO_DAC_HW_CHANNEL", content)
            self.assertIn("msleep(JIELI_AUDIO_PA_RELEASE_DELAY_MS);", content)
            self.assertIn('#include "tuya_board_audio_config.h"', content)
            self.assertIn('#include "server/audio_dev.h"', content)
            self.assertTrue((board.parent / "tuya_board_audio_config.h").is_file())

    def test_wl82_board_init_initialises_the_vendor_adc(self):
        # The ADKEY the TuyaOpen board registers through TDL needs the vendor's
        # process-global ADC queue, and only adc_init() creates it. Without the
        # call adc_add_sample_ch() returns an out-of-range index, tkl_adc_init()
        # fails with OPRT_COM_ERROR and the button never opens, which leaves
        # every button-driven app doing nothing.
        with tempfile.TemporaryDirectory() as tmp:
            board = self._stage(tmp, "wl82")
            content = board.read_text(encoding="utf-8")
            self.assertIn("adc_init();", content)
            self.assertLess(content.index("void board_init(void)"),
                            content.index("adc_init();"))

    def test_wl83_keeps_its_own_board_init(self):
        with tempfile.TemporaryDirectory() as tmp:
            board = self._stage(tmp, "wl83")
            self.assertNotIn("adc_init();", board.read_text(encoding="utf-8"))

    def test_wl83_sequences_pa_release_in_board_init_without_device_row(self):
        with tempfile.TemporaryDirectory() as tmp:
            content = self._stage(tmp, "wl83").read_text(encoding="utf-8")
            self.assertNotIn("&audio_dev_ops", content)
            self.assertIn("dac_early_init(JIELI_AUDIO_DAC_HW_CHANNEL, JIELI_AUDIO_DAC_VCM_CAP_ENABLE)", content)
            self.assertIn(".mic_port = JIELI_AUDIO_MIC_PORTS,", content)
            self.assertIn(".vcm_init_delay_ms = 1000,", content)
            self.assertIn('#include "tuya_board_audio_config.h"', content)

    def test_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            board = self._stage(tmp, "wl82")
            once = board.read_text(encoding="utf-8")
            header = pathlib.Path(tmp) / "audio_config.h"
            configure_audio_board(board, header, AUDIO_PROFILES["wl82"])
            self.assertEqual(board.read_text(encoding="utf-8"), once)

    def test_sentinel_path_repairs_includes_without_repaching(self):
        with tempfile.TemporaryDirectory() as tmp:
            board = pathlib.Path(tmp) / "board.c"
            header = pathlib.Path(tmp) / "audio_config.h"
            header.write_text("x", encoding="utf-8")
            board.write_text(
                '#include "asm/includes.h"\n'
                '#include "audio_config.h"\n'
                "/* TUYAOPEN_JIELI_AUDIO_BOARD_CONFIG */\n"
                "REGISTER_DEVICES(device_table) = {\n"
                '    {"audio", &audio_dev_ops, (void *)&tuya_audio_data },\n'
                "};\n",
                encoding="utf-8",
            )
            configure_audio_board(board, header, AUDIO_PROFILES["wl82"])
            content = board.read_text(encoding="utf-8")
            self.assertIn('#include "tuya_board_audio_config.h"', content)
            self.assertIn('#include "server/audio_dev.h"', content)
            self.assertEqual(content.count("audio_dev_ops"), 1)
            self.assertTrue((board.parent / "tuya_board_audio_config.h").is_file())


TUYAOPEN_ROOT = PLATFORM_ROOT.parents[1]
SDKS_PRESENT = all(
    (PLATFORM_ROOT / chip.sdk_dir / chip.probe).is_file() for chip in JIELI_CHIPS.values()
)
ADAPTER_PRESENT = (TUYAOPEN_ROOT / "tools/porting/adapter").is_dir()


class StagedAudioMakefileTest(unittest.TestCase):
    """End-to-end check of the staged Makefile against the vendored SDKs."""

    def test_staged_makefile_carries_the_adapter_audio_includes(self):
        if not (SDKS_PRESENT and ADAPTER_PRESENT):
            self.skipTest("vendored Jieli SDKs or the TuyaOpen adapter tree are not checked out")
        # The audio adapter compiles tkl_audio.c / tkl_vad.c / tkl_kws.c, whose
        # public headers live only under tools/porting/adapter/{media,vad,kws};
        # a missing -I there fails compilation of the adapter on either chip.
        for name, chip in JIELI_CHIPS.items():
            with self.subTest(chip=name):
                with tempfile.TemporaryDirectory() as tmp:
                    build_root = create_staging_tree(
                        PLATFORM_ROOT / chip.sdk_dir,
                        pathlib.Path(tmp) / "jieli-staging",
                        TUYAOPEN_ROOT,
                        tuya_lib_dir=pathlib.Path(tmp) / "libs",
                        uart_log_port=1 if name == "wl82" else 0,
                        chip=chip,
                    )
                    makefile = (
                        build_root / chip.sdk_source_relative
                        / chip.board_build_relative / "Makefile"
                    ).read_text(encoding="utf-8")
                    for domain in ("media", "vad", "kws"):
                        expected = (
                            f"-I{TUYAOPEN_ROOT.as_posix()}"
                            f"/tools/porting/adapter/{domain}"
                        )
                        self.assertIn(expected, makefile)
                    profile = AUDIO_PROFILES[name]
                    for source in profile.vendor_sources:
                        entry = f"c_SRC_FILES += ../../../../../{source}"
                        self.assertEqual(makefile.count(entry), 1, source)


if __name__ == "__main__":
    unittest.main()
