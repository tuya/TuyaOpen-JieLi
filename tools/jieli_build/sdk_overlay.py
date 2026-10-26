"""Create and clean an isolated Jieli SDK staging tree.

The staging tree is kept across builds rather than deleted and recreated. The
vendor make's objects live inside it, so deleting it costs a full recompile of
every vendor source -- tens of seconds, and the largest single cost of a build
that changed nothing. Instead the tree records what it was built from, in a
state file beside it, and a tree whose inputs all still match is left exactly as
it was. Any input that cannot be shown to match forces a full rebuild: a stale
staging tree produces a wrong image silently, which is far worse than a slow
build.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional

from .audio_profile import AUDIO_PROFILES, apply_audio_profile
from .board_config import (
    configure_audio_board, configure_board_memory, configure_full_stack_app_config,
    configure_camera_app_config, configure_camera_board, configure_camera_board_header,
    configure_full_stack_board, configure_lcd_app_config, configure_lcd_board,
    configure_lcd_board_header, configure_log_uart_baudrate, configure_log_uart_block,
    configure_lowpower, configure_service_uart, configure_usb_download_app_config,
    configure_usb_download_board, configure_usb_download_board_header,
)
from .board_facts import BoardFacts, board_facts_digest
from .chip_profiles import JIELI_CHIPS, PLATFORM_ROOT, resolve_chip
from .errors import BuildError
from .toolchain import resolve_tool_path
from .feature_map import (
    FEATURES,
    enabled_features,
    SDK_SOURCE_PREFIX,
    feature_defines,
    feature_sources,
    link_libraries,
)

# SDK-root-relative sources that keep the vendor USB slave stack serviced while
# the Tuya app runs. msd_upgrade.c owns go_mask_usb_updata() and user_setup.c
# answers the private GET_STATUS download-mode request; task_pc.c owns the
# persistent usb_msd0 worker. The adapter event hook below routes OTG events
# without compiling the SDK's unrelated SD mount implementation.
USB_DEVICE_SOURCES = (
    "apps/common/usb/usb_config.c",
    "apps/common/usb/usb_epbuf_manager.c",
    "apps/common/usb/device/usb_device.c",
    "apps/common/usb/device/descriptor.c",
    "apps/common/usb/device/user_setup.c",
    "apps/common/usb/device/cdc.c",
    "apps/common/usb/device/msd.c",
    "apps/common/usb/device/msd_upgrade.c",
    "apps/common/usb/device/task_pc.c",
)

# Paths the vendor USB device stack expects on the include search path. The
# host/ entries are needed by usb_epbuf_manager.c even for a device-only build.
USB_INCLUDE_DIRS = (
    "apps/common",
    "apps/common/usb",
    "apps/common/usb/device",
    "apps/common/usb/host",
    "apps/common/usb/include",
    "apps/common/usb/include/host",
    "include_lib/driver/device/usb",
    "include_lib/driver/device/usb/device",
    "include_lib/driver/device/usb/host",
)


def usb_device_sources() -> list[str]:
    """Render the USB device sources relative to the demo board directory."""
    return ["../../../../../" + source for source in USB_DEVICE_SOURCES]


def usb_include_lines() -> list[str]:
    """Render the USB include paths relative to the demo board directory."""
    return ["-I../../../../../" + directory for directory in USB_INCLUDE_DIRS]


# The vendor LCD stack lives in apps/common/lcd and is not referenced by the
# staged demo_hello profile at all, so its objects are absent unless staged
# explicitly. The authoritative list of what the display path needs is the
# vendor's own demo_ui Makefile; the display feature in feature_map.py mirrors
# its LCD/dma2d entries.
#
# fb_component/ IS required. It reads like a UI-side compositor, but
# lcd_driver.c's DMM/DMA2D path resolves into it, and leaving it out leaves
# mipi_start_display/dpi_close/dmm_deinit/dma2d_free undefined at link time.
#
# Only the one panel the board selects is compiled. Panel drivers register
# themselves into the .lcd_device_drive linker section, so compiling a panel is
# what makes it selectable; the other nine would only cost image space.
#
# The vendor profile's freetype and rlottie entries are text/vector renderers
# and are not part of the panel path, so they are not staged.
def lcd_sources() -> list[str]:
    """Render the display feature's sources relative to the demo board directory."""
    return [SDK_SOURCE_PREFIX + source for source in FEATURES["display"].sources]


def lcd_include_lines(cpu: str) -> list[str]:
    """Render the display feature's include paths relative to the demo board directory."""
    return [
        "-I" + SDK_SOURCE_PREFIX + directory.format(cpu=cpu)
        for directory in FEATURES["display"].includes
    ]


def audio_board_config_path(tuyaopen_root: Path, board_name: Optional[str], profile) -> Path:
    """Return the board's audio_config.h, preferring the selected board.

    The chip profile also carries a board name, and that is what a board with
    no audio data of its own falls back to. Reading the selected board first is
    what lets a second board on the same chip bring its own audio data instead
    of silently inheriting the first one's.
    """
    boards_root = tuyaopen_root / "boards" / "JIELI"
    if board_name:
        selected = boards_root / board_name / "audio_config.h"
        if selected.is_file():
            return selected
    return boards_root / profile.audio_board_name / "audio_config.h"


# The vendor demo_hello Makefile lists its own peripheral self-test programs and
# the dhrystone benchmark in c_SRC_FILES. Nothing in a TuyaOpen image references
# them: -flto is on and sdk.map carries no apps/common/example entry, so all 32
# objects are discarded at link time. They are still recompiled on every build --
# create_staging_tree recreates the staging tree each time -- and they are about
# 45% of the vendor-side objects, so they dominate the vendor compile.
VENDOR_EXAMPLE_SOURCE_PREFIX = "../../../../../apps/common/example/"


def drop_vendor_example_sources(content: str, expects_examples: bool = False) -> str:
    """Remove the vendor demo's own example sources from c_SRC_FILES.

    Every one of those lines ends with a line continuation, so removing whole
    lines keeps the surrounding list valid.

    Only the wl83 demo Makefile lists these sources; the wl82 one lists none, so
    an empty result is a valid vendor layout rather than a format error. The
    caller says which it expects: for a chip that does list them, finding none
    means the Makefile moved and the strip silently stopped working, which would
    quietly put ~45% of the vendor compile back.
    """
    kept = [
        line
        for line in content.splitlines(keepends=True)
        if not line.lstrip().startswith(VENDOR_EXAMPLE_SOURCE_PREFIX)
    ]
    dropped = len(content.splitlines()) - len(kept)
    if expects_examples and dropped == 0:
        raise BuildError(
            "Jieli demo Makefile was expected to list "
            f"{VENDOR_EXAMPLE_SOURCE_PREFIX} sources but lists none"
        )
    return "".join(kept)


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


def _ignore_cpu_build_outputs(directory: str, names: list[str]) -> set[str]:
    generated = {
        "app.bin",
        "download.bat",
        "download.sh",
        "isd_config.ini",
        "isd_config_loader.ini",
        "jl_isd.fw",
        "jl_isd.ufw",
        "sdk.elf",
        "sdk.elf.objs.txt",
        "sdk.ld",
        "sdk.map",
        "sdk_used_list.used",
        "section.txt",
    }
    ignored = {name for name in names if name in generated or name.endswith((".bc", ".o", ".d"))}
    if Path(directory).parent.name == "cpu" and "liba" in names:
        ignored.add("liba")
    return ignored


def _ignore_demo_build_outputs(directory: str, names: list[str]) -> set[str]:
    return {
        name
        for name in names
        if name in {"objs", "dist", ".build", "section.txt"} or name.endswith((".o", ".d"))
    }


def stage_sdk_inputs(sdk_root: Path, build_root: Path, chip) -> Path:
    """Copy writable SDK paths and link SDK directories used only as inputs."""
    vendor_root = sdk_root / chip.sdk_source_relative
    source_root = build_root / chip.sdk_source_relative
    source_root.mkdir(parents=True, exist_ok=True)

    for name in ("include_lib", "lib", "tools"):
        source = vendor_root / name
        if not source.is_dir():
            raise BuildError(f"Jieli SDK input directory is missing: {source}")
        # The make/post-build flow reads these trees; generated artifacts are
        # written under cpu/<chip>/tools, which is part of the copied CPU tree.
        link_directory(source_root / name, source)

    vendor_cpu_root = vendor_root / "cpu" / chip.cpu
    staged_cpu_root = source_root / "cpu" / chip.cpu
    if not vendor_cpu_root.is_dir():
        raise BuildError(f"Jieli SDK CPU profile is missing: {vendor_cpu_root}")
    shutil.copytree(vendor_cpu_root, staged_cpu_root, ignore=_ignore_cpu_build_outputs)
    vendor_liba = vendor_cpu_root / "liba"
    if vendor_liba.is_dir():
        # The linker consumes the prebuilt archives without modifying them.
        link_directory(staged_cpu_root / "liba", vendor_liba)

    vendor_common = vendor_root / "apps" / "common"
    staged_apps = source_root / "apps"
    staged_common = staged_apps / "common"
    staged_common.mkdir(parents=True, exist_ok=True)
    for item in vendor_common.iterdir():
        destination = staged_common / item.name
        if item.name == "movable" and item.is_dir():
            # pre_build writes section.txt here, so keep this tree local.
            shutil.copytree(item, destination, ignore=_ignore_demo_build_outputs)
        elif item.is_dir():
            link_directory(destination, item)
        else:
            shutil.copy2(item, destination)

    demo_source = vendor_root / "apps" / "demo" / "demo_hello"
    shutil.copytree(demo_source, staged_apps / "demo" / "demo_hello", ignore=_ignore_demo_build_outputs)
    return source_root


def stage_audio_sdk_sources(source_overlay_root: Path, vendor_root: Path, chip_name: str) -> None:
    """Expose the vendor audio trees the audio profile's inputs reference.

    The minimal staging tree copies only cpu, apps, include_lib, lib and tools;
    trees the profile lists under staged_vendor_trees live outside them, so
    link those in read-only.
    """
    profile = AUDIO_PROFILES.get(chip_name)
    if profile is None:
        return
    missing = [
        required for required in profile.staged_vendor_files
        if not (vendor_root / required).is_file()
    ]
    if missing:
        raise BuildError(f"{chip_name} audio SDK configuration is missing: {missing[0]}")
    for tree in profile.staged_vendor_trees:
        destination = source_overlay_root / tree
        if not destination.exists():
            link_directory(destination, vendor_root / tree)


def clean_staging_tree(staging_root: Path) -> None:
    """Remove only generated staging files, never files under the vendor SDK."""
    if staging_root.is_dir():
        shutil.rmtree(staging_root)


# The state file lives inside the tree it describes, so anything that removes
# the tree -- clean_staging_tree, a manual rm -rf, a fresh .build -- removes the
# record with it and the next build stages from scratch.
STAGING_STATE_NAME = ".jieli-staging-state.json"
STAGING_STATE_VERSION = 1


def _ignore_python_caches(directory: str, names: list[str]) -> set[str]:
    """Keep interpreter caches out of a content digest.

    ``__pycache__`` is rewritten by every test run with a fresh embedded
    timestamp, so digesting it would make the staging tree look stale on every
    build that follows a test run.
    """
    del directory
    return {name for name in names if name == "__pycache__" or name.endswith((".pyc", ".pyo"))}


def _absolute(path: Optional[Path]) -> str:
    """Render a path the way the staging hooks consume it, or "" for none."""
    return Path(path).resolve().as_posix() if path is not None else ""


def _file_digest(path: Optional[Path]) -> str:
    """Digest one input file's bytes, or say that the file is absent.

    Absent is a distinct value rather than an empty digest, so adding or
    removing a file the staging hooks read always changes the result.
    """
    if path is None:
        return "<none>"
    try:
        content = Path(path).read_bytes()
    except OSError:
        return "<absent>"
    return "sha256:" + hashlib.sha256(content).hexdigest()


def _walk(root: Path, ignore):
    """Yield (relative posix path, entry) pairs, in a stable order.

    The order has to be independent of the filesystem's, or two runs over the
    same tree would digest differently.
    """
    for dirpath, dirnames, filenames in os.walk(root):
        ignored = ignore(dirpath, list(dirnames) + list(filenames)) if ignore else set()
        dirnames[:] = sorted(name for name in dirnames if name not in ignored)
        for name in sorted(filenames):
            if name in ignored:
                continue
            path = Path(dirpath) / name
            yield path.relative_to(root).as_posix(), path


def _tree_metadata_digest(root: Path, ignore=None) -> str:
    """Digest a tree from file sizes and mtimes, never from file contents.

    The vendor SDK is most of a gigabyte, so reading it on every build would
    cost more than the compile this guard exists to skip. Size plus mtime is the
    same evidence make itself compares, and it notices every edit a checkout, an
    editor or a build leaves behind. Its weak boundary is a file rewritten to
    exactly the same length inside one filesystem timestamp tick: that is
    invisible here, and the tree is then treated as unchanged. The tick is
    100 ns on NTFS and 2 s on FAT, so the window is narrow but not empty.
    """
    digest = hashlib.sha256()
    if not root.is_dir():
        digest.update(b"<absent>\n")
        return "sha256:" + digest.hexdigest()
    for relative, path in _walk(root, ignore):
        try:
            info = path.stat()
        except OSError:
            continue
        digest.update(f"{relative}\0{info.st_size}\0{info.st_mtime_ns}\n".encode("utf-8"))
    return "sha256:" + digest.hexdigest()


def _tree_content_digest(root: Path, ignore=_ignore_python_caches) -> str:
    """Digest a small tree by content, for trees where a miss would be silent.

    Used for the trees the staging hooks themselves live in and for the sources
    copied in from TuyaOpen: they are kilobytes, so reading them costs nothing,
    and they have no metadata blind spot.
    """
    digest = hashlib.sha256()
    if not root.is_dir():
        digest.update(b"<absent>\n")
        return "sha256:" + digest.hexdigest()
    for relative, path in _walk(root, ignore):
        digest.update(f"{relative}\0".encode("utf-8"))
        try:
            digest.update(path.read_bytes())
        except OSError:
            digest.update(b"<unreadable>")
        digest.update(b"\n")
    return "sha256:" + digest.hexdigest()


def _loose_entries_digest(root: Path) -> str:
    """Digest a directory's own files, and the names of its subdirectories.

    ``stage_sdk_inputs`` copies a directory's loose files but links its
    subdirectories, so a change inside a linked subdirectory reaches the vendor
    make directly and can never go stale in the staging tree. Only the loose
    files can, so only they are covered; the subdirectory names are recorded so
    that adding or removing one is noticed.
    """
    digest = hashlib.sha256()
    if not root.is_dir():
        digest.update(b"<absent>\n")
        return "sha256:" + digest.hexdigest()
    for entry in sorted(root.iterdir(), key=lambda path: path.name):
        if entry.is_dir():
            digest.update(f"{entry.name}/\n".encode("utf-8"))
            continue
        try:
            info = entry.stat()
        except OSError:
            continue
        digest.update(f"{entry.name}\0{info.st_size}\0{info.st_mtime_ns}\n".encode("utf-8"))
    return "sha256:" + digest.hexdigest()


def _staged_vendor_fingerprint(vendor_root: Path, chip) -> str:
    """Fingerprint every vendor path the staging tree copies rather than links.

    ``stage_sdk_inputs`` links include_lib, lib, tools and the apps/common
    subdirectories, so the compiler reads those straight from the vendor
    checkout and they can never be stale in staging. What it copies -- the CPU
    profile, the demo profile, apps/common's loose files and its movable tree --
    can be, so that is what this covers. ``liba`` is linked too, but covering a
    linked path only ever costs an unnecessary rebuild, never correctness, and
    the CPU tree is small enough that separating it out would not pay.

    Everything here is covered by the same ignore rules the copy itself uses, so
    a file the copy would skip cannot invalidate the tree either.
    """
    parts = (
        _tree_metadata_digest(vendor_root / "cpu" / chip.cpu, _ignore_cpu_build_outputs),
        _tree_metadata_digest(
            vendor_root / "apps" / "common" / "movable", _ignore_demo_build_outputs
        ),
        _tree_metadata_digest(
            vendor_root / "apps" / "demo" / "demo_hello", _ignore_demo_build_outputs
        ),
        _loose_entries_digest(vendor_root / "apps" / "common"),
    )
    digest = hashlib.sha256()
    for part in parts:
        digest.update(part.encode("utf-8"))
        digest.update(b"\n")
    return "sha256:" + digest.hexdigest()


def staging_input_components(
    *,
    sdk_root: Path,
    tuyaopen_root: Path,
    platform_root: Path,
    chip,
    board_name: Optional[str],
    board_facts: str,
    features: str,
    audio_config_source: Optional[Path],
    header_dir: Optional[Path],
    tuya_lib_dir: Optional[Path],
    uart_log_port: int,
    uart_log_baudrate: int,
) -> dict[str, str]:
    """Name every input the staged bytes depend on, one digest each.

    Kept as named components rather than a single hash so a rebuild can say
    which input moved. The set is complete by construction: it covers each
    value ``create_staging_tree`` is handed, each file it reads to decide what
    to write, each tree it copies in, and the vendor trees it copies from.
    Everything else it reads -- the adapter include directories, the TuyaOpen
    sources CMake compiles -- is consumed by the compiler, not by staging, and
    the vendor make's own dependency files track those.
    """
    vendor_root = Path(sdk_root) / chip.sdk_source_relative
    return {
        "chip": chip.name,
        "sdk_root": _absolute(sdk_root),
        "tuyaopen_root": _absolute(tuyaopen_root),
        "platform_root": _absolute(platform_root),
        "header_dir": _absolute(header_dir),
        "tuya_lib_dir": _absolute(tuya_lib_dir),
        "uart": f"{uart_log_port}:{uart_log_baudrate}",
        "board": board_name or "",
        # The board's own facts, and the audio header the board brings. Both are
        # read to decide what to write into the staged board files.
        "board_facts": board_facts,
        "features": features,
        "audio_config": _file_digest(audio_config_source),
        # The staging hooks themselves. An edit to any of them can change the
        # staged bytes without changing any other input.
        "staging_scripts": _tree_content_digest(platform_root / "tools" / "jieli_build"),
        "build_example": _file_digest(platform_root / "build_example.py"),
        # Trees copied into staging, so their content is staged bytes.
        "platform_entry": _tree_content_digest(platform_root / "tuyaos" / "entry"),
        "platform_adapter": _tree_content_digest(platform_root / "tuyaos" / "tuyaos_adapter"),
        "tuyaopen_utilities": _tree_content_digest(
            tuyaopen_root / "tools" / "porting" / "adapter" / "utilities"
        ),
        # The vendor SDK, which the rest of the staging tree is copied from.
        "vendor": _staged_vendor_fingerprint(vendor_root, chip),
    }


def staging_input_digest(**components) -> str:
    """Combine named components into the digest the state file records."""
    digest = hashlib.sha256()
    for name in sorted(components):
        digest.update(f"{name}={components[name]}\n".encode("utf-8"))
    return "sha256:" + digest.hexdigest()


def read_staging_state(staging_root: Path) -> Optional[dict]:
    """Read a staging tree's recorded inputs, or None if it has none.

    A state file this code cannot read -- truncated, hand-edited, written by a
    different version -- is treated as no state at all, which forces a rebuild.
    """
    state_path = staging_root / STAGING_STATE_NAME
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(state, dict) or state.get("version") != STAGING_STATE_VERSION:
        return None
    return state


def staging_tree_is_current(staging_root: Path, digest: str) -> bool:
    """Whether the staging tree can be reused as it stands.

    The build root has to be there as well as the record: a tree someone
    partially deleted still carries its state file, and reusing it would fail
    somewhere further along instead of restaging.
    """
    if not (staging_root / "build").is_dir():
        return False
    state = read_staging_state(staging_root)
    return state is not None and state.get("digest") == digest


def write_staging_state(staging_root: Path, components: dict[str, str]) -> None:
    """Record what the staging tree was built from, once it is complete."""
    state = {
        "version": STAGING_STATE_VERSION,
        "digest": staging_input_digest(**components),
        "components": components,
    }
    (staging_root / STAGING_STATE_NAME).write_text(
        json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def read_adapter_sources(platform_root: Path, chip_name: str) -> list[str]:
    """Read the source manifest shared by CMake and the SDK Makefile bridge."""
    if chip_name not in JIELI_CHIPS:
        raise BuildError(f"unsupported Jieli chip '{chip_name}'")
    manifest = platform_root / "tuyaos" / "tuyaos_adapter" / "adapter_sources.txt"
    if not manifest.is_file():
        raise BuildError(f"Jieli adapter source manifest is missing: {manifest}")

    selected: list[str] = []
    seen: set[str] = set()
    for line_number, raw_line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        source = raw_line.partition("#")[0].strip().replace("\\", "/")
        if not source:
            continue
        path = Path(source)
        if path.is_absolute() or ".." in path.parts or not source.endswith(".c"):
            raise BuildError(f"invalid adapter source at {manifest}:{line_number}: {source}")
        if source in seen:
            raise BuildError(f"duplicate adapter source at {manifest}:{line_number}: {source}")
        seen.add(source)

        parts = path.parts
        if len(parts) >= 3 and parts[:2] == ("src", "chip"):
            if parts[2] not in ("common", chip_name):
                continue
        if not (manifest.parent / source).is_file():
            raise BuildError(f"adapter source does not exist at {manifest}:{line_number}: {source}")
        selected.append(source)

    if not selected:
        raise BuildError(f"Jieli adapter source manifest has no sources for {chip_name}: {manifest}")
    return selected


def _make_path(path: Path) -> str:
    """Render host paths in a form that survives Makefile shell expansion."""
    return path.as_posix()


def build_make_command(sdk_root: Path, tool_dir: Path, jobs: int = 1, chip=None) -> list[str]:
    if jobs < 1:
        raise ValueError("jobs must be at least 1")
    chip = chip or resolve_chip()
    board_dir = sdk_root / chip.sdk_source_relative / chip.board_build_relative
    elf_target = "../../../../../" + chip.elf_relative.as_posix()
    command = [
        "make",
        "-C",
        str(board_dir),
        f"TOOL_DIR={_make_path(tool_dir)}",
        f"-j{jobs}",
        "pre_build",
        elf_target,
    ]
    if os.name == "nt":
        command.insert(4, "LINK_AT=0")
        helper = Path(__file__).resolve().parent / "mkdir.py"
        command.insert(5, f'MKDIR="{Path(sys.executable).as_posix()}" "{helper.as_posix()}" -p')
    return command

def create_staging_tree(
    sdk_root: Path,
    staging_root: Path,
    tuyaopen_root: Path,
    header_dir: Optional[Path] = None,
    tuya_lib_dir: Optional[Path] = None,
    uart_log_port: int = 0,
    uart_log_baudrate: int = 115200,
    platform_root: Path = PLATFORM_ROOT,
    chip=None,
    facts: Optional[BoardFacts] = None,
    features: Optional[Iterable[str]] = None,
    tool_dir: Optional[Path] = None,
) -> Path:
    """Create a small overlay tree without modifying the vendor checkout.

    A tree whose inputs all still match the ones it was built from is returned
    untouched, so the vendor make inside it stays incremental. Anything else --
    no tree, no record, an unreadable record, or any input that moved -- restages
    from scratch.
    """
    chip = chip or resolve_chip()
    # Every board-level decision below reads this, never a per-board branch.
    # The caller reads the board's facts from the evaluated config, so a board
    # that does not declare them is a build error there, not a default here.
    if facts is None:
        raise BuildError("board facts are required to stage a Jieli build")
    if features is None:
        raise BuildError("the enabled feature set is required to stage a Jieli build")
    features = tuple(features)
    board_name = facts.board
    profile = AUDIO_PROFILES.get(chip.name)
    audio_config_source = (
        audio_board_config_path(tuyaopen_root, board_name, profile)
        if profile is not None and profile.board_declarations
        else None
    )
    components = staging_input_components(
        sdk_root=sdk_root,
        tuyaopen_root=tuyaopen_root,
        platform_root=platform_root,
        chip=chip,
        board_name=board_name,
        board_facts=board_facts_digest(facts),
        features=','.join(features),
        audio_config_source=audio_config_source,
        header_dir=header_dir,
        tuya_lib_dir=tuya_lib_dir,
        uart_log_port=uart_log_port,
        uart_log_baudrate=uart_log_baudrate,
    )
    if staging_tree_is_current(staging_root, staging_input_digest(**components)):
        return staging_root / "build"

    if staging_root.exists():
        shutil.rmtree(staging_root)

    build_root = staging_root / "build"
    vendor_root = sdk_root / chip.sdk_source_relative
    stage_sdk_inputs(sdk_root, build_root, chip)
    source_overlay_root = build_root / chip.sdk_source_relative
    stage_audio_sdk_sources(source_overlay_root, vendor_root, chip.name)
    apps_root = source_overlay_root / "apps"
    board_file = source_overlay_root / chip.board_build_relative / "board.c"
    app_config_file = source_overlay_root / "apps/demo/demo_hello/include/app_config.h"
    # board_demo.h is the wl83 demo profile's board header; the wl82 demo has
    # none. Which SDK file carries which setting is a chip-layout fact.
    board_header = source_overlay_root / "apps/demo/demo_hello/board/wl83/board_demo.h"
    if facts.display is not None and chip.name != "wl83":
        raise BuildError(
            f"{facts.board} declares a display panel, but the platform stages the "
            f"MIPI-DSI panel path only into the wl83 SDK"
        )
    if chip.name == "wl82":
        configure_board_memory(app_config_file, facts)
        configure_log_uart_block(board_file, uart_log_port, uart_log_baudrate, facts)
    else:
        configure_log_uart_baudrate(board_header, uart_log_port, uart_log_baudrate, facts)
    # Only when the product asked for the power manager: without it the board
    # profile keeps the vendor's sleep-closed macro, and the firmware behaves
    # exactly as it did before this feature existed.
    #
    # The STA power mode is checked against the staged wifi_conf.c, because
    # that is the file that turns these macros into PSMode= - a sleep mask
    # alone leaves the station in CAM.
    if "power" in features:
        configure_lowpower(
            chip.name,
            app_config_file,
            board_header,
            wifi_conf=source_overlay_root / "apps/common/net/wifi_conf.c",
            compiler=resolve_tool_path(tool_dir, "clang") if tool_dir is not None else None,
            sdk_root=sdk_root,
        )
    if facts.service_uart is not None:
        configure_service_uart(board_file, facts)
    reference_board_file = (
        vendor_root / "apps/demo/demo_wifi_ext/board/wl83/board.c"
        if chip.name == "wl83"
        else None
    )
    configure_full_stack_board(board_file, reference_board_file)
    configure_full_stack_app_config(app_config_file)
    configure_usb_download_board(
        board_file, "TCFG_USB_DEVICE" if chip.name == "wl83" else "0x03",
    )
    if chip.name == "wl83":
        configure_usb_download_board_header(board_header, facts)
        configure_board_memory(
            source_overlay_root / "apps/demo/demo_hello/board/wl83/chip_cfg.h",
            facts,
            ddr_config_file=board_header,
        )
        if facts.display is not None:
            configure_lcd_board_header(board_header, facts)
            configure_lcd_app_config(app_config_file)
            configure_lcd_board(board_file)
        if "camera" in features:
            configure_camera_app_config(app_config_file)
            configure_camera_board_header(board_header, dvp_enabled=True, csi2_enabled=True)
            configure_camera_board(board_file, dvp_enabled=True, csi2_enabled=True)
    else:
        configure_usb_download_app_config(app_config_file)
    if profile is not None and profile.board_declarations:
        configure_audio_board(board_file, audio_config_source, profile)
    shutil.copytree(
        platform_root / "tuyaos" / "entry",
        source_overlay_root / "tuyaos" / "entry",
    )
    shutil.copytree(
        platform_root / "tuyaos/tuyaos_adapter",
        source_overlay_root / "tuyaos_adapter",
    )
    # JIELI vendors the utilities module inside the adapter, so the staged
    # vendor build copies this platform's own tree rather than the shared
    # tools/porting one.
    utilities_root = platform_root / "tuyaos" / "tuyaos_adapter" / "include" / "utilities"
    if utilities_root.is_dir():
        shutil.copytree(utilities_root, source_overlay_root / "tuya_utilities")

    makefile = source_overlay_root / chip.board_build_relative / "Makefile"
    content = makefile.read_text(encoding="utf-8")
    vendor_main = "../../../../../apps/demo/demo_hello/app_main.c"
    if vendor_main not in content:
        raise BuildError(f"Jieli demo Makefile has no app_main source: {makefile}")
    content = content.replace(vendor_main, "../../../../../tuyaos/entry/jieli_app_entry.c")
    content = drop_vendor_example_sources(content, chip.demo_lists_example_sources)
    extra_sources_list = [
        "../../../../../tuyaos_adapter/" + source
        for source in read_adapter_sources(platform_root, chip.name)
    ]
    extra_sources_list.extend(
        [
            "../../../../../tuya_utilities/src/tuya_hashmap.c",
            "../../../../../tuya_utilities/src/tuya_list.c",
            "../../../../../tuya_utilities/src/tuya_mem_heap.c",
            "../../../../../tuya_utilities/src/tuya_queue.c",
            "../../../../../tuya_utilities/src/tuya_ringbuf.c",
            "../../../../../tuya_utilities/src/tuya_smartpointer.c",
            "../../../../../tuya_utilities/src/tuya_tools.c",
        ]
    )
    extra_sources_list.extend(usb_device_sources())
    extra_sources_list.append("../../../../../tuyaos/entry/jieli_usb_download.c")
    # Every feature-owned source comes from the enabled set, display included:
    # the board declaring a panel is what puts "display" in that set, so listing
    # the display sources separately here would emit them twice and the linker
    # would reject the duplicate objects.
    extra_sources_list.extend(
        SDK_SOURCE_PREFIX + source
        for source in feature_sources(features)
    )
    source_lines = ["c_SRC_FILES += \\"]
    for index, source in enumerate(extra_sources_list):
        suffix = " \\" if index + 1 < len(extra_sources_list) else ""
        source_lines.append(f"    {source}{suffix}")
    extra_sources = "\n".join(source_lines) + "\n"
    marker = "c_OBJS    :="
    if marker not in content:
        raise BuildError(f"AC79 demo Makefile has no object list marker: {makefile}")
    content = content.replace(marker, extra_sources + "\n" + marker, 1)
    if chip.name == "wl82":
        # The AC791 Wi-Fi/BLE SDK libraries use the SDRAM linker layout, so
        # remove the demo template's no-SDRAM define in staging.
        content = content.replace("\t-DCONFIG_NO_SDRAM_ENABLE \\\n", "", 1)
    content += "\n"
    content += "INCLUDES += \\\n"
    content += "    -I../../../../../tuyaos_adapter/include \\\n"
    adapter_include = source_overlay_root / "tuyaos_adapter" / "include"
    for include_dir in sorted(path for path in adapter_include.iterdir() if path.is_dir()):
        content += f"    -I../../../../../tuyaos_adapter/include/{include_dir.name} \\\n"
    # tkl_init.h follows the shared TuyaOpen layout under init/include.
    content += "    -I../../../../../tuyaos_adapter/include/init/include \\\n"
    tuyaopen_root_make = _make_path(tuyaopen_root)
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/system \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/uart \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/init/include \\\n"
    content += f"    -I{_make_path(platform_root)}/tuyaos/tuyaos_adapter/include/utilities/include \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/flash \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/network \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/wifi \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/bluetooth \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/timer \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/security \\\n"
    # tkl_adc.h lives beside the other TKL domain headers, not under any
    # per-chip profile, so every chip's staged Makefile needs it.
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/adc \\\n"
    # tkl_pinmux.c compiles for both chips - it refuses everything on wl82 -
    # so its contract has to be on the include path either way.
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/pinmux \\\n"
    # tkl_i2c.c compiles on both chips; its contract is likewise not under a
    # per-chip profile.
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/i2c \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/pwm \\\n"
    content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/spi \\\n"
    if chip.name == "wl83":
        # tkl_gpio.c includes the shared GPIO contract from this domain. The
        # MIPI-DSI contract is not here: it ships with this platform under
        # tuyaos_adapter/include/mipi_dsi, which the loop above already adds.
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/gpio \\\n"
    if chip.name in AUDIO_PROFILES:
        # The audio adapter's public TKL headers (tkl_audio.h, tkl_vad.h,
        # tkl_kws.h) exist only under these TuyaOpen adapter domains.
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/media \\\n"
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/vad \\\n"
        content += f"    -I{tuyaopen_root_make}/tools/porting/adapter/kws \\\n"
    content += "    -I../../../../../apps/common/include \\\n"
    content += "    -I../../../../../apps/common/config/include \\\n"
    for include in usb_include_lines():
        content += f"    {include} \\\n"
    if facts.display is not None:
        for include in lcd_include_lines(chip.cpu):
            content += f"    {include} \\\n"
    content += "    -I../../../../../include_lib/btstack \\\n"
    content += "    -I../../../../../include_lib/btstack/le \\\n"
    content += "    -I../../../../../include_lib/btctrler \\\n"
    content += f"    -I../../../../../include_lib/btctrler/port/{chip.cpu} \\\n"
    content += f"    -I{tuyaopen_root_make}/src/common/include\n"
    content += "INCLUDES += \\\n"
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
    selected_chip_define = "JIELI_SELECTED_CHIP_WL83" if chip.name == "wl83" else "JIELI_SELECTED_CHIP_WL82"
    content += f"DEFINES += -D{selected_chip_define}=1\n"
    content += "CFLAGS += -include stdbool.h -DBOOL_DEFINE_CONFLICT\n"
    content += "DEFINES += " + " ".join(
        "-D" + define for define in feature_defines(features)
    ) + "\n"
    if chip.name in AUDIO_PROFILES:
        # The vendor media runtime and the staged app entry both key off this
        # switch; without it tkl_jieli_audio_prepare compiles out entirely.
        content += "DEFINES += -DCONFIG_MEDIA_ENABLE -DCONFIG_AUDIO_ENABLE -DCONFIG_AUDIO_ONCHIP\n"
    if tuya_lib_dir is None:
        raise BuildError("TuyaOpen library directory is required for the Jieli image")
    content += "LFLAGS += \\\n"
    # The vendor LTO used-symbol list does not reference wlc_main when
    # Wi-Fi is entered through the Tuya TKL layer. Force-retain the
    # vendor entry point so cpu.a[wlc.c.o] is extracted by the linker.
    content += "    -u wlc_main \\\n"
    tuya_lib_dir_make = _make_path(tuya_lib_dir)
    content += f"    --start-group {tuya_lib_dir_make}/libtuyaapp.a {tuya_lib_dir_make}/libtuyaos.a \\\n"
    # AC791's vendor Makefile links cpu.a/system.a before this Tuya
    # library group. BLE/Wi-Fi archives introduce provider references
    # later, so repeat those archives inside the group for a rescan.
    liba = f"../../../../../cpu/{chip.cpu}/liba"
    # The feature switches do not gate the link yet. libcrypto_mbedtls.a (wl83
    # only) and video.a (display only) are the two chip/board-conditional
    # additions; feature_map.py owns both facts, so the chip and display cases
    # are stated once instead of branched here.
    for library in link_libraries(chip.name, features):
        content += f"    {liba}/{library} \\\n"
    content += "    --end-group\n"
    if header_dir is not None:
        content += f"INCLUDES += -I{_make_path(header_dir)}\n"
    makefile.write_text(content, encoding="utf-8")

    if profile is not None:
        # The staged entry replaced app_main.c in the Makefile, and it owns
        # the task table the audio tasks must be registered in.
        apply_audio_profile(
            makefile,
            source_overlay_root / "tuyaos" / "entry" / "jieli_app_entry.c",
            chip.name,
        )
    # Recorded last, so a staging run that failed partway leaves no claim that
    # the tree is complete.
    write_staging_state(staging_root, components)
    return build_root
