#!/usr/bin/env python3
"""Validate the host inputs needed by the Jieli WL82/WL83 build bridge."""

from __future__ import annotations

import os
import shutil
import sys

from tools.jieli_build.chip_profiles import resolve_chip, resolve_sdk_root
from tools.jieli_build.errors import BuildError
from tools.jieli_build.portable_toolchain import prepare_bundled_windows_toolchain
from tools.jieli_build.toolchain import (
    download_windows_toolchain_installer,
    resolve_tool_dir,
    resolve_tool_path,
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
