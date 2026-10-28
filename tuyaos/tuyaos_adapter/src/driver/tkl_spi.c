#include "tkl_spi.h"
#include "tuya_error_code.h"

#include <stdint.h>

#include "device.h"
#include "asm/spi.h"

/* Synchronous, 8-bit SPI master over registered spiN devices. ioctl names
 * differ by chip. Board data owns pins, clock, edges and CS; cfg does not
 * remap pins, and mode/frequency are not applied here. Slave, IRQ/DMA and
 * unsupported framing are unavailable through this path. Unregistered ports
 * fail at dev_open(); callers may control an unassigned CS as GPIO.
 */
#define TKL_SPI_PORT_MAX 3

static void *s_spi_handles[TKL_SPI_PORT_MAX];

#if defined(JIELI_SELECTED_CHIP_WL82)
static int8_t s_wl82_spi_pin_group[TKL_SPI_PORT_MAX] = {-1, -1, -1};

OPERATE_RET jieli_wl82_spi_pinmux_select(uint32_t spi_port, uint32_t group)
{
    if (spi_port >= TKL_SPI_PORT_MAX ||
        (spi_port == 0u && group != 1u) ||
        (spi_port == 1u && group > 1u) ||
        (spi_port == 2u && group != 0u && group != 2u)) {
        return OPRT_NOT_SUPPORTED;
    }
    if (s_wl82_spi_pin_group[spi_port] >= 0 && s_wl82_spi_pin_group[spi_port] != (int8_t)group) {
        /* Pinmux is configured one signal at a time. Do not let a later signal
         * silently move the whole peripheral away from the earlier one. */
        return OPRT_INVALID_PARM;
    }
    if (s_spi_handles[spi_port] != NULL &&
        dev_ioctl(s_spi_handles[spi_port], IOCTL_SPI_SET_PORT, group) != 0) {
        return OPRT_COM_ERROR;
    }
    s_wl82_spi_pin_group[spi_port] = (int8_t)group;
    return OPRT_OK;
}

static OPERATE_RET jieli_wl82_spi_pinmux_apply(TUYA_SPI_NUM_E port)
{
    if (s_wl82_spi_pin_group[port] < 0) {
        return OPRT_OK;
    }
    return dev_ioctl(s_spi_handles[port], IOCTL_SPI_SET_PORT,
                     (u32)s_wl82_spi_pin_group[port]) == 0 ? OPRT_OK : OPRT_COM_ERROR;
}
#endif

static const char *jieli_spi_device_name(TUYA_SPI_NUM_E port)
{
    switch (port) {
    case TUYA_SPI_NUM_0: return "spi0";
    case TUYA_SPI_NUM_1: return "spi1";
    case TUYA_SPI_NUM_2: return "spi2";
    default:             return NULL;
    }
}

static int jieli_spi_valid(TUYA_SPI_NUM_E port)
{
    return port < TUYA_SPI_NUM_MAX && port < TKL_SPI_PORT_MAX &&
           jieli_spi_device_name(port) != NULL;
}

OPERATE_RET tkl_spi_init(TUYA_SPI_NUM_E port, const TUYA_SPI_BASE_CFG_T *cfg)
{
    const char *name;

    if (cfg == NULL || !jieli_spi_valid(port)) {
        return OPRT_INVALID_PARM;
    }
    if (cfg->role != TUYA_SPI_ROLE_MASTER) {
        /* Slave has no data path through the device layer. */
        return OPRT_NOT_SUPPORTED;
    }
    if (cfg->databits != TUYA_SPI_DATA_BIT8) {
        return OPRT_NOT_SUPPORTED;
    }
    if (cfg->bitorder != TUYA_SPI_ORDER_MSB2LSB) {
        /* The board's spi_attr fixes the sample/update edges; there is no
         * separate bit-order knob in the platform data, so this cannot be
         * honoured for the other order. */
        return OPRT_NOT_SUPPORTED;
    }

    name = jieli_spi_device_name(port);
    if (s_spi_handles[port] != NULL) {
        return OPRT_OK;
    }
    s_spi_handles[port] = dev_open(name, NULL);
    if (s_spi_handles[port] == NULL) {
        /* The board did not register this port. On the AC792N profile spi1 is
         * gated behind a demo switch that is off, so this is the expected
         * result until a board declares it. */
        return OPRT_NOT_SUPPORTED;
    }
#if defined(JIELI_SELECTED_CHIP_WL82)
    if (jieli_wl82_spi_pinmux_apply(port) != OPRT_OK) {
        dev_close(s_spi_handles[port]);
        s_spi_handles[port] = NULL;
        return OPRT_COM_ERROR;
    }
#endif
#if defined(JIELI_SELECTED_CHIP_WL83)
    /* Synchronous: dev_write must not return before the bytes are out, because
     * the TKL contract has no completion callback to wait on. */
    if (dev_ioctl(s_spi_handles[port], IOCTL_SPI_SET_ASYNC_SEND, 0u) != 0) {
        dev_close(s_spi_handles[port]);
        s_spi_handles[port] = NULL;
        return OPRT_COM_ERROR;
    }
#endif
    return OPRT_OK;
}

OPERATE_RET tkl_spi_deinit(TUYA_SPI_NUM_E port)
{
    if (!jieli_spi_valid(port)) {
        return OPRT_INVALID_PARM;
    }
    if (s_spi_handles[port] == NULL) {
        return OPRT_OK;
    }
    dev_close(s_spi_handles[port]);
    s_spi_handles[port] = NULL;
    return OPRT_OK;
}

OPERATE_RET tkl_spi_send(TUYA_SPI_NUM_E port, void *data, uint32_t size)
{
    int ret;

    if (!jieli_spi_valid(port) || s_spi_handles[port] == NULL || data == NULL || size == 0u) {
        return OPRT_INVALID_PARM;
    }
    if (size > SPI_MAX_SIZE) {
        return OPRT_INVALID_PARM;
    }
    ret = dev_write(s_spi_handles[port], data, size);
    /* dev_write returns the number of bytes accepted (see device.h users and
     * the vendor video decoder). A short write is a failed TKL transfer. */
    return ret == (int)size ? OPRT_OK : OPRT_OS_ADAPTER_SPI_BUS_ERR;
}

OPERATE_RET tkl_spi_recv(TUYA_SPI_NUM_E port, void *data, uint32_t size)
{
    int ret;

    if (!jieli_spi_valid(port) || s_spi_handles[port] == NULL || data == NULL || size == 0u) {
        return OPRT_INVALID_PARM;
    }
    if (size > SPI_MAX_SIZE) {
        return OPRT_INVALID_PARM;
    }
    ret = dev_read(s_spi_handles[port], data, size);
    /* dev_read also returns a byte count; accepting a short read would leave
     * the caller with partially initialized receive data. */
    return ret == (int)size ? OPRT_OK : OPRT_OS_ADAPTER_SPI_BUS_ERR;
}

OPERATE_RET tkl_spi_transfer(TUYA_SPI_NUM_E port, void *send_buf, void *receive_buf, uint32_t length)
{
    if (length == 0u) {
        return OPRT_INVALID_PARM;
    }
    /* Full duplex is a separate vendor path (IOCTL_SPI_FDX_TR_DATA on both
     * chips, spi_fdx_data_t shape). It is used when both buffers are given and
     * the chip exposes it; otherwise this falls back to half duplex, which is
     * what the callers in this tree (display, LED pixel) actually need. */
#if defined(JIELI_SELECTED_CHIP_WL83)
    if (send_buf != NULL && receive_buf != NULL) {
        spi_fdx_data_t fdx;
        int ret;

        if (!jieli_spi_valid(port) || s_spi_handles[port] == NULL) {
            return OPRT_INVALID_PARM;
        }
        fdx.send_buf = (u8 *)send_buf;
        fdx.recv_buf = (u8 *)receive_buf;
        fdx.len = length;
        fdx.callback = NULL;
        ret = dev_ioctl(s_spi_handles[port], IOCTL_SPI_FDX_TR_DATA, (u32)(uintptr_t)&fdx);
        return ret < 0 ? OPRT_OS_ADAPTER_SPI_BUS_ERR : OPRT_OK;
    }
#endif
    if (send_buf != NULL) {
        OPERATE_RET rt = tkl_spi_send(port, send_buf, length);
        if (rt != OPRT_OK || receive_buf == NULL) {
            return rt;
        }
    }
    if (receive_buf != NULL) {
        return tkl_spi_recv(port, receive_buf, length);
    }
    return OPRT_INVALID_PARM;
}

OPERATE_RET tkl_spi_transfer_with_length(TUYA_SPI_NUM_E port, void *send_buf, uint32_t send_len, void *receive_buf,
                                         uint32_t receive_len)
{
    if (send_buf != NULL && receive_buf != NULL &&
        (send_len != receive_len || send_len == 0u)) {
        /* Full duplex requires equal lengths; unequal halves must be issued
         * as separate operations so the chip never clocks more bits than it
         * has data for. */
        OPERATE_RET rt = tkl_spi_send(port, send_buf, send_len);
        if (rt != OPRT_OK) {
            return rt;
        }
        return tkl_spi_recv(port, receive_buf, receive_len);
    }
    if (send_buf == NULL && receive_buf == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (send_buf != NULL && receive_buf != NULL) {
        return tkl_spi_transfer(port, send_buf, receive_buf, send_len);
    }
    if (send_buf != NULL) {
        return tkl_spi_send(port, send_buf, send_len);
    }
    return tkl_spi_recv(port, receive_buf, receive_len);
}

OPERATE_RET tkl_spi_abort_transfer(TUYA_SPI_NUM_E port)
{
    (void)port;
    /* Transfers are synchronous and are not interruptible from another
     * context; there is nothing left to abort by the time this returns. */
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_spi_get_status(TUYA_SPI_NUM_E port, TUYA_SPI_STATUS_T *status)
{
    (void)port;
    (void)status;
    /* No busy/error status register is surfaced through the device layer; the
     * result of the last call is its return code. */
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_spi_irq_init(TUYA_SPI_NUM_E port, TUYA_SPI_IRQ_CB cb)
{
    (void)port;
    (void)cb;
    /* Synchronous transfers have no completion event to deliver. */
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_spi_irq_enable(TUYA_SPI_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_spi_irq_disable(TUYA_SPI_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}

int32_t tkl_spi_get_data_count(TUYA_SPI_NUM_E port)
{
    (void)port;
    /* Byte-exact synchronous calls: the count is the caller's size on success
     * and there is no in-flight transfer to report between calls. */
    return 0;
}

OPERATE_RET tkl_spi_ioctl(TUYA_SPI_NUM_E port, uint32_t cmd, void *args)
{
    (void)port;
    (void)cmd;
    (void)args;
    /* No portable ioctl is defined by the TKL contract that this backend can
     * honour on both chips; CS is owned by the board or by the caller as GPIO. */
    return OPRT_NOT_SUPPORTED;
}

uint32_t tkl_spi_get_max_dma_data_length(void)
{
    /* Synchronous path: the whole buffer goes through dev_write/dev_read, whose
     * limit is the vendor's SPI_MAX_SIZE. Reported so callers can chunk. */
    return SPI_MAX_SIZE;
}
