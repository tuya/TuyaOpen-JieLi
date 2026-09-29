#ifndef TKL_JIELI_AUDIO_BACKEND_H
#define TKL_JIELI_AUDIO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "tkl_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Normalized PCM contract: 16 kHz, signed 16-bit, mono. Native SDK request
 * structures must remain private to a per-SoC provider. */
typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
} TKL_JIELI_AUDIO_PCM_CONFIG_T;

typedef void (*TKL_JIELI_AUDIO_CAPTURE_CB)(void *cookie, const uint8_t *data, size_t size, uint64_t pts);
typedef uintptr_t TKL_JIELI_AUDIO_CRITICAL_STATE_T;

typedef struct {
    OPERATE_RET (*ai_init)(void *backend, const TKL_JIELI_AUDIO_PCM_CONFIG_T *config,
                           TKL_JIELI_AUDIO_CAPTURE_CB callback, void *cookie);
    OPERATE_RET (*ai_start)(void *backend);
    /* Stop native capture and close the provider callback gate. If callbacks
     * are still known in flight, return an error so TKL can retry. The public
     * JieLi audio_server API does not document a late-callback barrier, so a
     * provider must keep callback state static and drop callbacks after stop. */
    OPERATE_RET (*ai_stop)(void *backend);
    OPERATE_RET (*ai_uninit)(void *backend);
    OPERATE_RET (*ai_set_volume)(void *backend, int32_t volume);
    OPERATE_RET (*ai_get_volume)(void *backend, int32_t *volume);
    OPERATE_RET (*ao_init)(void *backend, const TKL_JIELI_AUDIO_PCM_CONFIG_T *config, void **stream);
    OPERATE_RET (*ao_start)(void *backend, void *stream);
    OPERATE_RET (*ao_stop)(void *backend, void *stream);
    OPERATE_RET (*ao_uninit)(void *backend, void *stream);
    OPERATE_RET (*ao_set_volume)(void *backend, void *stream, int32_t volume);
    OPERATE_RET (*ao_get_volume)(void *backend, void *stream, int32_t *volume);
    /* Must copy into a finite queue and return promptly; full queues return
     * OPRT_BUFFER_NOT_ENOUGH. It must never retain the caller's PCM pointer. */
    OPERATE_RET (*ao_write)(void *backend, void *stream, const uint8_t *data, size_t size);
    OPERATE_RET (*ao_flush)(void *backend, void *stream);
    /* Short, non-blocking critical section used for callback gate/state changes.
     * Providers typically map this to interrupt-state save/restore or an OS lock. */
    TKL_JIELI_AUDIO_CRITICAL_STATE_T (*critical_enter)(void *backend);
    void (*critical_exit)(void *backend, TKL_JIELI_AUDIO_CRITICAL_STATE_T state);
} TKL_JIELI_AUDIO_BACKEND_OPS_T;

/* Install one provider before either direction is initialized. Passing NULL
 * removes it only while both directions are uninitialized. TKL lifecycle calls
 * are externally serialized. A capture callback must not reenter stop/uninit.
 * Provider stop reports callbacks observed in flight; late SDK callbacks must
 * be gated before they can reach the Tuya callback. Critical sections protect
 * shared state and must not call a provider, VAD, or Tuya callback while held. */
OPERATE_RET tkl_jieli_audio_backend_install(const TKL_JIELI_AUDIO_BACKEND_OPS_T *ops, void *backend);

#ifdef __cplusplus
}
#endif

#endif
