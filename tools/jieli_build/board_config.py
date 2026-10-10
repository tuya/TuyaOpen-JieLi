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
        "#define USB_DEVICE_CLASS_CONFIG             (MASSSTORAGE_CLASS)\n"
        "#define TCFG_HOST_AUDIO_ENABLE              0\n"
        "#define TCFG_HOST_UVC_ENABLE                0\n"
        "#define TCFG_HID_HOST_ENABLE                0\n"
        "#define TCFG_UDISK_ENABLE                   0\n"
        '#include "usb_std_class_def.h"\n'
        '#include "usb_common_def.h"\n'
        "#endif\n\n"
    )
    _insert_before_final_endif(app_config_file, block)


def configure_usb_download_board_header(
    board_header: Path, usb_device: str = "TCFG_FUSB_DEVICE"
) -> None:
    """Enable the AC792 USB slave/MSD stack in the staged board profile.

    board_demo.h is reached from the USB sources through
    app_config.h -> board_config.h -> board_demo.h.
    """
    block = (
        f"{USB_DOWNLOAD_MARKER}\n"
        "#define TCFG_FUSB_DEVICE                    BIT(0)\n"
        "#define TCFG_HUSB_DEVICE                    BIT(1)\n"
        f"#define TCFG_USB_DEVICE                     {usb_device}\n"
        "#define TCFG_PC_ENABLE                      1\n"
        "#define USB_PC_NO_APP_MODE                  2\n"
        "#define USB_MALLOC_ENABLE                   1\n"
        "#define USB_DEVICE_CLASS_CONFIG             (MASSSTORAGE_CLASS)\n"
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


def configure_lcd_board_header(
    board_header: Path,
    panel_macro: str = "TCFG_LCD_MIPI_ST7701S_480x800",
) -> None:
    """Select a MIPI-DSI panel in the staged vendor board profile.

    The values are stated explicitly rather than left to lcd_board_cfg_template.h:
    that header deliberately carries no block for the 480x800 ST7701S panel (its
    own copy is commented out with the note that the board file states it
    directly), so including it here would leave every TCFG_LCD_* macro undefined.
    They match the vendor's demo_ui profile for this panel.

    TCFG_LCD_INPUT_FORMAT must agree with the LVGL colour depth the TuyaOpen side
    builds with; the vendor header notes the same requirement.
    """
    block = (
        f"{LCD_MARKER}\n"
        "#define TCFG_LCD_ENABLE                     1\n"
        f"#define {panel_macro}    1\n"
        "#define TCFG_LCD_INPUT_FORMAT               LCD_IN_RGB565\n"
        '#define TCFG_LCD_DEVICE_NAME                "MIPI_480x800_ST7701S"\n'
        "#define TCFG_LCD_BL_VALUE                   1\n"
        "#define TCFG_LCD_RESET_IO                   IO_PORTB_00\n"
        "#define TCFG_LCD_BL_IO                      IO_PORTB_01\n"
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
