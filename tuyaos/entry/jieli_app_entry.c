#include "app_config.h"
#include "system/includes.h"
#include "os/os_api.h"
#include "event/event.h"
#include "event/key_event.h"

#include "tkl_init.h"

void tuya_app_main(void);

extern void ai_chat_jieli_key_event(int event) __attribute__((weak));

static void jieli_ai_key_event_handler(struct sys_event *event)
{
    struct key_event *key;

    if (event == NULL || event->type != SYS_KEY_EVENT || event->payload == NULL) return;
    key = (struct key_event *)event->payload;
    if (key->value != KEY_K1) return;
    if (ai_chat_jieli_key_event == NULL) return;

    switch (key->action) {
    case KEY_EVENT_HOLD:
        ai_chat_jieli_key_event(5); /* TDL_BUTTON_LONG_PRESS_START */
        break;
    case KEY_EVENT_UP:
        ai_chat_jieli_key_event(1); /* TDL_BUTTON_PRESS_UP */
        break;
    case KEY_EVENT_CLICK:
        ai_chat_jieli_key_event(2); /* TDL_BUTTON_PRESS_SINGLE_CLICK */
        break;
    default:
        break;
    }
}
#ifdef CONFIG_MEDIA_ENABLE
OPERATE_RET tkl_jieli_audio_prepare(void);
#endif

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
    { 0, 0, 0, 0 },
};

void app_main(void)
{
#ifdef CONFIG_MEDIA_ENABLE
    OPERATE_RET audio_ret = tkl_jieli_audio_prepare();
    if (audio_ret != 0) {
        printf("[JIELI_AUDIO] audio prepare failed: %d\n", audio_ret);
    }
#endif
    (void)tkl_init();
    (void)register_sys_event_handler(SYS_KEY_EVENT, 0, 1, jieli_ai_key_event_handler);
    tuya_app_main();
}
