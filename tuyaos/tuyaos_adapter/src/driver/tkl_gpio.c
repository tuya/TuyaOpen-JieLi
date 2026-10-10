#include "tkl_gpio.h"
#include "tuya_error_code.h"

#include "gpio.h"

/* The JieLi pin namespace is already flat and matches Tuya's: the vendor header
 * defines IO_PORTA_00 as IO_GROUP_NUM * 0 + 0 with IO_GROUP_NUM == 16, so port A
 * occupies 0..15, B 16..31 and so on, and a Tuya pin index is the vendor pin
 * number directly. No lookup table is needed.
 *
 * The mapping only reaches as far as Tuya's enum does: TUYA_GPIO_NUM_MAX is 64,
 * so PORTE (64..79) and PORTF (80..95) cannot be addressed through this
 * contract. That includes PE11, which this board uses as the console RX. Those
 * ports stay reachable only from the vendor side; widening the contract is a
 * separate change and is not needed by the display path.
 */
#define TKL_GPIO_PIN_MAX 64

static int tkl_gpio_valid(TUYA_GPIO_NUM_E pin_id)
{
    return (pin_id < TUYA_GPIO_NUM_MAX) && (pin_id < TKL_GPIO_PIN_MAX);
}

OPERATE_RET tkl_gpio_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_BASE_CFG_T *cfg)
{
    if (!tkl_gpio_valid(pin_id) || cfg == NULL) {
        return OPRT_INVALID_PARM;
    }

    /* Check the mode before touching the pin. The vendor API has no open-drain
     * setting - gpio_set_hd() selects a drive strength, not an output type - so
     * an open-drain request cannot be honoured. Driving it push-pull instead
     * would leave the line actively high where the caller expected it released,
     * which on a shared line is a conflict rather than a degradation. Refuse it
     * and leave the pin as it was. */
    if (cfg->direct == TUYA_GPIO_OUTPUT &&
        (cfg->mode == TUYA_GPIO_OPENDRAIN || cfg->mode == TUYA_GPIO_OPENDRAIN_PULLUP)) {
        return OPRT_NOT_SUPPORTED;
    }

    /* Digital function: the analogue alternative (die = 0) is only meaningful
     * for pins routed to the ADC, which this contract does not cover. */
    (void)gpio_set_die((u32)pin_id, 1);

    if (cfg->direct == TUYA_GPIO_OUTPUT) {
        (void)gpio_direction_output((u32)pin_id,
                                    cfg->level == TUYA_GPIO_LEVEL_HIGH ? 1 : 0);
        return OPRT_OK;
    }

    (void)gpio_direction_input((u32)pin_id);

    /* Pull selection is deliberately not mapped here. The vendor takes a
     * strength enum (gpio_pull_up_mode_t) with no documented Tuya equivalent,
     * and guessing a strength would silently change the electrical behaviour.
     * Inputs therefore keep whatever the vendor default is. The display path
     * only drives outputs, so nothing in this phase depends on it. */
    if (cfg->mode == TUYA_GPIO_PULLUP || cfg->mode == TUYA_GPIO_PULLDOWN) {
        return OPRT_NOT_SUPPORTED;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_deinit(TUYA_GPIO_NUM_E pin_id)
{
    if (!tkl_gpio_valid(pin_id)) {
        return OPRT_INVALID_PARM;
    }
    /* Leave the pin as a floating input rather than holding whatever level it
     * was last driven to. */
    (void)gpio_direction_input((u32)pin_id);
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_write(TUYA_GPIO_NUM_E pin_id, TUYA_GPIO_LEVEL_E level)
{
    if (!tkl_gpio_valid(pin_id)) {
        return OPRT_INVALID_PARM;
    }
    if (level != TUYA_GPIO_LEVEL_LOW && level != TUYA_GPIO_LEVEL_HIGH) {
        return OPRT_INVALID_PARM;
    }
    (void)gpio_direction_output((u32)pin_id, level == TUYA_GPIO_LEVEL_HIGH ? 1 : 0);
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_read(TUYA_GPIO_NUM_E pin_id, TUYA_GPIO_LEVEL_E *level)
{
    if (!tkl_gpio_valid(pin_id) || level == NULL) {
        return OPRT_INVALID_PARM;
    }
    *level = gpio_read((u32)pin_id) ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_irq_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_IRQ_T *cfg)
{
    (void)pin_id;
    (void)cfg;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_gpio_irq_enable(TUYA_GPIO_NUM_E pin_id)
{
    (void)pin_id;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_gpio_irq_disable(TUYA_GPIO_NUM_E pin_id)
{
    (void)pin_id;
    return OPRT_NOT_SUPPORTED;
}
