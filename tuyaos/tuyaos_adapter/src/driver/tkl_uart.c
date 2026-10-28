#include "tkl_uart.h"
#include "tkl_semaphore.h"
#include "tkl_system.h"
#include "tkl_thread.h"
#include "tuya_error_code.h"

#include <stdint.h>

#include "device.h"
#include "uart.h"

#define TKL_UART_PORT_MAX 2

/* The SDK receives into a registered circular buffer. Its uart_test.c uses a
 * 1 KiB, 32-byte-aligned buffer; opening without one leaves RX without storage.
 */
#define TKL_UART_RX_BUFFER_SIZE 1024

static uint8_t s_uart_rx_buffer[TKL_UART_PORT_MAX][TKL_UART_RX_BUFFER_SIZE] __attribute__((aligned(32)));

enum jieli_uart_ioctl_cmd {
    JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_ADDR,
    JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_LENTH,
    JIELI_UART_IOCTL_SET_RECV_BLOCK,
    JIELI_UART_IOCTL_SET_BAUDRATE,
    JIELI_UART_IOCTL_SET_IRQ_EVENT_CB,
    JIELI_UART_IOCTL_START,
    JIELI_UART_IOCTL_GET_RECV_CNT,
};

static void *s_uart_handles[TKL_UART_PORT_MAX];

/* Set while the receive task should keep running; shared by both chips because
 * tkl_uart_deinit() clears it whichever backend is built. */
static volatile uint8_t s_uart_rx_active[TKL_UART_PORT_MAX];

#if defined(JIELI_SELECTED_CHIP_WL82)
extern OPERATE_RET jieli_wl82_uart0_pinmux_apply(void);
#endif

#if defined(JIELI_SELECTED_CHIP_WL83)
/* ISR only posts a semaphore: dev_read() takes a mutex and is unsafe in IRQ
 * context. Separate trampolines identify the port; RX and OT cover bursts and
 * single-character input respectively.
 */
#define TKL_UART_RX_TASK_STACK 2048

static TUYA_UART_IRQ_CB s_uart_rx_cb[TKL_UART_PORT_MAX];
static TKL_SEM_HANDLE s_uart_rx_sem[TKL_UART_PORT_MAX];
static TKL_THREAD_HANDLE s_uart_rx_thread[TKL_UART_PORT_MAX];

static void jieli_uart_irq_dispatch(uint8_t port_id, uart_irq_event_t event)
{
    if (event != UART_IRQ_EVENT_RX && event != UART_IRQ_EVENT_OT) {
        return;
    }
    if (s_uart_rx_sem[port_id] != NULL) {
        (void)tkl_semaphore_post(s_uart_rx_sem[port_id]);
    }
}

static void jieli_uart_irq_event_cb_0(uart_irq_event_t event) { jieli_uart_irq_dispatch(0u, event); }
static void jieli_uart_irq_event_cb_1(uart_irq_event_t event) { jieli_uart_irq_dispatch(1u, event); }

static void (*const s_uart_irq_event_cb[TKL_UART_PORT_MAX])(uart_irq_event_t) = {
    jieli_uart_irq_event_cb_0,
    jieli_uart_irq_event_cb_1,
};

/* Runs the TAL receive callback in task context. TAL's callback drains the port
 * with tkl_uart_read(), which is why the port is opened non-blocking. */
static void jieli_uart_rx_task(void *arg)
{
    uint8_t port_id = (uint8_t)(uintptr_t)arg;

    while (s_uart_rx_active[port_id]) {
        if (tkl_semaphore_wait(s_uart_rx_sem[port_id], TKL_SEM_WAIT_FOREVER) != OPRT_OK) {
            continue;
        }
        if (!s_uart_rx_active[port_id]) {
            break;
        }
        if (s_uart_rx_cb[port_id] != NULL) {
            s_uart_rx_cb[port_id]((TUYA_UART_NUM_E)port_id);
        }
    }
    /* Allow a later init/registration to start a fresh task. */
    s_uart_rx_thread[port_id] = NULL;
}
#else /* JIELI_SELECTED_CHIP_WL82 */
/* WL82 exposes no IRQ-event callback. Poll unread-byte count every 10 ms and
 * notify TAL in task context; the SDK's awtk caller confirms GET_RECV_CNT uses
 * a pointer argument. This adds polling latency; bytes remain buffered until
 * TAL drains them. The timeout callback is unused because its context is not
 * established and TAL callbacks take a mutex.
 */
#define TKL_UART_RX_POLL_PERIOD_MS 10
#define TKL_UART_RX_TASK_STACK     2048

static TUYA_UART_IRQ_CB s_uart_rx_cb[TKL_UART_PORT_MAX];
static TKL_THREAD_HANDLE s_uart_rx_thread[TKL_UART_PORT_MAX];

/* jieli_uart_wl82_recv_cnt() and jieli_uart_poll_task() are defined after
 * jieli_uart_ioctl(), which they call. */
#endif

static const char *jieli_uart_device_name(TUYA_UART_NUM_E port_id)
{
    if (port_id == 0u) {
        return "uart0";
    }
#if defined(JIELI_SELECTED_CHIP_WL83)
    if (port_id == 1u) {
        return "uart2";
    }
#endif
    return NULL;
}

static uint32_t jieli_uart_ioctl(enum jieli_uart_ioctl_cmd cmd)
{
#if defined(JIELI_SELECTED_CHIP_WL83)
    switch (cmd) {
    case JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_ADDR:
        return IOCTL_UART_SET_CIRCULAR_BUFF_ADDR;
    case JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_LENTH:
        return IOCTL_UART_SET_CIRCULAR_BUFF_LENTH;
    case JIELI_UART_IOCTL_SET_RECV_BLOCK:
        return IOCTL_UART_SET_RECV_BLOCK;
    case JIELI_UART_IOCTL_SET_BAUDRATE:
        return IOCTL_UART_SET_BAUDRATE;
    case JIELI_UART_IOCTL_SET_IRQ_EVENT_CB:
        return IOCTL_UART_SET_IRQ_EVENT_CB;
    case JIELI_UART_IOCTL_START:
        return IOCTL_UART_START;
    case JIELI_UART_IOCTL_GET_RECV_CNT:
        return IOCTL_UART_GET_RECV_CNT;
    default:
        return 0u;
    }
#elif defined(JIELI_SELECTED_CHIP_WL82)
    switch (cmd) {
    case JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_ADDR:
        return UART_SET_CIRCULAR_BUFF_ADDR;
    case JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_LENTH:
        return UART_SET_CIRCULAR_BUFF_LENTH;
    case JIELI_UART_IOCTL_SET_RECV_BLOCK:
        return UART_SET_RECV_BLOCK;
    case JIELI_UART_IOCTL_SET_BAUDRATE:
        return UART_SET_BAUDRATE;
    case JIELI_UART_IOCTL_START:
        return UART_START;
    case JIELI_UART_IOCTL_GET_RECV_CNT:
        return UART_GET_RECV_CNT;
    default:
        return 0u;
    }
#else
#error "Jieli UART mapping requires a selected chip"
#endif
}

#if defined(JIELI_SELECTED_CHIP_WL82)
static int jieli_uart_wl82_recv_cnt(TUYA_UART_NUM_E port_id, uint32_t *cnt)
{
    if (s_uart_handles[port_id] == NULL) {
        return -1;
    }
    return dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_GET_RECV_CNT),
                     (u32)(uintptr_t)cnt);
}

static void jieli_uart_poll_task(void *arg)
{
    uint8_t port_id = (uint8_t)(uintptr_t)arg;
    uint32_t last = 0;

    while (s_uart_rx_active[port_id]) {
        uint32_t cnt = 0;

        if (jieli_uart_wl82_recv_cnt((TUYA_UART_NUM_E)port_id, &cnt) != 0) {
            /* The query is not answering; leave the notifier idle rather than
             * spin on it. */
            (void)tkl_system_sleep(TKL_UART_RX_POLL_PERIOD_MS);
            continue;
        }
        if (cnt != last && s_uart_rx_cb[port_id] != NULL) {
            s_uart_rx_cb[port_id]((TUYA_UART_NUM_E)port_id);
            /* TAL drained the ring in the callback; re-baseline instead of
             * assuming it is empty. */
            last = 0;
            if (jieli_uart_wl82_recv_cnt((TUYA_UART_NUM_E)port_id, &last) != 0) {
                last = 0;
            }
        } else {
            last = cnt;
        }
        (void)tkl_system_sleep(TKL_UART_RX_POLL_PERIOD_MS);
    }
    s_uart_rx_thread[port_id] = NULL;
}
#endif

OPERATE_RET tkl_uart_init(TUYA_UART_NUM_E port_id, TUYA_UART_BASE_CFG_T *cfg)
{
    const char *device_name;

    if (port_id >= TUYA_UART_NUM_MAX || port_id >= TKL_UART_PORT_MAX || cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    /* The registered JieLi UART device API only exposes baud-rate control;
     * framing and hardware flow control are fixed by the SDK. Do not report a
     * successful init for settings the backend cannot apply. */
    if (cfg->baudrate == 0u || cfg->databits != TUYA_UART_DATA_LEN_8BIT ||
        cfg->stopbits != TUYA_UART_STOP_LEN_1BIT ||
        cfg->parity != TUYA_UART_PARITY_TYPE_NONE || cfg->flowctrl != TUYA_UART_FLOWCTRL_NONE) {
        return OPRT_NOT_SUPPORTED;
    }
    device_name = jieli_uart_device_name(port_id);
    if (device_name == NULL) {
        return OPRT_NOT_SUPPORTED;
    }
    if (s_uart_handles[port_id] != NULL) {
        return OPRT_OK;
    }

    s_uart_handles[port_id] = dev_open(device_name, NULL);
#if defined(JIELI_SELECTED_CHIP_WL82)
    /* dev_open() runs the SDK's board default pin setup. Re-apply a Tuya
     * pinmux selection made before tkl_uart_init() and before UART_START. */
    if (s_uart_handles[port_id] != NULL && port_id == TUYA_UART_NUM_0 &&
        jieli_wl82_uart0_pinmux_apply() != OPRT_OK) {
        dev_close(s_uart_handles[port_id]);
        s_uart_handles[port_id] = NULL;
        return OPRT_COM_ERROR;
    }
#endif
    if (s_uart_handles[port_id] != NULL &&
        (dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_ADDR),
                   (u32)(uintptr_t)s_uart_rx_buffer[port_id]) != 0 ||
         dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_SET_CIRCULAR_BUFF_LENTH),
                   (u32)sizeof(s_uart_rx_buffer[port_id])) != 0 ||
         /* Non-blocking: TAL's receive callback drains the port with
          * tkl_uart_read() until it returns no more bytes, so a blocking
          * dev_read would hang on the second iteration. TAL's own
          * tal_uart_read() blocks on its semaphore instead. */
         dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_SET_RECV_BLOCK), 0u) != 0 ||
         dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_SET_BAUDRATE), cfg->baudrate) != 0 ||
         dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_START), 0u) != 0)) {
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
    if (port_id >= TUYA_UART_NUM_MAX || port_id >= TKL_UART_PORT_MAX || s_uart_handles[port_id] == NULL) {
        return OPRT_INVALID_PARM;
    }

    /* Stop the receive task before the handle goes away. Without this the wl83
     * task waits forever on a semaphore the closed port can no longer post,
     * and the wl82 poll task keeps waking every 10 ms against a NULL handle -
     * a leaked thread in both cases, and a callback that could still fire into
     * a caller that believes it has deinitialised. */
    s_uart_rx_active[port_id] = 0;
#if defined(JIELI_SELECTED_CHIP_WL83)
    /* The task is blocked in tkl_semaphore_wait(); post once so it wakes, sees
     * the flag and exits. The vendor callback is cleared too, so no further
     * posts arrive after close. */
    if (s_uart_rx_sem[port_id] != NULL) {
        (void)tkl_semaphore_post(s_uart_rx_sem[port_id]);
    }
    (void)dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_SET_IRQ_EVENT_CB), 0u);
#endif
    s_uart_rx_cb[port_id] = NULL;

    /* Wait (bounded) for the task to actually exit before the handle goes away.
     * Without a join, a quick init+register inside the task's exit window sees
     * a non-NULL thread handle, skips creating a new task, and leaves the port
     * with a callback registered but no task to service it - received bytes are
     * then silently dropped. The task clears its own handle on the way out. */
    {
        uint32_t spins = 0;
        while (s_uart_rx_thread[port_id] != NULL && spins < 20u) {
            (void)tkl_system_sleep(5);
            spins++;
        }
        /* The callback runs in this worker and may still be using the UART
         * handle. Keep the handle alive if it does not exit in time; callers
         * can retry deinit after the worker observes the stop request. */
        if (s_uart_rx_thread[port_id] != NULL) {
            return OPRT_COM_ERROR;
        }
    }

    dev_close(s_uart_handles[port_id]);
    s_uart_handles[port_id] = NULL;
    return OPRT_OK;
}

int tkl_uart_write(TUYA_UART_NUM_E port_id, void *buff, uint16_t len)
{
    if (port_id >= TUYA_UART_NUM_MAX || port_id >= TKL_UART_PORT_MAX || s_uart_handles[port_id] == NULL ||
        buff == NULL) {
        return OPRT_INVALID_PARM;
    }
    int ret = dev_write(s_uart_handles[port_id], buff, len);
    return ret < 0 ? OPRT_OS_ADAPTER_UART_SEND_FAILED : ret;
}

void tkl_uart_rx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB rx_cb)
{
    if (port_id >= TUYA_UART_NUM_MAX || port_id >= TKL_UART_PORT_MAX) {
        return;
    }
#if defined(JIELI_SELECTED_CHIP_WL83)
    /* TAL calls this after tkl_uart_init(), so the handle is already open and
     * START has already run; the vendor ioctl only stores the callback pointer.
     * The wl82 SDK exposes no interrupt-event callback at all, so receive
     * notification stays unwired on that chip. */
    s_uart_rx_cb[port_id] = rx_cb;
    if (rx_cb == NULL || s_uart_handles[port_id] == NULL) {
        return;
    }
    /* Publish "should run" before anything else: if a previous task is still
     * exiting, it re-reads this flag and keeps running rather than leaving the
     * port with no consumer. */
    s_uart_rx_active[port_id] = 1;
    if (s_uart_rx_sem[port_id] == NULL &&
        tkl_semaphore_create_init(&s_uart_rx_sem[port_id], 0u, 1u) != OPRT_OK) {
        s_uart_rx_sem[port_id] = NULL;
        s_uart_rx_active[port_id] = 0;
        return;
    }
    if (s_uart_rx_thread[port_id] == NULL) {
        if (tkl_thread_create(&s_uart_rx_thread[port_id], "tuya_uart_rx", TKL_UART_RX_TASK_STACK, 3u,
                              jieli_uart_rx_task, (void *)(uintptr_t)port_id) != OPRT_OK) {
            s_uart_rx_active[port_id] = 0;
            s_uart_rx_thread[port_id] = NULL;
            return;
        }
    }
    (void)dev_ioctl(s_uart_handles[port_id], jieli_uart_ioctl(JIELI_UART_IOCTL_SET_IRQ_EVENT_CB),
                    (u32)(uintptr_t)s_uart_irq_event_cb[port_id]);
#else
    s_uart_rx_cb[port_id] = rx_cb;
    if (rx_cb == NULL || s_uart_handles[port_id] == NULL) {
        return;
    }
    if (s_uart_rx_thread[port_id] == NULL) {
        s_uart_rx_active[port_id] = 1;
        if (tkl_thread_create(&s_uart_rx_thread[port_id], "tuya_uart_rx", TKL_UART_RX_TASK_STACK, 4u,
                              jieli_uart_poll_task, (void *)(uintptr_t)port_id) != OPRT_OK) {
            s_uart_rx_active[port_id] = 0;
            s_uart_rx_thread[port_id] = NULL;
        }
    } else {
        /* A previous task is still winding down; keep it alive rather than
         * leaving the port with a callback and no poller. */
        s_uart_rx_active[port_id] = 1;
    }
#endif
}

void tkl_uart_tx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB tx_cb)
{
    (void)port_id;
    (void)tx_cb;
}

int tkl_uart_read(TUYA_UART_NUM_E port_id, void *buff, uint16_t len)
{
    if (port_id >= TUYA_UART_NUM_MAX || port_id >= TKL_UART_PORT_MAX || s_uart_handles[port_id] == NULL ||
        buff == NULL) {
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
