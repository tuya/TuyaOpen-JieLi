#!/usr/bin/env python3
"""Validate the host inputs needed by the Jieli WL82/WL83 build bridge."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys

from tools.jieli_build.chip_profiles import resolve_chip, resolve_sdk_root
from tools.jieli_build.errors import BuildError
from tools.jieli_build.portable_toolchain import prepare_bundled_windows_toolchain
from tools.jieli_build.toolchain import (
    download_windows_toolchain_installer,
    install_linux_toolchain,
    resolve_tool_dir,
    resolve_tool_path,
)

PLATFORM_ROOT = Path(__file__).resolve().parent


def _check_linux_udev_rules(platform_root: Path) -> None:
    if sys.platform != "linux":
        return
    rules_src = platform_root / "tools" / "99-jieli.rules"
    if not rules_src.is_file():
        return
    has_rule = False
    for rules_dir in (Path("/etc/udev/rules.d"), Path("/usr/lib/udev/rules.d"), Path("/lib/udev/rules.d")):
        if rules_dir.is_dir():
            for rule_file in rules_dir.glob("*.rules"):
                try:
                    text = rule_file.read_text(encoding="utf-8", errors="ignore")
                    if "4c4a" in text.lower() and "3654" in text.lower():
                        has_rule = True
                        break
                except Exception:
                    continue
        if has_rule:
            break
    if not has_rule:
        dest = Path("/etc/udev/rules.d/99-jieli.rules")
        try:
            res = subprocess.run(
                ["sudo", "-n", "cp", str(rules_src), str(dest)],
                capture_output=True,
                check=False,
            )
            if res.returncode == 0:
                subprocess.run(["sudo", "-n", "udevadm", "control", "--reload-rules"], capture_output=True, check=False)
                subprocess.run(["sudo", "-n", "udevadm", "trigger"], capture_output=True, check=False)
                print("[JIELI] udev rules installed to /etc/udev/rules.d/99-jieli.rules")
                return
        except Exception:
            pass
        print(
            f"[JIELI] Notice: Non-root USB flashing rule not installed. To install:\n"
            f"        sudo cp {rules_src} /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger"
        )


def main(argv: list[str] | None = None) -> int:
    args = sys.argv if argv is None else argv
    try:
        chip_name = args[4].strip() if len(args) > 4 else ""
        if not chip_name:
            raise BuildError("CONFIG_CHIP_CHOICE is missing from build hook arguments")
        chip = resolve_chip(chip_name=chip_name)
        os.environ["JIELI_CHIP"] = chip.name
        sdk_root = resolve_sdk_root(chip_name=chip.name)
    except BuildError as exc:
        print(f"[JIELI] build setup failed: {exc}", file=sys.stderr)
        return 1

    try:
        tool_dir = resolve_tool_dir(sdk_root)
    except BuildError as exc:
        if os.name == "nt" and "JIELI_TOOL_DIR" not in os.environ:
            try:
                tool_dir = prepare_bundled_windows_toolchain()
            except BuildError as prepare_error:
                print(f"[JIELI] build setup failed: {prepare_error}", file=sys.stderr)
                return 1

            if tool_dir is None:
                print(f"[JIELI] build setup failed: {exc}", file=sys.stderr)
                try:
                    installer = download_windows_toolchain_installer()
                except BuildError as download_error:
                    print(f"[JIELI] {download_error}", file=sys.stderr)
                else:
                    print(f"[JIELI] Installer saved to: {installer}")
                    try:
                        os.startfile(str(installer))
                    except OSError as launch_error:
                        print(
                            f"[JIELI] Could not start the toolchain installer: {launch_error}",
                            file=sys.stderr,
                        )
                        print(f"[JIELI] Start this installer manually: {installer}")
                    else:
                        print(
                            "[JIELI] Complete the installer, then run `tos.py build` again. "
                            "Toolchain installation is interactive."
                        )
                return 1
        elif sys.platform.startswith("linux") and "JIELI_TOOL_DIR" not in os.environ:
            try:
                tool_dir = install_linux_toolchain()
            except BuildError as install_error:
                print(f"[JIELI] build setup failed: {install_error}", file=sys.stderr)
                return 1
        else:
            print(f"[JIELI] build setup failed: {exc}", file=sys.stderr)
            return 1

    make = shutil.which("make")
    if not make:
        print("[JIELI] build setup failed: make not found", file=sys.stderr)
        return 1

    vendor_source_root = sdk_root / chip.sdk_source_relative
    postbuild = vendor_source_root / chip.tools_relative / "download.sh"
    host_client = shutil.which("host-client")
    if host_client and chip.name == "wl82" and not postbuild.is_file():
        print(f"[JIELI] build setup failed: postbuild script not found: {postbuild}", file=sys.stderr)
        return 1
    if not host_client:
        objcopy = resolve_tool_path(tool_dir, "objcopy")
        if not objcopy.is_file():
            print(
                "[JIELI] build setup failed: host-client is unavailable and "
                f"Jieli objcopy was not found: {objcopy}",
                file=sys.stderr,
            )
            return 1

    print(f"[JIELI] SDK: {vendor_source_root}")
    print(f"[JIELI] toolchain: {tool_dir}")
    print(f"[JIELI] make: {make}")
    print(f"[JIELI] postbuild: {postbuild if host_client else 'raw app.bin via objcopy'}")
    _check_linux_udev_rules(PLATFORM_ROOT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
