/**
 * @file tkl_dvp.c
 * @brief JIELI WL83 DVP0 camera driver adapter for TuyaOpen
 *
 * Implements the TuyaOpen tkl_dvp interface on top of Jieli's native
 * video_server (video1.1 channel, DVP0 parallel camera path).
 */

#include "tkl_dvp.h"
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

#define JL_000DC   0x63643030
#define JPEG_HEAD  0xE0FFD8FF
#define JPEG_HEAD1 0xC0FFD8FF

#define JL_DVP_BUF_SIZE (128 * 1024)

static struct server *s_video_rec     = NULL;
static uint8_t       *s_video_buf     = NULL;
static uint32_t       s_video_buf_len = 0;
static TUYA_DVP_CFG_T s_dvp_cfg;
static bool           s_is_inited  = false;
static bool           s_is_running = false;
static uint32_t       s_frame_id   = 0;

static int s_dvp_packet_cb(u8 type, u8 *buf, u32 size)
{
    if (!s_is_running || !buf || size < 8) {
        return 0;
    }

    uint32_t *head = (uint32_t *)buf;
    if (type == VIDEO_REC_JPEG_TYPE_VIDEO || type == 2) {
        if ((*head == JL_000DC && *(head + 2) == JPEG_HEAD) || *(head + 2) == JPEG_HEAD ||
            (*head == JL_000DC && *(head + 2) == JPEG_HEAD1) || *(head + 2) == JPEG_HEAD1) {
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
                }
            }
        }
    }

    return 0;
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

    s_video_rec = server_open("video_server", "video1.1");
    if (!s_video_rec) {
        return OPRT_COM_ERROR;
    }

    s_video_buf_len = JL_DVP_BUF_SIZE;
    s_video_buf     = (uint8_t *)malloc(s_video_buf_len);
    if (!s_video_buf) {
        server_close(s_video_rec);
        s_video_rec = NULL;
        return OPRT_MALLOC_FAILED;
    }

    union video_req req;
    memset(&req, 0, sizeof(req));
    req.rec.camera_type = VIDEO_CAMERA_NORMAL;
    req.rec.channel     = 1;
    req.rec.width       = s_dvp_cfg.width ? s_dvp_cfg.width : 640;
    req.rec.height      = s_dvp_cfg.height ? s_dvp_cfg.height : 480;
    req.rec.state       = VIDEO_STATE_START;
    req.rec.format      = VIDEO_FMT_AVI;
    req.rec.quality     = VIDEO_MID_Q;
    req.rec.fps         = s_dvp_cfg.fps ? (uint8_t)s_dvp_cfg.fps : 15;
    req.rec.real_fps    = req.rec.fps;
    req.rec.online      = 1;
    req.rec.buf         = s_video_buf;
    req.rec.buf_len     = s_video_buf_len;

    int err = server_request(s_video_rec, VIDEO_REQ_REC, &req);
    if (err != 0) {
        server_close(s_video_rec);
        s_video_rec = NULL;
        free(s_video_buf);
        s_video_buf = NULL;
        return OPRT_COM_ERROR;
    }

    req.rec.packet_cb = s_dvp_packet_cb;
    err               = server_request(s_video_rec, VIDEO_REQ_SET_PACKET_CALLBACK, &req);
    if (err != 0) {
        memset(&req, 0, sizeof(req));
        req.rec.channel = 1;
        req.rec.state   = VIDEO_STATE_STOP;
        server_request(s_video_rec, VIDEO_REQ_REC, &req);
        server_close(s_video_rec);
        s_video_rec = NULL;
        free(s_video_buf);
        s_video_buf = NULL;
        return OPRT_COM_ERROR;
    }

    s_is_running = true;
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
