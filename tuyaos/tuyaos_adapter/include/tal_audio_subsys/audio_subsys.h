/*
 * @Description: file content
 * @FilePath: /tal_audio_subsys/include/audio_subsys.h
 * @Version: v0.0.0
 * @Company: ClarePhang
 * @Author: clare.phang <clare.phang@foxmail.com>
 * @Date: 2024-06-24 10:55:31
 * @LastEditors: clare.phang
 * @LastEditTime: 2024-07-03 11:09:34
 */
#ifndef __AUDIO_SUBSYS_H__
#define __AUDIO_SUBSYS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "common/audio_subsys_types.h"
#include "common/audio_subsys_module.h"

#include "utils/audio_subsys_utils.h"
#if defined(AUDIO_SUBSYS_MODULE_AEC) && (AUDIO_SUBSYS_MODULE_AEC == 1)
#include "aec/audio_subsys_aec.h"
#endif
#if defined(AUDIO_SUBSYS_MODULE_AGC) && (AUDIO_SUBSYS_MODULE_AGC == 1)
#include "agc/audio_subsys_agc.h"
#endif
#if defined(AUDIO_SUBSYS_MODULE_FILTER) && (AUDIO_SUBSYS_MODULE_FILTER == 1)
#include "filter/audio_subsys_filter.h"
#endif
#if defined(AUDIO_SUBSYS_MODULE_NS) && (AUDIO_SUBSYS_MODULE_NS == 1)
#include "ns/audio_subsys_ns.h"
#endif
#if defined(AUDIO_SUBSYS_MODULE_RESAMPLE) && (AUDIO_SUBSYS_MODULE_RESAMPLE == 1)
#include "resample/audio_subsys_resample.h"
#endif
#if defined(AUDIO_SUBSYS_MODULE_VAD) && (AUDIO_SUBSYS_MODULE_VAD == 1)
#include "vad/audio_subsys_vad.h"
#endif
#if defined(AUDIO_SUBSYS_MULTI_BASE_SPEEXDSP) && (AUDIO_SUBSYS_MULTI_BASE_SPEEXDSP == 1)
#include "speexdsp/audio_subsys_speexdsp_wrap.h"
#endif
#if defined(CONFIG_AUDIO_SUBSYS_MULTI_BASE_WEBRTC_APM) && (CONFIG_AUDIO_SUBSYS_MULTI_BASE_WEBRTC_APM == 1)
#include "webrtc_apm/audio_subsys_webrtc_apm_wrap.h"
#endif
#include "manager/audio_subsys_manager.h"

#ifdef __cplusplus
}
#endif

#endif /* !__AUDIO_SUBSYS_H__ */
