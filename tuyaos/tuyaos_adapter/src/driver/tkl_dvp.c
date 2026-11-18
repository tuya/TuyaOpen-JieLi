/**
 * @file tkl_dvp.c
 * @brief Jieli WL82 camera TKL: vendor video pipeline behind the Tuya DVP TDD.
 *
 * Two board paths share one TKL contract, and the board Kconfig choice picks
 * exactly one of them:
 *
 *   JIELI_CAMERA_PATH_DVP  J1 24-pin DVP  -> dev_open("video0") + VIDIOC overlay
 *   JIELI_CAMERA_PATH_SPI  J4 12-pin SPI  -> dev_open("video1.0") for the sensor
 *                                            + dev_open("spi2") for the data
 *
 * The board has both connectors, but PA2/PA3/PA4 and PH0-PH3 are wired to
 * both, so they cannot run together. The sensor is probed, configured and
 * reset by the vendor drivers; this file only moves frames.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_dvp.h"

#include <stdio.h>
#include <string.h>

#include "tkl_memory.h"
#include "tkl_system.h"
#include "tkl_thread.h"

#include "device.h"
#include "device/gpio.h"
#include "video/video.h"
#include "video/video_ioctl.h"
#include "video/camera.h"

#if defined(JIELI_SELECTED_CHIP_WL82) && ((defined(JIELI_CAMERA_PATH_DVP) && (JIELI_CAMERA_PATH_DVP == 1)) ||          \
                                          (defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)))

/* The vendor make receives these from the staging overlay; the Tuya-side
 * compile of the adapter sees the board Kconfig through tuya_kconfig.h. */
#if !defined(JIELI_CAMERA_WIDTH)
#if defined(JIELI_BOARD_CAMERA_WIDTH)
#define JIELI_CAMERA_WIDTH (JIELI_BOARD_CAMERA_WIDTH)
#else
#define JIELI_CAMERA_WIDTH 640
#endif
#endif
#if !defined(JIELI_CAMERA_HEIGHT)
#if defined(JIELI_BOARD_CAMERA_HEIGHT)
#define JIELI_CAMERA_HEIGHT (JIELI_BOARD_CAMERA_HEIGHT)
#else
#define JIELI_CAMERA_HEIGHT 480
#endif
#endif

#define CAM_TAG "camera"

static TUYA_DVP_CFG_T    s_cfg;
static TKL_THREAD_HANDLE s_thread       = NULL;
static volatile int      s_running      = 0;
static volatile int      s_stopped      = 1;
static void             *s_video_dev    = NULL;
static u8               *s_yuv_buf      = NULL;
static u8               *s_yuv_packed   = NULL;
static u32               s_yuv_buf_size = 0;
static uint32_t          s_frame_id     = 0;
static u32               s_frame_count  = 0;

static void camera_deliver_frame(u8 *data, u32 len, u16 width, u16 height, TUYA_FRAME_FMT_E fmt)
{
    TUYA_DVP_FRAME_MANAGE_T *frame;

    if (s_cfg.inter_cfg.assign_cb == NULL || s_cfg.inter_cfg.post_cb == NULL) {
        return;
    }
    if (data == NULL || len == 0) {
        return;
    }

    frame = s_cfg.inter_cfg.assign_cb(fmt, s_cfg.inter_cfg.cb_param);
    if (frame == NULL || frame->data == NULL) {
        return;
    }
    if (len > frame->data_len) {
        len = frame->data_len;
    }

    memcpy(frame->data, data, len);
    frame->frame_fmt         = fmt;
    frame->frame_id          = ++s_frame_id;
    frame->width             = width;
    frame->height            = height;
    frame->data_len          = len;
    frame->total_frame_len   = len;
    frame->is_frame_complete = 1;
    frame->is_i_frame        = 1;

    s_cfg.inter_cfg.post_cb(frame, s_cfg.inter_cfg.cb_param);

    if (s_frame_count++ == 0) {
        printf("[%s] first frame: %ux%u fmt=%d len=%u\n", CAM_TAG, (unsigned)width, (unsigned)height, (int)fmt,
               (unsigned)len);
    }
}

/* ------------------------------------------------------------------------- */
/* DVP: J1 / video0                                                          */
/* ------------------------------------------------------------------------- */
#if defined(JIELI_CAMERA_PATH_DVP) && (JIELI_CAMERA_PATH_DVP == 1)

/* common_lib.a has no prototype in the public headers. */
extern void flushinv_dcache(void *ptr, int len);

/* video0 hands back planar YUV422P (Y, then U, then V); TDL's YUV422
 * contract is the packed 4-byte/two-pixel group. Interleave here so the
 * generic example and tdl_disp_convert_yuv422_to_fb see one format on both
 * the DVP and SPI paths, instead of every consumer decoding the vendor's
 * layout itself. */
static void yuv422p_to_packed_yuyv(const u8 *src, u8 *dst, u32 width, u32 height)
{
    size_t         y_size = (size_t)width * (size_t)height;
    const u8      *u_plane = src + y_size;
    const u8      *v_plane = u_plane + y_size / 2U;

    for (u32 row = 0; row < height; row++) {
        const u8 *y_row = src + (size_t)row * width;
        const u8 *u_row = u_plane + (size_t)row * (width / 2U);
        const u8 *v_row = v_plane + (size_t)row * (width / 2U);
        u8       *out   = dst + (size_t)row * width * 2U;

        for (u32 x = 0; x < width; x += 2U) {
            out[0] = y_row[x];
            out[1] = u_row[x >> 1];
            out[2] = y_row[x + 1U];
            out[3] = v_row[x >> 1];
            out += 4;
        }
    }
}

static void dvp_capture_thread(void *arg)
{
    struct video_format fmt;
    struct yuv_image    yuv;
    int                 err;

    (void)arg;

    /* The vendor's DVP examples open the YUV overlay channel as
     * "video0.2"; keep a bare "video0" fallback for profiles that register
     * the node without sub-devices. */
    s_video_dev = dev_open("video0.2", NULL);
    if (s_video_dev == NULL) {
        s_video_dev = dev_open("video0", NULL);
    }
    if (s_video_dev == NULL) {
        printf("[%s] video0 open failed (no DVP sensor?)\n", CAM_TAG);
        s_stopped = 1;
        return;
    }

    {
        char                      logo[16] = {0};
        struct camera_device_info info;

        memset(&info, 0, sizeof(info));
        if (dev_ioctl(s_video_dev, CAMERA_GET_SENSOR_TYPE, (u32)logo) == 0) {
            printf("[%s] sensor: %s\n", CAM_TAG, logo);
        }
        if (dev_ioctl(s_video_dev, CAMERA_GET_SENSOR_INFO, (u32)&info) == 0) {
            printf("[%s] sensor info: %ux%u fps=%u real_fps=%u\n", CAM_TAG, (unsigned)info.width, (unsigned)info.height,
                   (unsigned)info.fps, (unsigned)info.real_fps);
        }
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type        = VIDEO_BUF_TYPE_VIDEO_OVERLAY;
    fmt.pixelformat = VIDEO_PIX_FMT_YUV422;
    fmt.src_width   = JIELI_CAMERA_WIDTH;
    fmt.src_height  = JIELI_CAMERA_HEIGHT;
    fmt.win.width   = JIELI_CAMERA_WIDTH;
    fmt.win.height  = JIELI_CAMERA_HEIGHT;
    fmt.fps         = s_cfg.fps;
    fmt.static_buf  = s_yuv_buf;
    fmt.sbuf_size   = s_yuv_buf_size;

    err = dev_ioctl(s_video_dev, VIDIOC_SET_FMT, (u32)&fmt);
    if (err) {
        printf("[%s] VIDIOC_SET_FMT failed: %d\n", CAM_TAG, err);
        goto out;
    }
    err = dev_ioctl(s_video_dev, VIDIOC_SET_OVERLAY, 0);
    if (err) {
        printf("[%s] VIDIOC_SET_OVERLAY failed: %d\n", CAM_TAG, err);
        goto out;
    }

    printf("[%s] DVP capture started\n", CAM_TAG);
    while (s_running) {
        memset(&yuv, 0, sizeof(yuv));
        err = dev_ioctl(s_video_dev, VIDIOC_GET_OVERLAY, (u32)&yuv);
        if (err || yuv.addr == NULL || yuv.size == 0) {
            tkl_system_sleep(5);
            continue;
        }
        /* The vendor writes the overlay buffer from DMA; the CPU copy below
         * must not read stale lines. This is the TKL's job, not the example's. */
        flushinv_dcache(yuv.addr, (int)yuv.size);
        yuv422p_to_packed_yuyv(yuv.addr, s_yuv_packed, JIELI_CAMERA_WIDTH, JIELI_CAMERA_HEIGHT);
        camera_deliver_frame(s_yuv_packed, s_yuv_buf_size, JIELI_CAMERA_WIDTH, JIELI_CAMERA_HEIGHT,
                             TUYA_FRAME_FMT_YUV422);
        dev_ioctl(s_video_dev, VIDIOC_CLEAR_OVERLAY, 0);
    }

out:
    dev_close(s_video_dev);
    s_video_dev = NULL;
    printf("[%s] DVP capture stopped\n", CAM_TAG);
    s_stopped = 1;
}

static OPERATE_RET dvp_prepare(void)
{
    s_yuv_buf_size = (u32)JIELI_CAMERA_WIDTH * JIELI_CAMERA_HEIGHT * 2U;
    s_yuv_buf      = tkl_system_malloc(s_yuv_buf_size);
    s_yuv_packed   = tkl_system_malloc(s_yuv_buf_size);
    if (s_yuv_buf == NULL || s_yuv_packed == NULL) {
        printf("[%s] no memory for %u-byte YUV422 buffer\n", CAM_TAG, (unsigned)s_yuv_buf_size);
        if (s_yuv_buf != NULL) {
            tkl_system_free(s_yuv_buf);
            s_yuv_buf = NULL;
        }
        if (s_yuv_packed != NULL) {
            tkl_system_free(s_yuv_packed);
            s_yuv_packed = NULL;
        }
        return OPRT_MALLOC_FAILED;
    }
    return OPRT_OK;
}

static void dvp_release(void)
{
    if (s_yuv_buf != NULL) {
        tkl_system_free(s_yuv_buf);
        s_yuv_buf = NULL;
    }
    if (s_yuv_packed != NULL) {
        tkl_system_free(s_yuv_packed);
        s_yuv_packed = NULL;
    }
}
#endif /* JIELI_CAMERA_PATH_DVP */

/* ------------------------------------------------------------------------- */
/* SPI: J4 / video1 + spi2                                                   */
/* ------------------------------------------------------------------------- */
#if defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)

#include "asm/spi.h"

/* Vendor framing: one frame is FHEAD + H*(LHEAD+payload+LEND) + FEND. The
 * header/tail words live around every line, so a frame packet is
 * (width*2 + 8) * height + 8 bytes, not just width*2*height + 24. The SPI
 * DMA block must be at least one whole packet or READ_DATA never returns a
 * buffer whose first and last words are the frame tags.
 */
/* GC0310 over SPI always streams its native 640x480 YUYV raster: the page-0
 * crop window and the CISCTL output-size registers were both measured to leave
 * the 1288-byte row stride untouched, so a smaller sensor-side mode is not
 * available. Keep the wire geometry fixed to VGA here and decimate to the
 * configured preview size when a frame is complete. */
#define SPI_SENSOR_WIDTH  640U
#define SPI_SENSOR_HEIGHT 480U
#define SPI_LINE_BYTES    (SPI_SENSOR_WIDTH * 2U)
#define SPI_PACKET_BYTES  ((SPI_LINE_BYTES + 8U) * SPI_SENSOR_HEIGHT + 8U)
#define SPI_FRAME_BYTES   (SPI_LINE_BYTES * SPI_SENSOR_HEIGHT)
#define SPI_OUT_BYTES     ((u32)JIELI_CAMERA_WIDTH * (u32)JIELI_CAMERA_HEIGHT * 2U)

/* The DMA buffer used to be a 311 KiB static .sram array (two packets). That
 * fits the link, but it sits in .ram0_bss from reset, cutting the internal
 * heap from ~464 KiB to ~152 KiB before the vendor network/BT stacks run.
 * Allocate one packet on demand instead: the heap is whole at boot and the
 * camera pays for the buffer only when it starts. */
static u8   *s_spi_buf;
static u8   *s_spi_buf_raw;
static u32   s_spi_buf_size;
static u8   *s_spi_frame;
static u8   *s_spi_out;
static void *s_spi_dev    = NULL;
static void *s_sensor_dev = NULL;
/* Handshake between the receive loop and the sensor bring-up: the sensor must
 * not be (re)started until the SPI2 slave is armed, otherwise the first packet
 * is captured mid-frame and every later packet inherits the same byte offset
 * (the frame is exactly block_size long), so the 0xFF0000AB head never lines
 * up. */
static volatile int s_sensor_ready = 0;
static volatile int s_spi_armed    = 0;
/* Set by the sensor thread when it is done touching video1.0 / PH2, so the
 * capture thread's teardown waits instead of closing the device underneath it. */
static volatile int s_sensor_done = 0;

static u8 *spi_aligned_alloc(u32 size, u8 **raw_out)
{
    u8 *raw = tkl_system_malloc(size + 32U);
    if (raw == NULL) {
        return NULL;
    }
    *raw_out = raw;
    return (u8 *)(((uintptr_t)raw + 31U) & ~(uintptr_t)31U);
}

static int spi_tag_eq(const u8 *p, u8 b3)
{
    return p[0] == 0xff && p[1] == 0x00 && p[2] == 0x00 && p[3] == b3;
}

/* Alternate receive path: one interrupt per wire line, like the vendor
 * Devkitboard_test/spi_camera.c demo. The block API needs a whole 618 KiB
 * frame per READ_DATA and starts losing bytes as soon as the reader is late;
 * the line path re-arms 1.3 KiB every interrupt, so a late reader only costs
 * one line. Default stays on the block path until this is verified on the
 * board. */
#ifndef JIELI_SPI_LINE_IRQ
#define JIELI_SPI_LINE_IRQ 1
#endif

#if JIELI_SPI_LINE_IRQ
#ifndef ___interrupt
#define ___interrupt __attribute__((interrupt("")))
#endif
#ifndef sec
#define sec(x) __attribute__((section(#x)))
#endif
extern void *ram_malloc(unsigned long size);
extern void ram_free(void *rmem);
extern u8 dma_copy_async(void *dst, void *src, int len);

#define SPI_IRQ_LINE_MID    (4U + SPI_LINE_BYTES + 4U)  /* LHEAD + payload + LEND */
#define SPI_IRQ_LINE_FIRST  (4U + SPI_IRQ_LINE_MID)     /* FHEAD + line           */
#define SPI_IRQ_BUF_NUM     8U
#define SPI_IRQ_FRAME_NUM   3U
#define SPI_IRQ_TAG_FHEAD   0xab0000ffU /* little-endian ff0000ab */
#define SPI_IRQ_TAG_LHEAD   0x800000ffU /* little-endian ff000080 */

static u8               *s_irq_lines;
static u8               *s_irq_frames[SPI_IRQ_FRAME_NUM];
static u8               *s_irq_out;
static volatile int      s_irq_active;
static volatile u32      s_irq_armed;
static volatile u32      s_irq_row;
static volatile u32      s_irq_wr;
static volatile u32      s_irq_ready; /* completed frame index + 1, 0 = none */
static volatile u32      s_irq_lines_done;
static volatile u32      s_irq_tag_err;

static ___interrupt void spi_line_isr(void) sec(.volatile_ram_code)
{
    u32 row = s_irq_row;
    u8 *buf = s_irq_lines + (u32)s_irq_armed * SPI_IRQ_LINE_FIRST;
    u32 tag = *(volatile u32 *)buf;

    JL_SPI2->CON |= BIT(14); /* clear the line interrupt */

    if (s_irq_active) {
        if (tag == (row == 0U ? SPI_IRQ_TAG_FHEAD : SPI_IRQ_TAG_LHEAD)) {
            /* Copy with the DMA engine, exactly like the vendor ISR: the next
             * line follows within ~100 us at 24 MHz PCLK, and a CPU memcpy of
             * 1280 bytes into the SDRAM frame buffer does not finish in time.
             * Both addresses are 4-byte aligned: the line buffers come from
             * ram_malloc with the head at +4/+8, and each frame row is 1280
             * bytes. Rotating 8 line buffers x SPI_IRQ_FRAME_NUM frames keep
             * the DMA ahead of the re-arm. */
            (void)dma_copy_async((void *)(s_irq_frames[s_irq_wr] + (size_t)row * SPI_LINE_BYTES),
                                 (void *)(buf + (row == 0U ? 8U : 4U)), (int)SPI_LINE_BYTES);
            row++;
            s_irq_lines_done++;
        } else {
            /* Keep the row index; the next line is re-armed for the same slot. */
            s_irq_tag_err++;
        }

        if (row >= SPI_SENSOR_HEIGHT) {
            s_irq_ready = s_irq_wr + 1U;
            s_irq_wr    = (s_irq_wr + 1U) % SPI_IRQ_FRAME_NUM;
            row         = 0U;
        }
        s_irq_row = row;

        s_irq_armed = (s_irq_armed + 1U) % SPI_IRQ_BUF_NUM;
        JL_SPI2->ADR = (u32)(s_irq_lines + (u32)s_irq_armed * SPI_IRQ_LINE_FIRST);
        JL_SPI2->CNT = (row == 0U || row == SPI_SENSOR_HEIGHT - 1U) ? SPI_IRQ_LINE_FIRST : SPI_IRQ_LINE_MID;
    }
}
#endif /* JIELI_SPI_LINE_IRQ */


/* Streaming frame assembler.
 *
 * The DMA hands over one 618248-byte block at a time, but a block boundary
 * lands in the middle of a line, and the 2-wire link drops or duplicates a
 * byte now and then. Two things follow:
 *
 *  - a line that starts near the end of a block is carried into the next
 *    block (s_line_carry) instead of being dropped, which used to leave a
 *    frame one row short and force the whole frame out;
 *  - a line is accepted only when its end tag sits where the framing says
 *    (within a small window for the link's byte slips). YUYV pixel data can
 *    spell 0xff000080 (Y=255,U=0,Y=0,V=128), and those false heads used to be
 *    copied as rows - the purple/green streaks on the panel.
 *
 * A frame is delivered at its end tag when at least 476 of the 480 rows are
 * present (the missing bottom rows repeat the last one); grossly short or
 * time-overrun assemblies are dropped instead of painting two frames as one.
 */
static u32 s_spi_line;
static int s_spi_assembling;
static u32 s_frame_start_ms;
static u8  s_line_carry[SPI_LINE_BYTES];
static u32 s_line_carry_len;
static int s_line_pending;

static u32 s_dbg_frames;
static u32 s_dbg_skipped;

/* A real line start is followed by its end tag at a fixed offset, or (when
 * that tag itself was hit by a byte slip) by the next line head at the same
 * fixed offset from the following word. YUYV pixel data can spell
 * 0xFF000080 (Y=255,U=0,Y=0,V=128) in saturated scenes, so a bare tag match is
 * not proof of a row boundary; requiring the framing around it is. */
static int spi_line_start_ok(const u8 *p, const u8 *end)
{
    /* p is the start tag; the end tag belongs at p + 4 + SPI_LINE_BYTES.
     * Accept it within a small window: the link shifts framing bytes. */
    for (int k = -6; k <= 6; k++) {
        const u8 *t = p + 4 + SPI_LINE_BYTES + k;

        if (t >= p + 4 && t + 4 <= end && (spi_tag_eq(t, 0x9d) || spi_tag_eq(t, 0xa6))) {
            return 1;
        }
    }
    /* End tag missing: the next line head sitting exactly after this line's
     * 4+LINE bytes still proves this was a real start. */
    if (p + 8 + SPI_LINE_BYTES + 4 <= end && spi_tag_eq(p + 4 + SPI_LINE_BYTES + 4, 0x80)) {
        return 1;
    }
    return 0;
}

/* Nearest-neighbour decimation from the sensor's VGA raster to the configured
 * preview size. Only integer ratios are supported (this board uses 2:1 in both
 * axes); anything else is copied unscaled by the caller. The source is packed
 * YUYV, so one output YUYV quad covers two source quads:
 * {Y(sx), U(sx), Y(sx+2), V(sx)} keeps the chroma pair each output pixel
 * shares with its neighbour instead of copying bytes at the wrong phase,
 * which would tint the row. */
static int spi_frame_to_preview(const u8 *src)
{
    const u32 sw = SPI_SENSOR_WIDTH;
    const u32 sh = SPI_SENSOR_HEIGHT;
    const u32 dw = (u32)JIELI_CAMERA_WIDTH;
    const u32 dh = (u32)JIELI_CAMERA_HEIGHT;

    if (dw == 0 || dh == 0 || (dw & 1U) || (sw % dw) != 0 || (sh % dh) != 0) {
        /* No integer ratio: report it so the caller drops the frame instead
         * of delivering the previous preview as if it were this one. */
        return 0;
    }
    for (u32 y = 0; y < dh; y++) {
        const u32  sy   = y * (sh / dh);
        const u8  *srow = src + (size_t)sy * SPI_LINE_BYTES;
        u8        *drow = s_spi_out + (size_t)y * dw * 2U;

        for (u32 x = 0; x < dw; x += 2U) {
            const u32  sx = x * (sw / dw);
            const u8  *s  = srow + (size_t)sx * 2U;
            u8        *d  = drow + (size_t)x * 2U;

            d[0] = s[0]; /* Y(sx)   */
            d[1] = s[1]; /* U(sx)   */
            d[2] = s[4]; /* Y(sx+2) */
            d[3] = s[3]; /* V(sx)   */
        }
    }
    return 1;
}

static int spi_consume_packet(const u8 *buf, int len)
{
    const u8 *p         = buf;
    const u8 *end       = buf + len;
    int       delivered = 0;

    /* Finish the line that started at the end of the previous block. */
    if (s_line_pending && s_spi_assembling && s_spi_line < (u32)SPI_SENSOR_HEIGHT) {
        u32 need  = SPI_LINE_BYTES - s_line_carry_len;
        u32 avail = (u32)(end - p);
        u32 take  = (avail < need) ? avail : need;

        if (take > 0) {
            memcpy(s_line_carry + s_line_carry_len, p, take);
            s_line_carry_len += take;
            p += take;
        }
        if (s_line_carry_len == SPI_LINE_BYTES) {
            memcpy(s_spi_frame + s_spi_line * SPI_LINE_BYTES, s_line_carry, SPI_LINE_BYTES);
            s_spi_line++;
            s_line_pending = 0;
        }
    }

    while (p + 4 <= end) {
        const u8 *ff = memchr(p, 0xff, (size_t)(end - p));

        if (ff == NULL || ff + 4 > end) {
            break;
        }
        p = ff;
        if (spi_tag_eq(p, 0xab)) {
            s_spi_line       = 0;
            s_spi_assembling = 1;
            s_line_pending   = 0;
            s_frame_start_ms = (u32)tkl_system_get_millisecond();
            p += 4;
        } else if (spi_tag_eq(p, 0x80)) {
            if (s_spi_assembling && s_spi_line < (u32)SPI_SENSOR_HEIGHT) {
                if (p + 4 + SPI_LINE_BYTES <= end) {
                    /* Only a start whose framing checks out is a row. A bare
                     * 0xFF000080 inside saturated pixel data is skipped here:
                     * copying it painted the purple/green streaks, and faking
                     * a row for it poisoned every frame in bright scenes. */
                    if (spi_line_start_ok(p, end)) {
                        memcpy(s_spi_frame + s_spi_line * SPI_LINE_BYTES, p + 4, SPI_LINE_BYTES);
                        s_spi_line++;
                    }
                } else {
                    /* Straddles the block: carry the head bytes into the next. */
                    u32 avail = (u32)(end - (p + 4));

                    memcpy(s_line_carry, p + 4, avail);
                    s_line_carry_len = avail;
                    s_line_pending   = 1;
                    p                = end;
                    break;
                }
            }
            p += 4;
        } else if (spi_tag_eq(p, 0xb6) || spi_tag_eq(p, 0xa6)) {
            /* A frame that took far longer than the link's frame period is a
             * mixed assembly: its end tag or the next frame head was lost, so
             * rows from the following frame were appended and the seam shows
             * as a horizontal split that moves. Drop it instead of painting
             * two frames as one. */
            if (s_spi_assembling && s_spi_line > 0 &&
                (u32)tkl_system_get_millisecond() - s_frame_start_ms < 400u) {
                /* A frame missing its last few rows is completed with the
                 * previous row rather than dropped: on this link a single byte
                 * slip per frame is common, and rejecting every shortened
                 * frame left the panel stuck on the last good picture. Grossly
                 * short assemblies (more than four rows) are still dropped. */
                if (s_spi_line >= (u32)SPI_SENSOR_HEIGHT) {
                    s_spi_line = (u32)SPI_SENSOR_HEIGHT;
                } else if (s_spi_line >= (u32)SPI_SENSOR_HEIGHT - 4u) {
                    const u8 *last = s_spi_frame + (s_spi_line - 1u) * SPI_LINE_BYTES;

                    while (s_spi_line < (u32)SPI_SENSOR_HEIGHT) {
                        memcpy(s_spi_frame + s_spi_line * SPI_LINE_BYTES, last, SPI_LINE_BYTES);
                        s_spi_line++;
                    }
                }
                if (s_spi_line == (u32)SPI_SENSOR_HEIGHT) {
                    if (JIELI_CAMERA_WIDTH == SPI_SENSOR_WIDTH && JIELI_CAMERA_HEIGHT == SPI_SENSOR_HEIGHT) {
                        camera_deliver_frame(s_spi_frame, SPI_FRAME_BYTES, SPI_SENSOR_WIDTH, SPI_SENSOR_HEIGHT,
                                             TUYA_FRAME_FMT_YUV422);
                        s_dbg_frames++;
                        delivered = 1;
                    } else if (spi_frame_to_preview(s_spi_frame)) {
                        camera_deliver_frame(s_spi_out, SPI_OUT_BYTES, JIELI_CAMERA_WIDTH, JIELI_CAMERA_HEIGHT,
                                             TUYA_FRAME_FMT_YUV422);
                        s_dbg_frames++;
                        delivered = 1;
                    } else {
                        s_dbg_skipped++;
                    }
                } else {
                    s_dbg_skipped++;
                }
            }
            s_spi_assembling = 0;
            s_spi_line       = 0;
            s_line_pending   = 0;
            p += 4;
        } else {
            p++;
        }
    }
    return delivered;
}

/* The sensor streams as soon as video1.0 opens, and the vendor SPI example is
 * explicit that IOCTL_SPI_READ_DATA must be armed before the peer sends, or the
 * receiver locks onto a partial packet. Open the sensor from its own thread so
 * the capture loop is already waiting when the first packet arrives. */
static void spi_sensor_open_thread(void *arg)
{
    (void)arg;
    tkl_system_sleep(200);

    printf("[%s] SPI: opening video1.0\n", CAM_TAG);
    s_sensor_dev = dev_open("video1.0", NULL);
    if (s_sensor_dev == NULL) {
        printf("[%s] video1.0 open failed (no SPI sensor?)\n", CAM_TAG);
        s_sensor_ready = -1;
        s_sensor_done  = 1;
        return;
    }
    printf("[%s] SPI: video1.0 ok\n", CAM_TAG);

    /* Opening video1.0 resets and reconfigures the sensor, and on this board
     * that clears the PH2 XCLK output channel: measured on AC791, the module
     * stays mute (PA3 PCLK never toggles) until the channel is re-armed after
     * dev_open. Re-applying it here is what makes the sensor start clocking. */
    gpio_output_channle(IO_PORTH_02, CH1_PLL_24M);
    tkl_system_sleep(50);
    printf("[%s] SPI: XCLK re-armed after video1 open\n", CAM_TAG);

    {
        char                      logo[16] = {0};
        struct camera_device_info info;

        memset(&info, 0, sizeof(info));
        if (dev_ioctl(s_sensor_dev, CAMERA_GET_SENSOR_TYPE, (u32)logo) == 0) {
            printf("[%s] sensor: %s\n", CAM_TAG, logo);
        }
        if (dev_ioctl(s_sensor_dev, CAMERA_GET_SENSOR_INFO, (u32)&info) == 0) {
            printf("[%s] sensor info: %ux%u fps=%u\n", CAM_TAG, (unsigned)info.width, (unsigned)info.height,
                   (unsigned)info.fps);
        }
    }

    /* The SPI2 slave must be armed while the sensor is quiet, and the sensor
     * must then (re)start its stream from a frame head. Signal readiness, wait
     * for the receiver, then reset the sensor: GC0310 re-runs its table and
     * restarts streaming, so the first clock SPI2 sees is the 0xFF0000AB head. */
    s_sensor_ready = 1;
    {
        int waited = 0;
        while (s_running && !s_spi_armed && waited < 3000) {
            tkl_system_sleep(10);
            waited += 10;
        }
        if (s_running && s_spi_armed) {
            int r = dev_ioctl(s_sensor_dev, CAMERA_SET_SENSOR_RESET, 0);
            printf("[%s] SPI: sensor reset after spi arm ret=%d\n", CAM_TAG, r);
            tkl_system_sleep(30);
        }
    }

    /* No VIDIOC_STREAM_ON here: the vendor SPI-camera demo
     * (apps/common/example/video/camera/spi/main.c) opens video1.0 and starts
     * receiving straight away, and on this pin/board the ioctl blocks the
     * caller instead of starting the sensor. GC0310_init configures the SPI
     * output block (page 0x03) during dev_open already. */
    s_sensor_done = 1;
}

#if JIELI_SPI_LINE_IRQ
/* Mirrors the vendor demo's camera_reinit: one rejected line tag means the
 * fixed-size reads no longer line up, and re-arming the same slot keeps them
 * misaligned forever. Re-arm the receiver at a frame boundary and restart the
 * sensor in the vendor's order (receiver first, then sensor head). */
static void spi_line_resync(void)
{
    JL_SPI2->CON &= ~BIT(13);
    s_irq_row    = 0;
    s_irq_ready  = 0;
    s_irq_wr     = 0;
    s_irq_armed  = 0;
    JL_SPI2->ADR = (u32)s_irq_lines;
    JL_SPI2->CNT = SPI_IRQ_LINE_FIRST;
    tkl_system_sleep(20); /* vendor's settle time between arm and sensor head */
#if defined(JIELI_CAMERA_TDD_SENSOR) && (JIELI_CAMERA_TDD_SENSOR == 1)
    {
        extern OPERATE_RET tkl_camera_sensor_restart(void);
        (void)tkl_camera_sensor_restart();
    }
#else
    if (s_sensor_dev != NULL) {
        (void)dev_ioctl(s_sensor_dev, CAMERA_SET_SENSOR_RESET, 0);
    }
#endif
    JL_SPI2->CON |= BIT(13);
}
#endif

static void spi_capture_thread(void *arg)
{
#if JIELI_SPI_LINE_IRQ
    u32 last_ms = 0;
#else
    struct spi_user su;
    u8             *read_addr;
    int             ret;
#endif

    (void)arg;

    /* The J4 sensor clock is PH2 (shared with the DVP path on purpose). The
     * vendor sensor driver configures its registers over iic1. The vendor's
     * DevKitBoard SPI-camera example drives PH2 with CH1_PLL_24M
     * (apps/common/example/Devkitboard_test/spi_camera.c:255); CH3 is the
     * wifi_camera board's PH8 channel and does not clock this pin. */
    printf("[%s] SPI: output XCLK PH2 (CH1_PLL_24M)\n", CAM_TAG);
    gpio_output_channle(IO_PORTH_02, CH1_PLL_24M);

    /* Bring the sensor up first (open + XCLK re-arm), then arm SPI2 and only
     * then let the sensor restart its stream: the vendor order is video1.0 ->
     * 20 ms -> spi2 -> receive, and arming before the peer sends is mandatory
     * (apps/common/example/peripheral/spi/spi_slave/main.c:64). */
#if defined(JIELI_CAMERA_TDD_SENSOR) && (JIELI_CAMERA_TDD_SENSOR == 1)
    /* TDD already ran the register table during open(); there is no video1
     * device to wait for. The restart below re-aligns the stream. */
    s_sensor_ready = 1;
    s_sensor_done  = 1;
#else
    {
        static TKL_THREAD_HANDLE s_sensor_thread;
        if (tkl_thread_create(&s_sensor_thread, "cam_sensor", 2048, 4, spi_sensor_open_thread, NULL) != OPRT_OK) {
            printf("[%s] sensor thread create failed\n", CAM_TAG);
            goto out_sensor;
        }
    }
    {
        int waited = 0;
        while (s_running && s_sensor_ready == 0 && waited < 3000) {
            tkl_system_sleep(10);
            waited += 10;
        }
    }
    if (s_sensor_ready != 1) {
        printf("[%s] SPI: sensor did not come up\n", CAM_TAG);
        goto out_sensor;
    }
#endif

    printf("[%s] SPI: opening spi2\n", CAM_TAG);
    s_spi_dev = dev_open("spi2", NULL);
    if (s_spi_dev == NULL) {
        printf("[%s] spi2 open failed\n", CAM_TAG);
        goto out_sensor;
    }
    printf("[%s] SPI: spi2 ok\n", CAM_TAG);

#if JIELI_SPI_LINE_IRQ
    dev_ioctl(s_spi_dev, IOCTL_SPI_SET_IRQ_CPU_ID, 1);
    dev_ioctl(s_spi_dev, IOCTL_SPI_SET_IRQ_FUNC, (u32)spi_line_isr);
    printf("[%s] SPI: line-IRQ mode, %u line buffers\n", CAM_TAG, (unsigned)SPI_IRQ_BUF_NUM);
#else
    memset(&su, 0, sizeof(su));
    su.buf            = s_spi_buf;
    su.buf_size       = s_spi_buf_size;
    su.block_size     = SPI_PACKET_BYTES;
    su.first_dma_size = SPI_PACKET_BYTES;
    su.dma_max_cnt    = SPI_MAX_SIZE;

    dev_ioctl(s_spi_dev, IOCTL_SPI_SET_IRQ_CPU_ID, 1);
    printf("[%s] SPI: SET_USER_INFO buf=%p size=%u\n", CAM_TAG, s_spi_buf, (unsigned)s_spi_buf_size);
    dev_ioctl(s_spi_dev, IOCTL_SPI_SET_USER_INFO, (u32)&su);
#endif

    /* The vendor DevKitBoard SPI-camera demo programs the controller directly
     * after dev_open (spi_camera.c:266-272): clear bits 2/11, then enable the
     * receive direction, IRQ and slave mode. The platform attr alone left the
     * controller silent, so apply the same bits here. */
    JL_SPI2->CON &= ~BIT(2);
    JL_SPI2->CON &= ~BIT(11);
    JL_SPI2->CON |= BIT(10);
    JL_SPI2->CON |= BIT(12);
    JL_SPI2->CON |= BIT(13);
    JL_SPI2->CON |= BIT(1);
    JL_SPI2->CON |= BIT(0);

#if JIELI_SPI_LINE_IRQ
    s_irq_armed  = 0;
    s_irq_row    = 0;
    s_irq_wr     = 0;
    s_irq_ready  = 0;
    s_irq_active = 1;
    JL_SPI2->ADR = (u32)s_irq_lines;
    JL_SPI2->CNT = SPI_IRQ_LINE_FIRST;

    s_spi_armed = 1;
    printf("[%s] SPI capture started (%ux%u, line IRQ, first %u B / line %u B)\n", CAM_TAG, SPI_SENSOR_WIDTH,
           SPI_SENSOR_HEIGHT, (unsigned)SPI_IRQ_LINE_FIRST, (unsigned)SPI_IRQ_LINE_MID);

#if defined(JIELI_CAMERA_TDD_SENSOR) && (JIELI_CAMERA_TDD_SENSOR == 1)
    /* The board provides this (see board_com_api.c); it resets and re-runs the
     * TDD sensor register table so the first clock SPI2 sees is a frame head. */
    {
        extern OPERATE_RET tkl_camera_sensor_restart(void);
        int r = tkl_camera_sensor_restart();
        printf("[%s] SPI: TDD sensor restart ret=%d\n", CAM_TAG, r);
    }
#endif

    while (s_running) {
        if (s_irq_ready) {
            u32 idx = s_irq_ready - 1u;

            s_irq_ready = 0;
            if (JIELI_CAMERA_WIDTH == SPI_SENSOR_WIDTH && JIELI_CAMERA_HEIGHT == SPI_SENSOR_HEIGHT) {
                camera_deliver_frame(s_irq_frames[idx], SPI_FRAME_BYTES, SPI_SENSOR_WIDTH, SPI_SENSOR_HEIGHT,
                                     TUYA_FRAME_FMT_YUV422);
                s_dbg_frames++;
            } else if (spi_frame_to_preview(s_irq_frames[idx])) {
                camera_deliver_frame(s_irq_out, SPI_OUT_BYTES, JIELI_CAMERA_WIDTH, JIELI_CAMERA_HEIGHT,
                                     TUYA_FRAME_FMT_YUV422);
                s_dbg_frames++;
            } else {
                s_dbg_skipped++;
            }
        } else {
            tkl_system_sleep(2);
        }
        {
            /* A rejected tag desynchronises the fixed-size reads; the vendor
             * demo restarts the pipeline (camera_reinit) on the same event.
             * Resync at most every 200 ms so a noisy link cannot starve the
             * capture loop. */
            static u32 last_tag_err;
            static u32 last_resync_ms;
            u32        tag_err = s_irq_tag_err;

            if (tag_err != last_tag_err) {
                u32 now = (u32)tkl_system_get_millisecond();

                last_tag_err = tag_err;
                if (now - last_resync_ms >= 200u) {
                    last_resync_ms = now;
                    printf("[%s] SPI: tag error %u, resyncing\n", CAM_TAG, (unsigned)tag_err);
                    spi_line_resync();
                }
            }
        }
        {
            u32 now_ms = (u32)tkl_system_get_millisecond();

            if (last_ms == 0) {
                last_ms = now_ms;
            } else if (now_ms - last_ms >= 5000u) {
                printf("[%s] SPI st frames=%u lines=%u tag_err=%u\n", CAM_TAG, (unsigned)s_dbg_frames,
                       (unsigned)s_irq_lines_done, (unsigned)s_irq_tag_err);
                last_ms = now_ms;
            }
        }
    }
    s_irq_active = 0;
#else
    s_spi_armed = 1;
    printf("[%s] SPI capture started (%ux%u, packet %u B)\n", CAM_TAG, SPI_SENSOR_WIDTH, SPI_SENSOR_HEIGHT,
           (unsigned)SPI_PACKET_BYTES);

    while (s_running) {
        read_addr = NULL;
        ret       = dev_ioctl(s_spi_dev, IOCTL_SPI_READ_DATA, (u32)&read_addr);
        if (ret <= 0 || read_addr == NULL) {
            static u32 idle;
            if ((idle++ & 0x3fffffu) == 0u) {
                printf("[%s] SPI idle, no data from sensor yet\n", CAM_TAG);
            }
            continue;
        }
        (void)spi_consume_packet(read_addr, ret);
        {
            static u32 dbg_reads;
            if (dbg_reads < 20) {
                dbg_reads++;
                if (ret >= 4) {
                    printf("[%s] SPI read ret=%d head=%02x%02x%02x%02x tail=%02x%02x%02x%02x\n", CAM_TAG, ret,
                           read_addr[0], read_addr[1], read_addr[2], read_addr[3], read_addr[ret - 4],
                           read_addr[ret - 3], read_addr[ret - 2], read_addr[ret - 1]);
                }
            }
        }
        dev_ioctl(s_spi_dev, IOCTL_SPI_FREE_DATA, 0);
        {
            static u32 last_ms;
            u32        now_ms = (u32)tkl_system_get_millisecond();
            if (last_ms == 0) {
                last_ms = now_ms;
            } else if (now_ms - last_ms >= 5000u) {
                printf("[%s] SPI st frames=%u skipped=%u\n", CAM_TAG, (unsigned)s_dbg_frames,
                       (unsigned)s_dbg_skipped);
                last_ms = now_ms;
            }
        }
    }
#endif

    dev_close(s_spi_dev);
    s_spi_dev = NULL;
out_sensor:
    /* The sensor thread owns video1.0 and PH2; do not close underneath it. */
    {
        int waited = 0;
        while (!s_sensor_done && waited < 500) {
            tkl_system_sleep(10);
            waited += 10;
        }
    }
    if (s_sensor_dev != NULL) {
        dev_close(s_sensor_dev);
        s_sensor_dev = NULL;
    }
    gpio_clear_output_channle(IO_PORTH_02, CH1_PLL_24M);
    printf("[%s] SPI capture stopped\n", CAM_TAG);
    s_stopped = 1;
}

static OPERATE_RET spi_prepare(void)
{
    s_sensor_ready   = 0;
    s_spi_armed      = 0;
    s_sensor_done    = 0;
    s_dbg_frames     = 0;
    s_dbg_skipped    = 0;

#if defined(JIELI_CAMERA_TDD_SENSOR) && (JIELI_CAMERA_TDD_SENSOR == 1)
    /* The sensor is configured over IIC1 by the TDD before tkl_dvp_start(),
     * so it needs its MCLK from init() rather than from the vendor video1
     * open path. Re-asserting the channel later is harmless. */
    printf("[%s] SPI: output XCLK PH2 early for TDD sensor init\n", CAM_TAG);
    gpio_output_channle(IO_PORTH_02, CH1_PLL_24M);
    tkl_system_sleep(5);
#endif

    /* DMA buffers: the driver fills whole block_size frames and hands them to
     * the reader. Four blocks give the unpacking copy room to fall behind
     * without re-arming in the middle of a frame, which the vendor API docs
     * call out as corrupting ("IOCTL_SPI_READ_DATA 需保证此时对方SPI还没有发送数据").
     * Two blocks (the vendor minimum) still slipped here. */
#if JIELI_SPI_LINE_IRQ
    /* Line buffers must live in internal RAM: the ISR runs from
     * .volatile_ram_code and the vendor warns that SDRAM line buffers
     * occasionally receive corrupt data. */
    s_irq_lines = ram_malloc((unsigned long)SPI_IRQ_BUF_NUM * SPI_IRQ_LINE_FIRST);
    if (s_irq_lines == NULL) {
        printf("[%s] no internal RAM for %u line buffers\n", CAM_TAG,
               (unsigned)(SPI_IRQ_BUF_NUM * SPI_IRQ_LINE_FIRST));
        return OPRT_MALLOC_FAILED;
    }
    for (u32 i = 0; i < SPI_IRQ_FRAME_NUM; i++) {
        s_irq_frames[i] = tkl_system_malloc(SPI_FRAME_BYTES);
        if (s_irq_frames[i] == NULL) {
            printf("[%s] no memory for frame %u\n", CAM_TAG, (unsigned)i);
            while (i > 0u) {
                tkl_system_free(s_irq_frames[--i]);
                s_irq_frames[i] = NULL;
            }
            ram_free(s_irq_lines);
            s_irq_lines = NULL;
            return OPRT_MALLOC_FAILED;
        }
    }
    if (JIELI_CAMERA_WIDTH != SPI_SENSOR_WIDTH || JIELI_CAMERA_HEIGHT != SPI_SENSOR_HEIGHT) {
        s_irq_out = tkl_system_malloc(SPI_OUT_BYTES);
        if (s_irq_out == NULL) {
            printf("[%s] no memory for %u-byte preview frame\n", CAM_TAG, (unsigned)SPI_OUT_BYTES);
            for (u32 i = 0; i < SPI_IRQ_FRAME_NUM; i++) {
                tkl_system_free(s_irq_frames[i]);
                s_irq_frames[i] = NULL;
            }
            ram_free(s_irq_lines);
            s_irq_lines = NULL;
            return OPRT_MALLOC_FAILED;
        }
    }
    printf("[%s] SPI buffers: lines=%p (%u B) frames=3x%u B out=%p\n", CAM_TAG, s_irq_lines,
           (unsigned)(SPI_IRQ_BUF_NUM * SPI_IRQ_LINE_FIRST), (unsigned)SPI_FRAME_BYTES, s_irq_out);
    return OPRT_OK;
#else
    s_spi_buf_size = SPI_PACKET_BYTES * 4U;
    s_spi_buf      = spi_aligned_alloc(s_spi_buf_size, &s_spi_buf_raw);
    if (s_spi_buf == NULL) {
        printf("[%s] no memory for %u-byte SPI DMA buffer\n", CAM_TAG, (unsigned)s_spi_buf_size);
        return OPRT_MALLOC_FAILED;
    }

    s_spi_frame = tkl_system_malloc(SPI_FRAME_BYTES);
    if (s_spi_frame == NULL) {
        printf("[%s] no memory for %u-byte SPI frame\n", CAM_TAG, (unsigned)SPI_FRAME_BYTES);
        tkl_system_free(s_spi_buf_raw);
        s_spi_buf     = NULL;
        s_spi_buf_raw = NULL;
        return OPRT_MALLOC_FAILED;
    }

    /* The preview buffer only exists when the configured size differs from the
     * sensor's fixed VGA raster (see spi_frame_to_preview). */
    if (JIELI_CAMERA_WIDTH != SPI_SENSOR_WIDTH || JIELI_CAMERA_HEIGHT != SPI_SENSOR_HEIGHT) {
        s_spi_out = tkl_system_malloc(SPI_OUT_BYTES);
        if (s_spi_out == NULL) {
            printf("[%s] no memory for %u-byte SPI preview frame\n", CAM_TAG, (unsigned)SPI_OUT_BYTES);
            tkl_system_free(s_spi_frame);
            s_spi_frame = NULL;
            tkl_system_free(s_spi_buf_raw);
            s_spi_buf     = NULL;
            s_spi_buf_raw = NULL;
            return OPRT_MALLOC_FAILED;
        }
    }

    printf("[%s] SPI buffers: dma=%p (%u B) frame=%p (%u B) out=%p (%u B)\n", CAM_TAG, s_spi_buf,
           (unsigned)s_spi_buf_size, s_spi_frame, (unsigned)SPI_FRAME_BYTES, s_spi_out, (unsigned)SPI_OUT_BYTES);
    return OPRT_OK;
#endif
}

static void spi_release(void)
{
#if JIELI_SPI_LINE_IRQ
    if (s_irq_out != NULL) {
        tkl_system_free(s_irq_out);
        s_irq_out = NULL;
    }
    for (u32 i = 0; i < SPI_IRQ_FRAME_NUM; i++) {
        if (s_irq_frames[i] != NULL) {
            tkl_system_free(s_irq_frames[i]);
            s_irq_frames[i] = NULL;
        }
    }
    if (s_irq_lines != NULL) {
        ram_free(s_irq_lines);
        s_irq_lines = NULL;
    }
#else
    if (s_spi_out != NULL) {
        tkl_system_free(s_spi_out);
        s_spi_out = NULL;
    }
    if (s_spi_frame != NULL) {
        tkl_system_free(s_spi_frame);
        s_spi_frame = NULL;
    }
    if (s_spi_buf != NULL) {
        /* Free the original allocation, not the aligned interior pointer. */
        tkl_system_free(s_spi_buf_raw);
        s_spi_buf     = NULL;
        s_spi_buf_raw = NULL;
    }
#endif
}
#endif /* JIELI_CAMERA_PATH_SPI */

/* ------------------------------------------------------------------------- */
/* TKL entry points                                                          */
/* ------------------------------------------------------------------------- */
OPERATE_RET tkl_dvp_init(TUYA_DVP_CFG_T *dvp_cfg)
{
    if (dvp_cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    memcpy(&s_cfg, dvp_cfg, sizeof(s_cfg));
    s_frame_id    = 0;
    s_frame_count = 0;

#if defined(JIELI_CAMERA_PATH_DVP) && (JIELI_CAMERA_PATH_DVP == 1)
    return dvp_prepare();
#elif defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)
    return spi_prepare();
#else
    return OPRT_NOT_SUPPORTED;
#endif
}

OPERATE_RET tkl_dvp_start(void)
{
    if (s_thread != NULL) {
        if (!s_stopped) {
            return OPRT_OK; /* already running */
        }
        /* The thread exited on its own (open failure). Release the stale
         * handle so this call can start a fresh session. */
        tkl_thread_release(s_thread);
        s_thread = NULL;
    }

#if defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)
    /* Per-session handshake state: after stop -> start the old "ready/armed"
     * values would let the sensor thread skip the ordering this code exists
     * to guarantee. */
    s_sensor_ready = 0;
    s_spi_armed    = 0;
    s_sensor_done  = 0;
#endif

    s_running = 1;
    s_stopped = 0;

#if defined(JIELI_CAMERA_PATH_DVP) && (JIELI_CAMERA_PATH_DVP == 1)
    if (tkl_thread_create(&s_thread, "cam_dvp", 4096, 4, dvp_capture_thread, NULL) != OPRT_OK)
#elif defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)
    if (tkl_thread_create(&s_thread, "cam_spi", 4096, 4, spi_capture_thread, NULL) != OPRT_OK)
#else
    if (1)
#endif
    {
        s_running = 0;
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_stop(void)
{
    int waited;

    if (s_thread == NULL) {
        return OPRT_OK;
    }
    s_running = 0;
    /* The capture thread closes its vendor devices (SPI2 -> sensor -> XCLK,
     * or video0) before it marks itself stopped. Wait for that teardown so a
     * later path switch sees PA2/PA3/PA4 and PH0-PH3 released, not a task that
     * was killed mid-ioctl still holding them. */
    for (waited = 0; waited < 100 && !s_stopped; waited++) {
        tkl_system_sleep(10);
    }
    if (!s_stopped) {
        /* The thread is still inside a vendor ioctl. Freeing the DMA target
         * or the frame buffer now would leave it writing into freed memory,
         * so keep the session and report the failure. */
        printf("[%s] capture thread did not stop in 1s; keeping buffers\n", CAM_TAG);
        return OPRT_COM_ERROR;
    }
    tkl_thread_release(s_thread);
    s_thread = NULL;
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_deinit(void)
{
    OPERATE_RET rt = tkl_dvp_stop();

    if (rt != OPRT_OK) {
        return rt;
    }

#if defined(JIELI_CAMERA_PATH_DVP) && (JIELI_CAMERA_PATH_DVP == 1)
    dvp_release();
#elif defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)
    spi_release();
#endif
    return rt;
}

#else /* not a WL82 camera build */

#if defined(JIELI_SELECTED_CHIP_WL83)

/* AC792N DVP0 (J49 SEN_FPC): the vendor video_server's video1.1 channel, the
 * same path the vendor wifi_camera application uses. The connector wiring
 * (IIC1 on PE7/PE8, data PE0-PE5, sync PE3-PE5, XCLK PD8, RESET PE9) is staged
 * into the vendor board header by configure_camera_board_header(); this file
 * only powers the module and moves frames. */

#ifndef __FS_H__
#define __FS_H__ /* The vendor fs.h redefines newlib's FILE, which tkl headers pull in. */
#endif

#include <stdint.h>
#include <string.h>

#include "server/server_core.h"
#include "server/video_server.h"
#include "device/device.h"
#include "asm/gpio.h"
#include "driver/device/gpio.h"

/* Mirrors TCFG_VIDEO1_XCLK / TCFG_VIDEO1_RESET from the staged board header,
 * which is not on the adapter's include path. The LDO rail enable is a Tuya
 * AC792N baseboard fact with no vendor macro. */
#define DVP_LDO_GPIO   IO_PORTE_06
#define DVP_XCLK_GPIO  IO_PORTD_08
#define DVP_RESET_GPIO IO_PORTE_09

extern void avdd18_enable(uint8_t level, uint8_t step);
extern void avdd28_enable(uint8_t level, uint8_t step);
extern void clk_out1(uint32_t gpio, uint32_t index);
extern void delay_us(unsigned int us);

#define DVP_TAG     "camera"
#define DVP_BUF_SIZE (256 * 1024)
/* The vendor AVI mux tags each JPEG chunk with the little-endian "00dc". */
#define DVP_CHUNK_TAG 0x63643030u

static struct server  *s_video_rec = NULL;
static uint8_t        *s_video_buf = NULL;
static TUYA_DVP_CFG_T s_cfg;
static volatile int   s_running   = 0;
static uint32_t       s_frame_id  = 0;

static void dvp_deliver_jpeg(u8 *data, u32 len)
{
    TUYA_DVP_FRAME_MANAGE_T *frame;

    if (s_cfg.inter_cfg.assign_cb == NULL || s_cfg.inter_cfg.post_cb == NULL) {
        return;
    }
    if (data == NULL || len < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        return;
    }

    frame = s_cfg.inter_cfg.assign_cb(TUYA_FRAME_FMT_JPEG, s_cfg.inter_cfg.cb_param);
    if (frame == NULL || frame->data == NULL) {
        return;
    }

    frame->frame_fmt       = TUYA_FRAME_FMT_JPEG;
    frame->frame_id        = ++s_frame_id;
    frame->width           = s_cfg.width;
    frame->height          = s_cfg.height;
    frame->is_i_frame      = 1;

    if (len > frame->data_len) {
        /* The TDL encoded pool is smaller than this frame; a truncated JPEG
         * cannot decode. Hand it over flagged incomplete so the TDD returns
         * the node to the pool instead of forwarding a torn frame. */
        static u32 oversize_log;
        if ((oversize_log++ & 0x3Fu) == 0u) {
            printf("[%s] jpeg %u B exceeds the frame pool (%u B), dropping\n", DVP_TAG, (unsigned)len,
                   (unsigned)frame->data_len);
        }
        frame->data_len          = 0;
        frame->total_frame_len   = 0;
        frame->is_frame_complete = 0;
        s_cfg.inter_cfg.post_cb(frame, s_cfg.inter_cfg.cb_param);
        return;
    }

    memcpy(frame->data, data, len);
    frame->data_len          = len;
    frame->total_frame_len   = len;
    frame->is_frame_complete = 1;

    s_cfg.inter_cfg.post_cb(frame, s_cfg.inter_cfg.cb_param);

    if (s_frame_id == 1) {
        printf("[%s] first frame: %ux%u jpeg len=%u\n", DVP_TAG, (unsigned)s_cfg.width, (unsigned)s_cfg.height,
               (unsigned)len);
    }
}

static int dvp_packet_cb(u8 type, u8 *buf, u32 size)
{
    u8  *jpeg = buf;
    u32  head0;
    u32  head8;

    if (!s_running || buf == NULL || size < 12) {
        return 0;
    }
    if (type != VIDEO_REC_JPEG_TYPE_VIDEO) {
        return 0;
    }
    /* The stream hands out AVI chunks: an 8-byte head in front of the JPEG.
     * The buffer is a byte stream, so read the tags unaligned-safely. */
    memcpy(&head0, buf, sizeof(head0));
    memcpy(&head8, buf + 8, sizeof(head8));
    if (head0 == DVP_CHUNK_TAG || head8 == 0xE0FFD8FFu || head8 == 0xC0FFD8FFu) {
        jpeg += 8;
        size -= 8;
    }

    dvp_deliver_jpeg(jpeg, size);
    return 0;
}

static void *dvp_out_open(const char *path, void *arg)
{
    (void)path;
    return arg ? arg : (void *)1;
}

static void dvp_out_close(void *fd)
{
    (void)fd;
}

static int dvp_out_send(void *fd, char *data, unsigned int len, unsigned int flag)
{
    (void)fd;
    (void)dvp_packet_cb((u8)flag, (u8 *)data, (u32)len);
    return (int)len;
}

static void dvp_power_on(void)
{
    gpio_direction_output(DVP_LDO_GPIO, 1);
    delay_us(20000);

    avdd18_enable(0, 1);
    delay_us(150);
    avdd18_enable(0, 2);

    avdd28_enable(0, 1);
    delay_us(200);
    avdd28_enable(0, 2);

    clk_out1(DVP_XCLK_GPIO, 4 /* CLK_OUT1_STD_24M */);

    gpio_direction_output(DVP_RESET_GPIO, 0);
    delay_us(20000);
    gpio_direction_output(DVP_RESET_GPIO, 1);
    delay_us(20000);
}

OPERATE_RET tkl_dvp_init(TUYA_DVP_CFG_T *dvp_cfg)
{
    if (dvp_cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    /* The vendor video server only hands out the AVI/JPEG stream; a raw or
     * H264 request would silently never produce frames. BOTH is accepted
     * because the JPEG half is exactly what this backend delivers. */
    if (dvp_cfg->output_mode != TUYA_CAMERA_OUTPUT_JPEG &&
        dvp_cfg->output_mode != TUYA_CAMERA_OUTPUT_JPEG_YUV422_BOTH) {
        return OPRT_NOT_SUPPORTED;
    }
    memcpy(&s_cfg, dvp_cfg, sizeof(s_cfg));
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_start(void)
{
    union video_req req;
    u16 w;
    u16 h;
    int err;

    if (s_running) {
        return OPRT_OK;
    }

    dvp_power_on();

    s_video_rec = server_open("video_server", "video1.1");
    if (s_video_rec == NULL) {
        printf("[%s] server_open video1.1 failed\n", DVP_TAG);
        return OPRT_COM_ERROR;
    }

    s_video_buf = (uint8_t *)tkl_system_malloc(DVP_BUF_SIZE);
    if (s_video_buf == NULL) {
        server_close(s_video_rec);
        s_video_rec = NULL;
        return OPRT_MALLOC_FAILED;
    }

    w = s_cfg.width ? s_cfg.width : 640;
    h = s_cfg.height ? s_cfg.height : 480;

    memset(&req, 0, sizeof(req));
    req.rec.camera_type = VIDEO_CAMERA_NORMAL;
    req.rec.channel     = 1;
    req.rec.width       = w;
    req.rec.src_w       = w;
    req.rec.height      = h;
    req.rec.src_h       = h;
    req.rec.state       = VIDEO_STATE_START;
    req.rec.format      = USER_VIDEO_FMT_AVI;
    req.rec.quality     = VIDEO_LOW_Q;
    req.rec.fps         = (u8)(s_cfg.fps ? s_cfg.fps : 15);
    req.rec.real_fps    = req.rec.fps;
    req.rec.abr_kbps    = 3000;
    req.rec.cycle_time  = 60;
    req.rec.enable_dri  = 0;
    req.rec.online      = 1;
    req.rec.buf         = s_video_buf;
    req.rec.buf_len     = DVP_BUF_SIZE;

    req.rec.target   = VIDEO_TO_OUT;
    strcpy(req.rec.net_par.netpath, "usr://camera_dvp0");
    req.rec.out.path  = req.rec.net_par.netpath;
    req.rec.out.arg   = (void *)1;
    req.rec.out.open  = dvp_out_open;
    req.rec.out.send  = dvp_out_send;
    req.rec.out.close = dvp_out_close;

    err = server_request(s_video_rec, VIDEO_REQ_REC, &req);
    if (err != 0) {
        printf("[%s] VIDEO_REQ_REC failed: %d\n", DVP_TAG, err);
        server_close(s_video_rec);
        s_video_rec = NULL;
        tkl_system_free(s_video_buf);
        s_video_buf = NULL;
        return OPRT_COM_ERROR;
    }

    s_running = 1;
    printf("[%s] DVP0 started (%ux%u)\n", DVP_TAG, (unsigned)w, (unsigned)h);
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_stop(void)
{
    if (!s_running) {
        return OPRT_OK;
    }
    s_running = 0;

    if (s_video_rec != NULL) {
        union video_req req;

        memset(&req, 0, sizeof(req));
        req.rec.channel = 1;
        req.rec.state   = VIDEO_STATE_STOP;
        (void)server_request(s_video_rec, VIDEO_REQ_REC, &req);
        server_close(s_video_rec);
        s_video_rec = NULL;
    }
    if (s_video_buf != NULL) {
        tkl_system_free(s_video_buf);
        s_video_buf = NULL;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_deinit(void)
{
    (void)tkl_dvp_stop();
    memset(&s_cfg, 0, sizeof(s_cfg));
    return OPRT_OK;
}

#else /* not a WL82 or WL83 camera build */

OPERATE_RET tkl_dvp_init(TUYA_DVP_CFG_T *dvp_cfg)
{
    (void)dvp_cfg;
    return OPRT_NOT_SUPPORTED;
}
OPERATE_RET tkl_dvp_deinit(void)
{
    return OPRT_NOT_SUPPORTED;
}
OPERATE_RET tkl_dvp_start(void)
{
    return OPRT_NOT_SUPPORTED;
}
OPERATE_RET tkl_dvp_stop(void)
{
    return OPRT_NOT_SUPPORTED;
}

#endif /* not a WL82 or WL83 camera build */

#endif /* not a WL82 camera build */
