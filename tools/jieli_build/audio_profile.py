"""Per-chip vendor build inputs the audio adapter needs.

JieLi's per-app Makefiles list their sources explicitly, so enabling the audio
adapter means naming the vendor runtime it calls. Keep that knowledge as data
so adding a chip or a feature is a table edit, not new control flow.
"""

from __future__ import annotations

import re
from pathlib import Path

from .errors import BuildError


class AudioProfile:
    def __init__(self, vendor_sources, includes, per_file_flags, link_libs, app_tasks):
        self.vendor_sources = tuple(vendor_sources)
        self.includes = tuple(includes)
        self.per_file_flags = tuple(per_file_flags)
        self.link_libs = tuple(link_libs)
        self.app_tasks = tuple(app_tasks)


# The staged board's own sdk_config.h / jlstream_node_cfg.h / app_config.h are
# not self-contained, so every object that pulls in a vendor media header needs
# them force-included. Applied per object rather than globally so the rest of
# the tree keeps its own configuration.
_WL83_MEDIA_CFLAGS = (
    "-include ../../../../../apps/wifi_camera/board/wl83/sdk_config.h "
    "-include ../../../../../apps/wifi_camera/board/wl83/jlstream_node_cfg.h "
    "-DBOARD_CONFIG_H "
    "-include ../../../../../apps/wifi_camera/include/app_config.h "
    "-include ../../../../../audio/cpu/wl83/audio_config_def.h "
    "-include ../../../../../apps/demo/demo_hello/board/wl83/tuya_board_audio_config.h"
)

# Objects compiled from the vendor audio runtime, in the order the vendor's own
# Makefiles list them. Kept explicit because the vendor ships no dependency
# manifest for "what does the audio runtime need".
_WL83_AUDIO_OBJECTS = (
    "objs/audio/log_config/lib_media_config.c.o",
    "objs/audio/common/audio_general.c.o",
    "objs/audio/CVP/audio_cvp.c.o",
    "objs/audio/common/audio_dvol.c.o",
    "objs/audio/cpu/wl83/audio_setup.c.o",
    "objs/audio/common/audio_node_config.c.o",
    "objs/audio/common/audio_volume_mixer.c.o",
    "objs/audio/common/common_func.c.o",
    "objs/audio/cpu/wl83/audio_config.c.o",
    "objs/audio/framework/plugs/source/adc_file.c.o",
    "objs/audio/framework/plugs/source/multi_ch_adc_file.c.o",
    "objs/audio/framework/nodes/volume_node.c.o",
    "objs/audio/interface/player/a2dp_player.c.o",
    "objs/audio/interface/player/esco_player.c.o",
)

AUDIO_PROFILES = {
    "wl82": AudioProfile(
        vendor_sources=("apps/common/audio_music/audio_config.c",),
        includes=("include_lib/media",),
        per_file_flags=(),
        link_libs=("audio_server.a", "media_app.a"),
        app_tasks=(
            ("audio_server", '    {"audio_server", 16, 512, 64},\n'),
            ("audio_mix", '    {"audio_mix", 28, 512, 0},\n'),
            ("audio_encoder", '    {"audio_encoder", 12, 384, 64},\n'),
        ),
    ),
    "wl83": AudioProfile(
        vendor_sources=(
            "audio/log_config/lib_media_config.c",
            "audio/CVP/audio_cvp.c",
            "audio/common/audio_dvol.c",
            "audio/common/audio_general.c",
            "audio/common/audio_node_config.c",
            "audio/common/audio_volume_mixer.c",
            "audio/common/common_func.c",
            "audio/cpu/wl83/audio_config.c",
            "audio/cpu/wl83/audio_setup.c",
            "audio/framework/plugs/source/adc_file.c",
            "audio/framework/plugs/source/multi_ch_adc_file.c",
            "audio/framework/nodes/volume_node.c",
            "audio/interface/player/a2dp_player.c",
            "audio/interface/player/esco_player.c",
        ),
        includes=(
            "include_lib/media",
            "include_lib/media/cpu/wl83",
            "include_lib/media/cpu/wl83/asm",
            "include_lib/media/framework/include",
            "include_lib/media/cvp",
            "audio/cpu/wl83",
            "audio/CVP",
            "audio",
            "audio/common",
            "audio/effect",
            "audio/effect/spatial_effect",
            "audio/framework/include",
            "audio/interface",
            "audio/interface/includes",
            "apps/wifi_camera/include",
            "apps/wifi_camera/board/wl83",
        ),
        per_file_flags=tuple((obj, _WL83_MEDIA_CFLAGS) for obj in _WL83_AUDIO_OBJECTS)
        + (
            # Our adapter reaches the same vendor media headers but not
            # jlstream_node_cfg.h, so it takes the narrower flag set.
            (
                "objs/tuyaos_adapter/src/driver/tkl_audio.c.o",
                "-include ../../../../../apps/demo/demo_hello/board/wl83/tuya_board_audio_config.h",
            ),
        ),
        link_libs=("media.a", "stream_media_server.a", "fs.a"),
        app_tasks=(
            ("audio_server", '    {"audio_server", 16, 512, 64},\n'),
            ("audio_mix", '    {"audio_mix", 28, 512, 0},\n'),
            ("audio_encoder", '    {"audio_encoder", 12, 384, 64},\n'),
            ("tuya_audio_capture", '    {"tuya_audio_capture", 12, 768, 64},\n'),
        ),
    ),
}


def _profile_for(chip_name: str) -> AudioProfile:
    if chip_name not in AUDIO_PROFILES:
        raise BuildError(f"audio build is unsupported for chip '{chip_name}'")
    return AUDIO_PROFILES[chip_name]


def missing_audio_includes(makefile_text: str, chip_name: str) -> tuple[str, ...]:
    """Report the profile's -I paths a staged Makefile does not carry yet."""
    profile = _profile_for(chip_name)
    return tuple(
        f"-I../../../../../{include}"
        for include in profile.includes
        if f"-I../../../../../{include}" not in makefile_text
    )


def apply_audio_profile(makefile: Path, app_main: Path, chip_name: str) -> None:
    """Add the chip's audio vendor inputs to a staged vendor tree.

    Touches the five places the vendor build hardcodes: the source list, the
    include list, per-object config flags, the link group, and the app task
    table. Idempotent - staging trees survive between incremental builds.
    """
    profile = _profile_for(chip_name)

    content = makefile.read_text(encoding="utf-8")
    marker = "c_OBJS    :="
    if marker not in content:
        raise BuildError(f"Jieli Makefile has no object list marker: {makefile}")

    for source in reversed(profile.vendor_sources):
        entry = f"c_SRC_FILES += ../../../../../{source}\n"
        if entry not in content:
            content = content.replace(marker, entry + marker, 1)

    missing_includes = missing_audio_includes(content, chip_name)
    if missing_includes:
        content += "\nINCLUDES += " + " ".join(missing_includes) + "\n"

    for obj, flags in profile.per_file_flags:
        entry = f"{obj}: CFLAGS += {flags}\n"
        if entry not in content:
            content += entry

    end_groups = list(re.finditer(r"^\s*--end-group\s*$", content, re.MULTILINE))
    if not end_groups:
        raise BuildError(f"Jieli Makefile has no full-stack library group: {makefile}")
    end_group = end_groups[-1]
    start_groups = list(re.finditer(r"--start-group", content[: end_group.start()]))
    if not start_groups:
        raise BuildError(f"Jieli Makefile has no library group start: {makefile}")
    group_start = start_groups[-1].end()
    group_content = content[group_start:end_group.start()]
    missing_libs = [name for name in profile.link_libs
                    if f"/liba/{name}" not in group_content]
    if missing_libs:
        entries = "".join(
            f"    ../../../../../cpu/{chip_name}/liba/{name} \\\n" for name in missing_libs
        )
        content = content[: end_group.start()] + entries + content[end_group.start():]

    makefile.write_text(content, encoding="utf-8")

    app = app_main.read_text(encoding="utf-8")
    table_start = app.index("const struct task_info task_info_table[]")
    table_end = app.find("};", table_start)
    if table_end < 0:
        raise BuildError(f"Jieli app entry has no task table: {app_main}")
    table = app[table_start:table_end]
    missing_tasks = [row for name, row in profile.app_tasks if f'"{name}"' not in table]
    if missing_tasks:
        sentinel = re.search(r"^\s*\{\s*0\s*,\s*0\s*,\s*0\s*,\s*0\s*\}\s*,?\s*$",
                             table, re.MULTILINE)
        if sentinel is None:
            raise BuildError(f"Jieli app task table has no sentinel row: {app_main}")
        insert_at = table_start + sentinel.start()
        app = app[:insert_at] + "".join(missing_tasks) + app[insert_at:]
        app_main.write_text(app, encoding="utf-8")
