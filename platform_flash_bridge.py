"""TuyaOpen flash bridge for Jieli AC79x boards.

On Windows, use the downloader shipped with the local AC79 SDK by default.
JIELI_FLASH_CMD remains available for board-specific uploader overrides.
The chip is selected via the JIELI_CHIP environment variable (default: wl82)
and falls back to probing the per-chip SDK directories on disk, including the
legacy wl82 layout where the SDK sat at the module root.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any


MODULE_ROOT = Path(__file__).resolve().parent

# (sdk_subdir, tools_subdir, isd -dev argument, boot address, reboot delay)
_FLASH_CHIPS = {
    "wl82": ("chip/wl82/AC79_AIoT_SDK", "cpu/wl82/tools", "wl82", "0x1c02000", "500"),
    "wl83": ("chip/wl83/AC792_SDK", "sdk/cpu/wl83/tools", "wl83", "0x103000", "50"),
}


def _staging_tools_dir(image: Path, chip_name: str) -> Path | None:
    """Locate the vendor build's generated tools dir for this image.

    isd_config.ini, uboot.boot and cfg_tool.bin are *produced by the vendor
    build* into the app's staging tree (the Makefile preprocesses
    cpu/<chip>/tools/isd_config_rule.c). They are not shipped in the SDK source
    tree, so pointing the downloader at the SDK checkout can never satisfy it -
    which is why flashing used to fail with "downloader is unavailable" even
    though isd_download.exe was right there.

    The staging tree mirrors the SDK layout, so the sub-path is chip-specific:
    wl82 stages at build/cpu/wl82/tools but wl83 at build/sdk/cpu/wl83/tools
    (JIELI_CHIPS sets sdk_source_relative to "." and "sdk" respectively). Use
    the same tools_subdir the SDK-side lookup uses rather than a literal.
    """
    entry = _FLASH_CHIPS.get(chip_name)
    if entry is None:
        return None
    tools_subdir = entry[1]

    for parent in image.resolve().parents:
        candidates = [parent / ".build" / "jieli-staging"]
        # A build configured with its own binary directory puts the staging tree
        # beside that directory instead of under .build.
        candidates += [sibling / "jieli-staging" for sibling in parent.glob("*")]
        for staging in candidates:
            tools = staging / "build" / tools_subdir
            if (tools / "isd_config.ini").is_file():
                return tools
    return None


def _resolve_flash_chip(name: str, image: Path | None = None) -> tuple[str, Path, str, str, str] | None:
    entry = _FLASH_CHIPS.get(name)
    if entry is None:
        return None
    sdk_dir, tools_subdir, dev, boot, reboot = entry
    candidates = []
    if image is not None:
        staging = _staging_tools_dir(image, name)
        if staging is not None:
            candidates.append(staging)
    candidates.append(MODULE_ROOT / sdk_dir / tools_subdir)
    if name == "wl82":
        # Legacy layout fallback for checkouts predating the chip/ split.
        candidates.append(MODULE_ROOT / "AC79_AIoT_SDK" / "cpu/wl82" / "tools")
    for tools_dir in candidates:
        if (tools_dir / "isd_download.exe").is_file():
            return dev, tools_dir, dev, boot, reboot
    return None


def _split_command(command_text: str) -> list[str]:
    """Split a configured uploader command without corrupting Windows paths."""
    if os.name != "nt":
        return shlex.split(command_text)

    # POSIX mode treats backslashes in ``C:\\path`` as escape characters.  The
    # non-POSIX mode preserves them; remove only the quote characters that were
    # used to keep paths containing spaces together.
    tokens = shlex.split(command_text, posix=False)
    return [
        token[1:-1]
        if len(token) >= 2 and token[0] == token[-1] and token[0] in ("'", '"')
        else token
        for token in tokens
    ]


def _default_flash_command(image: Path, chip_name: str,
                           origin: Path | None = None) -> tuple[list[str], Path] | None:
    """Return the SDK's Windows USB downloader command when it is available.

    `image` is what gets flashed; `origin` is the artifact it was copied from,
    used only to locate the staging tree (the image itself may live in a
    temporary directory when its name has to be shortened).
    """
    if os.name != "nt":
        return None

    resolved = _resolve_flash_chip(chip_name, origin if origin is not None else image)
    if resolved is None:
        return None
    dev, tools_dir, _, boot_addr, reboot_delay = resolved
    executable = tools_dir / "isd_download.exe"
    config = tools_dir / "isd_config.ini"
    uboot = tools_dir / "uboot.boot"
    cfg_tool = tools_dir / "cfg_tool.bin"
    if not all(path.is_file() for path in (executable, config, uboot, cfg_tool)):
        return None

    return (
        [
            str(executable),
            str(config),
            "-gen2",
            "-tonorflash",
            "-dev",
            dev,
            "-boot",
            boot_addr,
            "-div1",
            "-wait",
            "300",
            "-uboot",
            str(uboot),
            "-app",
            str(image),
            str(cfg_tool),
            "-res",
            "cfg",
            "-reboot",
            reboot_delay,
            "-extend-bin",
        ],
        tools_dir,
    )


def platform_flash(
    *,
    using_data: dict[str, str],
    binfile: str,
    port: str,
    baud: int,
    boards_root: str,
    logger: Any,
) -> dict[str, object]:
    del boards_root

    image = Path(binfile)
    if not image.is_file() or image.stat().st_size == 0:
        return {"success": False, "message": f"firmware image not found: {image}"}

    values = {
        "binfile": str(image),
        "port": port,
        "baud": str(baud or 0),
        "chip": using_data.get("CONFIG_CHIP_CHOICE", "wl82"),
        "board": using_data.get("CONFIG_BOARD_CHOICE", "AC79_DevKitBoard"),
    }
    command_text = os.environ.get("JIELI_FLASH_CMD", "").strip()
    staged_dir = None
    working_dir = None
    if command_text:
        try:
            command = _split_command(command_text.format(**values))
        except (KeyError, ValueError) as exc:
            return {"success": False, "message": f"invalid JIELI_FLASH_CMD: {exc}"}
        if not command:
            return {"success": False, "message": "JIELI_FLASH_CMD is empty"}
    else:
        # isd_download packages the input by basename and rejects names longer
        # than 15 characters.  Keep the user-facing artifact name unchanged,
        # but stage long project outputs under the SDK's conventional app.bin.
        upload_image = image
        if len(image.name) > 15:
            staged_dir = tempfile.TemporaryDirectory(prefix="jieli_flash_")
            upload_image = Path(staged_dir.name) / "app.bin"
            shutil.copyfile(image, upload_image)

        default = _default_flash_command(upload_image, values["chip"], origin=image)
        if default is None:
            if staged_dir is not None:
                staged_dir.cleanup()
            return {
                "success": False,
                "message": (
                    "Jieli SDK downloader is unavailable; set JIELI_FLASH_CMD to a "
                    "Jieli uploader command using {binfile}, {port}, and {baud} "
                    "placeholders"
                ),
            }
        command, working_dir = default

    logger.info(f"Jieli flash command: {shlex.join(command)}")
    try:
        try:
            result = subprocess.run(command, cwd=working_dir, check=False)
        except OSError as exc:
            return {"success": False, "message": f"cannot start Jieli uploader: {exc}"}
    finally:
        if staged_dir is not None:
            staged_dir.cleanup()
    if result.returncode != 0:
        return {"success": False, "message": f"uploader exited with {result.returncode}"}
    return {"success": True, "message": "Jieli uploader completed"}
