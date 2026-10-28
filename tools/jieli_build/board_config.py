"""Apply board-specific configuration to staging copies of SDK files."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path
from typing import Optional

from .errors import BuildError
from .audio_profile import AudioProfile
from .board_facts import BoardFacts, LogUart, ServiceUart

# The UART platform data names both the pins and the port group they belong to
# (``.port = PORTA_5_6`` next to ``.tx_pin = IO_PORTA_05``). The board states the
# pins; the group is derived from them, because the schema has no field for it
# and a pin pair is what actually selects the group.
_PIN_PATTERN = re.compile(r"^IO_PORT([A-Z])_(\d{1,2})$")

# The port-group macros the wl82 SDK's asm/uart.h defines, and the only values a
# UART platform-data block may name. The header lists exactly these, so a pair
# that is not among them has no macro to expand to: the generated block would
# name an identifier the vendor headers never define, and the compile would fail
# inside SDK code rather than here.
#
# tests/test_board_config.py cross-checks this against the header when the SDK
# checkout is present, so a vendor update that adds or removes a group fails
# there rather than at link time.
UART_PORT_GROUPS = (
    "PORTA_3_4", "PORTA_5_6",
    "PORTB_1_2", "PORTB_3_4", "PORTB_6_7",
    "PORTC_3_4", "PORTC_9_10",
    "PORTE_0_1",
    "PORTH_0_1", "PORTH_2_3", "PORTH_6_7",
)


def _uart_port_group(tx_pin: str, rx_pin: str) -> str:
    """Return the vendor port-group macro for a same-port TX/RX pin pair.

    The macro has to be one the SDK actually defines; see UART_PORT_GROUPS. A
    pair on the same port can still name a group the SDK has no macro for, and
    synthesising the name from the pins would hide that until the vendor header
    rejects it.
    """
    tx = _PIN_PATTERN.match(tx_pin)
    rx = _PIN_PATTERN.match(rx_pin)
    if tx is None or rx is None or tx.group(1) != rx.group(1):
        raise BuildError(
            f"service UART pins {tx_pin!r}/{rx_pin!r} are not a same-port "
            f"IO_PORT<X>_<n> pair, so no port group can be derived for them"
        )
    port_group = f"PORT{tx.group(1)}_{int(tx.group(2))}_{int(rx.group(2))}"
    if port_group not in UART_PORT_GROUPS:
        raise BuildError(
            f"service UART pins {tx_pin!r}/{rx_pin!r} name the port group "
            f"{port_group}, which this SDK does not define. It defines: "
            f"{', '.join(UART_PORT_GROUPS)}"
        )
    return port_group


def configure_log_uart_block(
    board_file: Path, uart_port: int, baudrate: int, facts: BoardFacts
) -> None:
    """Program the board's documented log UART into the staged board file.

    This is the wl82 layout, where the log UART is a UART platform-data block in
    the demo's board.c. The port is asserted against the board's own declaration
    so a build that selected a different controller fails rather than logging
    into nothing.
    """
    log_uart: LogUart = facts.log_uart
    if uart_port != log_uart.port:
        raise BuildError(
            f"{facts.board} logs on SDK UART{log_uart.port}, but the build "
            f"selected UART{uart_port}"
        )
    if log_uart.tx_pin is None:
        raise BuildError(
            f"{facts.board} logs on UART{log_uart.port} but declares no tx_pin, "
            f"which this chip's board file has to rewrite"
        )
    if not board_file.is_file():
        return

    name = f"uart{log_uart.port}"
    content = board_file.read_text(encoding="utf-8")
    marker = f"UART{log_uart.port}_PLATFORM_DATA_BEGIN({name}_data)"
    end = f"UART{log_uart.port}_PLATFORM_DATA_END();"
    start = content.find(marker)
    if start < 0:
        # The wl82 demo board carries only the vendor's UART2 block, so a new
        # log UART is inserted ahead of it.
        uart2_marker = "UART2_PLATFORM_DATA_BEGIN(uart2_data)"
        insertion = content.find(uart2_marker)
        if insertion < 0:
            raise BuildError(f"Jieli board file has no UART2 insertion point: {board_file}")
        uart_block = (
            f"UART{log_uart.port}_PLATFORM_DATA_BEGIN({name}_data)\n"
            f"    .baudrate = {baudrate},\n"
            "    .port = PORT_REMAP,\n"
            "    .output_channel = OUTPUT_CHANNEL0,\n"
            f"    .tx_pin = {log_uart.tx_pin},\n"
            "    .rx_pin = -1,\n"
            "    .max_continue_recv_cnt = 1024,\n"
            "    .idle_sys_clk_cnt = 500000,\n"
            "    .clk_src = PLL_48M,\n"
            "    .flags = UART_DEBUG,\n"
            f"UART{log_uart.port}_PLATFORM_DATA_END();\n\n"
        )
        content = content[:insertion] + uart_block + content[insertion:]
    else:
        finish = content.find(end, start)
        if finish < 0:
            raise BuildError(
                f"Jieli board file has an incomplete UART{log_uart.port} block: {board_file}"
            )
        finish += len(end)
        uart_block = content[start:finish]
        replacements = (
            (r"\.baudrate\s*=\s*\d+\s*,", f".baudrate = {baudrate},"),
            (r"\.port\s*=\s*[^,]+,", ".port = PORT_REMAP,"),
            (r"\.output_channel\s*=\s*[^,]+,", ".output_channel = OUTPUT_CHANNEL0,"),
            (r"\.tx_pin\s*=\s*[^,]+,", f".tx_pin = {log_uart.tx_pin},"),
            (r"\.rx_pin\s*=\s*[^,]+,", ".rx_pin = -1,"),
        )
        for pattern, replacement in replacements:
            uart_block, count = re.subn(pattern, replacement, uart_block, count=1)
            if count != 1:
                raise BuildError(
                    f"UART{log_uart.port} log setting was not found in {board_file}"
                )
        content = content[:start] + uart_block + content[finish:]

    # The vendor's debug_uart_init still points at the UART2 block it was
    # written for; the log UART it must initialize is the board's.
    init_marker = "uart_init(&uart2_data);"
    content, count = (
        content.replace(init_marker, f"uart_init(&{name}_data);", 1),
        content.count(init_marker),
    )
    if count != 1:
        raise BuildError(f"Jieli debug_uart_init does not initialize UART2 in {board_file}")
    board_file.write_text(content, encoding="utf-8")

def configure_log_uart_baudrate(
    board_header: Path, uart_port: int, baudrate: int, facts: BoardFacts
) -> None:
    """Set the board's documented log UART baud in the staged board profile.

    This is the wl83 layout, where the log UART is the vendor board profile's
    ``TCFG_UART<n>_BAUDRATE``. The port is asserted against the board's own
    declaration for the same reason as the wl82 block hook.
    """
    log_uart: LogUart = facts.log_uart
    if uart_port != log_uart.port:
        raise BuildError(
            f"{facts.board} logs on SDK UART{log_uart.port}, but the build "
            f"selected UART{uart_port}"
        )
    content = board_header.read_text(encoding="utf-8")
    content, count = re.subn(
        rf"(#define\s+TCFG_UART{log_uart.port}_BAUDRATE\s+)\d+",
        rf"\g<1>{baudrate}",
        content,
        count=1,
    )
    if count != 1:
        raise BuildError(
            f"UART{log_uart.port} baudrate was not found in {board_header}"
        )
    board_header.write_text(content, encoding="utf-8")

def configure_service_uart(board_file: Path, facts: BoardFacts) -> None:
    """Route the Tuya logical UART0/TAL CLI to the board's service UART.

    The UART index, the pins and the port group all come from the board's facts;
    only the platform-data shape is the vendor's.
    """
    service: Optional[ServiceUart] = facts.service_uart
    if service is None:
        raise BuildError(f"{facts.board} declares no service UART to configure")
    if not board_file.is_file():
        return

    name = f"uart{service.port}"
    port_group = _uart_port_group(service.tx_pin, service.rx_pin)
    content = board_file.read_text(encoding="utf-8")
    begin = f"UART{service.port}_PLATFORM_DATA_BEGIN({name}_data)"
    end = f"UART{service.port}_PLATFORM_DATA_END();"
    start = content.find(begin)
    finish = content.find(end, start) if start >= 0 else -1
    if start < 0:
        uart_block = (
            f"UART{service.port}_PLATFORM_DATA_BEGIN({name}_data)\n"
            "    .baudrate = 115200,\n"
            f"    .port = {port_group},\n"
            f"    .tx_pin = {service.tx_pin},\n"
            f"    .rx_pin = {service.rx_pin},\n"
            "    .max_continue_recv_cnt = 1024,\n"
            "    .idle_sys_clk_cnt = 500000,\n"
            "    .clk_src = PLL_48M,\n"
            "    .flags = 0,\n"
            f"UART{service.port}_PLATFORM_DATA_END();\n\n"
        )
        insertion = content.find("UART1_PLATFORM_DATA_BEGIN(uart1_data)")
        if insertion < 0:
            insertion = content.find("UART2_PLATFORM_DATA_BEGIN(uart2_data)")
        if insertion < 0:
            raise BuildError(f"Jieli board file has no UART insertion point: {board_file}")
        content = content[:insertion] + uart_block + content[insertion:]
    else:
        if finish < 0:
            raise BuildError(
                f"Jieli board file has an incomplete UART{service.port} "
                f"service block: {board_file}"
            )
        finish += len(end)
        uart_block = content[start:finish]
        replacements = (
            (r"\.baudrate\s*=\s*\d+\s*,", ".baudrate = 115200,"),
            (r"\.port\s*=\s*[^,]+,", f".port = {port_group},"),
            (r"\.tx_pin\s*=\s*[^,]+,", f".tx_pin = {service.tx_pin},"),
            (r"\.rx_pin\s*=\s*[^,]+,", f".rx_pin = {service.rx_pin},"),
            (r"\.flags\s*=\s*[^,]+,", ".flags = 0,"),
        )
        for pattern, replacement in replacements:
            uart_block, count = re.subn(pattern, replacement, uart_block, count=1)
            if count != 1:
                raise BuildError(
                    f"UART{service.port} service setting was not found in {board_file}"
                )
        content = content[:start] + uart_block + content[finish:]

    uart_registered = re.search(
        r'\{\s*"' + name + r'"\s*,\s*&uart_dev_ops\s*,\s*\(void\s*\*\)\s*&'
        + name + r'_data\s*\}',
        content,
    )
    if not uart_registered:
        device_table = "REGISTER_DEVICES(device_table) = {"
        if device_table not in content:
            raise BuildError(f"Jieli board file has no device table: {board_file}")
        content = content.replace(
            device_table,
            device_table + f'\n    {{"{name}", &uart_dev_ops, (void *)&{name}_data }},',
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

def configure_board_memory(
    config_file: Path, facts: BoardFacts, ddr_config_file: Optional[Path] = None
) -> None:
    """Apply the board's memory map to the SDK config that carries it.

    ``config_file`` is where this chip's SDK keeps ``__FLASH_SIZE__`` and
    ``__SDRAM_SIZE__``. ``ddr_config_file`` is the board profile that gates the
    external DDR through ``CONFIG_NO_SDRAM_ENABLE``; a chip whose DDR is not
    gated that way passes none, and a board that declares DDR then fails rather
    than silently staging a map the SDK will not use.
    """
    _rewrite_memory_macros(
        config_file,
        {
            "__FLASH_SIZE__": facts.memory.flash_mib,
            "__SDRAM_SIZE__": facts.memory.sdram_mib,
        },
    )

    if not facts.memory.external_ddr:
        return
    if ddr_config_file is None:
        raise BuildError(
            f"{facts.board} declares external DDR, but this chip's SDK enables "
            f"it through a board profile the platform did not stage"
        )
    if not ddr_config_file.is_file():
        raise BuildError(f"Jieli board config not found: {ddr_config_file}")
    content = ddr_config_file.read_text(encoding="utf-8")
    content, count = re.subn(
        r"(?m)^[ \t]*#define[ \t]+CONFIG_NO_SDRAM_ENABLE[ \t]*$",
        # A comment, not a fact the build reads: kept byte-for-byte because it
        # is already baked into every staged tree.
        "/* External DDR is enabled for AC7926A DevKit. */",
        content,
    )
    if count != 1:
        raise BuildError(
            f"Expected exactly one CONFIG_NO_SDRAM_ENABLE definition in "
            f"{ddr_config_file}, found {count}"
        )
    ddr_config_file.write_text(content, encoding="utf-8")

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


# The vendor SDK already implements the download-mode entry point: msd_upgrade.c
# defines go_mask_usb_updata(), and user_setup.c calls it from the private
# GET_STATUS wValue=0xfc/wIndex=0xfe handler. Both are gated only by
# TCFG_USB_SLAVE_ENABLE, so keeping the USB device stack serviced at runtime is
# what makes the host tool able to command download mode without the UPDATE key.
USB_DOWNLOAD_MARKER = "/* TuyaOpen Jieli runtime USB download-mode configuration. */"


def _insert_before_final_endif(
    config_file: Path, block: str, marker: str = USB_DOWNLOAD_MARKER
) -> None:
    """Insert a configuration block inside a header's include guard.

    Idempotent per marker: a header already carrying that block is left alone,
    so the independent injectors do not overwrite each other.
    """
    if not config_file.is_file():
        raise BuildError(f"Jieli config not found: {config_file}")
    content = config_file.read_text(encoding="utf-8")
    if marker in content:
        return
    insert_at = content.rfind("#endif")
    if insert_at < 0:
        raise BuildError(f"Jieli config has no final #endif: {config_file}")
    config_file.write_text(content[:insert_at] + block + content[insert_at:], encoding="utf-8")


# ---------------------------------------------------------------------------
# Low power
# ---------------------------------------------------------------------------

# The sleep sources AC791 may use, from asm/power_interface.h. The set is the
# vendor's own: apps/demo/demo_ble/include/app_config.h:24 and
# apps/demo/demo_edr/include/app_config.h:47 ship exactly these four behind
# CONFIG_LOW_POWER_ENABLE, and demo_ble registers no power callbacks, so the
# mask is known to stand on its own. The commented-out value beside the demo
# profile's zero names the same sources minus SYS_SLEEP_BY_IDLE.
WL82_LOWPOWER_SEL = "SYS_SLEEP_EN|SYS_SLEEP_BY_IDLE|RF_SLEEP_EN|RF_FORCE_SYS_SLEEP_EN"

# A sleep mask on its own does not put the station into power save: the shared
# apps/common/net/wifi_conf.c picks the STA's 802.11 power mode from its own
# macros, and with neither set it writes "PSMode=CAM" into the WLAN config it
# hands the driver (wifi_conf.c:519-525 and :561-567). CAM is continuous awake.
#
# CONFIG_LOW_POWER_ENABLE is the switch the vendor uses for exactly this
# combination. The AC791 power measurement that produced the reference figures
# builds P1_LEGACY/P1_FAST as "mask 30 + CONFIG_LOW_POWER_ENABLE" (see
# ac791_wifi_power_test, docs/AC791_烧录与iperf测试.md and its demo_wifi
# app_config.h), and both modes are used there: CONFIG_LOW_POWER_ENABLE ->
# WL_STA_SLEEP 2 -> Fast_PSP, CONFIG_WIFI_IDLE_RESUME_BASEBAND_ENABLE ->
# WL_STA_SLEEP 1 -> Legacy_PSP.
#
# Fast PSP is the mode this platform selects, because it is the low-power
# probe's own P1_FAST arm and the more aggressive of the two. The macro's
# other consumers in the staged tree are inert here:
#   apps/common/net/config_network.c:97,170 holds the RF awake across
#     provisioning - the correct behaviour, since the driver would otherwise
#     doze during netcfg;
#   apps/common/audio_music/audio_config.c:30 only changes a mix buffer size
#     inside CONFIG_AUDIO_MIX_ENABLE, which nothing in this build defines.
WL82_STA_PS_MACRO = "CONFIG_LOW_POWER_ENABLE"
# The STA power mode this configuration must compile to. wifi_conf.c derives it
# as CONFIG_LOW_POWER_ENABLE -> 2, CONFIG_WIFI_IDLE_RESUME_BASEBAND_ENABLE -> 1,
# else 0, and prints it as a PSMode= line.
WL82_STA_PS_VALUE = 2  # Fast_PSP

# AC792's equivalent, from lowpower_config_t in asm/power/power_api.h. Only the
# plain sleep level is used: DEEP_SLEEP_EN powers the chip down and reboots on
# wake, which is a different thing from anything TuyaOpen's power manager asks
# for on this platform.
#
# It is written as the literal 1 rather than the enum name SLEEP_EN, and that
# is load-bearing. The SDK compares this macro inside #if
# (apps/common/net/wifi_conf.c:55 and :570, audio/log_config/lib_media_config.c
# :57), where an identifier that is not a macro expands to 0 - so with the enum
# name both sides of "TCFG_LOWPOWER_FUNCTION != LOWPOWER_CLOSE" read as 0 and
# the low-power branches stay compiled out. A literal is the only spelling that
# reaches both the struct initialiser and the preprocessor. The same reason
# applies in reverse to the stock LOWPOWER_CLOSE, which is why
# config_wifi_lowpower_enable is 0 in every shipped AC792 board.
#
# That comparison is also what selects the STA power mode here
# (wifi_conf.c:565-575): the same evaluation yields WL_STA_SLEEP 1, i.e.
# PSMode=Legacy_PSP. Without the literal the station would stay in CAM there
# too, and the wifi_psmode_* listen-policy calls in the TKL would switch the
# policy of a radio that never entered power save.
WL83_LOWPOWER_FUNCTION = "1 /* SLEEP_EN */"
# wl83 derives the STA mode from the same macro, through an integer only, so
# the expected value is asserted rather than named a second time.
WL83_STA_PS_VALUE = 1  # Legacy_PSP


def _redefine_macro(config_file: Path, name: str, value: str) -> None:
    """Rewrite an existing ``#define <name> ...`` in a staged header.

    The low-power macros are already defined by the vendor profile, so this
    replaces the definition instead of appending a second one: a later #define
    would warn on redefinition and leave the earlier value in force for every
    line above it. A missing macro is a hard error - it means the vendor
    profile moved, and a silent no-op here would ship a build that claims to
    have low power and does not.
    """
    if not config_file.is_file():
        raise BuildError(f"Jieli config not found: {config_file}")
    content = config_file.read_text(encoding="utf-8")
    pattern = re.compile(rf"^#define\s+{re.escape(name)}\s+.*$", re.MULTILINE)
    updated, count = pattern.subn(f"#define {name} {value}", content, count=1)
    if count == 0:
        raise BuildError(f"Jieli config has no #define {name}: {config_file}")
    config_file.write_text(updated, encoding="utf-8")


def _define_macro(config_file: Path, name: str) -> None:
    """Append ``#define <name>`` before the final ``#endif`` of a staged header.

    Used for switches the vendor profile does not define at all. Idempotent:
    a header that already carries the define is left alone.
    """
    _insert_before_final_endif(config_file, f"#define {name}\n", marker=f"#define {name}")


# The vendor file that decides the station's power mode. Both SDKs put the same
# three-way choice in their copy of apps/common/net/wifi_conf.c, which this
# platform stages and compiles as part of the Wi-Fi feature.
_WIFI_CONF_RELATIVE = "apps/common/net/wifi_conf.c"


def _ps_mode_decision_block(wifi_conf: Path) -> str:
    """Cut the vendor's PSMode selection out of wifi_conf.c.

    The decision is a group of #if/#elif/#else branches that end in the value
    of ``PSMode=``. Reproducing the branches here would be a second copy of
    vendor logic that diverges the moment the SDK changes, so the block is
    located by its surrounding markers and returned verbatim. A missing marker
    is an error: it means the vendor moved the decision and the check must be
    re-pointed rather than silently passing on nothing.
    """
    if not wifi_conf.is_file():
        raise BuildError(f"Jieli Wi-Fi config not found: {wifi_conf}")
    text = wifi_conf.read_text(encoding="utf-8", errors="replace")

    marker = text.find("PSMode=")
    if marker < 0:
        raise BuildError(f"{wifi_conf} has no PSMode selection to check")

    # The vendor splits the decision over two sibling blocks: one defines
    # WL_STA_SLEEP from the mode switches, the next turns WL_STA_SLEEP into the
    # PSMode= string. Both are needed - the second alone would report CAM for
    # every configuration because WL_STA_SLEEP would be undefined.
    markers = ("#define WL_STA_SLEEP", "PSMode=")
    spans = []
    for marker in markers:
        offset = text.find(marker)
        if offset < 0:
            raise BuildError(f"{wifi_conf} has no {marker!r} selection to check")
        span = _enclosing_directive_span(text, offset)
        if span is None:
            raise BuildError(f"{wifi_conf}: {marker!r} is not guarded by an #if")
        spans.append(span)

    start = min(begin for begin, _ in spans)
    end = max(finish for _, finish in spans)
    return text[start:end]


def _enclosing_directive_span(text: str, offset: int) -> Optional[tuple[int, int]]:
    """Return the outermost #if..#endif span that contains ``offset``."""
    chosen = None
    start = text.find("#if")
    while start >= 0:
        end = _directive_block_end(text, start)
        if start < offset < end:
            chosen = (start, end)
        start = text.find("#if", start + 1)
    return chosen


def _directive_block_end(text: str, start: int) -> int:
    """Return the offset just past the #endif that closes the #if at ``start``."""
    depth = 0
    end = start
    for line in text[start:].splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith("#if"):
            depth += 1
        elif stripped.startswith("#endif"):
            depth -= 1
            if depth == 0:
                return end + len(line)
        end += len(line)
    raise BuildError("the PSMode= selection in wifi_conf.c is not closed")


def _check_ps_mode(
    config_file: Path,
    wifi_conf: Path,
    expected: int,
    compiler: Optional[str] = None,
    sdk_root: Optional[Path] = None,
) -> None:
    """Assert the STA power mode the staged profile actually compiles to.

    The expected value is checked on preprocessor output, not on the text this
    code just wrote: the staged profile is scanned first, then the vendor's own
    PSMode branches are run against it. A macro that cannot reach the #if (the
    AC792 enum case, where the comparison reads as 0 on both sides) fails here
    instead of silently shipping CAM, and a future SDK that adds a fourth
    branch is caught rather than assumed compatible.

    ``config_file`` is the vendor profile the macros are defined in - the wl82
    app_config.h or the wl83 board header - and it is deliberately scanned on
    its own: the decision depends only on the macros it defines, so the check
    stays valid even if the rest of the SDK include path changes.

    ``expected`` is 0 (CAM), 1 (Legacy_PSP) or 2 (Fast_PSP).
    """
    mode_names = {0: "CAM", 1: "Legacy_PSP", 2: "Fast_PSP"}
    if expected not in mode_names:
        raise BuildError(f"unsupported expected STA power mode {expected}")

    # The staged header is not self-contained, and the check has to reproduce
    # the chain the real build uses or it evaluates a different program:
    # board_demo.h is wrapped in #ifdef CONFIG_BOARD_DEMO, which board_config.h
    # defines *before* including it. Including board_config.h instead of the
    # header is what the real app_config.h does. Without it every macro in
    # board_demo.h stays unexpanded and the comparison reads as "CAM".
    include = config_file
    if config_file.name == "board_demo.h":
        chain = config_file.parent / "board_config.h"
        if chain.is_file():
            include = chain

    source = f'#include "{include.as_posix()}"\n'
    # The wl83 comparison names an enum constant, and an unknown identifier in
    # #if is 0; wifi_conf.c includes the header that declares it, so the check
    # includes the same one.
    if sdk_root is not None:
        power_api = sdk_root / "include_lib/driver/cpu/wl83/asm/power/power_api.h"
        if power_api.is_file():
            source += f'#include "{power_api.as_posix()}"\n'
    source += _ps_mode_decision_block(wifi_conf)
    compiler = compiler or os.environ.get("CC") or "clang"

    try:
        result = subprocess.run(
            [compiler, "-E", "-P", "-x", "c", "-"],
            input=source,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise BuildError(f"cannot run {compiler} to check the STA power mode: {error}")

    if result.returncode != 0:
        raise BuildError(
            f"cannot check the STA power mode of {config_file}: "
            f"{(result.stderr or result.stdout).strip()}"
        )

    # Exactly one literal survives the preprocessor; whichever it is, that is
    # the PSMode the driver will be configured with.
    found = re.findall(r"PSMode=([A-Za-z_]+)", result.stdout)
    if len(set(found)) != 1:
        raise BuildError(
            f"{config_file} does not compile the station to a single power mode: {found}"
        )

    got_name = found[0]
    want_name = mode_names[expected]
    if got_name != want_name:
        raise BuildError(
            f"{config_file} compiles the station to PSMode={got_name}, but this "
            f"build must use {want_name}; the switch that selects it is not "
            f"reaching the #if in {_WIFI_CONF_RELATIVE}"
        )


def configure_lowpower(
    chip_name: str,
    app_config_file: Path,
    board_header: Path,
    wifi_conf: Optional[Path] = None,
    compiler: Optional[str] = None,
    sdk_root: Optional[Path] = None,
) -> None:
    """Open the chip's sleep path in the staged board profile.

    Only called for builds that ask for TuyaOpen's power manager, so a build
    without it keeps the vendor profile exactly as it ships. That is what makes
    "the power component is off" mean "the firmware behaves as it did before".

    Which file carries the macro is a chip-layout fact: the AC791 demo profile
    keeps its TCFG_LOWPOWER_* block in app_config.h, the AC792 one in the board
    header reached through app_config.h -> board_config.h -> board_demo.h.

    Opening the sleep mask is only half of it. Both chips pick the station's
    802.11 power mode from a separate compile-time switch in the shared
    wifi_conf.c, and neither demo profile sets it, so the station would stay in
    CAM: awake continuously, with the sleep mask applying only to moments the
    radio is idle anyway. Both are written here, and the result is read back
    out of the preprocessor.
    """
    if chip_name not in ("wl82", "wl83"):
        raise BuildError(f"no Jieli low-power profile for chip '{chip_name}'")
    if wifi_conf is None:
        raise BuildError("the staged wifi_conf.c is required to check the STA power mode")

    if chip_name == "wl82":
        _redefine_macro(app_config_file, "TCFG_LOWPOWER_LOWPOWER_SEL", WL82_LOWPOWER_SEL)
        _define_macro(app_config_file, WL82_STA_PS_MACRO)
        _check_ps_mode(
            app_config_file, wifi_conf, WL82_STA_PS_VALUE, compiler, sdk_root
        )
    elif chip_name == "wl83":
        _redefine_macro(board_header, "TCFG_LOWPOWER_FUNCTION", WL83_LOWPOWER_FUNCTION)
        _check_ps_mode(board_header, wifi_conf, WL83_STA_PS_VALUE, compiler, sdk_root)
    else:
        raise BuildError(f"no Jieli low-power profile for chip '{chip_name}'")


def configure_usb_download_app_config(app_config_file: Path) -> None:
    """Enable the AC79 USB slave/MSD stack that serves download-mode requests.

    MASSSTORAGE_CLASS is what pulls in msd.c/msd_upgrade.c and enables the SCSI
    trigger; the control-endpoint GET_STATUS handshake works with any slave
    class. USB_PC_NO_APP_MODE=2 starts the stack straight from the OTG event
    handler, because the Tuya image has no vendor PC app state machine.
    """
    block = (
        f"{USB_DOWNLOAD_MARKER}\n"
        "#define CONFIG_USB_ENABLE                   1\n"
        "#ifdef CONFIG_USB_ENABLE\n"
        "#define TCFG_PC_ENABLE                      1\n"
        "#define USB_PC_NO_APP_MODE                  2\n"
        "#define USB_MALLOC_ENABLE                   1\n"
        "#if defined(__has_include)\n"
        "#if __has_include(\"board_com_api.h\")\n"
        '#include "board_com_api.h"\n'
        "#endif\n"
        "#endif\n"
        "#if defined(__has_include)\n"
        "#if __has_include(\"tuya_kconfig.h\")\n"
        '#include "tuya_kconfig.h"\n'
        "#endif\n"
        "#endif\n"
        "#if (defined(ENABLE_JIELI_USB_CDC_LOG) && ENABLE_JIELI_USB_CDC_LOG) || \\\n"
        "    (defined(CONFIG_ENABLE_JIELI_USB_CDC_LOG) && CONFIG_ENABLE_JIELI_USB_CDC_LOG)\n"
        "#define TCFG_USB_SLAVE_CDC_ENABLE           1\n"
        "#define USB_DEVICE_CLASS_CONFIG             (MASSSTORAGE_CLASS | CDC_CLASS)\n"
        "#else\n"
        "#define USB_DEVICE_CLASS_CONFIG             (MASSSTORAGE_CLASS)\n"
        "#endif\n"
        "#define TCFG_HOST_AUDIO_ENABLE              0\n"
        "#define TCFG_HOST_UVC_ENABLE                0\n"
        "#define TCFG_HID_HOST_ENABLE                0\n"
        "#define TCFG_UDISK_ENABLE                   0\n"
        '#include "usb_std_class_def.h"\n'
        '#include "usb_common_def.h"\n'
        "#endif\n\n"
    )
    _insert_before_final_endif(app_config_file, block)


def configure_usb_download_board_header(board_header: Path, facts: BoardFacts) -> None:
    """Enable the AC792 USB slave/MSD stack in the staged board profile.

    board_demo.h is reached from the USB sources through
    app_config.h -> board_config.h -> board_demo.h. The device macro is the
    board's own: a host port (``TCFG_HUSB_DEVICE``) or a full-speed device port
    (``TCFG_FUSB_DEVICE``).
    """
    block = (
        f"{USB_DOWNLOAD_MARKER}\n"
        "#define TCFG_FUSB_DEVICE                    BIT(0)\n"
        "#define TCFG_HUSB_DEVICE                    BIT(1)\n"
        f"#define TCFG_USB_DEVICE                     {facts.usb_device}\n"
        "#define TCFG_PC_ENABLE                      1\n"
        "#define USB_PC_NO_APP_MODE                  2\n"
        "#define USB_MALLOC_ENABLE                   1\n"
        "#if defined(__has_include)\n"
        "#if __has_include(\"board_com_api.h\")\n"
        '#include "board_com_api.h"\n'
        "#endif\n"
        "#endif\n"
        "#if defined(__has_include)\n"
        "#if __has_include(\"tuya_kconfig.h\")\n"
        '#include "tuya_kconfig.h"\n'
        "#endif\n"
        "#endif\n"
        "#if (defined(ENABLE_JIELI_USB_CDC_LOG) && ENABLE_JIELI_USB_CDC_LOG) || \\\n"
        "    (defined(CONFIG_ENABLE_JIELI_USB_CDC_LOG) && CONFIG_ENABLE_JIELI_USB_CDC_LOG)\n"
        "#define TCFG_USB_SLAVE_CDC_ENABLE           1\n"
        "#define USB_DEVICE_CLASS_CONFIG             (MASSSTORAGE_CLASS | CDC_CLASS)\n"
        "#else\n"
        "#define USB_DEVICE_CLASS_CONFIG             (MASSSTORAGE_CLASS)\n"
        "#endif\n"
        '#include "usb_std_class_def.h"\n'
        '#include "usb_common_def.h"\n\n'
    )
    _insert_before_final_endif(board_header, block)


def configure_usb_download_board(board_file: Path, usb_ports: str = "0x03") -> None:
    """Register the OTG device so the USB slave stack is started and serviced.

    The OTG driver raises DEVICE_EVENT_FROM_OTG on cable events; the Tuya adapter
    hook routes those to pc_device_event_handler(), which calls usb_start() and keeps
    the usb_msd0 worker alive for as long as the app runs.
    """
    if not board_file.is_file():
        return

    content = board_file.read_text(encoding="utf-8")
    if USB_DOWNLOAD_MARKER in content:
        return

    device_table = "REGISTER_DEVICES(device_table) = {"
    if device_table not in content:
        raise BuildError(f"Jieli board file has no device table: {board_file}")

    platform_data = (
        f"{USB_DOWNLOAD_MARKER}\n"
        '#include "otg.h"\n'
        "\n"
        "static const struct otg_dev_data otg_data = {\n"
        f"    .usb_dev_en = {usb_ports},\n"
        "    .slave_online_cnt = 10,\n"
        "    .slave_offline_cnt = 10,\n"
        "    .host_online_cnt = 10,\n"
        "    .host_offline_cnt = 10,\n"
        "    .detect_mode = OTG_SLAVE_MODE | OTG_CHARGE_MODE,\n"
        "    .detect_time_interval = 50,\n"
        "};\n\n"
    )
    content = content.replace(device_table, platform_data + device_table, 1)
    content = content.replace(
        device_table,
        device_table + '\n    { "otg", &usb_dev_ops, (void *)&otg_data},',
        1,
    )
    board_file.write_text(content, encoding="utf-8")


# The vendor LCD driver has no compile-time default of its own: the panel is
# selected by the board profile, and the whole block sits behind CONFIG_UI_ENABLE
# in the vendor's own board headers. TuyaOpen's staged app is demo_hello, whose
# profile carries no LCD macro at all, so the selection has to be injected.
LCD_MARKER = "/* TuyaOpen Jieli MIPI-DSI panel configuration. */"


def configure_lcd_app_config(app_config_file: Path) -> None:
    """Bring up the video subsystem the panel path depends on.

    CONFIG_UI_ENABLE is not a UI feature switch here: cpu/wl83/setup.c gates
    video_clock_early_init(TCFG_VIDEO_CLK) / video_eva_xbus_init() /
    jlgpu_clock_early_init(TCFG_GPU_CLK) on it, and the DSI/DPI/DMM/DMA2D blocks
    the panel resolves into need those clock domains powered. Without them the
    first video register access faults with "sfr_video_inv" and the board resets
    in a loop.

    This goes into app_config.h rather than the board header because setup.c is a
    CPU-level file that includes app_config.h directly; reaching it through
    board_config.h -> board_demo.h depends on include precedence that is not
    guaranteed for every translation unit.

    The clock values match the vendor demo_ui profile.
    """
    block = (
        f"{LCD_MARKER}\n"
        "#define CONFIG_UI_ENABLE                    1\n"
        "#define CONFIG_VIDEO_ENABLE                 1\n"
        "#define TCFG_VIDEO_CLK                      TCFG_SYS_CLK\n"
        "#define TCFG_GPU_CLK                        TCFG_SYS_CLK\n\n"
    )
    _insert_before_final_endif(app_config_file, block, LCD_MARKER)


def configure_lcd_board_header(board_header: Path, facts: BoardFacts) -> None:
    """Select the board's MIPI-DSI panel in the staged vendor board profile.

    The panel macro, device name and reset/backlight pins are the board's facts.
    The values are stated explicitly rather than left to
    lcd_board_cfg_template.h: that header deliberately carries no block for the
    480x800 ST7701S panel (its own copy is commented out with the note that the
    board file states it directly), so including it here would leave every
    TCFG_LCD_* macro undefined. They match the vendor's demo_ui profile for this
    panel.

    TCFG_LCD_INPUT_FORMAT must agree with the LVGL colour depth the TuyaOpen side
    builds with; the vendor header notes the same requirement.
    """
    display = facts.display
    if display is None:
        raise BuildError(f"{facts.board} declares no display panel to configure")
    block = (
        f"{LCD_MARKER}\n"
        "#define TCFG_LCD_ENABLE                     1\n"
        f"#define {display.panel_macro}    1\n"
        "#define TCFG_LCD_INPUT_FORMAT               LCD_IN_RGB565\n"
        f'#define TCFG_LCD_DEVICE_NAME                "{display.device_name}"\n'
        "#define TCFG_LCD_BL_VALUE                   1\n"
        f"#define TCFG_LCD_RESET_IO                   {display.reset_io}\n"
        f"#define TCFG_LCD_BL_IO                      {display.backlight_io}\n"
        "#define TCFG_LCD_RS_IO                      -1\n"
        "#define TCFG_LCD_CS_IO                      -1\n"
        "#define TCFG_LCD_TE_ENABLE                  0\n"
        "#define TCFG_LCD_TE_IO                      -1\n"
        "#define TCFG_LCD_SPI_INTERFACE              NULL\n"
        # The dma2d/fb_component layer the panel path resolves into needs the
        # vendor's storage-root and frame-buffer-count macros. demo_ui defines
        # them under `#if TCFG_LCD_ENABLE`; the same four are taken here, minus
        # its USE_LVGL_V9_UI_DEMO, which would pull the SDK's own LVGL in.
        #
        # FB_LCD_BUF_NUM is 0 in the vendor profile, not the 2 the fb_lcd.c
        # error message suggests: the message fires when the macro is absent,
        # and 0 is the value the vendor actually builds with.
        '#define CONFIG_STORAGE_PATH                 "storage/sdx"\n'
        '#define SDX_DEV                             "sdx"\n'
        '#define CONFIG_ROOT_PATH                    CONFIG_STORAGE_PATH"/C/"\n'
        "#define LV_DISP_UI_FB_NUM                   2\n"
        "#define FB_LCD_BUF_NUM                      0\n\n"
    )
    _insert_before_final_endif(board_header, block, LCD_MARKER)


def configure_lcd_board(board_file: Path) -> None:
    """Give the vendor LCD device a board config to match against.

    lcd_driver.c receives this through dev_open("lcd", &lcd_data); without it the
    panel lookup has nothing to compare the device name to. The shape mirrors the
    vendor's own demo_ui board (board_develop.c:320-358).
    """
    content = board_file.read_text(encoding="utf-8")
    if LCD_MARKER in content:
        return
    # The staged demo_hello board.c opens with app_config.h; board_config.h is
    # reached through it, not included directly.
    anchor = '#include "app_config.h"'
    if anchor not in content:
        raise BuildError(f"Jieli board file has no app_config.h include: {board_file}")
    block = (
        f"\n{LCD_MARKER}\n"
        "#if TCFG_LCD_ENABLE\n"
        '#include "lcd_driver.h"\n'
        # .te_mode.edge is an EDGE_* value, which lives in asm/exti.h; the
        # vendor's demo_ui board includes it alongside lcd_driver.h.
        '#include "asm/exti.h"\n'
        "LCD_PLATFORM_DATA_BEGIN(lcd_bd_cfg)\n"
        "    .lcd_name               = TCFG_LCD_DEVICE_NAME,\n"
        "    .lcd_io                 = {\n"
        "        .backlight          = TCFG_LCD_BL_IO,\n"
        "        .backlight_value    = TCFG_LCD_BL_VALUE,\n"
        "        .lcd_reset          = TCFG_LCD_RESET_IO,\n"
        "        .lcd_cs             = TCFG_LCD_CS_IO,\n"
        "        .lcd_rs             = TCFG_LCD_RS_IO,\n"
        "    },\n"
        "    .te_mode                = {\n"
        "        .te_mode_en         = TCFG_LCD_TE_ENABLE,\n"
        "        .gpio               = TCFG_LCD_TE_IO,\n"
        "        .edge               = EDGE_NEGATIVE,\n"
        "    },\n"
        "    .spi_lcd_interface      = TCFG_LCD_SPI_INTERFACE,\n"
        "LCD_PLATFORM_DATA_END()\n\n"
        "static const struct lcd_platform_data lcd_data = {\n"
        "    .cfg_num    = ARRAY_SIZE(lcd_bd_cfg),\n"
        "    .config_ptr = lcd_bd_cfg,\n"
        "};\n"
        "#endif\n"
    )
    content = content.replace(anchor, anchor + "\n" + block, 1)

    # Registering the device is separate from supplying its platform data: the
    # vendor driver is reached through dev_open("lcd"), which walks the device
    # table. Without this entry the open returns NULL and the panel is never
    # initialised - the failure is silent at the driver level and only shows up
    # as a failed tkl_disp_init() on the Tuya side.
    #
    # The demo_ui profile also lists "fb0"/"fb1"/"fb2"/"fb_out" here. Those come
    # from fb_component, which the TuyaOpen image does not stage, so only the
    # panel device is registered.
    device_table = "REGISTER_DEVICES(device_table) = {"
    if device_table not in content:
        raise BuildError(f"Jieli board file has no device table: {board_file}")
    lcd_entry = (
        "#if TCFG_LCD_ENABLE\n"
        '    { "lcd", &lcd_dev_ops, (void *)&lcd_data },\n'
        "#endif\n"
    )
    content = content.replace(device_table, device_table + "\n" + lcd_entry, 1)
    board_file.write_text(content, encoding="utf-8")
