#include "tkl_init.h"
#include "tkl_memory.h"
#include "tkl_ota.h"
#include "tuya_error_code.h"
#include "asm/cpu.h"
#include "system/malloc.h"
#include "system/sys_time.h"

#include <string.h>
#include <stdlib.h>

static TKL_ABILITY_T s_jieli_ability = {
    .uart = TRUE,
    .wifi = TRUE,
    .bt = TRUE,
};

/* Declared here rather than in a private header, matching how tkl_vad.c
 * declares tkl_jieli_vad_feed_capture. */
#if defined(CONFIG_MEDIA_ENABLE)
OPERATE_RET tkl_jieli_audio_prepare(void);
#endif

OPERATE_RET tkl_init(void)
{
#if defined(CONFIG_MEDIA_ENABLE)
    /* Vendor audio_server bring-up. The board's own audio registration happens
     * separately, through board_register_hardware() -> tdd_audio_register(). */
    return tkl_jieli_audio_prepare();
#else
    return OPRT_OK;
#endif
}

char *tkl_get_version(void)
{
    return "jieli-wl82-tkl-0.1.0";
}

TKL_ABILITY_T *tkl_get_ability(void)
{
    return &s_jieli_ability;
}

void *tkl_system_malloc(size_t size) { return malloc(size); }
void tkl_system_free(void *ptr) { free(ptr); }
void *tkl_system_memset(void *src, int ch, const size_t n) { return memset(src, ch, n); }
void *tkl_system_memcpy(void *src, const void *dst, const size_t n) { return memcpy(src, dst, n); }
void *tkl_system_calloc(size_t nitems, size_t size) { return calloc(nitems, size); }
void *tkl_system_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
int tkl_system_memcmp(const void *str1, const void *str2, size_t n) { return memcmp(str1, str2, n); }

/* PSRAM aliases.
 *
 * JIELI defines no ENABLE_EXT_RAM, so tal_memory.h's Malloc() resolves to
 * tal_malloc() and the AI/audio path never asks for PSRAM. Direct callers do,
 * though - tal_psram_* and the shipped audio examples (output_speaker,
 * output_sdcard) call tkl_system_psram_* unconditionally, and without these
 * the whole image fails to link on JIELI with "undefined reference to
 * tkl_system_psram_malloc".
 *
 * The vendor exposes no PSRAM allocator on wl82 or wl83 (wl82 has no psram_api
 * at all), so route to the ordinary heap. That is the same memory the non-PSRAM
 * path uses, which is the honest answer on a platform with no external RAM -
 * callers asking for "large" buffers get the system heap and a NULL on failure,
 * exactly as before. */
void *tkl_system_psram_malloc(size_t size) { return malloc(size); }
void tkl_system_psram_free(void *ptr) { free(ptr); }
void *tkl_system_psram_calloc(size_t nitems, size_t size) { return calloc(nitems, size); }
void *tkl_system_psram_realloc(void *ptr, size_t size) { return realloc(ptr, size); }

int tkl_system_get_free_heap_size(void)
{
    /* The vendor wl82 release does not export heap statistics to apps. */
    return 0;
}

int tkl_system_psram_get_free_heap_size(void)
{
    /* No separate PSRAM pool to report - see the aliases above. */
    return 0;
}

SYS_TICK_T tkl_system_get_tick_count(void)
{
    return (SYS_TICK_T)timer_get_ms();
}

SYS_TIME_T tkl_system_get_millisecond(void)
{
    return (SYS_TIME_T)timer_get_ms();
}

int tkl_system_get_random(uint32_t range)
{
    uint32_t value = (uint32_t)rand32();
    return range ? (int)(value % range) : (int)(value & 0x7fffffffU);
}

void tkl_system_reset(void)
{
    system_reset();
}

TUYA_RESET_REASON_E tkl_system_get_reset_reason(char **describe)
{
    if (describe) {
        *describe = "unknown";
    }
    return TUYA_RESET_REASON_UNKNOWN;
}

OPERATE_RET tkl_system_get_cpu_info(TUYA_CPU_INFO_T **cpu_ary, int *cpu_cnt)
{
    if (!cpu_ary || !cpu_cnt) {
        return OPRT_INVALID_PARM;
    }
    *cpu_ary = NULL;
    *cpu_cnt = 0;
    return OPRT_OK;
}
