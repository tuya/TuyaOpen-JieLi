#include "tkl_vad.h"

#include <stdint.h>
#include <string.h>

#include "tkl_mutex.h"

#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
#include "tal_audio_engine_compat.h"
#include "tkl_semaphore.h"
#include "tkl_system.h"
#include "tkl_thread.h"
/* rnn_vad_process() consumes exactly 320 samples (640 bytes, 20 ms at
 * 16 kHz): the engine keeps a 640-sample window, shifts it by half on every
 * call, and rnn_vad_init() converts the millisecond parameters at 20 ms per
 * frame. Feeding the configured frame_duration_ms instead would desynchronise
 * every timing parameter. */
#define JIELI_RNN_VAD_FRAME_BYTES 640u
/* The engine runs on its own task: the capture callback executes on the
 * vendor audio_encoder task, whose 4 KiB stack the MFCC/FFT/RNN path
 * overflows (exception_analyze -> stackoverflow, cpu 1, current_task
 * audio_encoder). */
#define JIELI_RNN_VAD_RING_FRAMES 8u
#define JIELI_RNN_VAD_TASK_STACK  8192u
#define JIELI_RNN_VAD_TASK_PRIO   3u
#endif

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

#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
static void *s_rnn_vad;
static uint32_t s_rnn_vad_frames;

/* Complete 640-byte frames waiting for the VAD task, guarded by s_vad_mutex.
 * write_pos - read_pos is the used size; both are monotonic. */
static uint8_t s_vad_ring[JIELI_RNN_VAD_FRAME_BYTES * JIELI_RNN_VAD_RING_FRAMES];
static uint32_t s_vad_ring_read;
static uint32_t s_vad_ring_write;
static uint32_t s_vad_ring_dropped;
static TKL_SEM_HANDLE s_vad_sem;
static TKL_THREAD_HANDLE s_vad_thread;
static uint8_t s_vad_task_exit;
static volatile uint8_t s_vad_task_busy;
#endif

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

#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
static void __vad_task(void *arg);
#endif

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

#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
    if (s_rnn_vad == NULL) {
        RNN_VAD_PARAM_T param = {
            .min_speech_len      = (float)config->speech_min_ms,
            .max_speech_interval = (float)config->noise_min_ms,
        };

        s_rnn_vad = rnn_vad_create();
        if (s_rnn_vad == NULL || rnn_vad_init(&param, s_rnn_vad) != 0) {
            if (s_rnn_vad != NULL) {
                rnn_vad_destroy(s_rnn_vad);
                s_rnn_vad = NULL;
            }
            s_vad.initialized = 0;
            __vad_unlock();
            return OPRT_COM_ERROR;
        }
        /* The engine's threshold is a dB value; scale > 1 lowers it. */
        rnn_vad_set_callback(s_rnn_vad, -40.0f / (config->scale > 0.0f ? config->scale : 1.0f));
        s_rnn_vad_frames = 0;

        s_vad_ring_read = 0;
        s_vad_ring_write = 0;
        s_vad_ring_dropped = 0;
        s_vad_task_exit = 0;
        s_vad_task_busy = 0;
        if (s_vad_thread == NULL) {
            if (tkl_semaphore_create_init(&s_vad_sem, 0, 1) != OPRT_OK) {
                rnn_vad_destroy(s_rnn_vad);
                s_rnn_vad = NULL;
                s_vad.initialized = 0;
                __vad_unlock();
                return OPRT_COM_ERROR;
            }
            if (tkl_thread_create(&s_vad_thread, "tuya_vad", JIELI_RNN_VAD_TASK_STACK,
                                  JIELI_RNN_VAD_TASK_PRIO, __vad_task, NULL) != OPRT_OK) {
                s_vad_thread = NULL;
                (void)tkl_semaphore_release(s_vad_sem);
                s_vad_sem = NULL;
                rnn_vad_destroy(s_rnn_vad);
                s_rnn_vad = NULL;
                s_vad.initialized = 0;
                __vad_unlock();
                return OPRT_COM_ERROR;
            }
        }
        printf("[RNN_VAD] engine ready, speech_min=%d noise_min=%d\n", config->speech_min_ms, config->noise_min_ms);
    }
#endif

    __vad_unlock();
    return OPRT_OK;
}

static uint32_t __required_frame_bytes(void)
{
#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
    return JIELI_RNN_VAD_FRAME_BYTES;
#else
    return (s_vad.config.sample_rate * (uint32_t)s_vad.config.frame_duration_ms / 1000u) * 2u;
#endif
}

#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
/* Queues one complete frame for the engine task. Caller holds s_vad_mutex.
 * The queue is bounded so a stalled consumer costs frames, not the capture
 * task's stack. */
static void __vad_ring_push(const uint8_t *frame)
{
    uint32_t used = s_vad_ring_write - s_vad_ring_read;
    uint32_t pos;
    size_t first;

    if (used + JIELI_RNN_VAD_FRAME_BYTES > sizeof(s_vad_ring)) {
        ++s_vad_ring_dropped;
        return;
    }
    pos = s_vad_ring_write % (uint32_t)sizeof(s_vad_ring);
    first = sizeof(s_vad_ring) - pos;
    if (first > JIELI_RNN_VAD_FRAME_BYTES) first = JIELI_RNN_VAD_FRAME_BYTES;
    memcpy(s_vad_ring + pos, frame, first);
    if (JIELI_RNN_VAD_FRAME_BYTES > first) {
        memcpy(s_vad_ring, frame + first, JIELI_RNN_VAD_FRAME_BYTES - first);
    }
    s_vad_ring_write += JIELI_RNN_VAD_FRAME_BYTES;
    (void)tkl_semaphore_post(s_vad_sem);
}

/* Runs the RNN engine on a stack large enough for the MFCC/FFT/RNN path.
 * nnom_vad_run() applies the speech_min_len / max_speech_interval hysteresis
 * internally (1 when >10 of the last 80 frames are speech), so its 0/1
 * verdict maps straight onto the status. */
static void __vad_task(void *arg)
{
    uint8_t frame[JIELI_RNN_VAD_FRAME_BYTES];

    (void)arg;
    for (;;) {
        if (tkl_semaphore_wait(s_vad_sem, TKL_SEM_WAIT_FOREVER) != OPRT_OK) {
            continue;
        }
        if (__vad_lock() != OPRT_OK) {
            continue;
        }
        if (s_vad_task_exit) {
            __vad_unlock();
            break;
        }
        if (!s_vad.started ||
            (s_vad_ring_write - s_vad_ring_read) < JIELI_RNN_VAD_FRAME_BYTES) {
            __vad_unlock();
            continue;
        }
        {
            uint32_t pos = s_vad_ring_read % (uint32_t)sizeof(s_vad_ring);
            size_t first = sizeof(s_vad_ring) - pos;

            if (first > JIELI_RNN_VAD_FRAME_BYTES) first = JIELI_RNN_VAD_FRAME_BYTES;
            memcpy(frame, s_vad_ring + pos, first);
            if (JIELI_RNN_VAD_FRAME_BYTES > first) {
                memcpy(frame + first, s_vad_ring, JIELI_RNN_VAD_FRAME_BYTES - first);
            }
            s_vad_ring_read += JIELI_RNN_VAD_FRAME_BYTES;
        }
        /* Set the busy flag while still holding the lock: tkl_vad_stop()
         * takes the lock, clears started, releases it and only then waits on
         * this flag. Marking the engine call under the lock closes the window
         * where stop sees "idle" for a frame that is about to be processed. */
        s_vad_task_busy = 1;
        __vad_unlock();

        float v = rnn_vad_process(s_rnn_vad, (short *)frame);

        if (__vad_lock() != OPRT_OK) {
            s_vad_task_busy = 0;
            continue;
        }
        s_vad_task_busy = 0;
        s_vad.status = (v > 0.0f) ? TKL_VAD_STATUS_SPEECH : TKL_VAD_STATUS_NONE;
        if (s_rnn_vad_frames < 5u || (s_rnn_vad_frames % 500u) == 0u) {
            printf("[RNN_VAD] raw=%d.%03d\n", (int)v, (int)((v < 0 ? -v : v) * 1000.0f) % 1000);
        }
        ++s_rnn_vad_frames;
        __vad_unlock();
    }
}
#else
/* RMS detector: runs inline, it only needs a few registers. */
static void __vad_process_frame(uint8_t *data, uint32_t len)
{
    uint64_t squares = 0;
    uint32_t samples = len / 2u, i;
    float threshold;

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
}
#endif

/* Splits an arbitrary captured block into complete engine frames. The capture
 * path delivers a fixed 20 ms per callback, so a frame longer than that spans
 * callbacks. Slicing only the whole frames contained in one callback starved
 * the detector at 30-60 ms: frame_bytes exceeded size, the loop body never
 * ran, and the status stayed NONE with no error anywhere. Accumulate instead,
 * and keep the tail for the next call. Caller holds s_vad_mutex. */
static void __vad_accumulate(const uint8_t *data, size_t size)
{
    size_t frame_bytes = __required_frame_bytes();
    size_t offset = 0;

    if (frame_bytes == 0 || frame_bytes > sizeof(s_vad_pcm)) {
        return;
    }
    while (offset < size) {
        size_t want = frame_bytes - s_vad_pcm_len;
        size_t take = (size - offset < want) ? (size - offset) : want;
        memcpy(s_vad_pcm + s_vad_pcm_len, data + offset, take);
        s_vad_pcm_len += take;
        offset += take;
        if (s_vad_pcm_len == frame_bytes) {
            /* Clearing the length first keeps the accumulator consistent if
             * the engine ever re-enters the feed. */
            s_vad_pcm_len = 0;
#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
            __vad_ring_push(s_vad_pcm);
#else
            __vad_process_frame(s_vad_pcm, (uint32_t)frame_bytes);
#endif
        }
    }
}

OPERATE_RET tkl_vad_feed(uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) return OPRT_INVALID_PARM;
    if (__vad_lock() != OPRT_OK) return OPRT_RESOURCE_NOT_READY;
    if (!s_vad.initialized || !s_vad.started) { __vad_unlock(); return OPRT_RESOURCE_NOT_READY; }

#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
    /* The RNN engine's unit is fixed at 20 ms, so a caller handing over one
     * capture callback's worth of audio is sliced here rather than rejected. */
    __vad_accumulate(data, len);
#else
    if (len != __required_frame_bytes()) { __vad_unlock(); return OPRT_INVALID_PARM; }
    __vad_process_frame(data, len);
#endif

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
#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
    if (s_rnn_vad != NULL) {
        rnn_vad_start(s_rnn_vad);
        s_rnn_vad_frames = 0;
        s_vad_ring_read = 0;
        s_vad_ring_write = 0;
    }
#endif
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
#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
    s_vad_ring_read = 0;
    s_vad_ring_write = 0;
#endif
    __vad_unlock();

#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
    /* The task may be inside the engine; let it leave before resetting the
     * model state. Worst case is one 20 ms frame. */
    {
        int i;
        for (i = 0; i < 20 && s_vad_task_busy; ++i) {
            tkl_system_sleep(2);
        }
    }
    if (s_rnn_vad != NULL) {
        rnn_vad_stop(s_rnn_vad);
    }
#endif
    return OPRT_OK;
}

OPERATE_RET tkl_vad_deinit(void)
{
#if defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)
    if (s_vad_thread != NULL) {
        s_vad_task_exit = 1;
        if (s_vad_sem != NULL) {
            (void)tkl_semaphore_post(s_vad_sem);
        }
        (void)tkl_thread_release(s_vad_thread);
        s_vad_thread = NULL;
        /* Let the task observe exit before the model is released. */
        tkl_system_sleep(50);
    }
    if (s_vad_sem != NULL) {
        (void)tkl_semaphore_release(s_vad_sem);
        s_vad_sem = NULL;
    }
    if (s_rnn_vad != NULL) {
        rnn_vad_destroy(s_rnn_vad);
        s_rnn_vad = NULL;
    }
#endif
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
    if (data == NULL || __vad_lock() != OPRT_OK) return;
    if (!s_vad.initialized || !s_vad.started) { __vad_unlock(); return; }
    __vad_accumulate(data, size);
    __vad_unlock();
}
