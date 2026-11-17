#include "tkl_kws.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tkl_mutex.h"
#include "tkl_semaphore.h"
#include "tkl_system.h"
#include "tkl_thread.h"

#if defined(JIELI_SELECTED_CHIP_WL82) && defined(JIELI_KWS_ENABLE) &&                                                      \
    !(defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1))

/* WL82 (AC791) KWS provider: the vendor jlsp far-keyword engine.
 *
 * The vendor app (apps/common/asr/jlkws/jlkws.c) runs the same engine from its
 * own audio_server encoder. TuyaOpen already owns the single encoder stream in
 * tkl_audio.c, so instead of opening a second one this adapter feeds the PCM
 * that the TKL capture callback already receives. The engine itself is a pure
 * PCM-in/keyword-out library (`jl_far_kws_model_process`), so no vendor audio
 * server, AEC or task wiring is needed.
 *
 * The built-in model is the SDK's fixed far-keyword model. Its word list is
 * the one documented in jlkws.c: 小杰小杰 / 小杰同学 / 播放音乐 / 暂停播放 /
 * 增大音量 / 减小音量 / 上一首 / 下一首. It does NOT contain the TuyaOpen
 * product word 你好涂鸦; there is no trained model for that word in this SDK.
 * See docs/plans/2026-11-12-jieli-followup/reports/02-report.md.
 *
 * The public TKL contract has no slot for a vendor-specific word, so a
 * detected vendor wake word is reported as TKL_KWS_WAKEUP_WORD_UNKNOWN. This
 * is deliberate: mapping it to TKL_KWS_WAKEUP_NIHAO_TUYA would silently
 * pretend the product word works. */

/* The vendor header (apps/common/asr/jlkws/jlsp_far_keyword.h) lives outside
 * the adapter include set. Declare the two entry points this file uses; the
 * signatures are copied verbatim from that header. */
int jl_far_kws_model_get_heap_size(int model, int *model_size, int *private_heap_size,
                                   int *share_heap_size);
void *jl_far_kws_model_init(int model, char *private_heap_buffer, int private_heap_size,
                            char *share_heap_buffer, int share_heap_size, int model_size,
                            float *confidence, int online);
int jl_far_kws_model_reset(void *m);
int jl_far_kws_model_process(void *m, int model, char *pcm, int size);
int jl_far_kws_model_free(void *m);

extern void *zalloc(size_t size);
extern void free(void *mem);

/* jlsp_far_keyword_param.c.o reads this switch at run time. jlkws.c defines it
 * when JLKWS_ALGORITHM is selected; the TKL build selects the engine directly,
 * so the definition moves here. */
const int CONFIG_KWS_RAM_USE_ENABLE = 1;

#define JIELI_KWS_MODEL_ID        0
#define JIELI_KWS_SAMPLE_RATE     16000
#define JIELI_KWS_FRAME_SAMPLES   160
#define JIELI_KWS_FRAME_BYTES     (JIELI_KWS_FRAME_SAMPLES * 2)
#define JIELI_KWS_RING_BYTES      (JIELI_KWS_FRAME_BYTES * 32)
#define JIELI_KWS_TASK_STACK      4096
#define JIELI_KWS_TASK_PRIORITY   3

#ifdef JIELI_KWS_VENDOR_SELFTEST
/* The vendor archive carries its own PCM test vector. Feeding it through the
 * same entry point separates "engine/licence broken" from "acoustic input did
 * not match the model". Test-build only. */
extern const int16_t command_test_data10[];
extern const int command_test_data_size;

static void __vendor_selftest(void *model)
{
    int size = command_test_data_size;
    int offset;
    int detections = 0;
    printf("[JIELI_KWS] vendor selftest data=%d bytes\n", size);
    if (size <= 0) return;
    for (offset = 0; offset + JIELI_KWS_FRAME_BYTES <= size; offset += JIELI_KWS_FRAME_BYTES) {
        int ret = jl_far_kws_model_process(model, JIELI_KWS_MODEL_ID,
                                           (char *)command_test_data10 + offset,
                                           JIELI_KWS_FRAME_BYTES);
        if (ret > 1) {
            ++detections;
            printf("[JIELI_KWS] vendor selftest detect ret=%d at %d ms\n",
                   ret, (offset / 2) * 1000 / JIELI_KWS_SAMPLE_RATE);
            (void)jl_far_kws_model_reset(model);
        }
    }
    (void)jl_far_kws_model_reset(model);
    printf("[JIELI_KWS] vendor selftest done detections=%d\n", detections);
}
#endif

/* Return codes seen in jlkws.c: >1 is a detection. 2 is the wake word used by
 * the vendor QYAI path; 3 is the second training word. 4..10 are the vendor
 * command words. Only 2 and 3 are wake words for a chat assistant. */
#define JIELI_KWS_RET_WAKE_XIAOJIE_XIAOJIE 2
#define JIELI_KWS_RET_WAKE_XIAOJIE_TONGXUE 3
#define JIELI_KWS_RET_CMD_MIN              4

/* Detection confidence for the eight training words, copied from jlkws.c. */
static float s_confidence[8] = {
    0.5f, 0.5f, 0.5f, 0.5f,
    0.5f, 0.5f, 0.5f, 0.5f,
};

typedef struct {
    void *model;
    char *private_heap;
    char *share_heap;
    int private_heap_size;
    int share_heap_size;
    int model_size;

    uint8_t ring[JIELI_KWS_RING_BYTES];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t dropped_bytes;
    uint32_t frames_processed;
    uint32_t detections;
    uint32_t last_process_ms;
    uint32_t ret_neg;
    uint32_t ret_zero;
    uint32_t ret_one;
    uint64_t frame_ms_sum;
    uint32_t frame_ms_peak;

    TKL_MUTEX_HANDLE lock;
    TKL_SEM_HANDLE sem;
    TKL_THREAD_HANDLE thread;
    TKL_KWS_WAKEUP_CB wakeup_cb;

    uint8_t running;
    uint8_t exit;
    uint8_t initialized;
} JIELI_KWS_STATE_T;

static JIELI_KWS_STATE_T s_kws;

static uint32_t __ring_used(void)
{
    return s_kws.write_pos - s_kws.read_pos;
}

static void __ring_clear(void)
{
    s_kws.read_pos = 0;
    s_kws.write_pos = 0;
}

static int __ring_write(const uint8_t *data, size_t size)
{
    uint32_t used = __ring_used();
    uint32_t free_bytes = (uint32_t)sizeof(s_kws.ring) - used;
    uint32_t first;
    uint32_t write_pos;

    if (size > free_bytes) {
        /* The consumer fell behind. Drop the incoming block rather than
         * overwriting audio the engine has not seen: a torn frame is worse
         * than a gap, and the counter makes the condition visible. */
        s_kws.dropped_bytes += (uint32_t)size;
        return -1;
    }
    write_pos = s_kws.write_pos % (uint32_t)sizeof(s_kws.ring);
    first = (uint32_t)sizeof(s_kws.ring) - write_pos;
    if (first > size) first = (uint32_t)size;
    memcpy(s_kws.ring + write_pos, data, first);
    if (size > first) {
        memcpy(s_kws.ring, data + first, size - first);
    }
    s_kws.write_pos += (uint32_t)size;
    return 0;
}

/* Copies one fixed engine frame out of the ring. Caller holds the lock. */
static int __ring_read_frame(uint8_t *frame)
{
    uint32_t read_pos;
    uint32_t first;

    if (__ring_used() < JIELI_KWS_FRAME_BYTES) return -1;
    read_pos = s_kws.read_pos % (uint32_t)sizeof(s_kws.ring);
    first = (uint32_t)sizeof(s_kws.ring) - read_pos;
    if (first > JIELI_KWS_FRAME_BYTES) first = JIELI_KWS_FRAME_BYTES;
    memcpy(frame, s_kws.ring + read_pos, first);
    if (JIELI_KWS_FRAME_BYTES > first) {
        memcpy(frame + first, s_kws.ring, JIELI_KWS_FRAME_BYTES - first);
    }
    s_kws.read_pos += JIELI_KWS_FRAME_BYTES;
    return 0;
}

static void __notify_wakeup(int ret)
{
    TKL_KWS_WAKEUP_CB cb;
    TKL_KWS_WAKEUP_WORD_E word = TKL_KWS_WAKEUP_WORD_UNKNOWN;

    ++s_kws.detections;
    if (ret == JIELI_KWS_RET_WAKE_XIAOJIE_XIAOJIE) {
        printf("[JIELI_KWS] vendor wake word 0 (小杰小杰), ret=%d\n", ret);
    } else if (ret == JIELI_KWS_RET_WAKE_XIAOJIE_TONGXUE) {
        printf("[JIELI_KWS] vendor wake word 1 (小杰同学), ret=%d\n", ret);
    } else if (ret >= JIELI_KWS_RET_CMD_MIN) {
        printf("[JIELI_KWS] vendor command word ignored, ret=%d\n", ret);
        return;
    } else {
        printf("[JIELI_KWS] unknown detection ret=%d\n", ret);
        return;
    }

    cb = s_kws.wakeup_cb;
    if (cb != NULL) {
        cb(word);
    }
}

static void __kws_task(void *arg)
{
    uint8_t frame[JIELI_KWS_FRAME_BYTES];
    (void)arg;

    while (!s_kws.exit) {
        int ret;
        SYS_TIME_T start;

        (void)tkl_semaphore_wait(s_kws.sem, 200);
        if (s_kws.exit) break;
        if (!s_kws.running) continue;

        for (;;) {
            uint32_t busy = 0;
            uint64_t ms = 0;
            uint32_t i;
            if (tkl_mutex_lock(s_kws.lock) != OPRT_OK) break;
            if (__ring_read_frame(frame) != 0) {
                busy = 0;
            } else {
                busy = 1;
            }
            (void)tkl_mutex_unlock(s_kws.lock);
            if (!busy) break;

            for (i = 0; i < JIELI_KWS_FRAME_SAMPLES; ++i) {
                int16_t sample;
                memcpy(&sample, frame + i * 2u, sizeof(sample));
                ms += (uint64_t)((int32_t)sample * (int32_t)sample);
            }
            ms /= JIELI_KWS_FRAME_SAMPLES;
            s_kws.frame_ms_sum += ms;
            if (ms > s_kws.frame_ms_peak) s_kws.frame_ms_peak = (uint32_t)ms;

            start = tkl_system_get_millisecond();
            ret = jl_far_kws_model_process(s_kws.model, JIELI_KWS_MODEL_ID,
                                           (char *)frame, (int)sizeof(frame));
            s_kws.last_process_ms = (uint32_t)(tkl_system_get_millisecond() - start);
            ++s_kws.frames_processed;
            if (ret > 1) {
                __notify_wakeup(ret);
                (void)jl_far_kws_model_reset(s_kws.model);
            } else if (ret < 0) {
                ++s_kws.ret_neg;
            } else if (ret == 0) {
                ++s_kws.ret_zero;
            } else {
                ++s_kws.ret_one;
            }
            /* 100 frames = 1 s at 16 kHz / 10 ms. Log once per minute so the
             * capture->engine path is observable without per-frame spam.
             * frame_ms_peak/avg are the mean-square of the input PCM, so the
             * mic level can be compared with the VAD threshold (500 RMS =
             * 250000 mean-square). */
            if ((s_kws.frames_processed % 6000u) == 0u) {
                uint32_t used;
                (void)tkl_mutex_lock(s_kws.lock);
                used = __ring_used();
                (void)tkl_mutex_unlock(s_kws.lock);
                printf("[JIELI_KWS] stats frames=%u last=%ums ring=%u dropped=%u "
                       "ms_peak=%u ms_avg=%u ret(-1/0/1)=%u/%u/%u\n",
                       s_kws.frames_processed, s_kws.last_process_ms, used,
                       s_kws.dropped_bytes, s_kws.frame_ms_peak,
                       (uint32_t)(s_kws.frame_ms_sum / 1000u),
                       s_kws.ret_neg, s_kws.ret_zero, s_kws.ret_one);
                s_kws.frame_ms_sum = 0;
                s_kws.frame_ms_peak = 0;
                s_kws.ret_neg = 0;
                s_kws.ret_zero = 0;
                s_kws.ret_one = 0;
            }
        }
    }
}

OPERATE_RET tkl_kws_init(void)
{
    int model_size = 0;
    int private_heap_size = 0;
    int share_heap_size = 0;
    int query_ret;

    if (s_kws.initialized) return OPRT_OK;

    memset(&s_kws, 0, sizeof(s_kws));
    /* The vendor getter returns model_size, not a status code (jlkws.c ignores
     * the return value and reads only the three out-parameters). Treat the
     * returned value as the model size and validate the sizes instead. */
    query_ret = jl_far_kws_model_get_heap_size(JIELI_KWS_MODEL_ID, &model_size,
                                               &private_heap_size, &share_heap_size);
    if (query_ret > 0) {
        model_size = query_ret;
    }
    if (model_size <= 0 || private_heap_size <= 0 || share_heap_size <= 0) {
        printf("[JIELI_KWS] heap query failed: model=%d private=%d share=%d\n",
               model_size, private_heap_size, share_heap_size);
        return OPRT_COM_ERROR;
    }
    printf("[JIELI_KWS] model heap: model=%d private=%d share=%d bytes\n",
           model_size, private_heap_size, share_heap_size);

    s_kws.private_heap = (char *)zalloc((size_t)private_heap_size);
    s_kws.share_heap = (char *)zalloc((size_t)share_heap_size);
    if (s_kws.private_heap == NULL || s_kws.share_heap == NULL) {
        printf("[JIELI_KWS] heap allocation failed: private=%p share=%p\n",
               (void *)s_kws.private_heap, (void *)s_kws.share_heap);
        goto __error;
    }
    s_kws.private_heap_size = private_heap_size;
    s_kws.share_heap_size = share_heap_size;
    s_kws.model_size = model_size;

    s_kws.model = jl_far_kws_model_init(JIELI_KWS_MODEL_ID, s_kws.private_heap,
                                       private_heap_size, s_kws.share_heap,
                                       share_heap_size, model_size, s_confidence, 0);
    if (s_kws.model == NULL) {
        printf("[JIELI_KWS] model init failed\n");
        goto __error;
    }

    if (tkl_mutex_create_init(&s_kws.lock) != OPRT_OK) {
        printf("[JIELI_KWS] mutex create failed\n");
        goto __error;
    }
    if (tkl_semaphore_create_init(&s_kws.sem, 0, 1) != OPRT_OK) {
        printf("[JIELI_KWS] semaphore create failed\n");
        goto __error;
    }
    if (tkl_thread_create(&s_kws.thread, "tuya_kws", JIELI_KWS_TASK_STACK,
                          JIELI_KWS_TASK_PRIORITY, __kws_task, NULL) != OPRT_OK) {
        printf("[JIELI_KWS] task create failed\n");
        goto __error;
    }

    s_kws.initialized = 1;
    printf("[JIELI_KWS] init ok (vendor far-keyword model %d, 16 kHz mono)\n",
           JIELI_KWS_MODEL_ID);
#ifdef JIELI_KWS_VENDOR_SELFTEST
    __vendor_selftest(s_kws.model);
#endif
    return OPRT_OK;

__error:
    if (s_kws.thread != NULL) {
        s_kws.exit = 1;
        if (s_kws.sem != NULL) {
            (void)tkl_semaphore_post(s_kws.sem);
        }
        (void)tkl_thread_release(s_kws.thread);
        s_kws.thread = NULL;
    }
    if (s_kws.sem != NULL) {
        (void)tkl_semaphore_release(s_kws.sem);
        s_kws.sem = NULL;
    }
    if (s_kws.lock != NULL) {
        (void)tkl_mutex_release(s_kws.lock);
        s_kws.lock = NULL;
    }
    if (s_kws.model != NULL) {
        (void)jl_far_kws_model_free(s_kws.model);
        s_kws.model = NULL;
    }
    if (s_kws.private_heap != NULL) {
        free(s_kws.private_heap);
        s_kws.private_heap = NULL;
    }
    if (s_kws.share_heap != NULL) {
        free(s_kws.share_heap);
        s_kws.share_heap = NULL;
    }
    return OPRT_COM_ERROR;
}

OPERATE_RET tkl_kws_reg_wakeup_cb(TKL_KWS_WAKEUP_CB wakeup_cb)
{
    if (wakeup_cb == NULL) return OPRT_INVALID_PARM;
    s_kws.wakeup_cb = wakeup_cb;
    return OPRT_OK;
}

OPERATE_RET tkl_kws_enable(void)
{
    if (!s_kws.initialized) return OPRT_RESOURCE_NOT_READY;
    if (s_kws.lock != NULL && tkl_mutex_lock(s_kws.lock) == OPRT_OK) {
        __ring_clear();
        (void)tkl_mutex_unlock(s_kws.lock);
    }
    if (s_kws.model != NULL) {
        (void)jl_far_kws_model_reset(s_kws.model);
    }
    s_kws.running = 1;
    (void)tkl_semaphore_post(s_kws.sem);
    printf("[JIELI_KWS] enabled\n");
    return OPRT_OK;
}

OPERATE_RET tkl_kws_disable(void)
{
    if (!s_kws.initialized) return OPRT_OK;
    s_kws.running = 0;
    if (s_kws.lock != NULL && tkl_mutex_lock(s_kws.lock) == OPRT_OK) {
        __ring_clear();
        (void)tkl_mutex_unlock(s_kws.lock);
    }
    printf("[JIELI_KWS] disabled (frames=%u detections=%u dropped=%u)\n",
           s_kws.frames_processed, s_kws.detections, s_kws.dropped_bytes);
    return OPRT_OK;
}

OPERATE_RET tkl_kws_deinit(void)
{
    TKL_THREAD_HANDLE thread;
    TKL_SEM_HANDLE sem;
    TKL_MUTEX_HANDLE lock;

    if (!s_kws.initialized) return OPRT_OK;
    s_kws.running = 0;
    s_kws.exit = 1;
    thread = s_kws.thread;
    sem = s_kws.sem;
    lock = s_kws.lock;
    s_kws.thread = NULL;
    s_kws.sem = NULL;
    s_kws.lock = NULL;
    s_kws.initialized = 0;

    if (sem != NULL) (void)tkl_semaphore_post(sem);
    if (thread != NULL) (void)tkl_thread_release(thread);
    /* Give the task a scheduling opportunity to observe exit before the model
     * and heaps it is using are released. */
    tkl_system_sleep(50);
    if (sem != NULL) (void)tkl_semaphore_release(sem);
    if (lock != NULL) (void)tkl_mutex_release(lock);
    if (s_kws.model != NULL) {
        (void)jl_far_kws_model_free(s_kws.model);
        s_kws.model = NULL;
    }
    if (s_kws.private_heap != NULL) {
        free(s_kws.private_heap);
        s_kws.private_heap = NULL;
    }
    if (s_kws.share_heap != NULL) {
        free(s_kws.share_heap);
        s_kws.share_heap = NULL;
    }
    memset(&s_kws, 0, sizeof(s_kws));
    return OPRT_OK;
}

void tkl_jieli_kws_feed_capture(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0) return;
    if (!s_kws.initialized || !s_kws.running) return;
    if (tkl_mutex_lock(s_kws.lock) != OPRT_OK) return;
    (void)__ring_write(data, size);
    (void)tkl_mutex_unlock(s_kws.lock);
    (void)tkl_semaphore_post(s_kws.sem);
}

#elif defined(JIELI_TAL_AUDIO_ENGINE) && (JIELI_TAL_AUDIO_ENGINE == 1)

/* KWS provider backed by the user-supplied TAL archive (keyword_tflite_*).
 * The engine consumes PCM units handed in by the caller; it does not own a
 * capture stream. The embedded model is nhty/phrase trained, but the archive
 * carries no token table, so the keyword is registered with a placeholder
 * token id until the export side supplies the real mapping. */
#include "tal_audio_engine_compat.h"

#define TAL_KWS_UNIT_SAMPLES   160
#define TAL_KWS_UNIT_BYTES     (TAL_KWS_UNIT_SAMPLES * 2)

static void          *s_tal_kws_ctx;
static void          *s_tal_kws_mem;
static TKL_KWS_WAKEUP_CB s_tal_kws_cb;
static uint8_t        s_tal_kws_ready;
static uint8_t        s_tal_kws_running;
static uint32_t       s_tal_kws_units;
static TKL_KWS_WAKEUP_WORD_E s_tal_kws_word = TKL_KWS_WAKEUP_WORD_UNKNOWN;

OPERATE_RET tkl_kws_init(void)
{
    TAL_KWS_CONFIG_T cfg;

    if (s_tal_kws_ready) {
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

    s_tal_kws_mem = malloc((size_t)mem_size);
    printf("[TAL_KWS] init: arena=%p\n", s_tal_kws_mem);
    if (s_tal_kws_mem == NULL) {
        return OPRT_MALLOC_FAILED;
    }

    int create_ret = keyword_tflite_create("", &cfg, s_tal_kws_mem, mem_size, &s_tal_kws_ctx);
    printf("[TAL_KWS] init: create=%d ctx=%p\n", create_ret, s_tal_kws_ctx);
    if (create_ret != 0 || s_tal_kws_ctx == NULL) {
        free(s_tal_kws_mem);
        s_tal_kws_mem = NULL;
        return OPRT_COM_ERROR;
    }

    s_tal_kws_ready = 1;
    printf("[TAL_KWS] engine ready, mem=%d bytes\n", mem_size);
    return OPRT_OK;
}

OPERATE_RET tkl_kws_wakeup_word_config(TKL_KWS_WAKEUP_WORD_E *words, uint32_t count)
{
    if (!s_tal_kws_ready || words == NULL || count == 0) {
        return OPRT_INVALID_PARM;
    }

    s_tal_kws_word = TKL_KWS_WAKEUP_WORD_UNKNOWN;

    for (uint32_t i = 0; i < count; i++) {
        if (words[i] == TKL_KWS_WAKEUP_NIHAO_TUYA) {
            /* TODO: replace 0 with the real token id once the training side
             * provides the token/word table for the embedded nhty model. */
            int token_ids[1] = {0};
            if (keyword_tflite_add_keyword(s_tal_kws_ctx, "nhty", token_ids, 1, 0.30f) == 0) {
                s_tal_kws_word = TKL_KWS_WAKEUP_NIHAO_TUYA;
                printf("[TAL_KWS] keyword nhty registered (placeholder token)\n");
            } else {
                printf("[TAL_KWS] keyword registration failed\n");
            }
        }
    }

    return s_tal_kws_word == TKL_KWS_WAKEUP_WORD_UNKNOWN ? OPRT_NOT_SUPPORTED : OPRT_OK;
}

uint32_t tkl_kws_get_process_uint_size(void)
{
    return TAL_KWS_UNIT_BYTES;
}

TKL_KWS_WAKEUP_WORD_E tkl_kws_recognize_wakeup_word(uint8_t *data, uint32_t len)
{
    TAL_KWS_RESULT_T result;

    if (!s_tal_kws_ready || !s_tal_kws_running || data == NULL || len != TAL_KWS_UNIT_BYTES) {
        return TKL_KWS_WAKEUP_WORD_UNKNOWN;
    }

    memset(&result, 0, sizeof(result));
    if (keyword_tflite_forward_pcm(s_tal_kws_ctx, (const short *)data, (int)(len / 2u), &result) != 0) {
        return TKL_KWS_WAKEUP_WORD_UNKNOWN;
    }

    /* One unit is 20 ms, so this marks the inference cadence: a silent UART
     * then means the caller is not feeding or the engine is not running,
     * rather than "no keyword". */
    ++s_tal_kws_units;
    if (s_tal_kws_units <= 5u || (s_tal_kws_units % 500u) == 0u) {
        printf("[TAL_KWS] units=%u score=%d.%03d name=\"%s\"\n", (unsigned)s_tal_kws_units,
               (int)result.score, (int)((result.score < 0 ? -result.score : result.score) * 1000.0f) % 1000,
               result.name);
    }

    if (result.name[0] != 0 || result.score > 0.0f) {
        if (s_tal_kws_word != TKL_KWS_WAKEUP_WORD_UNKNOWN && s_tal_kws_cb != NULL) {
            s_tal_kws_cb(s_tal_kws_word);
        }
        keyword_tflite_reset(s_tal_kws_ctx);
        return s_tal_kws_word;
    }

    return TKL_KWS_WAKEUP_WORD_UNKNOWN;
}

OPERATE_RET tkl_kws_reg_wakeup_cb(TKL_KWS_WAKEUP_CB wakeup_cb)
{
    if (wakeup_cb == NULL) {
        return OPRT_INVALID_PARM;
    }
    s_tal_kws_cb = wakeup_cb;
    return OPRT_OK;
}

OPERATE_RET tkl_kws_enable(void)
{
    if (!s_tal_kws_ready) {
        return OPRT_RESOURCE_NOT_READY;
    }
    keyword_tflite_reset(s_tal_kws_ctx);
    s_tal_kws_units = 0;
    s_tal_kws_running = 1;
    return OPRT_OK;
}

OPERATE_RET tkl_kws_disable(void)
{
    s_tal_kws_running = 0;
    return OPRT_OK;
}

OPERATE_RET tkl_kws_deinit(void)
{
    if (s_tal_kws_ctx != NULL) {
        keyword_tflite_destroy(s_tal_kws_ctx);
        s_tal_kws_ctx = NULL;
    }
    if (s_tal_kws_mem != NULL) {
        free(s_tal_kws_mem);
        s_tal_kws_mem = NULL;
    }
    s_tal_kws_ready = 0;
    s_tal_kws_running = 0;
    s_tal_kws_word = TKL_KWS_WAKEUP_WORD_UNKNOWN;
    return OPRT_OK;
}

void tkl_jieli_kws_feed_capture(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
}

#else /* WL83, or WL82 built without WAKEUP/FREE: no vendor KWS engine */

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

void tkl_jieli_kws_feed_capture(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
}

#endif /* JIELI_SELECTED_CHIP_WL82 */
