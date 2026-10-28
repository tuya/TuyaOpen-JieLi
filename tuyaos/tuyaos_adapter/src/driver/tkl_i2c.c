#include "tkl_i2c.h"
#include "tuya_error_code.h"

#include <stdint.h>

#include "device.h"
#include "iic.h"
#if defined(JIELI_SELECTED_CHIP_WL82)
#include "ioctl_cmds.h"
#endif

#if defined(JIELI_SELECTED_CHIP_WL82)
extern OPERATE_RET jieli_wl82_iic1_pinmux_apply(void);
#endif

/* Master transfers use byte-level device ioctls. WL82 and WL83 have different
 * ioctl names and lock semantics, mapped below. This path supports 7-bit
 * addresses; slave mode, IRQ completion and register-width control are not
 * exposed by these device APIs. */
#define TKL_I2C_PORT_MAX 2

static void *s_i2c_handles[TKL_I2C_PORT_MAX];
/* Retain the vendor bus lock across repeated-start operations. */
static uint8_t s_i2c_bus_held[TKL_I2C_PORT_MAX];

/* Forward declarations used by deinit before their definitions below. */
static OPERATE_RET jieli_i2c_acquire(TUYA_I2C_NUM_E port);
static void jieli_i2c_release(TUYA_I2C_NUM_E port);

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

/* Translate SDK ioctl failures to a TKL bus error. */
static OPERATE_RET jieli_i2c_ioctl(void *handle, uint32_t cmd, uint32_t arg)
{
    int ret = dev_ioctl(handle, cmd, (u32)arg);
    if (ret < 0) {
        return OPRT_OS_ADAPTER_I2C_TRANS_ERR;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_i2c_init(TUYA_I2C_NUM_E port, const TUYA_IIC_BASE_CFG_T *cfg)
{
    const char *name;
#if defined(JIELI_SELECTED_CHIP_WL83)
    uint32_t speed_hz;
#endif

    if (cfg == NULL || !jieli_i2c_valid(port)) {
        return OPRT_INVALID_PARM;
    }
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
    /* WL82 iic0 is software IIC with fixed timing. Hardware iic1 uses the
     * DevKit divider 0x3f (~385 kHz); IOCTL_SET_SPEED expects a divider. */
    if (port != TUYA_I2C_NUM_1 || cfg->speed != TUYA_IIC_BUS_SPEED_400K) {
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
    name = jieli_i2c_device_name(port);
    if (s_i2c_handles[port] != NULL) {
        return OPRT_OK;
    }
    s_i2c_handles[port] = dev_open(name, NULL);
    if (s_i2c_handles[port] == NULL) {
        /* The board profile did not register this port. */
        return OPRT_NOT_SUPPORTED;
    }
#if defined(JIELI_SELECTED_CHIP_WL82)
    /* Re-apply the selected IIC1 route after open. */
    if (port == TUYA_I2C_NUM_1 && jieli_wl82_iic1_pinmux_apply() != OPRT_OK) {
        dev_close(s_i2c_handles[port]);
        s_i2c_handles[port] = NULL;
        return OPRT_COM_ERROR;
    }
    if (dev_ioctl(s_i2c_handles[port], IOCTL_SET_SPEED, 0x3fu) != 0) {
        dev_close(s_i2c_handles[port]);
        s_i2c_handles[port] = NULL;
        return OPRT_COM_ERROR;
    }
#else
    /* WL83's hardware-IIC baud ioctl takes Hz. */
    if (dev_ioctl(s_i2c_handles[port], IOCTL_IIC_SET_BAUDRATE, speed_hz) != 0) {
        dev_close(s_i2c_handles[port]);
        s_i2c_handles[port] = NULL;
        return OPRT_COM_ERROR;
    }
#endif
    return OPRT_OK;
}

OPERATE_RET tkl_i2c_deinit(TUYA_I2C_NUM_E port)
{
    if (!jieli_i2c_valid(port)) {
        return OPRT_INVALID_PARM;
    }
    if (s_i2c_handles[port] == NULL) {
        return OPRT_OK;
    }
    /* Release any unfinished repeated-start lock before closing the handle. */
    jieli_i2c_release(port);
    dev_close(s_i2c_handles[port]);
    s_i2c_handles[port] = NULL;
    return OPRT_OK;
}

/* START acquires the SDK lock; byte ioctls generate wire conditions. */
static OPERATE_RET jieli_i2c_acquire(TUYA_I2C_NUM_E port)
{
    if (s_i2c_bus_held[port]) {
        return OPRT_OK;
    }
    if (jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_START), 0u) != OPRT_OK) {
        return OPRT_OS_ADAPTER_I2C_BUS_ERR;
    }
    s_i2c_bus_held[port] = 1u;
    return OPRT_OK;
}

/* Release the SDK lock after a completed or failed transfer. */
static void jieli_i2c_release(TUYA_I2C_NUM_E port)
{
    if (!s_i2c_bus_held[port]) {
        return;
    }
    (void)jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_STOP), 0u);
    s_i2c_bus_held[port] = 0u;
}

OPERATE_RET tkl_i2c_master_send(TUYA_I2C_NUM_E port, uint16_t dev_addr, const void *data, uint32_t size,
                                BOOL_T xfer_pending)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;
    OPERATE_RET rt;

    if (!jieli_i2c_valid(port) || s_i2c_handles[port] == NULL || data == NULL || size == 0 ||
        dev_addr > 0x7Fu) {
        return OPRT_INVALID_PARM;
    }
    if (size > 0xFFFFu) {
        /* This backend sends one byte per ioctl and caps one request. */
        return OPRT_INVALID_PARM;
    }

    /* Address byte: 7-bit address shifted up, R/W clear for a write. */
    rt = jieli_i2c_acquire(port);
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_TX_WITH_START),
                         (uint8_t)((dev_addr << 1) & 0xFEu));
    if (rt != OPRT_OK) {
        jieli_i2c_release(port);
        return rt;
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
            jieli_i2c_release(port);
            return rt;
        }
    }
    if (xfer_pending) {
        /* Keep the lock for the repeated-start follow-up. */
    } else {
        jieli_i2c_release(port);
    }
    return OPRT_OK;
}

OPERATE_RET tkl_i2c_master_receive(TUYA_I2C_NUM_E port, uint16_t dev_addr, void *data, uint32_t size,
                                   BOOL_T xfer_pending)
{
    uint8_t *p = (uint8_t *)data;
    uint32_t i;
    OPERATE_RET rt;

    if (!jieli_i2c_valid(port) || s_i2c_handles[port] == NULL || data == NULL || size == 0 ||
        dev_addr > 0x7Fu) {
        return OPRT_INVALID_PARM;
    }

    /* Send the read address after START. */
    rt = jieli_i2c_acquire(port);
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = jieli_i2c_ioctl(s_i2c_handles[port], jieli_i2c_ioctl_cmd(JIELI_I2C_CMD_TX_WITH_START),
                         (uint8_t)(((dev_addr << 1) & 0xFEu) | 0x01u));
    if (rt != OPRT_OK) {
        jieli_i2c_release(port);
        return rt;
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
            jieli_i2c_release(port);
            return rt;
        }
        p[i] = byte;
    }
    if (!xfer_pending) {
        jieli_i2c_release(port);
    }
    return OPRT_OK;
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
    if (!jieli_i2c_valid(port) || s_i2c_handles[port] == NULL) {
        return OPRT_INVALID_PARM;
    }
    /* Reopen the device to reinitialize its controller. */
    dev_close(s_i2c_handles[port]);
    s_i2c_bus_held[port] = 0u;
    s_i2c_handles[port] = dev_open(jieli_i2c_device_name(port), NULL);
    return s_i2c_handles[port] != NULL ? OPRT_OK : OPRT_OS_ADAPTER_I2C_TRANS_ERR;
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
