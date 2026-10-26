#include "tkl_output.h"
#include "tuya_error_code.h"

#include <stdarg.h>
#include <stdio.h>

#if defined(__has_include)
#if __has_include("board_com_api.h")
#include "board_com_api.h"
#endif
#endif

#if (defined(ENABLE_JIELI_USB_CDC_LOG) && ENABLE_JIELI_USB_CDC_LOG) || \
    (defined(CONFIG_ENABLE_JIELI_USB_CDC_LOG) && CONFIG_ENABLE_JIELI_USB_CDC_LOG)
#define USE_JIELI_USB_CDC_LOG 1
extern unsigned int cdc_write_data(int usb_id, const unsigned char *buf, unsigned int len);
#else
#define USE_JIELI_USB_CDC_LOG 0
#endif

#define BOOT_LOG_BUF_SIZE (8 * 1024)
static char s_boot_log_buf[BOOT_LOG_BUF_SIZE];
static uint32_t s_boot_log_len = 0;
static int s_boot_log_dumped = 0;

void tkl_log_dump_boot_history(void)
{
    if (s_boot_log_len > 0) {
        cdc_write_data(0, (const unsigned char *)"\r\n=== BOOT LOG REPLAY START ===\r\n", 33);
        cdc_write_data(1, (const unsigned char *)"\r\n=== BOOT LOG REPLAY START ===\r\n", 33);
        cdc_write_data(0, (const unsigned char *)s_boot_log_buf, s_boot_log_len);
        cdc_write_data(1, (const unsigned char *)s_boot_log_buf, s_boot_log_len);
        cdc_write_data(0, (const unsigned char *)"\r\n=== BOOT LOG REPLAY END ===\r\n", 31);
        cdc_write_data(1, (const unsigned char *)"\r\n=== BOOT LOG REPLAY END ===\r\n", 31);
        s_boot_log_dumped = 1;
    }
}

void tkl_log_output(const char *format, ...)
{
    va_list args;

#if USE_JIELI_USB_CDC_LOG
    char log_buf[512];
    va_start(args, format);
    int len = vsnprintf(log_buf, sizeof(log_buf), format, args);
    va_end(args);

    if (len > 0) {
        unsigned int write_len = (len < (int)sizeof(log_buf)) ? (unsigned int)len : (sizeof(log_buf) - 1);
        if (!s_boot_log_dumped && s_boot_log_len + write_len < BOOT_LOG_BUF_SIZE) {
            memcpy(s_boot_log_buf + s_boot_log_len, log_buf, write_len);
            s_boot_log_len += write_len;
        }
        cdc_write_data(0, (const unsigned char *)log_buf, write_len);
        cdc_write_data(1, (const unsigned char *)log_buf, write_len);
    }
#endif
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

OPERATE_RET tkl_log_close(void)
{
    return OPRT_OK;
}

OPERATE_RET tkl_log_open(void)
{
    return OPRT_OK;
}
