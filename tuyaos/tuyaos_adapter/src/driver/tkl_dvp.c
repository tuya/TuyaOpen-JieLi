/**
 * @file tkl_dvp.c
 * @brief Jieli WL82 camera TKL: vendor video pipeline behind the Tuya DVP TDD.
 *
 * Two board paths share one TKL contract, and the board Kconfig choice picks
 * exactly one of them:
 *
 *   JIELI_BOARD_CAMERA_DVP  J1 24-pin DVP  -> dev_open("video0") + VIDIOC overlay
 *   JIELI_BOARD_CAMERA_SPI  J4 12-pin SPI  -> dev_open("spi2") for the data;
 *                                            the GC0310 is configured by the
 *                                            TDD over IIC1, no video1 device
 *
 * The board has both connectors, but PA2/PA3/PA4 and PH0-PH3 are wired to
 * both, so they cannot run together. On the DVP path the sensor is probed,
 * configured and reset by the vendor driver; on the SPI path the board's TDD
 * sensor driver owns that and this file only moves frames.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_dvp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tkl_memory.h"
#include "tkl_system.h"
#include "tkl_thread.h"
#include "tkl_mutex.h"
#include "tkl_semaphore.h"

#include "device.h"
#include "device/gpio.h"
#include "video/video.h"
#include "video/video_ioctl.h"
#include "video/camera.h"

#if defined(JIELI_SELECTED_CHIP_WL82) && ((defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)) ||          \
                                          (defined(JIELI_BOARD_CAMERA_SPI) && (JIELI_BOARD_CAMERA_SPI == 1)))

/* The board names the camera geometry; these are only the fallbacks for a
 * build that did not stage them. */
#if !defined(JIELI_BOARD_CAMERA_WIDTH)
#define JIELI_BOARD_CAMERA_WIDTH 640
#endif
#if !defined(JIELI_BOARD_CAMERA_HEIGHT)
#define JIELI_BOARD_CAMERA_HEIGHT 480
#endif

#define CAM_TAG "camera"

/* common_lib.a has no prototype in the public headers. Both paths read DMA
 * targets from the CPU, so invalidate the lines before touching them. */
extern void flushinv_dcache(void *ptr, int len);

#define DVP_CH1_BUF_NUM 2

/* One entry per channel-1 frame buffer. The block layout follows the standard
 * contract: the TUYA_MEDIA_BUF_T descriptor sits at the block start and the
 * payload begins at the 64-byte aligned TUYA_MEDIA_BUF_HDR offset, so a frame
 * can be released from the payload pointer alone. */
typedef struct {
    TUYA_MEDIA_BUF_T *buf;
    UINT8_T          *raw;  /* original allocation, kept for free() */
    UINT8_T          *block;
    UINT8_T          *data;
    UINT32_T          size;
    bool              busy;
} DVP_POOL_ITEM_T;

static TUYA_DVP_BASE_CFG_T      s_cfg;
static TUYA_MEDIA_OUTPUT_CFG_T  s_out[3];
static bool                     s_inited;
static bool                     s_started;
static UINT16_T                 s_width  = JIELI_BOARD_CAMERA_WIDTH;
static UINT16_T                 s_height = JIELI_BOARD_CAMERA_HEIGHT;
static DVP_POOL_ITEM_T          s_pool[DVP_CH1_BUF_NUM];
static TUYA_MEDIA_BUF_T        *s_ready[DVP_CH1_BUF_NUM];
static UINT8_T                  s_ready_head;
static UINT8_T                  s_ready_tail;
static UINT8_T                  s_ready_count;
static TKL_MUTEX_HANDLE         s_lock      = NULL;
static TKL_SEM_HANDLE           s_ready_sem = NULL;
static TKL_THREAD_HANDLE s_thread       = NULL;
static volatile int      s_running      = 0;
static volatile int      s_stopped      = 1;
static void             *s_video_dev    = NULL;
static UINT32_T          s_frame_id     = 0;
static UINT32_T          s_frame_count  = 0;
static UINT32_T          s_frame_drops  = 0;

static void dvp_notify(TUYA_CAMERA_EVENT_E event)
{
    if (s_cfg.event_cb != NULL) {
        s_cfg.event_cb(event, s_cfg.event_arg);
    }
}

/* Releases a delivered frame back to the TKL pool; memory itself stays
 * allocated until deinit so repeated init/deinit keeps a level footprint. */
static VOID_T dvp_pool_free_cb(TUYA_MEDIA_BUF_T *buf)
{
    DVP_POOL_ITEM_T *item;

    if (buf == NULL || buf->priv == NULL) {
        return;
    }
    item = (DVP_POOL_ITEM_T *)buf->priv;
    if (s_lock != NULL) {
        tkl_mutex_lock(s_lock);
        item->busy = false;
        tkl_mutex_unlock(s_lock);
    } else {
        item->busy = false;
    }
}

static OPERATE_RET dvp_pool_init(UINT16_T width, UINT16_T height)
{
    UINT32_T data_size = (UINT32_T)width * (UINT32_T)height * 2U;
    int      i;

    if (tkl_mutex_create_init(&s_lock) != OPRT_OK) {
        return OPRT_COM_ERROR;
    }
    if (tkl_semaphore_create_init(&s_ready_sem, 0, DVP_CH1_BUF_NUM) != OPRT_OK) {
        tkl_mutex_release(s_lock);
        s_lock = NULL;
        return OPRT_COM_ERROR;
    }
    s_ready_head = s_ready_tail = s_ready_count = 0;

    for (i = 0; i < DVP_CH1_BUF_NUM; i++) {
        DVP_POOL_ITEM_T *item = &s_pool[i];
        UINT8_T         *raw;
        UINT8_T         *aligned;

        memset(item, 0, sizeof(*item));
        raw = tkl_system_malloc(data_size + TUYA_MEDIA_BUF_HDR + 63U);
        if (raw == NULL) {
            return OPRT_MALLOC_FAILED;
        }
        aligned     = (UINT8_T *)(((uintptr_t)raw + 63U) & ~(uintptr_t)63U);
        item->raw   = raw;
        item->block = aligned;
        item->data  = aligned + TUYA_MEDIA_BUF_HDR;
        item->size  = data_size;
        item->buf   = (TUYA_MEDIA_BUF_T *)(void *)aligned;
        item->busy  = false;
    }
    return OPRT_OK;
}

static void dvp_pool_deinit(void)
{
    int i;

    for (i = 0; i < DVP_CH1_BUF_NUM; i++) {
        if (s_pool[i].raw != NULL) {
            tkl_system_free(s_pool[i].raw);
            memset(&s_pool[i], 0, sizeof(s_pool[i]));
        }
    }
    s_ready_head = s_ready_tail = s_ready_count = 0;
    if (s_ready_sem != NULL) {
        tkl_semaphore_release(s_ready_sem);
        s_ready_sem = NULL;
    }
    if (s_lock != NULL) {
        tkl_mutex_release(s_lock);
        s_lock = NULL;
    }
}

static void camera_deliver_frame(UINT8_T *data, UINT32_T len, UINT16_T width, UINT16_T height, TUYA_FRAME_FMT_E fmt)
{
    DVP_POOL_ITEM_T  *item = NULL;
    TUYA_MEDIA_BUF_T *buf;
    int               i;

    if (data == NULL || len == 0 || s_lock == NULL || s_pool[0].size == 0) {
        return;
    }
    if (len > s_pool[0].size) {
        len = s_pool[0].size;
    }

    tkl_mutex_lock(s_lock);
    for (i = 0; i < DVP_CH1_BUF_NUM; i++) {
        if (!s_pool[i].busy) {
            item       = &s_pool[i];
            item->busy = true;
            break;
        }
    }
    if (item == NULL || s_ready_count >= DVP_CH1_BUF_NUM) {
        /* Consumer is behind: refuse the frame rather than reclaim one that
         * has already been delivered. */
        tkl_mutex_unlock(s_lock);
        s_frame_drops++;
        return;
    }

    memcpy(item->data, data, len);
    buf            = item->buf;
    buf->buffer    = item->data;
    buf->length    = len;
    buf->free_cb   = dvp_pool_free_cb;
    buf->channel   = TUYA_CAMERA_FRAME_CHANNEL_1;
    buf->format    = fmt;
    buf->timestamp = (UINT64_T)tkl_system_get_millisecond();
    buf->width     = width;
    buf->height    = height;
    buf->priv      = item;
    buf->user_args = NULL;

    s_ready[s_ready_tail] = buf;
    s_ready_tail          = (UINT8_T)((s_ready_tail + 1U) % DVP_CH1_BUF_NUM);
    s_ready_count++;
    tkl_mutex_unlock(s_lock);
    tkl_semaphore_post(s_ready_sem);

    if (s_frame_count++ == 0) {
        printf("[%s] first frame: %ux%u fmt=%d len=%u\n", CAM_TAG, (unsigned)width, (unsigned)height, (int)fmt,
               (unsigned)len);
    }
}

/* ------------------------------------------------------------------------- */
/* DVP: J1 / video0                                                          */
/* ------------------------------------------------------------------------- */
#if defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)

/* DVP-only state. The overlay target must be a 32-byte aligned BSS array (the
 * ISP DMA writes it and the vendor demo uses the same storage class), and it
 * must stay inside this guard: on an SPI-only build the array would otherwise
 * reserve 614,400 bytes of SDRAM heap that the SPI frame pool needs. */
static UINT8_T          *s_yuv_buf      = NULL;
static UINT8_T s_yuv_buf_mem[(UINT32_T)JIELI_BOARD_CAMERA_WIDTH * (UINT32_T)JIELI_BOARD_CAMERA_HEIGHT * 2U] __attribute__((aligned(32)));
/* The sensor runs at the board geometry (the TDD configures it there) and the
 * TKL decimates to whatever the caller asked for, so keep the two apart. */
#define DVP_SENSOR_WIDTH  ((UINT32_T)JIELI_BOARD_CAMERA_WIDTH)
#define DVP_SENSOR_HEIGHT ((UINT32_T)JIELI_BOARD_CAMERA_HEIGHT)
#define DVP_STAGE_NUM 2

/* Timing-only sensor descriptor. The TDD configures the GC2145 over TKL I2C
 * before tkl_dvp_start(), so what the vendor video framework still needs from
 * a "sensor" is the parallel-bus shape and the geometry the ISC should latch.
 * The ops below are deliberately no-ops: touching the sensor here would undo
 * the register table the TDD just wrote. */
static INT32_T dvp_timing_sensor_check(UINT8_T isp_dev, UINT32_T reset_gpio, UINT32_T pwdn_gpio)
{
    (void)isp_dev;
    (void)reset_gpio;
    (void)pwdn_gpio;
    return 0;
}

static INT32_T dvp_timing_sensor_init(UINT8_T isp_dev, UINT16_T *width, UINT16_T *height, UINT8_T *format, UINT8_T *frame_freq)
{
    (void)isp_dev;
    (void)format;
    if (width != NULL) {
        *width = (UINT16_T)DVP_SENSOR_WIDTH;
    }
    if (height != NULL) {
        *height = (UINT16_T)DVP_SENSOR_HEIGHT;
    }
    if (frame_freq != NULL) {
        *frame_freq = (UINT8_T)JIELI_BOARD_CAMERA_FPS;
    }
    return 0;
}

static INT32_T dvp_timing_sensor_set_size_fps(UINT16_T *width, UINT16_T *height, UINT8_T *freq)
{
    (void)width;
    (void)height;
    (void)freq;
    return 0;
}

REGISTER_CAMERA(GC2145_TIMING) = {
    .logo        = "GC2145",
    .isp_dev     = ISP_DEV_NONE,
    .in_format   = SEN_IN_FORMAT_YUYV,
    .mbus_type   = SEN_MBUS_PARALLEL,
    .mbus_config = SEN_MBUS_DATA_WIDTH_8B | SEN_MBUS_HSYNC_ACTIVE_HIGH | SEN_MBUS_PCLK_SAMPLE_FALLING |
                   SEN_MBUS_VSYNC_ACTIVE_HIGH,
#if defined(JIELI_BOARD_CAMERA_HV_SYNC_SWAP) && (JIELI_BOARD_CAMERA_HV_SYNC_SWAP == 1)
    /* The vendor GC2145 descriptor sets this whenever the vendor config has
     * CONFIG_CAMERA_H_V_EXCHANGE (on for WL82/AC791): SYNC0 is wired to VSYNC
     * and SYNC1 to HSYNC. Without it the ISP only reports "isc line err" and
     * never completes a frame. */
    .sync_config  = SEN_MBUS_SYNC0_VSYNC_SYNC1_HSYNC,
#endif
    .fps          = JIELI_BOARD_CAMERA_FPS,
    .sen_size     = {(UINT16_T)DVP_SENSOR_WIDTH, (UINT16_T)DVP_SENSOR_HEIGHT},
    .cap_fps      = JIELI_BOARD_CAMERA_FPS,
    .sen_cap_size = {(UINT16_T)DVP_SENSOR_WIDTH, (UINT16_T)DVP_SENSOR_HEIGHT},
    .ops          = {
        .sensor_check = dvp_timing_sensor_check,
        .init         = dvp_timing_sensor_init,
        .set_size_fps = dvp_timing_sensor_set_size_fps,
    },
};

static UINT8_T          *s_stage[DVP_STAGE_NUM];
static UINT8_T           s_stage_count   = 0;
static UINT8_T           s_stage_tail    = 0;
static TKL_SEM_HANDLE    s_stage_sem     = NULL;
static TKL_THREAD_HANDLE s_proc_thread   = NULL;
static volatile int      s_proc_running  = 0;
static volatile int      s_proc_exited   = 1;
static int               s_first_frame_logged = 0;
static UINT8_T          *s_yuv_packed   = NULL;
static UINT32_T          s_yuv_buf_size = 0;  /* one sensor frame (overlay + staging) */
static UINT32_T          s_yuv_out_size = 0;  /* one delivered frame (cfg geometry)  */

/* Stop and join the conversion thread. A timeout keeps the handle and the
 * buffers: tkl_thread_release only requests deletion, so freeing staging
 * slots or the packed buffer while the task still converts them would be a
 * use-after-free. The caller must keep the whole session alive on failure. */
static OPERATE_RET dvp_proc_join(void)
{
    int waited;

    if (s_proc_thread == NULL) {
        return OPRT_OK;
    }

    s_proc_running = 0;
    if (s_stage_sem != NULL) {
        tkl_semaphore_post(s_stage_sem);
    }
    for (waited = 0; waited < 100 && !s_proc_exited; waited++) {
        tkl_system_sleep(10);
    }
    if (!s_proc_exited) {
        return OPRT_COM_ERROR;
    }

    tkl_thread_release(s_proc_thread);
    s_proc_thread = NULL;
    return OPRT_OK;
}

/* video0 hands back planar YUV422P (Y, then U, then V); TDL's YUV422
 * contract is the packed 4-byte/two-pixel group. Interleave here so the
 * generic example and tdl_disp_convert_yuv422_to_fb see one format on both
 * the DVP and SPI paths, instead of every consumer decoding the vendor's
 * layout itself. */
/* The pi32v2 core faults on a misaligned wide load, and the U/V planes
 * advance by one byte per two output pixels, so an optimizing compiler can
 * fold two samples into an odd-address wide load. The source pointers are
 * volatile to force one byte load per sample; the destination is written as
 * one aligned 32-bit word per pixel pair.
 *
 * The output may be an integer subdivision of the sensor frame (the board
 * configures the sensor at VGA and the panel wants 320x240): every out_w/out_h
 * ratio is a plain pixel/row skip, so the decimation costs no extra pass. */
static void yuv422p_to_packed_yuyv(const UINT8_T *src, UINT8_T *dst, UINT32_T in_w, UINT32_T in_h, UINT32_T out_w, UINT32_T out_h)
{
    size_t              y_size = (size_t)in_w * (size_t)in_h;
    const volatile UINT8_T *y_plane = (const volatile UINT8_T *)src;
    const volatile UINT8_T *u_plane = (const volatile UINT8_T *)(src + y_size);
    const volatile UINT8_T *v_plane = u_plane + y_size / 2U;
    const UINT32_T      step_x  = in_w / out_w;
    const UINT32_T      step_y  = in_h / out_h;

    for (UINT32_T row = 0; row < out_h; row++) {
        const UINT32_T     in_row = row * step_y;
        const volatile UINT8_T *y_row  = y_plane + (size_t)in_row * in_w;
        const volatile UINT8_T *u_row  = u_plane + (size_t)in_row * (in_w / 2U);
        const volatile UINT8_T *v_row  = v_plane + (size_t)in_row * (in_w / 2U);
        UINT32_T          *out    = (UINT32_T *)(void *)(dst + (size_t)row * out_w * 2U);

        for (UINT32_T col = 0; col < out_w / 2U; col++) {
            const UINT32_T sx0 = (2U * col) * step_x;
            const UINT32_T sx1 = (2U * col + 1U) * step_x;
            const UINT32_T cx  = col * step_x;
            UINT32_T  word = (UINT32_T)y_row[sx0] | ((UINT32_T)u_row[cx] << 8) | ((UINT32_T)y_row[sx1] << 16) |
                       ((UINT32_T)v_row[cx] << 24);

            out[col] = word;
        }
    }
}

/* Converts and delivers frames on its own task: the capture thread only copies
 * the vendor buffer out and releases it, so it returns to the driver quickly. */
static void dvp_process_thread(void *arg)
{
    (void)arg;

    while (s_proc_running) {
        UINT8_T idx;

        if (tkl_semaphore_wait(s_stage_sem, 100) != OPRT_OK) {
            continue;
        }
        if (!s_proc_running) {
            break;
        }
        tkl_mutex_lock(s_lock);
        if (s_stage_count == 0U) {
            tkl_mutex_unlock(s_lock);
            continue;
        }
        idx = s_stage_tail;
        tkl_mutex_unlock(s_lock);

        yuv422p_to_packed_yuyv(s_stage[idx], s_yuv_packed, DVP_SENSOR_WIDTH, DVP_SENSOR_HEIGHT, s_width, s_height);
        camera_deliver_frame(s_yuv_packed, s_yuv_out_size, s_width, s_height, TUYA_FRAME_FMT_YUV422);

        if (!s_first_frame_logged) {
            s_first_frame_logged = 1;
            printf("[%s] first frame delivered\n", CAM_TAG);
        }

        tkl_mutex_lock(s_lock);
        s_stage_tail = (UINT8_T)((s_stage_tail + 1U) % DVP_STAGE_NUM);
        s_stage_count--;
        tkl_mutex_unlock(s_lock);
    }
    s_proc_exited = 1;
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
        if (dev_ioctl(s_video_dev, CAMERA_GET_SENSOR_TYPE, (UINT32_T)logo) == 0) {
            printf("[%s] sensor: %s\n", CAM_TAG, logo);
        }
        if (dev_ioctl(s_video_dev, CAMERA_GET_SENSOR_INFO, (UINT32_T)&info) == 0) {
            printf("[%s] sensor info: %ux%u fps=%u real_fps=%u\n", CAM_TAG, (unsigned)info.width, (unsigned)info.height,
                   (unsigned)info.fps, (unsigned)info.real_fps);
        }
    }

    /* video0's open is what turns the sensor clock on (PH2 is an ISC output
     * channel) and brings the ISP up; the table the TDD wrote before
     * tkl_dvp_start() therefore landed on a sensor without a clock. Ask the
     * board to replay reset + tables now, exactly like the SPI path does after
     * arming SPI2. */
    {
        extern OPERATE_RET tkl_camera_sensor_restart(void);
        int r = tkl_camera_sensor_restart();
        printf("[%s] DVP: TDD sensor restart ret=%d\n", CAM_TAG, r);
    }

    /* The vendor DVP example keeps its overlay buffer in BSS (ALIGNE(32)),
     * not on the heap. The ISP writes this buffer from DMA, so match the
     * vendor's storage class before blaming the processing path. */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type        = VIDEO_BUF_TYPE_VIDEO_OVERLAY;
    fmt.pixelformat = VIDEO_PIX_FMT_YUV422;
    fmt.src_width   = (UINT16_T)DVP_SENSOR_WIDTH;
    fmt.src_height  = (UINT16_T)DVP_SENSOR_HEIGHT;
    fmt.win.width   = (UINT16_T)DVP_SENSOR_WIDTH;
    fmt.win.height  = (UINT16_T)DVP_SENSOR_HEIGHT;
    fmt.fps         = s_cfg.dvp_cfg.fps;
    fmt.static_buf  = s_yuv_buf;
    fmt.sbuf_size   = s_yuv_buf_size;

    err = dev_ioctl(s_video_dev, VIDIOC_SET_FMT, (UINT32_T)&fmt);
    if (err) {
        printf("[%s] VIDIOC_SET_FMT failed: %d\n", CAM_TAG, err);
        goto out;
    }
    err = dev_ioctl(s_video_dev, VIDIOC_SET_OVERLAY, 0);
    if (err) {
        printf("[%s] VIDIOC_SET_OVERLAY failed: %d\n", CAM_TAG, err);
        goto out;
    }
    printf("[%s] SET_FMT ok, static overlay buf=%p/%u\n", CAM_TAG, (void *)s_yuv_buf, (unsigned)s_yuv_buf_size);

    printf("[%s] DVP capture started\n", CAM_TAG);
    {
        while (s_running) {
            UINT32_T len;
            UINT8_T slot   = DVP_STAGE_NUM;
            int     dropped = 0;

            /* Hybrid mode: SET_FMT registered the BSS buffer and SET_OVERLAY(0)
             * asks the driver to fill it; GET hands back the same address. The
             * struct is zeroed first so the fallbacks below only trigger for a
             * driver that leaves a field untouched. */
            memset(&yuv, 0, sizeof(yuv));
            err = dev_ioctl(s_video_dev, VIDIOC_GET_OVERLAY, (UINT32_T)&yuv);
            if (err) {
                tkl_system_sleep(5);
                continue;
            }
            if (yuv.addr == NULL) {
                yuv.addr = s_yuv_buf;
            }
            if (yuv.size == 0U) {
                yuv.size = s_yuv_buf_size;
            }
            len = (yuv.size < s_yuv_buf_size) ? yuv.size : s_yuv_buf_size;

            /* Copy while holding the lock: the consumer may only see a slot
             * after its bytes are complete. The copy is ~1 ms, the conversion
             * runs on dvp_process_thread. */
            tkl_mutex_lock(s_lock);
            if (s_stage_count < DVP_STAGE_NUM) {
                slot = (UINT8_T)((s_stage_tail + s_stage_count) % DVP_STAGE_NUM);
                flushinv_dcache(yuv.addr, (int)len);
                memcpy(s_stage[slot], yuv.addr, len);
                s_stage_count++;
            } else {
                dropped = 1;
            }
            tkl_mutex_unlock(s_lock);

            dev_ioctl(s_video_dev, VIDIOC_CLEAR_OVERLAY, 0);

            if (dropped) {
                s_frame_drops++;
                continue;
            }
            tkl_semaphore_post(s_stage_sem);
        }
    }

out:
    dev_close(s_video_dev);
    s_video_dev = NULL;
    printf("[%s] DVP capture stopped\n", CAM_TAG);
    s_stopped = 1;
}

static void dvp_release(void);

static OPERATE_RET dvp_prepare(void)
{
    int i;

    s_yuv_buf_size = DVP_SENSOR_WIDTH * DVP_SENSOR_HEIGHT * 2U;
    s_yuv_out_size = (UINT32_T)s_width * (UINT32_T)s_height * 2U;
    s_yuv_packed   = tkl_system_malloc(s_yuv_out_size);
    for (i = 0; i < DVP_STAGE_NUM; i++) {
        s_stage[i] = tkl_system_malloc(s_yuv_buf_size);
    }
    if (s_yuv_packed == NULL) {
        printf("[%s] no memory for %u-byte YUV422 buffer\n", CAM_TAG, (unsigned)s_yuv_out_size);
        for (i = 0; i < DVP_STAGE_NUM; i++) {
            if (s_stage[i] != NULL) {
                tkl_system_free(s_stage[i]);
                s_stage[i] = NULL;
            }
        }
        return OPRT_MALLOC_FAILED;
    }
    for (i = 0; i < DVP_STAGE_NUM; i++) {
        if (s_stage[i] == NULL) {
            dvp_release();
            return OPRT_MALLOC_FAILED;
        }
    }
    if (tkl_semaphore_create_init(&s_stage_sem, 0, DVP_STAGE_NUM) != OPRT_OK) {
        dvp_release();
        return OPRT_COM_ERROR;
    }
    s_stage_count = 0;
    s_stage_tail  = 0;
    s_yuv_buf     = s_yuv_buf_mem;
    return OPRT_OK;
}

static void dvp_release(void)
{
    int i;

    s_yuv_buf = NULL;
    for (i = 0; i < DVP_STAGE_NUM; i++) {
        if (s_stage[i] != NULL) {
            tkl_system_free(s_stage[i]);
            s_stage[i] = NULL;
        }
    }
    s_stage_count = 0;
    s_stage_tail  = 0;
    if (s_stage_sem != NULL) {
        tkl_semaphore_release(s_stage_sem);
        s_stage_sem = NULL;
    }
    if (s_yuv_packed != NULL) {
        tkl_system_free(s_yuv_packed);
        s_yuv_packed = NULL;
    }
}
#endif /* JIELI_BOARD_CAMERA_DVP */

/* ------------------------------------------------------------------------- */
/* SPI: J4 / spi2 + TDD sensor                                              */
/* ------------------------------------------------------------------------- */
#if defined(JIELI_BOARD_CAMERA_SPI) && (JIELI_BOARD_CAMERA_SPI == 1)

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
#define SPI_OUT_BYTES     ((UINT32_T)JIELI_BOARD_CAMERA_WIDTH * (UINT32_T)JIELI_BOARD_CAMERA_HEIGHT * 2U)

/* The DMA buffer used to be a 311 KiB static .sram array (two packets). That
 * fits the link, but it sits in .ram0_bss from reset, cutting the internal
 * heap from ~464 KiB to ~152 KiB before the vendor network/BT stacks run.
 * Allocate one packet on demand instead: the heap is whole at boot and the
 * camera pays for the buffer only when it starts. */
static UINT8_T *s_spi_buf;
static UINT8_T *s_spi_buf_raw;
static UINT32_T s_spi_buf_size;
static UINT8_T *s_spi_frame;
static UINT8_T *s_spi_out;
static void *s_spi_dev    = NULL;
/* Set by the capture thread once SPI2 is armed. */
static volatile int s_spi_armed = 0;

static UINT8_T *spi_aligned_alloc(UINT32_T size, UINT8_T **raw_out)
{
    UINT8_T *raw = tkl_system_malloc(size + 32U);
    if (raw == NULL) {
        return NULL;
    }
    *raw_out = raw;
    return (UINT8_T *)(((uintptr_t)raw + 31U) & ~(uintptr_t)31U);
}

static int spi_tag_eq(const UINT8_T *p, UINT8_T b3)
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
extern UINT8_T dma_copy_async(void *dst, void *src, int len);

#define SPI_IRQ_LINE_MID    (4U + SPI_LINE_BYTES + 4U)  /* LHEAD + payload + LEND */
#define SPI_IRQ_LINE_FIRST  (4U + SPI_IRQ_LINE_MID)     /* FHEAD + line           */
#define SPI_IRQ_BUF_NUM     8U
#define SPI_IRQ_FRAME_NUM   3U
#define SPI_IRQ_TAG_FHEAD   0xab0000ffU /* little-endian ff0000ab */
#define SPI_IRQ_TAG_LHEAD   0x800000ffU /* little-endian ff000080 */

static UINT8_T          *s_irq_lines;
static UINT8_T          *s_irq_frames[SPI_IRQ_FRAME_NUM];
static UINT8_T          *s_irq_out;
static volatile int      s_irq_active;
static volatile UINT32_T s_irq_armed;
static volatile UINT32_T s_irq_row;
static volatile UINT32_T s_irq_wr;
static volatile UINT32_T s_irq_ready; /* completed frame index + 1, 0 = none */
static volatile UINT32_T s_irq_lines_done;
static volatile UINT32_T s_irq_tag_err;

static ___interrupt void spi_line_isr(void) sec(.volatile_ram_code)
{
    UINT32_T row = s_irq_row;
    UINT8_T *buf = s_irq_lines + (UINT32_T)s_irq_armed * SPI_IRQ_LINE_FIRST;
    UINT32_T tag = *(volatile UINT32_T *)buf;

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
        JL_SPI2->ADR = (UINT32_T)(s_irq_lines + (UINT32_T)s_irq_armed * SPI_IRQ_LINE_FIRST);
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
static UINT32_T s_spi_line;
static int s_spi_assembling;
static UINT32_T s_frame_start_ms;
static UINT8_T s_line_carry[SPI_LINE_BYTES];
static UINT32_T s_line_carry_len;
static int s_line_pending;

static UINT32_T s_dbg_frames;
static UINT32_T s_dbg_skipped;

/* A real line start is followed by its end tag at a fixed offset, or (when
 * that tag itself was hit by a byte slip) by the next line head at the same
 * fixed offset from the following word. YUYV pixel data can spell
 * 0xFF000080 (Y=255,U=0,Y=0,V=128) in saturated scenes, so a bare tag match is
 * not proof of a row boundary; requiring the framing around it is. */
static int spi_line_start_ok(const UINT8_T *p, const UINT8_T *end)
{
    /* p is the start tag; the end tag belongs at p + 4 + SPI_LINE_BYTES.
     * Accept it within a small window: the link shifts framing bytes. */
    for (int k = -6; k <= 6; k++) {
        const UINT8_T *t = p + 4 + SPI_LINE_BYTES + k;

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
static int spi_frame_to_preview(const UINT8_T *src)
{
    const UINT32_T sw = SPI_SENSOR_WIDTH;
    const UINT32_T sh = SPI_SENSOR_HEIGHT;
    const UINT32_T dw = (UINT32_T)JIELI_BOARD_CAMERA_WIDTH;
    const UINT32_T dh = (UINT32_T)JIELI_BOARD_CAMERA_HEIGHT;

    if (dw == 0 || dh == 0 || (dw & 1U) || (sw % dw) != 0 || (sh % dh) != 0) {
        /* No integer ratio: report it so the caller drops the frame instead
         * of delivering the previous preview as if it were this one. */
        return 0;
    }
    for (UINT32_T y = 0; y < dh; y++) {
        const UINT32_T sy   = y * (sh / dh);
        const UINT8_T *srow = src + (size_t)sy * SPI_LINE_BYTES;
        UINT8_T   *drow = s_spi_out + (size_t)y * dw * 2U;

        for (UINT32_T x = 0; x < dw; x += 2U) {
            const UINT32_T sx = x * (sw / dw);
            const UINT8_T *s  = srow + (size_t)sx * 2U;
            UINT8_T   *d  = drow + (size_t)x * 2U;

            d[0] = s[0]; /* Y(sx)   */
            d[1] = s[1]; /* U(sx)   */
            d[2] = s[4]; /* Y(sx+2) */
            d[3] = s[3]; /* V(sx)   */
        }
    }
    return 1;
}

static int spi_consume_packet(const UINT8_T *buf, int len)
{
    const UINT8_T *p         = buf;
    const UINT8_T *end       = buf + len;
    int       delivered = 0;

    /* Finish the line that started at the end of the previous block. */
    if (s_line_pending && s_spi_assembling && s_spi_line < (UINT32_T)SPI_SENSOR_HEIGHT) {
        UINT32_T need  = SPI_LINE_BYTES - s_line_carry_len;
        UINT32_T avail = (UINT32_T)(end - p);
        UINT32_T take  = (avail < need) ? avail : need;

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
        const UINT8_T *ff = memchr(p, 0xff, (size_t)(end - p));

        if (ff == NULL || ff + 4 > end) {
            break;
        }
        p = ff;
        if (spi_tag_eq(p, 0xab)) {
            s_spi_line       = 0;
            s_spi_assembling = 1;
            s_line_pending   = 0;
            s_frame_start_ms = (UINT32_T)tkl_system_get_millisecond();
            p += 4;
        } else if (spi_tag_eq(p, 0x80)) {
            if (s_spi_assembling && s_spi_line < (UINT32_T)SPI_SENSOR_HEIGHT) {
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
                    UINT32_T avail = (UINT32_T)(end - (p + 4));

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
                (UINT32_T)tkl_system_get_millisecond() - s_frame_start_ms < 400u) {
                /* A frame missing its last few rows is completed with the
                 * previous row rather than dropped: on this link a single byte
                 * slip per frame is common, and rejecting every shortened
                 * frame left the panel stuck on the last good picture. Grossly
                 * short assemblies (more than four rows) are still dropped. */
                if (s_spi_line >= (UINT32_T)SPI_SENSOR_HEIGHT) {
                    s_spi_line = (UINT32_T)SPI_SENSOR_HEIGHT;
                } else if (s_spi_line >= (UINT32_T)SPI_SENSOR_HEIGHT - 4u) {
                    const UINT8_T *last = s_spi_frame + (s_spi_line - 1u) * SPI_LINE_BYTES;

                    while (s_spi_line < (UINT32_T)SPI_SENSOR_HEIGHT) {
                        memcpy(s_spi_frame + s_spi_line * SPI_LINE_BYTES, last, SPI_LINE_BYTES);
                        s_spi_line++;
                    }
                }
                if (s_spi_line == (UINT32_T)SPI_SENSOR_HEIGHT) {
                    /* The SPI DMA fills SDRAM behind the CPU cache; drop any
                     * lines this buffer kept from an earlier delivery. */
                    flushinv_dcache(s_spi_frame, SPI_FRAME_BYTES);
                    if (JIELI_BOARD_CAMERA_WIDTH == SPI_SENSOR_WIDTH && JIELI_BOARD_CAMERA_HEIGHT == SPI_SENSOR_HEIGHT) {
                        camera_deliver_frame(s_spi_frame, SPI_FRAME_BYTES, SPI_SENSOR_WIDTH, SPI_SENSOR_HEIGHT,
                                             TUYA_FRAME_FMT_YUV422);
                        s_dbg_frames++;
                        delivered = 1;
                    } else if (spi_frame_to_preview(s_spi_frame)) {
                        camera_deliver_frame(s_spi_out, SPI_OUT_BYTES, JIELI_BOARD_CAMERA_WIDTH, JIELI_BOARD_CAMERA_HEIGHT,
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
    JL_SPI2->ADR = (UINT32_T)s_irq_lines;
    JL_SPI2->CNT = SPI_IRQ_LINE_FIRST;
    tkl_system_sleep(20); /* vendor's settle time between arm and sensor head */
    {
        extern OPERATE_RET tkl_camera_sensor_restart(void);
        (void)tkl_camera_sensor_restart();
    }
    JL_SPI2->CON |= BIT(13);
}
#endif

static void spi_capture_thread(void *arg)
{
#if JIELI_SPI_LINE_IRQ
    UINT32_T last_ms = 0;
#else
    struct spi_user su;
    UINT8_T        *read_addr;
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
    /* The sensor is configured by the TDD over IIC1 (see board_com_api.c),
     * so SPI2 is the only device this file opens: no vendor video1 device is
     * involved. */

    printf("[%s] SPI: opening spi2\n", CAM_TAG);
    s_spi_dev = dev_open("spi2", NULL);
    if (s_spi_dev == NULL) {
        printf("[%s] spi2 open failed\n", CAM_TAG);
        goto out_sensor;
    }
    printf("[%s] SPI: spi2 ok\n", CAM_TAG);

#if JIELI_SPI_LINE_IRQ
    dev_ioctl(s_spi_dev, IOCTL_SPI_SET_IRQ_CPU_ID, 1);
    dev_ioctl(s_spi_dev, IOCTL_SPI_SET_IRQ_FUNC, (UINT32_T)spi_line_isr);
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
    dev_ioctl(s_spi_dev, IOCTL_SPI_SET_USER_INFO, (UINT32_T)&su);
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
    JL_SPI2->ADR = (UINT32_T)s_irq_lines;
    JL_SPI2->CNT = SPI_IRQ_LINE_FIRST;

    s_spi_armed = 1;
    printf("[%s] SPI capture started (%ux%u, line IRQ, first %u B / line %u B)\n", CAM_TAG, SPI_SENSOR_WIDTH,
           SPI_SENSOR_HEIGHT, (unsigned)SPI_IRQ_LINE_FIRST, (unsigned)SPI_IRQ_LINE_MID);

    /* The board provides this (see board_com_api.c); it resets and re-runs the
     * TDD sensor register table so the first clock SPI2 sees is a frame head. */
    {
        extern OPERATE_RET tkl_camera_sensor_restart(void);
        int r = tkl_camera_sensor_restart();
        printf("[%s] SPI: TDD sensor restart ret=%d\n", CAM_TAG, r);
    }

    while (s_running) {
        if (s_irq_ready) {
            UINT32_T idx = s_irq_ready - 1u;

            s_irq_ready = 0;
            /* The SPI DMA fills SDRAM behind the CPU cache; drop any lines
             * this frame buffer kept from an earlier delivery. */
            flushinv_dcache(s_irq_frames[idx], SPI_FRAME_BYTES);
            if (JIELI_BOARD_CAMERA_WIDTH == SPI_SENSOR_WIDTH && JIELI_BOARD_CAMERA_HEIGHT == SPI_SENSOR_HEIGHT) {
                camera_deliver_frame(s_irq_frames[idx], SPI_FRAME_BYTES, SPI_SENSOR_WIDTH, SPI_SENSOR_HEIGHT,
                                     TUYA_FRAME_FMT_YUV422);
                s_dbg_frames++;
            } else if (spi_frame_to_preview(s_irq_frames[idx])) {
                camera_deliver_frame(s_irq_out, SPI_OUT_BYTES, JIELI_BOARD_CAMERA_WIDTH, JIELI_BOARD_CAMERA_HEIGHT,
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
            static UINT32_T last_tag_err;
            static UINT32_T last_resync_ms;
            UINT32_T   tag_err = s_irq_tag_err;

            if (tag_err != last_tag_err) {
                UINT32_T now = (UINT32_T)tkl_system_get_millisecond();

                last_tag_err = tag_err;
                if (now - last_resync_ms >= 200u) {
                    last_resync_ms = now;
                    printf("[%s] SPI: tag error %u, resyncing\n", CAM_TAG, (unsigned)tag_err);
                    spi_line_resync();
                }
            }
        }
        {
            UINT32_T now_ms = (UINT32_T)tkl_system_get_millisecond();

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
        ret       = dev_ioctl(s_spi_dev, IOCTL_SPI_READ_DATA, (UINT32_T)&read_addr);
        if (ret <= 0 || read_addr == NULL) {
            static UINT32_T idle;
            if ((idle++ & 0x3fffffu) == 0u) {
                printf("[%s] SPI idle, no data from sensor yet\n", CAM_TAG);
            }
            continue;
        }
        (void)spi_consume_packet(read_addr, ret);
        {
            static UINT32_T dbg_reads;
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
            static UINT32_T last_ms;
            UINT32_T   now_ms = (UINT32_T)tkl_system_get_millisecond();
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
    gpio_clear_output_channle(IO_PORTH_02, CH1_PLL_24M);
    printf("[%s] SPI capture stopped\n", CAM_TAG);
    s_stopped = 1;
}

static OPERATE_RET spi_prepare(void)
{
    s_spi_armed      = 0;
    s_dbg_frames     = 0;
    s_dbg_skipped    = 0;

    /* The sensor is configured over IIC1 by the TDD before tkl_dvp_start(),
     * so it needs its MCLK from init() rather than from a vendor video1
     * open. Re-asserting the channel later is harmless. */
    printf("[%s] SPI: output XCLK PH2 early for TDD sensor init\n", CAM_TAG);
    gpio_output_channle(IO_PORTH_02, CH1_PLL_24M);
    tkl_system_sleep(5);

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
    for (UINT32_T i = 0; i < SPI_IRQ_FRAME_NUM; i++) {
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
    if (JIELI_BOARD_CAMERA_WIDTH != SPI_SENSOR_WIDTH || JIELI_BOARD_CAMERA_HEIGHT != SPI_SENSOR_HEIGHT) {
        s_irq_out = tkl_system_malloc(SPI_OUT_BYTES);
        if (s_irq_out == NULL) {
            printf("[%s] no memory for %u-byte preview frame\n", CAM_TAG, (unsigned)SPI_OUT_BYTES);
            for (UINT32_T i = 0; i < SPI_IRQ_FRAME_NUM; i++) {
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
    if (JIELI_BOARD_CAMERA_WIDTH != SPI_SENSOR_WIDTH || JIELI_BOARD_CAMERA_HEIGHT != SPI_SENSOR_HEIGHT) {
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
    for (UINT32_T i = 0; i < SPI_IRQ_FRAME_NUM; i++) {
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
#endif /* JIELI_BOARD_CAMERA_SPI */

/* ------------------------------------------------------------------------- */
/* TKL entry points                                                          */
/* ------------------------------------------------------------------------- */
OPERATE_RET tkl_dvp_init(const TUYA_DVP_BASE_CFG_T *cfg)
{
    OPERATE_RET rt;
    UINT8_T     i;
    bool        ch1 = false;

    if (cfg == NULL || cfg->output_num == 0U || cfg->outputs == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (s_inited) {
        return OPRT_INIT_MORE_THAN_ONCE;
    }
    if ((unsigned)cfg->sync_polarity > 3U) {
        return OPRT_INVALID_PARM;
    }
    if (cfg->dvp_cfg.width == 0U || cfg->dvp_cfg.height == 0U || cfg->dvp_cfg.fps == 0U) {
        return OPRT_INVALID_PARM;
    }
    /* Planar YUV422 has no odd-width chroma, and the DVP packer stores one
     * aligned 32-bit word per pixel pair (an odd width would leave every
     * other row's destination 2-byte aligned on pi32v2). */
    if ((cfg->dvp_cfg.width & 1U) != 0U) {
        return OPRT_NOT_SUPPORTED;
    }
    /* The WL82 vendor path delivers packed YUV422 from a sensor the TDD has
     * already configured; it does not produce any other input format. */
    if (cfg->dvp_cfg.format != TUYA_FRAME_FMT_YUV422) {
        return OPRT_INVALID_PARM;
    }
    /* The sensor is configured by the TDD at the board geometry; the caller may
     * ask for an integer subdivision of it (the 320x480 panel wants half VGA),
     * which the conversion thread decimates. */
    if (cfg->dvp_cfg.width == 0U || cfg->dvp_cfg.height == 0U ||
        ((UINT32_T)JIELI_BOARD_CAMERA_WIDTH % cfg->dvp_cfg.width) != 0U ||
        ((UINT32_T)JIELI_BOARD_CAMERA_HEIGHT % cfg->dvp_cfg.height) != 0U) {
        return OPRT_NOT_SUPPORTED;
    }

    for (i = 0; i < cfg->output_num; i++) {
        const TUYA_MEDIA_OUTPUT_CFG_T *out = &cfg->outputs[i];

        switch (out->channel) {
        case TUYA_CAMERA_FRAME_CHANNEL_1:
            if (ch1) {
                return OPRT_INVALID_PARM;
            }
            if (out->format != TUYA_FRAME_FMT_YUV422) {
                return OPRT_INVALID_PARM;
            }
            if (out->gop != 0U || out->level != TUYA_ENCODE_BITRATE_LEVEL_LOW) {
                return OPRT_INVALID_PARM;
            }
            if (out->rotate != TUYA_FRAME_ROTATE_0) {
                return OPRT_NOT_SUPPORTED;
            }
            if ((out->width != 0U && out->width != cfg->dvp_cfg.width) ||
                (out->height != 0U && out->height != cfg->dvp_cfg.height)) {
                return OPRT_NOT_SUPPORTED;
            }
            ch1 = true;
            break;
        case TUYA_CAMERA_FRAME_CHANNEL_2:
        case TUYA_CAMERA_FRAME_CHANNEL_3:
            /* No RGB display or H.264 encode output on this platform. */
            return OPRT_NOT_SUPPORTED;
        default:
            return OPRT_INVALID_PARM;
        }
    }
    if (!ch1) {
        return OPRT_NOT_SUPPORTED;
    }

    /* Deep-copy the config and the outputs array; cb/arg stay APP-owned. */
    s_cfg = *cfg;
    for (i = 0; i < cfg->output_num && i < (UINT8_T)(sizeof(s_out) / sizeof(s_out[0])); i++) {
        s_out[i] = cfg->outputs[i];
    }
    s_cfg.outputs = s_out;
    s_width       = cfg->dvp_cfg.width;
    s_height      = cfg->dvp_cfg.height;
    s_frame_id    = 0;
    s_frame_count = 0;
    s_frame_drops = 0;

    rt = dvp_pool_init(s_width, s_height);
    if (rt != OPRT_OK) {
        dvp_pool_deinit();
        memset(&s_cfg, 0, sizeof(s_cfg));
        return rt;
    }

#if defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)
    rt = dvp_prepare();
#elif defined(JIELI_BOARD_CAMERA_SPI) && (JIELI_BOARD_CAMERA_SPI == 1)
    rt = spi_prepare();
#else
    rt = OPRT_NOT_SUPPORTED;
#endif
    if (rt != OPRT_OK) {
        dvp_pool_deinit();
        memset(&s_cfg, 0, sizeof(s_cfg));
        return rt;
    }

    s_inited  = true;
    s_started = false;
    dvp_notify(TUYA_CAMERA_EVENT_INITIALIZED);
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_start(void)
{
    if (!s_inited) {
        return OPRT_COM_ERROR;
    }
    if (s_thread != NULL) {
        if (!s_stopped) {
            s_started = true;
            return OPRT_OK; /* already running */
        }
        /* The thread exited on its own (open failure). Release the stale
         * handle so this call can start a fresh session, and make sure the
         * conversion thread of that session is gone before a new one shares
         * the staging ring. */
        tkl_thread_release(s_thread);
        s_thread = NULL;
#if defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)
        if (dvp_proc_join() != OPRT_OK) {
            printf("[%s] previous proc thread still running; start refused\n", CAM_TAG);
            return OPRT_COM_ERROR;
        }
#endif
    }

#if defined(JIELI_BOARD_CAMERA_SPI) && (JIELI_BOARD_CAMERA_SPI == 1)
    /* Per-session handshake state: the capture thread re-arms SPI2 before the
     * sensor restart this code exists to guarantee. */
    s_spi_armed = 0;
#endif

    s_running = 1;
    s_stopped = 0;

#if defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)
    if (tkl_thread_create(&s_thread, "cam_dvp", 4096, 4, dvp_capture_thread, NULL) != OPRT_OK)
#elif defined(JIELI_BOARD_CAMERA_SPI) && (JIELI_BOARD_CAMERA_SPI == 1)
    if (tkl_thread_create(&s_thread, "cam_spi", 4096, 4, spi_capture_thread, NULL) != OPRT_OK)
#else
    if (1)
#endif
    {
        s_running = 0;
        return OPRT_COM_ERROR;
    }
#if defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)
    /* A stop may leave undelivered frames staged; drop them so a restart
     * starts from a clean queue. */
    tkl_mutex_lock(s_lock);
    s_stage_count = 0;
    s_stage_tail  = 0;
    s_first_frame_logged = 0;
    tkl_mutex_unlock(s_lock);
    s_proc_running = 1;
    s_proc_exited  = 0;
    if (tkl_thread_create(&s_proc_thread, "cam_proc", 4096, 5, dvp_process_thread, NULL) != OPRT_OK) {
        int waited;

        s_proc_running = 0;
        s_proc_exited  = 1;
        s_running      = 0;
        /* tkl_thread_release only requests deletion and frees the argument
         * immediately, so the capture task must have seen s_running = 0 and
         * returned before its handle is released. */
        for (waited = 0; waited < 100 && !s_stopped; waited++) {
            tkl_system_sleep(10);
        }
        if (s_stopped) {
            tkl_thread_release(s_thread);
            s_thread = NULL;
        }
        return OPRT_COM_ERROR;
    }
#endif
    s_started = true;
    dvp_notify(TUYA_CAMERA_EVENT_STARTED);
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_stop(void)
{
    int waited;

    if (!s_inited) {
        return OPRT_COM_ERROR;
    }
    if (s_thread == NULL) {
        s_started = false;
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
         * now would leave it writing into freed memory, so keep the session
         * and report the failure. */
        printf("[%s] capture thread did not stop in 1s; keeping buffers\n", CAM_TAG);
        return OPRT_COM_ERROR;
    }
#if defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)
    /* Join the conversion thread before releasing the capture handle: a
     * failed join must keep the session (and the staging buffers) alive so a
     * retried stop or deinit does not free memory the task still converts. */
    if (dvp_proc_join() != OPRT_OK) {
        printf("[%s] proc thread did not stop in 1s; keeping buffers\n", CAM_TAG);
        return OPRT_COM_ERROR;
    }
#endif
    tkl_thread_release(s_thread);
    s_thread  = NULL;
    s_started = false;
    dvp_notify(TUYA_CAMERA_EVENT_STOPPED);
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_deinit(void)
{
    TUYA_CAMERA_EVENT_CB event_cb;
    VOID_T              *event_arg;
    OPERATE_RET          rt;

    if (!s_inited) {
        return OPRT_COM_ERROR;
    }
    rt = tkl_dvp_stop();
    if (rt != OPRT_OK) {
        return rt;
    }

#if defined(JIELI_BOARD_CAMERA_DVP) && (JIELI_BOARD_CAMERA_DVP == 1)
    dvp_release();
#elif defined(JIELI_BOARD_CAMERA_SPI) && (JIELI_BOARD_CAMERA_SPI == 1)
    spi_release();
#endif

    event_cb  = s_cfg.event_cb;
    event_arg = s_cfg.event_arg;
    dvp_pool_deinit();
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_inited  = false;
    s_started = false;
    if (event_cb != NULL) {
        event_cb(TUYA_CAMERA_EVENT_DEINITIALIZED, event_arg);
    }
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_get_frame(TUYA_CAMERA_FRAME_CHANNEL_E channel, TUYA_MEDIA_BUF_T **out, UINT32_T timeout_ms)
{
    if (out == NULL) {
        return OPRT_INVALID_PARM;
    }
    *out = NULL;
    if (channel == TUYA_CAMERA_FRAME_CHANNEL_2 || channel == TUYA_CAMERA_FRAME_CHANNEL_3) {
        return OPRT_NOT_SUPPORTED;
    }
    if (channel != TUYA_CAMERA_FRAME_CHANNEL_1) {
        return OPRT_INVALID_PARM;
    }
    if (!s_inited) {
        return OPRT_COM_ERROR;
    }
    if (!s_started) {
        return OPRT_TIMEOUT;
    }

    for (;;) {
        tkl_mutex_lock(s_lock);
        if (s_ready_count > 0U) {
            TUYA_MEDIA_BUF_T *buf = s_ready[s_ready_head];

            s_ready_head = (UINT8_T)((s_ready_head + 1U) % DVP_CH1_BUF_NUM);
            s_ready_count--;
            tkl_mutex_unlock(s_lock);
            *out = buf;
            return OPRT_OK;
        }
        tkl_mutex_unlock(s_lock);

        if (tkl_semaphore_wait(s_ready_sem, timeout_ms) != OPRT_OK) {
            return OPRT_TIMEOUT;
        }
        if (!s_started) {
            return OPRT_TIMEOUT;
        }
    }
}

OPERATE_RET tkl_dvp_request_idr(void)
{
    /* No H.264 encode channel exists on this platform. */
    return OPRT_NOT_SUPPORTED;
}

#else /* not a WL82 camera build */

OPERATE_RET tkl_dvp_init(const TUYA_DVP_BASE_CFG_T *cfg)
{
    (void)cfg;
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
OPERATE_RET tkl_dvp_get_frame(TUYA_CAMERA_FRAME_CHANNEL_E channel, TUYA_MEDIA_BUF_T **out, UINT32_T timeout_ms)
{
    (void)channel;
    (void)out;
    (void)timeout_ms;
    return OPRT_NOT_SUPPORTED;
}
OPERATE_RET tkl_dvp_request_idr(void)
{
    return OPRT_NOT_SUPPORTED;
}

#endif
