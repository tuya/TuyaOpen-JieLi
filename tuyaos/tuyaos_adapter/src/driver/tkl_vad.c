#include "tkl_vad.h"

#include <stdint.h>
#include <string.h>

#include "tkl_mutex.h"

/* Feeds every complete configured VAD frame contained in a captured PCM
 * buffer. No-op while VAD is stopped/uninitialized. The capture path in
 * tkl_audio.c is the only caller; it declares this rather than sharing a
 * private header. */
void tkl_jieli_vad_feed_capture(const uint8_t *data, size_t size);

#define JIELI_VAD_BASE_RMS       500.0f
#define JIELI_VAD_MAX_FRAME_MS   60
/* tkl_vad_init pins sample_rate to 16000 and channels to 1, so the largest
 * accepted frame has a fixed size. */
#define JIELI_VAD_MAX_FRAME_BYTES ((16000u * JIELI_VAD_MAX_FRAME_MS / 1000u) * 2u)

typedef struct {
    TKL_VAD_CONFIG_T config;
    uint32_t speech_frames;
    uint32_t noise_frames;
    TKL_VAD_STATUS_T status;
    uint8_t initialized;
    uint8_t started;
} VAD_STATE_T;

static VAD_STATE_T s_vad;
static TKL_MUTEX_HANDLE s_vad_mutex;

/* Carry-over for tkl_jieli_vad_feed_capture: the WL83 capture callback delivers
 * a fixed 20 ms (640 bytes) per call, so a frame longer than that spans calls
 * and its head must survive between them. Guarded by s_vad_mutex. */
static uint8_t s_vad_pcm[JIELI_VAD_MAX_FRAME_BYTES];
static size_t s_vad_pcm_len;

/* tkl_vad_init/deinit are lifecycle calls and must not race with each other.
 * Runtime feed/start/stop/status operations share this recursive TKL mutex. */
static OPERATE_RET __vad_lock(void)
{
    return s_vad_mutex == NULL ? OPRT_RESOURCE_NOT_READY : tkl_mutex_lock(s_vad_mutex);
}

static void __vad_unlock(void)
{
    (void)tkl_mutex_unlock(s_vad_mutex);
}

OPERATE_RET tkl_vad_init(TKL_VAD_CONFIG_T *config)
{
    if (config == NULL || config->sample_rate != 16000 || config->channel_num != 1 ||
        config->frame_duration_ms < 10 || config->frame_duration_ms > JIELI_VAD_MAX_FRAME_MS ||
        config->frame_duration_ms % 10 != 0 || config->speech_min_ms < 0 || config->noise_min_ms < 0 ||
        config->scale != config->scale || config->scale < 0.05f || config->scale > 10.0f) {
        return OPRT_INVALID_PARM;
    }
    if (s_vad_mutex == NULL) {
        OPERATE_RET ret = tkl_mutex_create_init(&s_vad_mutex);
        if (ret != OPRT_OK) return ret;
    }
    if (tkl_mutex_lock(s_vad_mutex) != OPRT_OK) return OPRT_COM_ERROR;
    s_vad.config = *config;
    s_vad.speech_frames = 0;
    s_vad.noise_frames = 0;
    s_vad.status = TKL_VAD_STATUS_NONE;
    s_vad.initialized = 1;
    s_vad.started = 0;
    s_vad_pcm_len = 0;
    __vad_unlock();
    return OPRT_OK;
}

static uint32_t __required_frame_bytes(void)
{
    return (s_vad.config.sample_rate * (uint32_t)s_vad.config.frame_duration_ms / 1000u) * 2u;
}

OPERATE_RET tkl_vad_feed(uint8_t *data, uint32_t len)
{
    uint64_t squares = 0;
    uint32_t samples, i;
    float threshold;
    if (data == NULL) return OPRT_INVALID_PARM;
    if (__vad_lock() != OPRT_OK) return OPRT_RESOURCE_NOT_READY;
    if (!s_vad.initialized || !s_vad.started) { __vad_unlock(); return OPRT_RESOURCE_NOT_READY; }
    if (len != __required_frame_bytes()) { __vad_unlock(); return OPRT_INVALID_PARM; }
    samples = len / 2u;
    for (i = 0; i < samples; ++i) {
        int16_t sample;
        memcpy(&sample, data + i * 2u, sizeof(sample));
        squares += (uint64_t)((int32_t)sample * (int32_t)sample);
    }
    /* Avoid sqrt/libm in the embedded adapter: compare mean-square against the
     * squared threshold. scale increases sensitivity (lower required RMS). */
    threshold = JIELI_VAD_BASE_RMS / s_vad.config.scale;
    if (squares >= (uint64_t)(threshold * threshold * samples)) {
        s_vad.noise_frames = 0;
        if (s_vad.speech_frames < UINT32_MAX) ++s_vad.speech_frames;
        if (s_vad.speech_frames * (uint32_t)s_vad.config.frame_duration_ms >=
            (uint32_t)s_vad.config.speech_min_ms) s_vad.status = TKL_VAD_STATUS_SPEECH;
    } else if (squares < (uint64_t)(threshold * threshold * 0.49f * samples)) {
        s_vad.speech_frames = 0;
        if (s_vad.noise_frames < UINT32_MAX) ++s_vad.noise_frames;
        if (s_vad.noise_frames * (uint32_t)s_vad.config.frame_duration_ms >=
            (uint32_t)s_vad.config.noise_min_ms) s_vad.status = TKL_VAD_STATUS_NONE;
    } else {
        s_vad.speech_frames = 0;
        s_vad.noise_frames = 0;
    }
    __vad_unlock();
    return OPRT_OK;
}

TKL_VAD_STATUS_T tkl_vad_get_status(void)
{
    TKL_VAD_STATUS_T status;
    if (__vad_lock() != OPRT_OK) return TKL_VAD_STATUS_NONE;
    status = s_vad.initialized && s_vad.started ? s_vad.status : TKL_VAD_STATUS_NONE;
    __vad_unlock();
    return status;
}

OPERATE_RET tkl_vad_start(void)
{
    if (__vad_lock() != OPRT_OK) return OPRT_RESOURCE_NOT_READY;
    if (!s_vad.initialized) { __vad_unlock(); return OPRT_RESOURCE_NOT_READY; }
    s_vad.started = 1;
    s_vad.speech_frames = 0;
    s_vad.noise_frames = 0;
    s_vad.status = TKL_VAD_STATUS_NONE;
    s_vad_pcm_len = 0;
    __vad_unlock();
    return OPRT_OK;
}

OPERATE_RET tkl_vad_stop(void)
{
    if (__vad_lock() != OPRT_OK) return OPRT_OK;
    s_vad.started = 0;
    s_vad.speech_frames = 0;
    s_vad.noise_frames = 0;
    s_vad.status = TKL_VAD_STATUS_NONE;
    s_vad_pcm_len = 0;
    __vad_unlock();
    return OPRT_OK;
}

OPERATE_RET tkl_vad_deinit(void)
{
    if (s_vad_mutex != NULL) {
        (void)tkl_mutex_lock(s_vad_mutex);
        memset(&s_vad, 0, sizeof(s_vad));
        s_vad_pcm_len = 0;
        (void)tkl_mutex_unlock(s_vad_mutex);
        (void)tkl_mutex_release(s_vad_mutex);
        s_vad_mutex = NULL;
    }
    return OPRT_OK;
}

void tkl_jieli_vad_feed_capture(const uint8_t *data, size_t size)
{
    size_t frame_bytes;
    size_t offset = 0;
    if (data == NULL || __vad_lock() != OPRT_OK) return;
    if (!s_vad.initialized || !s_vad.started) { __vad_unlock(); return; }
    frame_bytes = __required_frame_bytes();
    if (frame_bytes == 0 || frame_bytes > sizeof(s_vad_pcm)) { __vad_unlock(); return; }

    /* The capture path delivers a fixed 20 ms per callback, so a frame longer
     * than that spans callbacks. Slicing only the whole frames contained in one
     * callback starved the detector at 30-60 ms: frame_bytes exceeded size, the
     * loop body never ran, and the status stayed NONE with no error anywhere.
     * Accumulate instead, and keep the tail for the next call. */
    while (offset < size) {
        size_t want = frame_bytes - s_vad_pcm_len;
        size_t take = (size - offset < want) ? (size - offset) : want;
        memcpy(s_vad_pcm + s_vad_pcm_len, data + offset, take);
        s_vad_pcm_len += take;
        offset += take;
        if (s_vad_pcm_len == frame_bytes) {
            s_vad_pcm_len = 0;
            /* s_vad_mutex is recursive, so feeding under it is safe; clearing
             * the length first keeps the accumulator consistent if feed ever
             * re-enters. */
            (void)tkl_vad_feed(s_vad_pcm, (uint32_t)frame_bytes);
        }
    }
    __vad_unlock();
}
