/**
 * @file tkl_adc.c
 * @brief Tuya ADC adapter for JieLi WL82 and WL83.
 *
 * The JieLi SDK owns one process-global ADC sampler queue. Board startup must
 * call adc_init() before registering the ADC channels. This adapter only adds
 * channels to that queue and tracks which channels are available through the
 * Tuya ADC API; it never resets or removes shared vendor queue entries.
 */

#include "tkl_adc.h"

#include "tuya_error_code.h"
#include "generic/typedef.h"
#include "asm/adc_api.h"

#include <stdbool.h>

#define JIELI_ADC_RAW_MAX       1023U
#define JIELI_ADC_REF_MV        3300U

#if defined(CONFIG_CPU_WL83)
#define JIELI_ADC_EXTERNAL_CHANNEL_COUNT 16U /* ADC_IO_CH_PD00..ADC_IO_CH_DM */
#else
/* WL82's ADC0 external GPIO channels occupy AD_CH_PA07 through AD_CH_DP. */
#define JIELI_ADC_EXTERNAL_CHANNEL_COUNT 14U
#endif

static uint16_t s_active_channel_mask;

static uint8_t jieli_adc_channel_count(uint16_t mask)
{
    uint8_t count = 0;

    while (mask != 0U) {
        count += (uint8_t)(mask & 1U);
        mask >>= 1;
    }

    return count;
}

static uint16_t jieli_adc_supported_mask(void)
{
    return (uint16_t)((1UL << JIELI_ADC_EXTERNAL_CHANNEL_COUNT) - 1UL);
}

static OPERATE_RET jieli_adc_validate_port(TUYA_ADC_NUM_E port_num)
{
    return (port_num == TUYA_ADC_NUM_0) ? OPRT_OK : OPRT_NOT_SUPPORTED;
}

static OPERATE_RET jieli_adc_read_raw(TUYA_ADC_NUM_E port_num, uint8_t ch_id, int32_t *data)
{
    uint32_t raw;

    if (data == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (OPRT_OK != jieli_adc_validate_port(port_num)) {
        return OPRT_NOT_SUPPORTED;
    }
    if (ch_id >= JIELI_ADC_EXTERNAL_CHANNEL_COUNT ||
        (s_active_channel_mask & (uint16_t)(1U << ch_id)) == 0U) {
        return OPRT_INVALID_PARM;
    }

    raw = adc_get_value(ch_id);
    if (raw > JIELI_ADC_RAW_MAX) {
        return OPRT_COM_ERROR;
    }

    *data = (int32_t)raw;
    return OPRT_OK;
}

OPERATE_RET tkl_adc_init(TUYA_ADC_NUM_E port_num, TUYA_ADC_BASE_CFG_T *cfg)
{
    uint16_t requested_mask;
    uint8_t channel_count;
    uint8_t channel;

    if (cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (OPRT_OK != jieli_adc_validate_port(port_num)) {
        return OPRT_NOT_SUPPORTED;
    }

    if ((cfg->ch_list.data & 0xFFFF0000UL) != 0U || cfg->width != 10U ||
        cfg->type != TUYA_ADC_EXTERNAL_SAMPLE_VOL || cfg->mode > TUYA_ADC_SCAN) {
        return OPRT_NOT_SUPPORTED;
    }

    requested_mask = (uint16_t)cfg->ch_list.data;
    channel_count = jieli_adc_channel_count(requested_mask);
    if (requested_mask == 0U || (requested_mask & (uint16_t)~jieli_adc_supported_mask()) != 0U ||
        channel_count == 0U || cfg->ch_nums != channel_count) {
        return OPRT_INVALID_PARM;
    }

    for (channel = 0; channel < JIELI_ADC_EXTERNAL_CHANNEL_COUNT; channel++) {
        uint32_t queue_index;

        if ((requested_mask & (uint16_t)(1U << channel)) == 0U) {
            continue;
        }

        /* adc_init() belongs to board startup; reinitializing it clears this shared queue. */
        queue_index = adc_add_sample_ch(channel);
        if (queue_index >= ADC_MAX_CH) {
            return OPRT_COM_ERROR;
        }
    }

    s_active_channel_mask |= requested_mask;
    return OPRT_OK;
}

OPERATE_RET tkl_adc_deinit(TUYA_ADC_NUM_E port_num)
{
    if (OPRT_OK != jieli_adc_validate_port(port_num)) {
        return OPRT_NOT_SUPPORTED;
    }

    /* Do not remove vendor channels: the process-global queue can be shared. */
    s_active_channel_mask = 0U;
    return OPRT_OK;
}

uint8_t tkl_adc_width_get(TUYA_ADC_NUM_E port_num)
{
    return (OPRT_OK == jieli_adc_validate_port(port_num)) ? 10U : 0U;
}

uint32_t tkl_adc_ref_voltage_get(TUYA_ADC_NUM_E port_num)
{
    return (OPRT_OK == jieli_adc_validate_port(port_num)) ? JIELI_ADC_REF_MV : 0U;
}

int32_t tkl_adc_temperature_get(void)
{
    return OPRT_NOT_SUPPORTED;
}

static OPERATE_RET jieli_adc_read_list(TUYA_ADC_NUM_E port_num, int32_t *buff, uint16_t len, bool voltage)
{
    uint8_t channel;
    uint16_t read_count = 0;

    if (buff == NULL || len == 0U) {
        return OPRT_INVALID_PARM;
    }
    if (OPRT_OK != jieli_adc_validate_port(port_num)) {
        return OPRT_NOT_SUPPORTED;
    }
    if (s_active_channel_mask == 0U || len > jieli_adc_channel_count(s_active_channel_mask)) {
        return OPRT_INVALID_PARM;
    }

    for (channel = 0; channel < JIELI_ADC_EXTERNAL_CHANNEL_COUNT && read_count < len; channel++) {
        uint32_t sample;

        if ((s_active_channel_mask & (uint16_t)(1U << channel)) == 0U) {
            continue;
        }

        sample = voltage ? adc_get_voltage(channel) : adc_get_value(channel);
        if (!voltage && sample > JIELI_ADC_RAW_MAX) {
            return OPRT_COM_ERROR;
        }
        buff[read_count++] = (int32_t)sample;
    }

    return (read_count == len) ? OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_adc_read_data(TUYA_ADC_NUM_E port_num, int32_t *buff, uint16_t len)
{
    return jieli_adc_read_list(port_num, buff, len, false);
}

OPERATE_RET tkl_adc_read_single_channel(TUYA_ADC_NUM_E port_num, uint8_t ch_id, int32_t *data)
{
    return jieli_adc_read_raw(port_num, ch_id, data);
}

OPERATE_RET tkl_adc_read_voltage(TUYA_ADC_NUM_E port_num, int32_t *buff, uint16_t len)
{
    return jieli_adc_read_list(port_num, buff, len, true);
}

OPERATE_RET tkl_adc_ioctl(ADC_IOCTL_CMD_E cmd, void *args)
{
    (void)cmd;
    (void)args;
    return OPRT_NOT_SUPPORTED;
}
