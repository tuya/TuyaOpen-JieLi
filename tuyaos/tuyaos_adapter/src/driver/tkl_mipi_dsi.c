#include "tkl_mipi_dsi.h"
#include "tuya_error_code.h"

#include <stdint.h>

#if defined(JIELI_SELECTED_CHIP_WL83)

#include "device.h"
#include "lcd_driver.h"
#include "lcd_config.h"
#include "asm/cache.h"

/* The vendor LCD stack is a framebuffer pump, not an allocator: the panel has no
 * GRAM, so the caller owns the buffers and hands one to
 * IOCTL_LCD_RGB_START_DISPLAY for the controller to scan out. This backend
 * therefore stores the address it is given rather than copying anything into a
 * buffer of its own - the application's frame buffer is the one the controller
 * reads.
 *
 * The vendor names the device "lcd" and its IOCTLs IOCTL_LCD_RGB_* even for a
 * MIPI panel; it reuses the RGB framebuffer plumbing, and the backend is written
 * against that structure rather than a DSI-specific one.
 */
static void *s_lcd_dev;
static TUYA_MIPI_DSI_BASE_CFG_T s_cfg;
static uint32_t s_base_addr;
static uint8_t s_started;
static TUYA_MIPI_DSI_ISR_CB s_isr_cb;

/* The vendor driver takes the next buffer to scan out from this callback when
 * it finishes a frame. Returning NULL means "the software has not finished a
 * frame, keep showing what you have", so a hook that always returns NULL freezes
 * the panel on its first frame no matter how often the caller transfers. This is
 * the swap mechanism; START_DISPLAY only kicks off the very first frame.
 *
 * It runs in interrupt context, so it only moves the address and forwards the
 * notification - no blocking, no allocation. */
static uint32_t volatile s_next_addr;
/* The address the controller is currently scanning. Needed to tell a real swap
 * from a re-submission of the same buffer. */
static uint32_t s_scan_addr;

static void *tkl_mipi_dsi_frame_end_hook(void)
{
    uint32_t next = s_next_addr;
    s_next_addr = 0;

    if (next == 0u) {
        /* Nothing submitted; tell the driver to keep scanning what it has. */
        return NULL;
    }

    /* A buffer is only retired when the controller moves to a different one.
     * Re-submitting the address it is already scanning changes nothing, so no
     * buffer became drawable and the caller must not be told otherwise - the
     * only buffer in play is the one still being scanned. */
    if (next != s_scan_addr) {
        s_scan_addr = next;
        if (s_isr_cb != NULL) {
            s_isr_cb(MIPI_DSI_OUTPUT_FINISH);
        }
    }
    return (void *)(uintptr_t)next;
}

/* The panel is fixed by the board profile, and its geometry is whatever the
 * vendor's lcd_config.h entry for that panel says. The controller always scans
 * that full panel: it has no way to be told a smaller one. So a caller that
 * asked for different dimensions would get a success while its (smaller) buffer
 * was read past the end. Accept only the geometry the panel actually has. */
static int tkl_mipi_dsi_size_supported(uint16_t width, uint16_t height)
{
    return width == LCD_W && height == LCD_H;
}

OPERATE_RET tkl_mipi_dsi_init(TUYA_MIPI_DSI_BASE_CFG_T *cfg)
{
    if (cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (!tkl_mipi_dsi_size_supported(cfg->width, cfg->height)) {
        return OPRT_NOT_SUPPORTED;
    }
    if (s_lcd_dev != NULL) {
        return OPRT_OK;
    }

    /* The board supplies lcd_platform_data through the vendor device table; the
     * panel is matched by name inside lcd_driver.c. */
    s_lcd_dev = dev_open("lcd", NULL);
    if (s_lcd_dev == NULL) {
        return OPRT_COM_ERROR;
    }

    s_cfg = *cfg;
    s_base_addr = 0;
    s_next_addr = 0;
    s_scan_addr = 0;
    s_started = 0;
    (void)dev_ioctl(s_lcd_dev, IOCTL_LCD_RGB_SET_ISR_CB, (u32)(uintptr_t)tkl_mipi_dsi_frame_end_hook);
    return OPRT_OK;
}

OPERATE_RET tkl_mipi_dsi_deinit(void)
{
    if (s_lcd_dev == NULL) {
        return OPRT_OK;
    }
    dev_close(s_lcd_dev);
    s_lcd_dev = NULL;
    s_started = 0;
    s_isr_cb = NULL;
    s_base_addr = 0;
    s_next_addr = 0;
    return OPRT_OK;
}

OPERATE_RET tkl_mipi_dsi_irq_cb_register(TUYA_MIPI_DSI_ISR_CB cb)
{
    s_isr_cb = cb;
    return OPRT_OK;
}

OPERATE_RET tkl_mipi_dsi_ppi_set(uint16_t width, uint16_t height)
{
    /* Recorded and reported only. The panel timing is fixed by its init table
     * and the frame buffer is the caller's, so there is nothing to program here
     * - but the value is still checked, because accepting one the controller
     * cannot scan is what lets a caller size its buffer wrongly. */
    if (!tkl_mipi_dsi_size_supported(width, height)) {
        return OPRT_NOT_SUPPORTED;
    }
    s_cfg.width = width;
    s_cfg.height = height;
    return OPRT_OK;
}

OPERATE_RET tkl_mipi_dsi_pixel_mode_set(TUYA_DISPLAY_PIXEL_FMT_E mode)
{
    if (mode != TUYA_PIXEL_FMT_RGB565) {
        return OPRT_NOT_SUPPORTED;
    }
    s_cfg.fmt = mode;
    return OPRT_OK;
}

OPERATE_RET tkl_mipi_dsi_base_addr_set(uint32_t addr)
{
    if (addr == 0u) {
        return OPRT_INVALID_PARM;
    }

    /* The panel is fed by DMA from SDRAM, so the drawn lines have to leave the
     * CPU cache before the controller is told to scan them. */
    if (s_cfg.width != 0u && s_cfg.height != 0u) {
        DcuFlushRegion((void *)(uintptr_t)addr,
                       (u32)(s_cfg.width * s_cfg.height * 2u));
    }

    s_base_addr = addr;
    return OPRT_OK;
}

OPERATE_RET tkl_mipi_dsi_display_transfer_start(void)
{
    if (s_lcd_dev == NULL || s_base_addr == 0u) {
        return OPRT_INVALID_PARM;
    }

    if (!s_started) {
        /* Kick off scan-out. The vendor driver takes every later frame from the
         * frame-end callback, so this is called exactly once. */
        s_started = 1;
        s_scan_addr = s_base_addr;
        (void)dev_ioctl(s_lcd_dev, IOCTL_LCD_RGB_START_DISPLAY, (u32)s_base_addr);
        return OPRT_OK;
    }

    /* Hand the finished frame to the controller and wait until it has taken it,
     * which is also what makes the buffer the caller draws into next reusable. */
    s_next_addr = s_base_addr;
    (void)dev_ioctl(s_lcd_dev, IOCTL_LCD_RGB_WAIT_FB_SWAP_FINISH, 0);
    return OPRT_OK;
}

OPERATE_RET tkl_mipi_dsi_display_transfer_stop(void)
{
    return tkl_mipi_dsi_deinit();
}

#else /* !JIELI_SELECTED_CHIP_WL83 */

/* The vendor LCD stack this backend drives is only present in the WL83 SDK:
 * lcd_driver.h and the apps/common/lcd tree it needs are not shipped for WL82.
 * The manifest lists this file for every chip, so the WL82 build compiles this
 * translation unit too and must get a complete, honest set of symbols rather
 * than a missing header. Every entry reports unsupported; the display path
 * simply does not exist on that chip yet.
 */
OPERATE_RET tkl_mipi_dsi_init(TUYA_MIPI_DSI_BASE_CFG_T *cfg)
{
    (void)cfg;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_mipi_dsi_deinit(void)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_mipi_dsi_irq_cb_register(TUYA_MIPI_DSI_ISR_CB cb)
{
    (void)cb;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_mipi_dsi_ppi_set(uint16_t width, uint16_t height)
{
    (void)width;
    (void)height;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_mipi_dsi_pixel_mode_set(TUYA_DISPLAY_PIXEL_FMT_E mode)
{
    (void)mode;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_mipi_dsi_base_addr_set(uint32_t addr)
{
    (void)addr;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_mipi_dsi_display_transfer_start(void)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_mipi_dsi_display_transfer_stop(void)
{
    return OPRT_NOT_SUPPORTED;
}

#endif /* JIELI_SELECTED_CHIP_WL83 */
