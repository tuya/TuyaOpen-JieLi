#include "app_config.h"
#include "system/includes.h"
#include "os/os_api.h"

#include "tkl_init.h"

void tuya_app_main(void);

const struct irq_info irq_info_table[] = {
    { -1, -1, -1 },
};

const struct task_info task_info_table[] = {
    { "app_core", 15, 2048, 1024 },
    { "sys_event", 29, 512, 0 },
    { "systimer", 14, 256, 0 },
    { "sys_timer", 9, 512, 128 },
#if (defined(CONFIG_NET_ENABLE) && CONFIG_NET_ENABLE) || (defined(TCFG_WIFI_ENABLE) && TCFG_WIFI_ENABLE)
    { "tcpip_thread", 16, 800, 0 },
#endif
#if defined(TCFG_WIFI_ENABLE) && TCFG_WIFI_ENABLE
    /* Wi-Fi scan and association run on the vendor tasklet. */
    { "tasklet", 10, 1400, 0 },
    { "RtmpMlmeTask", 17, 900, 0 },
    { "RtmpCmdQTask", 17, 300, 0 },
    { "wl_rx_irq_thread", 5, 256, 0 },
#elif defined(CONFIG_WIFI_ENABLE)
    { "tasklet", 10, 1400, 0 },
    { "RtmpMlmeTask", 17, 700, 0 },
    { "RtmpCmdQTask", 17, 300, 0 },
    { "wl_rx_irq_thread", 5, 256, 0 },
#endif
#ifdef CONFIG_BT_ENABLE
#if CPU_CORE_NUM > 1
    { "#C0btctrler", 19, 512, 384 },
    { "#C0btstack", 18, 1024, 384 },
#else
    { "btctrler", 19, 512, 384 },
    { "btstack", 18, 768, 384 },
#endif
#endif
    /* Match the vendor task table: WL82 pins usb_msd0 to core 0 on dual-core
     * builds, while WL83 wifi_camera registers the unpinned name even though
     * CPU_CORE_NUM is 2. task_pc.c creates both workers by these names. */
#if defined(TCFG_USB_SLAVE_ENABLE) && TCFG_USB_SLAVE_ENABLE
#if CPU_CORE_NUM > 1 && !defined(JIELI_SELECTED_CHIP_WL83)
    { "#C0usb_msd0", 1, 512, 128 },
#else
    { "usb_msd0", 1, 512, 128 },
#endif
    { "usb_msd1", 1, 512, 128 },
#endif
    /* Video server tasks for camera and video pipeline */
    { "video_server", 16, 768, 128 },
    { "video_server_transfer", 23, 1024, 0 },
    { "video_engine_server", 10, 1024, 1024 },
    { "video_engine_task", 10, 2048, 0 },
    { "vpkg_server", 16, 512, 128 },
    { "video_dec_server", 16, 1024, 256 },
    { "vunpkg_server", 16, 1024, 128 },
    { "jpg_dec", 10, 1024, 32 },
    { "avi0", 28, 320, 64 },
    { "avi1", 28, 320, 64 },
    { "avi2", 28, 320, 64 },
    { 0, 0, 0, 0 },
};

void app_main(void)
{
    (void)tkl_init();
    tuya_app_main();
}
