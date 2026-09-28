#include "tkl_uart.h"
#include "tuya_error_code.h"

#include "device.h"
#include "uart.h"

#if defined(CONFIG_CPU_WL83)
#define JIELI_UART_SET_RECV_BLOCK IOCTL_UART_SET_RECV_BLOCK
#define JIELI_UART_SET_BAUDRATE  IOCTL_UART_SET_BAUDRATE
#define JIELI_UART_START         IOCTL_UART_START
#else
#define JIELI_UART_SET_RECV_BLOCK UART_SET_RECV_BLOCK
#define JIELI_UART_SET_BAUDRATE  UART_SET_BAUDRATE
#define JIELI_UART_START         UART_START
#endif

static void *s_uart_handles[TUYA_UART_NUM_MAX];

static const char *jieli_uart_device_name(TUYA_UART_NUM_E port_id)
{
#if defined(CONFIG_CPU_WL82)
    /* TAL CLI uses Tuya UART0, routed to hardware UART0 on the AC79 board. */
    return port_id == 0u ? "uart0" : NULL;
#else
    return port_id == 0u ? "uart1" : "uart2";
#endif
}

OPERATE_RET tkl_uart_init(TUYA_UART_NUM_E port_id, TUYA_UART_BASE_CFG_T *cfg)
{
    if (port_id >= TUYA_UART_NUM_MAX || cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    const char *device_name = jieli_uart_device_name(port_id);
    if (device_name == NULL) {
        return OPRT_NOT_SUPPORTED;
    }
    if (s_uart_handles[port_id] != NULL) {
        return OPRT_OK;
    }

    s_uart_handles[port_id] = dev_open(device_name, NULL);
    if (s_uart_handles[port_id] != NULL &&
        (dev_ioctl(s_uart_handles[port_id], JIELI_UART_SET_RECV_BLOCK, 1u) != 0 ||
         dev_ioctl(s_uart_handles[port_id], JIELI_UART_SET_BAUDRATE, cfg->baudrate) != 0 ||
         dev_ioctl(s_uart_handles[port_id], JIELI_UART_START, 0u) != 0)) {
        dev_close(s_uart_handles[port_id]);
        s_uart_handles[port_id] = NULL;
    }
    if (s_uart_handles[port_id] == NULL) {
        return OPRT_OS_ADAPTER_UART_INIT_FAILED;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_uart_deinit(TUYA_UART_NUM_E port_id)
{
    if (port_id >= TUYA_UART_NUM_MAX || s_uart_handles[port_id] == NULL) {
        return OPRT_INVALID_PARM;
    }
    dev_close(s_uart_handles[port_id]);
    s_uart_handles[port_id] = NULL;
    return OPRT_OK;
}

int tkl_uart_write(TUYA_UART_NUM_E port_id, void *buff, uint16_t len)
{
    if (port_id >= TUYA_UART_NUM_MAX || s_uart_handles[port_id] == NULL || buff == NULL) {
        return OPRT_INVALID_PARM;
    }
    int ret = dev_write(s_uart_handles[port_id], buff, len);
    return ret < 0 ? OPRT_OS_ADAPTER_UART_SEND_FAILED : ret;
}

void tkl_uart_rx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB rx_cb)
{
    (void)port_id;
    (void)rx_cb;
}

void tkl_uart_tx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB tx_cb)
{
    (void)port_id;
    (void)tx_cb;
}

int tkl_uart_read(TUYA_UART_NUM_E port_id, void *buff, uint16_t len)
{
    if (port_id >= TUYA_UART_NUM_MAX || s_uart_handles[port_id] == NULL || buff == NULL) {
        return OPRT_INVALID_PARM;
    }
    int ret = dev_read(s_uart_handles[port_id], buff, len);
    return ret < 0 ? OPRT_OS_ADAPTER_UART_READ_FAILED : ret;
}

OPERATE_RET tkl_uart_set_tx_int(TUYA_UART_NUM_E port_id, BOOL_T enable)
{
    (void)port_id;
    (void)enable;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_set_rx_flowctrl(TUYA_UART_NUM_E port_id, BOOL_T enable)
{
    (void)port_id;
    (void)enable;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_wait_for_data(TUYA_UART_NUM_E port_id, int timeout_ms)
{
    (void)port_id;
    (void)timeout_ms;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_ioctl(TUYA_UART_NUM_E port_id, uint32_t cmd, void *arg)
{
    (void)port_id;
    (void)cmd;
    (void)arg;
    return OPRT_NOT_SUPPORTED;
}
