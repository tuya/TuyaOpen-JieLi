"""Per-chip SDK profiles and target resolution."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Mapping, Optional

from .errors import BuildError

PLATFORM_ROOT = Path(__file__).resolve().parents[2]

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
        demo_lists_example_sources: bool = False,
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
        # Whether this chip's vendor demo_hello Makefile lists its own
        # apps/common/example/** self-test programs in c_SRC_FILES. The staging
        # step strips them; the flag lets it tell "nothing to strip because this
        # vendor layout has none" apart from "the Makefile moved and the strip
        # silently stopped working". wl82 lists none, wl83 lists 34.
        self.demo_lists_example_sources = demo_lists_example_sources

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
        demo_lists_example_sources=True,
    ),
}

def resolve_chip(environ: Optional[Mapping[str, str]] = None, chip_name: Optional[str] = None):
    env = os.environ if environ is None else environ
    name = chip_name if chip_name is not None else env.get("JIELI_CHIP", "wl82")
    name = name.strip()
    chip = JIELI_CHIPS.get(name)
    if chip is None:
        raise BuildError(f"unknown JIELI_CHIP '{name}'; expected one of {sorted(JIELI_CHIPS)}")
    return chip

def resolve_sdk_root(
    environ: Optional[Mapping[str, str]] = None,
    module_root: Path = PLATFORM_ROOT,
    chip_name: Optional[str] = None,
) -> Path:
    env = os.environ if environ is None else environ
    chip = resolve_chip(env, chip_name)
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
