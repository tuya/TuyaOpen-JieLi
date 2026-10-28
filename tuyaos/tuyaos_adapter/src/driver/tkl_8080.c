#include "tkl_8080.h"
#include "tkl_gpio.h"
#include "tuya_error_code.h"
#include "tkl_system.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(JIELI_SELECTED_CHIP_WL82)
#include "device/device.h"
#endif

#if defined(JIELI_SELECTED_CHIP_WL82)

#define JIELI_8080_WRITE_CHUNK 16384u

typedef struct {
    TUYA_8080_BASE_CFG_T cfg;
    void *emi;
    TUYA_MCU8080_ISR_CB callback;
    uint32_t frame_addr;
    bool initialized;
} JIELI_8080_CTX_T;

static JIELI_8080_CTX_T s_8080;

static OPERATE_RET jieli_8080_set_dc(TUYA_GPIO_LEVEL_E level)
{
    return tkl_gpio_write(s_8080.cfg.dc_pin, level);
}

static OPERATE_RET jieli_8080_write(const uint8_t *data, uint32_t len)
{
    uint32_t offset = 0;

    if (s_8080.emi == NULL || (data == NULL && len != 0u)) {
        return OPRT_INVALID_PARM;
    }

    while (offset < len) {
        uint32_t chunk = len - offset;
        int written;

        if (chunk > JIELI_8080_WRITE_CHUNK) {
            chunk = JIELI_8080_WRITE_CHUNK;
        }
        written = dev_write(s_8080.emi, (void *)(data + offset), chunk);
        if (written < 0 || (uint32_t)written != chunk) {
            return OPRT_COM_ERROR;
        }
        offset += chunk;
    }

    return OPRT_OK;
}

static OPERATE_RET jieli_8080_write_command(uint32_t cmd)
{
    uint8_t byte;
    OPERATE_RET ret;

    if (cmd > UINT8_MAX) {
        return OPRT_INVALID_PARM;
    }
    ret = jieli_8080_set_dc(TUYA_GPIO_LEVEL_LOW);
    if (ret != OPRT_OK) {
        return ret;
    }
    byte = (uint8_t)cmd;
    return jieli_8080_write(&byte, sizeof(byte));
}

static OPERATE_RET jieli_8080_write_data(const uint8_t *data, uint32_t len)
{
    OPERATE_RET ret = jieli_8080_set_dc(TUYA_GPIO_LEVEL_HIGH);
    if (ret != OPRT_OK) {
        return ret;
    }
    return jieli_8080_write(data, len);
}

OPERATE_RET tkl_8080_init(TUYA_8080_BASE_CFG_T *cfg)
{
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {0};
    OPERATE_RET ret;

    if (cfg == NULL || cfg->width == 0u || cfg->height == 0u || cfg->data_bits != 8u ||
        cfg->pixel_fmt != TUYA_PIXEL_FMT_RGB565 || cfg->dc_pin >= TUYA_GPIO_NUM_MAX ||
        (cfg->rst_pin != TUYA_GPIO_NUM_MAX && cfg->rst_pin >= TUYA_GPIO_NUM_MAX)) {
        return OPRT_INVALID_PARM;
    }
    if (s_8080.initialized) {
        return OPRT_COM_ERROR;
    }

    memset(&s_8080, 0, sizeof(s_8080));
    s_8080.cfg = *cfg;

    gpio_cfg.mode = TUYA_GPIO_PUSH_PULL;
    gpio_cfg.direct = TUYA_GPIO_OUTPUT;
    gpio_cfg.level = TUYA_GPIO_LEVEL_HIGH;
    ret = tkl_gpio_init(cfg->dc_pin, &gpio_cfg);
    if (ret != OPRT_OK) {
        return ret;
    }

    if (cfg->rst_pin < TUYA_GPIO_NUM_MAX) {
        ret = tkl_gpio_init(cfg->rst_pin, &gpio_cfg);
        if (ret != OPRT_OK) {
            (void)tkl_gpio_deinit(cfg->dc_pin);
            return ret;
        }
        (void)tkl_system_sleep(10);
        ret = tkl_gpio_write(cfg->rst_pin, TUYA_GPIO_LEVEL_LOW);
        if (ret == OPRT_OK) {
            (void)tkl_system_sleep(20);
            ret = tkl_gpio_write(cfg->rst_pin, TUYA_GPIO_LEVEL_HIGH);
        }
        if (ret != OPRT_OK) {
            (void)tkl_gpio_deinit(cfg->rst_pin);
            (void)tkl_gpio_deinit(cfg->dc_pin);
            return ret;
        }
        (void)tkl_system_sleep(120);
    }

    s_8080.emi = dev_open("emi", NULL);
    if (s_8080.emi == NULL) {
        if (cfg->rst_pin < TUYA_GPIO_NUM_MAX) {
            (void)tkl_gpio_deinit(cfg->rst_pin);
        }
        (void)tkl_gpio_deinit(cfg->dc_pin);
        return OPRT_NOT_SUPPORTED;
    }

    s_8080.initialized = true;
    return OPRT_OK;
}

OPERATE_RET tkl_8080_deinit(void)
{
    int close_ret = 0;

    if (!s_8080.initialized) {
        return OPRT_OK;
    }
    if (s_8080.emi != NULL) {
        close_ret = dev_close(s_8080.emi);
        s_8080.emi = NULL;
    }
    (void)tkl_gpio_deinit(s_8080.cfg.dc_pin);
    if (s_8080.cfg.rst_pin < TUYA_GPIO_NUM_MAX) {
        (void)tkl_gpio_deinit(s_8080.cfg.rst_pin);
    }
    s_8080.initialized = false;
    s_8080.frame_addr = 0u;
    s_8080.callback = NULL;
    return close_ret < 0 ? OPRT_COM_ERROR : OPRT_OK;
}

OPERATE_RET tkl_8080_irq_cb_register(TUYA_MCU8080_ISR_CB cb)
{
    if (!s_8080.initialized) {
        return OPRT_COM_ERROR;
    }
    s_8080.callback = cb;
    return OPRT_OK;
}

OPERATE_RET tkl_8080_ppi_set(uint16_t width, uint16_t height)
{
    if (!s_8080.initialized || width == 0u || height == 0u) {
        return OPRT_INVALID_PARM;
    }
    s_8080.cfg.width = width;
    s_8080.cfg.height = height;
    return OPRT_OK;
}

OPERATE_RET tkl_8080_pixel_mode_set(TUYA_DISPLAY_PIXEL_FMT_E mode)
{
    if (!s_8080.initialized || mode != TUYA_PIXEL_FMT_RGB565) {
        return OPRT_NOT_SUPPORTED;
    }
    s_8080.cfg.pixel_fmt = mode;
    s_8080.cfg.data_bits = 8u;
    return OPRT_OK;
}

OPERATE_RET tkl_8080_base_addr_set(uint32_t addr)
{
    if (!s_8080.initialized || addr == 0u) {
        return OPRT_INVALID_PARM;
    }
    s_8080.frame_addr = addr;
    return OPRT_OK;
}

OPERATE_RET tkl_8080_transfer_start(void)
{
    uint64_t frame_len;
    OPERATE_RET ret;

    if (!s_8080.initialized || s_8080.frame_addr == 0u || s_8080.cfg.width == 0u || s_8080.cfg.height == 0u ||
        s_8080.cfg.pixel_fmt != TUYA_PIXEL_FMT_RGB565 || s_8080.cfg.data_bits != 8u) {
        return OPRT_INVALID_PARM;
    }
    frame_len = (uint64_t)s_8080.cfg.width * s_8080.cfg.height * 2u;
    if (frame_len > UINT32_MAX) {
        return OPRT_INVALID_PARM;
    }

    ret = jieli_8080_write_data((const uint8_t *)(uintptr_t)s_8080.frame_addr, (uint32_t)frame_len);
    if (ret == OPRT_OK && s_8080.callback != NULL) {
        s_8080.callback(TUYA_MCU8080_OUTPUT_FINISH);
    }
    return ret;
}

OPERATE_RET tkl_8080_transfer_stop(void)
{
    /* EMI writes are synchronous; there is no pending transfer to cancel. */
    return s_8080.initialized ? OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_8080_cmd_send(uint32_t cmd)
{
    if (!s_8080.initialized) {
        return OPRT_COM_ERROR;
    }
    return jieli_8080_write_command(cmd);
}

OPERATE_RET tkl_8080_cmd_send_with_param(uint32_t cmd, uint32_t *param, uint8_t param_cnt)
{
    uint8_t packed[UINT8_MAX];
    OPERATE_RET ret;

    if (!s_8080.initialized || (param_cnt != 0u && param == NULL)) {
        return OPRT_INVALID_PARM;
    }
    for (uint32_t i = 0; i < param_cnt; ++i) {
        if (param[i] > UINT8_MAX) {
            return OPRT_INVALID_PARM;
        }
        packed[i] = (uint8_t)param[i];
    }

    ret = jieli_8080_write_command(cmd);
    if (ret != OPRT_OK || param_cnt == 0u) {
        return ret;
    }
    return jieli_8080_write_data(packed, param_cnt);
}

#else

OPERATE_RET tkl_8080_init(TUYA_8080_BASE_CFG_T *cfg) { (void)cfg; return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_deinit(void) { return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_irq_cb_register(TUYA_MCU8080_ISR_CB cb) { (void)cb; return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_ppi_set(uint16_t width, uint16_t height) { (void)width; (void)height; return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_pixel_mode_set(TUYA_DISPLAY_PIXEL_FMT_E mode) { (void)mode; return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_base_addr_set(uint32_t addr) { (void)addr; return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_transfer_start(void) { return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_transfer_stop(void) { return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_cmd_send(uint32_t cmd) { (void)cmd; return OPRT_NOT_SUPPORTED; }
OPERATE_RET tkl_8080_cmd_send_with_param(uint32_t cmd, uint32_t *param, uint8_t param_cnt)
{
    (void)cmd;
    (void)param;
    (void)param_cnt;
    return OPRT_NOT_SUPPORTED;
}

#endif