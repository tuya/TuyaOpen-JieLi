/**
 * @file tkl_dvp.c
 * @brief JIELI WL83 DVP0 camera driver adapter for TuyaOpen
 *
 * Implements the TuyaOpen tkl_dvp interface on top of Jieli's native
 * video_server (video1.1 channel, DVP0 parallel camera path).
 */

#include "tkl_dvp.h"
#include "tkl_output.h"
#include "tuya_error_code.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#if defined(JIELI_SELECTED_CHIP_WL83)

#ifndef __FS_H__
#define __FS_H__ /* Prevent SDK's fs.h from conflicting with newlib stdio.h FILE typedef */
#endif

#include "server/server_core.h"
#include "server/video_server.h"
#include "device/device.h"
#include "asm/gpio.h"
#include "driver/device/gpio.h"

extern void avdd18_enable(uint8_t level, uint8_t step);
extern void avdd28_enable(uint8_t level, uint8_t step);
extern void clk_out0(uint32_t gpio, uint32_t index);
extern void clk_out1(uint32_t gpio, uint32_t index);
extern void delay_us(unsigned int us);

#define JL_000DC   0x63643030
#define JPEG_HEAD  0xE0FFD8FF
#define JPEG_HEAD1 0xC0FFD8FF

#define JL_DVP_BUF_SIZE (256 * 1024)

static struct server *s_video_rec     = NULL;
static uint8_t       *s_video_buf     = NULL;
static uint32_t       s_video_buf_len = 0;
static TUYA_DVP_CFG_T s_dvp_cfg;
static bool           s_is_inited  = false;
static bool           s_is_running = false;
static uint32_t       s_frame_id   = 0;
volatile int          g_dvp_open_status = 0;
volatile int          g_dvp_rec_err = 0;
volatile uint32_t     g_dvp_packet_count = 0;

static int s_dvp_packet_cb(u8 type, u8 *buf, u32 size)
{
    g_dvp_packet_count++;
    uint32_t s_pkt_cnt = g_dvp_packet_count;
    if (s_pkt_cnt <= 10 || s_pkt_cnt % 30 == 0) {
        tkl_log_output("[DVP-CB] packet #%u: type=%d, size=%u, data[0..3]=0x%02x 0x%02x 0x%02x 0x%02x\r\n",
                       s_pkt_cnt, type, size,
                       size > 0 ? buf[0] : 0, size > 1 ? buf[1] : 0,
                       size > 2 ? buf[2] : 0, size > 3 ? buf[3] : 0);
    }
    if (!s_is_running || !buf || size < 8) {
        return 0;
    }

    uint32_t *head = (uint32_t *)buf;
    if (type == VIDEO_REC_JPEG_TYPE_VIDEO || type == 2) {
        if ((*head == JL_000DC && *(head + 2) == JPEG_HEAD) || *(head + 2) == JPEG_HEAD ||
            (*head == JL_000DC && *(head + 2) == JPEG_HEAD1) || *(head + 2) == JPEG_HEAD1 ||
            (buf[0] == '0' && buf[1] == '0' && buf[2] == 'd' && buf[3] == 'c')) {
            buf += 8;
            size -= 8;
        }

        if (size >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
            if (s_dvp_cfg.inter_cfg.assign_cb && s_dvp_cfg.inter_cfg.post_cb) {
                TUYA_DVP_FRAME_MANAGE_T *dvp_frame =
                    s_dvp_cfg.inter_cfg.assign_cb(TUYA_FRAME_FMT_JPEG, s_dvp_cfg.inter_cfg.cb_param);
                if (dvp_frame != NULL) {
                    s_frame_id++;
                    dvp_frame->frame_id          = s_frame_id;
                    dvp_frame->width             = s_dvp_cfg.width;
                    dvp_frame->height            = s_dvp_cfg.height;
                    dvp_frame->is_i_frame        = 1;
                    dvp_frame->is_frame_complete = 1;
                    dvp_frame->frame_fmt         = TUYA_FRAME_FMT_JPEG;
                    if (dvp_frame->data != NULL && size <= dvp_frame->data_len) {
                        memcpy(dvp_frame->data, buf, size);
                        dvp_frame->data_len        = size;
                        dvp_frame->total_frame_len = size;
                        s_dvp_cfg.inter_cfg.post_cb(dvp_frame, s_dvp_cfg.inter_cfg.cb_param);
                    }
                } else {
                    if (s_pkt_cnt % 30 == 0) {
                        tkl_log_output("[DVP-CB] assign_cb returned NULL\r\n");
                    }
                }
            }
        }
    }

    return 0;
}

static void *s_dvp_out_open(const char *path, void *arg)
{
    tkl_log_output("[DVP] out_open called: path=%s\r\n", path ? path : "NULL");
    return arg ? arg : (void *)1;
}

static void s_dvp_out_close(void *fd)
{
    tkl_log_output("[DVP] out_close called: fd=%p\r\n", fd);
}

static int s_dvp_out_send(void *fd, char *data, unsigned int len, unsigned int flag)
{
    (void)fd;
    s_dvp_packet_cb((uint8_t)flag, (uint8_t *)data, (uint32_t)len);
    return len;
}

OPERATE_RET tkl_dvp_init(TUYA_DVP_CFG_T *dvp_cfg)
{
    if (dvp_cfg == NULL) {
        return OPRT_INVALID_PARM;
    }

    memcpy(&s_dvp_cfg, dvp_cfg, sizeof(TUYA_DVP_CFG_T));
    s_is_inited = true;
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_deinit(void)
{
    tkl_dvp_stop();
    s_is_inited = false;
    memset(&s_dvp_cfg, 0, sizeof(s_dvp_cfg));
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_start(void)
{
    if (!s_is_inited) {
        return OPRT_COM_ERROR;
    }
    if (s_is_running) {
        return OPRT_OK;
    }

    tkl_log_output("[DVP] Powering on sensor AVDD lines...\r\n");
    avdd18_enable(0, 1);
    delay_us(150);
    avdd18_enable(0, 2);

    avdd28_enable(0, 1);
    delay_us(200);
    avdd28_enable(0, 2);

    tkl_log_output("[DVP] Starting XCLK (24MHz) on PD8...\r\n");
    clk_out0(IO_PORTD_08, 4 /* CLK_OUT0_STD_24M */);
    clk_out1(IO_PORTD_08, 4 /* CLK_OUT1_STD_24M */);

    /* Hardware reset sequence for CAM2 (DVP0) */
    gpio_direction_output(IO_PORTE_09, 0); /* RESET low */
    delay_us(20000);
    gpio_direction_output(IO_PORTE_09, 1); /* RESET high */
    delay_us(20000);

    /* Check I2C1 pin states (PE7 SDA, PE8 SCL) */
    gpio_set_die(IO_PORTE_07, 1);
    gpio_set_die(IO_PORTE_08, 1);
    gpio_direction_input(IO_PORTE_07);
    gpio_direction_input(IO_PORTE_08);
    gpio_set_pull_up(IO_PORTE_07, 1);
    gpio_set_pull_up(IO_PORTE_08, 1);
    delay_us(1000);
    int sda_v = gpio_read(IO_PORTE_07);
    int scl_v = gpio_read(IO_PORTE_08);
    tkl_log_output("[DVP] I2C1 pins idle check: SDA(PE7)=%d, SCL(PE8)=%d\r\n", sda_v, scl_v);

    tkl_log_output("[DVP] opening video_server video1.1...\r\n");
    s_video_rec = server_open("video_server", "video1.1");
    if (!s_video_rec) {
        g_dvp_open_status = 1;
        tkl_log_output("[DVP] ERROR: server_open video1.1 failed!\r\n");
        return OPRT_COM_ERROR;
    }
    tkl_log_output("[DVP] server_open video1.1 OK, allocating buffer...\r\n");

    s_video_buf_len = JL_DVP_BUF_SIZE;
    s_video_buf     = (uint8_t *)malloc(s_video_buf_len);
    if (!s_video_buf) {
        g_dvp_open_status = 2;
        server_close(s_video_rec);
        s_video_rec = NULL;
        return OPRT_MALLOC_FAILED;
    }

    uint16_t w = s_dvp_cfg.width ? s_dvp_cfg.width : 640;
    uint16_t h = s_dvp_cfg.height ? s_dvp_cfg.height : 480;

    union video_req req;
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
    req.rec.fps         = s_dvp_cfg.fps ? (uint8_t)s_dvp_cfg.fps : 15;
    req.rec.real_fps    = 15;
    req.rec.abr_kbps    = 3000;
    req.rec.cycle_time  = 60;
    req.rec.enable_dri  = 0;
    req.rec.online      = 1;
    req.rec.buf         = s_video_buf;
    req.rec.buf_len     = s_video_buf_len;

    /* Route video_server stream packets to out_send callback */
    req.rec.target      = VIDEO_TO_OUT;
    strcpy(req.rec.net_par.netpath, "usr://camera_dvp0");
    req.rec.out.path    = req.rec.net_par.netpath;
    req.rec.out.arg     = (void *)1;
    req.rec.out.open    = s_dvp_out_open;
    req.rec.out.send    = s_dvp_out_send;
    req.rec.out.close   = s_dvp_out_close;

    int err = server_request(s_video_rec, VIDEO_REQ_REC, &req);
    g_dvp_rec_err = err;
    if (err != 0) {
        g_dvp_open_status = 3;
        tkl_log_output("[DVP] ERROR: VIDEO_REQ_REC failed: %d\r\n", err);
        server_close(s_video_rec);
        s_video_rec = NULL;
        free(s_video_buf);
        s_video_buf = NULL;
        return OPRT_COM_ERROR;
    }
    tkl_log_output("[DVP] VIDEO_REQ_REC OK!\r\n");

    req.rec.packet_cb = s_dvp_packet_cb;
    err               = server_request(s_video_rec, VIDEO_REQ_SET_PACKET_CALLBACK, &req);
    if (err != 0) {
        tkl_log_output("[DVP] note: VIDEO_REQ_SET_PACKET_CALLBACK returned %d\r\n", err);
    }

    g_dvp_open_status = 10;
    s_is_running = true;
    tkl_log_output("[DVP] DVP0 started successfully, status=10\r\n");
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_stop(void)
{
    if (!s_is_running) {
        return OPRT_OK;
    }

    s_is_running = false;

    if (s_video_rec != NULL) {
        union video_req req;
        memset(&req, 0, sizeof(req));
        req.rec.channel = 1;
        req.rec.state   = VIDEO_STATE_STOP;
        server_request(s_video_rec, VIDEO_REQ_REC, &req);
        server_close(s_video_rec);
        s_video_rec = NULL;
    }

    if (s_video_buf != NULL) {
        free(s_video_buf);
        s_video_buf     = NULL;
        s_video_buf_len = 0;
    }

    return OPRT_OK;
}

#else

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

#endif
