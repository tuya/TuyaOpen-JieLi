import pathlib
import unittest

from tools.jieli_build.audio_profile import AUDIO_PROFILES, AudioProfile


class AudioProfileTest(unittest.TestCase):
    def test_both_chips_have_a_profile(self):
        self.assertEqual(set(AUDIO_PROFILES), {"wl82", "wl83"})

    def test_profiles_are_plain_data(self):
        for name, profile in AUDIO_PROFILES.items():
            with self.subTest(chip=name):
                self.assertIsInstance(profile, AudioProfile)
                self.assertTrue(all(s.endswith(".c") for s in profile.vendor_sources))
                self.assertTrue(all(lib.endswith(".a") for lib in profile.link_libs))

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


if __name__ == "__main__":
    unittest.main()
