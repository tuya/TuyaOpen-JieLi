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
    def __init__(self, vendor_sources, includes, per_file_flags, link_libs, app_tasks,
                 board_declarations="", board_early_init_pre="", board_early_init_post="",
                 board_init="", registers_audio_device=False,
                 staged_vendor_trees=(), staged_vendor_files=(), audio_board_name=""):
        self.vendor_sources = tuple(vendor_sources)
        self.includes = tuple(includes)
        self.per_file_flags = tuple(per_file_flags)
        self.link_libs = tuple(link_libs)
        self.app_tasks = tuple(app_tasks)
        # Board-file patch inputs consumed by board_config.configure_audio_board:
        # the DAC/ADC platform structs, the PA-mute/VCM bring-up sequencing
        # around devices_init() and board_init(), and whether the vendor
        # audio_server device must appear in REGISTER_DEVICES(device_table).
        self.board_declarations = board_declarations
        self.board_early_init_pre = board_early_init_pre
        self.board_early_init_post = board_early_init_post
        self.board_init = board_init
        self.registers_audio_device = registers_audio_device
        # Vendor SDK-root directories linked read-only into the staged root
        # because the minimal staging does not copy them, plus the files that
        # must exist in the vendor checkout for those links to be usable.
        self.staged_vendor_trees = tuple(staged_vendor_trees)
        self.staged_vendor_files = tuple(staged_vendor_files)
        # TuyaOpen board directory (under boards/JIELI) whose audio_config.h
        # defines the JIELI_AUDIO_* macros the board patch and per-object
        # flags consume.
        self.audio_board_name = audio_board_name


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

# Per-chip audio platform data for the staged board.c, transcribed from the
# vendor board profiles. JIELI_AUDIO_* macros come from the TuyaOpen board's
# audio_config.h, staged next to board.c as tuya_board_audio_config.h.
_WL82_BOARD_DECLARATIONS = '''
/* TUYAOPEN_JIELI_AUDIO_BOARD_CONFIG */
static const struct dac_platform_data tuya_audio_dac_data = {
    .pa_auto_mute = 0,
    .pa_mute_port = JIELI_AUDIO_PA_MUTE_PORT,
    .pa_mute_value = JIELI_AUDIO_PA_MUTE_LEVEL,
    .differ_output = JIELI_AUDIO_DAC_DIFFER_OUTPUT,
    .hw_channel = JIELI_AUDIO_DAC_HW_CHANNEL,
    .ch_num = JIELI_AUDIO_DAC_CHANNEL_COUNT,
    .vcm_init_delay_ms = JIELI_AUDIO_DAC_VCM_INIT_DELAY_MS,
};
static const struct adc_platform_data tuya_audio_adc_data = {
    .mic_channel = JIELI_AUDIO_MIC_CHANNEL,
    .mic_ch_num = JIELI_AUDIO_MIC_CHANNEL_COUNT,
    /* MIC bias remains at the vendor default; no board-confirmed override. */
};
static const struct audio_pf_data tuya_audio_pf_data = {
    .adc_pf_data = &tuya_audio_adc_data,
    .dac_pf_data = &tuya_audio_dac_data,
};
static const struct audio_platform_data tuya_audio_data = {
    .private_data = (void *)&tuya_audio_pf_data,
};
'''

_WL83_BOARD_DECLARATIONS = '''
/* TUYAOPEN_JIELI_AUDIO_BOARD_CONFIG */
static const struct dac_platform_data tuya_audio_dac_data = {
    .pa_auto_mute = 0,
    .pa_mute_port = JIELI_AUDIO_PA_MUTE_PORT,
    .pa_mute_value = JIELI_AUDIO_PA_MUTE_LEVEL,
    .differ_output = JIELI_AUDIO_DAC_DIFFER_OUTPUT,
    .hw_channel = JIELI_AUDIO_DAC_HW_CHANNEL,
    .ch_num = JIELI_AUDIO_DAC_CHANNEL_COUNT,
    .vcm_init_delay_ms = 1000,
};
static const struct adc_platform_data tuya_audio_adc_data = {
    .mic_port = JIELI_AUDIO_MIC_PORTS,
    .mic_ch_num = JIELI_AUDIO_MIC_CHANNEL_COUNT,
    .all_channel_open = JIELI_AUDIO_ADC_ALL_CHANNEL_OPEN,
    /* Board routing selects the onboard MIC1/MIC2 input pairs. */
};
static const struct audio_pf_data tuya_audio_pf_data = {
    .adc_pf_data = &tuya_audio_adc_data,
    .dac_pf_data = &tuya_audio_dac_data,
};
static const struct audio_platform_data tuya_audio_data = {
    .private_data = (void *)&tuya_audio_pf_data,
};
'''

# wl82 drives the PA mute and starts the DAC before devices_init(), then
# releases the PA after the VCM settle delay; the audio_server device must be
# registered for the vendor runtime to find it.
_WL82_BOARD_EARLY_INIT_PRE = (
    "    gpio_direction_output(JIELI_AUDIO_PA_MUTE_PORT, JIELI_AUDIO_PA_MUTE_LEVEL);\n"
    "    dac_early_init(0, JIELI_AUDIO_DAC_HW_CHANNEL, JIELI_AUDIO_DAC_VCM_INIT_DELAY_MS);\n"
)
_WL82_BOARD_EARLY_INIT_POST = (
    "\n    msleep(JIELI_AUDIO_PA_RELEASE_DELAY_MS);\n"
    "    gpio_direction_output(JIELI_AUDIO_PA_MUTE_PORT, !JIELI_AUDIO_PA_MUTE_LEVEL);"
)

# wl83 mutes the PA before devices_init() and releases it at the top of
# board_init(); the SDK audio path here opens no registered audio device.
_WL83_BOARD_EARLY_INIT_PRE = (
    "    gpio_direction_output(JIELI_AUDIO_PA_MUTE_PORT, JIELI_AUDIO_PA_MUTE_LEVEL);\n"
)
_WL83_BOARD_INIT = (
    "\n    dac_early_init(JIELI_AUDIO_DAC_HW_CHANNEL, JIELI_AUDIO_DAC_VCM_CAP_ENABLE);\n"
    "    msleep(JIELI_AUDIO_PA_RELEASE_DELAY_MS);\n"
    "    gpio_direction_output(JIELI_AUDIO_PA_MUTE_PORT, !JIELI_AUDIO_PA_MUTE_LEVEL);\n"
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
        board_declarations=_WL82_BOARD_DECLARATIONS,
        board_early_init_pre=_WL82_BOARD_EARLY_INIT_PRE,
        board_early_init_post=_WL82_BOARD_EARLY_INIT_POST,
        registers_audio_device=True,
        audio_board_name="AC79_DevKitBoard",
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
        board_declarations=_WL83_BOARD_DECLARATIONS,
        board_early_init_pre=_WL83_BOARD_EARLY_INIT_PRE,
        board_init=_WL83_BOARD_INIT,
        staged_vendor_trees=("audio", "apps/wifi_camera"),
        staged_vendor_files=(
            "audio/log_config/lib_media_config.c",
            "apps/wifi_camera/board/wl83/sdk_config.h",
            "apps/wifi_camera/board/wl83/jlstream_node_cfg.h",
        ),
        audio_board_name="AC792N_Develop_Board",
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
