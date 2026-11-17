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
from .staging_fs import write_staged_bytes, write_staged_text

# The UART platform data names both the pins and the port group they belong to
# (``.port = PORTA_5_6`` next to ``.tx_pin = IO_PORTA_05``). The board states the
# pins; the group is derived from them, because the schema has no field for it
# and a pin pair is what actually selects the group.
_PIN_PATTERN = re.compile(r"^IO_PORT([A-Z])_(\d{1,2})$")
_ADC_INIT_CALL = re.compile(r"\badc_init\s*\(\s*\)\s*;")


def _mask_c_noncode(content: str, *, reject_line_comment_continuation: bool = False) -> str:
    """Mask comments and literals while preserving source offsets and lines."""
    masked = list(content)
    state = "code"
    index = 0
    while index < len(content):
        char = content[index]
        next_char = content[index + 1] if index + 1 < len(content) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                masked[index] = masked[index + 1] = " "
                index += 2
                state = "line_comment"
                continue
            if char == "/" and next_char == "*":
                masked[index] = masked[index + 1] = " "
                index += 2
                state = "block_comment"
                continue
            if char in ('"', "'"):
                masked[index] = " "
                state = "string" if char == '"' else "char"
        elif state in ("line_comment", "block_comment", "string", "char"):
            if char not in "\r\n":
                masked[index] = " "
            if state == "line_comment" and char == "\\" and next_char in "\r\n":
                # C phase-2 line splicing extends // comments across this newline.
                if reject_line_comment_continuation:
                    raise BuildError("board_init has a continued // comment; cannot safely stage ADC init")
                if next_char == "\r" and index + 2 < len(content) and content[index + 2] == "\n":
                    index += 3
                else:
                    index += 2
                continue
            if state == "line_comment" and char in "\r\n":
                state = "code"
            elif state == "block_comment" and char == "*" and next_char == "/":
                masked[index + 1] = " "
                index += 2
                state = "code"
                continue
            elif state in ("string", "char"):
                if char == "\\" and index + 1 < len(content):
                    if content[index + 1] not in "\r\n":
                        masked[index + 1] = " "
                    index += 2
                    continue
                if (state == "string" and char == '"') or (state == "char" and char == "'"):
                    state = "code"
        index += 1
    return "".join(masked)


def _board_init_body(content: str) -> tuple[int, int]:
    """Return the opening and closing brace offsets for board_init()."""
    masked = _mask_c_noncode(content)
    match = re.search(r"void\s+board_init\s*\([^)]*\)\s*\{", masked)
    if match is None:
        raise BuildError("Jieli board file has no board_init function")
    opening = masked.find("{", match.start(), match.end())
    brace_depth = 0
    conditional_depth = 0
    in_function = False
    line_offset = 0
    for line in masked.splitlines(keepends=True):
        line_end = line_offset + len(line)
        directive = re.match(r"\s*#\s*(\w+)\b", line)
        if directive:
            if in_function and any(char in line for char in "{}"):
                raise BuildError("board_init preprocessor directive contains braces")
            if in_function and line.rstrip("\r\n").endswith("\\"):
                raise BuildError("board_init has a continued preprocessor directive")
            command = directive.group(1)
            if command in ("if", "ifdef", "ifndef"):
                conditional_depth += 1
            elif command == "endif":
                conditional_depth -= 1
                if conditional_depth < 0:
                    raise BuildError("board_init has an unmatched #endif before its body")
        elif conditional_depth and (in_function or line_offset <= opening < line_end) and any(
            char in "{}" for char in line[max(0, opening - line_offset):]
        ):
            raise BuildError("board_init conditional branch contains braces")

        for offset in range(max(opening, line_offset), line_end):
            if offset == opening:
                in_function = True
            if not in_function or directive:
                continue
            if masked[offset] == "{":
                brace_depth += 1
            elif masked[offset] == "}":
                brace_depth -= 1
                if brace_depth == 0:
                    body = content[opening + 1:offset]
                    _mask_c_noncode(body, reject_line_comment_continuation=True)
                    return opening, offset
        line_offset = line_end
    raise BuildError("Jieli board_init function has unbalanced braces")


def _board_init_adc_call_count(content: str) -> int:
    opening, closing = _board_init_body(content)
    body = content[opening + 1 : closing]
    # Comments and literals are not executable calls; masking preserves offsets.
    body = _mask_c_noncode(body)
    return len(_ADC_INIT_CALL.findall(body))


def _make_adc_call_unconditional(content: str) -> str:
    """Keep ADC init unconditional without changing its board_init sequence point.

    We do not evaluate preprocessor expressions: the effective definitions can
    come from nested includes and build flags. Only a one-line #if/#endif block
    containing exactly the call can be safely moved. Other conditional shapes
    fail staging rather than guessing about execution or initialization order.
    """
    opening, closing = _board_init_body(content)
    body = content[opening + 1:closing]
    masked = _mask_c_noncode(body)
    match = _ADC_INIT_CALL.search(masked)
    if match is None:
        raise BuildError("Could not locate executable adc_init() in board_init")

    lines = body.splitlines(keepends=True)
    offsets: list[int] = []
    offset = 0
    for line in lines:
        offsets.append(offset)
        offset += len(line)
    call_line_index = next(
        (index for index, start in enumerate(offsets) if start <= match.start() < start + len(lines[index])),
        None,
    )
    if call_line_index is None:
        raise BuildError("Could not identify adc_init() line in board_init")

    call_line = lines[call_line_index]
    call_line_start = offsets[call_line_index]
    if masked[call_line_start:match.start()].strip() or masked[match.end():call_line_start + len(call_line)].strip():
        raise BuildError("adc_init() shares a line with other code; cannot safely relocate it")

    depth = 0
    for index in range(call_line_index):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|endif)\b", lines[index])
        if directive:
            depth += 1 if directive.group(1) != "endif" else -1
    if depth == 0:
        return content

    # Accept only a top-level guard directly wrapping this call. This keeps
    # placement relative to surrounding hardware initialization and rejects
    # nested guards, #else/#elif, and wider condition-controlled sequences.
    if call_line_index == 0 or call_line_index + 1 >= len(lines):
        raise BuildError("Conditionally guarded adc_init() has no simple #if/#endif wrapper")
    guard_line = lines[call_line_index - 1]
    end_line = lines[call_line_index + 1]
    if not re.match(r"\s*#\s*if\b", guard_line) or not re.match(r"\s*#\s*endif\b", end_line):
        raise BuildError("Conditionally guarded adc_init() uses #else/#elif or a complex guard")
    if depth != 1:
        raise BuildError("Conditionally guarded adc_init() is nested; cannot safely relocate it")
    if masked[offsets[call_line_index - 1] + len(guard_line):call_line_start].strip():
        raise BuildError("Conditionally guarded adc_init() has intervening code")
    end_start = offsets[call_line_index + 1]
    if masked[call_line_start + len(call_line):end_start].strip():
        raise BuildError("Conditionally guarded adc_init() has intervening code")

    indentation = re.match(r"[ \t]*", call_line).group(0)
    newline = "\r\n" if "\r\n" in body else "\n"
    inserted_call = f"{indentation}adc_init();{newline}"
    guard_start = offsets[call_line_index - 1]
    body = (
        body[:guard_start]
        + inserted_call
        + body[guard_start:call_line_start]
        + body[call_line_start:call_line_start + (match.start() - call_line_start)]
        + body[match.end():]
    )
    return content[:opening + 1] + body + content[closing:]

# The port-group macros the wl82 SDK's asm/uart.h defines, and the only values a
# UART platform-data block may name. The header lists exactly these, so a pair
# that is not among them has no macro to expand to: the generated block would
# name an identifier the vendor headers never define, and the compile would fail
# inside SDK code rather than here.
#
# A pair that is not among them has no macro to expand to: the generated block
# would name an identifier the vendor headers never define, and the compile
# would fail inside SDK code rather than here.
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
    write_staged_text(board_file, content)

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
    write_staged_text(board_header, content)

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

    write_staged_text(board_file, content)

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
    write_staged_text(board_file, content)

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
    write_staged_text(app_config_file, content[:insert_at] + config + content[insert_at:])

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

    write_staged_text(config_file, content)

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
    write_staged_text(ddr_config_file, content)


_TUYA_FLASH_PARTITIONS = ("KV_KEY", "KV_DATA", "UF", "RCD", "USER0")


def _c_hex_literal(content: str, name: str, source: Path) -> int:
    """Read one plain hex #define; the flash map is parsed, not duplicated."""
    match = re.search(rf"(?m)^[ \t]*#define[ \t]+{name}[ \t]+(0x[0-9A-Fa-f]+)U?[ \t]*$", content)
    if match is None:
        raise BuildError(
            f"Expected one hex-literal {name} in {source}; the vendor reserved area "
            f"is derived from it"
        )
    return int(match.group(1), 16)


def tuya_flash_reserved_area(tkl_flash_c: Path) -> tuple[int, int]:
    """Return the (start, length) span of the TuyaOpen flash partitions."""
    if not tkl_flash_c.is_file():
        raise BuildError(f"TuyaOpen flash map not found: {tkl_flash_c}")

    content = tkl_flash_c.read_text(encoding="utf-8")
    spans = []
    for partition in _TUYA_FLASH_PARTITIONS:
        start = _c_hex_literal(content, f"JIELI_{partition}_START", tkl_flash_c)
        size = _c_hex_literal(content, f"JIELI_{partition}_SIZE", tkl_flash_c)
        if size == 0:
            raise BuildError(f"Zero-sized TuyaOpen flash partition: JIELI_{partition}_SIZE")
        spans.append((start, start + size))

    start = min(span[0] for span in spans)
    end = max(span[1] for span in spans)
    if start % 0x1000 or (end - start) % 0x1000:
        raise BuildError("TuyaOpen flash partitions must be 4 KiB aligned")
    return start, end - start


def configure_flash_reserved_area(
    rule_file: Path, tkl_flash_c: Path, facts: BoardFacts
) -> None:
    """Reserve the TuyaOpen partition span in the vendor flash map.

    ``_OPT=1`` tells the downloader not to touch the area, so the KV
    credentials and the UF/RCD partitions survive a USB download. The address
    is read from tkl_flash.c, the map the runtime actually uses.
    """
    if not rule_file.is_file():
        raise BuildError(f"Vendor flash rule not found: {rule_file}")

    start, length = tuya_flash_reserved_area(tkl_flash_c)
    flash_size = facts.memory.flash_mib * 1024 * 1024
    if start + length > flash_size:
        raise BuildError(
            f"TuyaOpen flash area 0x{start:06X}+0x{length:06X} exceeds the "
            f"{facts.board} {facts.memory.flash_mib} MiB flash"
        )

    block = (
        "// TUYAOPEN_RESERVED_AREA_BEGIN\n"
        f"TUYAOPEN_ADR=0x{start:06X}; [TuyaOpen KV/UF/RCD/USER0]\n"
        f"TUYAOPEN_LEN=0x{length:06X};\n"
        "TUYAOPEN_OPT=1;\n"
        "// TUYAOPEN_RESERVED_AREA_END\n"
    )
    content = rule_file.read_text(encoding="utf-8")
    content, count = re.subn(
        r"(?ms)^[ \t]*// TUYAOPEN_RESERVED_AREA_BEGIN.*?// TUYAOPEN_RESERVED_AREA_END\r?\n?",
        block,
        content,
    )
    if count == 0:
        anchor = "BTIF_ADR=AUTO;"
        if anchor not in content:
            raise BuildError(f"Vendor flash rule has no {anchor}: {rule_file}")
        content = content.replace(anchor, block + "\n" + anchor, 1)
    elif count != 1:
        raise BuildError(
            f"Expected at most one TUYAOPEN_RESERVED_AREA block in {rule_file}, found {count}"
        )
    write_staged_text(rule_file, content)


def configure_peripheral_header(board_file: Path, config_values) -> Path:
    """Stage the board's peripheral pin facts as a header next to board.c.

    The adapter sources are compiled with this header force-included, so a pin
    a board declares in Kconfig reaches the platform data without a second copy
    in C. Symbols the board does not declare stay undefined, which lets the
    adapter keep its own fallback, and the header is written either way so the
    build's -include never points at a missing file.
    """
    pins = (
        ("CONFIG_JIELI_BOARD_SPI0_CLK_PIN", "TUYA_BOARD_SPI0_CLK_PIN"),
        ("CONFIG_JIELI_BOARD_SPI0_DO_PIN", "TUYA_BOARD_SPI0_DO_PIN"),
        ("CONFIG_JIELI_BOARD_SPI0_DI_PIN", "TUYA_BOARD_SPI0_DI_PIN"),
        ("CONFIG_JIELI_BOARD_SPI1_CLK_PIN", "TUYA_BOARD_SPI1_CLK_PIN"),
        ("CONFIG_JIELI_BOARD_SPI1_DO_PIN", "TUYA_BOARD_SPI1_DO_PIN"),
        ("CONFIG_JIELI_BOARD_SPI1_DI_PIN", "TUYA_BOARD_SPI1_DI_PIN"),
        ("CONFIG_JIELI_BOARD_SPI2_CLK_PIN", "TUYA_BOARD_SPI2_CLK_PIN"),
        ("CONFIG_JIELI_BOARD_SPI2_DO_PIN", "TUYA_BOARD_SPI2_DO_PIN"),
        ("CONFIG_JIELI_BOARD_SPI2_DI_PIN", "TUYA_BOARD_SPI2_DI_PIN"),
        ("CONFIG_JIELI_BOARD_UART1_TX_PIN", "TUYA_BOARD_UART1_TX_PIN"),
        ("CONFIG_JIELI_BOARD_UART1_RX_PIN", "TUYA_BOARD_UART1_RX_PIN"),
        ("CONFIG_JIELI_BOARD_PWM0_H_PIN", "TUYA_BOARD_PWM0_H_PIN"),
        ("CONFIG_JIELI_BOARD_PWM0_L_PIN", "TUYA_BOARD_PWM0_L_PIN"),
        ("CONFIG_JIELI_BOARD_PWM1_H_PIN", "TUYA_BOARD_PWM1_H_PIN"),
        ("CONFIG_JIELI_BOARD_PWM1_L_PIN", "TUYA_BOARD_PWM1_L_PIN"),
        ("CONFIG_JIELI_BOARD_PWM2_H_PIN", "TUYA_BOARD_PWM2_H_PIN"),
        ("CONFIG_JIELI_BOARD_PWM2_L_PIN", "TUYA_BOARD_PWM2_L_PIN"),
        ("CONFIG_JIELI_BOARD_PWM3_H_PIN", "TUYA_BOARD_PWM3_H_PIN"),
        ("CONFIG_JIELI_BOARD_PWM3_L_PIN", "TUYA_BOARD_PWM3_L_PIN"),
        ("CONFIG_JIELI_BOARD_PWM0_PIN", "TUYA_BOARD_PWM0_PIN"),
        ("CONFIG_JIELI_BOARD_IIC0_SCL_PIN", "TUYA_BOARD_IIC0_SCL_PIN"),
        ("CONFIG_JIELI_BOARD_IIC0_SDA_PIN", "TUYA_BOARD_IIC0_SDA_PIN"),
        ("CONFIG_JIELI_BOARD_IIC1_SCL_PIN", "TUYA_BOARD_IIC1_SCL_PIN"),
        ("CONFIG_JIELI_BOARD_IIC1_SDA_PIN", "TUYA_BOARD_IIC1_SDA_PIN"),
        ("CONFIG_JIELI_BOARD_IIC2_SCL_PIN", "TUYA_BOARD_IIC2_SCL_PIN"),
        ("CONFIG_JIELI_BOARD_IIC2_SDA_PIN", "TUYA_BOARD_IIC2_SDA_PIN"),
    )
    defines = []
    for symbol, macro in pins:
        raw = None if config_values is None else config_values.get(symbol)
        if raw is None:
            continue
        text = str(raw).strip()
        if not text:
            continue
        try:
            value = int(text, 0)
        except ValueError:
            raise BuildError(f"{symbol} must be an integer pin number, got {raw!r}")
        if value < 0:
            raise BuildError(f"{symbol} must not be negative, got {value}")
        defines.append(f"#define {macro} {value}")

    if config_values is not None and \
            str(config_values.get("CONFIG_JIELI_BOARD_UART1_ENABLE", "")).strip() == "y":
        defines.append("#define TUYA_BOARD_UART1_ENABLE 1")

    lines = [
        "/* Generated by the JIELI platform from the board Kconfig. */",
        "#ifndef TUYA_BOARD_PERIPH_CONFIG_H",
        "#define TUYA_BOARD_PERIPH_CONFIG_H",
        "",
    ]
    lines.extend(defines)
    if defines:
        lines.append("")
    lines.append("#endif")
    lines.append("")

    header = board_file.parent / "tuya_board_periph_config.h"
    write_staged_text(header, "\n".join(lines))
    return header


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
        write_staged_text(board_file, content)
        write_staged_bytes(staged_header, audio_config_header.read_bytes())
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
    init_marker = re.search(
        r"void\s+board_init\s*\([^)]*\)\s*\{", _mask_c_noncode(content)
    )
    if init_marker is None:
        raise BuildError(f"Jieli board file has no board_init function: {board_file}")
    existing_adc_calls = _board_init_adc_call_count(content)
    if existing_adc_calls > 1:
        raise BuildError(
            f"Jieli board_init already contains {existing_adc_calls} adc_init() calls: {board_file}"
        )
    profile_init = board.board_init
    profile_adc_calls = len(_ADC_INIT_CALL.findall(profile_init))
    if existing_adc_calls:
        # Do not infer effective macros from app_config.h text: nested guards,
        # included headers, and build flags determine the actual preprocessing.
        # Preserve unconditional calls; safely hoist only a simple direct guard.
        try:
            content = _make_adc_call_unconditional(content)
        except BuildError as error:
            raise BuildError(f"Cannot guarantee board ADC initialization in {board_file}: {error}") from error
    if existing_adc_calls and profile_adc_calls:
        # Do not reinitialize the shared SDK ADC queue if this board already
        # performs startup initialization (WL83's demo board does).
        profile_init = _ADC_INIT_CALL.sub("", profile_init)
    elif not existing_adc_calls and not profile_adc_calls:
        # The TKL ADC registers channels on the SDK's global queue. Ensure it
        # exists even on a board whose profile has no ADC-specific init code.
        profile_init = "\n    adc_init();\n" + profile_init
    if profile_adc_calls > 1:
        raise BuildError("Jieli audio board profile contains multiple adc_init() calls")
    if profile_init:
        content = content[:init_marker.end()] + profile_init + content[init_marker.end():]

    content = content[:table_start] + board.board_declarations + "\n" + content[table_start:]
    table_start = content.find(table_marker)
    table_open = content.find("{", table_start + len(table_marker) - 1)
    if table_open < 0:
        raise BuildError(f"Jieli device table has no opening brace: {board_file}")
    if board.registers_audio_device:
        content = content[: table_open + 1] + (
            '\n    {"audio", &audio_dev_ops, (void *)&tuya_audio_data },'
        ) + content[table_open + 1 :]
    write_staged_text(board_file, content)
    write_staged_bytes(staged_header, audio_config_header.read_bytes())


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
    write_staged_text(board_file, content)


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
    write_staged_text(config_file, content[:insert_at] + block + content[insert_at:])


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
    write_staged_text(config_file, updated)


def _define_macro(config_file: Path, name: str, value: str = None) -> None:
    """Append ``#define <name> [value]`` before the final ``#endif`` of a staged
    header.

    Used for switches the vendor profile does not define at all. Idempotent:
    a header that already carries the define is left alone. A bare define is
    only valid where the vendor writes ``#ifdef``; the vendor files that test
    ``#if NAME`` need a value, so callers pass one when that is the case.
    """
    text = f"#define {name}" if value is None else f"#define {name} {value}"
    _insert_before_final_endif(config_file, f"{text}\n", marker=f"#define {name}")


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


def _usb_class_block(usb_cdc_log: bool) -> str:
    """The USB class list, decided here rather than by the C preprocessor.

    The vendor make never sees a TuyaOpen ``-D``, so a ``#if defined(CONFIG_*)``
    in the generated header is always false: it silently left the CDC class out
    and cdc.c compiled to an empty object, which then failed to link against the
    log output. The switch is a Kconfig value the caller already has, so the
    class list is written literally. ``usb_common_def.h`` derives
    TCFG_USB_SLAVE_CDC_ENABLE from MASSSTORAGE_CLASS/CDC_CLASS, so that macro
    must not be set here as well.
    """
    classes = "MASSSTORAGE_CLASS | CDC_CLASS" if usb_cdc_log else "MASSSTORAGE_CLASS"
    return f"#define USB_DEVICE_CLASS_CONFIG             ({classes})\n"


def configure_dual_bank_app_config(app_config_file: Path, config_values) -> None:
    """Turn on the vendor's dual-bank OTA engine when the board asks for it.

    ``lib_update_config.c`` maps CONFIG_DOUBLE_BANK_ENABLE onto
    ``support_dual_bank_update_en``, which is what ota_impl.h's
    dual_bank_passive_update_* path and the uboot bank switch key off.
    """
    if config_values is None:
        return
    if str(config_values.get("CONFIG_JIELI_BOARD_DUAL_BANK", "")).strip() not in ("y", "true", "True"):
        return
    # The vendor profile defines both together: a dual-bank image needs the
    # duplicated uboot, or the boot side cannot switch between banks.
    # lib_update_config.c and download.c evaluate these in ``#if`` expressions,
    # so they need a value; a bare define fails to compile.
    _define_macro(app_config_file, "CONFIG_DOUBLE_BANK_ENABLE", "1")
    _define_macro(app_config_file, "CONFIG_SUPPORT_DOUBLE_UBOOT", "1")


def configure_usb_download_app_config(app_config_file: Path, usb_cdc_log: bool = False) -> None:
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
        + _usb_class_block(usb_cdc_log)
        + "#define TCFG_HOST_AUDIO_ENABLE              0\n"
        "#define TCFG_HOST_UVC_ENABLE                0\n"
        "#define TCFG_HID_HOST_ENABLE                0\n"
        "#define TCFG_UDISK_ENABLE                   0\n"
        '#include "usb_std_class_def.h"\n'
        '#include "usb_common_def.h"\n'
        "#endif\n\n"
    )
    _insert_before_final_endif(app_config_file, block)


def configure_usb_download_board_header(
    board_header: Path, facts: BoardFacts, usb_cdc_log: bool = False
) -> None:
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
        + _usb_class_block(usb_cdc_log)
        + '#include "usb_std_class_def.h"\n'
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
    write_staged_text(board_file, content)


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
    write_staged_text(board_file, content)
