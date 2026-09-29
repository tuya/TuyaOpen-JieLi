#include "driver/tkl_jieli_audio_backend.h"

#include "driver/tkl_jieli_audio_server_native.h"
#include "tuya_error_code.h"

static OPERATE_RET __map_native_result(int result)
{
    if (result == 0) return OPRT_OK;
    if (result == -2) return OPRT_INVALID_PARM;
    if (result == -3) return OPRT_NOT_SUPPORTED;
    if (result == -4) return OPRT_RESOURCE_NOT_READY;
    if (result == -31) return OPRT_BUFFER_NOT_ENOUGH;
    if (result == OPRT_TIMEOUT) return OPRT_TIMEOUT;
    return OPRT_COM_ERROR;
}

static JIELI_AUDIO_NATIVE_PCM_CONFIG_T __native_config(const TKL_JIELI_AUDIO_PCM_CONFIG_T *config)
{
    JIELI_AUDIO_NATIVE_PCM_CONFIG_T native_config;
    native_config.sample_rate = config->sample_rate;
    native_config.bits_per_sample = config->bits_per_sample;
    native_config.channels = config->channels;
    return native_config;
}

static OPERATE_RET __ai_init(void *backend, const TKL_JIELI_AUDIO_PCM_CONFIG_T *config,
                             TKL_JIELI_AUDIO_CAPTURE_CB callback, void *cookie)
{
    JIELI_AUDIO_NATIVE_PCM_CONFIG_T native_config;
    (void)backend;
    if (config == NULL) return OPRT_INVALID_PARM;
    native_config = __native_config(config);
    return __map_native_result(jieli_audio_native_ai_init(&native_config, callback, cookie));
}

static OPERATE_RET __ai_start(void *backend)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ai_start());
}

static OPERATE_RET __ai_stop(void *backend)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ai_stop());
}

static OPERATE_RET __ai_uninit(void *backend)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ai_uninit());
}

static OPERATE_RET __ai_set_volume(void *backend, int32_t volume)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ai_set_volume(volume));
}

static OPERATE_RET __ai_get_volume(void *backend, int32_t *volume)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ai_get_volume(volume));
}

static OPERATE_RET __ao_init(void *backend, const TKL_JIELI_AUDIO_PCM_CONFIG_T *config, void **stream)
{
    JIELI_AUDIO_NATIVE_PCM_CONFIG_T native_config;
    (void)backend;
    if (config == NULL) return OPRT_INVALID_PARM;
    native_config = __native_config(config);
    return __map_native_result(jieli_audio_native_ao_init(&native_config, stream));
}

static OPERATE_RET __ao_start(void *backend, void *stream)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ao_start(stream));
}

static OPERATE_RET __ao_stop(void *backend, void *stream)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ao_stop(stream));
}

static OPERATE_RET __ao_uninit(void *backend, void *stream)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ao_uninit(stream));
}

static OPERATE_RET __ao_set_volume(void *backend, void *stream, int32_t volume)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ao_set_volume(stream, volume));
}

static OPERATE_RET __ao_get_volume(void *backend, void *stream, int32_t *volume)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ao_get_volume(stream, volume));
}

static OPERATE_RET __ao_write(void *backend, void *stream, const uint8_t *data, size_t size)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ao_write(stream, data, size));
}

static OPERATE_RET __ao_flush(void *backend, void *stream)
{
    (void)backend;
    return __map_native_result(jieli_audio_native_ao_flush(stream));
}

static TKL_JIELI_AUDIO_CRITICAL_STATE_T __critical_enter(void *backend)
{
    (void)backend;
    return (TKL_JIELI_AUDIO_CRITICAL_STATE_T)jieli_audio_native_critical_enter();
}

static void __critical_exit(void *backend, TKL_JIELI_AUDIO_CRITICAL_STATE_T state)
{
    (void)backend;
    jieli_audio_native_critical_exit((uintptr_t)state);
}

static const TKL_JIELI_AUDIO_BACKEND_OPS_T s_audio_ops = {
    .ai_init = __ai_init,
    .ai_start = __ai_start,
    .ai_stop = __ai_stop,
    .ai_uninit = __ai_uninit,
    .ai_set_volume = __ai_set_volume,
    .ai_get_volume = __ai_get_volume,
    .ao_init = __ao_init,
    .ao_start = __ao_start,
    .ao_stop = __ao_stop,
    .ao_uninit = __ao_uninit,
    .ao_set_volume = __ao_set_volume,
    .ao_get_volume = __ao_get_volume,
    .ao_write = __ao_write,
    .ao_flush = __ao_flush,
    .critical_enter = __critical_enter,
    .critical_exit = __critical_exit,
};

OPERATE_RET tkl_jieli_audio_server_install(void)
{
    if (jieli_audio_native_prepare() != 0) return OPRT_COM_ERROR;
    return tkl_jieli_audio_backend_install(&s_audio_ops, NULL);
}
