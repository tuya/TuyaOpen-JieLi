#ifndef TKL_JIELI_AUDIO_SERVER_NATIVE_H
#define TKL_JIELI_AUDIO_SERVER_NATIVE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
} JIELI_AUDIO_NATIVE_PCM_CONFIG_T;

typedef void (*JIELI_AUDIO_NATIVE_CAPTURE_CB)(void *cookie, const uint8_t *data, size_t size, uint64_t pts);

/* Native operations return 0 on success and a negative private error code. */
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
/* Stop accepting frames and wait until queued PCM and the DAC FIFO have drained. */
int jieli_audio_native_ao_flush(void *stream);
uintptr_t jieli_audio_native_critical_enter(void);
void jieli_audio_native_critical_exit(uintptr_t state);

#endif /* TKL_JIELI_AUDIO_SERVER_NATIVE_H */
