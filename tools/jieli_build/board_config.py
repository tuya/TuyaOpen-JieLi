"""Apply board-specific configuration to staging copies of SDK files."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Optional

from .errors import BuildError
from .audio_profile import AudioProfile

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
    """Route Tuya logical UART0/TAL CLI to AC79 hardware UART0 on PA5/PA6."""
    if not board_file.is_file():
        return

    content = board_file.read_text(encoding="utf-8")
    begin = "UART0_PLATFORM_DATA_BEGIN(uart0_data)"
    end = "UART0_PLATFORM_DATA_END();"
    start = content.find(begin)
    finish = content.find(end, start) if start >= 0 else -1
    if start < 0:
        uart_block = (
            "UART0_PLATFORM_DATA_BEGIN(uart0_data)\n"
            "    .baudrate = 115200,\n"
            "    .port = PORTA_5_6,\n"
            "    .tx_pin = IO_PORTA_05,\n"
            "    .rx_pin = IO_PORTA_06,\n"
            "    .max_continue_recv_cnt = 1024,\n"
            "    .idle_sys_clk_cnt = 500000,\n"
            "    .clk_src = PLL_48M,\n"
            "    .flags = 0,\n"
            "UART0_PLATFORM_DATA_END();\n\n"
        )
        insertion = content.find("UART1_PLATFORM_DATA_BEGIN(uart1_data)")
        if insertion < 0:
            insertion = content.find("UART2_PLATFORM_DATA_BEGIN(uart2_data)")
        if insertion < 0:
            raise BuildError(f"Jieli board file has no UART insertion point: {board_file}")
        content = content[:insertion] + uart_block + content[insertion:]
    else:
        if finish < 0:
            raise BuildError(f"Jieli board file has an incomplete UART0 service block: {board_file}")
        finish += len(end)
        uart_block = content[start:finish]
        replacements = (
            (r"\.baudrate\s*=\s*\d+\s*,", ".baudrate = 115200,"),
            (r"\.port\s*=\s*[^,]+,", ".port = PORTA_5_6,"),
            (r"\.tx_pin\s*=\s*[^,]+,", ".tx_pin = IO_PORTA_05,"),
            (r"\.rx_pin\s*=\s*[^,]+,", ".rx_pin = IO_PORTA_06,"),
            (r"\.flags\s*=\s*[^,]+,", ".flags = 0,"),
        )
        for pattern, replacement in replacements:
            uart_block, count = re.subn(pattern, replacement, uart_block, count=1)
            if count != 1:
                raise BuildError(f"AC79 UART0 service setting was not found in {board_file}")
        content = content[:start] + uart_block + content[finish:]

    uart0_registered = re.search(
        r'\{\s*"uart0"\s*,\s*&uart_dev_ops\s*,\s*\(void\s*\*\)\s*&uart0_data\s*\}',
        content,
    )
    if not uart0_registered:
        device_table = "REGISTER_DEVICES(device_table) = {"
        if device_table not in content:
            raise BuildError(f"Jieli board file has no device table: {board_file}")
        content = content.replace(
            device_table,
            device_table + '\n    {"uart0", &uart_dev_ops, (void *)&uart0_data },',
            1,
        )

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

def configure_ac79_devkit_memory(app_config_file: Path) -> None:
    """Use the AC79 DevKit reference memory map in the staging copy."""
    _rewrite_memory_macros(app_config_file, {"__FLASH_SIZE__": 8, "__SDRAM_SIZE__": 8})


def _rewrite_memory_macros(config_file: Path, sizes: dict[str, int]) -> None:
    """Set named memory macros exactly once in an SDK staging config."""
    if not config_file.is_file():
        raise BuildError(f"Jieli config not found: {config_file}")

    content = config_file.read_text(encoding="utf-8")
    for macro, size_mib in sizes.items():
        pattern = re.compile(rf"(?m)^([ \t]*#define[ \t]+{macro}[ \t]+)[^\r\n]*$")
        content, count = pattern.subn(
            lambda match: f"{match.group(1)}({size_mib} * 1024 * 1024)",
            content,
        )
        if count != 1:
            raise BuildError(
                f"Expected exactly one {macro} definition in {config_file}, found {count}"
            )

    config_file.write_text(content, encoding="utf-8")

def configure_ac792_devkit_memory(chip_config_file: Path, board_config_file: Path) -> None:
    """Use the AC792N development-board reference SKU (AC7926A) memory map."""
    _rewrite_memory_macros(chip_config_file, {"__FLASH_SIZE__": 8, "__SDRAM_SIZE__": 16})

    if not board_config_file.is_file():
        raise BuildError(f"Jieli board config not found: {board_config_file}")
    content = board_config_file.read_text(encoding="utf-8")
    content, count = re.subn(
        r"(?m)^[ \t]*#define[ \t]+CONFIG_NO_SDRAM_ENABLE[ \t]*$",
        "/* External DDR is enabled for AC7926A DevKit. */",
        content,
    )
    if count != 1:
        raise BuildError(
            f"Expected exactly one CONFIG_NO_SDRAM_ENABLE definition in "
            f"{board_config_file}, found {count}"
        )
    board_config_file.write_text(content, encoding="utf-8")

def configure_audio_board(board_file: Path, audio_config_header: Path, board: "AudioProfile") -> None:
    """Patch the staged board file with the profile's audio device and init.

    Registers the audio device the vendor runtime opens, injects the board's
    DAC/ADC platform structs, and sequences the PA-mute/VCM bring-up. Also
    stages the TuyaOpen board's audio_config.h next to board.c as
    tuya_board_audio_config.h. Idempotent: the
    TUYAOPEN_JIELI_AUDIO_BOARD_CONFIG sentinel short-circuits to include and
    header repair only, so staging trees survive between builds.
    """
    if not board_file.is_file():
        raise BuildError(f"Jieli board file is missing for audio staging: {board_file}")
    if not audio_config_header.is_file():
        raise BuildError(f"Jieli audio board profile is missing: {audio_config_header}")

    content = board_file.read_text(encoding="utf-8")
    board_audio_include = '#include "tuya_board_audio_config.h"'
    staged_header = board_file.parent / "tuya_board_audio_config.h"
    sentinel = "/* TUYAOPEN_JIELI_AUDIO_BOARD_CONFIG */"
    if sentinel in content:
        if board_audio_include not in content:
            sdk_audio_include = '#include "audio_config.h"'
            if sdk_audio_include not in content:
                raise BuildError(f"Jieli audio board file has no board audio config include: {board_file}")
            content = content.replace(sdk_audio_include, board_audio_include, 1)
        if '#include "server/audio_dev.h"' not in content:
            content = content.replace(
                board_audio_include,
                board_audio_include + '\n#include "server/audio_dev.h"',
                1,
            )
        board_file.write_text(content, encoding="utf-8")
        staged_header.write_bytes(audio_config_header.read_bytes())
        return

    include_marker = '#include "asm/includes.h"'
    if include_marker not in content:
        raise BuildError(f"Jieli board file has no audio include insertion point: {board_file}")
    content = content.replace(
        include_marker,
        include_marker + '\n' + board_audio_include + '\n#include "server/audio_dev.h"',
        1,
    )

    table_marker = "REGISTER_DEVICES(device_table) = {"
    table_start = content.find(table_marker)
    if table_start < 0:
        raise BuildError(f"Jieli board file has no device table: {board_file}")
    if re.search(r'\{\s*"audio"\s*,\s*&audio_dev_ops', content):
        raise BuildError(f"Jieli board file already registers an audio device: {board_file}")

    early_marker = re.search(r"void\s+board_early_init\s*\([^)]*\)\s*\{", content)
    if early_marker is None:
        raise BuildError(f"Jieli board file has no board_early_init function: {board_file}")
    devices_marker = content.find("devices_init();", early_marker.end())
    if devices_marker < 0:
        raise BuildError(f"Jieli board early init has no devices_init call: {board_file}")
    devices_finish = devices_marker + len("devices_init();")
    content = (
        content[:devices_marker]
        + board.board_early_init_pre
        + content[devices_marker:devices_finish]
        + board.board_early_init_post
        + content[devices_finish:]
    )
    if board.board_init:
        init_marker = re.search(r"void\s+board_init\s*\([^)]*\)\s*\{", content)
        if init_marker is None:
            raise BuildError(f"Jieli board file has no board_init function: {board_file}")
        content = content[:init_marker.end()] + board.board_init + content[init_marker.end():]

    content = content[:table_start] + board.board_declarations + "\n" + content[table_start:]
    table_start = content.find(table_marker)
    table_open = content.find("{", table_start + len(table_marker) - 1)
    if table_open < 0:
        raise BuildError(f"Jieli device table has no opening brace: {board_file}")
    if board.registers_audio_device:
        content = content[: table_open + 1] + (
            '\n    {"audio", &audio_dev_ops, (void *)&tuya_audio_data },'
        ) + content[table_open + 1 :]
    board_file.write_text(content, encoding="utf-8")
    staged_header.write_bytes(audio_config_header.read_bytes())


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
