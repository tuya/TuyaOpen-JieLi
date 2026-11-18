#include "tkl_i2c.h"
#include "tuya_error_code.h"

#include <stdint.h>

#include "device.h"
#include "iic.h"
#include "jieli_pinmux_internal.h"
#include "system/os/pthread.h"
#include "tkl_system.h"
#if defined(JIELI_SELECTED_CHIP_WL83)
#include "asm/gpio.h"
#include "asm/iic.h"
#include "init.h"

/* HW_IIC1_PLATFORM_DATA_BEGIN does more than fill the platform data: it also
 * emits a platform_initcall that pulls hw_iic_ops into the image. A hand-written
 * struct leaves the node in the .device table with no ops behind it, and
 * devices_init() stalls on that node before the application ever runs - the
 * board then shows neither its panel nor its USB device. Use the vendor macro
 * and state only the fields the board chose. */
/* Board-declared pins, staged into tuya_board_periph_config.h. The defaults
 * below only apply to a board that declares nothing. */
#ifndef TUYA_BOARD_IIC0_SCL_PIN
#define TUYA_BOARD_IIC0_SCL_PIN IO_PORTA_04
#endif
#ifndef TUYA_BOARD_IIC0_SDA_PIN
#define TUYA_BOARD_IIC0_SDA_PIN IO_PORTA_05
#endif
#ifndef TUYA_BOARD_IIC1_SCL_PIN
#define TUYA_BOARD_IIC1_SCL_PIN IO_PORTA_06
#endif
#ifndef TUYA_BOARD_IIC1_SDA_PIN
#define TUYA_BOARD_IIC1_SDA_PIN IO_PORTA_07
#endif
#ifndef TUYA_BOARD_IIC2_SCL_PIN
#define TUYA_BOARD_IIC2_SCL_PIN IO_PORTA_02
#endif
#ifndef TUYA_BOARD_IIC2_SDA_PIN
#define TUYA_BOARD_IIC2_SDA_PIN IO_PORTA_03
#endif

HW_IIC1_PLATFORM_DATA_BEGIN(s_iic1_pd)
    .role = IIC_ROLE_MASTER,
    .hd_level = GPIO_DRIVE_STRENGTH_2p5mA,
    .clk_pin = TUYA_BOARD_IIC1_SCL_PIN,
    .dat_pin = TUYA_BOARD_IIC1_SDA_PIN,
    .io_filter = IIC_FILTER_CLOSE,
    .internal_pull_up_enable = 1,
    .fast_resp = 0,
    .slave_addr = 0,
    .adv_enable = 0,
    .ie_enable = 0,
    .stretch_disable = 0,
    .baudrate = 100000,
    .timeout_cnt = 40,
    .reg = NULL,
HW_IIC1_PLATFORM_DATA_END()
REGISTER_DEVICE(iic1_node) = { "iic1", &iic_dev_ops, (void *)&s_iic1_pd };

/* IIC0 is the 792's enhanced hardware IIC. The vendor board registers it only
 * under TCFG_IIC_ENABLE, which the staged demo configuration leaves off, so the
 * adapter registers its own node with the vendor demo pins. */
HW_IIC0_PLATFORM_DATA_BEGIN(s_iic0_pd)
    .role = IIC_ROLE_MASTER,
    .hd_level = GPIO_DRIVE_STRENGTH_2p5mA,
    .clk_pin = TUYA_BOARD_IIC0_SCL_PIN,
    .dat_pin = TUYA_BOARD_IIC0_SDA_PIN,
    .io_filter = 2,
    .internal_pull_up_enable = 1,
    .fast_resp = 0,
    .ie_enable = 0,
    .baudrate = 100000,
    .timeout_cnt = 40,
HW_IIC0_PLATFORM_DATA_END()
REGISTER_DEVICE(iic0_node) = { "iic0", &iic_dev_ops, (void *)&s_iic0_pd };

/* Software IIC (SDK iic2) owns its pins directly, so no pinmux is applied for
 * this port. PA2/PA3 keep it clear of IIC0 (PA4/PA5) and IIC1 (PA6/PA7). */
SW_IIC_PLATFORM_DATA_BEGIN(s_iic2_pd)
    .clk_pin = TUYA_BOARD_IIC2_SCL_PIN,
    .dat_pin = TUYA_BOARD_IIC2_SDA_PIN,
    .sw_iic_delay = 80,
    .sw_iic_byte_gap_time = 300,
    .hd_level = GPIO_DRIVE_STRENGTH_2p5mA,
SW_IIC_PLATFORM_DATA_END()
REGISTER_DEVICE(iic2_node) = { "iic2", &iic_dev_ops, (void *)&s_iic2_pd };
#endif

#if defined(JIELI_SELECTED_CHIP_WL82)
#include "asm/gpio.h"
#include "asm/iic.h"
#include "init.h"
#include "ioctl_cmds.h"

/* Board-declared pins, staged into tuya_board_periph_config.h. */
#ifndef TUYA_BOARD_IIC0_SCL_PIN
#define TUYA_BOARD_IIC0_SCL_PIN IO_PORTA_00
#endif
#ifndef TUYA_BOARD_IIC0_SDA_PIN
#define TUYA_BOARD_IIC0_SDA_PIN IO_PORTA_01
#endif
#ifndef TUYA_BOARD_IIC1_SCL_PIN
#define TUYA_BOARD_IIC1_SCL_PIN IO_PORTA_07
#endif
#ifndef TUYA_BOARD_IIC1_SDA_PIN
#define TUYA_BOARD_IIC1_SDA_PIN IO_PORTA_08
#endif

/* Mirror the WL82 SDK hardware-IIC platform data. The pins come from the board,
 * and iic_init() stores this object directly as the device's hardware
 * configuration, so every field stays initialized.
 *
 * The SPI camera path is the exception, and only in the node *type*: its bus
 * shares PH0/PH1 with the sensor's 2-wire SPI output, and the hardware
 * controller does not work there. Measured on AC791 with otherwise identical
 * code: with iic1 registered as hardware IIC the TDD's ID probe and the whole
 * register table go through, but the SPI receiver then never sees a valid
 * line (frames=0, tag errors only); the same build with the software IIC on
 * those pads streams 640x480 at 4.4 fps. Every other build stays on hardware
 * IIC. The pins themselves come from the board in both cases. */
#if defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)
/* J4 control bus: PH0/PH1, from JIELI_BOARD_IIC1_SCL/SDA_PIN (the board
 * Kconfig defaults them to PH0/PH1 when the SPI camera path is selected). */
SW_IIC_PLATFORM_DATA_BEGIN(s_iic1_pd)
    .clk_pin = TUYA_BOARD_IIC1_SCL_PIN,
    .dat_pin = TUYA_BOARD_IIC1_SDA_PIN,
    .sw_iic_delay = 50,
SW_IIC_PLATFORM_DATA_END()
REGISTER_DEVICE(iic1_node) = {"iic1", &iic_dev_ops, (void *)&s_iic1_pd};
#else
static struct iic_device s_iic1_dev;
static struct hw_iic_platform_data s_iic1_pd = {
    .head = {
        .type = IIC_TYPE_HW,
        .p_iic_device = &s_iic1_dev,
    },
    .iic = {
        .clk_pin = TUYA_BOARD_IIC1_SCL_PIN,
        .dat_pin = TUYA_BOARD_IIC1_SDA_PIN,
        .baudrate = 0x3fu,
        .occupy_reg = (u32)(uintptr_t)&JL_IOMAP->CON1,
        .occupy_io_mask = ~(BIT(18) | BIT(19)),
        .occupy_io_value = 0u,
        .reg = JL_IIC,
        .outport_map = {
            {IO_PORT_USB_DPA, IO_PORT_USB_DMA, 0u},
            {IO_PORTA_07, IO_PORTA_08, BIT(18)},
            {IO_PORTC_01, IO_PORTC_02, BIT(19)},
            {IO_PORTH_00, IO_PORTH_01, BIT(18) | BIT(19)},
        },
    },
};

REGISTER_DEVICE(iic1_node) = {"iic1", &iic_dev_ops, &s_iic1_pd};
#endif

/* WL82 has no hardware IIC0: its iic0 is the vendor software IIC. Register it
 * here because the staged demo board does not. The SPI camera path keeps iic0
 * on the camera's PH0/PH1 as well: the vendor video probe opens iic0 when it
 * runs as ISP_DEV_0, and with iic0 on PA0/PA1 every sensor check NACKed while
 * the same sensor answered on iic1. */
#if defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)
SW_IIC_PLATFORM_DATA_BEGIN(s_iic0_pd)
    .clk_pin = IO_PORTH_00,
    .dat_pin = IO_PORTH_01,
    .sw_iic_delay = 50,
SW_IIC_PLATFORM_DATA_END()
#else
SW_IIC_PLATFORM_DATA_BEGIN(s_iic0_pd)
    .clk_pin = TUYA_BOARD_IIC0_SCL_PIN,
    .dat_pin = TUYA_BOARD_IIC0_SDA_PIN,
    .sw_iic_delay = 50,
SW_IIC_PLATFORM_DATA_END()
#endif
REGISTER_DEVICE(iic0_node) = {"iic0", &iic_dev_ops, (void *)&s_iic0_pd};
#endif

/* Master transfers use byte-level device ioctls. WL82 and WL83 have different
 * ioctl names and lock semantics, mapped below. This path supports 7-bit
 * addresses; slave mode, IRQ completion and register-width control are not
 * exposed by these device APIs. */
#define TKL_I2C_PORT_MAX 3

static void *s_i2c_handles[TKL_I2C_PORT_MAX];
static uint32_t s_i2c_speed_hz[TKL_I2C_PORT_MAX];
/* Retain the vendor bus lock across repeated-start operations. */
static uint8_t s_i2c_bus_held[TKL_I2C_PORT_MAX];
static uint8_t s_i2c_owner_valid[TKL_I2C_PORT_MAX];
static pthread_t s_i2c_owner[TKL_I2C_PORT_MAX];
static pthread_mutex_t s_i2c_state_mutex[TKL_I2C_PORT_MAX];
static volatile int s_i2c_mutex_state;

/* Forward declarations used by deinit before their definitions below. */
static OPERATE_RET jieli_i2c_acquire(TUYA_I2C_NUM_E port);
static OPERATE_RET jieli_i2c_release(TUYA_I2C_NUM_E port);

static OPERATE_RET jieli_i2c_state_lock(TUYA_I2C_NUM_E port)
{
    if (s_i2c_mutex_state != 2) {
        if (__sync_bool_compare_and_swap(&s_i2c_mutex_state, 0, 1)) {
            int i;
            for (i = 0; i < TKL_I2C_PORT_MAX; i++) {
                if (pthread_mutex_init(&s_i2c_state_mutex[i], NULL) != 0) {
                    s_i2c_mutex_state = -1;
                    return OPRT_COM_ERROR;
                }
            }
            s_i2c_mutex_state = 2;
        } else {
            while (s_i2c_mutex_state == 1) (void)tkl_system_sleep(1u);
        }
    }
    if (s_i2c_mutex_state != 2) return OPRT_COM_ERROR;
    return pthread_mutex_lock(&s_i2c_state_mutex[port]) == 0 ? OPRT_OK : OPRT_COM_ERROR;
}

static void jieli_i2c_state_unlock(TUYA_I2C_NUM_E port)
{
    (void)pthread_mutex_unlock(&s_i2c_state_mutex[port]);
}

static const char *jieli_i2c_device_name(TUYA_I2C_NUM_E port)
{
    if (port == TUYA_I2C_NUM_0) {
        return "iic0";
    }
#if defined(JIELI_SELECTED_CHIP_WL83)
    /* wl83 registers iic0..iic2 in its board profile. */
    if (port == TUYA_I2C_NUM_1) {
        return "iic1";
    }
    if (port == TUYA_I2C_NUM_2) {
        return "iic2";
    }
#else
    /* AC79_DevKitBoard's board file registers iic0 and iic1 only: "iic0" is a
     * software IIC and "iic1" the hardware one. Both are usable through the
     * same device API, so the mapping is by index and not by type. */
    if (port == TUYA_I2C_NUM_1) {
        return "iic1";
    }
#endif
    return NULL;
}

enum jieli_i2c_cmd {
    JIELI_I2C_CMD_START,
    JIELI_I2C_CMD_STOP,
    JIELI_I2C_CMD_TX,
    JIELI_I2C_CMD_TX_WITH_START,
    JIELI_I2C_CMD_TX_WITH_STOP,
    JIELI_I2C_CMD_RX_WITH_ACK,
    JIELI_I2C_CMD_RX_WITH_NACK,
    JIELI_I2C_CMD_RX_WITH_STOP,
};

#if defined(JIELI_SELECTED_CHIP_WL83)
static uint32_t jieli_i2c_ioctl_cmd(enum jieli_i2c_cmd cmd)
{
    switch (cmd) {
    case JIELI_I2C_CMD_START:         return IOCTL_IIC_START;
    case JIELI_I2C_CMD_STOP:          return IOCTL_IIC_STOP;
    case JIELI_I2C_CMD_TX:            return IOCTL_IIC_TX_BYTE;
    case JIELI_I2C_CMD_TX_WITH_START: return IOCTL_IIC_TX_BYTE_WITH_START_BIT;
    case JIELI_I2C_CMD_TX_WITH_STOP:  return IOCTL_IIC_TX_BYTE_WITH_STOP_BIT;
    case JIELI_I2C_CMD_RX_WITH_ACK:   return IOCTL_IIC_RX_BYTE_WITH_ACK;
    case JIELI_I2C_CMD_RX_WITH_NACK:  return IOCTL_IIC_RX_BYTE_WITH_NACK;
    case JIELI_I2C_CMD_RX_WITH_STOP:  return IOCTL_IIC_RX_BYTE_WITH_STOP_BIT;
    default:                          return 0u;
    }
}
#else
static uint32_t jieli_i2c_ioctl_cmd(enum jieli_i2c_cmd cmd)
{
    switch (cmd) {
    case JIELI_I2C_CMD_START:         return IIC_IOCTL_START;
    /* START/STOP acquire and release the device-layer lock. */
    case JIELI_I2C_CMD_STOP:          return IIC_IOCTL_STOP;
    case JIELI_I2C_CMD_TX:            return IIC_IOCTL_TX;
    case JIELI_I2C_CMD_TX_WITH_START: return IIC_IOCTL_TX_WITH_START_BIT;
    case JIELI_I2C_CMD_TX_WITH_STOP:  return IIC_IOCTL_TX_WITH_STOP_BIT;
    case JIELI_I2C_CMD_RX_WITH_ACK:   return IIC_IOCTL_RX_WITH_ACK;
    case JIELI_I2C_CMD_RX_WITH_NACK:  return IIC_IOCTL_RX_WITH_NOACK;
    case JIELI_I2C_CMD_RX_WITH_STOP:  return IIC_IOCTL_RX_WITH_STOP_BIT;
    default:                          return 0u;
    }
}
#endif

static int jieli_i2c_valid(TUYA_I2C_NUM_E port)
{
    return port < TUYA_I2C_NUM_MAX && port < TKL_I2C_PORT_MAX &&
           jieli_i2c_device_name(port) != NULL;
}

/* Software IIC drives its own pins and has fixed timing: TKL must not route a
 * pinmux for it, and a requested speed cannot be applied. */
static int jieli_i2c_is_software(TUYA_I2C_NUM_E port)
{
#if defined(JIELI_SELECTED_CHIP_WL83)
    return port == TUYA_I2C_NUM_2;
#else
#if defined(JIELI_CAMERA_PATH_SPI) && (JIELI_CAMERA_PATH_SPI == 1)
    /* The SPI camera build registers iic1 as the software IIC on PH0/PH1, so
     * init/reset must not route the hardware pinmux onto those pads. */
    return port == TUYA_I2C_NUM_0 || port == TUYA_I2C_NUM_1;
#else
    return port == TUYA_I2C_NUM_0;
#endif
#endif
}

/* Translate SDK ioctl failures to a TKL bus error. */
static OPERATE_RET jieli_i2c_ioctl(void *handle, uint32_t cmd, uint32_t arg)
{
    int ret = dev_ioctl(handle, cmd, (u32)arg);
    if (ret < 0) {
        return OPRT_OS_ADAPTER_I2C_TRANS_ERR;
    }
    return OPRT_OK;
}

static OPERATE_RET jieli_i2c_set_speed(TUYA_I2C_NUM_E port, uint32_t speed_hz)
{
    if (jieli_i2c_is_software(port)) {
        return OPRT_OK;
    }
#if defined(JIELI_SELECTED_CHIP_WL82)
    (void)speed_hz;
    /* The selected WL82 board's hardware IIC divider maps to about 385 kHz. */
    return dev_ioctl(s_i2c_handles[port], IOCTL_SET_SPEED, 0x3fu) == 0 ? OPRT_OK : OPRT_COM_ERROR;
#else
    return dev_ioctl(s_i2c_handles[port], IOCTL_IIC_SET_BAUDRATE, speed_hz) == 0 ? OPRT_OK : OPRT_COM_ERROR;
#endif
}

OPERATE_RET tkl_i2c_init(TUYA_I2C_NUM_E port, const TUYA_IIC_BASE_CFG_T *cfg)
{
    const char *name;
    uint32_t speed_hz = 0u;

    if (cfg == NULL || port >= TUYA_I2C_NUM_MAX || port >= TKL_I2C_PORT_MAX) {
        return OPRT_INVALID_PARM;
    }
    name = jieli_i2c_device_name(port);
    if (name == NULL) return OPRT_NOT_SUPPORTED;
    if (cfg->role != TUYA_IIC_MODE_MASTER) {
        return OPRT_NOT_SUPPORTED;
    }
    if (cfg->addr_width != TUYA_IIC_ADDRESS_7BIT) {
        /* This transfer path only emits 7-bit address phases. */
        return OPRT_NOT_SUPPORTED;
    }
    if (cfg->speed != TUYA_IIC_BUS_SPEED_100K && cfg->speed != TUYA_IIC_BUS_SPEED_400K &&
        cfg->speed != TUYA_IIC_BUS_SPEED_1M && cfg->speed != TUYA_IIC_BUS_SPEED_3_4M) {
        return OPRT_INVALID_PARM;
    }
#if defined(JIELI_SELECTED_CHIP_WL82)
    /* WL82's hardware iic1 has a fixed divider (about 385 kHz); port 0 is the
     * software IIC whose timing comes from its own delay field. The SPI camera
     * build registers iic1 as software too (PH0/PH1), so the 400 kHz gate only
     * applies to the hardware controller -- otherwise a perfectly valid 100 kHz
     * request from a sensor TDD is refused with OPRT_NOT_SUPPORTED. */
    if (jieli_i2c_is_software(port)) {
        /* Bit-banged port: the speed field does not drive the bus. */
    } else if (port == TUYA_I2C_NUM_1) {
        if (cfg->speed != TUYA_IIC_BUS_SPEED_400K) {
            return OPRT_NOT_SUPPORTED;
        }
    } else if (port != TUYA_I2C_NUM_0) {
        return OPRT_NOT_SUPPORTED;
    }
#else
    switch (cfg->speed) {
    case TUYA_IIC_BUS_SPEED_100K: speed_hz = 100000u; break;
    case TUYA_IIC_BUS_SPEED_400K: speed_hz = 400000u; break;
    case TUYA_IIC_BUS_SPEED_1M: speed_hz = 1000000u; break;
    default: return OPRT_NOT_SUPPORTED; /* no verified 3.4 MHz mode */
    }
#endif
    if (jieli_i2c_state_lock(port) != OPRT_OK) return OPRT_COM_ERROR;
    if (s_i2c_handles[port] != NULL) {
        /* A pending transaction spans API calls, so the state mutex alone
         * cannot keep its transfer configuration stable between calls. */
        if (s_i2c_bus_held[port]) {
            jieli_i2c_state_unlock(port);
            return OPRT_OS_ADAPTER_I2C_BUS_ERR;
        }
        OPERATE_RET rt = jieli_i2c_set_speed(port, speed_hz);
        if (rt == OPRT_OK) s_i2c_speed_hz[port] = speed_hz;
        jieli_i2c_state_unlock(port);
        return rt;
    }
    s_i2c_handles[port] = dev_open(name, NULL);
    if (s_i2c_handles[port] == NULL) {
        /* The board profile did not register this port. */
        jieli_i2c_state_unlock(port);
        return OPRT_NOT_SUPPORTED;
    }
    /* Apply any saved route after the SDK's board/device open setup. */
    if (!jieli_i2c_is_software(port) && jieli_pinmux_iic_apply(port) != OPRT_OK) {
        dev_close(s_i2c_handles[port]);
        s_i2c_handles[port] = NULL;
        jieli_i2c_state_unlock(port);
        return OPRT_COM_ERROR;
    }
    if (jieli_i2c_set_speed(port, speed_hz) != OPRT_OK) {
        dev_close(s_i2c_handles[port]);
        s_i2c_handles[port] = NULL;
        jieli_i2c_state_unlock(port);
        return OPRT_COM_ERROR;
    }
    s_i2c_speed_hz[port] = speed_hz;
    jieli_i2c_state_unlock(port);
    return OPRT_OK;
}

OPERATE_RET tkl_i2c_deinit(TUYA_I2C_NUM_E port)
{
    if (!jieli_i2c_valid(port)) {
        return OPRT_INVALID_PARM;
    }
    if (jieli_i2c_state_lock(port) != OPRT_OK) return OPRT_COM_ERROR;
    if (s_i2c_handles[port] == NULL) {
        jieli_i2c_state_unlock(port);
        return OPRT_OK;
    }
    if (s_i2c_bus_held[port] &&
        (!s_i2c_owner_valid[port] || !pthread_equal(pthread_self(), s_i2c_owner[port]))) {
        jieli_i2c_state_unlock(port);
        return OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    /* Do not close while the SDK lock may still be held. */
    if (jieli_i2c_release(port) != OPRT_OK) {
        jieli_i2c_state_unlock(port);
        return OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    if (dev_close(s_i2c_handles[port]) != 0) {
        jieli_i2c_state_unlock(port);
        return OPRT_COM_ERROR;
    }
    s_i2c_handles[port] = NULL;
    jieli_i2c_state_unlock(port);
    return OPRT_OK;
}

/* START acquires the SDK lock; byte ioctls generate wire conditions. */
static OPERATE_RET jieli_i2c_acquire(TUYA_I2C_NUM_E port)
{
    if (s_i2c_bus_held[port]) {
        return s_i2c_owner_valid[port] && pthread_equal(pthread_self(), s_i2c_owner[port])
                   ? OPRT_OK
                   : OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    if (jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_START), 0u) != OPRT_OK) {
        return OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    s_i2c_bus_held[port] = 1u;
    s_i2c_owner[port] = pthread_self();
    s_i2c_owner_valid[port] = 1u;
    return OPRT_OK;
}

/* Release the SDK lock after a completed or failed transfer. */
static OPERATE_RET jieli_i2c_release(TUYA_I2C_NUM_E port)
{
    if (!s_i2c_bus_held[port]) {
        return OPRT_OK;
    }
    if (!s_i2c_owner_valid[port] || !pthread_equal(pthread_self(), s_i2c_owner[port])) {
        return OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    OPERATE_RET stop_result = jieli_i2c_ioctl(
        s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_STOP), 0u);
    /* Both SDK device wrappers release their bus mutex after STOP even when
     * the lower ioctl reports an error. Keep local ownership aligned. */
    s_i2c_bus_held[port] = 0u;
    s_i2c_owner_valid[port] = 0u;
    return stop_result == OPRT_OK ? OPRT_OK : OPRT_OS_ADAPTER_I2C_TRANS_ERR;
}

OPERATE_RET tkl_i2c_master_send(TUYA_I2C_NUM_E port, uint16_t dev_addr, const void *data, uint32_t size,
                                BOOL_T xfer_pending)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;
    OPERATE_RET rt;

    if (!jieli_i2c_valid(port) || data == NULL || size == 0 || dev_addr > 0x7Fu) {
        return OPRT_INVALID_PARM;
    }
    if (size > 0xFFFFu) {
        /* This backend sends one byte per ioctl and caps one request. */
        return OPRT_INVALID_PARM;
    }

    if (jieli_i2c_state_lock(port) != OPRT_OK) return OPRT_COM_ERROR;
    if (s_i2c_handles[port] == NULL) {
        jieli_i2c_state_unlock(port);
        return OPRT_INVALID_PARM;
    }

    /* Address byte: 7-bit address shifted up, R/W clear for a write. */
    rt = jieli_i2c_acquire(port);
    if (rt != OPRT_OK) {
        goto out;
    }
    rt = jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_TX_WITH_START),
                         (uint8_t)((dev_addr << 1) & 0xFEu));
    if (rt != OPRT_OK) {
        (void)jieli_i2c_release(port);
        goto out;
    }
    for (i = 0; i < size; i++) {
        BOOL_T last = (i + 1 == size);
        /* START only on the first byte; STOP only on the final byte. */
        if (last && !xfer_pending) {
            rt = jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_TX_WITH_STOP), p[i]);
        } else {
            rt = jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_TX), p[i]);
        }
        if (rt != OPRT_OK) {
            /* Do not leave the bus locked after an ioctl failure. */
            (void)jieli_i2c_release(port);
            goto out;
        }
    }
    if (!xfer_pending) rt = jieli_i2c_release(port);
out:
    jieli_i2c_state_unlock(port);
    return rt;
}

OPERATE_RET tkl_i2c_master_receive(TUYA_I2C_NUM_E port, uint16_t dev_addr, void *data, uint32_t size,
                                   BOOL_T xfer_pending)
{
    uint8_t *p = (uint8_t *)data;
    uint32_t i;
    OPERATE_RET rt;

    if (!jieli_i2c_valid(port) || data == NULL || size == 0 || dev_addr > 0x7Fu) {
        return OPRT_INVALID_PARM;
    }

    if (jieli_i2c_state_lock(port) != OPRT_OK) return OPRT_COM_ERROR;
    if (s_i2c_handles[port] == NULL) {
        jieli_i2c_state_unlock(port);
        return OPRT_INVALID_PARM;
    }

    /* Send the read address after START. */
    rt = jieli_i2c_acquire(port);
    if (rt != OPRT_OK) {
        goto out;
    }
    rt = jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_TX_WITH_START),
                         (uint8_t)(((dev_addr << 1) & 0xFEu) | 0x01u));
    if (rt != OPRT_OK) {
        (void)jieli_i2c_release(port);
        goto out;
    }
    for (i = 0; i < size; i++) {
        uint8_t byte = 0;
        if (i + 1 == size) {
            /* NACK the final byte; include STOP unless another phase follows. */
            if (xfer_pending) {
                rt = jieli_i2c_ioctl(s_i2c_handles[port],
                                     jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_RX_WITH_NACK), (uint32_t)(uintptr_t)&byte);
            } else {
                rt = jieli_i2c_ioctl(s_i2c_handles[port],
                                     jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_RX_WITH_STOP), (uint32_t)(uintptr_t)&byte);
            }
        } else {
            rt = jieli_i2c_ioctl(s_i2c_handles[port],
                                 jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_RX_WITH_ACK), (uint32_t)(uintptr_t)&byte);
        }
        if (rt != OPRT_OK) {
            (void)jieli_i2c_release(port);
            goto out;
        }
        p[i] = byte;
    }
    if (!xfer_pending) rt = jieli_i2c_release(port);
out:
    jieli_i2c_state_unlock(port);
    return rt;
}

/* Unsupported TKL operations. */

OPERATE_RET tkl_i2c_irq_init(TUYA_I2C_NUM_E port, TUYA_I2C_IRQ_CB cb)
{
    (void)port;
    (void)cb;
    /* Transfers are synchronous and expose no completion event. */
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_irq_enable(TUYA_I2C_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_irq_disable(TUYA_I2C_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_set_slave_addr(TUYA_I2C_NUM_E port, uint16_t dev_addr)
{
    (void)port;
    (void)dev_addr;
    /* The device layer exposes no slave data path. */
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_slave_send(TUYA_I2C_NUM_E port, const void *data, uint32_t size)
{
    (void)port;
    (void)data;
    (void)size;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_slave_receive(TUYA_I2C_NUM_E port, void *data, uint32_t size)
{
    (void)port;
    (void)data;
    (void)size;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_get_status(TUYA_I2C_NUM_E port, TUYA_IIC_STATUS_T *status)
{
    (void)port;
    (void)status;
    /* The device layer exposes no bus status register. */
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_reset(TUYA_I2C_NUM_E port)
{
    OPERATE_RET rt;

    if (!jieli_i2c_valid(port)) {
        return OPRT_INVALID_PARM;
    }
    if (jieli_i2c_state_lock(port) != OPRT_OK) return OPRT_COM_ERROR;
    if (s_i2c_handles[port] == NULL) {
        jieli_i2c_state_unlock(port);
        return OPRT_INVALID_PARM;
    }
    if (s_i2c_bus_held[port] &&
        (!s_i2c_owner_valid[port] || !pthread_equal(pthread_self(), s_i2c_owner[port]))) {
        jieli_i2c_state_unlock(port);
        return OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    if (jieli_i2c_release(port) != OPRT_OK) {
        jieli_i2c_state_unlock(port);
        return OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    if (dev_close(s_i2c_handles[port]) != 0) {
        jieli_i2c_state_unlock(port);
        return OPRT_COM_ERROR;
    }
    s_i2c_handles[port] = NULL;
    s_i2c_handles[port] = dev_open(jieli_i2c_device_name(port), NULL);
    if (s_i2c_handles[port] == NULL) {
        rt = OPRT_OS_ADAPTER_I2C_TRANS_ERR;
        goto out;
    }
    /* A software port owns its pads directly; routing the hardware IIC1
     * function onto them here would take PH0/PH1 away from the bit-banged
     * bus (the SPI camera build registers iic1 that way). */
    rt = jieli_i2c_is_software(port) ? OPRT_OK : jieli_pinmux_iic_apply(port);
    if (rt == OPRT_OK) rt = jieli_i2c_set_speed(port, s_i2c_speed_hz[port]);
    if (rt != OPRT_OK) {
        (void)dev_close(s_i2c_handles[port]);
        s_i2c_handles[port] = NULL;
        rt = OPRT_COM_ERROR;
    }
out:
    jieli_i2c_state_unlock(port);
    return rt;
}

int32_t tkl_i2c_get_data_count(TUYA_I2C_NUM_E port)
{
    (void)port;
    /* The synchronous byte path has no in-flight count. */
    return 0;
}

OPERATE_RET tkl_i2c_ioctl(TUYA_I2C_NUM_E port, uint32_t cmd, void *args)
{
    (void)port;
    (void)cmd;
    (void)args;
    /* Register-address width is protocol-specific and not handled here. */
    return OPRT_NOT_SUPPORTED;
}
