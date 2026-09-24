"""Host-side helpers for the TuyaOpen Jieli build bridge (multi-chip).

One platform, several chips: each chip pins its own vendor SDK under
chip/<cpu>/ and contributes a probe file used to locate a checkout on disk.
Select the target with the JIELI_CHIP environment variable (default: wl82).
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Mapping, Optional


MODULE_ROOT = Path(__file__).resolve().parent


class JielichipConfig:
    """Per-chip layout of a vendor SDK checkout plus the build entry points."""

    def __init__(
        self,
        name: str,
        cpu: str,
        sdk_dir: str,
        probe: Path,
        sdk_source_relative: Path,
        board_build_relative: Path,
        elf_relative: Path,
        tools_relative: Path,
        raw_app_sections: tuple[str, ...],
        legacy_sdk_dirs=(),
    ):
        self.name = name
        self.cpu = cpu
        self.sdk_dir = sdk_dir
        self.probe = probe
        self.sdk_source_relative = sdk_source_relative
        self.board_build_relative = board_build_relative
        self.elf_relative = elf_relative
        self.tools_relative = tools_relative
        self.raw_app_sections = raw_app_sections
        self.legacy_sdk_dirs = tuple(legacy_sdk_dirs)


JIELI_CHIPS = {
    "wl82": JielichipConfig(
        name="wl82",
        cpu="wl82",
        sdk_dir="chip/wl82/AC79_AIoT_SDK",
        # Legacy layout kept as a disk fallback: the SDK used to sit at the
        # module root and existing checkouts may still have it there.
        legacy_sdk_dirs=("AC79_AIoT_SDK",),
        probe=Path("apps/demo/demo_hello/board/wl82/Makefile"),
        sdk_source_relative=Path("."),
        board_build_relative=Path("apps/demo/demo_hello/board/wl82"),
        elf_relative=Path("cpu/wl82/tools/sdk.elf"),
        tools_relative=Path("cpu/wl82/tools"),
        raw_app_sections=(".text", ".data", ".ram0_data", ".cache_ram_data", ".dynamic_data"),
    ),
    "wl83": JielichipConfig(
        name="wl83",
        cpu="wl83",
        sdk_dir="chip/wl83/AC792_SDK",
        legacy_sdk_dirs=(),
        probe=Path("sdk/apps/demo/demo_hello/board/wl83/Makefile"),
        sdk_source_relative=Path("sdk"),
        board_build_relative=Path("apps/demo/demo_hello/board/wl83"),
        elf_relative=Path("cpu/wl83/tools/sdk.elf"),
        tools_relative=Path("cpu/wl83/tools"),
        # Match AC792 SDK's Windows download.c section concatenation order.
        raw_app_sections=(".text", ".data", ".dcache_ram_data", ".video_ram_data", ".ram0_data"),
    ),
}


def resolve_chip(environ: Optional[Mapping[str, str]] = None):
    env = os.environ if environ is None else environ
    name = env.get("JIELI_CHIP", "wl82").strip() or "wl82"
    chip = JIELI_CHIPS.get(name)
    if chip is None:
        raise BuildError(f"unknown JIELI_CHIP '{name}'; expected one of {sorted(JIELI_CHIPS)}")
    return chip


# Retained as module-level constants for the wl82 default path; new callers
# should use resolve_chip() instead.
BOARD_BUILD_RELATIVE = JIELI_CHIPS["wl82"].board_build_relative
TOOLS_RELATIVE = JIELI_CHIPS["wl82"].tools_relative


TOOL_ALIASES = {
    "clang": ("clang", "clang.exe"),
    "lto-wrapper": (
        "lto-wrapper",
        "lto-wrapper.exe",
        "pi32v2-lto-wrapper",
        "pi32v2-lto-wrapper.exe",
    ),
    "lto-ar": ("lto-ar", "lto-ar.exe", "pi32v2-lto-ar", "pi32v2-lto-ar.exe"),
    "objcopy": ("objcopy", "objcopy.exe", "llvm-objcopy", "llvm-objcopy.exe"),
    "objdump": ("objdump", "objdump.exe", "llvm-objdump", "llvm-objdump.exe"),
    "objsizedump": (
        "objsizedump",
        "objsizedump.exe",
        "llvm-objsizedump",
        "llvm-objsizedump.exe",
    ),
}


class BuildError(RuntimeError):
    """Raised when a required Jieli build input is unavailable."""


def resolve_tool_path(tool_dir: Path, logical_name: str) -> Path:
    for name in TOOL_ALIASES[logical_name]:
        candidate = tool_dir / name
        if candidate.is_file():
            return candidate
    return tool_dir / TOOL_ALIASES[logical_name][0]


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


def configure_ac79_log_uart(board_file: Path, uart_port: int, baudrate: int) -> None:
    """Match the official AC79 DevKitBoard UART1/PB3 logging configuration."""
    if uart_port != 1:
        raise BuildError("AC79_DevKitBoard logging uses SDK UART1 (PB3)")
    if not board_file.is_file():
        return

    content = board_file.read_text(encoding="utf-8")
    marker = "UART1_PLATFORM_DATA_BEGIN(uart1_data)"
    end = "UART1_PLATFORM_DATA_END();"
    start = content.find(marker)
    if start < 0:
        uart2_marker = "UART2_PLATFORM_DATA_BEGIN(uart2_data)"
        insertion = content.find(uart2_marker)
        if insertion < 0:
            raise BuildError(f"Jieli board file has no UART2 insertion point: {board_file}")
        uart_block = (
            "UART1_PLATFORM_DATA_BEGIN(uart1_data)\n"
            f"    .baudrate = {baudrate},\n"
            "    .port = PORT_REMAP,\n"
            "    .output_channel = OUTPUT_CHANNEL0,\n"
            "    .tx_pin = IO_PORTB_03,\n"
            "    .rx_pin = -1,\n"
            "    .max_continue_recv_cnt = 1024,\n"
            "    .idle_sys_clk_cnt = 500000,\n"
            "    .clk_src = PLL_48M,\n"
            "    .flags = UART_DEBUG,\n"
            "UART1_PLATFORM_DATA_END();\n\n"
        )
        content = content[:insertion] + uart_block + content[insertion:]
    else:
        finish = content.find(end, start)
        if finish < 0:
            raise BuildError(f"Jieli board file has an incomplete UART1 block: {board_file}")
        finish += len(end)
        uart_block = content[start:finish]
        replacements = (
            (r"\.baudrate\s*=\s*\d+\s*,", f".baudrate = {baudrate},"),
            (r"\.port\s*=\s*[^,]+,", ".port = PORT_REMAP,"),
            (r"\.output_channel\s*=\s*[^,]+,", ".output_channel = OUTPUT_CHANNEL0,"),
            (r"\.tx_pin\s*=\s*[^,]+,", ".tx_pin = IO_PORTB_03,"),
            (r"\.rx_pin\s*=\s*[^,]+,", ".rx_pin = -1,"),
        )
        for pattern, replacement in replacements:
            uart_block, count = re.subn(pattern, replacement, uart_block, count=1)
            if count != 1:
                raise BuildError(f"AC79 UART1 log setting was not found in {board_file}")
        content = content[:start] + uart_block + content[finish:]

    init_marker = "uart_init(&uart2_data);"
    content, count = content.replace(init_marker, "uart_init(&uart1_data);", 1), content.count(init_marker)
    if count != 1:
        raise BuildError(f"AC79 debug_uart_init does not initialize UART2 in {board_file}")
    board_file.write_text(content, encoding="utf-8")


def configure_ac792_log_uart(board_header: Path, uart_port: int, baudrate: int) -> None:
    """Set AC792N's documented UART0 log baud in the staged board profile."""
    if uart_port != 0:
        raise BuildError("AC792N_Develop_Board logging uses SDK UART0 (PD1)")
    content = board_header.read_text(encoding="utf-8")
    content, count = re.subn(
        r"(#define\s+TCFG_UART0_BAUDRATE\s+)\d+",
        rf"\g<1>{baudrate}",
        content,
        count=1,
    )
    if count != 1:
        raise BuildError(f"AC792 UART0 baudrate was not found in {board_header}")
    board_header.write_text(content, encoding="utf-8")


def configure_service_uart(board_file: Path) -> None:
    """Move Tuya logical UART0/CLI to UART2, separate from UART1/PB3 logs."""
    if not board_file.is_file():
        return

    content = board_file.read_text(encoding="utf-8")
    begin = "UART2_PLATFORM_DATA_BEGIN(uart2_data)"
    end = "UART2_PLATFORM_DATA_END();"
    start = content.find(begin)
    finish = content.find(end, start) if start >= 0 else -1
    if start < 0 or finish < 0:
        raise BuildError(f"Jieli board file has no complete UART2 service block: {board_file}")
    finish += len(end)
    uart_block = content[start:finish]
    replacements = (
        (r"\.baudrate\s*=\s*\d+\s*,", ".baudrate = 115200,"),
        (r"\.port\s*=\s*[^,]+,", ".port = PORTB_6_7,"),
        (r"\.tx_pin\s*=\s*[^,]+,", ".tx_pin = IO_PORTB_06,"),
        (r"\.rx_pin\s*=\s*[^,]+,", ".rx_pin = IO_PORTB_07,"),
        (r"\.flags\s*=\s*[^,]+,", ".flags = 0,"),
    )
    for pattern, replacement in replacements:
        uart_block, count = re.subn(pattern, replacement, uart_block, count=1)
        if count != 1:
            raise BuildError(f"AC79 UART2 service setting was not found in {board_file}")
    content = content[:start] + uart_block + content[finish:]

    if '{"uart2", &uart_dev_ops, (void *)&uart2_data },' not in content:
        raise BuildError(f"Jieli board file has no uart2 service device entry: {board_file}")

    board_file.write_text(content, encoding="utf-8")


def configure_full_stack_wifi(board_file: Path, reference_board_file: Optional[Path] = None) -> None:
    """Add the chip-specific RF calibration required by the WiFi SDK libraries."""
    if not board_file.is_file():
        return

    content = board_file.read_text(encoding="utf-8")
    if "wifi_calibration_param wifi_calibration_param" in content:
        return

    marker = "/**************************  POWER config ****************************/"
    calibration_block = None
    if reference_board_file is not None and reference_board_file.is_file():
        reference = reference_board_file.read_text(encoding="utf-8")
        start = reference.find("const struct wifi_calibration_param wifi_calibration_param = {")
        finish = reference.find("};", start) if start >= 0 else -1
        if start >= 0 and finish >= 0:
            calibration_block = reference[start : finish + 2]
    if calibration_block is None:
        calibration_block = (
            "const struct wifi_calibration_param wifi_calibration_param = {\n"
            "    .xosc_l = 0xb,\n"
            "    .xosc_r = 0xb,\n"
            "    .pa_trim_data = {1, 7, 4, 7, 11, 1, 7},\n"
            "    .mcs_dgain = {45, 45, 45, 42, 60, 60, 75, 70,\n"
            "                   62, 52, 50, 38, 62, 80, 70, 62,\n"
            "                   50, 48, 40, 36},\n"
            "};"
        )
    calibration = (
        "#if defined CONFIG_BT_ENABLE || defined CONFIG_WIFI_ENABLE\n"
        "#include \"wifi/wifi_connect.h\"\n"
        f"{calibration_block}\n"
        "#endif\n\n"
    )
    if marker in content:
        content = content.replace(marker, calibration + marker, 1)
    else:
        board_init_marker = "void board_init("
        if board_init_marker not in content:
            raise BuildError(f"Jieli board file has no calibration insertion point: {board_file}")
        content = content.replace(board_init_marker, calibration + board_init_marker, 1)
    board_file.write_text(content, encoding="utf-8")


def configure_full_stack_app_config(app_config_file: Path) -> None:
    """Enable the vendor Wi-Fi/BLE pieces used by the Tuya full-stack image."""
    if not app_config_file.is_file():
        return

    content = app_config_file.read_text(encoding="utf-8")
    marker = "/* TuyaOpen Jieli full-stack network/BLE configuration. */"
    if marker in content:
        return

    config = """/* TuyaOpen Jieli full-stack network/BLE configuration. */
#define CONFIG_NET_ENABLE                  1
#define CONFIG_WIFI_ENABLE

#ifdef CONFIG_BT_ENABLE
#define TCFG_BT_MODE                       BT_NORMAL
#define CONFIG_BT_RX_BUFF_SIZE              0
#define CONFIG_BT_TX_BUFF_SIZE              0
#define TCFG_USER_BLE_ENABLE                1
#define TCFG_USER_BT_CLASSIC_ENABLE         0
#define BT_NET_CFG_EN                       0
#define BT_NET_HID_EN                       0
#define TCFG_BLE_SECURITY_EN                0
#endif

"""
    insert_at = content.rfind("#endif")
    if insert_at < 0:
        raise BuildError(f"Jieli app_config.h has no final #endif: {app_config_file}")
    app_config_file.write_text(content[:insert_at] + config + content[insert_at:], encoding="utf-8")


def configure_ac792_devkit_memory(chip_config_file: Path, board_config_file: Path) -> None:
    """Use the AC792N development-board reference SKU (AC7926A) memory map."""
    content = chip_config_file.read_text(encoding="utf-8")
    content = content.replace("(1 * 1024 * 1024)", "(8 * 1024 * 1024)", 1)
    content = content.replace("(2 * 1024 * 1024)", "(16 * 1024 * 1024)", 1)
    chip_config_file.write_text(content, encoding="utf-8")

    content = board_config_file.read_text(encoding="utf-8")
    content = content.replace("#define CONFIG_NO_SDRAM_ENABLE", "/* External DDR is enabled for AC7926A DevKit. */", 1)
    board_config_file.write_text(content, encoding="utf-8")


def configure_full_stack_board(board_file: Path, reference_board_file: Optional[Path] = None) -> None:
    """Apply the vendor board initialization needed before Tuya starts networking."""
    configure_full_stack_wifi(board_file, reference_board_file)
    content = board_file.read_text(encoding="utf-8")
    if "cfg_file_parse();" in content:
        return

    marker = "void board_init()\n{\n\tboard_power_init();"
    if marker in content:
        replacement = marker + "\n#ifdef CONFIG_BT_ENABLE\n    void cfg_file_parse(void);\n    cfg_file_parse();\n#endif"
        content = content.replace(marker, replacement, 1)
    else:
        board_init_marker = "void board_init(void)\n{"
        if board_init_marker not in content:
            raise BuildError(f"Jieli board file has no board_init function: {board_file}")
        replacement = board_init_marker + "\n#ifdef CONFIG_BT_ENABLE\n    void cfg_file_parse(void);\n    cfg_file_parse();\n#endif"
        content = content.replace(board_init_marker, replacement, 1)
    board_file.write_text(content, encoding="utf-8")


def resolve_sdk_root(
    environ: Optional[Mapping[str, str]] = None,
    module_root: Path = MODULE_ROOT,
) -> Path:
    env = os.environ if environ is None else environ
    chip = resolve_chip(env)
    configured = env.get("JIELI_SDK_ROOT", "").strip()
    candidates = []
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.append((module_root / chip.sdk_dir).resolve())
    for legacy in chip.legacy_sdk_dirs:
        candidates.append((module_root / legacy).resolve())
    if chip.name == "wl82":
        candidates.append((module_root / "../../../AC79_AIoT_SDK").resolve())

    for candidate in candidates:
        if (candidate / chip.probe).is_file():
            return candidate

    searched = ", ".join(str(path) for path in candidates)
    sdk_repo = "fw-AC79_AIoT_SDK" if chip.name == "wl82" else "fw-AC792_SDK"
    raise BuildError(
        f"Jieli SDK not found for chip '{chip.name}'; set JIELI_SDK_ROOT to a "
        f"{sdk_repo} checkout. Searched: {searched}"
    )


def resolve_tool_dir(
    sdk_root: Path,
    environ: Optional[Mapping[str, str]] = None,
    module_root: Path = MODULE_ROOT,
) -> Path:
    env = os.environ if environ is None else environ
    configured = env.get("JIELI_TOOL_DIR", "").strip()
    candidates = []
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.extend(
        [
            (sdk_root.parent / "ipc_ac7916a/toolchain/jieli-linux-toolchains/pi32v2/bin").resolve(),
            Path("C:/JL/pi32/bin"),
            Path("/opt/jieli/pi32v2/bin"),
            (module_root.parents[2] / "ipc_ac7916a/toolchain/jieli-linux-toolchains/pi32v2/bin").resolve(),
            (module_root.parents[3] / "ipc_ac7916a/toolchain/jieli-linux-toolchains/pi32v2/bin").resolve(),
        ]
    )

    required = ("clang", "lto-wrapper", "lto-ar", "objdump", "objsizedump")
    for candidate in candidates:
        if candidate.is_dir() and all(resolve_tool_path(candidate, name).is_file() for name in required):
            return candidate

    searched = ", ".join(str(path) for path in candidates)
    raise BuildError(
        "Jieli pi32v2 toolchain not found; set JIELI_TOOL_DIR to pi32v2/bin. "
        f"Required tools: {', '.join(required)}. Searched: {searched}"
    )


def build_make_command(sdk_root: Path, tool_dir: Path, jobs: int = 1) -> list[str]:
    if jobs < 1:
        raise ValueError("jobs must be at least 1")
    chip = resolve_chip()
    board_dir = sdk_root / chip.sdk_source_relative / chip.board_build_relative
    elf_target = "../../../../../" + chip.elf_relative.as_posix()
    command = [
        "make",
        "-C",
        str(board_dir),
        f"TOOL_DIR={tool_dir}",
        f"-j{jobs}",
        "pre_build",
        elf_target,
    ]
    if os.name == "nt":
        command.insert(4, "LINK_AT=0")
    return command


def create_staging_tree(
    sdk_root: Path,
    staging_root: Path,
    tuyaopen_root: Path,
    header_dir: Optional[Path] = None,
    full_stack: bool = False,
    tuya_lib_dir: Optional[Path] = None,
    uart_log_port: int = 0,
    uart_log_baudrate: int = 115200,
) -> Path:
    """Create a small overlay tree without modifying the vendor checkout."""
    if staging_root.exists():
        shutil.rmtree(staging_root)

    build_root = staging_root / "build"
    chip = resolve_chip()
    vendor_root = sdk_root / chip.sdk_source_relative
    source_overlay_root = build_root / chip.sdk_source_relative
    source_overlay_root.mkdir(parents=True)
    for name in ("cpu", "include_lib", "lib", "tools"):
        link_directory(source_overlay_root / name, vendor_root / name)

    apps_root = source_overlay_root / "apps"
    apps_root.mkdir()
    link_directory(apps_root / "common", vendor_root / "apps/common")
    shutil.copytree(
        vendor_root / "apps/demo/demo_hello",
        apps_root / "demo/demo_hello",
    )
    board_file = source_overlay_root / chip.board_build_relative / "board.c"
    app_config_file = source_overlay_root / "apps/demo/demo_hello/include/app_config.h"
    if chip.name == "wl82":
        configure_ac79_log_uart(board_file, uart_log_port, uart_log_baudrate)
        configure_service_uart(board_file)
    else:
        configure_ac792_log_uart(
            source_overlay_root / "apps/demo/demo_hello/board/wl83/board_demo.h",
            uart_log_port,
            uart_log_baudrate,
        )
    if full_stack:
        reference_board_file = (
            vendor_root / "apps/demo/demo_wifi_ext/board/wl83/board.c"
            if chip.name == "wl83"
            else None
        )
        configure_full_stack_board(board_file, reference_board_file)
        configure_full_stack_app_config(app_config_file)
    if chip.name == "wl83":
        configure_ac792_devkit_memory(
            source_overlay_root / "apps/demo/demo_hello/board/wl83/chip_cfg.h",
            source_overlay_root / "apps/demo/demo_hello/board/wl83/board_demo.h",
        )
    platform_root = tuyaopen_root / "platform/JIELI"
    entry_source = "tuyaos_switch_app_main.c" if full_stack else "tuyaos_app_main.c"
    shutil.copy2(platform_root / entry_source, source_overlay_root / "tuyaos_app_main.c")
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
    content = content.replace(vendor_main, "../../../../../tuyaos_app_main.c")
    extra_sources = "c_SRC_FILES += \\\n"
    extra_sources += (
        "    ../../../../../tuyaos_adapter/src/system/tkl_output.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tkl_system.c \\\n"
        "    ../../../../../tuyaos_adapter/src/driver/tkl_uart.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tkl_thread.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tkl_mutex.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tkl_semaphore.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tkl_queue.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tkl_sleep.c \\\n"
        "    ../../../../../tuyaos_adapter/src/driver/tkl_flash.c \\\n"
        "    ../../../../../tuyaos_adapter/src/driver/tkl_ota.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tkl_assert.c \\\n"
        "    ../../../../../tuyaos_adapter/src/driver/tkl_bluetooth.c \\\n"
        "    ../../../../../tuyaos_adapter/src/driver/tkl_network.c \\\n"
        "    ../../../../../tuyaos_adapter/src/driver/tkl_wifi.c \\\n"
        "    ../../../../../tuyaos_adapter/src/system/tuyaopen_license.c \\\n"
        "    ../../../../../tuya_utilities/src/tuya_hashmap.c \\\n"
        "    ../../../../../tuya_utilities/src/tuya_list.c \\\n"
        "    ../../../../../tuya_utilities/src/tuya_mem_heap.c \\\n"
        "    ../../../../../tuya_utilities/src/tuya_queue.c \\\n"
        "    ../../../../../tuya_utilities/src/tuya_ringbuf.c \\\n"
        "    ../../../../../tuya_utilities/src/tuya_smartpointer.c \\\n"
        "    ../../../../../tuya_utilities/src/tuya_tools.c\n"
    )
    if full_stack:
        extra_sources = extra_sources.rstrip("\n") + " \\\n"
        extra_sources += (
            "    ../../../../../apps/common/config/bt_profile_config.c \\\n"
            "    ../../../../../apps/common/config/log_config/app_config.c \\\n"
            "    ../../../../../apps/common/config/log_config/lib_btctrler_config.c \\\n"
            "    ../../../../../apps/common/config/log_config/lib_btstack_config.c \\\n"
            "    ../../../../../apps/common/net/assign_macaddr.c \\\n"
            "    ../../../../../apps/common/net/config_network.c \\\n"
            "    ../../../../../apps/common/net/platform_cfg.c \\\n"
            "    ../../../../../apps/common/net/wifi_conf.c\n"
        )
    marker = "c_OBJS    :="
    if marker not in content:
        raise BuildError(f"AC79 demo Makefile has no object list marker: {makefile}")
    content = content.replace(marker, extra_sources + "\n" + marker, 1)
    if full_stack and chip.name == "wl82":
        # The AC791 Wi-Fi/BLE SDK libraries use the SDRAM linker layout, so
        # remove the minimal hello template's no-SDRAM define in staging.
        content = content.replace("\t-DCONFIG_NO_SDRAM_ENABLE \\\n", "", 1)
    content += "\n"
    content += "INCLUDES += \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/system \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/uart \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/init/include \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/utilities/include \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/flash \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/network \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/wifi \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/bluetooth \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/timer \\\n"
    content += f"    -I{tuyaopen_root}/tools/porting/adapter/security \\\n"
    content += "    -I../../../../../apps/common/include \\\n"
    content += "    -I../../../../../apps/common/config/include \\\n"
    content += "    -I../../../../../include_lib/btstack \\\n"
    content += "    -I../../../../../include_lib/btstack/le \\\n"
    content += "    -I../../../../../include_lib/btctrler \\\n"
    content += f"    -I../../../../../include_lib/btctrler/port/{chip.cpu} \\\n"
    content += f"    -I{tuyaopen_root}/src/common/include\n"
    content += "INCLUDES += \\\n"
    content += "    -I../../../../../tuyaos_adapter/include \\\n"
    content += "    -I../../../../../tuyaos_adapter/include/system \\\n"
    content += "    -I../../../../../tuyaos_adapter/include/driver \\\n"
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
    content += "CFLAGS += -include stdbool.h -DBOOL_DEFINE_CONFLICT\n"
    if full_stack:
        content += "DEFINES += -DCONFIG_NET_ENABLE=1 -DCONFIG_BT_ENABLE=1 -DCONFIG_TWS_ENABLE -DCONFIG_BTCTRLER_TASK_DEL_ENABLE -DCONFIG_LMP_CONN_SUSPEND_ENABLE -DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE\n"
        if tuya_lib_dir is None:
            raise BuildError("TuyaOpen library directory is required for the full Jieli image")
        content += "LFLAGS += \\\n"
        # The vendor LTO used-symbol list does not reference wlc_main when
        # Wi-Fi is entered through the Tuya TKL layer.  Force-retain the
        # vendor entry point so cpu.a[wlc.c.o] is extracted by the linker.
        content += "    -u wlc_main \\\n"
        content += f"    --start-group {tuya_lib_dir}/libtuyaapp.a {tuya_lib_dir}/libtuyaos.a \\\n"
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
        for library in libraries:
            content += f"    {liba}/{library} \\\n"
        content += "    --end-group\n"
    if header_dir is not None:
        content += f"INCLUDES += -I{header_dir}\n"
    makefile.write_text(content, encoding="utf-8")
    return build_root


def find_qio_artifact(tools_dir: Path) -> Path:
    # Without the vendor host-client, app.bin is the artifact generated by the
    # current build. Prefer it over stale packages kept in the SDK checkout.
    for name in ("app.bin", "jl_isd.ufw", "jl_isd.fw"):
        candidate = tools_dir / name
        if candidate.is_file() and candidate.stat().st_size > 0:
            return candidate
    raise BuildError(
        f"Jieli postbuild produced no flash package in {tools_dir}; "
        "expected jl_isd.ufw, jl_isd.fw, or app.bin"
    )


def tuya_qio_name(params: Mapping[str, str]) -> str:
    project = params.get("CONFIG_PROJECT_NAME", "jieli_uart_hello")
    version = params.get("CONFIG_PROJECT_VERSION", "1.0.0")
    return f"{project}_QIO_{version}.bin"
