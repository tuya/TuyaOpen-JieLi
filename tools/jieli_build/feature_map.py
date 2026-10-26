"""The declarative feature closure map for the Jieli SDK overlay.

The Jieli board catalog declares feature switches -- ``ENABLE_WIFI``,
``ENABLE_BLUETOOTH`` and ``ENABLE_MEDIA`` in ``boards/JIELI/Kconfig`` -- and
nothing on the platform reads them. The staging overlay stages the Wi-Fi and
Bluetooth sources, macros and link archives unconditionally. This module states,
per feature, the *closure* of that feature: the macros it needs, the vendor
sources it compiles, the archives it links, the include paths it needs, and the
shared dependencies any enabled feature keeps.

Two properties matter for the next step, where the switches start to gate the
build:

* Sources, macros and archives have to be removed together. Staging
  ``apps/common/net/*.c`` but still linking ``wl_wifi_sta.a`` leaves the archive
  referencing symbols whose only definitions were those sources, and the link
  fails. A feature is a triple, not a list of sources.
* Every entry is evidence-backed. Where the link products do not prove an
  archive's owner, the entry says so instead of guessing.

Evidence comes from the two link products a real build writes next to the ELF:

* ``sdk.elf.resolution.txt`` -- the LLVM LTO resolution file, one ``-r=``
  record per object and symbol, with ``p`` in the flags marking the prevailing
  definition. It answers "which object defines this symbol".
* ``sdk.map`` -- the ``Archive member included to satisfy reference by file
  (symbol)`` section, which names the object that pulled each archive member
  in. It answers "what needs this archive".

Both are read by ``tools/jieli_build/check_jieli_provider_map.py``; the
attribution notes below quote what they show for the audited wl82
(``switch_demo``) and wl83 (``your_chat_bot``) images.

Media/audio is deliberately not a feature here. Its sources, macros and
archives live in ``audio_profile.py`` and are applied by a different hook, so a
table that claimed to close it over would be half true; it is a candidate for a
follow-up rather than a guess now.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Mapping, Optional

# The staged Makefile names SDK inputs relative to the demo board directory.
# Sources and includes in this table stay SDK-root-relative; the overlay adds
# this prefix.
SDK_SOURCE_PREFIX = "../../../../../"

# Archives every image links whatever feature set is enabled. They are the
# runtime (scheduler, events, drivers, heap) the vendor SDK and the TuyaOpen
# adapter both sit on, so no feature can drop them. Evidence: in both audited
# maps these archives' extracted members are referenced from board/setup/init
# objects, not from any one feature's code.
#
# hsm.a is listed here because the overlay links it unconditionally, but the
# evidence is weaker than the others: its only extracted functional member,
# hsm.c.o, is referenced by wl_wifi_sta.a's wifi_connect.c for HSM_STATE_Create.
# On that evidence it belongs to Wi-Fi, not to every image. It is kept shared
# because that is what today's link line does; the gating step has to re-check
# it rather than inherit this placement.
SHARED_DEPENDENCIES: tuple[str, ...] = (
    "cpu.a",
    "system.a",
    "hsm.a",
    "event.a",
    "common_lib.a",
)


@dataclass(frozen=True)
class Feature:
    """One feature's complete closure.

    ``sources`` and ``includes`` are SDK-root-relative. ``defines`` are the
    macro names (without the ``-D``) the vendor code keys off. ``libraries``
    is derived from the link order below rather than restated, so the closure
    and the linker command line cannot drift apart. ``kconfig`` is None when
    the feature has no switch of its own.
    """

    name: str
    kconfig: Optional[str]
    defines: tuple[str, ...]
    sources: tuple[str, ...]
    includes: tuple[str, ...]
    removal_note: str
    shared_dependencies: tuple[str, ...] = SHARED_DEPENDENCIES
    # True when the feature is on precisely while its switch is *off*. The
    # vendor lwIP is the case: CONFIG_ENABLE_LIBLWIP selects TuyaOpen's own
    # stack, so the vendor archive belongs on the link line only while that
    # switch is unset. See the feature's removal_note.
    kconfig_inverted: bool = False

    @property
    def libraries(self) -> tuple[str, ...]:
        """The archives this feature owns, wherever they sit on the link line."""
        return tuple(
            archive for archive, owner in _owned_library_entries() if owner == self.name
        )


WIFI = Feature(
    name="wifi",
    kconfig="CONFIG_ENABLE_WIFI",
    # sdk_overlay.py:557 emits -DCONFIG_NET_ENABLE=1 in the same line as the
    # Bluetooth macros.
    defines=("CONFIG_NET_ENABLE=1",),
    # sdk_overlay.py:475-478.
    sources=(
        "apps/common/net/assign_macaddr.c",
        "apps/common/net/config_network.c",
        "apps/common/net/platform_cfg.c",
        "apps/common/net/wifi_conf.c",
    ),
    # sdk_overlay.py:542, 548, 553 emit these unconditionally today.
    includes=("include_lib/net",),
    removal_note=(
        "Removing Wi-Fi drops the four apps/common/net sources, CONFIG_NET_ENABLE, "
        "the include_lib/net search path and the wpasupplicant/http_cli/https_cli/"
        "json/wl_wifi_sta/net_server/wl_rf_common archives. The archives have to go "
        "with the sources: the net sources are what reference wl_wifi_sta and "
        "wpasupplicant, and leaving those archives on the link line would only "
        "surface as unresolved symbols."
    ),
)


BLUETOOTH = Feature(
    name="bluetooth",
    kconfig="CONFIG_ENABLE_BLUETOOTH",
    # sdk_overlay.py:557. CONFIG_TWS_ENABLE is the true-wireless-stereo pair mode,
    # CONFIG_BTCTRLER_TASK_DEL_ENABLE lets the controller task be torn down and
    # the two CONFIG_LMP_* macros are link-layer policy; all three are
    # Bluetooth-only.
    defines=(
        "CONFIG_BT_ENABLE=1",
        "CONFIG_TWS_ENABLE",
        "CONFIG_BTCTRLER_TASK_DEL_ENABLE",
        "CONFIG_LMP_CONN_SUSPEND_ENABLE",
        "CONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE",
    ),
    # sdk_overlay.py:471-474. log_config/app_config.c is a general log-tag table
    # (BOARD/BT/UI/APP tags) rather than a Bluetooth-only file, but it is staged
    # in this block and its sections are discarded at link time in both audited
    # images, so it is closed over Bluetooth here. It is the one entry in this
    # table whose owner the link products do not really decide.
    sources=(
        "apps/common/config/bt_profile_config.c",
        "apps/common/config/log_config/app_config.c",
        "apps/common/config/log_config/lib_btctrler_config.c",
        "apps/common/config/log_config/lib_btstack_config.c",
    ),
    # sdk_overlay.py:534-537 emits these unconditionally today.
    includes=(
        "include_lib/btstack",
        "include_lib/btstack/le",
        "include_lib/btctrler",
        "include_lib/btctrler/port/{cpu}",
    ),
    removal_note=(
        "Removing Bluetooth drops the four config sources, the five CONFIG_BT_*/"
        "CONFIG_TWS_*/CONFIG_LMP_* macros, the btstack/btctrler include paths and "
        "the btctrler/btstack/crypto_toolbox_Osize/lib_ccm_aes archives. The Tuya "
        "Bluetooth adapter (objs/tuyaos_adapter/src/driver/tkl_bluetooth.c) "
        "references btstack.a directly, so a Bluetooth-off image must not stage "
        "that adapter either."
    ),
)


DISPLAY = Feature(
    name="display",
    kconfig="CONFIG_ENABLE_DISPLAY",
    defines=(),
    # sdk_overlay.py:80-95 (LCD_SOURCES), staged only when the board declares a
    # panel. The panel driver registers itself into the .lcd_device_drive
    # linker section, so compiling it is what makes the panel selectable.
    sources=(
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
    ),
    # sdk_overlay.py:96-114 (LCD_INCLUDE_DIRS). The set below the first two is
    # the part of the vendor demo_ui profile's INCLUDES that the display path
    # needs and the demo_hello profile does not already carry, taken from
    # demo_ui/board/wl83/Makefile rather than added one failing include at a
    # time. Its LVGL, freetype and rlottie entries are deliberately excluded:
    # this build takes LVGL from TuyaOpen's src/liblvgl, and the other two are
    # text/vector renderers the panel path does not use. The cpu/<chip>/asm
    # entry is templated because it is the one chip-specific directory in the
    # set; the overlay only stages this feature into the wl83 SDK.
    includes=(
        "apps/common/lcd/include",
        "apps/common/lcd/lcd_driver",
        "apps/common",
        "apps/common/include",
        "apps/common/lzw",
        "apps/common/usb",
        "include_lib/driver/cpu/{cpu}/asm",
        "include_lib/driver/device/video",
        "include_lib/driver/device/video/pipeline",
        "include_lib/driver/device/video/pipeline/buffer",
        "include_lib/driver/device/video/pipeline/core",
    ),
    removal_note=(
        "Removing the display drops the twelve LCD/ISP/DMA2D sources, their "
        "include paths and video.a. video.a is not optional: it carries the "
        "prebuilt DSI/DPI/DMM/DMA2D implementations (dsi_dev_init, dpi_open, "
        "dmm_config, dma2d_init) that lcd_driver.c resolves into, and it has no "
        "source in the tree. Its extracted members are referenced from the "
        "isp_*.c.o objects staged by this same feature."
    ),
)


CAMERA = Feature(
    name="camera",
    kconfig="CONFIG_ENABLE_CAMERA",
    defines=("CONFIG_VIDEO_ENABLE=1",),
    sources=(
        "apps/common/camera/isp_scenes.c",
        "apps/common/camera/isp_tools.c",
        "apps/common/camera/gc0308/gc0308.c",
        "apps/common/camera/gc2145_mipi/gc2145_mipi.c",
        "apps/common/camera/sc1346_mipi/SC1346_mipi.c",
        "apps/common/camera/sc1346_mipi/SC1346_mipi_ae.c",
        "apps/common/camera/sc1346_mipi/SC1346_mipi_awb.c",
        "apps/common/camera/sc1346_mipi/SC1346_mipi_iq.c",
    ),
    includes=(
        "apps/common/camera/include",
        "apps/common/camera",
        "apps/common/camera/sc1346_mipi",
        "include_lib/server",
        "include_lib/server/include",
        "include_lib/driver/cpu/{cpu}/asm",
        "include_lib/driver/device/video",
        "include_lib/driver/device/video/pipeline",
        "include_lib/driver/device/video/pipeline/buffer",
        "include_lib/driver/device/video/pipeline/core",
    ),
    removal_note=(
        "Removing camera drops the camera sensor and ISP sources, video server and engine archives."
    ),
)


TLS = Feature(
    name="tls",
    kconfig=None,
    defines=(),
    sources=(),
    includes=(),
    removal_note=(
        "TuyaOpen's mbedTLS 3.1.0 is unconditional: src/libtls is always built "
        "into libtuyaos.a and the Jieli platform has no Kconfig switch for it, "
        "so this feature has no switch to turn off. It exists in the map because "
        "the vendor ships a second, partial mbedTLS (libmbedtls_3_4_0.a) and a "
        "crypto archive (libcrypto_mbedtls.a) that both sit on the link line "
        "next to it, and check_jieli_provider_map.py audits which side wins. "
        "Both archives are reached through the Wi-Fi chain -- libmbedtls_3_4_0.a "
        "only by https_cli.c, libcrypto_mbedtls.a only by wpasupplicant's sae.c "
        "and wpa_common.c -- so they cannot be dropped while Wi-Fi stays on."
    ),
)


LWIP = Feature(
    name="lwip",
    kconfig="CONFIG_ENABLE_LIBLWIP",
    kconfig_inverted=True,
    defines=(),
    sources=(),
    # sdk_overlay.py:542-545 emits these unconditionally today.
    includes=(
        "include_lib/net/lwip_2_2_0",
        "include_lib/net/lwip_2_2_0/lwip/src/include",
        "include_lib/net/lwip_2_2_0/lwip/src/include/compat",
        "include_lib/net/lwip_2_2_0/lwip/port",
    ),
    removal_note=(
        "CONFIG_ENABLE_LIBLWIP is inverted here: when it is set the build takes "
        "TuyaOpen's lwip-2.1.2 from src/liblwip, and only when it is unset does "
        "the vendor lwip_2_2_0.a stay on the link line. Removing the vendor "
        "archive therefore means enabling the TuyaOpen stack, not dropping the "
        "network stack. In the audited maps the archive's members are referenced "
        "by the TuyaOpen network adapter (objs/tuyaos_adapter/src/driver/"
        "tkl_network.c.o) as well as by its own tcp/sockets/netif members."
    ),
)


POWER = Feature(
    name="power",
    kconfig="CONFIG_ENABLE_TUYA_PM",
    defines=(),
    sources=(),
    includes=(),
    removal_note=(
        "TuyaOpen's power manager. It owns no vendor source and no archive: the "
        "work is in the adapter's TKL (tuyaos_adapter/src/system/tkl_sleep.c and "
        "src/driver/tkl_wifi.c), which is staged either way. What this feature "
        "switches is the board's low-power profile - board_config."
        "configure_lowpower() rewrites the staged TCFG_LOWPOWER_* macro so the "
        "core may idle. It is deliberately off unless the product asks for it, "
        "so a build without the power manager keeps the vendor's stock 'low "
        "power closed' profile byte for byte."
    ),
)


FEATURES: Mapping[str, Feature] = {
    feature.name: feature for feature in (WIFI, BLUETOOTH, DISPLAY, CAMERA, TLS, LWIP, POWER)
}

# The order the overlay appends feature-owned sources to the vendor Makefile's
# c_SRC_FILES. It is not alphabetical: it is the order the overlay emitted them
# before this table existed, and the staged Makefile is compared byte for byte.
SOURCE_ORDER: tuple[str, ...] = ("camera", "display", "bluetooth", "wifi")

# The order the overlay emits feature macros. CONFIG_NET_ENABLE comes first
# because that is the order of the existing DEFINES line.
DEFINE_ORDER: tuple[str, ...] = ("camera", "wifi", "bluetooth")

# The linker command line, in the order the archives appear inside the
# --start-group. It is deliberately not feature-grouped: the vendor Makefile
# interleaves the Wi-Fi, TLS and lwIP archives, and the group's rescan depends
# on the order, so a feature-grouped rewrite would not be byte-identical. Each
# entry names the feature that owns the archive; None means shared, never
# removed.
LINK_ORDER: tuple[tuple[str, Optional[str]], ...] = (
    ("cpu.a", None),
    ("system.a", None),
    ("hsm.a", None),
    ("event.a", None),
    ("common_lib.a", None),
    # sdk_overlay.py:576-581, minus the shared five above and the two archives
    # appended below. Attribution from the maps:
    ("wpasupplicant.a", "wifi"),  # wpa_supplicant.c, sae.c, eap.c, driver_wext.c
    ("http_cli.a", "wifi"),  # referenced by objs/apps/common/net/assign_macaddr.c.o
    ("https_cli.a", "wifi"),  # referenced by http_cli.c.o
    ("json.a", "wifi"),  # referenced by assign_macaddr.c.o and json_tokener.c.o
    ("libmbedtls_3_4_0.a", "tls"),  # referenced only by https_cli.c.o
    ("lwip_2_2_0.a", "lwip"),  # tcp.c, sockets.c, netif.c, tkl_network.c.o
    ("wl_wifi_sta.a", "wifi"),  # mlme.c, rtmp_init.c, net_device.c, wifi_connect.c
    # net_server.a is on the link line but no functional member of it is
    # extracted in either audited image -- only version.z.o. Its owner is
    # UNKNOWN, not Wi-Fi: the name suggests a Wi-Fi-side server but nothing in
    # the link products proves it. It is placed with Wi-Fi because that is the
    # company it keeps on the link line; the gating step must not treat this as
    # evidence.
    ("net_server.a", "wifi"),  # UNKNOWN: only version.z.o extracted
    ("wl_rf_common.a", "wifi"),  # wl_hw_init.c, wf_rf_init.c, wf_phy_mac_init.c
    ("btctrler.a", "bluetooth"),  # lmp.c, link_layer.c, hci_controller.c
    ("btstack.a", "bluetooth"),  # btstack_main.c, hci_vendor.c, tkl_bluetooth.c.o
    # crypto_toolbox_Osize.a and lib_ccm_aes.a are pulled in by BLE link-layer
    # and baseband-encryption objects: Crypto_hash.c, btctrler_task.c,
    # authentication.c, ll_bis.c, aes_cmac.c / bb_le_encrypt.c, aes128.c,
    # RF_ble.c. The wl82 map also shows one reference from system.a's
    # os_api.c.o, which is why they are not filed as unambiguously Bluetooth.
    ("crypto_toolbox_Osize.a", "bluetooth"),
    ("lib_ccm_aes.a", "bluetooth"),
)

# Archives only one chip links, appended after LINK_ORDER in this order.
CHIP_LIBRARIES: Mapping[str, tuple[tuple[str, Optional[str]], ...]] = {
    # AC792's WPA/SAE archives delegate crypto primitives to libcrypto_mbedtls.a;
    # AC791's equivalent implementation is bundled in its wpasupplicant archive.
    # The archive is referenced by wpasupplicant's sae.c and wpa_common.c.
    "wl83": (("libcrypto_mbedtls.a", "tls"),),
}

# Archives the display stack appends after the chip's own additions.
DISPLAY_LIBRARIES: tuple[tuple[str, Optional[str]], ...] = (("video.a", "display"),)
CAMERA_LIBRARIES: tuple[tuple[str, Optional[str]], ...] = (
    ("video_engine.a", "camera"),
    ("video_rec_server.a", "camera"),
    ("video.a", "camera"),
)


def _owned_library_entries() -> tuple[tuple[str, Optional[str]], ...]:
    """Every archive the link line can carry, with its owner, in no order.

    Used to derive each feature's ``libraries`` closure from the one ordered
    list, so a feature cannot claim an archive the linker never sees.
    """
    entries = list(LINK_ORDER)
    entries.extend(DISPLAY_LIBRARIES)
    entries.extend(CAMERA_LIBRARIES)
    for chip_entries in CHIP_LIBRARIES.values():
        entries.extend(chip_entries)
    return tuple(entries)


# The values a Kconfig bool can carry when it is set.
TRUTHY = ("y", "true", "True")


def enabled_features(params: Mapping[str, str], *, has_display: bool) -> tuple[str, ...]:
    """The features the evaluated config turns on, in the map's own order.

    A feature with no switch of its own is always part of the build. The rest
    follow their symbol, inverted where the feature says so -- the vendor lwIP
    is on while CONFIG_ENABLE_LIBLWIP is *unset*, because that switch selects
    TuyaOpen's own stack.

    Display follows the board rather than a symbol: whether the hardware
    carries a panel is a board fact, and the platform already reads it from
    there. CONFIG_ENABLE_DISPLAY is the application asking for the display
    stack, which is a different question.

    An absent symbol means the switch is off, which is how Kconfig writes a
    bool left at n.
    """
    enabled: list[str] = []
    for name, feature in FEATURES.items():
        if name == "display":
            if has_display:
                enabled.append(name)
            continue
        if feature.kconfig is None:
            enabled.append(name)
            continue
        set_to_y = params.get(feature.kconfig, "").strip() in TRUTHY
        if set_to_y != feature.kconfig_inverted:
            enabled.append(name)
    return tuple(enabled)


def link_libraries(chip_name: str, enabled: Iterable[str]) -> tuple[str, ...]:
    """Return the archives to link, in the vendor Makefile's order.

    ``enabled`` is the set of feature names to keep; the shared archives are
    always emitted.
    """
    wanted = set(enabled)
    ordered = [
        archive for archive, owner in LINK_ORDER if owner is None or owner in wanted
    ]
    ordered.extend(
        archive
        for archive, owner in CHIP_LIBRARIES.get(chip_name, ())
        if owner is None or owner in wanted
    )
    ordered.extend(
        archive for archive, owner in DISPLAY_LIBRARIES if owner is None or owner in wanted
    )
    ordered.extend(
        archive for archive, owner in CAMERA_LIBRARIES if owner is None or owner in wanted
    )
    seen = set()
    deduped = []
    for arch in ordered:
        if arch not in seen:
            seen.add(arch)
            deduped.append(arch)
    return tuple(deduped)


def feature_sources(enabled: Iterable[str]) -> tuple[str, ...]:
    """Return feature-owned sources in the order the overlay appends them."""
    wanted = set(enabled)
    seen = set()
    result = []
    for name in SOURCE_ORDER:
        if name in wanted:
            for source in FEATURES[name].sources:
                if source not in seen:
                    seen.add(source)
                    result.append(source)
    return tuple(result)


def feature_defines(enabled: Iterable[str]) -> tuple[str, ...]:
    """Return feature macros in the order the overlay emits them."""
    wanted = set(enabled)
    return tuple(
        define
        for name in DEFINE_ORDER
        if name in wanted
        for define in FEATURES[name].defines
    )


def feature_includes(enabled: Iterable[str], cpu: str) -> tuple[str, ...]:
    """Return feature include directories, with ``{cpu}`` filled in.

    Only the display feature's includes are emitted by name today; the Wi-Fi,
    Bluetooth and lwIP paths are listed here for the gating step but are still
    written unconditionally by the vendor template.
    """
    wanted = set(enabled)
    return tuple(
        directory.format(cpu=cpu)
        for name in SOURCE_ORDER
        if name in wanted
        for directory in FEATURES[name].includes
    )
