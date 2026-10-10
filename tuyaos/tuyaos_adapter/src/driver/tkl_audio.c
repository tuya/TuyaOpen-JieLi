#include "tkl_audio.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "system/generic/printf.h"
#include "system/os/os_api.h"
#include "tuya_error_code.h"

#if defined(JIELI_SELECTED_CHIP_WL83)
/* WL83 (AC792) provider: the SDK's ADC/DAC runtime. */
#include "system/includes.h"
#include "media/includes.h"
#include "audio_adc.h"
#include "audio_dac.h"
#include "cpu/wl83/audio_config.h"
#include "adc_file.h"
#elif !defined(JIELI_SELECTED_CHIP_WL82)
#error "Jieli audio requires a selected chip"
#endif

#define JIELI_AUDIO_MAX_PLAY_BYTES (8u * 1024u)
#define JIELI_AUDIO_CALLBACK_DRAIN_TIMEOUT_TICKS 100u

/* Normalized PCM contract: 16 kHz, signed 16-bit, mono. The provider at the
 * bottom of this file converts it to whatever its SDK expects; the SDK request
 * structures stay private to that provider. */
typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
} JIELI_AUDIO_NATIVE_PCM_CONFIG_T;

typedef void (*JIELI_AUDIO_NATIVE_CAPTURE_CB)(void *cookie, const uint8_t *data, size_t size, uint64_t pts);

typedef struct {
    TKL_AUDIO_CONFIG_T config;
    uint8_t initialized;
    uint8_t started;
    uint8_t callbacks_enabled;
    uint32_t callbacks_inflight;
} AI_STATE_T;

typedef struct {
    TKL_AUDIO_CONFIG_T config;
    void *stream;
    uint8_t initialized;
    uint8_t started;
} AO_STATE_T;

static AI_STATE_T s_ai;
static AO_STATE_T s_ao;

/* The SoC provider shares this translation unit, so the TKL entry points call
 * it directly instead of through an ops table. Both chips expose the same
 * entry points; the chip branch at the bottom picks the implementation. */
int jieli_audio_native_prepare(void);
int jieli_audio_native_ai_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config,
                               JIELI_AUDIO_NATIVE_CAPTURE_CB callback, void *cookie);
int jieli_audio_native_ai_start(void);
int jieli_audio_native_ai_stop(void);
int jieli_audio_native_ai_uninit(void);
int jieli_audio_native_ai_set_volume(int volume);
int jieli_audio_native_ai_get_volume(int *volume);
int jieli_audio_native_ao_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config, void **stream);
int jieli_audio_native_ao_start(void *stream);
int jieli_audio_native_ao_stop(void *stream);
int jieli_audio_native_ao_uninit(void *stream);
int jieli_audio_native_ao_set_volume(void *stream, int volume);
int jieli_audio_native_ao_get_volume(void *stream, int *volume);
int jieli_audio_native_ao_write(void *stream, const uint8_t *data, size_t size);
int jieli_audio_native_ao_flush(void *stream);
uintptr_t jieli_audio_native_critical_enter(void);
void jieli_audio_native_critical_exit(uintptr_t state);

/* Same translation unit family: declared here rather than in a private header. */
void tkl_jieli_vad_feed_capture(const uint8_t *data, size_t size);

/* Provider operations return 0 or a negative private code; the TKL API returns
 * OPERATE_RET, so every call into the provider goes through this translation. */
static OPERATE_RET __map_native_result(int result)
{
    if (result == 0) return OPRT_OK;
    if (result == -2) return OPRT_INVALID_PARM;
    if (result == -3) return OPRT_NOT_SUPPORTED;
    if (result == -4) return OPRT_RESOURCE_NOT_READY;
    if (result == -31) return OPRT_BUFFER_NOT_ENOUGH;
    if (result == OPRT_TIMEOUT) return OPRT_TIMEOUT;
    return OPRT_COM_ERROR;
}

static OPERATE_RET __validate_config(const TKL_AUDIO_CONFIG_T *config, int require_capture_callback)
{
    if (config == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (config->sample != TKL_AUDIO_SAMPLE_16K || config->datebits != TKL_AUDIO_DATABITS_16 ||
        config->channel != TKL_AUDIO_CHANNEL_MONO || config->codectype != TKL_CODEC_AUDIO_PCM) {
        return OPRT_NOT_SUPPORTED;
    }
    if (require_capture_callback && config->put_cb == NULL) return OPRT_INVALID_PARM;
    return OPRT_OK;
}

static JIELI_AUDIO_NATIVE_PCM_CONFIG_T __pcm_config(const TKL_AUDIO_CONFIG_T *config)
{
    JIELI_AUDIO_NATIVE_PCM_CONFIG_T pcm;
    pcm.sample_rate = (uint32_t)config->sample;
    pcm.bits_per_sample = (uint8_t)config->datebits;
    pcm.channels = (uint8_t)config->channel;
    return pcm;
}

static void __capture(void *cookie, const uint8_t *data, size_t size, uint64_t pts)
{
    AI_STATE_T *state = (AI_STATE_T *)cookie;
    TKL_AUDIO_FRAME_INFO_T frame;
    TKL_FRAME_PUT_CB put_cb;
    char *copy;
    uintptr_t key;

    if (state == NULL || data == NULL || size == 0 || size > UINT32_MAX || (size & 1u)) {
        return;
    }
    key = jieli_audio_native_critical_enter();
    if (!state->initialized || !state->started || !state->callbacks_enabled || state->config.put_cb == NULL) {
        jieli_audio_native_critical_exit(key);
        return;
    }
    ++state->callbacks_inflight;
    put_cb = state->config.put_cb;
    jieli_audio_native_critical_exit(key);

    copy = (char *)malloc(size);
    if (copy == NULL) {
        goto __done;
    }
    memcpy(copy, data, size);
    /* A capture callback may aggregate several configured VAD frames. Feed
     * complete subframes while preserving the original application callback. */
    tkl_jieli_vad_feed_capture((const uint8_t *)copy, size);
    memset(&frame, 0, sizeof(frame));
    frame.type = TKL_AUDIO_FRAME;
    frame.pbuf = copy;
    frame.buf_size = (uint32_t)size;
    frame.used_size = (uint32_t)size;
    frame.pts = pts;
    frame.codectype = TKL_CODEC_AUDIO_PCM;
    frame.sample = TKL_AUDIO_SAMPLE_16K;
    frame.datebits = TKL_AUDIO_DATABITS_16;
    frame.channel = TKL_AUDIO_CHANNEL_MONO;
    put_cb(&frame);
    free(copy);

__done:
    key = jieli_audio_native_critical_enter();
    --state->callbacks_inflight;
    jieli_audio_native_critical_exit(key);
}

/* Called once from the app entry before any audio request so the chip's SDK
 * runtime (locks, queues, worker tasks) exists. The ops table it used to
 * install is gone now that the provider shares this translation unit. */
OPERATE_RET tkl_jieli_audio_prepare(void)
{
    return jieli_audio_native_prepare() == 0 ? OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_ai_init(TKL_AUDIO_CONFIG_T *pconfig, int32_t count)
{
    OPERATE_RET ret;
    JIELI_AUDIO_NATIVE_PCM_CONFIG_T pcm;

    printf("[TKL_AUDIO] ai_init enter: initialized=%u count=%d config=%p\n",
           (unsigned)s_ai.initialized, count, (void *)pconfig);
    if (s_ai.initialized) {
        printf("[TKL_AUDIO] ai_init reuse existing capture\n");
        return OPRT_OK;
    }
    /* The JIELI TDD caller passes exactly one config (the PLATFORM_JIELI
     * branch of tdd_audio.c); the count=0 calls come from the non-JIELI
     * branch only. Reject multi-stream configurations. */
    if (pconfig == NULL || count != 1) {
        printf("[TKL_AUDIO] ai_init rejected: config=%p count=%d (expected one config)\n",
               (void *)pconfig, count);
        return OPRT_INVALID_PARM;
    }
    printf("[TKL_AUDIO] ai config: sample=%d bits=%d channels=%d codec=%d put_cb=%u\n",
           pconfig[0].sample, pconfig[0].datebits, pconfig[0].channel, pconfig[0].codectype,
           pconfig[0].put_cb != NULL ? 1u : 0u);
    ret = __validate_config(&pconfig[0], 1);
    if (ret != OPRT_OK) {
        printf("[TKL_AUDIO] ai config validation failed: %d\n", ret);
        return ret;
    }
    pcm = __pcm_config(&pconfig[0]);
    printf("[TKL_AUDIO] ai dispatch native init: rate=%u bits=%u channels=%u\n",
           (unsigned)pcm.sample_rate, (unsigned)pcm.bits_per_sample, (unsigned)pcm.channels);
    ret = __map_native_result(jieli_audio_native_ai_init(&pcm, __capture, &s_ai));
    if (ret != OPRT_OK) {
        printf("[TKL_AUDIO] ai native init failed: %d\n", ret);
        return ret;
    }
    {
        uintptr_t key = jieli_audio_native_critical_enter();
        s_ai.config = pconfig[0];
        s_ai.initialized = 1;
        s_ai.started = 0;
        s_ai.callbacks_enabled = 0;
        s_ai.callbacks_inflight = 0;
        jieli_audio_native_critical_exit(key);
    }
    printf("[TKL_AUDIO] ai_init success\n");
    return OPRT_OK;
}

OPERATE_RET tkl_ai_start(int32_t card, TKL_AI_CHN_E chn)
{
    OPERATE_RET ret;
    uintptr_t key;
    printf("[TKL_AUDIO] ai_start enter: card=%d channel=%d initialized=%u\n",
           card, (int)chn, (unsigned)s_ai.initialized);
    if (card != 0 || chn != TKL_AI_0) return OPRT_INVALID_PARM;
    if (!s_ai.initialized) return OPRT_RESOURCE_NOT_READY;
    key = jieli_audio_native_critical_enter();
    if (s_ai.started && s_ai.callbacks_enabled) { jieli_audio_native_critical_exit(key); return OPRT_OK; }
    s_ai.callbacks_enabled = 1;
    jieli_audio_native_critical_exit(key);
    ret = __map_native_result(jieli_audio_native_ai_start());
    printf("[TKL_AUDIO] ai_start native result=%d\n", ret);
    if (ret == OPRT_OK) {
        uintptr_t key = jieli_audio_native_critical_enter();
        s_ai.started = 1;
        jieli_audio_native_critical_exit(key);
    } else {
        uintptr_t key = jieli_audio_native_critical_enter();
        s_ai.callbacks_enabled = 0;
        s_ai.started = 0;
        jieli_audio_native_critical_exit(key);
        (void)jieli_audio_native_ai_stop();
    }
    return ret;
}

OPERATE_RET tkl_ai_set_vol(int32_t card, TKL_AI_CHN_E chn, int32_t vol)
{
    if (card != 0 || chn != TKL_AI_0 || vol < 0 || vol > 100) return OPRT_INVALID_PARM;
    if (!s_ai.initialized) return OPRT_RESOURCE_NOT_READY;
    return __map_native_result(jieli_audio_native_ai_set_volume(vol));
}

OPERATE_RET tkl_ai_get_frame(int32_t card, TKL_AI_CHN_E chn, TKL_AUDIO_FRAME_INFO_T *frame)
{
    (void)card; (void)chn; (void)frame;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ai_set_vqe(int32_t card, TKL_AI_CHN_E chn, TKL_AUDIO_VQE_TYPE_E type,
                           TKL_AUDIO_VQE_PARAM_T *param)
{
    if (card != 0 || chn != TKL_AI_0 || type < TKL_AUDIO_VQE_AEC || type >= TKL_AUDIO_VQE_MAX || param == NULL) {
        return OPRT_INVALID_PARM;
    }
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ai_get_vqe(int32_t card, TKL_AI_CHN_E chn, TKL_AUDIO_VQE_TYPE_E type,
                           TKL_AUDIO_VQE_PARAM_T *param)
{
    if (card != 0 || chn != TKL_AI_0 || type < TKL_AUDIO_VQE_AEC || type >= TKL_AUDIO_VQE_MAX || param == NULL) {
        return OPRT_INVALID_PARM;
    }
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ai_stop(int32_t card, TKL_AI_CHN_E chn)
{
    OPERATE_RET ret;
    uint32_t callbacks_inflight;
    uint32_t waited = 0;
    uintptr_t key;
    if (card != 0 || chn != TKL_AI_0) return OPRT_INVALID_PARM;
    if (!s_ai.initialized || !s_ai.started) return OPRT_OK;
    key = jieli_audio_native_critical_enter();
    s_ai.callbacks_enabled = 0;
    jieli_audio_native_critical_exit(key);
    ret = __map_native_result(jieli_audio_native_ai_stop());
    do {
        key = jieli_audio_native_critical_enter();
        callbacks_inflight = s_ai.callbacks_inflight;
        jieli_audio_native_critical_exit(key);
        if (callbacks_inflight == 0) break;
        os_time_dly(1);
    } while (waited++ < JIELI_AUDIO_CALLBACK_DRAIN_TIMEOUT_TICKS);

    /* Some providers report in-flight callbacks while closing the native
     * stream. Once TKL callbacks have drained, retry their idempotent stop so
     * the provider can finish its own teardown before the caller frees state. */
    if (ret != OPRT_OK && callbacks_inflight == 0) {
        ret = __map_native_result(jieli_audio_native_ai_stop());
    }

    key = jieli_audio_native_critical_enter();
    callbacks_inflight = s_ai.callbacks_inflight;
    if (ret == OPRT_OK && callbacks_inflight == 0) s_ai.started = 0;
    jieli_audio_native_critical_exit(key);
    if (ret != OPRT_OK) return ret;
    return callbacks_inflight == 0 ? OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_ai_uninit(void)
{
    OPERATE_RET ret;
    if (!s_ai.initialized) return OPRT_OK;
    if (s_ai.started) {
        ret = tkl_ai_stop(0, TKL_AI_0);
        if (ret != OPRT_OK) return ret;
    }
    ret = __map_native_result(jieli_audio_native_ai_uninit());
    if (ret == OPRT_OK) {
        uintptr_t key = jieli_audio_native_critical_enter();
        memset(&s_ai, 0, sizeof(s_ai));
        jieli_audio_native_critical_exit(key);
    }
    return ret;
}

OPERATE_RET tkl_ao_init(TKL_AUDIO_CONFIG_T *pconfig, int32_t count, void **handle)
{
    OPERATE_RET ret;
    JIELI_AUDIO_NATIVE_PCM_CONFIG_T pcm;
    void *stream = NULL;
    printf("[TKL_AUDIO] ao_init enter: initialized=%u count=%d config=%p handle=%p\n",
           (unsigned)s_ao.initialized, count, (void *)pconfig, (void *)handle);
    if (handle == NULL || pconfig == NULL || count != 1) {
        printf("[TKL_AUDIO] ao_init rejected: config=%p handle=%p count=%d\n",
               (void *)pconfig, (void *)handle, count);
        return OPRT_INVALID_PARM;
    }
    if (s_ao.initialized) { *handle = &s_ao; return OPRT_OK; }
    ret = __validate_config(pconfig, 0);
    if (ret != OPRT_OK) {
        printf("[TKL_AUDIO] ao config validation failed: %d\n", ret);
        return ret;
    }
    pcm = __pcm_config(pconfig);
    printf("[TKL_AUDIO] ao dispatch native init: rate=%u bits=%u channels=%u\n",
           (unsigned)pcm.sample_rate, (unsigned)pcm.bits_per_sample, (unsigned)pcm.channels);
    ret = __map_native_result(jieli_audio_native_ao_init(&pcm, &stream));
    if (ret != OPRT_OK) {
        printf("[TKL_AUDIO] ao native init failed: %d\n", ret);
        return ret;
    }
    if (stream == NULL) {
        return OPRT_COM_ERROR;
    }
    s_ao.config = *pconfig;
    s_ao.stream = stream;
    s_ao.initialized = 1;
    s_ao.started = 0;
    *handle = &s_ao;
    printf("[TKL_AUDIO] ao_init success: stream=%p\n", stream);
    return OPRT_OK;
}

static int __ao_handle_valid(void *handle)
{
    return handle == NULL || handle == &s_ao;
}

OPERATE_RET tkl_ao_start(int32_t card, TKL_AO_CHN_E chn, void *handle)
{
    OPERATE_RET ret;
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle)) return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_RESOURCE_NOT_READY;
    if (s_ao.started) return OPRT_OK;
    ret = __map_native_result(jieli_audio_native_ao_start(s_ao.stream));
    printf("[TKL_AUDIO] ao_start native result=%d\n", ret);
    if (ret == OPRT_OK) s_ao.started = 1;
    return ret;
}

OPERATE_RET tkl_ao_set_vol(int32_t card, TKL_AO_CHN_E chn, void *handle, int32_t vol)
{
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle) || vol < 0 || vol > 100)
        return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_RESOURCE_NOT_READY;
    return __map_native_result(jieli_audio_native_ao_set_volume(s_ao.stream, vol));
}

OPERATE_RET tkl_ao_get_vol(int32_t card, TKL_AO_CHN_E chn, void *handle, int32_t *vol)
{
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle) || vol == NULL) return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_RESOURCE_NOT_READY;
    return __map_native_result(jieli_audio_native_ao_get_volume(s_ao.stream, vol));
}

OPERATE_RET tkl_ao_put_frame(int32_t card, TKL_AO_CHN_E chn, void *handle, TKL_AUDIO_FRAME_INFO_T *frame)
{
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle)) {
        printf("[TKL_AUDIO] ao_put rejected route: card=%d chn=%d handle=%p expected=%p\n",
               (int)card, (int)chn, handle, (void *)&s_ao);
        return OPRT_INVALID_PARM;
    }
    if (frame == NULL || frame->pbuf == NULL || frame->used_size == 0 ||
        frame->used_size > JIELI_AUDIO_MAX_PLAY_BYTES ||
        frame->sample != TKL_AUDIO_SAMPLE_16K || frame->datebits != TKL_AUDIO_DATABITS_16 ||
        frame->channel != TKL_AUDIO_CHANNEL_MONO || frame->codectype != TKL_CODEC_AUDIO_PCM ||
        (frame->used_size & 1u)) {
        /* The 8-field breakdown is debug-only; every rejected frame would
         * otherwise spam the production log at up to one line per write. */
        printf("[TKL_AUDIO] ao_put rejected frame: frame=%p bytes=%u\n",
               (void *)frame, frame ? (unsigned)frame->used_size : 0u);
#if defined(JIELI_AUDIO_DEBUG_FRAME_REJECT) && JIELI_AUDIO_DEBUG_FRAME_REJECT
        printf("[TKL_AUDIO] ao_put rejected frame detail: data=%p max=%u sample=%u bits=%u channel=%u codec=%u\n",
               frame ? (void *)frame->pbuf : NULL, (unsigned)JIELI_AUDIO_MAX_PLAY_BYTES,
               frame ? (unsigned)frame->sample : 0u, frame ? (unsigned)frame->datebits : 0u,
               frame ? (unsigned)frame->channel : 0u, frame ? (unsigned)frame->codectype : 0u);
#endif
        return OPRT_INVALID_PARM;
    }
    if (!s_ao.initialized || !s_ao.started) {
        printf("[TKL_AUDIO] ao_put rejected state: initialized=%u started=%u stream=%p\n",
               (unsigned)s_ao.initialized, (unsigned)s_ao.started, s_ao.stream);
        return OPRT_RESOURCE_NOT_READY;
    }
    return __map_native_result(jieli_audio_native_ao_write(s_ao.stream, (const uint8_t *)frame->pbuf,
                                                           frame->used_size));
}

OPERATE_RET tkl_ao_stop(int32_t card, TKL_AO_CHN_E chn, void *handle)
{
    OPERATE_RET flush_ret;
    OPERATE_RET stop_ret;
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle)) return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_OK;

    /* Drain accepted audio before stopping the hardware and closing the DAC.
     * Chip divergence: on wl83 the flush below really drains (it waits for
     * audio_dac_idle), but on wl82 jieli_audio_native_ao_flush() just zeroes
     * the software queue and returns, so up to JIELI_AUDIO_PLAY_QUEUE_SIZE
     * (~16 KiB, ~0.5 s at 16 kHz mono) of already-accepted PCM is dropped on
     * AC791. The vendor wl82 SDK exposes no DAC-idle query, so a real drain
     * is deferred; do not assume this call drains on both chips. */
    flush_ret = __map_native_result(jieli_audio_native_ao_flush(s_ao.stream));
    if (flush_ret != OPRT_OK) {
        /* Keep the native writer alive so a later flush can finish draining. */
        s_ao.started = 0;
        return flush_ret;
    }

    stop_ret = __map_native_result(jieli_audio_native_ao_stop(s_ao.stream));
    /* Gate TKL writes after every stop request, including failed cleanup, so a
     * later start/stop can retry the provider operation instead of trusting a
     * stale started bit. */
    s_ao.started = 0;
    if (stop_ret != OPRT_OK) return stop_ret;
    return OPRT_OK;
}

OPERATE_RET tkl_ao_uninit(void *handle)
{
    OPERATE_RET ret;
    if (!__ao_handle_valid(handle)) return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_OK;
    /* Always drain before uninit, even after a prior stop/drain error. */
    ret = tkl_ao_stop(0, TKL_AO_0, handle);
    if (ret != OPRT_OK) return ret;
    ret = __map_native_result(jieli_audio_native_ao_uninit(s_ao.stream));
    if (ret == OPRT_OK) memset(&s_ao, 0, sizeof(s_ao));
    return ret;
}

OPERATE_RET tkl_ai_detect_start(int32_t card, TKL_MEDIA_DETECT_TYPE_E type)
{
    (void)card; (void)type;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ai_detect_stop(int32_t card, TKL_MEDIA_DETECT_TYPE_E type)
{
    (void)card; (void)type;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ai_detect_get_result(int32_t card, TKL_MEDIA_DETECT_TYPE_E type, TKL_AUDIO_DETECT_RESULT_T *result)
{
    (void)card; (void)type; (void)result;
    return OPRT_NOT_SUPPORTED;
}

#if defined(JIELI_SELECTED_CHIP_WL83)
/* WL83 (AC792) provider: the SDK's ADC/DAC runtime. */

#ifndef JIELI_AUDIO_MIC_CHANNEL_MAP
#define JIELI_AUDIO_MIC_CHANNEL_MAP AUDIO_ADC_MIC_CH
#endif

#define WL83_CAPTURE_DMA_POINTS       320u
#define WL83_CAPTURE_DMA_BUFFER_COUNT 2u
#define WL83_CAPTURE_QUEUE_SIZE       (8u * 1024u)
#define WL83_CAPTURE_FRAME_SIZE       (WL83_CAPTURE_DMA_POINTS * sizeof(s16))
#define WL83_CAPTURE_CHANNEL_LIMIT    AUDIO_ADC_MAX_NUM
#define WL83_CAPTURE_TASK_NAME        "tuya_audio_capture"
/* Keep in sync with the vendor app-task row injected by
 * tools/jieli_build/audio_profile.py (app_tasks:
 * {"tuya_audio_capture", 12, 768, 64} — same name, priority and stack). The
 * playback task is not in that table; its stack is chosen to match here. */
#define WL83_CAPTURE_TASK_STACK       768
#define WL83_CAPTURE_TASK_PRIORITY    12
#define WL83_PLAY_QUEUE_SIZE          (16u * 1024u)
#define WL83_PLAY_TASK_NAME           "tuya_audio_playback"
#define WL83_PLAY_TASK_STACK          768
#define WL83_PLAY_TASK_PRIORITY       12
#define WL83_PLAY_WRITER_DRAIN_LIMIT  100u
#define WL83_PLAY_DRAIN_LIMIT         500u
/* How long to wait, once the software queue is empty, for the DAC FIFO to play
 * out what it already accepted. See jieli_audio_native_ao_flush(): an empty
 * FIFO accepted 1598 bytes at 16 kHz mono s16 (~50 ms), rounded up here. One
 * retry is one os_time_dly(1) tick, measured at 10 ms (WL83_PLAY_DRAIN_LIMIT
 * retries were observed to take ~5 s). */
#define WL83_PLAY_DAC_SETTLE_TICKS    10u
/* JieLi's digital volume curve has 0-31 levels (DEFAULT_DIGITAL_VOL_MAX). */
#define WL83_PLAY_VOLUME_LEVELS       31u

extern struct audio_adc_hdl adc_hdl;
extern struct audio_dac_hdl dac_hdl;
extern const int config_adc_async_en;
extern const struct adc_platform_cfg adc_platform_cfg_table[AUDIO_ADC_MAX_NUM];
/* Declared the same way audio/common/audio_volume_mixer.c does; the symbol
 * lives in the EQ library rather than a header this file already pulls in. */
extern float eq_db2mag(float x);
/* Vendor media-layer critical sections; defined in the driver/liba libs with
 * no public header reachable from this file. Explicit declarations keep the
 * calls from turning into implicit-int declarations. */
extern void media_irq_disable(void);
extern void media_irq_enable(void);

typedef struct {
    struct adc_mic_ch mic;
    struct audio_adc_output_hdl output;
    s16 *dma_buffer;
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;
    uint32_t sample_rate;
    uint32_t channel_map;
    int volume;
    uint32_t callbacks_inflight;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t used;
    uint32_t dropped_frames;
    uint8_t channel_count;
    uint8_t channel_index;
    uint8_t initialized;
    uint8_t resources_opened;
    uint8_t mic_opened;
    uint8_t dma_attached;
    uint8_t output_registered;
    uint8_t running;
} WL83_CAPTURE_STATE_T;

typedef struct {
    struct audio_dac_hdl *dac;
    uint32_t sample_rate;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t used;
    int volume;
    int write_error;
    uint8_t writer_inflight;
    uint8_t initialized;
    uint8_t running;
    uint8_t accepting_writes;
    uint8_t draining;
    uint8_t channel_open;
    uint8_t writer_output_logged;
    uint32_t queued_total;
    uint32_t written_total;
} WL83_PLAY_STATE_T;

static OS_SEM s_capture_sem;
static uint8_t s_capture_sem_ready;
static uint8_t s_capture_task_ready;
static OS_SEM s_play_sem;
static uint8_t s_play_sem_ready;
static uint8_t s_play_task_ready;
static OS_MUTEX s_play_lock;
static uint8_t s_play_lock_ready;
static WL83_CAPTURE_STATE_T s_capture;
static WL83_PLAY_STATE_T s_play;
static uint8_t s_capture_queue[WL83_CAPTURE_QUEUE_SIZE];
static uint8_t s_play_queue[WL83_PLAY_QUEUE_SIZE];

uintptr_t jieli_audio_native_critical_enter(void)
{
    media_irq_disable();
    return 1;
}

void jieli_audio_native_critical_exit(uintptr_t state)
{
    (void)state;
    media_irq_enable();
}

static void __capture_queue_write(const uint8_t *data, uint32_t size)
{
    uint32_t first;
    uint8_t queued = 0;

    if (data == NULL || size == 0 || size > WL83_CAPTURE_QUEUE_SIZE) {
        return;
    }

    media_irq_disable();
    if (s_capture.running && size <= WL83_CAPTURE_QUEUE_SIZE - s_capture.used) {
        first = WL83_CAPTURE_QUEUE_SIZE - s_capture.write_pos;
        if (first > size) {
            first = size;
        }
        memcpy(s_capture_queue + s_capture.write_pos, data, first);
        if (size > first) {
            memcpy(s_capture_queue, data + first, size - first);
        }
        s_capture.write_pos = (s_capture.write_pos + size) % WL83_CAPTURE_QUEUE_SIZE;
        s_capture.used += size;
        queued = 1;
    } else if (s_capture.running) {
        ++s_capture.dropped_frames;
    }
    media_irq_enable();

    if (queued && s_capture_sem_ready) {
        (void)os_sem_post(&s_capture_sem);
    }
}

static void __capture_adc_output(void *priv, s16 *data, int len)
{
    WL83_CAPTURE_STATE_T *state = (WL83_CAPTURE_STATE_T *)priv;
    s16 mono[WL83_CAPTURE_DMA_POINTS];
    uint32_t sample_count;
    uint32_t frames;
    uint32_t frame;

    if (state == NULL || data == NULL || len <= 0 || (len & 1) || !state->running) {
        return;
    }

    sample_count = (uint32_t)len / sizeof(s16);
    if (state->channel_count <= 1) {
        __capture_queue_write((const uint8_t *)data, (uint32_t)len);
        return;
    }

    /* WL83's ADC callback len is the byte count for one channel. With async
     * ADC enabled, data contains interleaved samples for all ADC channels,
     * so len / sizeof(s16) is already the number of output mono frames. */
    frames = sample_count;
    if (frames > WL83_CAPTURE_DMA_POINTS) {
        frames = WL83_CAPTURE_DMA_POINTS;
    }
    for (frame = 0; frame < frames; ++frame) {
        if ((state->channel_map & (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) ==
            (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) {
            int32_t mixed = (int32_t)data[frame * state->channel_count] +
                            (int32_t)data[frame * state->channel_count + 1u];
            mono[frame] = (s16)(mixed / 2);
        } else {
            mono[frame] = data[frame * state->channel_count + state->channel_index];
        }
    }
    if (frames != 0) {
        __capture_queue_write((const uint8_t *)mono, frames * sizeof(s16));
    }
}

static void __capture_dispatch_task(void *arg)
{
    uint8_t frame[WL83_CAPTURE_FRAME_SIZE];
    (void)arg;

    for (;;) {
        (void)os_sem_pend(&s_capture_sem, 0);

        for (;;) {
            JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
            void *cookie;
            uint32_t first;
            uint8_t dispatch = 0;

            media_irq_disable();
            if (!s_capture.running || s_capture.used < sizeof(frame) || s_capture.callback == NULL) {
                media_irq_enable();
                break;
            }

            first = WL83_CAPTURE_QUEUE_SIZE - s_capture.read_pos;
            if (first > sizeof(frame)) {
                first = sizeof(frame);
            }
            memcpy(frame, s_capture_queue + s_capture.read_pos, first);
            if (sizeof(frame) > first) {
                memcpy(frame + first, s_capture_queue, sizeof(frame) - first);
            }
            s_capture.read_pos = (s_capture.read_pos + sizeof(frame)) % WL83_CAPTURE_QUEUE_SIZE;
            s_capture.used -= sizeof(frame);
            ++s_capture.callbacks_inflight;
            callback = s_capture.callback;
            cookie = s_capture.cookie;
            dispatch = 1;
            media_irq_enable();

            if (dispatch) {
                callback(cookie, frame, sizeof(frame), 0);
                media_irq_disable();
                --s_capture.callbacks_inflight;
                media_irq_enable();
            }
        }
    }
}

static void __playback_write_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)os_sem_pend(&s_play_sem, 0);
        for (;;) {
            const uint8_t *data;
            struct audio_dac_hdl *dac;
            uint32_t contiguous;
            int running;
            int ret;

            (void)os_mutex_pend(&s_play_lock, 0);
            running = s_play.running;
            /* Flush seals producers but lets the worker drain accepted PCM. */
            if (!running || (!s_play.accepting_writes && !s_play.draining) || s_play.used == 0 ||
                s_play.dac == NULL) {
                (void)os_mutex_post(&s_play_lock);
                break;
            }
            contiguous = WL83_PLAY_QUEUE_SIZE - s_play.read_pos;
            if (contiguous > s_play.used) {
                contiguous = s_play.used;
            }
            data = s_play_queue + s_play.read_pos;
            dac = s_play.dac;
            s_play.writer_inflight = 1;
            (void)os_mutex_post(&s_play_lock);

            ret = audio_dac_write(dac, (void *)data, (int)contiguous);
            if (ret > 0) {
                if (!s_play.writer_output_logged) {
                    s_play.writer_output_logged = 1;
                    printf("[JIELI_AUDIO_WL83] DAC write accepted: requested=%u accepted=%d\n",
                           (unsigned)contiguous, ret);
                }
                if ((uint32_t)ret > contiguous) {
                    ret = (int)contiguous;
                }
                (void)os_mutex_pend(&s_play_lock, 0);
                s_play.read_pos = (s_play.read_pos + (uint32_t)ret) % WL83_PLAY_QUEUE_SIZE;
                s_play.used -= (uint32_t)ret;
                s_play.written_total += (uint32_t)ret;
                s_play.writer_inflight = 0;
                (void)os_mutex_post(&s_play_lock);
                continue;
            }
            if (ret == 0) {
                (void)os_mutex_pend(&s_play_lock, 0);
                s_play.writer_inflight = 0;
                (void)os_mutex_post(&s_play_lock);
                os_time_dly(1);
                continue;
            }

            (void)os_mutex_pend(&s_play_lock, 0);
            s_play.write_error = ret;
            s_play.read_pos = s_play.write_pos;
            s_play.used = 0;
            s_play.writer_inflight = 0;
            (void)os_mutex_post(&s_play_lock);
            printf("[JIELI_AUDIO_WL83] DAC writer failed: %d\n", ret);
            break;
        }
    }
}

int jieli_audio_native_prepare(void)
{
    int ret;

    if (!s_capture_sem_ready) {
        ret = os_sem_create(&s_capture_sem, 0);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] capture semaphore creation failed: %d\n", ret);
            return -1;
        }
        s_capture_sem_ready = 1;
    }
    if (!s_capture_task_ready) {
        ret = os_task_create(__capture_dispatch_task, NULL, WL83_CAPTURE_TASK_PRIORITY,
                             WL83_CAPTURE_TASK_STACK, 64, WL83_CAPTURE_TASK_NAME);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] capture task creation failed: %d\n", ret);
            return -1;
        }
        s_capture_task_ready = 1;
    }
    if (!s_play_sem_ready) {
        ret = os_sem_create(&s_play_sem, 0);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] playback semaphore creation failed: %d\n", ret);
            return -1;
        }
        s_play_sem_ready = 1;
    }
    if (!s_play_lock_ready) {
        ret = os_mutex_create(&s_play_lock);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] playback mutex creation failed: %d\n", ret);
            return -1;
        }
        s_play_lock_ready = 1;
    }
    if (!s_play_task_ready) {
        ret = os_task_create(__playback_write_task, NULL, WL83_PLAY_TASK_PRIORITY,
                             WL83_PLAY_TASK_STACK, 64, WL83_PLAY_TASK_NAME);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] playback task creation failed: %d\n", ret);
            return -1;
        }
        s_play_task_ready = 1;
    }

    printf("[JIELI_AUDIO_WL83] native ADC/DAC backend ready: adc_bits=%u dac=%p\n",
           (unsigned)adc_hdl.bit_width, (void *)&dac_hdl);
    return 0;
}

static int __capture_resources_close(void)
{
    uint32_t retries = 0;
    uint32_t callbacks_inflight;
    int ret;

    if (s_capture.output_registered) {
        audio_adc_del_output_handler(&adc_hdl, &s_capture.output);
        s_capture.output_registered = 0;
    }
    if (s_capture.mic_opened) {
        ret = audio_adc_mic_close(&s_capture.mic);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] audio_adc_mic_close failed: %d\n", ret);
        } else {
            s_capture.mic_opened = 0;
        }
    } else {
        ret = 0;
    }
    if (ret == 0) {
        if (s_capture.dma_buffer != NULL && !s_capture.dma_attached) {
            AUD_ADC_DMA_FREE(s_capture.dma_buffer);
        }
        s_capture.dma_buffer = NULL; /* audio_adc_mic_close releases attached DMA buffers */
        s_capture.dma_attached = 0;
        s_capture.resources_opened = 0;
    }

    do {
        media_irq_disable();
        callbacks_inflight = s_capture.callbacks_inflight;
        media_irq_enable();
        if (callbacks_inflight == 0) {
            return ret == 0 ? 0 : -1;
        }
        os_time_dly(1);
    } while (retries++ < 100u);

    printf("[JIELI_AUDIO_WL83] capture callbacks still in flight: %u\n",
           (unsigned)callbacks_inflight);
    return -1;
}

static int __capture_resources_open(void)
{
    const uint32_t channel_map = JIELI_AUDIO_MIC_CHANNEL_MAP;
    uint32_t channel;
    uint32_t dma_size;
    int ret;

    if (s_capture.resources_opened) {
        return 0;
    }
    audio_adc_file_init();
    printf("[JIELI_AUDIO_WL83] ADC capture open: map=0x%x board_mics=%u all_channels=%u async=%d vm_map=0x%x\n",
           (unsigned)channel_map, (unsigned)JIELI_AUDIO_MIC_CHANNEL_COUNT,
           (unsigned)JIELI_AUDIO_ADC_ALL_CHANNEL_OPEN, config_adc_async_en,
           (unsigned)audio_adc_file_get_mic_en_map());
    /* adc_file_mic_open() takes a channel bitmask and walks its set bits itself,
     * so this loop mirrors what it does internally; keeping it separate lets a
     * failing onboard input be logged before the vendor ADC asserts. The vendor
     * opens mics the same way (audio/jl_kws/jl_kws_audio.c, voice_mic_data.c). */
    for (channel = 0; channel < AUDIO_ADC_MAX_NUM; ++channel) {
        const uint32_t channel_bit = AUDIO_ADC_MIC(channel);
        if ((channel_map & channel_bit) == 0) {
            continue;
        }
        printf("[JIELI_AUDIO_WL83] opening ADC MIC%u: bit=0x%x mode=%u ain=%u bias=%u bias_rsel=%u dcc=%u inside_bias=%u power_io=%u dmic=%u\n",
               (unsigned)channel, (unsigned)channel_bit,
               (unsigned)adc_platform_cfg_table[channel].mic_mode,
               (unsigned)adc_platform_cfg_table[channel].mic_ain_sel,
               (unsigned)adc_platform_cfg_table[channel].mic_bias_sel,
               (unsigned)adc_platform_cfg_table[channel].mic_bias_rsel,
               (unsigned)adc_platform_cfg_table[channel].mic_dcc,
               (unsigned)adc_platform_cfg_table[channel].inside_bias_resistor,
               (unsigned)adc_platform_cfg_table[channel].power_io,
               (unsigned)adc_platform_cfg_table[channel].dmic_enable);
        ret = adc_file_mic_open(&s_capture.mic, (int)channel_bit);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] adc_file_mic_open failed: %d MIC%u map=0x%x\n",
                   ret, (unsigned)channel, (unsigned)channel_bit);
            return -1;
        }
        printf("[JIELI_AUDIO_WL83] ADC MIC%u open returned: %d\n",
               (unsigned)channel, ret);
    }
    s_capture.mic_opened = 1;

    ret = audio_adc_mic_set_sample_rate(&s_capture.mic, (int)s_capture.sample_rate);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] ADC sample-rate setup failed: %d\n", ret);
        goto __error;
    }

    s_capture.channel_count = config_adc_async_en ? WL83_CAPTURE_CHANNEL_LIMIT : 1;
    if (s_capture.channel_count == 0 || s_capture.channel_count > WL83_CAPTURE_CHANNEL_LIMIT) {
        printf("[JIELI_AUDIO_WL83] invalid ADC channel count: %u\n",
               (unsigned)s_capture.channel_count);
        goto __error;
    }
    s_capture.channel_map = channel_map;
    if ((channel_map & (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) ==
        (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) {
        s_capture.channel_index = 0;
    } else {
        s_capture.channel_index = (uint8_t)get_adc_seq(&adc_hdl, (u16)channel_map);
    }
    if (s_capture.channel_index >= s_capture.channel_count) {
        printf("[JIELI_AUDIO_WL83] invalid ADC sequence: index=%u channels=%u\n",
               (unsigned)s_capture.channel_index, (unsigned)s_capture.channel_count);
        goto __error;
    }

    dma_size = WL83_CAPTURE_DMA_POINTS * sizeof(s16) * WL83_CAPTURE_DMA_BUFFER_COUNT *
               s_capture.channel_count;
    s_capture.dma_buffer = (s16 *)AUD_ADC_DMA_MALLOC(dma_size);
    if (s_capture.dma_buffer == NULL) {
        printf("[JIELI_AUDIO_WL83] ADC DMA allocation failed: bytes=%u\n", (unsigned)dma_size);
        goto __error;
    }
    ret = audio_adc_mic_set_buffs(&s_capture.mic, s_capture.dma_buffer,
                                  WL83_CAPTURE_DMA_POINTS * 2u,
                                  WL83_CAPTURE_DMA_BUFFER_COUNT);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] ADC DMA setup failed: %d\n", ret);
        AUD_ADC_DMA_FREE(s_capture.dma_buffer);
        s_capture.dma_buffer = NULL;
        goto __error;
    }
    s_capture.dma_attached = 1;

    ret = audio_adc_mic_set_gain(&s_capture.mic, channel_map,
                                 (s_capture.volume * 19 + 50) / 100);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] ADC gain restore failed: %d\n", ret);
        goto __error;
    }

    s_capture.output.priv = &s_capture;
    s_capture.output.handler = __capture_adc_output;
    audio_adc_add_output_handler(&adc_hdl, &s_capture.output);
    s_capture.output_registered = 1;
    s_capture.resources_opened = 1;
    printf("[JIELI_AUDIO_WL83] onboard MIC1/MIC2 configured: map=0x%x adc_channels=%u sequence=%u mix=%s rate=%u dma=%u frame=%u\n",
           (unsigned)channel_map, (unsigned)s_capture.channel_count,
           (unsigned)s_capture.channel_index,
           (channel_map & (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) ==
                   (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1) ? "average" : "single",
           (unsigned)s_capture.sample_rate,
           (unsigned)dma_size, (unsigned)WL83_CAPTURE_DMA_POINTS);
    return 0;

__error:
    (void)__capture_resources_close();
    return -1;
}

int jieli_audio_native_ai_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config,
                               JIELI_AUDIO_NATIVE_CAPTURE_CB callback, void *cookie)
{
    if (config == NULL || callback == NULL) {
        return -2;
    }
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO_WL83] unsupported capture format: rate=%u bits=%u channels=%u\n",
               (unsigned)config->sample_rate, (unsigned)config->bits_per_sample,
               (unsigned)config->channels);
        return -3;
    }
    if (s_capture.initialized) {
        return -4;
    }
    if (!s_capture_task_ready || adc_hdl.bit_width != DATA_BIT_WIDE_16BIT) {
        printf("[JIELI_AUDIO_WL83] ADC runtime not initialized: task=%u adc_bits=%u\n",
               (unsigned)s_capture_task_ready, (unsigned)adc_hdl.bit_width);
        return -1;
    }

    memset(&s_capture, 0, sizeof(s_capture));
    s_capture.callback = callback;
    s_capture.cookie = cookie;
    s_capture.sample_rate = config->sample_rate;
    /* The onboard mic needs the top of the ADC gain range. At the previous
     * default of 50 this maps to gain 10 (+12 dB), which left speech peaks
     * around 540 of 32767 - buried in the noise floor, so the loopback played
     * back silence. 100 maps to gain 19 (+30 dB), the top of
     * audio_adc_mic_set_gain()'s documented 0(-8dB)~19(30dB) range, and
     * measured speech peaks of ~6600 (-14 dBFS). */
    s_capture.volume = 100;
    s_capture.initialized = 1;
    if (__capture_resources_open() != 0) {
        memset(&s_capture, 0, sizeof(s_capture));
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_start(void)
{
    int ret;

    if (!s_capture.initialized) {
        return -4;
    }
    if (s_capture.running) {
        return 0;
    }
    if (!s_capture.resources_opened && __capture_resources_open() != 0) {
        return -1;
    }
    media_irq_disable();
    s_capture.running = 1;
    s_capture.read_pos = s_capture.write_pos = s_capture.used = 0;
    media_irq_enable();

    ret = audio_adc_mic_start(&s_capture.mic);
    if (ret != 0) {
        media_irq_disable();
        s_capture.running = 0;
        media_irq_enable();
        printf("[JIELI_AUDIO_WL83] audio_adc_mic_start failed: %d\n", ret);
        (void)__capture_resources_close();
        return -1;
    }

    printf("[JIELI_AUDIO_WL83] onboard MIC capture started\n");
    return 0;
}

int jieli_audio_native_ai_stop(void)
{
    if (!s_capture.initialized) {
        return 0;
    }
    media_irq_disable();
    s_capture.running = 0;
    s_capture.used = 0;
    s_capture.read_pos = s_capture.write_pos = 0;
    media_irq_enable();

    if (__capture_resources_close() != 0) {
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_uninit(void)
{
    if (!s_capture.initialized) {
        return 0;
    }
    if (jieli_audio_native_ai_stop() != 0) {
        return -1;
    }
    memset(&s_capture, 0, sizeof(s_capture));
    return 0;
}

int jieli_audio_native_ai_set_volume(int volume)
{
    int gain;
    int ret;
    if (volume < 0 || volume > 100) {
        return -2;
    }
    if (!s_capture.initialized) {
        return -4;
    }
    s_capture.volume = volume;
    if (!s_capture.resources_opened) {
        return 0;
    }
    gain = (volume * 19 + 50) / 100;
    ret = audio_adc_mic_set_gain(&s_capture.mic, s_capture.channel_map, gain);
    return ret == 0 ? 0 : -1;
}

int jieli_audio_native_ai_get_volume(int *volume)
{
    if (volume == NULL) {
        return -2;
    }
    if (!s_capture.initialized) {
        return -4;
    }
    *volume = s_capture.volume;
    return 0;
}

int jieli_audio_native_ao_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config, void **stream)
{
    int ret;
    if (config == NULL || stream == NULL) {
        return -2;
    }
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO_WL83] unsupported playback format: rate=%u bits=%u channels=%u\n",
               (unsigned)config->sample_rate, (unsigned)config->bits_per_sample,
               (unsigned)config->channels);
        return -3;
    }
    if (s_play.initialized) {
        return -4;
    }
    s_play.dac = &dac_hdl;
    if (!s_play_task_ready || s_play.dac == NULL || s_play.dac->pd == NULL) {
        printf("[JIELI_AUDIO_WL83] DAC runtime not initialized\n");
        return -1;
    }
    ret = audio_dac_set_sample_rate(s_play.dac, (int)config->sample_rate);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] DAC sample-rate setup failed: %d\n", ret);
        s_play.dac = NULL;
        return -1;
    }
    s_play.sample_rate = config->sample_rate;
    s_play.volume = 50;

    /* Enter a JieLi audio state before the DAC channel starts.
     *
     * Under SYS_VOL_TYPE == VOL_TYPE_DIGITAL the vendor fade handler
     * (audio_fade_in_fade_out in audio/common/audio_volume_mixer.c) ignores the
     * level handed to audio_dac_set_volume() and applies the mixer's own
     * analog_volume_l/r and digital_volume instead. Those are zero-initialized
     * and only ever filled in by app_audio_state_switch(), which every vendor
     * app calls when entering a playback state. digital_volume is Q14, so a
     * zero value means the DAC plays silence at every requested volume.
     *
     * TuyaOpen drives the DAC directly and never runs the vendor app, so this
     * switch has to happen here. A NULL dvol handle is safe: audio_digital_vol_set()
     * returns early for NULL. */
    app_audio_state_switch(APP_AUDIO_STATE_MUSIC,
                           app_audio_volume_max_query(AppVol_BT_MUSIC), NULL);

    s_play.read_pos = 0;
    s_play.write_pos = 0;
    s_play.used = 0;
    s_play.write_error = 0;
    s_play.writer_inflight = 0;
    s_play.queued_total = 0;
    s_play.written_total = 0;
    s_play.accepting_writes = 0;
    s_play.channel_open = 0;
    s_play.initialized = 1;
    s_play.running = 0;
    *stream = &s_play;
    printf("[JIELI_AUDIO_WL83] onboard SPK configured: rate=%u channel=0x%x\n",
           (unsigned)s_play.sample_rate, (unsigned)audio_dac_get_channel(s_play.dac));
    return 0;
}

/* Map the requested 0-100 level onto JieLi's digital volume curve and write it
 * straight to the DAC.
 *
 * audio_dac_set_volume() only records a level, and under
 * SYS_VOL_TYPE == VOL_TYPE_DIGITAL the fade handler that consumes it applies the
 * mixer's own digital_volume rather than the recorded value, so the recorded
 * level never reaches the hardware on its own. The curve itself is the vendor's
 * (audio/common/audio_dvol.c default_dig_vol_table): Q14, 1.5 dB per step, with
 * level 31 at 16384 == 0 dB.
 *
 * Must run after audio_dac_channel_start(): the fade handler fires at channel
 * start and would overwrite an earlier write. */
static void __play_apply_digital_volume(void)
{
    struct audio_dac_hdl *dac;
    u32 level;
    u32 gain;

    (void)os_mutex_pend(&s_play_lock, 0);
    dac = s_play.dac;
    level = ((u32)s_play.volume * WL83_PLAY_VOLUME_LEVELS + 50u) / 100u;
    (void)os_mutex_post(&s_play_lock);

    if (dac == NULL) {
        return;
    }
    if (level == 0u) {
        gain = 0u;
    } else {
        /* Every step below the unity level costs 1.5 dB. */
        gain = (u32)(16384.0f * eq_db2mag(((float)level - (float)WL83_PLAY_VOLUME_LEVELS) * 1.5f) + 0.5f);
    }
    /* audio_dac_set_RL_digital_vol() is declared in audio_dac.h but not exported
     * by media.a, so drive both channels the way the fade handler's 0x3 mask
     * does. */
    (void)audio_dac_set_L_digital_vol(dac, (u16)gain);
    (void)audio_dac_set_R_digital_vol(dac, (u16)gain);
}

int jieli_audio_native_ao_start(void *stream)
{
    int ret;
    if (stream != &s_play || !s_play.initialized || s_play.dac == NULL) {
        return -4;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    if (s_play.running) {
        uint8_t channel_open = s_play.channel_open;
        if (s_play.accepting_writes) {
            (void)os_mutex_post(&s_play_lock);
            return 0;
        }
        (void)os_mutex_post(&s_play_lock);

        /* A prior drain timeout seals producers but leaves the DAC running so
         * the writer can finish accepted PCM. Reopen the stream on next start. */
        if (!channel_open) {
            ret = audio_dac_start(s_play.dac);
            if (ret != 0) {
                printf("[JIELI_AUDIO_WL83] audio_dac_start retry failed: %d\n", ret);
                return -1;
            }
            audio_dac_channel_start(NULL);
        }
        (void)os_mutex_pend(&s_play_lock, 0);
        if (s_play.write_error != 0) {
            printf("[JIELI_AUDIO_WL83] clearing prior DAC writer error before restart: %d\n",
                   s_play.write_error);
            s_play.write_error = 0;
        }
        s_play.accepting_writes = 1;
        s_play.draining = 0;
        s_play.channel_open = 1;
        (void)os_mutex_post(&s_play_lock);
        __play_apply_digital_volume();
        if (s_play_sem_ready) {
            (void)os_sem_post(&s_play_sem);
        }
        return 0;
    }
    (void)os_mutex_post(&s_play_lock);
    ret = audio_dac_start(s_play.dac);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] audio_dac_start failed: %d\n", ret);
        return -1;
    }
    /* audio_dac_start powers the DAC core; the SDK's public playback sample
     * also starts the DAC FIFO channel before writing PCM. */
    audio_dac_channel_start(NULL);
    (void)os_mutex_pend(&s_play_lock, 0);
    s_play.running = 1;
    s_play.accepting_writes = 1;
    s_play.draining = 0;
    s_play.write_error = 0;
    s_play.channel_open = 1;
    (void)os_mutex_post(&s_play_lock);
    /* The fade handler has just run with the mixer's own gain; put the level
     * the caller asked for on top of it. */
    __play_apply_digital_volume();
    printf("[JIELI_AUDIO_WL83] onboard SPK playback started\n");
    return 0;
}

int jieli_audio_native_ao_stop(void *stream)
{
    uint32_t retries = 0;
    uint8_t writer_inflight;
    uint8_t channel_open;
    int ret;
    if (stream != &s_play || !s_play.initialized || s_play.dac == NULL) {
        return -4;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    if (!s_play.running) {
        (void)os_mutex_post(&s_play_lock);
        return 0;
    }
    s_play.accepting_writes = 0;
    s_play.draining = 0;
    (void)os_mutex_post(&s_play_lock);
    for (;;) {
        (void)os_mutex_pend(&s_play_lock, 0);
        writer_inflight = s_play.writer_inflight;
        (void)os_mutex_post(&s_play_lock);
        if (!writer_inflight) {
            break;
        }
        if (retries++ >= WL83_PLAY_WRITER_DRAIN_LIMIT) {
            printf("[JIELI_AUDIO_WL83] DAC writer did not quiesce during stop\n");
            return -1;
        }
        os_time_dly(1);
    }

    (void)os_mutex_pend(&s_play_lock, 0);
    /* STOP means interrupt playback promptly and discard software-queued PCM. */
    s_play.read_pos = s_play.write_pos;
    s_play.used = 0;
    channel_open = s_play.channel_open;
    (void)os_mutex_post(&s_play_lock);

    if (channel_open) {
        audio_dac_channel_close(NULL);
        (void)os_mutex_pend(&s_play_lock, 0);
        s_play.channel_open = 0;
        (void)os_mutex_post(&s_play_lock);
    }
    ret = audio_dac_stop(s_play.dac);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] audio_dac_stop failed: %d\n", ret);
        /* Keep writes gated and running set so callers can retry stop safely. */
        return -1;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    s_play.running = 0;
    s_play.accepting_writes = 0;
    s_play.channel_open = 0;
    (void)os_mutex_post(&s_play_lock);
    return 0;
}

int jieli_audio_native_ao_uninit(void *stream)
{
    if (stream != &s_play) {
        return -2;
    }
    if (!s_play.initialized) {
        return 0;
    }
    if (s_play.running && jieli_audio_native_ao_stop(stream) != 0) {
        return -1;
    }
    memset(&s_play, 0, sizeof(s_play));
    return 0;
}

int jieli_audio_native_ao_set_volume(void *stream, int volume)
{
    int ret;
    uint8_t channel_open;
    if (stream != &s_play || volume < 0 || volume > 100) {
        return -2;
    }
    if (!s_play.initialized || s_play.dac == NULL) {
        return -4;
    }
    /* audio_dac_set_volume() takes JieLi's 0-100 level, the same scale its own
     * volume mixer feeds it (IDLE_DEFAULT_MAX_VOLUME is 100). Rescaling to a
     * 0-15 hardware-looking range capped playback at 15% and turned the 80%
     * default into a gain of 12, which is inaudible on the dev board's
     * amplifier. The capture path's 0-19 rescaling is correct: that is the
     * documented audio_adc_mic_set_gain() range. */
    ret = audio_dac_set_volume(s_play.dac, (u8)volume);
    if (ret != 0) {
        return -1;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    s_play.volume = volume;
    channel_open = s_play.channel_open;
    (void)os_mutex_post(&s_play_lock);
    if (channel_open) {
        /* Already playing, so the fade handler will not fire again and the
         * recorded level alone would not reach the hardware. Before the first
         * start, ao_start() applies it instead. */
        __play_apply_digital_volume();
    }
    return 0;
}

int jieli_audio_native_ao_get_volume(void *stream, int *volume)
{
    if (stream != &s_play || volume == NULL) {
        return -2;
    }
    if (!s_play.initialized) {
        return -4;
    }
    *volume = s_play.volume;
    return 0;
}

int jieli_audio_native_ao_write(void *stream, const uint8_t *data, size_t size)
{
    uint32_t first;

    if (stream != &s_play || data == NULL || size == 0 || size > WL83_PLAY_QUEUE_SIZE || (size & 1u)) {
        return -2;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    if (!s_play.initialized || !s_play.running || !s_play.accepting_writes || s_play.dac == NULL) {
        (void)os_mutex_post(&s_play_lock);
        return -4;
    }
    if (s_play.write_error != 0) {
        (void)os_mutex_post(&s_play_lock);
        /* Reserve -31 for queue-full backpressure; report DAC failures as COM_ERROR. */
        return -1;
    }
    if (size > WL83_PLAY_QUEUE_SIZE - s_play.used) {
        (void)os_mutex_post(&s_play_lock);
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    first = WL83_PLAY_QUEUE_SIZE - s_play.write_pos;
    if (first > size) {
        first = (uint32_t)size;
    }
    memcpy(s_play_queue + s_play.write_pos, data, first);
    if (size > first) {
        memcpy(s_play_queue, data + first, size - first);
    }
    s_play.write_pos = (s_play.write_pos + (uint32_t)size) % WL83_PLAY_QUEUE_SIZE;
    s_play.used += (uint32_t)size;
    s_play.queued_total += (uint32_t)size;
    (void)os_mutex_post(&s_play_lock);

    (void)os_sem_post(&s_play_sem);
    return 0;
}

int jieli_audio_native_ao_flush(void *stream)
{
    uint32_t retries = 0;
    uint32_t settle_ticks = 0;
    uint32_t queued_bytes;
    uint8_t writer_inflight;
    int write_error;
    struct audio_dac_hdl *dac;

    if (stream != &s_play) {
        return -2;
    }

    (void)os_mutex_pend(&s_play_lock, 0);
    if (!s_play.initialized || s_play.dac == NULL) {
        (void)os_mutex_post(&s_play_lock);
        return -4;
    }
    s_play.accepting_writes = 0;
    s_play.draining = 1;
    dac = s_play.dac;
    (void)os_mutex_post(&s_play_lock);
    printf("[JIELI_AUDIO_WL83] draining speaker queue: queued=%u session_in=%u session_out=%u\n",
           (unsigned)s_play.used, (unsigned)s_play.queued_total, (unsigned)s_play.written_total);

    for (;;) {
        (void)os_mutex_pend(&s_play_lock, 0);
        queued_bytes = s_play.used;
        writer_inflight = s_play.writer_inflight;
        write_error = s_play.write_error;
        (void)os_mutex_post(&s_play_lock);

        if (write_error != 0) {
            int stop_ret;
            printf("[JIELI_AUDIO_WL83] speaker drain aborted: DAC writer error=%d queued=%u\n",
                   write_error, (unsigned)queued_bytes);
            (void)os_mutex_pend(&s_play_lock, 0);
            s_play.draining = 0;
            (void)os_mutex_post(&s_play_lock);
            stop_ret = jieli_audio_native_ao_stop(stream);
            if (stop_ret == 0) {
                (void)os_mutex_pend(&s_play_lock, 0);
                s_play.write_error = 0;
                (void)os_mutex_post(&s_play_lock);
            }
            return -1;
        }

        if (queued_bytes == 0 && !writer_inflight) {
            /* Everything offered to the DAC has been accepted and no write is in
             * flight, so the only PCM left is what the DAC FIFO already holds.
             *
             * audio_dac_idle() cannot answer that question here: on wl83 it does
             * not report idle while the DAC is started, so the previous
             * condition was unsatisfiable and every flush burned the whole
             * retry budget (~5 s) before returning OPRT_TIMEOUT. tkl_ao_stop()
             * treats that as fatal and returns without calling
             * jieli_audio_native_ao_stop(), so the DAC was never stopped and
             * the next flush timed out too - a permanent 5 s penalty per stop,
             * which in turn starved the AI input task.
             *
             * Wait the FIFO out by time instead. See WL83_PLAY_DAC_SETTLE_TICKS
             * for where the number comes from. */
            if (settle_ticks++ >= WL83_PLAY_DAC_SETTLE_TICKS) {
                (void)os_mutex_pend(&s_play_lock, 0);
                s_play.draining = 0;
                (void)os_mutex_post(&s_play_lock);
                printf("[JIELI_AUDIO_WL83] speaker queue drained: session_in=%u session_out=%u\n",
                       (unsigned)s_play.queued_total, (unsigned)s_play.written_total);
                return 0;
            }
        } else {
            settle_ticks = 0;
        }

        if (retries++ >= WL83_PLAY_DRAIN_LIMIT) {
            printf("[JIELI_AUDIO_WL83] speaker drain timeout: queued=%u writer=%u dac_idle=%d "
                   "session_in=%u session_out=%u\n",
                   (unsigned)queued_bytes, (unsigned)writer_inflight, audio_dac_idle(dac),
                   (unsigned)s_play.queued_total, (unsigned)s_play.written_total);
            /* Leave draining enabled: the background writer can still deliver
             * accepted PCM, and a later flush/stop can retry the idle wait. */
            return OPRT_TIMEOUT;
        }
        os_time_dly(1);
    }
}
#elif defined(JIELI_SELECTED_CHIP_WL82)
/* WL82 (AC791) provider: the SDK's audio_server.
 *
 * audio_server.h pulls in fs/fs.h, which defines FILE and the fread/fwrite/
 * fseek/ftell/fgetc family exactly like newlib's <stdio.h> - and differently
 * from it. tuya_cloud_types.h (via tkl_audio.h) always includes <stdio.h>, so
 * no translation unit can hold both. The provider only ever passes FILE
 * around as an opaque handle (the vendor VFS hands it straight back), so let
 * the standard header win and keep fs/fs.h out of this translation unit. The
 * guard stays defined for the rest of the file on purpose: a later transitive
 * include of fs/fs.h would break the build again. */
#define __FS_H__
#include "system/server/server_core.h"
#include "server/audio_server.h"

#define JIELI_AUDIO_PLAY_QUEUE_SIZE (16u * 1024u)
/* Ticks to let the decoder drain the play queue on flush (~10 ms each, so this
 * is ~1 s - more than the 0.5 s the 16 KiB queue can hold at 16 kHz). */
#define JIELI_AUDIO_PLAY_FLUSH_LIMIT 100u
#define JIELI_AUDIO_CAPTURE_BUFFER_SIZE (8u * 1024u)
#define JIELI_AUDIO_CAPTURE_SESSION_COUNT 128u

/* The vendor audio tests release their VFS context after AUDIO_ENC_CLOSE.
 * Keep callback records in static storage and only recycle one after CLOSE
 * succeeds and all observed VFS callbacks have returned. */
typedef struct {
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;
    uint32_t callbacks_inflight;
    uint8_t allocated;
    uint8_t enabled;
    uint8_t stopped;
    uint8_t closed;
} AUDIO_CAPTURE_SESSION_T;

typedef struct {
    struct server *server;
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;
    AUDIO_CAPTURE_SESSION_T *current;
    uint32_t sample_rate;
    int volume;
    uint8_t initialized;
} AUDIO_CAPTURE_STATE_T;

typedef struct {
    struct server *server;
    OS_SEM data_sem;
    uint8_t queue[JIELI_AUDIO_PLAY_QUEUE_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t used;
    uint32_t sample_rate;
    int volume;
    uint8_t sem_ready;
    uint8_t initialized;
    uint8_t running;
} AUDIO_PLAY_STATE_T;

static OS_MUTEX s_audio_lock;
static uint8_t s_audio_lock_ready;
static AUDIO_CAPTURE_SESSION_T s_capture_sessions[JIELI_AUDIO_CAPTURE_SESSION_COUNT];
static AUDIO_CAPTURE_STATE_T s_capture;
static AUDIO_PLAY_STATE_T s_play;

static void __lock(void)
{
    (void)os_mutex_pend(&s_audio_lock, 0);
}

static void __unlock(void)
{
    (void)os_mutex_post(&s_audio_lock);
}

uintptr_t jieli_audio_native_critical_enter(void)
{
    __lock();
    return 1;
}

void jieli_audio_native_critical_exit(uintptr_t state)
{
    (void)state;
    __unlock();
}

int jieli_audio_native_prepare(void)
{
    int ret;
    if (s_audio_lock_ready) return 0;
    ret = os_mutex_create(&s_audio_lock);
    if (ret != 0) {
        printf("[JIELI_AUDIO] mutex creation failed: %d\n", ret);
        return -1;
    }
    s_audio_lock_ready = 1;
    return 0;
}

static void *__audio_vfs_open(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

static int __audio_vfs_fseek(void *file, u32 offset, int mode)
{
    (void)file;
    (void)offset;
    (void)mode;
    return -1;
}

static int __audio_vfs_ftell(void *file)
{
    (void)file;
    return -1;
}

static int __audio_vfs_flen(void *file)
{
    (void)file;
    return -1;
}

static int __audio_vfs_fclose(void *file)
{
    (void)file;
    return 0;
}

static int __capture_fwrite(void *file, void *data, u32 len)
{
    AUDIO_CAPTURE_SESSION_T *session = (AUDIO_CAPTURE_SESSION_T *)file;
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;

    if (session == NULL || data == NULL || len == 0) return 0;
    __lock();
    if (!session->enabled || session->callback == NULL) {
        __unlock();
        return (int)len;
    }
    ++session->callbacks_inflight;
    callback = session->callback;
    cookie = session->cookie;
    __unlock();

    callback(cookie, (const uint8_t *)data, len, 0);

    __lock();
    --session->callbacks_inflight;
    __unlock();
    return (int)len;
}

static const struct audio_vfs_ops s_capture_vfs = {
    .fopen = __audio_vfs_open,
    .fwrite = __capture_fwrite,
    .fseek = __audio_vfs_fseek,
    .ftell = __audio_vfs_ftell,
    .flen = __audio_vfs_flen,
    .fclose = __audio_vfs_fclose,
};

/* No audio_server event handler is registered on purpose.
 *
 * server_register_event_handler() binds the callback to the *calling* task and
 * requires that task to service a vendor message queue. Our callers are
 * TuyaOpen threads (tuya_app_main, ai_player) that never do, so the server
 * spins in "server_event_handler wait_send_event: <task>" and the next
 * server_request() deadlocks - measured on AC791 as tdl_audio_close() never
 * returning during a repeated open/close loop. The vendor registers a handler
 * only because it consumes the events; this adapter does not, so the delivery
 * path is simply left unregistered. The raw-SDK probe, which registers nothing,
 * ran every phase to completion on the same board.
 */

static int __capture_open_session(void)
{
    union audio_req req;
    AUDIO_CAPTURE_SESSION_T *session = NULL;
    u32 i;
    int ret;

    __lock();
    for (i = 0; i < JIELI_AUDIO_CAPTURE_SESSION_COUNT; ++i) {
        if (!s_capture_sessions[i].allocated) {
            session = &s_capture_sessions[i];
            memset(session, 0, sizeof(*session));
            session->allocated = 1;
            session->enabled = 1;
            session->callback = s_capture.callback;
            session->cookie = s_capture.cookie;
            s_capture.current = session;
            break;
        }
    }
    __unlock();
    if (session == NULL) return -4;

    memset(&req, 0, sizeof(req));
    req.enc.cmd = AUDIO_ENC_OPEN;
    req.enc.channel = 1;
    req.enc.volume = (u8)s_capture.volume;
    req.enc.output_buf_len = JIELI_AUDIO_CAPTURE_BUFFER_SIZE;
    req.enc.sample_rate = s_capture.sample_rate;
    req.enc.format = "pcm";
    req.enc.sample_source = "mic";
    req.enc.frame_size = (u16)(s_capture.sample_rate / 25u); /* PCM bytes per vendor sample. */
    req.enc.no_auto_start = 1;
    req.enc.vfs_ops = &s_capture_vfs;
    req.enc.file = (FILE *)session;
    ret = server_request(s_capture.server, AUDIO_REQ_ENC, &req);
    if (ret != 0) {
        printf("[JIELI_AUDIO] encoder OPEN request failed: %d\n", ret);
        __lock();
        session->enabled = 0;
        session->callback = NULL;
        session->cookie = NULL;
        /* Keep allocated set: the vendor server may still hold the session as
         * req.enc.file even after a failed OPEN, so the slot must never be
         * reused (memset would corrupt whatever it still references). */
        s_capture.current = NULL;
        __unlock();
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config,
                               JIELI_AUDIO_NATIVE_CAPTURE_CB callback, void *cookie)
{
    if (config == NULL || callback == NULL) {
        printf("[JIELI_AUDIO] ai_init rejected: config=%p callback_present=%u\n",
               (void *)config, callback != NULL ? 1u : 0u);
        return -2;
    }
    printf("[JIELI_AUDIO] ai_init config: rate=%u bits=%u channels=%u cookie=%p\n",
           (unsigned)config->sample_rate, (unsigned)config->bits_per_sample,
           (unsigned)config->channels, cookie);
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO] ai_init unsupported PCM format\n");
        return -3;
    }
    if (s_capture.initialized) {
        printf("[JIELI_AUDIO] ai_init rejected: capture already initialized\n");
        return -4;
    }

    memset(&s_capture, 0, sizeof(s_capture));
    printf("[JIELI_AUDIO] opening audio_server mode=enc\n");
    s_capture.server = server_open("audio_server", "enc");
    printf("[JIELI_AUDIO] server_open encoder handle=%p\n", (void *)s_capture.server);
    if (s_capture.server == NULL) {
        printf("[JIELI_AUDIO] server_open failed: name=audio_server mode=enc (service missing or open rejected)\n");
        return -1;
    }
    printf("[JIELI_AUDIO] server_open encoder succeeded\n");
    s_capture.callback = callback;
    s_capture.cookie = cookie;
    s_capture.sample_rate = config->sample_rate;
    /* req.enc.volume is the ADC gain on a 0-100 scale, and the onboard mic needs
     * the top of it. The wl83 provider above already defaults to 100 for exactly
     * this reason; wl82 kept 50 and measured peaks of only ~75 of 32767, i.e.
     * speech buried in the noise floor. The vendor's own wl82 audio app sets
     * CONFIG_AUDIO_ADC_GAIN to 100. */
    s_capture.volume = 100;
    s_capture.initialized = 1;
    return 0;
}

int jieli_audio_native_ai_start(void)
{
    union audio_req req;
    int ret;
    if (!s_capture.initialized || s_capture.server == NULL) {
        printf("[JIELI_AUDIO] ai_start rejected: initialized=%u server=%p\n",
               (unsigned)s_capture.initialized, (void *)s_capture.server);
        return -4;
    }
    ret = __capture_open_session();
    if (ret != 0) {
        printf("[JIELI_AUDIO] capture session open failed: %d\n", ret);
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.enc.cmd = AUDIO_ENC_START;
    ret = server_request(s_capture.server, AUDIO_REQ_ENC, &req);
    if (ret != 0) {
        printf("[JIELI_AUDIO] encoder START request failed: %d\n", ret);
        (void)jieli_audio_native_ai_stop();
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_stop(void)
{
    union audio_req req;
    AUDIO_CAPTURE_SESSION_T *session = s_capture.current;
    uint32_t inflight;
    uint32_t waited = 0;
    int stop_result = 0;

    if (session == NULL) return 0;
    __lock();
    session->enabled = 0;
    __unlock();

    if (!session->stopped) {
        memset(&req, 0, sizeof(req));
        req.enc.cmd = AUDIO_ENC_STOP;
        stop_result = server_request(s_capture.server, AUDIO_REQ_ENC, &req);
        if (stop_result != 0) return -1;
        __lock();
        session->stopped = 1;
        __unlock();
    }
    if (!session->closed) {
        memset(&req, 0, sizeof(req));
        req.enc.cmd = AUDIO_ENC_CLOSE;
        if (server_request(s_capture.server, AUDIO_REQ_ENC, &req) != 0) return -1;
        __lock();
        session->closed = 1;
        __unlock();
    }

    __lock();
    inflight = session->callbacks_inflight;
    __unlock();
    /* The session remains in static storage; wait for callbacks already in
     * progress before reporting a clean stop to TKL. */
    while (inflight != 0 && waited++ < 100u) {
        os_time_dly(1);
        __lock();
        inflight = session->callbacks_inflight;
        __unlock();
    }
    if (inflight != 0) {
        printf("[JIELI_AUDIO] encoder callbacks did not drain: %u\n", (unsigned)inflight);
        return -1;
    }

    __lock();
    session->callback = NULL;
    session->cookie = NULL;
    session->allocated = 0;
    session->stopped = 0;
    session->closed = 0;
    if (s_capture.current == session) s_capture.current = NULL;
    __unlock();
    return 0;
}

int jieli_audio_native_ai_uninit(void)
{
    if (!s_capture.initialized) return 0;
    if (s_capture.current != NULL && jieli_audio_native_ai_stop() != 0) return -1;
    server_close(s_capture.server);
    s_capture.server = NULL;
    s_capture.initialized = 0;
    s_capture.callback = NULL;
    s_capture.cookie = NULL;
    return 0;
}

int jieli_audio_native_ai_set_volume(int volume)
{
    union audio_req req;
    if (volume < 0 || volume > 100) return -2;
    if (!s_capture.initialized) return -4;
    s_capture.volume = volume;
    if (s_capture.current == NULL) return 0;
    memset(&req, 0, sizeof(req));
    req.enc.cmd = AUDIO_ENC_SET_VOLUME;
    req.enc.volume = (u8)volume;
    return server_request(s_capture.server, AUDIO_REQ_ENC, &req) == 0 ? 0 : -1;
}

int jieli_audio_native_ai_get_volume(int *volume)
{
    if (volume == NULL) return -2;
    if (!s_capture.initialized) return -4;
    *volume = s_capture.volume;
    return 0;
}

static int __play_fread(void *file, void *buffer, u32 length)
{
    AUDIO_PLAY_STATE_T *state = (AUDIO_PLAY_STATE_T *)file;
    if (state == NULL || buffer == NULL) return 0;
    if (length == 0) return 0;
    for (;;) {
        u32 chunk;
        u32 first;
        __lock();
        if (state->used == 0) {
            int running = state->running;
            __unlock();
            if (!running) break;
            (void)os_sem_pend(&state->data_sem, 0);
            continue;
        }
        chunk = length;
        if (chunk > state->used) chunk = state->used;
        first = JIELI_AUDIO_PLAY_QUEUE_SIZE - state->read_pos;
        if (first > chunk) first = chunk;
        memcpy((u8 *)buffer, state->queue + state->read_pos, first);
        if (chunk > first) memcpy((u8 *)buffer + first, state->queue, chunk - first);
        state->read_pos = (state->read_pos + chunk) % JIELI_AUDIO_PLAY_QUEUE_SIZE;
        state->used -= chunk;
        __unlock();
        /* Like the vendor PCM VFS, return once any bytes are available. A
         * final short chunk must not block waiting for a future write. */
        return (int)chunk;
    }
    return 0;
}

static const struct audio_vfs_ops s_play_vfs = {
    .fopen = __audio_vfs_open,
    .fread = __play_fread,
    .fseek = __audio_vfs_fseek,
    .ftell = __audio_vfs_ftell,
    .flen = __audio_vfs_flen,
    .fclose = __audio_vfs_fclose,
};

/* Open (or re-open) the PCM decoder on the already-open dec server.
 *
 * The vendor closes the decoder as part of AUDIO_DEC_STOP, so a later bare
 * AUDIO_DEC_START is rejected with -22 (EINVAL) - measured on AC791 as every
 * utterance after the first playing nothing, with "decoder START request
 * failed: -22" repeating. The vendor's own players avoid this by re-opening
 * after a stop (local_music.c closes the server and opens it again), but the
 * TDD contract is stop-then-start on the same handle, so the adapter re-opens
 * on demand instead.
 */
static int __play_open_decoder(void)
{
    union audio_req req;

    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_OPEN;
    req.dec.channel = 1;
    req.dec.volume = (u8)s_play.volume;
    /* req.dec has two volume fields and the vendor's own players always set
     * both - app_music.c opens with digital_volume = 100 and then keeps it in
     * step with volume. Leaving it zero-initialised left the digital gain at 0
     * while only the analog volume was set, which is heard as playback that is
     * simply too quiet. */
    req.dec.digital_volume = (u8)s_play.volume;
    req.dec.output_buf_len = JIELI_AUDIO_PLAY_QUEUE_SIZE;
    req.dec.sample_rate = s_play.sample_rate;
    req.dec.dec_type = "pcm";
    req.dec.sample_source = "dac";
    req.dec.vfs_ops = &s_play_vfs;
    req.dec.file = (FILE *)&s_play;
    return server_request(s_play.server, AUDIO_REQ_DEC, &req);
}

int jieli_audio_native_ao_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config, void **stream)
{
    union audio_req req;
    int ret;
    if (config == NULL || stream == NULL) {
        printf("[JIELI_AUDIO] ao_init rejected: config=%p stream_out=%p\n",
               (void *)config, (void *)stream);
        return -2;
    }
    printf("[JIELI_AUDIO] ao_init config: rate=%u bits=%u channels=%u\n",
           (unsigned)config->sample_rate, (unsigned)config->bits_per_sample, (unsigned)config->channels);
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO] ao_init unsupported PCM format\n");
        return -3;
    }
    if (s_play.initialized) {
        printf("[JIELI_AUDIO] ao_init rejected: playback already initialized\n");
        return -4;
    }
    if (!s_play.sem_ready) {
        ret = os_sem_create(&s_play.data_sem, 0);
        if (ret != 0) {
            printf("[JIELI_AUDIO] playback semaphore creation failed: %d\n", ret);
            return -1;
        }
        s_play.sem_ready = 1;
    }
    printf("[JIELI_AUDIO] opening audio_server mode=dec\n");
    s_play.server = server_open("audio_server", "dec");
    printf("[JIELI_AUDIO] server_open decoder handle=%p\n", (void *)s_play.server);
    if (s_play.server == NULL) {
        printf("[JIELI_AUDIO] server_open failed: name=audio_server mode=dec (service missing or open rejected)\n");
        return -1;
    }
    printf("[JIELI_AUDIO] server_open decoder succeeded\n");
    s_play.sample_rate = config->sample_rate;
    /* req.dec.volume is the level the DAC streams at (the vendor logs it as
     * "dac_streamon: ... volume = N"). tdd_audio asks for 80 before starting
     * the output, but that arrives after this OPEN, so the decoder used to
     * start at 50 and stay there for the head of every utterance - heard as
     * playback that is simply too quiet. Open at the same default tdd uses. */
    s_play.volume = 80;
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    ret = __play_open_decoder();
    printf("[JIELI_AUDIO] decoder OPEN result=%d\n", ret);
    if (ret != 0) {
        printf("[JIELI_AUDIO] decoder OPEN request failed: %d\n", ret);
        server_close(s_play.server);
        s_play.server = NULL;
        return -1;
    }
    s_play.initialized = 1;
    *stream = &s_play;
    return 0;
}

int jieli_audio_native_ao_start(void *stream)
{
    union audio_req req;
    int ret;
    if (stream != &s_play || !s_play.initialized) return -2;
    __lock();
    s_play.running = 1;
    __unlock();
    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_START;
    ret = server_request(s_play.server, AUDIO_REQ_DEC, &req);
    if (ret != 0) {
        /* The server closes the decoder on STOP, so the first START after a
         * stop is rejected (-22). Re-open and try once more before giving up. */
        printf("[JIELI_AUDIO] decoder START rejected (%d); reopening\n", ret);
        if (__play_open_decoder() != 0) {
            printf("[JIELI_AUDIO] decoder reopen failed\n");
            __lock();
            s_play.running = 0;
            __unlock();
            (void)os_sem_post(&s_play.data_sem);
            return -1;
        }
        memset(&req, 0, sizeof(req));
        req.dec.cmd = AUDIO_DEC_START;
        ret = server_request(s_play.server, AUDIO_REQ_DEC, &req);
        if (ret != 0) {
            printf("[JIELI_AUDIO] decoder START request failed: %d\n", ret);
            __lock();
            s_play.running = 0;
            __unlock();
            (void)os_sem_post(&s_play.data_sem);
            return -1;
        }
    }

    /* Re-apply the stored volume: the decoder only accepts SET_VOLUME while
     * running, and ao_set_volume() defers to here when it is called first
     * (which is what tdd_audio does on every play cycle). */
    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_SET_VOLUME;
    req.dec.volume = (u8)s_play.volume;
    req.dec.digital_volume = (u8)s_play.volume;
    if (server_request(s_play.server, AUDIO_REQ_DEC, &req) != 0) {
        printf("[JIELI_AUDIO] decoder SET_VOLUME after start failed\n");
    }
    return 0;
}

int jieli_audio_native_ao_stop(void *stream)
{
    union audio_req req;
    if (stream != &s_play || !s_play.initialized) return -2;
    __lock();
    s_play.running = 0;
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    __unlock();
    (void)os_sem_post(&s_play.data_sem);
    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_STOP;
    if (server_request(s_play.server, AUDIO_REQ_DEC, &req) != 0) {
        /* The vendor ends and closes the decoder by itself when playback
         * reaches its end ("audio dec end" -> "pcm_decoder_close"), so a STOP
         * sent after that is rejected. The decoder is already in the state this
         * call exists to produce, so report success rather than failing the
         * caller's close - measured on AC791 as tdl_audio_close() returning -1
         * on every round of a repeated open/close loop while the cycle itself
         * completed correctly. */
        printf("[JIELI_AUDIO] decoder STOP rejected; already stopped\n");
    }
    return 0;
}

int jieli_audio_native_ao_uninit(void *stream)
{
    if (stream != &s_play) return -2;
    if (!s_play.initialized) return 0;
    if (s_play.running && jieli_audio_native_ao_stop(stream) != 0) return -1;
    /* The WL82 PCM sample and both audio_server.h headers expose STOP; neither
     * SDK declares AUDIO_DEC_CLOSE. Match the vendor sample's STOP + close. */
    server_close(s_play.server);
    s_play.server = NULL;
    __lock();
    s_play.initialized = 0;
    s_play.running = 0;
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    __unlock();
    return 0;
}

int jieli_audio_native_ao_set_volume(void *stream, int volume)
{
    union audio_req req;
    int running;
    if (stream != &s_play || volume < 0 || volume > 100) return -2;
    if (!s_play.initialized) return -4;

    /* Record first, then apply only if the decoder is running.
     *
     * The vendor decoder rejects AUDIO_DEC_SET_VOLUME while stopped, and
     * tdd_audio sets the volume *before* starting the output on every play
     * cycle (__tdd_audio_start_output). Failing there made that helper return
     * early and skip tkl_ao_start entirely, so every utterance after the first
     * played nothing at all while the log filled with "tkl_ao_set_vol failed".
     * The stored value is applied by ao_start() once the decoder is running. */
    __lock();
    s_play.volume = volume;
    running = s_play.running;
    __unlock();
    if (!running) {
        return 0;
    }

    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_SET_VOLUME;
    req.dec.volume = (u8)volume;
    /* Keep both fields in step, as the vendor's players do. */
    req.dec.digital_volume = (u8)volume;
    if (server_request(s_play.server, AUDIO_REQ_DEC, &req) != 0) return -1;
    return 0;
}

int jieli_audio_native_ao_get_volume(void *stream, int *volume)
{
    if (stream != &s_play || volume == NULL) return -2;
    if (!s_play.initialized) return -4;
    *volume = s_play.volume;
    return 0;
}

int jieli_audio_native_ao_write(void *stream, const uint8_t *data, size_t size)
{
    u32 first;
    if (stream != &s_play || data == NULL || size == 0 || size > JIELI_AUDIO_PLAY_QUEUE_SIZE || (size & 1u))
        return -2;
    __lock();
    if (!s_play.running) {
        __unlock();
        return -4;
    }
    if (size > JIELI_AUDIO_PLAY_QUEUE_SIZE - s_play.used) {
        __unlock();
        return -31;
    }
    first = JIELI_AUDIO_PLAY_QUEUE_SIZE - s_play.write_pos;
    if (first > size) first = (u32)size;
    memcpy(s_play.queue + s_play.write_pos, data, first);
    if (size > first) memcpy(s_play.queue, data + first, size - first);
    s_play.write_pos = (s_play.write_pos + (u32)size) % JIELI_AUDIO_PLAY_QUEUE_SIZE;
    s_play.used += (u32)size;
    __unlock();
    (void)os_sem_post(&s_play.data_sem);
    return 0;
}

int jieli_audio_native_ao_flush(void *stream)
{
    uint32_t waited = 0;

    if (stream != &s_play) return -2;

    /* Let the decoder consume what is already queued before clearing it.
     *
     * The vendor calls our fread from the DAC path at realtime rate, so the
     * queue holds up to ~0.5 s of PCM that has been accepted but not yet
     * played. Zeroing it immediately truncated the tail of every utterance -
     * measured on AC791 as speech that stops mid-word. wl83 can wait on
     * audio_dac_idle(); wl82 exposes no such query, so wait on our own queue,
     * which the fread callback drains. */
    for (;;) {
        uint32_t used;
        int running;
        __lock();
        used = s_play.used;
        running = s_play.running;
        __unlock();
        if (used == 0 || !running || waited >= JIELI_AUDIO_PLAY_FLUSH_LIMIT) break;
        os_time_dly(1);
        ++waited;
    }

    __lock();
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    __unlock();
    return 0;
}
#endif
