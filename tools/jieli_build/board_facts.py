"""Read the hardware facts a Jieli board declares in its Kconfig.

The staging hooks apply board-specific values to the vendor SDK: the USB device
macro, the memory map, the log and service UARTs and the panel. Those values
used to be per-board branches inside this package, and briefly a JSON file
beside each board. They are now ordinary Kconfig settings under
``boards/JIELI/<board>/Kconfig``, so the platform reads them from the evaluated
config the way it reads every other setting, and a new board is data rather
than a platform edit.

Values that would name a vendor macro stay semantic in Kconfig -- ``usb_role``
is ``host`` or ``device``, the panel is its model -- and the mapping to the
vendor's vocabulary lives here, in the platform. Pin names are the SDK's own
vocabulary and the board writes them as the vendor headers spell them.

Nothing falls back to a default. A fact the platform acts on is a fact the
board has to state: a missing setting is a build error naming the symbol, never
a silently inherited value. A default would only move the hardcoding.
"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import asdict, dataclass
from typing import Mapping, Optional

from .errors import BuildError

# The namespace a board declares its facts in. Both boards declare the same
# names, so the platform reads one set whichever is selected; Kconfig resolves
# each value from the node the board choice makes visible, and a setting the
# selected board does not declare simply is not there.
# The Kconfig spelling, and the one the evaluated config carries.
KCONFIG_PREFIX = "JIELI_BOARD_"
SYMBOL_PREFIX = "CONFIG_" + KCONFIG_PREFIX

# What the board's semantic usb_role maps onto.
USB_DEVICE_MACROS = {
    "host": "TCFG_HUSB_DEVICE",
    "device": "TCFG_FUSB_DEVICE",
}

# Panel model -> (the vendor macro that selects its driver, the device name the
# vendor's LCD registration matches on). A board names the model; which macro
# and which driver that means is the SDK's business, not the board's.
PANELS = {
    "MIPI_480x800_ST7701S": ("TCFG_LCD_MIPI_ST7701S_480x800", "MIPI_480x800_ST7701S"),
}

# A board names pins the way the vendor headers spell them, IO_PORT<L>_<NN>.
# That spelling is also a number - IO_PORTA_07 is (16 * 0 + 7) - which is the
# same flat pin index TUYA_GPIO_NUM_E uses, so no translation is needed. What
# does need checking is whether the pin exists, because the chips do not
# populate every bit of every port. Widths come from each chip's
# asm/gpio.h masks: wl83 PORTC is 0x3fff and PORTF is 0x003f, while wl82
# declares A..I at 16 bits each.
_PIN_RE = re.compile(r"^IO_PORT([A-Z])_(\d{1,2})$")
_CHIP_PORT_WIDTHS = {
    # WL82.h defines JL_PORTA..JL_PORTH. asm/gpio.h also names IO_PORTI_00..15,
    # but there is no JL_PORTI register block, so port I is not offered.
    "wl82": {"A": 16, "B": 16, "C": 16, "D": 16, "E": 16, "F": 16, "G": 16, "H": 16},
    "wl83": {"A": 16, "B": 16, "C": 14, "D": 16, "E": 16, "F": 6},
}


def _check_pin(text: str, chip: str, symbol: str) -> str:
    """Reject a pin string the selected chip does not have.

    The generated C would eventually fail to compile on an undefined vendor
    macro, but that lands after the adapters link and says nothing about which
    board fact was wrong. This reports it while the config is being read.
    """
    match = _PIN_RE.match(text)
    if match is None:
        raise BuildError(
            f"{_symbol(symbol)} is {text!r}; expected the vendor spelling "
            "IO_PORT<port letter>_<pin number>, for example IO_PORTA_05"
        )
    port, bit = match.group(1), int(match.group(2))
    widths = _CHIP_PORT_WIDTHS.get(chip)
    if widths is None:
        raise BuildError(
            f"{_symbol(symbol)} is {text!r}, but the chip is {chip!r}; "
            "expected wl82 or wl83"
        )
    width = widths.get(port)
    if width is None or bit >= width:
        if width is None:
            detail = f"{chip} has no port {port}"
        else:
            detail = f"{chip} port {port} has {width} pins, so bit {bit} does not exist"
        raise BuildError(f"{_symbol(symbol)} is {text!r}: {detail}")
    return text


@dataclass(frozen=True)
class Memory:
    """The board's memory map: on-chip flash and external SDRAM, in MiB."""

    flash_mib: int
    sdram_mib: int
    # True only when the board actually populates the external DDR. The vendor
    # SDK otherwise disables it through CONFIG_NO_SDRAM_ENABLE.
    external_ddr: bool


@dataclass(frozen=True)
class LogUart:
    """The SDK UART the board logs on.

    ``tx_pin`` is optional: a board that keeps the vendor's default pin leaves
    it empty, and the staging hook then leaves the vendor pin alone.
    """

    port: int
    tx_pin: Optional[str]


@dataclass(frozen=True)
class ServiceUart:
    """The second UART the TAL CLI is routed to."""

    port: int
    tx_pin: str
    rx_pin: str


@dataclass(frozen=True)
class Display:
    """The MIPI-DSI panel the board carries."""

    panel_macro: str
    device_name: str
    reset_io: str
    backlight_io: str


@dataclass(frozen=True)
class BoardFacts:
    """Every board-level fact the staging hooks act on.

    ``service_uart`` and ``display`` are None for a board that has neither.
    """

    board: str
    usb_device: str
    memory: Memory
    log_uart: LogUart
    service_uart: Optional[ServiceUart]
    display: Optional[Display]
    dvp_camera: bool = False
    csi2_camera: bool = False


def board_facts_digest(facts: BoardFacts) -> str:
    """Render the facts so a staging tree can record which ones it was built from.

    The staging inputs are named components and this is the one that covers the
    board's facts. It is a digest of the resolved facts rather than of a file,
    because there is no longer a file: the values come from the evaluated
    config, and what staging acts on is the result.
    """
    rendered = json.dumps(asdict(facts), sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(rendered.encode("utf-8")).hexdigest()


def _symbol(name: str) -> str:
    return SYMBOL_PREFIX + name


def _text(params: Mapping[str, str], name: str) -> str:
    return params.get(_symbol(name), "").strip()


def _require(params: Mapping[str, str], name: str) -> str:
    value = _text(params, name)
    if not value:
        raise BuildError(
            f"the selected board does not declare {_symbol(name)}; add it to "
            f"boards/JIELI/<board>/Kconfig"
        )
    return value


def _require_int(params: Mapping[str, str], name: str) -> int:
    raw = _require(params, name)
    try:
        return int(raw)
    except ValueError as exc:
        raise BuildError(f"{_symbol(name)} is not an integer: {raw!r}") from exc


def _flag(params: Mapping[str, str], name: str) -> bool:
    """A Kconfig bool. Unset means the symbol is absent, which is false."""
    return params.get(_symbol(name), "").strip() in ("y", "true", "True")


def _parse_memory(params: Mapping[str, str]) -> Memory:
    flash = _require_int(params, "FLASH_MIB")
    sdram = _require_int(params, "SDRAM_MIB")
    for name, value in (("FLASH_MIB", flash), ("SDRAM_MIB", sdram)):
        if value <= 0:
            raise BuildError(f"{_symbol(name)} must be positive, found {value}")
    return Memory(
        flash_mib=flash,
        sdram_mib=sdram,
        external_ddr=_flag(params, "EXTERNAL_DDR"),
    )


def _parse_log_uart(params: Mapping[str, str]) -> LogUart:
    port = _require_int(params, "LOG_UART_PORT")
    if port < 0:
        raise BuildError(f"{_symbol('LOG_UART_PORT')} must not be negative, found {port}")
    return LogUart(port=port, tx_pin=_text(params, "LOG_UART_TX_PIN") or None)


def _parse_service_uart(params: Mapping[str, str]) -> Optional[ServiceUart]:
    if not _flag(params, "HAS_SERVICE_UART"):
        return None
    port = _require_int(params, "SERVICE_UART_PORT")
    if port < 0:
        raise BuildError(
            f"{_symbol('SERVICE_UART_PORT')} must not be negative, found {port}"
        )
    return ServiceUart(
        port=port,
        tx_pin=_require(params, "SERVICE_UART_TX_PIN"),
        rx_pin=_require(params, "SERVICE_UART_RX_PIN"),
    )


def _parse_display(params: Mapping[str, str]) -> Optional[Display]:
    if not _flag(params, "HAS_DISPLAY"):
        return None
    model = _require(params, "DISPLAY_PANEL")
    known = PANELS.get(model)
    if known is None:
        raise BuildError(
            f"{_symbol('DISPLAY_PANEL')} names panel {model!r}, which this "
            f"platform has no vendor macro for. It knows: {', '.join(sorted(PANELS))}"
        )
    panel_macro, device_name = known
    return Display(
        panel_macro=panel_macro,
        device_name=device_name,
        reset_io=_require(params, "DISPLAY_RESET_IO"),
        backlight_io=_require(params, "DISPLAY_BACKLIGHT_IO"),
    )


def _parse_usb_device(params: Mapping[str, str]) -> str:
    role = _require(params, "USB_ROLE")
    macro = USB_DEVICE_MACROS.get(role)
    if macro is None:
        raise BuildError(
            f"{_symbol('USB_ROLE')} is {role!r}; expected one of "
            f"{', '.join(sorted(USB_DEVICE_MACROS))}"
        )
    return macro


def _validate_pins(facts: "BoardFacts", chip: str) -> None:
    """Check every pin the board declares against the selected chip's ports."""
    if facts.log_uart.tx_pin is not None:
        _check_pin(facts.log_uart.tx_pin, chip, "LOG_UART_TX_PIN")
    if facts.service_uart is not None:
        _check_pin(facts.service_uart.tx_pin, chip, "SERVICE_UART_TX_PIN")
        _check_pin(facts.service_uart.rx_pin, chip, "SERVICE_UART_RX_PIN")
    if facts.display is not None:
        _check_pin(facts.display.reset_io, chip, "DISPLAY_RESET_IO")
        _check_pin(facts.display.backlight_io, chip, "DISPLAY_BACKLIGHT_IO")


def load_board_facts(params: Mapping[str, str]) -> BoardFacts:
    """Read the selected board's facts out of the evaluated config.

    ``params`` is the build's config mapping -- the same one the build plan and
    the staging hooks already read. The board name comes from
    CONFIG_BOARD_CHOICE, which the platform has already confirmed against its
    Kconfig choice flag.
    """
    board = params.get("CONFIG_BOARD_CHOICE", "").strip()
    if not board:
        raise BuildError("CONFIG_BOARD_CHOICE is missing, so no board facts apply")
    chip = params.get("CONFIG_CHIP_CHOICE", "").strip()
    if not chip:
        raise BuildError("CONFIG_CHIP_CHOICE is missing, so the board's pins cannot be validated")
    dvp_cam = (
        params.get("CONFIG_ENABLE_BOARD_CAMERA_DVP", "").strip() in ("y", "true", "True")
        or params.get("CONFIG_BOARD_CAMERA_DVP", "").strip() in ("y", "true", "True")
    )
    csi2_cam = (
        params.get("CONFIG_ENABLE_BOARD_CAMERA_CSI2", "").strip() in ("y", "true", "True")
        or params.get("CONFIG_BOARD_CAMERA_CSI2", "").strip() in ("y", "true", "True")
    )
    facts = BoardFacts(
        board=board,
        usb_device=_parse_usb_device(params),
        memory=_parse_memory(params),
        log_uart=_parse_log_uart(params),
        service_uart=_parse_service_uart(params),
        display=_parse_display(params),
        dvp_camera=dvp_cam,
        csi2_camera=csi2_cam,
    )
    _validate_pins(facts, chip)
    return facts
