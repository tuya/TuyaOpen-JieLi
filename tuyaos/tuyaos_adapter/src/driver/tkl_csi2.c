/**
 * @file tkl_csi2.c
 * @brief Jieli WL83 MIPI CSI-2 camera TKL (AC792N J47 MIPI_FPC).
 *
 * The vendor video_server owns the CSI-2 receiver: video0.1 is the channel
 * the vendor wifi_camera application opens for the MIPI connector. The
 * connector wiring (IIC2 on PE13/PE14, XCLK PE10, PWDN PE11, RESET PE12) is
 * staged into the vendor board header by configure_camera_board_header();
 * this file only powers the module and hands the stream to the Tuya TDD.
 *
 * DVP0 (J49) and CSI-2 (J47) share PD11-PD14, so a board populates one or the
 * other; the board Kconfig enforces that choice.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_csi2.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tkl_memory.h"
#include "tuya_error_code.h"

#if defined(JIELI_SELECTED_CHIP_WL83)

#ifndef __FS_H__
#define __FS_H__ /* The vendor fs.h redefines newlib's FILE, which tkl headers pull in. */
#endif

#include "server/server_core.h"
#include "server/video_server.h"
#include "device/device.h"
#include "asm/gpio.h"
#include "driver/device/gpio.h"

/* Mirrors TCFG_VIDEO0_* in the staged board header. */
#define CSI2_XCLK_GPIO  IO_PORTE_10
#define CSI2_PWDN_GPIO  IO_PORTE_11
#define CSI2_RESET_GPIO IO_PORTE_12

extern void avdd18_enable(uint8_t level, uint8_t step);
extern void avdd28_enable(uint8_t level, uint8_t step);
extern void clk_out0(uint32_t gpio, uint32_t index);
extern void delay_us(unsigned int us);

#define CSI2_TAG      "camera"
#define CSI2_BUF_SIZE (256 * 1024)
#define CSI2_CHUNK_TAG 0x63643030u

static struct server  *s_video_rec = NULL;
static uint8_t        *s_video_buf = NULL;
static TUYA_DVP_CFG_T  s_cfg;
static int             s_running = 0;
static uint32_t        s_frame_id = 0;

static void csi2_deliver_jpeg(u8 *data, u32 len)
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
    if (len > frame->data_len) {
        len = frame->data_len;
    }

    memcpy(frame->data, data, len);
    frame->frame_fmt         = TUYA_FRAME_FMT_JPEG;
    frame->frame_id          = ++s_frame_id;
    frame->width             = s_cfg.width;
    frame->height            = s_cfg.height;
    frame->data_len          = len;
    frame->total_frame_len   = len;
    frame->is_frame_complete = 1;
    frame->is_i_frame        = 1;

    s_cfg.inter_cfg.post_cb(frame, s_cfg.inter_cfg.cb_param);

    if (s_frame_id == 1) {
        printf("[%s] first CSI-2 frame: %ux%u jpeg len=%u\n", CSI2_TAG, (unsigned)s_cfg.width,
               (unsigned)s_cfg.height, (unsigned)len);
    }
}

static int csi2_packet_cb(u8 type, u8 *buf, u32 size)
{
    u8 *jpeg = buf;

    if (!s_running || buf == NULL || size < 8) {
        return 0;
    }
    if (type != VIDEO_REC_JPEG_TYPE_VIDEO && type != 2) {
        return 0;
    }
    if (*(u32 *)buf == CSI2_CHUNK_TAG || *(u32 *)(buf + 8) == 0xE0FFD8FFu ||
        *(u32 *)(buf + 8) == 0xC0FFD8FFu) {
        jpeg += 8;
        size -= 8;
    }

    csi2_deliver_jpeg(jpeg, size);
    return 0;
}

static void *csi2_out_open(const char *path, void *arg)
{
    (void)path;
    return arg ? arg : (void *)1;
}

static void csi2_out_close(void *fd)
{
    (void)fd;
}

static int csi2_out_send(void *fd, char *data, unsigned int len, unsigned int flag)
{
    (void)fd;
    (void)csi2_packet_cb((u8)flag, (u8 *)data, (u32)len);
    return (int)len;
}

OPERATE_RET tkl_csi2_init(TUYA_DVP_CFG_T *csi2_cfg)
{
    if (csi2_cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    memcpy(&s_cfg, csi2_cfg, sizeof(s_cfg));
    return OPRT_OK;
}

OPERATE_RET tkl_csi2_start(void)
{
    union video_req req;
    u16 w;
    u16 h;
    int err;

    if (s_running) {
        return OPRT_OK;
    }

    avdd18_enable(0, 1);
    delay_us(150);
    avdd18_enable(0, 2);
    avdd28_enable(0, 1);
    delay_us(200);
    avdd28_enable(0, 2);

    clk_out0(CSI2_XCLK_GPIO, 4 /* CLK_OUT0_STD_24M */);

    gpio_direction_output(CSI2_PWDN_GPIO, 1);
    gpio_direction_output(CSI2_RESET_GPIO, 0);
    delay_us(30000);
    gpio_direction_output(CSI2_PWDN_GPIO, 0);
    delay_us(10000);
    gpio_direction_output(CSI2_RESET_GPIO, 1);
    delay_us(30000);

    s_video_rec = server_open("video_server", "video0.1");
    if (s_video_rec == NULL) {
        printf("[%s] server_open video0.1 failed\n", CSI2_TAG);
        return OPRT_COM_ERROR;
    }

    s_video_buf = (uint8_t *)tkl_system_malloc(CSI2_BUF_SIZE);
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
    req.rec.real_fps    = 15;
    req.rec.abr_kbps    = 3000;
    req.rec.cycle_time  = 60;
    req.rec.enable_dri  = 0;
    req.rec.online      = 1;
    req.rec.buf         = s_video_buf;
    req.rec.buf_len     = CSI2_BUF_SIZE;

    req.rec.target   = VIDEO_TO_OUT;
    strcpy(req.rec.net_par.netpath, "usr://camera_csi2");
    req.rec.out.path  = req.rec.net_par.netpath;
    req.rec.out.arg   = (void *)1;
    req.rec.out.open  = csi2_out_open;
    req.rec.out.send  = csi2_out_send;
    req.rec.out.close = csi2_out_close;

    err = server_request(s_video_rec, VIDEO_REQ_REC, &req);
    if (err != 0) {
        printf("[%s] CSI-2 VIDEO_REQ_REC failed: %d\n", CSI2_TAG, err);
        server_close(s_video_rec);
        s_video_rec = NULL;
        tkl_system_free(s_video_buf);
        s_video_buf = NULL;
        return OPRT_COM_ERROR;
    }

    s_running = 1;
    printf("[%s] CSI-2 started (%ux%u)\n", CSI2_TAG, (unsigned)w, (unsigned)h);
    return OPRT_OK;
}

OPERATE_RET tkl_csi2_stop(void)
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

OPERATE_RET tkl_csi2_deinit(void)
{
    (void)tkl_csi2_stop();
    memset(&s_cfg, 0, sizeof(s_cfg));
    return OPRT_OK;
}

#else /* not a WL83 build */

OPERATE_RET tkl_csi2_init(TUYA_DVP_CFG_T *csi2_cfg)
{
    (void)csi2_cfg;
    return OPRT_NOT_SUPPORTED;
}
OPERATE_RET tkl_csi2_deinit(void)
{
    return OPRT_NOT_SUPPORTED;
}
OPERATE_RET tkl_csi2_start(void)
{
    return OPRT_NOT_SUPPORTED;
}
OPERATE_RET tkl_csi2_stop(void)
{
    return OPRT_NOT_SUPPORTED;
}

#endif
