/*
 * Compat declarations for the user-supplied TAL audio_subsys archive.
 *
 * The archive is pi32v2 LLVM bitcode. Its shipped headers describe a
 * high-level audio_subsys_manager API that the archive does not export; the
 * engines it does export are keyword_tflite_* and rnn_vad_*. The layouts
 * below are the ones the existing probe linked against (see
 * docs/plans/2026-11-12-jieli-followup/reports/02-report.md); they are kept
 * here so the TKL adapter has one place to update when a matching header is
 * available.
 */
#ifndef __TAL_AUDIO_ENGINE_COMPAT_H__
#define __TAL_AUDIO_ENGINE_COMPAT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   num_bins;
    int   sample_rate;
    int   arena_size;
    int   reserved3;
    int   num_mfcc;
    float preemph;
    int   reserved6;
    int   reserved7;
    int   reserved8;
    int   reserved9;
    int   reserved10;
    int   reserved11;
    int   model_type;
    int   reserved13;
} TAL_KWS_CONFIG_T;

/* Layout mirrors the archive's keyword_tflite_result_t exactly
 * ({ char name[64]; int; int; float }, 76 bytes, verified from the bitcode).
 * An extra field here shifts score into padding and it always reads 0.0. */
typedef struct {
    char  name[64];
    int   index;
    int   start_frame;
    float score;
} TAL_KWS_RESULT_T;

void keyword_tflite_config_init(TAL_KWS_CONFIG_T *config);
int  keyword_tflite_get_mem_size(TAL_KWS_CONFIG_T *config);
int  keyword_tflite_create(const char *model_name, TAL_KWS_CONFIG_T *config, void *mem, int mem_size, void **ctx);
int  keyword_tflite_add_keyword(void *ctx, const char *text, int *token_ids, int count, float threshold);
/* Returns 1 when a keyword was detected and *result was filled, 0 when the
 * unit produced no detection, -1 on bad arguments. */
int  keyword_tflite_forward_pcm(void *ctx, const short *pcm, int samples, TAL_KWS_RESULT_T *result);
int  keyword_tflite_reset(void *ctx);
void keyword_tflite_destroy(void *ctx);

typedef struct rnn_vad_param_in {
    float min_speech_len;      /* ms, default 200 */
    float max_speech_interval; /* ms, default 1000 */
} RNN_VAD_PARAM_T;

void  *rnn_vad_create(void);
int    rnn_vad_init(RNN_VAD_PARAM_T *param_in, void *obj);
float  rnn_vad_process(void *obj, short *x);
void   rnn_vad_destroy(void *obj);
void   rnn_vad_start(void *obj);
void   rnn_vad_stop(void *obj);
void   rnn_vad_set_callback(void *obj, float threshold);

#ifdef __cplusplus
}
#endif

#endif /* __TAL_AUDIO_ENGINE_COMPAT_H__ */
