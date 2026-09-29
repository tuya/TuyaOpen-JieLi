#include "driver/tkl_jieli_audio_server_native.h"

#include <limits.h>
#include <string.h>

#include "system/includes.h"
#include "system/os/os_api.h"
#include "media/includes.h"
#include "audio_adc.h"
#include "audio_dac.h"
#include "cpu/wl83/audio_config.h"
#include "adc_file.h"
#include "tuya_error_code.h"

#ifndef JIELI_AUDIO_MIC_CHANNEL_MAP
#define JIELI_AUDIO_MIC_CHANNEL_MAP AUDIO_ADC_MIC_CH
#endif

#define WL83_CAPTURE_DMA_POINTS       320u
#define WL83_CAPTURE_DMA_BUFFER_COUNT 2u
#define WL83_CAPTURE_QUEUE_SIZE       (8u * 1024u)
#define WL83_CAPTURE_FRAME_SIZE       (WL83_CAPTURE_DMA_POINTS * sizeof(s16))
#define WL83_CAPTURE_CHANNEL_LIMIT    AUDIO_ADC_MAX_NUM
#define WL83_CAPTURE_TASK_NAME        "tuya_audio_capture"
#define WL83_CAPTURE_TASK_STACK       768
#define WL83_CAPTURE_TASK_PRIORITY    12
#define WL83_PLAY_QUEUE_SIZE          (16u * 1024u)
#define WL83_PLAY_TASK_NAME           "tuya_audio_playback"
#define WL83_PLAY_TASK_STACK          768
#define WL83_PLAY_TASK_PRIORITY       12
#define WL83_PLAY_WRITER_DRAIN_LIMIT  100u
#define WL83_PLAY_DRAIN_LIMIT         500u
/* JieLi's digital volume curve has 0-31 levels (DEFAULT_DIGITAL_VOL_MAX). */
#define WL83_PLAY_VOLUME_LEVELS       31u

extern struct audio_adc_hdl adc_hdl;
extern struct audio_dac_hdl dac_hdl;
extern const int config_adc_async_en;
extern const struct adc_platform_cfg adc_platform_cfg_table[AUDIO_ADC_MAX_NUM];
/* Declared the same way audio/common/audio_volume_mixer.c does; the symbol
 * lives in the EQ library rather than a header this file already pulls in. */
extern float eq_db2mag(float x);

typedef struct {
    struct adc_mic_ch mic;
    struct audio_adc_output_hdl output;
    s16 *dma_buffer;
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;
    uint32_t sample_rate;
    uint32_t channel_map;
    int volume;
    uint32_t callbacks_inflight;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t used;
    uint32_t dropped_frames;
    uint8_t channel_count;
    uint8_t channel_index;
    uint8_t initialized;
    uint8_t resources_opened;
    uint8_t mic_opened;
    uint8_t dma_attached;
    uint8_t output_registered;
    uint8_t running;
} WL83_CAPTURE_STATE_T;

typedef struct {
    struct audio_dac_hdl *dac;
    uint32_t sample_rate;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t used;
    int volume;
    int write_error;
    uint8_t writer_inflight;
    uint8_t initialized;
    uint8_t running;
    uint8_t accepting_writes;
    uint8_t draining;
    uint8_t channel_open;
    uint8_t writer_output_logged;
    uint32_t queued_total;
    uint32_t written_total;
} WL83_PLAY_STATE_T;

static OS_SEM s_capture_sem;
static uint8_t s_capture_sem_ready;
static uint8_t s_capture_task_ready;
static OS_SEM s_play_sem;
static uint8_t s_play_sem_ready;
static uint8_t s_play_task_ready;
static OS_MUTEX s_play_lock;
static uint8_t s_play_lock_ready;
static WL83_CAPTURE_STATE_T s_capture;
static WL83_PLAY_STATE_T s_play;
static uint8_t s_capture_queue[WL83_CAPTURE_QUEUE_SIZE];
static uint8_t s_play_queue[WL83_PLAY_QUEUE_SIZE];

uintptr_t jieli_audio_native_critical_enter(void)
{
    media_irq_disable();
    return 1;
}

void jieli_audio_native_critical_exit(uintptr_t state)
{
    (void)state;
    media_irq_enable();
}

static void __capture_queue_write(const uint8_t *data, uint32_t size)
{
    uint32_t first;
    uint8_t queued = 0;

    if (data == NULL || size == 0 || size > WL83_CAPTURE_QUEUE_SIZE) {
        return;
    }

    media_irq_disable();
    if (s_capture.running && size <= WL83_CAPTURE_QUEUE_SIZE - s_capture.used) {
        first = WL83_CAPTURE_QUEUE_SIZE - s_capture.write_pos;
        if (first > size) {
            first = size;
        }
        memcpy(s_capture_queue + s_capture.write_pos, data, first);
        if (size > first) {
            memcpy(s_capture_queue, data + first, size - first);
        }
        s_capture.write_pos = (s_capture.write_pos + size) % WL83_CAPTURE_QUEUE_SIZE;
        s_capture.used += size;
        queued = 1;
    } else if (s_capture.running) {
        ++s_capture.dropped_frames;
    }
    media_irq_enable();

    if (queued && s_capture_sem_ready) {
        (void)os_sem_post(&s_capture_sem);
    }
}

static void __capture_adc_output(void *priv, s16 *data, int len)
{
    WL83_CAPTURE_STATE_T *state = (WL83_CAPTURE_STATE_T *)priv;
    s16 mono[WL83_CAPTURE_DMA_POINTS];
    uint32_t sample_count;
    uint32_t frames;
    uint32_t frame;

    if (state == NULL || data == NULL || len <= 0 || (len & 1) || !state->running) {
        return;
    }

    sample_count = (uint32_t)len / sizeof(s16);
    if (state->channel_count <= 1) {
        __capture_queue_write((const uint8_t *)data, (uint32_t)len);
        return;
    }

    /* WL83's ADC callback len is the byte count for one channel. With async
     * ADC enabled, data contains interleaved samples for all ADC channels,
     * so len / sizeof(s16) is already the number of output mono frames. */
    frames = sample_count;
    if (frames > WL83_CAPTURE_DMA_POINTS) {
        frames = WL83_CAPTURE_DMA_POINTS;
    }
    for (frame = 0; frame < frames; ++frame) {
        if ((state->channel_map & (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) ==
            (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) {
            int32_t mixed = (int32_t)data[frame * state->channel_count] +
                            (int32_t)data[frame * state->channel_count + 1u];
            mono[frame] = (s16)(mixed / 2);
        } else {
            mono[frame] = data[frame * state->channel_count + state->channel_index];
        }
    }
    if (frames != 0) {
        __capture_queue_write((const uint8_t *)mono, frames * sizeof(s16));
    }
}

static void __capture_dispatch_task(void *arg)
{
    uint8_t frame[WL83_CAPTURE_FRAME_SIZE];
    (void)arg;

    for (;;) {
        (void)os_sem_pend(&s_capture_sem, 0);

        for (;;) {
            JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
            void *cookie;
            uint32_t first;
            uint8_t dispatch = 0;

            media_irq_disable();
            if (!s_capture.running || s_capture.used < sizeof(frame) || s_capture.callback == NULL) {
                media_irq_enable();
                break;
            }

            first = WL83_CAPTURE_QUEUE_SIZE - s_capture.read_pos;
            if (first > sizeof(frame)) {
                first = sizeof(frame);
            }
            memcpy(frame, s_capture_queue + s_capture.read_pos, first);
            if (sizeof(frame) > first) {
                memcpy(frame + first, s_capture_queue, sizeof(frame) - first);
            }
            s_capture.read_pos = (s_capture.read_pos + sizeof(frame)) % WL83_CAPTURE_QUEUE_SIZE;
            s_capture.used -= sizeof(frame);
            ++s_capture.callbacks_inflight;
            callback = s_capture.callback;
            cookie = s_capture.cookie;
            dispatch = 1;
            media_irq_enable();

            if (dispatch) {
                callback(cookie, frame, sizeof(frame), 0);
                media_irq_disable();
                --s_capture.callbacks_inflight;
                media_irq_enable();
            }
        }
    }
}

static void __playback_write_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)os_sem_pend(&s_play_sem, 0);
        for (;;) {
            const uint8_t *data;
            struct audio_dac_hdl *dac;
            uint32_t contiguous;
            int running;
            int ret;

            (void)os_mutex_pend(&s_play_lock, 0);
            running = s_play.running;
            /* Flush seals producers but lets the worker drain accepted PCM. */
            if (!running || (!s_play.accepting_writes && !s_play.draining) || s_play.used == 0 ||
                s_play.dac == NULL) {
                (void)os_mutex_post(&s_play_lock);
                break;
            }
            contiguous = WL83_PLAY_QUEUE_SIZE - s_play.read_pos;
            if (contiguous > s_play.used) {
                contiguous = s_play.used;
            }
            data = s_play_queue + s_play.read_pos;
            dac = s_play.dac;
            s_play.writer_inflight = 1;
            (void)os_mutex_post(&s_play_lock);

            ret = audio_dac_write(dac, (void *)data, (int)contiguous);
            if (ret > 0) {
                if (!s_play.writer_output_logged) {
                    s_play.writer_output_logged = 1;
                    printf("[JIELI_AUDIO_WL83] DAC write accepted: requested=%u accepted=%d\n",
                           (unsigned)contiguous, ret);
                }
                if ((uint32_t)ret > contiguous) {
                    ret = (int)contiguous;
                }
                (void)os_mutex_pend(&s_play_lock, 0);
                s_play.read_pos = (s_play.read_pos + (uint32_t)ret) % WL83_PLAY_QUEUE_SIZE;
                s_play.used -= (uint32_t)ret;
                s_play.written_total += (uint32_t)ret;
                s_play.writer_inflight = 0;
                (void)os_mutex_post(&s_play_lock);
                continue;
            }
            if (ret == 0) {
                (void)os_mutex_pend(&s_play_lock, 0);
                s_play.writer_inflight = 0;
                (void)os_mutex_post(&s_play_lock);
                os_time_dly(1);
                continue;
            }

            (void)os_mutex_pend(&s_play_lock, 0);
            s_play.write_error = ret;
            s_play.read_pos = s_play.write_pos;
            s_play.used = 0;
            s_play.writer_inflight = 0;
            (void)os_mutex_post(&s_play_lock);
            printf("[JIELI_AUDIO_WL83] DAC writer failed: %d\n", ret);
            break;
        }
    }
}

int jieli_audio_native_prepare(void)
{
    int ret;

    if (!s_capture_sem_ready) {
        ret = os_sem_create(&s_capture_sem, 0);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] capture semaphore creation failed: %d\n", ret);
            return -1;
        }
        s_capture_sem_ready = 1;
    }
    if (!s_capture_task_ready) {
        ret = os_task_create(__capture_dispatch_task, NULL, WL83_CAPTURE_TASK_PRIORITY,
                             WL83_CAPTURE_TASK_STACK, 64, WL83_CAPTURE_TASK_NAME);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] capture task creation failed: %d\n", ret);
            return -1;
        }
        s_capture_task_ready = 1;
    }
    if (!s_play_sem_ready) {
        ret = os_sem_create(&s_play_sem, 0);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] playback semaphore creation failed: %d\n", ret);
            return -1;
        }
        s_play_sem_ready = 1;
    }
    if (!s_play_lock_ready) {
        ret = os_mutex_create(&s_play_lock);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] playback mutex creation failed: %d\n", ret);
            return -1;
        }
        s_play_lock_ready = 1;
    }
    if (!s_play_task_ready) {
        ret = os_task_create(__playback_write_task, NULL, WL83_PLAY_TASK_PRIORITY,
                             WL83_PLAY_TASK_STACK, 64, WL83_PLAY_TASK_NAME);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] playback task creation failed: %d\n", ret);
            return -1;
        }
        s_play_task_ready = 1;
    }

    printf("[JIELI_AUDIO_WL83] native ADC/DAC backend ready: adc_bits=%u dac=%p\n",
           (unsigned)adc_hdl.bit_width, (void *)&dac_hdl);
    return 0;
}

static int __capture_resources_close(void)
{
    uint32_t retries = 0;
    uint32_t callbacks_inflight;
    int ret;

    if (s_capture.output_registered) {
        audio_adc_del_output_handler(&adc_hdl, &s_capture.output);
        s_capture.output_registered = 0;
    }
    if (s_capture.mic_opened) {
        ret = audio_adc_mic_close(&s_capture.mic);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] audio_adc_mic_close failed: %d\n", ret);
        } else {
            s_capture.mic_opened = 0;
        }
    } else {
        ret = 0;
    }
    if (ret == 0) {
        if (s_capture.dma_buffer != NULL && !s_capture.dma_attached) {
            AUD_ADC_DMA_FREE(s_capture.dma_buffer);
        }
        s_capture.dma_buffer = NULL; /* audio_adc_mic_close releases attached DMA buffers */
        s_capture.dma_attached = 0;
        s_capture.resources_opened = 0;
    }

    do {
        media_irq_disable();
        callbacks_inflight = s_capture.callbacks_inflight;
        media_irq_enable();
        if (callbacks_inflight == 0) {
            return ret == 0 ? 0 : -1;
        }
        os_time_dly(1);
    } while (retries++ < 100u);

    printf("[JIELI_AUDIO_WL83] capture callbacks still in flight: %u\n",
           (unsigned)callbacks_inflight);
    return -1;
}

static int __capture_resources_open(void)
{
    const uint32_t channel_map = JIELI_AUDIO_MIC_CHANNEL_MAP;
    uint32_t channel;
    uint32_t dma_size;
    int ret;

    if (s_capture.resources_opened) {
        return 0;
    }
    audio_adc_file_init();
    printf("[JIELI_AUDIO_WL83] ADC capture open: map=0x%x board_mics=%u all_channels=%u async=%d vm_map=0x%x\n",
           (unsigned)channel_map, (unsigned)JIELI_AUDIO_MIC_CHANNEL_COUNT,
           (unsigned)JIELI_AUDIO_ADC_ALL_CHANNEL_OPEN, config_adc_async_en,
           (unsigned)audio_adc_file_get_mic_en_map());
    /* adc_file_mic_open() takes a channel bitmask and walks its set bits itself,
     * so this loop mirrors what it does internally; keeping it separate lets a
     * failing onboard input be logged before the vendor ADC asserts. The vendor
     * opens mics the same way (audio/jl_kws/jl_kws_audio.c, voice_mic_data.c). */
    for (channel = 0; channel < AUDIO_ADC_MAX_NUM; ++channel) {
        const uint32_t channel_bit = AUDIO_ADC_MIC(channel);
        if ((channel_map & channel_bit) == 0) {
            continue;
        }
        printf("[JIELI_AUDIO_WL83] opening ADC MIC%u: bit=0x%x mode=%u ain=%u bias=%u bias_rsel=%u dcc=%u inside_bias=%u power_io=%u dmic=%u\n",
               (unsigned)channel, (unsigned)channel_bit,
               (unsigned)adc_platform_cfg_table[channel].mic_mode,
               (unsigned)adc_platform_cfg_table[channel].mic_ain_sel,
               (unsigned)adc_platform_cfg_table[channel].mic_bias_sel,
               (unsigned)adc_platform_cfg_table[channel].mic_bias_rsel,
               (unsigned)adc_platform_cfg_table[channel].mic_dcc,
               (unsigned)adc_platform_cfg_table[channel].inside_bias_resistor,
               (unsigned)adc_platform_cfg_table[channel].power_io,
               (unsigned)adc_platform_cfg_table[channel].dmic_enable);
        ret = adc_file_mic_open(&s_capture.mic, (int)channel_bit);
        if (ret != 0) {
            printf("[JIELI_AUDIO_WL83] adc_file_mic_open failed: %d MIC%u map=0x%x\n",
                   ret, (unsigned)channel, (unsigned)channel_bit);
            return -1;
        }
        printf("[JIELI_AUDIO_WL83] ADC MIC%u open returned: %d\n",
               (unsigned)channel, ret);
    }
    s_capture.mic_opened = 1;

    ret = audio_adc_mic_set_sample_rate(&s_capture.mic, (int)s_capture.sample_rate);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] ADC sample-rate setup failed: %d\n", ret);
        goto __error;
    }

    s_capture.channel_count = config_adc_async_en ? WL83_CAPTURE_CHANNEL_LIMIT : 1;
    if (s_capture.channel_count == 0 || s_capture.channel_count > WL83_CAPTURE_CHANNEL_LIMIT) {
        printf("[JIELI_AUDIO_WL83] invalid ADC channel count: %u\n",
               (unsigned)s_capture.channel_count);
        goto __error;
    }
    s_capture.channel_map = channel_map;
    if ((channel_map & (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) ==
        (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) {
        s_capture.channel_index = 0;
    } else {
        s_capture.channel_index = (uint8_t)get_adc_seq(&adc_hdl, (u16)channel_map);
    }
    if (s_capture.channel_index >= s_capture.channel_count) {
        printf("[JIELI_AUDIO_WL83] invalid ADC sequence: index=%u channels=%u\n",
               (unsigned)s_capture.channel_index, (unsigned)s_capture.channel_count);
        goto __error;
    }

    dma_size = WL83_CAPTURE_DMA_POINTS * sizeof(s16) * WL83_CAPTURE_DMA_BUFFER_COUNT *
               s_capture.channel_count;
    s_capture.dma_buffer = (s16 *)AUD_ADC_DMA_MALLOC(dma_size);
    if (s_capture.dma_buffer == NULL) {
        printf("[JIELI_AUDIO_WL83] ADC DMA allocation failed: bytes=%u\n", (unsigned)dma_size);
        goto __error;
    }
    ret = audio_adc_mic_set_buffs(&s_capture.mic, s_capture.dma_buffer,
                                  WL83_CAPTURE_DMA_POINTS * 2u,
                                  WL83_CAPTURE_DMA_BUFFER_COUNT);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] ADC DMA setup failed: %d\n", ret);
        AUD_ADC_DMA_FREE(s_capture.dma_buffer);
        s_capture.dma_buffer = NULL;
        goto __error;
    }
    s_capture.dma_attached = 1;

    ret = audio_adc_mic_set_gain(&s_capture.mic, channel_map,
                                 (s_capture.volume * 19 + 50) / 100);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] ADC gain restore failed: %d\n", ret);
        goto __error;
    }

    s_capture.output.priv = &s_capture;
    s_capture.output.handler = __capture_adc_output;
    audio_adc_add_output_handler(&adc_hdl, &s_capture.output);
    s_capture.output_registered = 1;
    s_capture.resources_opened = 1;
    printf("[JIELI_AUDIO_WL83] onboard MIC1/MIC2 configured: map=0x%x adc_channels=%u sequence=%u mix=%s rate=%u dma=%u frame=%u\n",
           (unsigned)channel_map, (unsigned)s_capture.channel_count,
           (unsigned)s_capture.channel_index,
           (channel_map & (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1)) ==
                   (AUDIO_ADC_MIC_0 | AUDIO_ADC_MIC_1) ? "average" : "single",
           (unsigned)s_capture.sample_rate,
           (unsigned)dma_size, (unsigned)WL83_CAPTURE_DMA_POINTS);
    return 0;

__error:
    (void)__capture_resources_close();
    return -1;
}

int jieli_audio_native_ai_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config,
                               JIELI_AUDIO_NATIVE_CAPTURE_CB callback, void *cookie)
{
    if (config == NULL || callback == NULL) {
        return -2;
    }
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO_WL83] unsupported capture format: rate=%u bits=%u channels=%u\n",
               (unsigned)config->sample_rate, (unsigned)config->bits_per_sample,
               (unsigned)config->channels);
        return -3;
    }
    if (s_capture.initialized) {
        return -4;
    }
    if (!s_capture_task_ready || adc_hdl.bit_width != DATA_BIT_WIDE_16BIT) {
        printf("[JIELI_AUDIO_WL83] ADC runtime not initialized: task=%u adc_bits=%u\n",
               (unsigned)s_capture_task_ready, (unsigned)adc_hdl.bit_width);
        return -1;
    }

    memset(&s_capture, 0, sizeof(s_capture));
    s_capture.callback = callback;
    s_capture.cookie = cookie;
    s_capture.sample_rate = config->sample_rate;
    /* The onboard mic needs the top of the ADC gain range. At the previous
     * default of 50 this maps to gain 10 (+12 dB), which left speech peaks
     * around 540 of 32767 - buried in the noise floor, so the loopback played
     * back silence. 100 maps to gain 19 (+30 dB), the top of
     * audio_adc_mic_set_gain()'s documented 0(-8dB)~19(30dB) range, and
     * measured speech peaks of ~6600 (-14 dBFS). */
    s_capture.volume = 100;
    s_capture.initialized = 1;
    if (__capture_resources_open() != 0) {
        memset(&s_capture, 0, sizeof(s_capture));
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_start(void)
{
    int ret;

    if (!s_capture.initialized) {
        return -4;
    }
    if (s_capture.running) {
        return 0;
    }
    if (!s_capture.resources_opened && __capture_resources_open() != 0) {
        return -1;
    }
    media_irq_disable();
    s_capture.running = 1;
    s_capture.read_pos = s_capture.write_pos = s_capture.used = 0;
    media_irq_enable();

    ret = audio_adc_mic_start(&s_capture.mic);
    if (ret != 0) {
        media_irq_disable();
        s_capture.running = 0;
        media_irq_enable();
        printf("[JIELI_AUDIO_WL83] audio_adc_mic_start failed: %d\n", ret);
        (void)__capture_resources_close();
        return -1;
    }

    printf("[JIELI_AUDIO_WL83] onboard MIC capture started\n");
    return 0;
}

int jieli_audio_native_ai_stop(void)
{
    if (!s_capture.initialized) {
        return 0;
    }
    media_irq_disable();
    s_capture.running = 0;
    s_capture.used = 0;
    s_capture.read_pos = s_capture.write_pos = 0;
    media_irq_enable();

    if (__capture_resources_close() != 0) {
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_uninit(void)
{
    if (!s_capture.initialized) {
        return 0;
    }
    if (jieli_audio_native_ai_stop() != 0) {
        return -1;
    }
    memset(&s_capture, 0, sizeof(s_capture));
    return 0;
}

int jieli_audio_native_ai_set_volume(int volume)
{
    int gain;
    int ret;
    if (volume < 0 || volume > 100) {
        return -2;
    }
    if (!s_capture.initialized) {
        return -4;
    }
    s_capture.volume = volume;
    if (!s_capture.resources_opened) {
        return 0;
    }
    gain = (volume * 19 + 50) / 100;
    ret = audio_adc_mic_set_gain(&s_capture.mic, s_capture.channel_map, gain);
    return ret == 0 ? 0 : -1;
}

int jieli_audio_native_ai_get_volume(int *volume)
{
    if (volume == NULL) {
        return -2;
    }
    if (!s_capture.initialized) {
        return -4;
    }
    *volume = s_capture.volume;
    return 0;
}

int jieli_audio_native_ao_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config, void **stream)
{
    int ret;
    if (config == NULL || stream == NULL) {
        return -2;
    }
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO_WL83] unsupported playback format: rate=%u bits=%u channels=%u\n",
               (unsigned)config->sample_rate, (unsigned)config->bits_per_sample,
               (unsigned)config->channels);
        return -3;
    }
    if (s_play.initialized) {
        return -4;
    }
    s_play.dac = &dac_hdl;
    if (!s_play_task_ready || s_play.dac == NULL || s_play.dac->pd == NULL) {
        printf("[JIELI_AUDIO_WL83] DAC runtime not initialized\n");
        return -1;
    }
    ret = audio_dac_set_sample_rate(s_play.dac, (int)config->sample_rate);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] DAC sample-rate setup failed: %d\n", ret);
        s_play.dac = NULL;
        return -1;
    }
    s_play.sample_rate = config->sample_rate;
    s_play.volume = 50;

    /* Enter a JieLi audio state before the DAC channel starts.
     *
     * Under SYS_VOL_TYPE == VOL_TYPE_DIGITAL the vendor fade handler
     * (audio_fade_in_fade_out in audio/common/audio_volume_mixer.c) ignores the
     * level handed to audio_dac_set_volume() and applies the mixer's own
     * analog_volume_l/r and digital_volume instead. Those are zero-initialized
     * and only ever filled in by app_audio_state_switch(), which every vendor
     * app calls when entering a playback state. digital_volume is Q14, so a
     * zero value means the DAC plays silence at every requested volume.
     *
     * TuyaOpen drives the DAC directly and never runs the vendor app, so this
     * switch has to happen here. A NULL dvol handle is safe: audio_digital_vol_set()
     * returns early for NULL. */
    app_audio_state_switch(APP_AUDIO_STATE_MUSIC,
                           app_audio_volume_max_query(AppVol_BT_MUSIC), NULL);

    s_play.read_pos = 0;
    s_play.write_pos = 0;
    s_play.used = 0;
    s_play.write_error = 0;
    s_play.writer_inflight = 0;
    s_play.queued_total = 0;
    s_play.written_total = 0;
    s_play.accepting_writes = 0;
    s_play.channel_open = 0;
    s_play.initialized = 1;
    s_play.running = 0;
    *stream = &s_play;
    printf("[JIELI_AUDIO_WL83] onboard SPK configured: rate=%u channel=0x%x\n",
           (unsigned)s_play.sample_rate, (unsigned)audio_dac_get_channel(s_play.dac));
    return 0;
}

/* Map the requested 0-100 level onto JieLi's digital volume curve and write it
 * straight to the DAC.
 *
 * audio_dac_set_volume() only records a level, and under
 * SYS_VOL_TYPE == VOL_TYPE_DIGITAL the fade handler that consumes it applies the
 * mixer's own digital_volume rather than the recorded value, so the recorded
 * level never reaches the hardware on its own. The curve itself is the vendor's
 * (audio/common/audio_dvol.c default_dig_vol_table): Q14, 1.5 dB per step, with
 * level 31 at 16384 == 0 dB.
 *
 * Must run after audio_dac_channel_start(): the fade handler fires at channel
 * start and would overwrite an earlier write. */
static void __play_apply_digital_volume(void)
{
    struct audio_dac_hdl *dac;
    u32 level;
    u32 gain;

    (void)os_mutex_pend(&s_play_lock, 0);
    dac = s_play.dac;
    level = ((u32)s_play.volume * WL83_PLAY_VOLUME_LEVELS + 50u) / 100u;
    (void)os_mutex_post(&s_play_lock);

    if (dac == NULL) {
        return;
    }
    if (level == 0u) {
        gain = 0u;
    } else {
        /* Every step below the unity level costs 1.5 dB. */
        gain = (u32)(16384.0f * eq_db2mag(((float)level - (float)WL83_PLAY_VOLUME_LEVELS) * 1.5f) + 0.5f);
    }
    /* audio_dac_set_RL_digital_vol() is declared in audio_dac.h but not exported
     * by media.a, so drive both channels the way the fade handler's 0x3 mask
     * does. */
    (void)audio_dac_set_L_digital_vol(dac, (u16)gain);
    (void)audio_dac_set_R_digital_vol(dac, (u16)gain);
}

int jieli_audio_native_ao_start(void *stream)
{
    int ret;
    if (stream != &s_play || !s_play.initialized || s_play.dac == NULL) {
        return -4;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    if (s_play.running) {
        uint8_t channel_open = s_play.channel_open;
        if (s_play.accepting_writes) {
            (void)os_mutex_post(&s_play_lock);
            return 0;
        }
        (void)os_mutex_post(&s_play_lock);

        /* A prior drain timeout seals producers but leaves the DAC running so
         * the writer can finish accepted PCM. Reopen the stream on next start. */
        if (!channel_open) {
            ret = audio_dac_start(s_play.dac);
            if (ret != 0) {
                printf("[JIELI_AUDIO_WL83] audio_dac_start retry failed: %d\n", ret);
                return -1;
            }
            audio_dac_channel_start(NULL);
        }
        (void)os_mutex_pend(&s_play_lock, 0);
        if (s_play.write_error != 0) {
            printf("[JIELI_AUDIO_WL83] clearing prior DAC writer error before restart: %d\n",
                   s_play.write_error);
            s_play.write_error = 0;
        }
        s_play.accepting_writes = 1;
        s_play.draining = 0;
        s_play.channel_open = 1;
        (void)os_mutex_post(&s_play_lock);
        __play_apply_digital_volume();
        if (s_play_sem_ready) {
            (void)os_sem_post(&s_play_sem);
        }
        return 0;
    }
    (void)os_mutex_post(&s_play_lock);
    ret = audio_dac_start(s_play.dac);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] audio_dac_start failed: %d\n", ret);
        return -1;
    }
    /* audio_dac_start powers the DAC core; the SDK's public playback sample
     * also starts the DAC FIFO channel before writing PCM. */
    audio_dac_channel_start(NULL);
    (void)os_mutex_pend(&s_play_lock, 0);
    s_play.running = 1;
    s_play.accepting_writes = 1;
    s_play.draining = 0;
    s_play.write_error = 0;
    s_play.channel_open = 1;
    (void)os_mutex_post(&s_play_lock);
    /* The fade handler has just run with the mixer's own gain; put the level
     * the caller asked for on top of it. */
    __play_apply_digital_volume();
    printf("[JIELI_AUDIO_WL83] onboard SPK playback started\n");
    return 0;
}

int jieli_audio_native_ao_stop(void *stream)
{
    uint32_t retries = 0;
    uint8_t writer_inflight;
    uint8_t channel_open;
    int ret;
    if (stream != &s_play || !s_play.initialized || s_play.dac == NULL) {
        return -4;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    if (!s_play.running) {
        (void)os_mutex_post(&s_play_lock);
        return 0;
    }
    s_play.accepting_writes = 0;
    s_play.draining = 0;
    (void)os_mutex_post(&s_play_lock);
    for (;;) {
        (void)os_mutex_pend(&s_play_lock, 0);
        writer_inflight = s_play.writer_inflight;
        (void)os_mutex_post(&s_play_lock);
        if (!writer_inflight) {
            break;
        }
        if (retries++ >= WL83_PLAY_WRITER_DRAIN_LIMIT) {
            printf("[JIELI_AUDIO_WL83] DAC writer did not quiesce during stop\n");
            return -1;
        }
        os_time_dly(1);
    }

    (void)os_mutex_pend(&s_play_lock, 0);
    /* STOP means interrupt playback promptly and discard software-queued PCM. */
    s_play.read_pos = s_play.write_pos;
    s_play.used = 0;
    channel_open = s_play.channel_open;
    (void)os_mutex_post(&s_play_lock);

    if (channel_open) {
        audio_dac_channel_close(NULL);
        (void)os_mutex_pend(&s_play_lock, 0);
        s_play.channel_open = 0;
        (void)os_mutex_post(&s_play_lock);
    }
    ret = audio_dac_stop(s_play.dac);
    if (ret != 0) {
        printf("[JIELI_AUDIO_WL83] audio_dac_stop failed: %d\n", ret);
        /* Keep writes gated and running set so callers can retry stop safely. */
        return -1;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    s_play.running = 0;
    s_play.accepting_writes = 0;
    s_play.channel_open = 0;
    (void)os_mutex_post(&s_play_lock);
    return 0;
}

int jieli_audio_native_ao_uninit(void *stream)
{
    if (stream != &s_play) {
        return -2;
    }
    if (!s_play.initialized) {
        return 0;
    }
    if (s_play.running && jieli_audio_native_ao_stop(stream) != 0) {
        return -1;
    }
    memset(&s_play, 0, sizeof(s_play));
    return 0;
}

int jieli_audio_native_ao_set_volume(void *stream, int volume)
{
    int ret;
    uint8_t channel_open;
    if (stream != &s_play || volume < 0 || volume > 100) {
        return -2;
    }
    if (!s_play.initialized || s_play.dac == NULL) {
        return -4;
    }
    /* audio_dac_set_volume() takes JieLi's 0-100 level, the same scale its own
     * volume mixer feeds it (IDLE_DEFAULT_MAX_VOLUME is 100). Rescaling to a
     * 0-15 hardware-looking range capped playback at 15% and turned the 80%
     * default into a gain of 12, which is inaudible on the dev board's
     * amplifier. The capture path's 0-19 rescaling is correct: that is the
     * documented audio_adc_mic_set_gain() range. */
    ret = audio_dac_set_volume(s_play.dac, (u8)volume);
    if (ret != 0) {
        return -1;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    s_play.volume = volume;
    channel_open = s_play.channel_open;
    (void)os_mutex_post(&s_play_lock);
    if (channel_open) {
        /* Already playing, so the fade handler will not fire again and the
         * recorded level alone would not reach the hardware. Before the first
         * start, ao_start() applies it instead. */
        __play_apply_digital_volume();
    }
    return 0;
}

int jieli_audio_native_ao_get_volume(void *stream, int *volume)
{
    if (stream != &s_play || volume == NULL) {
        return -2;
    }
    if (!s_play.initialized) {
        return -4;
    }
    *volume = s_play.volume;
    return 0;
}

int jieli_audio_native_ao_write(void *stream, const uint8_t *data, size_t size)
{
    uint32_t first;

    if (stream != &s_play || data == NULL || size == 0 || size > WL83_PLAY_QUEUE_SIZE || (size & 1u)) {
        return -2;
    }
    (void)os_mutex_pend(&s_play_lock, 0);
    if (!s_play.initialized || !s_play.running || !s_play.accepting_writes || s_play.dac == NULL) {
        (void)os_mutex_post(&s_play_lock);
        return -4;
    }
    if (s_play.write_error != 0) {
        (void)os_mutex_post(&s_play_lock);
        /* Reserve -31 for queue-full backpressure; report DAC failures as COM_ERROR. */
        return -1;
    }
    if (size > WL83_PLAY_QUEUE_SIZE - s_play.used) {
        (void)os_mutex_post(&s_play_lock);
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    first = WL83_PLAY_QUEUE_SIZE - s_play.write_pos;
    if (first > size) {
        first = (uint32_t)size;
    }
    memcpy(s_play_queue + s_play.write_pos, data, first);
    if (size > first) {
        memcpy(s_play_queue, data + first, size - first);
    }
    s_play.write_pos = (s_play.write_pos + (uint32_t)size) % WL83_PLAY_QUEUE_SIZE;
    s_play.used += (uint32_t)size;
    s_play.queued_total += (uint32_t)size;
    (void)os_mutex_post(&s_play_lock);

    (void)os_sem_post(&s_play_sem);
    return 0;
}

int jieli_audio_native_ao_flush(void *stream)
{
    uint32_t retries = 0;
    uint32_t queued_bytes;
    uint8_t writer_inflight;
    int write_error;
    struct audio_dac_hdl *dac;

    if (stream != &s_play) {
        return -2;
    }

    (void)os_mutex_pend(&s_play_lock, 0);
    if (!s_play.initialized || s_play.dac == NULL) {
        (void)os_mutex_post(&s_play_lock);
        return -4;
    }
    s_play.accepting_writes = 0;
    s_play.draining = 1;
    dac = s_play.dac;
    (void)os_mutex_post(&s_play_lock);
    printf("[JIELI_AUDIO_WL83] draining speaker queue: queued=%u session_in=%u session_out=%u\n",
           (unsigned)s_play.used, (unsigned)s_play.queued_total, (unsigned)s_play.written_total);

    for (;;) {
        (void)os_mutex_pend(&s_play_lock, 0);
        queued_bytes = s_play.used;
        writer_inflight = s_play.writer_inflight;
        write_error = s_play.write_error;
        (void)os_mutex_post(&s_play_lock);

        if (write_error != 0) {
            int stop_ret;
            printf("[JIELI_AUDIO_WL83] speaker drain aborted: DAC writer error=%d queued=%u\n",
                   write_error, (unsigned)queued_bytes);
            (void)os_mutex_pend(&s_play_lock, 0);
            s_play.draining = 0;
            (void)os_mutex_post(&s_play_lock);
            stop_ret = jieli_audio_native_ao_stop(stream);
            if (stop_ret == 0) {
                (void)os_mutex_pend(&s_play_lock, 0);
                s_play.write_error = 0;
                (void)os_mutex_post(&s_play_lock);
            }
            return -1;
        }

        if (queued_bytes == 0 && !writer_inflight && audio_dac_idle(dac) == 1) {
            (void)os_mutex_pend(&s_play_lock, 0);
            s_play.draining = 0;
            (void)os_mutex_post(&s_play_lock);
            printf("[JIELI_AUDIO_WL83] speaker queue drained; DAC idle: session_in=%u session_out=%u\n",
                   (unsigned)s_play.queued_total, (unsigned)s_play.written_total);
            return 0;
        }

        if (retries++ >= WL83_PLAY_DRAIN_LIMIT) {
            printf("[JIELI_AUDIO_WL83] speaker drain timeout: queued=%u writer=%u dac_idle=%d "
                   "session_in=%u session_out=%u\n",
                   (unsigned)queued_bytes, (unsigned)writer_inflight, audio_dac_idle(dac),
                   (unsigned)s_play.queued_total, (unsigned)s_play.written_total);
            /* Leave draining enabled: the background writer can still deliver
             * accepted PCM, and a later flush/stop can retry the idle wait. */
            return OPRT_TIMEOUT;
        }
        os_time_dly(1);
    }
}
