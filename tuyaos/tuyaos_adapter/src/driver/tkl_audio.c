#include "tkl_audio.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "system/generic/printf.h"
#include "system/os/os_api.h"
#include "driver/tkl_jieli_audio_backend.h"
#include "tkl_vad_internal.h"

#define JIELI_AUDIO_MAX_PLAY_BYTES (8u * 1024u)
#define JIELI_AUDIO_CALLBACK_DRAIN_TIMEOUT_TICKS 100u

typedef struct {
    TKL_JIELI_AUDIO_BACKEND_OPS_T ops;
    void *backend;
    uint8_t installed;
} AUDIO_BACKEND_T;

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

static AUDIO_BACKEND_T s_backend;
static AI_STATE_T s_ai;
static AO_STATE_T s_ao;

static int __ops_valid(const TKL_JIELI_AUDIO_BACKEND_OPS_T *ops)
{
    return ops && ops->ai_init && ops->ai_start && ops->ai_stop && ops->ai_uninit && ops->ai_set_volume &&
           ops->ai_get_volume && ops->ao_init && ops->ao_start && ops->ao_stop && ops->ao_uninit &&
           ops->ao_set_volume && ops->ao_get_volume && ops->ao_write && ops->ao_flush && ops->critical_enter &&
           ops->critical_exit;
}

OPERATE_RET tkl_jieli_audio_backend_install(const TKL_JIELI_AUDIO_BACKEND_OPS_T *ops, void *backend)
{
    if (s_ai.initialized || s_ao.initialized) {
        printf("[TKL_AUDIO] backend install rejected: ai_initialized=%u ao_initialized=%u\n",
               (unsigned)s_ai.initialized, (unsigned)s_ao.initialized);
        return OPRT_RESOURCE_NOT_READY;
    }
    if (ops == NULL) {
        memset(&s_backend, 0, sizeof(s_backend));
        printf("[TKL_AUDIO] backend uninstalled\n");
        return OPRT_OK;
    }
    if (!__ops_valid(ops)) {
        printf("[TKL_AUDIO] backend install rejected: operation table incomplete\n");
        return OPRT_INVALID_PARM;
    }
    s_backend.ops = *ops;
    s_backend.backend = backend;
    s_backend.installed = 1;
    printf("[TKL_AUDIO] backend installed: context=%p\n", backend);
    return OPRT_OK;
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

static TKL_JIELI_AUDIO_PCM_CONFIG_T __pcm_config(const TKL_AUDIO_CONFIG_T *config)
{
    TKL_JIELI_AUDIO_PCM_CONFIG_T pcm;
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
    TKL_JIELI_AUDIO_CRITICAL_STATE_T key;

    if (state == NULL || data == NULL || size == 0 || size > UINT32_MAX || (size & 1u)) {
        return;
    }
    key = s_backend.ops.critical_enter(s_backend.backend);
    if (!state->initialized || !state->started || !state->callbacks_enabled || state->config.put_cb == NULL) {
        s_backend.ops.critical_exit(s_backend.backend, key);
        return;
    }
    ++state->callbacks_inflight;
    put_cb = state->config.put_cb;
    s_backend.ops.critical_exit(s_backend.backend, key);

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
    key = s_backend.ops.critical_enter(s_backend.backend);
    --state->callbacks_inflight;
    s_backend.ops.critical_exit(s_backend.backend, key);
}

OPERATE_RET tkl_ai_init(TKL_AUDIO_CONFIG_T *pconfig, int32_t count)
{
    OPERATE_RET ret;
    TKL_JIELI_AUDIO_PCM_CONFIG_T pcm;

    printf("[TKL_AUDIO] ai_init enter: installed=%u initialized=%u count=%d config=%p\n",
           (unsigned)s_backend.installed, (unsigned)s_ai.initialized, count, (void *)pconfig);
    if (s_ai.initialized) {
        printf("[TKL_AUDIO] ai_init reuse existing capture\n");
        return OPRT_OK;
    }
    if (!s_backend.installed) {
        printf("[TKL_AUDIO] ai_init rejected: backend not installed\n");
        return OPRT_NOT_SUPPORTED;
    }
    /* TDD currently passes count=0 while supplying a single config; preserve
     * that legacy convention and reject multi-stream configurations. */
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
    ret = s_backend.ops.ai_init(s_backend.backend, &pcm, __capture, &s_ai);
    if (ret != OPRT_OK) {
        printf("[TKL_AUDIO] ai native init failed: %d\n", ret);
        return ret;
    }
    {
        TKL_JIELI_AUDIO_CRITICAL_STATE_T key = s_backend.ops.critical_enter(s_backend.backend);
        s_ai.config = pconfig[0];
        s_ai.initialized = 1;
        s_ai.started = 0;
        s_ai.callbacks_enabled = 0;
        s_ai.callbacks_inflight = 0;
        s_backend.ops.critical_exit(s_backend.backend, key);
    }
    printf("[TKL_AUDIO] ai_init success\n");
    return OPRT_OK;
}

OPERATE_RET tkl_ai_start(int32_t card, TKL_AI_CHN_E chn)
{
    OPERATE_RET ret;
    TKL_JIELI_AUDIO_CRITICAL_STATE_T key;
    printf("[TKL_AUDIO] ai_start enter: card=%d channel=%d initialized=%u\n",
           card, (int)chn, (unsigned)s_ai.initialized);
    if (card != 0 || chn != TKL_AI_0) return OPRT_INVALID_PARM;
    key = s_backend.ops.critical_enter(s_backend.backend);
    if (!s_ai.initialized) { s_backend.ops.critical_exit(s_backend.backend, key); return OPRT_RESOURCE_NOT_READY; }
    if (s_ai.started && s_ai.callbacks_enabled) { s_backend.ops.critical_exit(s_backend.backend, key); return OPRT_OK; }
    s_ai.callbacks_enabled = 1;
    s_backend.ops.critical_exit(s_backend.backend, key);
    ret = s_backend.ops.ai_start(s_backend.backend);
    printf("[TKL_AUDIO] ai_start native result=%d\n", ret);
    if (ret == OPRT_OK) {
        TKL_JIELI_AUDIO_CRITICAL_STATE_T key = s_backend.ops.critical_enter(s_backend.backend);
        s_ai.started = 1;
        s_backend.ops.critical_exit(s_backend.backend, key);
    } else {
        TKL_JIELI_AUDIO_CRITICAL_STATE_T key = s_backend.ops.critical_enter(s_backend.backend);
        s_ai.callbacks_enabled = 0;
        s_ai.started = 0;
        s_backend.ops.critical_exit(s_backend.backend, key);
        (void)s_backend.ops.ai_stop(s_backend.backend);
    }
    return ret;
}

OPERATE_RET tkl_ai_set_vol(int32_t card, TKL_AI_CHN_E chn, int32_t vol)
{
    if (card != 0 || chn != TKL_AI_0 || vol < 0 || vol > 100) return OPRT_INVALID_PARM;
    if (!s_ai.initialized) return OPRT_RESOURCE_NOT_READY;
    return s_backend.ops.ai_set_volume(s_backend.backend, vol);
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
    TKL_JIELI_AUDIO_CRITICAL_STATE_T key;
    if (card != 0 || chn != TKL_AI_0) return OPRT_INVALID_PARM;
    key = s_backend.ops.critical_enter(s_backend.backend);
    if (!s_ai.initialized || !s_ai.started) { s_backend.ops.critical_exit(s_backend.backend, key); return OPRT_OK; }
    s_ai.callbacks_enabled = 0;
    s_backend.ops.critical_exit(s_backend.backend, key);
    ret = s_backend.ops.ai_stop(s_backend.backend);
    do {
        key = s_backend.ops.critical_enter(s_backend.backend);
        callbacks_inflight = s_ai.callbacks_inflight;
        s_backend.ops.critical_exit(s_backend.backend, key);
        if (callbacks_inflight == 0) break;
        os_time_dly(1);
    } while (waited++ < JIELI_AUDIO_CALLBACK_DRAIN_TIMEOUT_TICKS);

    /* Some providers report in-flight callbacks while closing the native
     * stream. Once TKL callbacks have drained, retry their idempotent stop so
     * the provider can finish its own teardown before the caller frees state. */
    if (ret != OPRT_OK && callbacks_inflight == 0) {
        ret = s_backend.ops.ai_stop(s_backend.backend);
    }

    key = s_backend.ops.critical_enter(s_backend.backend);
    callbacks_inflight = s_ai.callbacks_inflight;
    if (ret == OPRT_OK && callbacks_inflight == 0) s_ai.started = 0;
    s_backend.ops.critical_exit(s_backend.backend, key);
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
    ret = s_backend.ops.ai_uninit(s_backend.backend);
    if (ret == OPRT_OK) {
        TKL_JIELI_AUDIO_CRITICAL_STATE_T key = s_backend.ops.critical_enter(s_backend.backend);
        memset(&s_ai, 0, sizeof(s_ai));
        s_backend.ops.critical_exit(s_backend.backend, key);
    }
    return ret;
}

OPERATE_RET tkl_ao_init(TKL_AUDIO_CONFIG_T *pconfig, int32_t count, void **handle)
{
    OPERATE_RET ret;
    TKL_JIELI_AUDIO_PCM_CONFIG_T pcm;
    void *stream = NULL;
    printf("[TKL_AUDIO] ao_init enter: installed=%u initialized=%u count=%d config=%p handle=%p\n",
           (unsigned)s_backend.installed, (unsigned)s_ao.initialized, count,
           (void *)pconfig, (void *)handle);
    if (handle == NULL || pconfig == NULL || count != 1) {
        printf("[TKL_AUDIO] ao_init rejected: config=%p handle=%p count=%d\n",
               (void *)pconfig, (void *)handle, count);
        return OPRT_INVALID_PARM;
    }
    if (s_ao.initialized) { *handle = &s_ao; return OPRT_OK; }
    if (!s_backend.installed) return OPRT_NOT_SUPPORTED;
    ret = __validate_config(pconfig, 0);
    if (ret != OPRT_OK) {
        printf("[TKL_AUDIO] ao config validation failed: %d\n", ret);
        return ret;
    }
    pcm = __pcm_config(pconfig);
    printf("[TKL_AUDIO] ao dispatch native init: rate=%u bits=%u channels=%u\n",
           (unsigned)pcm.sample_rate, (unsigned)pcm.bits_per_sample, (unsigned)pcm.channels);
    ret = s_backend.ops.ao_init(s_backend.backend, &pcm, &stream);
    if (ret != OPRT_OK) {
        printf("[TKL_AUDIO] ao native init failed: %d\n", ret);
        return ret;
    }
    if (stream == NULL) {
        s_backend.ops.ao_uninit(s_backend.backend, stream);
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
    ret = s_backend.ops.ao_start(s_backend.backend, s_ao.stream);
    printf("[TKL_AUDIO] ao_start native result=%d\n", ret);
    if (ret == OPRT_OK) s_ao.started = 1;
    return ret;
}

OPERATE_RET tkl_ao_set_vol(int32_t card, TKL_AO_CHN_E chn, void *handle, int32_t vol)
{
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle) || vol < 0 || vol > 100)
        return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_RESOURCE_NOT_READY;
    return s_backend.ops.ao_set_volume(s_backend.backend, s_ao.stream, vol);
}

OPERATE_RET tkl_ao_get_vol(int32_t card, TKL_AO_CHN_E chn, void *handle, int32_t *vol)
{
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle) || vol == NULL) return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_RESOURCE_NOT_READY;
    return s_backend.ops.ao_get_volume(s_backend.backend, s_ao.stream, vol);
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
        printf("[TKL_AUDIO] ao_put rejected frame: frame=%p data=%p bytes=%u max=%u sample=%u bits=%u channel=%u codec=%u\n",
               (void *)frame, frame ? (void *)frame->pbuf : NULL,
               frame ? (unsigned)frame->used_size : 0u, (unsigned)JIELI_AUDIO_MAX_PLAY_BYTES,
               frame ? (unsigned)frame->sample : 0u, frame ? (unsigned)frame->datebits : 0u,
               frame ? (unsigned)frame->channel : 0u, frame ? (unsigned)frame->codectype : 0u);
        return OPRT_INVALID_PARM;
    }
    if (!s_ao.initialized || !s_ao.started) {
        printf("[TKL_AUDIO] ao_put rejected state: initialized=%u started=%u stream=%p\n",
               (unsigned)s_ao.initialized, (unsigned)s_ao.started, s_ao.stream);
        return OPRT_RESOURCE_NOT_READY;
    }
    return s_backend.ops.ao_write(s_backend.backend, s_ao.stream, (const uint8_t *)frame->pbuf, frame->used_size);
}

OPERATE_RET tkl_ao_stop(int32_t card, TKL_AO_CHN_E chn, void *handle)
{
    OPERATE_RET flush_ret;
    OPERATE_RET stop_ret;
    if (card != 0 || chn != TKL_AO_0 || !__ao_handle_valid(handle)) return OPRT_INVALID_PARM;
    if (!s_ao.initialized) return OPRT_OK;

    /* Drain accepted audio before stopping the hardware and closing the DAC. */
    flush_ret = s_backend.ops.ao_flush(s_backend.backend, s_ao.stream);
    if (flush_ret != OPRT_OK) {
        /* Keep the native writer alive so a later flush can finish draining. */
        s_ao.started = 0;
        return flush_ret;
    }

    stop_ret = s_backend.ops.ao_stop(s_backend.backend, s_ao.stream);
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
    ret = s_backend.ops.ao_uninit(s_backend.backend, s_ao.stream);
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
