/*
 * @Description: file content
 * @FilePath: /tal_audio_subsys/include/utils/audio_subsys_utils.h
 * @Version: v0.0.0
 * @Company: ClarePhang
 * @Author: clare.phang <clare.phang@foxmail.com>
 * @Date: 2024-07-26 17:06:08
 * @LastEditors: clare.phang
 * @LastEditTime: 2024-08-01 18:29:14
 */
#ifndef __AUDIO_SUBSYS_UTILS_H__
#define __AUDIO_SUBSYS_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "utils/audio_subsys_utils_debug.h"
#include "utils/audio_subsys_utils_logger.h"
#include "utils/audio_subsys_utils_memory.h"
#include "utils/audio_subsys_utils_types.h"

typedef struct multi_channel_audio {
    int   bits_per_sample;          /**< bits_per_sample, WARNING: NOW only support `16` (16bit/s16le) */

    void *multi_channel_data;       /**< multi-channel audio data pinter */
    int   multi_channel_dlen;       /**< multi-channel audio data length bytes number */

    int   mic_channels;             /**< microphone recode channel of number */
    void *mic_channel_data;         /**< microphone recode audio data pinter */
    int   mic_channel_dlen;         /**< microphone recode audio data length bytes number */

    int   ref_channels;             /**< speaker reference channel of number */
    void *ref_channel_data;         /**< speaker reference audio data pinter */
    int   ref_channel_dlen;         /**< speaker reference audio data length bytes number */

    int   out_channels;             /**< output audio channel of number, WARNING: only for `extract` */
    void *out_channel_data;         /**< output audio data pinter, WARNING: only for `extract` */
    int   out_channel_dlen;         /**< output audio data length bytes number, WARNING: only for `extract` */
} multi_channel_audio_t;

/**
 * @brief Multi-channel audio data split, multi-channel split to rec & ref
 *
 * @note `out_*` fields are not used.
 *
 * @param audio multi_channel_audio_t point
 * @return int 0: success, -1: fail
 */
int audio_subsys_utils_split_multi_channel_audio(multi_channel_audio_t *audio);

/**
 * @brief Multi-channel audio data merge, rec & ref merge to multi-channel
 *
 * @note `out_*` fields are not used.
 *
 * @param audio multi_channel_audio_t point
 * @return int 0: success, -1: fail
 */
int audio_subsys_utils_merge_multi_channel_audio(multi_channel_audio_t *audio);

/**
 * @brief Multi-channel audio data extract, extract destination channel to ouput
 *
 * @note `rec_channel_d*` / `ref_channel_d*` fields are not used.
 *
 * @param audio multi_channel_audio_t point
 * @return int 0: success, -1: fail
 */
int audio_subsys_utils_extract_multi_channel_audio(multi_channel_audio_t *audio);

#ifdef __cplusplus
}
#endif

#endif /** !__AUDIO_SUBSYS_UTILS_H__ */
