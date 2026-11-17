#ifndef AES_H_
#define AES_H_

/*
demo:
#include "speexdsp/audio_subsys_speexdsp_wrap2.h"
static void *speex_aec_handle;
static int tal_audio_subsys_vadflag;
#include "speexdsp/audio_subsys_rnn_vad.h"
static void *rnn_vad_handle;

speex_aec_handle = speex_aes_create(aec_info_pr->samp_rate_points);
rnn_vad_handle = rnn_vad_create();

speex_aes_process(speex_aec_handle, (short *)aec_info_pr->mic_addr, (short *)aec_info_pr->ref_addr, (short *)aec_info_pr->out_addr);
aec_vad_flag = rnn_vad_process(rnn_vad_handle, (short *)aec_info_pr->out_addr);

speex_get_param(speex_aec_handle, NULL, linearaec);
int tuya_wakeupword_feed_with_vad(uint8_t *data, uint16_t datalen, uint8_t vadflag);
tuya_wakeupword_feed_with_vad(linearaec, handle_len, asr_vad_flag);

*/

#ifdef __cplusplus
extern "C" {
#endif

/*framesize = 20ms*/
void *speex_aes_create(int framesize);

int speex_aes_process(void *obj, short *mic, short *ref, short *aec);

int speex_aes_set_param(void *obj, int level);
int speex_ns_set_param(void *obj, int level1, int level2);

float speex_get_param(void *obj, float *out, short *linearout);

void speex_aes_destory(void *obj);

#ifdef __cplusplus
}
#endif

#endif
