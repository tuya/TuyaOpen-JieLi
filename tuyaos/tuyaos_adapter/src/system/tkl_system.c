#include "tkl_init.h"
#include "tkl_memory.h"
#include "tkl_ota.h"
#include "tuya_error_code.h"
#include "asm/cpu.h"
#include "system/malloc.h"
#include "system/sys_time.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

extern u32 get_ram_free_heap_size(void);
extern int get_malloc_remain_heap_size(void);
extern u32 RAM_HEAP_BEGIN;
extern u32 RAM_HEAP_END;

/* Shared TuyaOpen code occasionally frees a PSRAM/psram-API allocation through
 * tal_free() or vice versa. Route each free to the heap that owns the pointer
 * instead of asserting in the vendor ram_free() range check. */
static int __jieli_is_ram_ptr(const void *ptr)
{
    uintptr_t address = (uintptr_t)ptr;
    return address >= (uintptr_t)&RAM_HEAP_BEGIN && address <= (uintptr_t)&RAM_HEAP_END;
}

/* wl83's internal heap is only about 90 KB and the vendor audio/wifi code keeps
 * allocating from it directly, while TuyaOpen asks for 16-20 KB buffers (AI
 * ringbufs) that no longer fit after provisioning. Keep a reserve for the
 * vendor and serve the oversized requests from SDRAM; the frees above already
 * dispatch on the address, so both heaps stay balanced. */
#define JIELI_RAM_RESERVE (16U * 1024U)

static void *__jieli_malloc(size_t size)
{
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    u32 free_size = get_ram_free_heap_size();

    if (free_size > size && (free_size - size) >= JIELI_RAM_RESERVE) {
        return ram_malloc(size);
    }

    void *ptr = malloc(size);
    if (ptr != NULL) {
        return ptr;
    }
#endif
    return ram_malloc(size);
}

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

void *tkl_system_malloc(size_t size) { return __jieli_malloc(size); }

void tkl_system_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (__jieli_is_ram_ptr(ptr)) {
        ram_free(ptr);
    } else {
        free(ptr);
    }
}

void *tkl_system_memset(void *src, int ch, const size_t n) { return memset(src, ch, n); }
void *tkl_system_memcpy(void *src, const void *dst, const size_t n) { return memcpy(src, dst, n); }

void *tkl_system_calloc(size_t nitems, size_t size)
{
    size_t total = nitems * size;
    void *ptr;

    if (size != 0 && total / size != nitems) {
        return NULL;
    }
    ptr = __jieli_malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *tkl_system_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return __jieli_malloc(size);
    }
    return __jieli_is_ram_ptr(ptr) ? ram_realloc(ptr, size) : realloc(ptr, size);
}

int tkl_system_memcmp(const void *str1, const void *str2, size_t n) { return memcmp(str1, str2, n); }

void *tkl_system_psram_malloc(size_t size) { return malloc(size); }

void tkl_system_psram_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (__jieli_is_ram_ptr(ptr)) {
        ram_free(ptr);
    } else {
        free(ptr);
    }
}

void *tkl_system_psram_calloc(size_t nitems, size_t size) { return calloc(nitems, size); }

void *tkl_system_psram_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return malloc(size);
    }
    return __jieli_is_ram_ptr(ptr) ? ram_realloc(ptr, size) : realloc(ptr, size);
}

int tkl_system_get_free_heap_size(void) { return (int)get_ram_free_heap_size(); }

int tkl_system_psram_get_free_heap_size(void) { return get_malloc_remain_heap_size(); }

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

/* The camera TDL guards its frame queues with the TKL critical section. The
 * Jieli port declared these in tkl_system.h but never implemented them, so a
 * camera build failed at link time. local_irq_disable() is the vendor's
 * PI32V2 interrupt gate; the return value remembers whether interrupts were
 * already off so nested sections do not re-enable them early. */
uint32_t tkl_system_enter_critical(void)
{
    uint32_t was_disabled = (uint32_t)cpu_irq_disabled();
    local_irq_disable();
    return was_disabled;
}

void tkl_system_exit_critical(uint32_t irq_mask)
{
    if (!irq_mask) {
        local_irq_enable();
    }
}
