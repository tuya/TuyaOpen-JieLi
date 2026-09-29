#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/tkl_jieli_audio_backend.h"
#include "tkl_audio.h"
#include "tkl_mutex.h"
#include "tkl_vad.h"

static uint8_t s_seen_copy;
static const uint8_t *s_backend_payload;
static TKL_JIELI_AUDIO_CAPTURE_CB s_capture;
static void *s_capture_cookie;
static int s_callback_count;
static int s_fail_ai_start;
static int s_backend_context;
static uint8_t s_play_queue[8];
static size_t s_play_queue_size;
static char s_ao_stop_events[4];
static size_t s_ao_stop_event_count;
static int s_mutex_token;

OPERATE_RET tkl_mutex_create_init(TKL_MUTEX_HANDLE *handle)
{ if (handle == NULL) return OPRT_INVALID_PARM; *handle = &s_mutex_token; return OPRT_OK; }
OPERATE_RET tkl_mutex_lock(const TKL_MUTEX_HANDLE handle)
{ return handle == NULL ? OPRT_INVALID_PARM : OPRT_OK; }
OPERATE_RET tkl_mutex_unlock(const TKL_MUTEX_HANDLE handle)
{ return handle == NULL ? OPRT_INVALID_PARM : OPRT_OK; }
OPERATE_RET tkl_mutex_release(const TKL_MUTEX_HANDLE handle)
{ return handle == NULL ? OPRT_INVALID_PARM : OPRT_OK; }

static OPERATE_RET ai_init(void *backend, const TKL_JIELI_AUDIO_PCM_CONFIG_T *config,
                           TKL_JIELI_AUDIO_CAPTURE_CB callback, void *cookie)
{
    assert(backend != NULL && config->sample_rate == 16000 && config->bits_per_sample == 16 && config->channels == 1);
    s_capture = callback;
    s_capture_cookie = cookie;
    return OPRT_OK;
}
static OPERATE_RET ai_start(void *backend) { assert(backend != NULL); if (s_fail_ai_start) { s_fail_ai_start = 0; return OPRT_COM_ERROR; } return OPRT_OK; }
static OPERATE_RET ai_stop(void *backend)
{
    static const uint8_t late_frame[4] = {0x77, 0, 0, 0};
    assert(backend != NULL);
    /* Simulate an in-flight queued callback while stop is quiescing it. The
     * adapter must gate it before entering the backend's stop routine. */
    s_capture(s_capture_cookie, late_frame, sizeof(late_frame), 44);
    return OPRT_OK;
}
static OPERATE_RET ai_uninit(void *backend) { assert(backend != NULL); return OPRT_OK; }
static OPERATE_RET ai_set_volume(void *backend, int32_t volume) { assert(backend != NULL && volume >= 0 && volume <= 100); return OPRT_OK; }
static OPERATE_RET ai_get_volume(void *backend, int32_t *volume) { assert(backend != NULL && volume != NULL); *volume = 50; return OPRT_OK; }
static OPERATE_RET ao_init(void *backend, const TKL_JIELI_AUDIO_PCM_CONFIG_T *config, void **stream)
{ assert(backend != NULL && config->sample_rate == 16000); *stream = backend; return OPRT_OK; }
static OPERATE_RET ao_start(void *backend, void *stream) { assert(backend == stream); return OPRT_OK; }
static OPERATE_RET ao_stop(void *backend, void *stream)
{
    assert(backend == stream);
    s_ao_stop_events[s_ao_stop_event_count++] = 'S';
    return OPRT_OK;
}
static OPERATE_RET ao_uninit(void *backend, void *stream) { assert(backend == stream); return OPRT_OK; }
static OPERATE_RET ao_set_volume(void *backend, void *stream, int32_t volume)
{ assert(backend == stream && volume >= 0 && volume <= 100); return OPRT_OK; }
static OPERATE_RET ao_get_volume(void *backend, void *stream, int32_t *volume)
{ assert(backend == stream && volume != NULL); *volume = 70; return OPRT_OK; }
static OPERATE_RET ao_write(void *backend, void *stream, const uint8_t *data, size_t size)
{
    assert(backend == stream && data != NULL && size > 0);
    if (size > sizeof(s_play_queue) - s_play_queue_size) return OPRT_BUFFER_NOT_ENOUGH;
    memcpy(s_play_queue + s_play_queue_size, data, size);
    s_play_queue_size += size;
    return OPRT_OK;
}
static OPERATE_RET ao_flush(void *backend, void *stream)
{
    assert(backend == stream);
    s_ao_stop_events[s_ao_stop_event_count++] = 'F';
    s_play_queue_size = 0;
    memset(s_play_queue, 0, sizeof(s_play_queue));
    return OPRT_OK;
}
static TKL_JIELI_AUDIO_CRITICAL_STATE_T critical_enter(void *backend)
{ assert(backend != NULL); return 0; }
static void critical_exit(void *backend, TKL_JIELI_AUDIO_CRITICAL_STATE_T state)
{ assert(backend != NULL); (void)state; }

static const TKL_JIELI_AUDIO_BACKEND_OPS_T s_ops = {
    ai_init, ai_start, ai_stop, ai_uninit, ai_set_volume, ai_get_volume,
    ao_init, ao_start, ao_stop, ao_uninit, ao_set_volume, ao_get_volume, ao_write, ao_flush,
    critical_enter, critical_exit
};

static int capture_cb(TKL_AUDIO_FRAME_INFO_T *frame)
{
    assert(frame != NULL);
    assert(frame->used_size > 0);
    assert(frame->pbuf != (char *)s_backend_payload);
    s_seen_copy = (uint8_t)frame->pbuf[0];
    ++s_callback_count;
    return 0;
}

static void test_audio_lifecycle_and_copy(void)
{
    TKL_AUDIO_CONFIG_T config;
    TKL_AUDIO_FRAME_INFO_T frame;
    uint8_t bytes[4] = {0x5a, 0, 0xa5, 0};
    uint8_t pcm[640];
    void *ao_handle = NULL;
    memset(&config, 0, sizeof(config));
    config.sample = TKL_AUDIO_SAMPLE_16K;
    config.datebits = TKL_AUDIO_DATABITS_16;
    config.channel = TKL_AUDIO_CHANNEL_MONO;
    config.codectype = TKL_CODEC_AUDIO_PCM;
    config.put_cb = capture_cb;
    assert(tkl_ai_init(&config, 0) == OPRT_NOT_SUPPORTED);
    assert(tkl_jieli_audio_backend_install(&s_ops, &s_backend_context) == OPRT_OK);
    assert(tkl_ai_init(&config, 0) == OPRT_INVALID_PARM);
    assert(tkl_ai_init(&config, 1) == OPRT_OK);
    s_fail_ai_start = 1;
    assert(tkl_ai_start(0, TKL_AI_0) == OPRT_COM_ERROR);
    assert(tkl_ai_start(0, TKL_AI_0) == OPRT_OK);
    s_backend_payload = bytes;
    s_capture(s_capture_cookie, bytes, sizeof(bytes), 42);
    assert(s_callback_count == 1 && s_seen_copy == bytes[0]);
    /* Inactive VAD must not suppress the application audio callback. */
    assert(s_callback_count == 1);
    {
        TKL_VAD_CONFIG_T cfg = {16000, 1, 20, 20, 10, 1.0f};
        uint32_t i;
        assert(tkl_vad_init(&cfg) == OPRT_OK);
        assert(tkl_vad_start() == OPRT_OK);
        for (i = 0; i < 320; i += 2) {
            int16_t sample = 4000;
            memcpy(pcm + i, &sample, sizeof(sample));
            memcpy(pcm + 320 + i, &sample, sizeof(sample));
        }
        s_backend_payload = pcm;
        s_capture(s_capture_cookie, pcm, sizeof(pcm), 43);
        assert(s_callback_count == 2);
        assert(tkl_vad_get_status() == TKL_VAD_STATUS_SPEECH);
        assert(tkl_vad_stop() == OPRT_OK);
        assert(tkl_vad_deinit() == OPRT_OK);
    }
#if SIZE_MAX > UINT32_MAX
    {
        int before = s_callback_count;
        s_capture(s_capture_cookie, bytes, (size_t)UINT32_MAX + 1u, 45);
        assert(s_callback_count == before);
    }
#endif
    assert(tkl_ai_set_vqe(0, TKL_AI_0, TKL_AUDIO_VQE_AEC, NULL) == OPRT_INVALID_PARM);
    {
        TKL_AUDIO_VQE_PARAM_T param = {1};
        assert(tkl_ai_set_vqe(0, TKL_AI_0, TKL_AUDIO_VQE_AEC, &param) == OPRT_NOT_SUPPORTED);
    }
    assert(tkl_ai_stop(0, TKL_AI_0) == OPRT_OK);
    s_capture(s_capture_cookie, bytes, sizeof(bytes), 43);
    assert(s_callback_count == 2);
    assert(tkl_ai_uninit() == OPRT_OK);
    config.put_cb = NULL;
    assert(tkl_ao_init(&config, 1, &ao_handle) == OPRT_OK);
    assert(tkl_ao_start(0, TKL_AO_0, ao_handle) == OPRT_OK);
    memset(&frame, 0, sizeof(frame));
    frame.pbuf = (char *)bytes; frame.used_size = sizeof(bytes);
    frame.sample = TKL_AUDIO_SAMPLE_16K; frame.datebits = TKL_AUDIO_DATABITS_16;
    frame.channel = TKL_AUDIO_CHANNEL_MONO; frame.codectype = TKL_CODEC_AUDIO_PCM;
    s_play_queue_size = 0;
    assert(tkl_ao_put_frame(0, TKL_AO_0, ao_handle, &frame) == OPRT_OK);
    assert(s_play_queue_size == sizeof(bytes));
    bytes[0] = 0x11;
    assert(s_play_queue[0] == 0x5a); /* provider copied the caller's buffer */
    assert(tkl_ao_put_frame(0, TKL_AO_0, ao_handle, &frame) == OPRT_OK);
    assert(tkl_ao_put_frame(0, TKL_AO_0, ao_handle, &frame) == OPRT_BUFFER_NOT_ENOUGH);
    assert(s_play_queue_size == sizeof(s_play_queue));
    s_ao_stop_event_count = 0;
    assert(tkl_ao_stop(0, TKL_AO_0, ao_handle) == OPRT_OK);
    assert(s_ao_stop_event_count == 2);
    assert(s_ao_stop_events[0] == 'F' && s_ao_stop_events[1] == 'S');
    assert(s_play_queue_size == 0);
    assert(tkl_ao_uninit(ao_handle) == OPRT_OK);
    assert(tkl_jieli_audio_backend_install(NULL, NULL) == OPRT_OK);
}

static void test_vad_unaligned_and_hysteresis(void)
{
    TKL_VAD_CONFIG_T cfg = {16000, 1, 20, 20, 10, 1.0f};
    uint8_t frame[321] = {0};
    uint32_t i;

    assert(tkl_vad_init(&cfg) == OPRT_OK);
    assert(tkl_vad_start() == OPRT_OK);
    assert(tkl_vad_feed(frame + 1, 320) == OPRT_OK);
    assert(tkl_vad_get_status() == TKL_VAD_STATUS_NONE);
    for (i = 0; i < 160; ++i) {
        int16_t sample = 4000;
        memcpy(frame + 1 + i * 2, &sample, sizeof(sample));
    }
    assert(tkl_vad_feed(frame + 1, 320) == OPRT_OK);
    assert(tkl_vad_get_status() == TKL_VAD_STATUS_NONE);
    assert(tkl_vad_feed(frame + 1, 320) == OPRT_OK);
    assert(tkl_vad_get_status() == TKL_VAD_STATUS_SPEECH);
    assert(tkl_vad_feed(frame + 1, 319) == OPRT_INVALID_PARM);
    assert(tkl_vad_stop() == OPRT_OK);
    assert(tkl_vad_deinit() == OPRT_OK);
}

static void test_vad_rejects_nonfinite_scale(void)
{
    TKL_VAD_CONFIG_T cfg = {16000, 1, 10, 10, 10, NAN};
    assert(tkl_vad_init(&cfg) == OPRT_INVALID_PARM);
    cfg.scale = INFINITY;
    assert(tkl_vad_init(&cfg) == OPRT_INVALID_PARM);
    cfg.scale = -INFINITY;
    assert(tkl_vad_init(&cfg) == OPRT_INVALID_PARM);
}

int main(void)
{
    TKL_VAD_CONFIG_T invalid = {16000, 1, 10, 10, 10, 0.0f};
    assert(tkl_vad_init(&invalid) == OPRT_INVALID_PARM);
    test_audio_lifecycle_and_copy();
    test_vad_rejects_nonfinite_scale();
    test_vad_unaligned_and_hysteresis();
    puts("audio TKL tests passed");
    return 0;
}
