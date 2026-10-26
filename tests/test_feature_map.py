"""Bind the feature closure map to the overlay's staged output.

The map is only useful if it says exactly what the overlay stages. These tests
freeze the rendered sources, macros and archives to the literals the overlay
emitted before the map existed, so a drift in either direction fails here rather
than silently changing a firmware.
"""

import unittest
from pathlib import Path

from tools.jieli_build import feature_map
from tools.jieli_build.feature_map import (
    FEATURES,
    SHARED_DEPENDENCIES,
    feature_defines,
    feature_sources,
    link_libraries,
)

PLATFORM_ROOT = Path(__file__).resolve().parents[1]

# What sdk_overlay.py emitted before it read the map, transcribed verbatim from
# the staged Makefile. Keep these frozen: they are the byte-identity contract.
EXPECTED_DEFINES = (
    "CONFIG_NET_ENABLE=1",
    "CONFIG_BT_ENABLE=1",
    "CONFIG_TWS_ENABLE",
    "CONFIG_BTCTRLER_TASK_DEL_ENABLE",
    "CONFIG_LMP_CONN_SUSPEND_ENABLE",
    "CONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE",
)

EXPECTED_FEATURE_SOURCES = (
    "apps/common/config/bt_profile_config.c",
    "apps/common/config/log_config/app_config.c",
    "apps/common/config/log_config/lib_btctrler_config.c",
    "apps/common/config/log_config/lib_btstack_config.c",
    "apps/common/net/assign_macaddr.c",
    "apps/common/net/config_network.c",
    "apps/common/net/platform_cfg.c",
    "apps/common/net/wifi_conf.c",
)

EXPECTED_WL82_LIBRARIES = (
    "cpu.a",
    "system.a",
    "hsm.a",
    "event.a",
    "common_lib.a",
    "wpasupplicant.a",
    "http_cli.a",
    "https_cli.a",
    "json.a",
    "libmbedtls_3_4_0.a",
    "lwip_2_2_0.a",
    "wl_wifi_sta.a",
    "net_server.a",
    "wl_rf_common.a",
    "btctrler.a",
    "btstack.a",
    "crypto_toolbox_Osize.a",
    "lib_ccm_aes.a",
)

DISPLAY_SOURCES = (
    "apps/common/camera/isp_scenes.c",
    "apps/common/camera/isp_tools.c",
    "apps/common/dma2d_gpu/dma2d_common_api.c",
    "apps/common/dma2d_gpu/gpu_common_api.c",
    "apps/common/lcd/lcd_driver/lcd_driver.c",
    "apps/common/lcd/lcd_driver/lcd_scenes.c",
    "apps/common/lcd/lcd_driver/lcd_tools.c",
    "apps/common/lcd/lcd_driver/fb_component/fb_combine.c",
    "apps/common/lcd/lcd_driver/fb_component/fb_lcd.c",
    "apps/common/lcd/lcd_driver/fb_component/fb_out_dev.c",
    "apps/common/lcd/lcd_driver/fb_component/fb_rotate.c",
    "apps/common/lcd/lcd_driver/mipi_lcd/lcd_mipi_st7701s_480x800.c",
)


class FeatureMapClosureTest(unittest.TestCase):
    def test_every_required_feature_is_declared(self):
        self.assertEqual(
            set(FEATURES), {"wifi", "bluetooth", "display", "camera", "tls", "lwip", "power"}
        )
        self.assertEqual(FEATURES["wifi"].kconfig, "CONFIG_ENABLE_WIFI")
        self.assertEqual(FEATURES["bluetooth"].kconfig, "CONFIG_ENABLE_BLUETOOTH")
        self.assertEqual(FEATURES["display"].kconfig, "CONFIG_ENABLE_DISPLAY")
        self.assertEqual(FEATURES["lwip"].kconfig, "CONFIG_ENABLE_LIBLWIP")
        self.assertEqual(FEATURES["power"].kconfig, "CONFIG_ENABLE_TUYA_PM")
        # TuyaOpen's TLS stack has no switch on this platform, so the feature
        # says so rather than inventing a symbol.
        self.assertIsNone(FEATURES["tls"].kconfig)

    def test_shared_dependencies_are_the_never_removed_archives(self):
        self.assertEqual(
            SHARED_DEPENDENCIES,
            ("cpu.a", "system.a", "hsm.a", "event.a", "common_lib.a"),
        )
        for feature in FEATURES.values():
            self.assertEqual(feature.shared_dependencies, SHARED_DEPENDENCIES)
        # The shared archives are the first five on the link line, owned by
        # nobody, so no feature can claim them.
        self.assertEqual(
            [archive for archive, owner in feature_map.LINK_ORDER if owner is None],
            list(SHARED_DEPENDENCIES),
        )

    def test_defines_render_in_the_emitted_order(self):
        self.assertEqual(feature_defines(("wifi", "bluetooth")), EXPECTED_DEFINES)
        self.assertEqual(
            feature_defines(("bluetooth", "wifi")), EXPECTED_DEFINES
        )

    def test_feature_sources_render_in_the_emitted_order(self):
        self.assertEqual(
            feature_sources(("bluetooth", "wifi")), EXPECTED_FEATURE_SOURCES
        )
        # Display is appended before the Bluetooth and Wi-Fi block.
        self.assertEqual(
            feature_sources(("display", "bluetooth", "wifi")),
            DISPLAY_SOURCES + EXPECTED_FEATURE_SOURCES,
        )

    def test_wl82_link_line_is_unchanged(self):
        self.assertEqual(
            link_libraries("wl82", ("wifi", "bluetooth", "tls", "lwip")),
            EXPECTED_WL82_LIBRARIES,
        )

    def test_wl83_and_display_additions_keep_their_position(self):
        self.assertEqual(
            link_libraries("wl83", ("wifi", "bluetooth", "tls", "lwip")),
            EXPECTED_WL82_LIBRARIES + ("libcrypto_mbedtls.a",),
        )
        self.assertEqual(
            link_libraries("wl83", ("wifi", "bluetooth", "tls", "lwip", "display")),
            EXPECTED_WL82_LIBRARIES + ("libcrypto_mbedtls.a", "video.a"),
        )

    def test_a_feature_owns_the_archives_the_link_line_puts_under_its_name(self):
        self.assertEqual(
            FEATURES["wifi"].libraries,
            (
                "wpasupplicant.a",
                "http_cli.a",
                "https_cli.a",
                "json.a",
                "wl_wifi_sta.a",
                "net_server.a",
                "wl_rf_common.a",
            ),
        )
        self.assertEqual(
            FEATURES["bluetooth"].libraries,
            ("btctrler.a", "btstack.a", "crypto_toolbox_Osize.a", "lib_ccm_aes.a"),
        )
        self.assertEqual(FEATURES["display"].libraries, ("video.a",))
        self.assertEqual(FEATURES["lwip"].libraries, ("lwip_2_2_0.a",))
        # The wl83-only crypto provider is part of the TLS closure.
        self.assertEqual(
            FEATURES["tls"].libraries, ("libmbedtls_3_4_0.a", "libcrypto_mbedtls.a")
        )

    def test_every_archive_on_the_link_line_has_one_owner_or_none(self):
        owned = {archive for archive, _ in feature_map.LINK_ORDER}
        owned.update(archive for archive, _ in feature_map.DISPLAY_LIBRARIES)
        owned.update(archive for archive, _ in feature_map.CAMERA_LIBRARIES)
        for entries in feature_map.CHIP_LIBRARIES.values():
            owned.update(archive for archive, _ in entries)
        declared = set(SHARED_DEPENDENCIES)
        for feature in FEATURES.values():
            declared.update(feature.libraries)
        self.assertEqual(owned, declared)
        self.assertEqual(len(owned), len(declared))

    def test_link_order_is_not_feature_grouped(self):
        # The order is the vendor Makefile's, so a feature-grouped rewrite would
        # break it. lwip sits between two Wi-Fi archives, which is the tell.
        order = [archive for archive, _ in feature_map.LINK_ORDER]
        self.assertLess(order.index("libmbedtls_3_4_0.a"), order.index("lwip_2_2_0.a"))
        self.assertLess(order.index("lwip_2_2_0.a"), order.index("wl_wifi_sta.a"))

    def test_the_media_switch_is_not_claimed(self):
        # Its closure lives in audio_profile.py; claiming it here would be half
        # true. This test records that decision so it is not quietly reversed.
        self.assertNotIn("media", FEATURES)


class FeatureMapEvidenceTest(unittest.TestCase):
    """Every declared source and include must exist in the vendor SDK it names."""

    SDK_ROOTS = {
        "wl82": PLATFORM_ROOT / "chip/wl82/AC79_AIoT_SDK",
        "wl83": PLATFORM_ROOT / "chip/wl83/AC792_SDK/sdk",
    }

    def setUp(self):
        if not all(root.is_dir() for root in self.SDK_ROOTS.values()):
            self.skipTest("vendor SDK checkouts are not present")

    def test_declared_sources_exist_in_the_vendor_tree(self):
        for name, feature in FEATURES.items():
            if not feature.sources:
                continue
            roots = self.SDK_ROOTS.values() if name not in ("display", "camera") else (
                self.SDK_ROOTS["wl83"],
            )
            for source in feature.sources:
                for root in roots:
                    self.assertTrue(
                        (root / source).is_file(),
                        f"{name} source does not exist: {source} under {root}",
                    )

    def test_declared_includes_exist_in_the_vendor_tree(self):
        for name, feature in FEATURES.items():
            if not feature.includes:
                continue
            for cpu, root in self.SDK_ROOTS.items():
                if name in ("display", "camera") and cpu != "wl83":
                    continue
                for directory in feature.includes:
                    resolved = directory.format(cpu=cpu)
                    self.assertTrue(
                        (root / resolved).is_dir(),
                        f"{name} include does not exist: {resolved} under {root}",
                    )

    def test_only_the_chip_specific_include_directories_are_templated(self):
        templated = {
            (name, directory)
            for name, feature in FEATURES.items()
            for directory in feature.includes
            if "{cpu}" in directory
        }
        self.assertEqual(
            templated,
            {
                ("display", "include_lib/driver/cpu/{cpu}/asm"),
                ("camera", "include_lib/driver/cpu/{cpu}/asm"),
                ("bluetooth", "include_lib/btctrler/port/{cpu}"),
            },
        )


if __name__ == "__main__":
    unittest.main()
