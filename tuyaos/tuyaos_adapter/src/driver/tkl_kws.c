#include "tkl_kws.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tkl_mutex.h"
#include "tkl_semaphore.h"
#include "tkl_system.h"
#include "tkl_thread.h"

#if defined(ENABLE_AUDIO) && (ENABLE_AUDIO == 1)

/* KWS provider backed by the TAL audio subsystem archive (keyword_tflite_*).
 * That archive plays the same role here as libtutuClear does on T5: a Tuya
 * algorithm library compiled for this chip, linked from the TKL port because
 * the engine depends on the chip ABI, not on the TDL/TAL data plane.
 *
 * The engine consumes PCM units handed in by the caller; it does not own a
 * capture stream. The archive's default model is not the shipped wake word, so
 * tal_nhty_model.c overrides g_fsmn_v8_0515_avg_int8_model_data with the nhty
 * training run, and the keyword is registered with the token ids derived below
 * because the archive carries no lexicon. */
#include "audio_subsys_kws.h"

#if defined(JIELI_SELECTED_CHIP_WL82)
/* Playback activity, from the AEC reference ring (tuya_audio_aec.c). */
int tuya_audio_aec_is_playing(void);
#endif

#define KWS_UNIT_SAMPLES   160
#define KWS_UNIT_BYTES     (KWS_UNIT_SAMPLES * 2)
/* x8 lifts quiet live speech into the model's range, but the window peak on
 * this mic reaches 32767 at x8 (clipped). x4 keeps the loudest speech near
 * 16k, inside the model's tested range and free of clipping distortion. */
#define KWS_INPUT_GAIN     4

static void          *s_kws_ctx;
static void          *s_kws_mem;
static TKL_KWS_WAKEUP_CB s_kws_cb;
static uint8_t        s_kws_ready;
static uint8_t        s_kws_running;
static uint32_t       s_kws_units;
static int16_t        s_kws_gain_buf[KWS_UNIT_SAMPLES];
static TKL_KWS_WAKEUP_WORD_E s_kws_word = TKL_KWS_WAKEUP_WORD_UNKNOWN;

/* Per-window diagnostics: what the window contained and what the inference
 * cost. The engine only writes result.score on a hit, so the max score is the
 * closest we get to a near-miss reading. */
static uint32_t       s_kws_win_peak;
static float          s_kws_win_score;
static uint32_t       s_kws_ms_sum;
static uint32_t       s_kws_ms_max;
static uint32_t       s_kws_ms_count;
static uint32_t       s_kws_drops;
static uint32_t       s_kws_win_drops;
static uint32_t       s_kws_win_skipped;
/* Feed/consume counters. They are printed from both sides so "no units=" can
 * be attributed to capture not enqueuing (fed stays 0) or the worker not
 * running (fed grows, popped stays 0). */
static uint32_t       s_kws_fed;
static uint32_t       s_kws_popped;

/* Capture-fed path: the AI modes register a callback and enable the engine but
 * never hand it PCM (the vendor engines pull it themselves), so the capture
 * hook queues units and this task runs the inference off the audio task. */
#define KWS_RING_UNITS 16
static int16_t           s_kws_ring[KWS_RING_UNITS][KWS_UNIT_SAMPLES];
static uint32_t          s_kws_ring_write;
static uint32_t          s_kws_ring_read;
static TKL_MUTEX_HANDLE  s_kws_lock;
static TKL_SEM_HANDLE    s_kws_sem;
static TKL_THREAD_HANDLE s_kws_thread;

static void __kws_task(void *arg);

/* The archive ships no lexicon, so the ids were derived from the engine
 * itself: registering one keyword per token and replaying a recorded
 * "你好涂鸦" gave the CTC path 4 -> 9 -> 5 -> 1 at 400/640/850/1060 ms; that
 * sequence detects the same recording at score 82 and, at 1/3 amplitude,
 * still at score 83.
 *
 * The acceptance threshold sits between the two populations we have measured:
 * a real utterance scores 0.82+, while room noise has produced 0.504 (a false
 * wake that opened a listen session). 0.30 let that through, so the bar is at
 * 0.60 - high enough to reject the noise cases seen so far and still well
 * below a genuine wake. */
#define KWS_ACCEPT_SCORE 0.60f

static int __kws_register_nhty(void)
{
    int token_ids[4] = {4, 9, 5, 1};

    if (s_kws_word == TKL_KWS_WAKEUP_NIHAO_TUYA) {
        return 0;
    }
    if (keyword_tflite_add_keyword(s_kws_ctx, "nhty", token_ids, 4, KWS_ACCEPT_SCORE) != 0) {
        printf("[TAL_KWS] keyword registration failed\n");
        return -1;
    }
    s_kws_word = TKL_KWS_WAKEUP_NIHAO_TUYA;
    printf("[TAL_KWS] keyword nhty registered (tokens 4 9 5 1)\n");
    return 0;
}

OPERATE_RET tkl_kws_init(void)
{
    TAL_KWS_CONFIG_T cfg;

    if (s_kws_ready) {
        return OPRT_OK;
    }

    memset(&cfg, 0, sizeof(cfg));
    keyword_tflite_config_init(&cfg);
    /* The archive's config_init() asks for a 96 KiB TFLite tensor arena, but
     * KeywordSpottingTflite's constructor rejects any arena outside 1..48 KiB
     * (it prints and calls exit(), which hangs the board on this SDK). 44 KiB
     * is inside the accepted range and still roomy for the fsmn int8 model. */
    cfg.arena_size = 44 * 1024;

    int mem_size = keyword_tflite_get_mem_size(&cfg);
    printf("[TAL_KWS] init: mem_size=%d arena=%d\n", mem_size, cfg.arena_size);
    if (mem_size <= 0) {
        return OPRT_COM_ERROR;
    }

    s_kws_mem = malloc((size_t)mem_size);
    printf("[TAL_KWS] init: arena=%p\n", s_kws_mem);
    if (s_kws_mem == NULL) {
        return OPRT_MALLOC_FAILED;
    }

    int create_ret = keyword_tflite_create("", &cfg, s_kws_mem, mem_size, &s_kws_ctx);
    printf("[TAL_KWS] init: create=%d ctx=%p\n", create_ret, s_kws_ctx);
    if (create_ret != 0 || s_kws_ctx == NULL) {
        free(s_kws_mem);
        s_kws_mem = NULL;
        return OPRT_COM_ERROR;
    }

    s_kws_ready = 1;
    (void)__kws_register_nhty();
    /* The bundled example starts feeding PCM as soon as the engine exists and
     * never calls tkl_kws_enable(), so start live; disable() still stops it. */
    s_kws_running = 1;
    printf("[TAL_KWS] engine ready, mem=%d bytes\n", mem_size);

    if (tkl_mutex_create_init(&s_kws_lock) == OPRT_OK &&
        tkl_semaphore_create_init(&s_kws_sem, 0, 1) == OPRT_OK) {
        /* The engine costs ~7 ms per 10 ms unit on this chip, so the worker
         * must sit at the bottom of the TKL priority band: audio, network and
         * the UI win every time they are runnable, and the engine catches up
         * on the idle slots. Ring drops record what that costs. */
        if (tkl_thread_create(&s_kws_thread, "tuya_kws", 16 * 1024, 1,
                              __kws_task, NULL) != OPRT_OK) {
            printf("[TAL_KWS] worker task create failed\n");
            s_kws_thread = NULL;
        }
    } else {
        printf("[TAL_KWS] worker sync create failed\n");
    }
    return OPRT_OK;
}

OPERATE_RET tkl_kws_wakeup_word_config(TKL_KWS_WAKEUP_WORD_E *words, uint32_t count)
{
    if (!s_kws_ready || words == NULL || count == 0) {
        return OPRT_INVALID_PARM;
    }

    s_kws_word = TKL_KWS_WAKEUP_WORD_UNKNOWN;

    for (uint32_t i = 0; i < count; i++) {
        if (words[i] == TKL_KWS_WAKEUP_NIHAO_TUYA) {
            (void)__kws_register_nhty();
        }
    }

    return s_kws_word == TKL_KWS_WAKEUP_WORD_UNKNOWN ? OPRT_NOT_SUPPORTED : OPRT_OK;
}

uint32_t tkl_kws_get_process_uint_size(void)
{
    return KWS_UNIT_BYTES;
}

TKL_KWS_WAKEUP_WORD_E tkl_kws_recognize_wakeup_word(uint8_t *data, uint32_t len)
{
    TAL_KWS_RESULT_T result;
    int detected;

    if (!s_kws_ready || !s_kws_running || data == NULL || len != KWS_UNIT_BYTES) {
        return TKL_KWS_WAKEUP_WORD_UNKNOWN;
    }

    memset(&result, 0, sizeof(result));
    /* KWS_INPUT_GAIN bridges the level gap between this mic and the model:
     * live speech peaks at 200-1800 raw, while the model was verified on a
     * recording at 23808. x4 keeps the loudest live speech near 16k (the
     * window peak reached 32767 at x8, i.e. clipped) and the clamp below
     * prevents a close talker from wrapping instead of clipping. */
    {
        int i;
        uint32_t peak = 0;

        for (i = 0; i < KWS_UNIT_SAMPLES; i++) {
            int32_t v = (int32_t)((const short *)data)[i] * KWS_INPUT_GAIN;
            int32_t a;

            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            s_kws_gain_buf[i] = (int16_t)v;
            a = v < 0 ? -v : v;
            if ((uint32_t)a > peak) {
                peak = (uint32_t)a;
            }
        }
        if (peak > s_kws_win_peak) {
            s_kws_win_peak = peak;
        }
    }
    {
        uint32_t t0 = (uint32_t)tkl_system_get_millisecond();
        uint32_t dt;

        /* 1 = keyword detected (result filled), 0 = no detection, -1 = bad args. */
        detected = keyword_tflite_forward_pcm(s_kws_ctx, s_kws_gain_buf, KWS_UNIT_SAMPLES, &result);
        dt = (uint32_t)tkl_system_get_millisecond() - t0;
        s_kws_ms_sum += dt;
        ++s_kws_ms_count;
        if (dt > s_kws_ms_max) {
            s_kws_ms_max = dt;
        }
    }
    if (detected < 0) {
        return TKL_KWS_WAKEUP_WORD_UNKNOWN;
    }
    if (result.score > s_kws_win_score) {
        s_kws_win_score = result.score;
    }

    /* One unit is 10 ms, so this marks the inference cadence: a silent UART
     * then means the caller is not feeding or the engine is not running,
     * rather than "no keyword". ms_avg is the per-unit inference cost, the
     * share of a 10 ms frame the engine takes; skipped counts the units the
     * worker dropped while the speaker was active. */
    ++s_kws_units;
    if (s_kws_units <= 5u || (s_kws_units % 500u) == 0u) {
        printf("[TAL_KWS] units=%u peak=%u maxscore=%d.%03d ms_avg=%u ms_max=%u drops=%u/%u skipped=%u\n",
               (unsigned)s_kws_units, (unsigned)s_kws_win_peak, (int)s_kws_win_score,
               (int)((s_kws_win_score < 0 ? -s_kws_win_score : s_kws_win_score) * 1000.0f) % 1000,
               (unsigned)(s_kws_ms_count ? s_kws_ms_sum / s_kws_ms_count : 0),
               (unsigned)s_kws_ms_max, (unsigned)s_kws_win_drops, (unsigned)s_kws_drops,
               (unsigned)s_kws_win_skipped);
        s_kws_win_peak = 0;
        s_kws_win_score = 0.0f;
        s_kws_ms_sum = 0;
        s_kws_ms_max = 0;
        s_kws_ms_count = 0;
        s_kws_win_drops = 0;
        s_kws_win_skipped = 0;
    }

    if (detected > 0) {
        if (result.score < KWS_ACCEPT_SCORE) {
            printf("[TAL_KWS] REJECTED score=%d.%03d < %d.%03d name=\"%s\" unit=%u\n", (int)result.score,
                   (int)((result.score < 0 ? -result.score : result.score) * 1000.0f) % 1000, (int)KWS_ACCEPT_SCORE,
                   (int)(KWS_ACCEPT_SCORE * 1000.0f) % 1000, result.name, (unsigned)s_kws_units);
            return TKL_KWS_WAKEUP_WORD_UNKNOWN;
        }
        printf("[TAL_KWS] DETECTED score=%d.%03d name=\"%s\" unit=%u\n", (int)result.score,
               (int)((result.score < 0 ? -result.score : result.score) * 1000.0f) % 1000, result.name,
               (unsigned)s_kws_units);
        if (s_kws_word != TKL_KWS_WAKEUP_WORD_UNKNOWN && s_kws_cb != NULL) {
            s_kws_cb(s_kws_word);
        }
        /* No reset() here: the keyword stays registered in the engine for the
         * next utterance. See tkl_kws_enable() for why the archive's reset is
         * not used on this path. */
        return s_kws_word;
    }

    return TKL_KWS_WAKEUP_WORD_UNKNOWN;
}

OPERATE_RET tkl_kws_reg_wakeup_cb(TKL_KWS_WAKEUP_CB wakeup_cb)
{
    if (wakeup_cb == NULL) {
        return OPRT_INVALID_PARM;
    }
    s_kws_cb = wakeup_cb;
    return OPRT_OK;
}

OPERATE_RET tkl_kws_enable(void)
{
    if (!s_kws_ready) {
        return OPRT_RESOURCE_NOT_READY;
    }
    /* Do not call keyword_tflite_reset() here. The standalone example detects
     * "你好涂鸦" while never resetting the engine; the AI modes reset it right
     * before the first PCM unit, and in that path the keyword never fires even
     * though add_keyword() reports success afterwards. Until the archive's
     * reset semantics are known, keep the keyword table as created in init().
     * Enabling is a no-op for the table when init() already registered it;
     * adding "nhty" twice would give the engine two identical CTC targets. */
    if (s_kws_word == TKL_KWS_WAKEUP_WORD_UNKNOWN) {
        (void)__kws_register_nhty();
    }
    s_kws_units = 0;
    s_kws_running = 1;
    printf("[TAL_KWS] enable (word=%d cb=%p)\n", (int)s_kws_word, (void *)s_kws_cb);
    return OPRT_OK;
}

OPERATE_RET tkl_kws_disable(void)
{
    printf("[TAL_KWS] disable (units=%u)\n", (unsigned)s_kws_units);
    s_kws_running = 0;
    return OPRT_OK;
}

OPERATE_RET tkl_kws_deinit(void)
{
    if (s_kws_ctx != NULL) {
        keyword_tflite_destroy(s_kws_ctx);
        s_kws_ctx = NULL;
    }
    if (s_kws_mem != NULL) {
        free(s_kws_mem);
        s_kws_mem = NULL;
    }
    s_kws_ready = 0;
    s_kws_running = 0;
    s_kws_word = TKL_KWS_WAKEUP_WORD_UNKNOWN;
    return OPRT_OK;
}

static void __kws_task(void *arg)
{
    int16_t unit[KWS_UNIT_SAMPLES];

    (void)arg;
    for (;;) {
        if (tkl_semaphore_wait(s_kws_sem, TKL_SEM_WAIT_FOREVER) != OPRT_OK) {
            continue;
        }
        /* The semaphore counts to 1, so one post can stand for many queued
         * units. Draining here is what keeps the ring from overflowing when
         * inference is slower than the 10 ms capture cadence; yield briefly
         * every two units so lwIP/TLS tasks are never starved. */
        uint32_t batch = 0;
        for (;;) {
            if (tkl_mutex_lock(s_kws_lock) != OPRT_OK) {
                break;
            }
            if (s_kws_ring_write == s_kws_ring_read || !s_kws_running) {
                (void)tkl_mutex_unlock(s_kws_lock);
                break;
            }
            memcpy(unit, s_kws_ring[s_kws_ring_read % KWS_RING_UNITS], sizeof(unit));
            ++s_kws_ring_read;
            ++s_kws_popped;
            (void)tkl_mutex_unlock(s_kws_lock);
            if ((s_kws_popped % 500u) == 0u) {
                printf("[TAL_KWS] worker popped=%u fed=%u\n", (unsigned)s_kws_popped, (unsigned)s_kws_fed);
            }
#if defined(JIELI_SELECTED_CHIP_WL82)
            if (tuya_audio_aec_is_playing()) {
                /* Speaker active: drop the unit instead of spending ~7 ms on
                 * inference, and never wake on our own TTS. The stream resumes
                 * when playback's reference ring drains. */
                ++s_kws_win_skipped;
                continue;
            }
#endif
            (void)tkl_kws_recognize_wakeup_word((uint8_t *)unit, KWS_UNIT_BYTES);
            if (++batch >= 2u) {
                batch = 0;
                (void)tkl_system_sleep(1u);
            }
        }
    }
}

/* Capture hook for the engine-internal path. Only used when a callback is
 * registered (the AI modes); the example feeds tkl_kws_recognize_wakeup_word()
 * itself and must not be processed twice. */
void tkl_jieli_kws_feed_capture(const uint8_t *data, size_t size)
{
    const uint8_t *pcm = data;
    static uint8_t carry[KWS_UNIT_BYTES];
    static uint32_t carry_len;
    uint32_t fed_before;

    if (!s_kws_ready || !s_kws_running || s_kws_cb == NULL || data == NULL ||
        s_kws_lock == NULL || s_kws_sem == NULL) {
        return;
    }
    fed_before = s_kws_fed;
    if (tkl_mutex_lock(s_kws_lock) != OPRT_OK) {
        return;
    }
    /* The capture callback is not required to split on our 10 ms boundary, so
     * the tail of one callback is glued to the head of the next. Dropping it
     * would leave a phase gap in every unit, and the CTC path is stateful. */
    if (carry_len != 0) {
        uint32_t want = KWS_UNIT_BYTES - carry_len;
        uint32_t take = (uint32_t)size < want ? (uint32_t)size : want;

        memcpy(carry + carry_len, pcm, take);
        carry_len += take;
        pcm += take;
        size -= take;
        if (carry_len == KWS_UNIT_BYTES) {
            if (s_kws_ring_write - s_kws_ring_read >= KWS_RING_UNITS) {
                ++s_kws_ring_read;
                ++s_kws_drops;
                ++s_kws_win_drops;
            }
            memcpy(s_kws_ring[s_kws_ring_write % KWS_RING_UNITS], carry, KWS_UNIT_BYTES);
            ++s_kws_ring_write;
            ++s_kws_fed;
            carry_len = 0;
            (void)tkl_semaphore_post(s_kws_sem);
        }
    }
    while (size >= KWS_UNIT_BYTES) {
        if (s_kws_ring_write - s_kws_ring_read >= KWS_RING_UNITS) {
            ++s_kws_ring_read; /* consumer fell behind: drop the oldest */
            ++s_kws_drops;
            ++s_kws_win_drops;
        }
        memcpy(s_kws_ring[s_kws_ring_write % KWS_RING_UNITS], pcm, KWS_UNIT_BYTES);
        ++s_kws_ring_write;
        ++s_kws_fed;
        pcm += KWS_UNIT_BYTES;
        size -= KWS_UNIT_BYTES;
        (void)tkl_semaphore_post(s_kws_sem);
    }
    if (size != 0) {
        memcpy(carry, pcm, size);
        carry_len = (uint32_t)size;
    }
    (void)tkl_mutex_unlock(s_kws_lock);
    if (s_kws_fed != fed_before && (s_kws_fed % 500u) == 0u) {
        printf("[TAL_KWS] feed enqueued=%u popped=%u wr=%u rd=%u drops=%u\n", (unsigned)s_kws_fed,
               (unsigned)s_kws_popped, (unsigned)s_kws_ring_write, (unsigned)s_kws_ring_read,
               (unsigned)s_kws_drops);
    }
}

#else /* no engine selected: keep the contract callable */

/* Keep the public contract callable so the shared capture path links on every
 * chip and on the default HOLD image without pulling the vendor archives. */
OPERATE_RET tkl_kws_init(void)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_kws_reg_wakeup_cb(TKL_KWS_WAKEUP_CB wakeup_cb)
{
    (void)wakeup_cb;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_kws_enable(void)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_kws_disable(void)
{
    return OPRT_OK;
}

OPERATE_RET tkl_kws_deinit(void)
{
    return OPRT_OK;
}

/* Same public contract as the engine branches, so the shared capture path and
 * the multimedia example link on every chip instead of failing to resolve
 * these symbols. */
OPERATE_RET tkl_kws_wakeup_word_config(TKL_KWS_WAKEUP_WORD_E *words, uint32_t count)
{
    (void)words;
    (void)count;
    return OPRT_NOT_SUPPORTED;
}

uint32_t tkl_kws_get_process_uint_size(void)
{
    return 0;
}

TKL_KWS_WAKEUP_WORD_E tkl_kws_recognize_wakeup_word(uint8_t *data, uint32_t len)
{
    (void)data;
    (void)len;
    return TKL_KWS_WAKEUP_WORD_UNKNOWN;
}

#endif /* ENABLE_AUDIO */
