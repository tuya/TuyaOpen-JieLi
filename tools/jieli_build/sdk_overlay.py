"""Create and clean an isolated Jieli SDK staging tree."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

from .audio_profile import AUDIO_PROFILES, apply_audio_profile
from .board_config import (
    configure_ac79_devkit_memory, configure_ac79_log_uart, configure_ac792_devkit_memory,
    configure_ac792_log_uart, configure_audio_board, configure_full_stack_app_config,
    configure_full_stack_board, configure_lcd_app_config, configure_lcd_board,
    configure_lcd_board_header, configure_service_uart,
    configure_usb_download_app_config,
    configure_usb_download_board, configure_usb_download_board_header,
)
from .chip_profiles import JIELI_CHIPS, PLATFORM_ROOT, resolve_chip
from .errors import BuildError

# SDK-root-relative sources that keep the vendor USB slave stack serviced while
# the Tuya app runs. msd_upgrade.c owns go_mask_usb_updata() and user_setup.c
# answers the private GET_STATUS download-mode request; task_pc.c owns the
# persistent usb_msd0 worker. The adapter event hook below routes OTG events
# without compiling the SDK's unrelated SD mount implementation.
USB_DEVICE_SOURCES = (
    "apps/common/usb/usb_config.c",
    "apps/common/usb/usb_epbuf_manager.c",
    "apps/common/usb/device/usb_device.c",
    "apps/common/usb/device/descriptor.c",
    "apps/common/usb/device/user_setup.c",
    "apps/common/usb/device/msd.c",
    "apps/common/usb/device/msd_upgrade.c",
    "apps/common/usb/device/task_pc.c",
)

# Paths the vendor USB device stack expects on the include search path. The
# host/ entries are needed by usb_epbuf_manager.c even for a device-only build.
USB_INCLUDE_DIRS = (
    "apps/common",
    "apps/common/usb",
    "apps/common/usb/device",
    "apps/common/usb/host",
    "apps/common/usb/include",
    "apps/common/usb/include/host",
    "include_lib/driver/device/usb",
    "include_lib/driver/device/usb/device",
    "include_lib/driver/device/usb/host",
)


def usb_device_sources() -> list[str]:
    """Render the USB device sources relative to the demo board directory."""
    return ["../../../../../" + source for source in USB_DEVICE_SOURCES]


def usb_include_lines() -> list[str]:
    """Render the USB include paths relative to the demo board directory."""
    return ["-I../../../../../" + directory for directory in USB_INCLUDE_DIRS]


# The vendor LCD stack lives in apps/common/lcd and is not referenced by the
# staged demo_hello profile at all, so its objects are absent unless staged
# explicitly. The authoritative list of what the display path needs is the
# vendor's own demo_ui Makefile; this mirrors its LCD/dma2d entries.
#
# fb_component/ IS required. It reads like a UI-side compositor, but
# lcd_driver.c's DMM/DMA2D path resolves into it, and leaving it out leaves
# mipi_start_display/dpi_close/dmm_deinit/dma2d_free undefined at link time.
#
# Only the one panel the board selects is compiled. Panel drivers register
# themselves into the .lcd_device_drive linker section, so compiling a panel is
# what makes it selectable; the other nine would only cost image space.
#
# The vendor profile's freetype and rlottie entries are text/vector renderers
# and are not part of the panel path, so they are not staged.
LCD_SOURCES = (
    # CONFIG_VIDEO_ENABLE brings in the ISP pipeline inside video.a, which
    # references the scene tables these two provide.
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
LCD_INCLUDE_DIRS = (
    "apps/common/lcd/include",
    "apps/common/lcd/lcd_driver",
    # The set below is the part of the vendor demo_ui profile's INCLUDES that
    # the display path needs and the demo_hello profile does not already carry.
    # Taken from demo_ui/board/wl83/Makefile rather than added one failing
    # include at a time. Its LVGL, freetype and rlottie entries are deliberately
    # excluded: this build takes LVGL from TuyaOpen's src/liblvgl, and the other
    # two are text/vector renderers the panel path does not use.
    "apps/common",
    "apps/common/include",
    "apps/common/lzw",
    "apps/common/usb",
    "include_lib/driver/cpu/wl83/asm",
    "include_lib/driver/device/video",
    "include_lib/driver/device/video/pipeline",
    "include_lib/driver/device/video/pipeline/buffer",
    "include_lib/driver/device/video/pipeline/core",
)


def lcd_sources() -> list[str]:
    """Render the LCD sources relative to the demo board directory."""
    return ["../../../../../" + source for source in LCD_SOURCES]


def lcd_include_lines() -> list[str]:
    """Render the LCD include paths relative to the demo board directory."""
    return ["-I../../../../../" + directory for directory in LCD_INCLUDE_DIRS]


# The vendor demo_hello Makefile lists its own peripheral self-test programs and
# the dhrystone benchmark in c_SRC_FILES. Nothing in a TuyaOpen image references
# them: -flto is on and sdk.map carries no apps/common/example entry, so all 32
# objects are discarded at link time. They are still recompiled on every build --
# create_staging_tree recreates the staging tree each time -- and they are about
# 45% of the vendor-side objects, so they dominate the vendor compile.
VENDOR_EXAMPLE_SOURCE_PREFIX = "../../../../../apps/common/example/"


def drop_vendor_example_sources(content: str, expects_examples: bool = False) -> str:
    """Remove the vendor demo's own example sources from c_SRC_FILES.

    Every one of those lines ends with a line continuation, so removing whole
    lines keeps the surrounding list valid.

    Only the wl83 demo Makefile lists these sources; the wl82 one lists none, so
    an empty result is a valid vendor layout rather than a format error. The
    caller says which it expects: for a chip that does list them, finding none
    means the Makefile moved and the strip silently stopped working, which would
    quietly put ~45% of the vendor compile back.
    """
    kept = [
        line
        for line in content.splitlines(keepends=True)
        if not line.lstrip().startswith(VENDOR_EXAMPLE_SOURCE_PREFIX)
    ]
    dropped = len(content.splitlines()) - len(kept)
    if expects_examples and dropped == 0:
        raise BuildError(
            "Jieli demo Makefile was expected to list "
            f"{VENDOR_EXAMPLE_SOURCE_PREFIX} sources but lists none"
        )
    return "".join(kept)


def link_directory(link: Path, target: Path) -> None:
    try:
        link.symlink_to(target, target_is_directory=True)
        return
    except OSError:
        if os.name != "nt":
            raise

    result = subprocess.run(
        ["cmd.exe", "/c", "mklink", "/J", str(link), str(target)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise OSError(result.returncode, f"cannot link {link} to {target}: {detail}")


def _ignore_cpu_build_outputs(directory: str, names: list[str]) -> set[str]:
    generated = {
        "app.bin",
        "download.bat",
        "download.sh",
        "isd_config.ini",
        "isd_config_loader.ini",
        "jl_isd.fw",
        "jl_isd.ufw",
        "sdk.elf",
        "sdk.elf.objs.txt",
        "sdk.ld",
        "sdk.map",
        "sdk_used_list.used",
        "section.txt",
    }
    ignored = {name for name in names if name in generated or name.endswith((".bc", ".o", ".d"))}
    if Path(directory).parent.name == "cpu" and "liba" in names:
        ignored.add("liba")
    return ignored


def _ignore_demo_build_outputs(directory: str, names: list[str]) -> set[str]:
    return {
        name
        for name in names
        if name in {"objs", "dist", ".build", "section.txt"} or name.endswith((".o", ".d"))
    }


def stage_sdk_inputs(sdk_root: Path, build_root: Path, chip) -> Path:
    """Copy writable SDK paths and link SDK directories used only as inputs."""
    vendor_root = sdk_root / chip.sdk_source_relative
    source_root = build_root / chip.sdk_source_relative
    source_root.mkdir(parents=True, exist_ok=True)

    for name in ("include_lib", "lib", "tools"):
        source = vendor_root / name
        if not source.is_dir():
            raise BuildError(f"Jieli SDK input directory is missing: {source}")
        # The make/post-build flow reads these trees; generated artifacts are
        # written under cpu/<chip>/tools, which is part of the copied CPU tree.
        link_directory(source_root / name, source)

    vendor_cpu_root = vendor_root / "cpu" / chip.cpu
    staged_cpu_root = source_root / "cpu" / chip.cpu
    if not vendor_cpu_root.is_dir():
        raise BuildError(f"Jieli SDK CPU profile is missing: {vendor_cpu_root}")
    shutil.copytree(vendor_cpu_root, staged_cpu_root, ignore=_ignore_cpu_build_outputs)
    vendor_liba = vendor_cpu_root / "liba"
    if vendor_liba.is_dir():
        # The linker consumes the prebuilt archives without modifying them.
        link_directory(staged_cpu_root / "liba", vendor_liba)

    vendor_common = vendor_root / "apps" / "common"
    staged_apps = source_root / "apps"
    staged_common = staged_apps / "common"
    staged_common.mkdir(parents=True, exist_ok=True)
    for item in vendor_common.iterdir():
        destination = staged_common / item.name
        if item.name == "movable" and item.is_dir():
            # pre_build writes section.txt here, so keep this tree local.
            shutil.copytree(item, destination, ignore=_ignore_demo_build_outputs)
        elif item.is_dir():
            link_directory(destination, item)
        else:
            shutil.copy2(item, destination)

    demo_source = vendor_root / "apps" / "demo" / "demo_hello"
    shutil.copytree(demo_source, staged_apps / "demo" / "demo_hello", ignore=_ignore_demo_build_outputs)
    return source_root


def stage_audio_sdk_sources(source_overlay_root: Path, vendor_root: Path, chip_name: str) -> None:
    """Expose the vendor audio trees the audio profile's inputs reference.

    The minimal staging tree copies only cpu, apps, include_lib, lib and tools;
    trees the profile lists under staged_vendor_trees live outside them, so
    link those in read-only.
    """
    profile = AUDIO_PROFILES.get(chip_name)
    if profile is None:
        return
    missing = [
        required for required in profile.staged_vendor_files
        if not (vendor_root / required).is_file()
    ]
    if missing:
        raise BuildError(f"{chip_name} audio SDK configuration is missing: {missing[0]}")
    for tree in profile.staged_vendor_trees:
        destination = source_overlay_root / tree
        if not destination.exists():
            link_directory(destination, vendor_root / tree)


def clean_staging_tree(staging_root: Path) -> None:
    """Remove only generated staging files, never files under the vendor SDK."""
    if staging_root.is_dir():
        shutil.rmtree(staging_root)


def read_adapter_sources(platform_root: Path, chip_name: str) -> list[str]:
    """Read the source manifest shared by CMake and the SDK Makefile bridge."""
    if chip_name not in JIELI_CHIPS:
        raise BuildError(f"unsupported Jieli chip '{chip_name}'")
    manifest = platform_root / "tuyaos" / "tuyaos_adapter" / "adapter_sources.txt"
    if not manifest.is_file():
        raise BuildError(f"Jieli adapter source manifest is missing: {manifest}")

    selected: list[str] = []
    seen: set[str] = set()
    for line_number, raw_line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        source = raw_line.partition("#")[0].strip().replace("\\", "/")
        if not source:
            continue
        path = Path(source)
        if path.is_absolute() or ".." in path.parts or not source.endswith(".c"):
            raise BuildError(f"invalid adapter source at {manifest}:{line_number}: {source}")
        if source in seen:
            raise BuildError(f"duplicate adapter source at {manifest}:{line_number}: {source}")
        seen.add(source)

        parts = path.parts
        if len(parts) >= 3 and parts[:2] == ("src", "chip"):
            if parts[2] not in ("common", chip_name):
                continue
        if not (manifest.parent / source).is_file():
            raise BuildError(f"adapter source does not exist at {manifest}:{line_number}: {source}")
        selected.append(source)

    if not selected:
        raise BuildError(f"Jieli adapter source manifest has no sources for {chip_name}: {manifest}")
    return selected


def _make_path(path: Path) -> str:
    """Render host paths in a form that survives Makefile shell expansion."""
    return path.as_posix()


def build_make_command(sdk_root: Path, tool_dir: Path, jobs: int = 1, chip=None) -> list[str]:
    if jobs < 1:
        raise ValueError("jobs must be at least 1")
    chip = chip or resolve_chip()
    board_dir = sdk_root / chip.sdk_source_relative / chip.board_build_relative
    elf_target = "../../../../../" + chip.elf_relative.as_posix()
    command = [
        "make",
        "-C",
        str(board_dir),
        f"TOOL_DIR={_make_path(tool_dir)}",
        f"-j{jobs}",
        "pre_build",
        elf_target,
    ]
    if os.name == "nt":
        command.insert(4, "LINK_AT=0")
        helper = Path(__file__).resolve().parent / "mkdir.py"
        command.insert(5, f'MKDIR="{Path(sys.executable).as_posix()}" "{helper.as_posix()}" -p')
    return command

def create_staging_tree(
    sdk_root: Path,
    staging_root: Path,
    tuyaopen_root: Path,
    header_dir: Optional[Path] = None,
    tuya_lib_dir: Optional[Path] = None,
    uart_log_port: int = 0,
    uart_log_baudrate: int = 115200,
    platform_root: Path = PLATFORM_ROOT,
    chip=None,
    board_name: Optional[str] = None,
) -> Path:
    """Create a small overlay tree without modifying the vendor checkout."""
    if staging_root.exists():
        shutil.rmtree(staging_root)

    build_root = staging_root / "build"
    chip = chip or resolve_chip()
    vendor_root = sdk_root / chip.sdk_source_relative
    stage_sdk_inputs(sdk_root, build_root, chip)
    source_overlay_root = build_root / chip.sdk_source_relative
    stage_audio_sdk_sources(source_overlay_root, vendor_root, chip.name)
    apps_root = source_overlay_root / "apps"
    board_file = source_overlay_root / chip.board_build_relative / "board.c"
    app_config_file = source_overlay_root / "apps/demo/demo_hello/include/app_config.h"
    if chip.name == "wl82":
        configure_ac79_devkit_memory(app_config_file)
        configure_ac79_log_uart(board_file, uart_log_port, uart_log_baudrate)
        configure_service_uart(board_file)
    else:
        configure_ac792_log_uart(
            source_overlay_root / "apps/demo/demo_hello/board/wl83/board_demo.h",
            uart_log_port,
            uart_log_baudrate,
        )
    reference_board_file = (
        vendor_root / "apps/demo/demo_wifi_ext/board/wl83/board.c"
        if chip.name == "wl83"
        else None
    )
    configure_full_stack_board(board_file, reference_board_file)
    configure_full_stack_app_config(app_config_file)
    configure_usb_download_board(
        board_file, "TCFG_USB_DEVICE" if chip.name == "wl83" else "0x03",
    )
    if chip.name == "wl83":
        configure_usb_download_board_header(
            source_overlay_root / "apps/demo/demo_hello/board/wl83/board_demo.h",
            "TCFG_HUSB_DEVICE" if board_name == "AC792N_Develop_Board" else "TCFG_FUSB_DEVICE",
        )
        configure_ac792_devkit_memory(
            source_overlay_root / "apps/demo/demo_hello/board/wl83/chip_cfg.h",
            source_overlay_root / "apps/demo/demo_hello/board/wl83/board_demo.h",
        )
        configure_lcd_board_header(
            source_overlay_root / "apps/demo/demo_hello/board/wl83/board_demo.h",
        )
        configure_lcd_app_config(app_config_file)
        configure_lcd_board(board_file)
    else:
        configure_usb_download_app_config(app_config_file)
    profile = AUDIO_PROFILES.get(chip.name)
    if profile is not None and profile.board_declarations:
        configure_audio_board(
            board_file,
            tuyaopen_root / "boards/JIELI" / profile.audio_board_name / "audio_config.h",
            profile,
        )
    shutil.copytree(
        platform_root / "tuyaos" / "entry",
        source_overlay_root / "tuyaos" / "entry",
    )
    shutil.copytree(
        platform_root / "tuyaos/tuyaos_adapter",
        source_overlay_root / "tuyaos_adapter",
    )
    utilities_root = tuyaopen_root / "tools/porting/adapter/utilities"
    if utilities_root.is_dir():
        shutil.copytree(utilities_root, source_overlay_root / "tuya_utilities")

    makefile = source_overlay_root / chip.board_build_relative / "Makefile"
    content = makefile.read_text(encoding="utf-8")
    vendor_main = "../../../../../apps/demo/demo_hello/app_main.c"
    if vendor_main not in content:
        raise BuildError(f"Jieli demo Makefile has no app_main source: {makefile}")
    content = content.replace(vendor_main, "../../../../../tuyaos/entry/jieli_app_entry.c")
    content = drop_vendor_example_sources(content, chip.demo_lists_example_sources)
    extra_sources_list = [
        "../../../../../tuyaos_adapter/" + source
        for source in read_adapter_sources(platform_root, chip.name)
    ]
    extra_sources_list.extend(
        [
            "../../../../../tuya_utilities/src/tuya_hashmap.c",
            "../../../../../tuya_utilities/src/tuya_list.c",
            "../../../../../tuya_utilities/src/tuya_mem_heap.c",
            "../../../../../tuya_utilities/src/tuya_queue.c",
            "../../../../../tuya_utilities/src/tuya_ringbuf.c",
            "../../../../../tuya_utilities/src/tuya_smartpointer.c",
            "../../../../../tuya_utilities/src/tuya_tools.c",
        ]
    )
    extra_sources_list.extend(usb_device_sources())
    extra_sources_list.append("../../../../../tuyaos/entry/jieli_usb_download.c")
    if chip.name == "wl83":
        extra_sources_list.extend(lcd_sources())
    extra_sources_list.extend(
        [
            "../../../../../apps/common/config/bt_profile_config.c",
            "../../../../../apps/common/config/log_config/app_config.c",
            "../../../../../apps/common/config/log_config/lib_btctrler_config.c",
            "../../../../../apps/common/config/log_config/lib_btstack_config.c",
            "../../../../../apps/common/net/assign_macaddr.c",
            "../../../../../apps/common/net/config_network.c",
            "../../../../../apps/common/net/platform_cfg.c",
            "../../../../../apps/common/net/wifi_conf.c",
        ]
    )
    source_lines = ["c_SRC_FILES += \\"]
    for index, source in enumerate(extra_sources_list):
        suffix = " \\" if index + 1 < len(extra_sources_list) else ""
        source_lines.append(f"    {source}{suffix}")
    extra_sources = "\n".join(source_lines) + "\n"
    marker = "c_OBJS    :="
    if marker not in content:
        raise BuildError(f"AC79 demo Makefile has no object list marker: {makefile}")
    content = content.replace(marker, extra_sources + "\n" + marker, 1)
    if chip.name == "wl82":
        # The AC791 Wi-Fi/BLE SDK libraries use the SDRAM linker layout, so
        # remove the demo template's no-SDRAM define in staging.
        content = content.replace("\t-DCONFIG_NO_SDRAM_ENABLE \\\n", "", 1)
    content += "\n"
    content += "INCLUDES += \\\n"
    content += "    -I../../../../../tuyaos_adapter/include \\\n"
    adapter_include = source_overlay_root / "tuyaos_adapter" / "include"
    for include_dir in sorted(path for path in adapter_include.iterdir() if path.is_dir()):
        content += f"    -I../../../../../tuyaos_adapter/include/{include_dir.name} \\\n"
    # tkl_init.h follows the shared TuyaOpen layout under init/include.
    content += "    -I../../../../../tuyaos_adapter/include/init/include \\\n"
    tuyaopen_root_make = _make_path(tuyaopen_root)
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/system \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/uart \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/init/include \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/utilities/include \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/flash \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/network \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/wifi \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/bluetooth \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/timer \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/security \\\n"
    # tkl_adc.h lives beside the other TKL domain headers, not under any
    # per-chip profile, so every chip's staged Makefile needs it.
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/adc \\\n"
    if chip.name == "wl83":
        # tkl_gpio.c includes the shared GPIO contract from this domain. The
        # MIPI-DSI contract is not here: it ships with this platform under
        # tuyaos_adapter/include/mipi_dsi, which the loop above already adds.
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/gpio \\\n"
    if chip.name in AUDIO_PROFILES:
        # The audio adapter's public TKL headers (tkl_audio.h, tkl_vad.h,
        # tkl_kws.h) exist only under these TuyaOpen adapter domains.
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/media \\\n"
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/vad \\\n"
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/kws \\\n"
    content += "    -I../../../../../apps/common/include \\\n"
    content += "    -I../../../../../apps/common/config/include \\\n"
    for include in usb_include_lines():
        content += f"    {include} \\\n"
    if chip.name == "wl83":
        for include in lcd_include_lines():
            content += f"    {include} \\\n"
    content += "    -I../../../../../include_lib/btstack \\\n"
    content += "    -I../../../../../include_lib/btstack/le \\\n"
    content += "    -I../../../../../include_lib/btctrler \\\n"
    content += f"    -I../../../../../include_lib/btctrler/port/{chip.cpu} \\\n"
    content += f"    -I{tuyaopen_root_make}/src/common/include\n"
    content += "INCLUDES += \\\n"
    content += "    -I../../../../../include_lib/driver/device \\\n"
    content += f"    -I../../../../../include_lib/driver/cpu/{chip.cpu} \\\n"
    content += "    -I../../../../../include_lib/net/lwip_2_2_0 \\\n"
    content += "    -I../../../../../include_lib/net/lwip_2_2_0/lwip/src/include \\\n"
    content += "    -I../../../../../include_lib/net/lwip_2_2_0/lwip/src/include/compat \\\n"
    content += "    -I../../../../../include_lib/net/lwip_2_2_0/lwip/port \\\n"
    content += "    -I../../../../../include_lib/system \\\n"
    content += "    -I../../../../../include_lib/system/generic \\\n"
    content += "    -I../../../../../include_lib/net \\\n"
    content += "    -I../../../../../include_lib/utils \\\n"
    content += "    -I../../../../../include_lib/utils/syscfg \\\n"
    content += "    -I../../../../../include_lib/utils/event \\\n"
    content += "    -I../../../../../include_lib\n"
    content += "INCLUDES += -I../../../../../include_lib/net\n"
    selected_chip_define = "JIELI_SELECTED_CHIP_WL83" if chip.name == "wl83" else "JIELI_SELECTED_CHIP_WL82"
    content += f"DEFINES += -D{selected_chip_define}=1\n"
    content += "CFLAGS += -include stdbool.h -DBOOL_DEFINE_CONFLICT\n"
    content += "DEFINES += -DCONFIG_NET_ENABLE=1 -DCONFIG_BT_ENABLE=1 -DCONFIG_TWS_ENABLE -DCONFIG_BTCTRLER_TASK_DEL_ENABLE -DCONFIG_LMP_CONN_SUSPEND_ENABLE -DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE\n"
    if chip.name in AUDIO_PROFILES:
        # The vendor media runtime and the staged app entry both key off this
        # switch; without it tkl_jieli_audio_prepare compiles out entirely.
        content += "DEFINES += -DCONFIG_MEDIA_ENABLE -DCONFIG_AUDIO_ENABLE -DCONFIG_AUDIO_ONCHIP\n"
    if tuya_lib_dir is None:
        raise BuildError("TuyaOpen library directory is required for the Jieli image")
    content += "LFLAGS += \\\n"
    # The vendor LTO used-symbol list does not reference wlc_main when
    # Wi-Fi is entered through the Tuya TKL layer. Force-retain the
    # vendor entry point so cpu.a[wlc.c.o] is extracted by the linker.
    content += "    -u wlc_main \\\n"
    tuya_lib_dir_make = _make_path(tuya_lib_dir)
    content += f"    --start-group {tuya_lib_dir_make}/libtuyaapp.a {tuya_lib_dir_make}/libtuyaos.a \\\n"
    # AC791's vendor Makefile links cpu.a/system.a before this Tuya
    # library group. BLE/Wi-Fi archives introduce provider references
    # later, so repeat those archives inside the group for a rescan.
    liba = f"../../../../../cpu/{chip.cpu}/liba"
    libraries = (
        "cpu.a", "system.a", "hsm.a", "event.a", "common_lib.a",
        "wpasupplicant.a", "http_cli.a", "https_cli.a", "json.a",
        "libmbedtls_3_4_0.a", "lwip_2_2_0.a", "wl_wifi_sta.a",
        "net_server.a", "wl_rf_common.a", "btctrler.a", "btstack.a",
        "crypto_toolbox_Osize.a", "lib_ccm_aes.a",
    )
    if chip.name == "wl83":
        # AC792's WPA/SAE archives delegate crypto primitives to this
        # provider; AC791's equivalent implementation is bundled in its
        # wpasupplicant archive.
        libraries += ("libcrypto_mbedtls.a",)
        # video.a carries the DSI/DPI/DMM/DMA2D driver implementations that
        # lcd_driver.c resolves into (dsi_dev_init, dpi_open, dmm_config,
        # dma2d_init, ...). They have no source in the tree - the vendor ships
        # them prebuilt, and the demo_ui profile links this archive for exactly
        # that reason. Only needed once the display stack is staged.
        libraries += ("video.a",)
    for library in libraries:
        content += f"    {liba}/{library} \\\n"
    content += "    --end-group\n"
    if header_dir is not None:
        content += f"INCLUDES += -I{_make_path(header_dir)}\n"
    makefile.write_text(content, encoding="utf-8")

    if profile is not None:
        # The staged entry replaced app_main.c in the Makefile, and it owns
        # the task table the audio tasks must be registered in.
        apply_audio_profile(
            makefile,
            source_overlay_root / "tuyaos" / "entry" / "jieli_app_entry.c",
            chip.name,
        )
    return build_root
