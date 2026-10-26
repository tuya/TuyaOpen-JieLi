/**
 * @file tkl_csi2.c
 * @brief JIELI WL83 MIPI CSI2 camera driver adapter for TuyaOpen
 *
 * Implements the TuyaOpen tkl_csi2 interface on top of Jieli's native
 * video_server (video0.1 channel, MIPI CSI2 camera path).
 */

#include "tkl_csi2.h"
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
#include "asm/eva_sfr.h"

extern void avdd18_enable(uint8_t level, uint8_t step);
extern void avdd28_enable(uint8_t level, uint8_t step);
extern void clk_out0(uint32_t gpio, uint32_t index);
extern void delay_us(unsigned int us);

void tkl_csi2_dump_regs(void)
{
    tkl_log_output("[CSI2-SFR] sys=0x%08x rmap=0x%08x lane=0x%08x tval=0x%08x task=0x%08x haw=%u vaw=%u\r\n",
                   (unsigned int)csi_sys_con, (unsigned int)csi_rmap_con,
                   (unsigned int)csi_lane_con, (unsigned int)csi_tval_con,
                   (unsigned int)csi_task_con,
                   (unsigned int)csi_task_haw, (unsigned int)csi_task_vaw);
    tkl_log_output("[CSI2-SFR] phy0=0x%08x phy1=0x%08x phy2=0x%08x | vbus: isp0=%u imc0=%u\r\n",
                   *(volatile uint32_t *)csi_phy_con0,
                   *(volatile uint32_t *)csi_phy_con1,
                   *(volatile uint32_t *)csi_phy_con2,
                   (unsigned int)vbus_set(vtid_isp0),
                   (unsigned int)vbus_set(vtid_imc_ch0));
}

#define JL_000DC   0x63643030
#define JPEG_HEAD  0xE0FFD8FF
#define JPEG_HEAD1 0xC0FFD8FF

#define JL_CSI2_BUF_SIZE (256 * 1024)

static struct server *s_video_rec     = NULL;
static uint8_t       *s_video_buf     = NULL;
static uint32_t       s_video_buf_len = 0;
static TUYA_DVP_CFG_T s_csi_cfg;
static bool           s_is_inited  = false;
static bool           s_is_running = false;
static uint32_t       s_frame_id   = 0;
volatile int          g_csi2_open_status = 0;
volatile int          g_csi2_rec_err = 0;
volatile uint32_t     g_csi2_packet_count = 0;

static int s_csi2_packet_cb(u8 type, u8 *buf, u32 size)
{
    g_csi2_packet_count++;
    uint32_t s_pkt_cnt = g_csi2_packet_count;
    if (s_pkt_cnt <= 10 || s_pkt_cnt % 30 == 0) {
        tkl_log_output("[CSI2-CB] packet #%u: type=%d, size=%u, data[0..3]=0x%02x 0x%02x 0x%02x 0x%02x\r\n",
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
            if (s_csi_cfg.inter_cfg.assign_cb && s_csi_cfg.inter_cfg.post_cb) {
                TUYA_DVP_FRAME_MANAGE_T *csi_frame =
                    s_csi_cfg.inter_cfg.assign_cb(TUYA_FRAME_FMT_JPEG, s_csi_cfg.inter_cfg.cb_param);
                if (csi_frame != NULL) {
                    s_frame_id++;
                    csi_frame->frame_id          = s_frame_id;
                    csi_frame->width             = s_csi_cfg.width;
                    csi_frame->height            = s_csi_cfg.height;
                    csi_frame->is_i_frame        = 1;
                    csi_frame->is_frame_complete = 1;
                    csi_frame->frame_fmt         = TUYA_FRAME_FMT_JPEG;
                    if (csi_frame->data != NULL && size <= csi_frame->data_len) {
                        memcpy(csi_frame->data, buf, size);
                        csi_frame->data_len        = size;
                        csi_frame->total_frame_len = size;
                        s_csi_cfg.inter_cfg.post_cb(csi_frame, s_csi_cfg.inter_cfg.cb_param);
                    }
                } else {
                    if (s_pkt_cnt % 30 == 0) {
                        tkl_log_output("[CSI2-CB] assign_cb returned NULL\r\n");
                    }
                }
            }
        } else {
            if (s_pkt_cnt <= 5) {
                tkl_log_output("[CSI2-CB] non-JPEG header: 0x%02x 0x%02x\r\n", buf[0], buf[1]);
            }
        }
    }

    return 0;
}

static void *s_csi2_out_open(const char *path, void *arg)
{
    tkl_log_output("[CSI2] out_open called: path=%s\r\n", path ? path : "NULL");
    return arg ? arg : (void *)1;
}

static void s_csi2_out_close(void *fd)
{
    tkl_log_output("[CSI2] out_close called: fd=%p\r\n", fd);
}

static int s_csi2_out_send(void *fd, char *data, unsigned int len, unsigned int flag)
{
    (void)fd;
    s_csi2_packet_cb((uint8_t)flag, (uint8_t *)data, (uint32_t)len);
    return len;
}

OPERATE_RET tkl_csi2_init(TUYA_DVP_CFG_T *csi2_cfg)
{
    if (csi2_cfg == NULL) {
        return OPRT_INVALID_PARM;
    }

    memcpy(&s_csi_cfg, csi2_cfg, sizeof(TUYA_DVP_CFG_T));
    s_is_inited = true;
    return OPRT_OK;
}

OPERATE_RET tkl_csi2_deinit(void)
{
    tkl_csi2_stop();
    s_is_inited = false;
    memset(&s_csi_cfg, 0, sizeof(s_csi_cfg));
    return OPRT_OK;
}

OPERATE_RET tkl_csi2_start(void)
{
    if (!s_is_inited) {
        return OPRT_COM_ERROR;
    }
    if (s_is_running) {
        return OPRT_OK;
    }

    tkl_log_output("[CSI2] Powering on sensor: AVDD18(PV1) and AVDD28(PV0)...\r\n");
    avdd18_enable(0, 1);
    delay_us(150);
    avdd18_enable(0, 2);

    avdd28_enable(0, 1);
    delay_us(200);
    avdd28_enable(0, 2);

    tkl_log_output("[CSI2] Starting XCLK (24MHz) on PE10...\r\n");
    clk_out0(IO_PORTE_10, 4 /* CLK_OUT0_STD_24M */);

    /* Hardware reset sequence for CAM1 */
    gpio_direction_output(IO_PORTE_11, 1); /* PWDN high */
    gpio_direction_output(IO_PORTE_12, 0); /* RESET low */
    delay_us(30000);

    gpio_direction_output(IO_PORTE_11, 0); /* PWDN low (active) */
    delay_us(10000);
    gpio_direction_output(IO_PORTE_12, 1); /* RESET high */
    delay_us(30000);

    /* Check I2C pin states */
    gpio_set_die(IO_PORTE_13, 1);
    gpio_set_die(IO_PORTE_14, 1);
    gpio_direction_input(IO_PORTE_13);
    gpio_direction_input(IO_PORTE_14);
    gpio_set_pull_up(IO_PORTE_13, 1);
    gpio_set_pull_up(IO_PORTE_14, 1);
    delay_us(1000);
    int sda_v = gpio_read(IO_PORTE_13);
    int scl_v = gpio_read(IO_PORTE_14);
    tkl_log_output("[CSI2] I2C pins idle check: SDA(PE13)=%d, SCL(PE14)=%d\r\n", sda_v, scl_v);

    tkl_log_output("[CSI2] opening video_server video0.1...\r\n");
    s_video_rec = server_open("video_server", "video0.1");
    if (!s_video_rec) {
        g_csi2_open_status = 1;
        tkl_log_output("[CSI2] ERROR: server_open video0.1 failed!\r\n");
        return OPRT_COM_ERROR;
    }
    tkl_log_output("[CSI2] server_open video0.1 OK, allocating buffer...\r\n");

    s_video_buf_len = JL_CSI2_BUF_SIZE;
    s_video_buf     = (uint8_t *)malloc(s_video_buf_len);
    if (!s_video_buf) {
        g_csi2_open_status = 2;
        server_close(s_video_rec);
        s_video_rec = NULL;
        return OPRT_MALLOC_FAILED;
    }

    uint16_t w = s_csi_cfg.width ? s_csi_cfg.width : 640;
    uint16_t h = s_csi_cfg.height ? s_csi_cfg.height : 480;

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
    req.rec.fps         = s_csi_cfg.fps ? (uint8_t)s_csi_cfg.fps : 15;
    req.rec.real_fps    = 15;
    req.rec.abr_kbps    = 3000;
    req.rec.cycle_time  = 60;
    req.rec.enable_dri  = 0;
    req.rec.online      = 1;
    req.rec.buf         = s_video_buf;
    req.rec.buf_len     = s_video_buf_len;

    /* Route video_server stream packets to out_send callback */
    req.rec.target      = VIDEO_TO_OUT;
    strcpy(req.rec.net_par.netpath, "usr://camera_csi2");
    req.rec.out.path    = req.rec.net_par.netpath;
    req.rec.out.arg     = (void *)1;
    req.rec.out.open    = s_csi2_out_open;
    req.rec.out.send    = s_csi2_out_send;
    req.rec.out.close   = s_csi2_out_close;

    int err = server_request(s_video_rec, VIDEO_REQ_REC, &req);
    g_csi2_rec_err = err;
    if (err != 0) {
        g_csi2_open_status = 3;
        tkl_log_output("[CSI2] ERROR: VIDEO_REQ_REC failed: %d\r\n", err);
        server_close(s_video_rec);
        s_video_rec = NULL;
        free(s_video_buf);
        s_video_buf = NULL;
        return OPRT_COM_ERROR;
    }
    tkl_log_output("[CSI2] VIDEO_REQ_REC OK!\r\n");

    /* Also register packet_cb as auxiliary listener if supported */
    req.rec.packet_cb = s_csi2_packet_cb;
    err               = server_request(s_video_rec, VIDEO_REQ_SET_PACKET_CALLBACK, &req);
    if (err != 0) {
        tkl_log_output("[CSI2] note: VIDEO_REQ_SET_PACKET_CALLBACK returned %d (using VIDEO_TO_OUT)\r\n", err);
    }

    g_csi2_open_status = 10;
    s_is_running = true;
    tkl_log_output("[CSI2] CSI2 started successfully, status=10\r\n");
    tkl_csi2_dump_regs();
    return OPRT_OK;
}

OPERATE_RET tkl_csi2_stop(void)
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
