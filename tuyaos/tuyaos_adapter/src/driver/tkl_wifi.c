#include "tkl_wifi.h"
#include "tkl_memory.h"
#include "tkl_system.h"
#include "tkl_thread.h"
#include "tkl_queue.h"
#include "tuya_error_code.h"

#include "asm/sfc_norflash_api.h"
#include "lwip/port/lwip.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static WIFI_EVENT_CB s_wifi_event_cb;
static WF_WK_MD_E s_wifi_mode = WWM_STATION;
static uint8_t s_wifi_mac[6];
static uint8_t s_mac_initialized;

#define JIELI_FLASH_UID_LEN 16U

/*
 * Both SDKs expose the same UID type through this header; only the WL83 call
 * takes a flash index. Keep that small compile-time difference in the Wi-Fi
 * TKL so no private cross-file prototype is needed.
 */
static int jieli_read_flash_uid(uint8_t uid[JIELI_FLASH_UID_LEN])
{
    const uint8_t *sdk_uid;

    if (uid == NULL) {
        return -1;
    }
#if defined(JIELI_SELECTED_CHIP_WL82)
    sdk_uid = get_norflash_uuid();
#elif defined(JIELI_SELECTED_CHIP_WL83)
    sdk_uid = get_norflash_uuid(0);
#else
#error "Jieli Wi-Fi MAC requires a selected chip"
#endif
    if (sdk_uid == NULL) {
        return -1;
    }
    memcpy(uid, sdk_uid, JIELI_FLASH_UID_LEN);
    return 0;
}

static int jieli_bytes_are(const uint8_t *bytes, uint32_t length, uint8_t value)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        if (bytes[i] != value) {
            return 0;
        }
    }
    return 1;
}

static int jieli_mac_is_unicast(const uint8_t mac[6])
{
    return mac != NULL && !jieli_bytes_are(mac, 6, 0x00) && !jieli_bytes_are(mac, 6, 0xFF) &&
           (mac[0] & 0x01U) == 0U;
}

static uint64_t jieli_uid_hash(const uint8_t uid[JIELI_FLASH_UID_LEN])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t i;
    static const uint8_t domain[] = "TuyaOpen-JieLi-dev-mac-v1";

    for (i = 0; i < sizeof(domain) - 1U; i++) {
        hash ^= domain[i];
        hash *= UINT64_C(1099511628211);
    }
    for (i = 0; i < JIELI_FLASH_UID_LEN; i++) {
        hash ^= uid[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int jieli_mac_initialize(void)
{
    uint8_t uid[JIELI_FLASH_UID_LEN];
    uint64_t hash;
    uint32_t i;

    if (s_mac_initialized) {
        return 0;
    }
    if (jieli_read_flash_uid(uid) != 0 || jieli_bytes_are(uid, sizeof(uid), 0x00) ||
        jieli_bytes_are(uid, sizeof(uid), 0xFF)) {
        return -1;
    }

    hash = jieli_uid_hash(uid);
    for (i = 0; i < 6U; i++) {
        s_wifi_mac[i] = (uint8_t)(hash >> (40U - (i * 8U)));
    }
    /* IEEE 802 locally administered unicast address for development Wi-Fi. */
    s_wifi_mac[0] = (uint8_t)((s_wifi_mac[0] & 0xFCU) | 0x02U);
    memset(uid, 0, sizeof(uid));
    s_mac_initialized = 1;
    printf("[JIELI][MAC] dev UID-derived WiFi %02X:%02X:%02X:%02X:%02X:%02X\n",
           s_wifi_mac[0], s_wifi_mac[1], s_wifi_mac[2], s_wifi_mac[3], s_wifi_mac[4], s_wifi_mac[5]);
    return 0;
}

static int jieli_mac_get_wifi(uint8_t mac[6])
{
    if (mac == NULL || jieli_mac_initialize() != 0) {
        return -1;
    }
    memcpy(mac, s_wifi_mac, sizeof(s_wifi_mac));
    return 0;
}

static int jieli_mac_set_wifi(const uint8_t mac[6])
{
    if (!jieli_mac_is_unicast(mac) || jieli_mac_initialize() != 0) {
        return -1;
    }
    memcpy(s_wifi_mac, mac, sizeof(s_wifi_mac));
    return 0;
}

/*
 * netmgr starts a single 20s watchdog when it calls station_connect, and this
 * adapter only reports WFE_CONNECTED once DHCP has completed, so association
 * and DHCP share that one budget. Keep their sum below 20s, with margin for
 * the events to be delivered: 12 + 5 = 17s. That still leaves the native
 * scan/association enough time to find an intermittently visible AP.
 */
#define JIELI_WIFI_STA_CONNECT_TIMEOUT_SEC 12
#define JIELI_WIFI_DHCP_TIMEOUT_SEC 5

/* wifi_connect.h is not included here because its wifi_def.h uses C++ enum
 * syntax, so declare this C-compatible SDK entry point directly. */
extern void wifi_set_sta_connect_timeout(int sec);

enum jieli_wifi_event {
    JIELI_WIFI_MODULE_INIT,
    JIELI_WIFI_MODULE_START,
    JIELI_WIFI_MODULE_STOP,
    JIELI_WIFI_MODULE_START_ERR,
    JIELI_WIFI_AP_START,
    JIELI_WIFI_AP_STOP,
    JIELI_WIFI_STA_START,
    JIELI_WIFI_STA_STOP,
    JIELI_WIFI_STA_SCAN_COMPLETED,
    JIELI_WIFI_STA_CONNECT_SUCC,
    JIELI_WIFI_STA_CONNECT_NO_SSID,
    JIELI_WIFI_STA_CONNECT_ASSOC_FAIL,
    JIELI_WIFI_STA_CONNECT_ASSOC_TIMEOUT,
    JIELI_WIFI_STA_DISCONNECT,
    JIELI_WIFI_SMP_CFG_START,
    JIELI_WIFI_SMP_CFG_STOP,
    JIELI_WIFI_SMP_CFG_TIMEOUT,
    JIELI_WIFI_SMP_CFG_COMPLETED,
    JIELI_WIFI_DHCP_SUCC,
    JIELI_WIFI_DHCP_TIMEOUT,
};

enum jieli_wifi_auth_mode {
    JIELI_WIFI_AUTH_OPEN,
    JIELI_WIFI_AUTH_WEP,
    JIELI_WIFI_AUTH_WAPI,
    JIELI_WIFI_AUTH_WPA,
    JIELI_WIFI_AUTH_WPA2,
    JIELI_WIFI_AUTH_WPA_WPA2,
    JIELI_WIFI_AUTH_WPA3,
};

struct jieli_wifi_scan_info {
    unsigned char ssid_len;
    char ssid[33];
    unsigned char mac_addr[6];
    char rssi;
    char snr;
    char rssi_db;
    char rssi_rsv;
    char channel_number;
    unsigned char signal_strength;
    unsigned char signal_quality;
    unsigned char supported_rates[16];
    /* wifi_def.h declares WIFI_802_11_AUTH_MODE with an 8-bit underlying
     * type.  Do not use a C enum here: on this toolchain a plain enum is
     * 32-bit and would shift the stride of every result after the first AP. */
    uint8_t auth_mode;
};

struct jieli_wifi_mode_info {
    int mode;
    char *ssid;
    char *pwd;
};

extern void wifi_set_event_callback(int (*cb)(void *, int));
extern int wifi_on(void);
extern int wifi_is_on(void);
extern int wifi_off(void);
extern void wifi_set_connect_sta_block(int block);
extern int wifi_get_mac(unsigned char *mac);
extern int wifi_set_mac(char *mac);
extern unsigned int wifi_get_channel(void);
extern void wifi_set_channel(unsigned char channel);
extern void wifi_get_bssid(unsigned char bssid[6]);
extern void wifi_get_mode_cur_info(struct jieli_wifi_mode_info *info);
extern int wifi_enter_sta_mode(char *ssid, char *password);
extern int wifi_enter_ap_mode(char *ssid, char *password);
extern void wifi_set_sta_connect_best_ssid(unsigned char enable);
extern int wifi_scan_req(void);
extern struct jieli_wifi_scan_info *wifi_get_scan_result(unsigned int *count);
extern void wifi_clear_scan_result(void);
extern char wifi_get_rssi(void);
extern int wifi_enter_smp_cfg_mode(void);
extern int wifi_get_sta_connect_state(void);

static OPERATE_RET jieli_result(int result)
{
    return (result == 0) ? OPRT_OK : OPRT_COM_ERROR;
}

static int jieli_parse_ipv4(const char *text, uint8_t address[4])
{
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    char tail;

    if (!text || !address || sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 ||
        c > 255 || d > 255) {
        return -1;
    }
    address[0] = (uint8_t)a;
    address[1] = (uint8_t)b;
    address[2] = (uint8_t)c;
    address[3] = (uint8_t)d;
    return 0;
}

static int jieli_set_ap_lan_info(const WF_AP_CFG_IF_S *cfg)
{
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    struct lan_setting setting = {0};

    if (jieli_parse_ipv4(cfg->ip.ip, ip) != 0 || jieli_parse_ipv4(cfg->ip.mask, mask) != 0 ||
        jieli_parse_ipv4(cfg->ip.gw, gateway) != 0 || ip[3] == 255) {
        return -1;
    }

    setting.WIRELESS_IP_ADDR0 = ip[0];
    setting.WIRELESS_IP_ADDR1 = ip[1];
    setting.WIRELESS_IP_ADDR2 = ip[2];
    setting.WIRELESS_IP_ADDR3 = ip[3];
    setting.WIRELESS_NETMASK0 = mask[0];
    setting.WIRELESS_NETMASK1 = mask[1];
    setting.WIRELESS_NETMASK2 = mask[2];
    setting.WIRELESS_NETMASK3 = mask[3];
    setting.WIRELESS_GATEWAY0 = gateway[0];
    setting.WIRELESS_GATEWAY1 = gateway[1];
    setting.WIRELESS_GATEWAY2 = gateway[2];
    setting.WIRELESS_GATEWAY3 = gateway[3];
    setting.SERVER_IPADDR1 = ip[0];
    setting.SERVER_IPADDR2 = ip[1];
    setting.SERVER_IPADDR3 = ip[2];
    setting.SERVER_IPADDR4 = ip[3];
    setting.CLIENT_IPADDR1 = ip[0];
    setting.CLIENT_IPADDR2 = ip[1];
    setting.CLIENT_IPADDR3 = ip[2];
    setting.CLIENT_IPADDR4 = (uint8_t)(ip[3] + 1);
    setting.SUB_NET_MASK1 = mask[0];
    setting.SUB_NET_MASK2 = mask[1];
    setting.SUB_NET_MASK3 = mask[2];
    setting.SUB_NET_MASK4 = mask[3];
    return net_set_lan_info(&setting);
}

static int jieli_wifi_event_cb(void *priv, int event);

/* Resident STA worker infrastructure (defined next to
 * tkl_wifi_station_connect); started early from tkl_wifi_init(). */
static OPERATE_RET jieli_wifi_worker_start(void);

static int jieli_wifi_event_cb(void *priv, int event)
{
    (void)priv;
    printf("[JIELI][WIFI] native event:%d\n", event);
    if (!s_wifi_event_cb) {
        return 0;
    }

    switch (event) {
    case JIELI_WIFI_MODULE_INIT:
        /* tkl_wifi_init() defers the native start, so apply the vendor-module
         * knobs here, on the first real wifi_on() from start_ap/station_connect.
        * Keep connect non-blocking and the best-SSID auto reconnect off so
         * provisioning credentials are the only thing the module dials. */
        wifi_set_connect_sta_block(0);
        wifi_set_sta_connect_best_ssid(0);
        /* Bound association + DHCP failure reporting before Tuya's 20s retry. */
        wifi_set_sta_connect_timeout(JIELI_WIFI_STA_CONNECT_TIMEOUT_SEC);
        lwip_set_dhcp_timeout(JIELI_WIFI_DHCP_TIMEOUT_SEC);
        break;
    case JIELI_WIFI_AP_START:
        /* The AP transition is asynchronous and may restore STA auto-connect. */
        wifi_set_sta_connect_best_ssid(0);
        break;
    case JIELI_WIFI_DHCP_SUCC:
        /* Association success only means the link is up. Tuya cloud needs a
         * valid IP/DNS route, so report WFE_CONNECTED only after DHCP. */
        s_wifi_event_cb(WFE_CONNECTED, NULL);
        break;
    case JIELI_WIFI_STA_CONNECT_NO_SSID:
    case JIELI_WIFI_STA_CONNECT_ASSOC_FAIL:
    case JIELI_WIFI_STA_CONNECT_ASSOC_TIMEOUT:
        s_wifi_event_cb(WFE_CONNECT_FAILED, NULL);
        break;
    case JIELI_WIFI_STA_DISCONNECT:
    case JIELI_WIFI_DHCP_TIMEOUT:
    case JIELI_WIFI_MODULE_STOP:
        s_wifi_event_cb(WFE_DISCONNECTED, NULL);
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t jieli_auth_mode(uint8_t mode)
{
    switch (mode) {
    case JIELI_WIFI_AUTH_OPEN:
        return WAAM_OPEN;
    case JIELI_WIFI_AUTH_WEP:
        return WAAM_WEP;
    case JIELI_WIFI_AUTH_WPA:
        return WAAM_WPA_PSK;
    case JIELI_WIFI_AUTH_WPA2:
        return WAAM_WPA2_PSK;
    case JIELI_WIFI_AUTH_WPA_WPA2:
        return WAAM_WPA_WPA2_PSK;
    case JIELI_WIFI_AUTH_WPA3:
        return WAAM_WPA_WPA3_SAE;
    default:
        return WAAM_UNKNOWN;
    }
}

OPERATE_RET tkl_wifi_init(WIFI_EVENT_CB cb)
{
    OPERATE_RET rt;

    s_wifi_event_cb = cb;
    wifi_set_event_callback(jieli_wifi_event_cb);
    /* Keep the initial WiFi start from reconnecting an empty/stale STA entry
     * while Tuya netcfg is using BLE provisioning.  Starting the native WiFi
     * task here triggers a periodic empty-SSID scan that starves the wl82 BLE
     * controller during the first large provisioning write.  Start WiFi
     * lazily from the STA/AP entry points below. */
    wifi_set_sta_connect_best_ssid(0);
    /* Bring the resident STA worker up in this early, quiet window; creating
     * it later from the BLE workqueue crashed the FreeRTOS list code. */
    rt = jieli_wifi_worker_start();
    if (rt != OPRT_OK) {
        return rt;
    }
    printf("[JIELI][WIFI] init registered, WiFi start is deferred\n");
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_scan_ap(const int8_t *ssid, AP_IF_S **ap_ary, uint32_t *num)
{
    uint32_t count = 0;
    struct jieli_wifi_scan_info *result;
    AP_IF_S *aps;
    uint32_t out_count = 0;

    if (!ap_ary || !num) {
        return OPRT_INVALID_PARM;
    }
    *ap_ary = NULL;
    *num = 0;

    if (wifi_scan_req() != 0) {
        return OPRT_COM_ERROR;
    }
    for (uint32_t retry = 0; retry < 100; ++retry) {
        result = wifi_get_scan_result(&count);
        if (result && count) {
            break;
        }
        tkl_system_delay(100);
    }
    if (!result || count == 0) {
        wifi_clear_scan_result();
        return OPRT_OK;
    }

    aps = (AP_IF_S *)tkl_system_calloc(count, sizeof(AP_IF_S));
    if (!aps) {
        wifi_clear_scan_result();
        return OPRT_MALLOC_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        /* TuyaOpen reserves one byte for the terminator in WIFI_SSID_LEN. */
        if (result[i].ssid_len > WIFI_SSID_LEN) {
            continue;
        }
        if (ssid && (strlen((const char *)ssid) != result[i].ssid_len ||
                     memcmp(ssid, result[i].ssid, result[i].ssid_len) != 0)) {
            continue;
        }
        aps[out_count].channel = (uint8_t)result[i].channel_number;
        aps[out_count].rssi = result[i].rssi;
        memcpy(aps[out_count].bssid, result[i].mac_addr, sizeof(aps[out_count].bssid));
        aps[out_count].s_len = result[i].ssid_len;
        memcpy(aps[out_count].ssid, result[i].ssid, result[i].ssid_len);
        aps[out_count].ssid[result[i].ssid_len] = '\0';
        aps[out_count].security = jieli_auth_mode(result[i].auth_mode);
        ++out_count;
    }
    wifi_clear_scan_result();
    if (!out_count) {
        tkl_system_free(aps);
        return OPRT_OK;
    }
    *ap_ary = aps;
    *num = out_count;
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_release_ap(AP_IF_S *ap)
{
    tkl_system_free(ap);
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_start_ap(const WF_AP_CFG_IF_S *cfg)
{
    int result;

    if (!cfg) {
        return OPRT_INVALID_PARM;
    }
    /* NOTE: same caller-context hazard as STA if the AP netcfg path is ever
     * re-enabled (wifi_on() on the caller's task); BLE-only netcfg does not
     * reach this today. Route through a worker thread before turning the
     * AP provisioning module back on. */
    if (!wifi_is_on() && wifi_on() != 0) {
        return OPRT_COM_ERROR;
    }
    /* Do not power down the shared WiFi/LwIP stack here; AP mode performs the
     * native mode transition and the BLE netcfg path runs concurrently. */
    wifi_set_sta_connect_best_ssid(0);
    if (jieli_set_ap_lan_info(cfg) != 0) {
        return OPRT_INVALID_PARM;
    }
    if (cfg->chan) {
        wifi_set_channel(cfg->chan);
    }
    s_wifi_mode = WWM_SOFTAP;
    result = wifi_enter_ap_mode((char *)cfg->ssid, (char *)cfg->passwd);
    /* AP mode reinitialization may restore the SDK default STA auto-connect. */
    wifi_set_sta_connect_best_ssid(0);
    return jieli_result(result);
}

OPERATE_RET tkl_wifi_stop_ap(void)
{
    /* Deliberately does NOT power the module down, for the same reason
     * tkl_wifi_start_ap() does not: the WiFi/LwIP stack is shared with the STA
     * path that runs immediately after. Tuya stops the AP netcfg with
     * ap_netcfg.c's `tal_wifi_ap_stop()` and then goes straight to
     * `tal_wifi_set_work_mode(WWM_STATION)` + the STA connect, so a wifi_off()
     * here is always followed by another wifi_on().
     *
     * That off/on pair is not symmetric in the vendor release: wifi_off()
     * detaches the netdev (`RtmpOSNetDevDetach(): dev->name=ra0!`) but leaves
     * the lwIP netif registered, so the next wifi_on() runs `Init_LwIP` a second
     * time and netif_add() trips lwIP's "netif already added" assertion in a
     * loop that never recovers. Measured on AC79_DevKitBoard (2026-10-10):
     * two `|Init_LwIP` lines in one boot, 119 assertions within 0.4 s of the
     * second, and the device never reached association. The 2026-10-09
     * `prov6.log` capture was still storming 31 minutes after boot.
     *
     * The AP -> STA transition is the vendor's job: the STA worker calls
     * wifi_enter_sta_mode() on the still-powered module. */
    s_wifi_mode = WWM_STATION;
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_set_cur_channel(const uint8_t chan)
{
    wifi_set_channel(chan);
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_get_cur_channel(uint8_t *chan)
{
    if (!chan) {
        return OPRT_INVALID_PARM;
    }
    *chan = (uint8_t)wifi_get_channel();
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_set_sniffer(const BOOL_T en, const SNIFFER_CALLBACK cb)
{
    (void)en;
    (void)cb;
    return OPRT_NOT_SUPPORTED;
}

static void jieli_ip_to_text(uint32_t value, char *text, size_t text_size)
{
    ip4addr_ntoa_r((const ip4_addr_t *)&value, text, text_size);
}

OPERATE_RET tkl_wifi_get_ip(const WF_IF_E wf, NW_IP_S *ip)
{
    struct netif_info info;
    (void)wf;
    if (!ip) {
        return OPRT_INVALID_PARM;
    }
    memset(ip, 0, sizeof(*ip));
    memset(&info, 0, sizeof(info));
    lwip_get_netif_info(WIFI_NETIF, &info);
    jieli_ip_to_text(info.ip, ip->ip, sizeof(ip->ip));
    jieli_ip_to_text(info.netmask, ip->mask, sizeof(ip->mask));
    jieli_ip_to_text(info.gw, ip->gw, sizeof(ip->gw));
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_get_ipv6(const WF_IF_E wf, NW_IP_TYPE type, NW_IP_S *ip)
{
    (void)wf;
    (void)type;
    if (!ip) {
        return OPRT_INVALID_PARM;
    }
    memset(ip, 0, sizeof(*ip));
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_set_ip(const WF_IF_E wf, NW_IP_S *ip)
{
    (void)wf;
    (void)ip;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_set_mac(const WF_IF_E wf, const NW_MAC_S *mac)
{
    OPERATE_RET rt;

    (void)wf;
    if (!mac) {
        return OPRT_INVALID_PARM;
    }
    if (jieli_mac_set_wifi(mac->mac) != 0) {
        return OPRT_INVALID_PARM;
    }
    if (!wifi_is_on()) {
        return OPRT_OK;
    }
    rt = jieli_result(wifi_set_mac((char *)mac->mac));
    return rt;
}

OPERATE_RET tkl_wifi_get_mac(const WF_IF_E wf, NW_MAC_S *mac)
{
    (void)wf;
    if (!mac) {
        return OPRT_INVALID_PARM;
    }
    return jieli_mac_get_wifi(mac->mac) == 0 ? OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_wifi_set_work_mode(const WF_WK_MD_E mode)
{
    switch (mode) {
    case WWM_POWERDOWN:
        s_wifi_mode = mode;
        return jieli_result(wifi_off());
    case WWM_STATION:
        /* Record the mode only. This call arrives on the BLE workqueue
         * during netcfg completion; the module power-up runs on the
         * tkl_wifi_station_connect() worker thread instead (see above). */
        s_wifi_mode = mode;
        return OPRT_OK;
    case WWM_SOFTAP:
        /* netcfg supplies the SSID/password in tkl_wifi_start_ap(). */
        s_wifi_mode = mode;
        return OPRT_OK;
    case WWM_SNIFFER:
        s_wifi_mode = mode;
        return jieli_result(wifi_enter_smp_cfg_mode());
    default:
        return OPRT_NOT_SUPPORTED;
    }
}

OPERATE_RET tkl_wifi_get_work_mode(WF_WK_MD_E *mode)
{
    struct jieli_wifi_mode_info info;
    if (!mode) {
        return OPRT_INVALID_PARM;
    }
    memset(&info, 0, sizeof(info));
    wifi_get_mode_cur_info(&info);
    switch (info.mode) {
    case 1:
        *mode = WWM_STATION;
        break;
    case 2:
        *mode = WWM_SOFTAP;
        break;
    case 4:
        *mode = WWM_SNIFFER;
        break;
    default:
        *mode = s_wifi_mode;
        break;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_get_connected_ap_info(FAST_WF_CONNECTED_AP_INFO_T **fast_ap_info)
{
    if (fast_ap_info) {
        *fast_ap_info = NULL;
    }
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_get_bssid(uint8_t *mac)
{
    if (!mac) {
        return OPRT_INVALID_PARM;
    }
    wifi_get_bssid(mac);
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_set_country_code(const COUNTRY_CODE_E ccode)
{
    return ccode == COUNTRY_CODE_CN ? OPRT_OK : OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_set_rf_calibrated(void) { return OPRT_OK; }

OPERATE_RET tkl_wifi_set_lp_mode(const BOOL_T enable, const uint8_t dtim)
{
    (void)enable;
    (void)dtim;
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_station_fast_connect(const FAST_WF_CONNECTED_AP_INFO_T *fast_ap_info)
{
    (void)fast_ap_info;
    return OPRT_NOT_SUPPORTED;
}

/*
 * STA association worker.
 *
 * tkl_wifi_station_connect() is reached from the BLE workqueue (netcfg
 * completion) and from netmgr retries. Creating a task at runtime from that
 * context (os_task_create) and/or driving the native module bring-up from it
 * repeatedly crashed the system with an AXI fault inside the FreeRTOS list
 * code (2026-09-18 logs, identical with a 5120 and a 11264 byte workqueue
 * stack, so not a plain overflow). The worker therefore runs as a resident
 * task created once during tkl_wifi_init() -- the same early, quiet window
 * the other TAL threads use -- and requests are queued to it. Outcomes reach
 * Tuya through the WFE_* event callbacks.
 */
typedef struct {
    char ssid[WIFI_SSID_LEN + 1];
    char passwd[WIFI_PASSWD_LEN + 1];
} jieli_sta_work_t;

#define JIELI_WIFI_WORK_QUEUE_LEN 4

static TKL_QUEUE_HANDLE s_sta_work_queue;
static TKL_THREAD_HANDLE s_sta_worker_thread;
/* The vendor Wi-Fi connect path may retain these pointers after
 * wifi_enter_sta_mode() returns. Keep the credentials in static storage, as
 * the reference ipc_ac7916a adapter does with jl_on_ssid/jl_on_password. */
static char s_sta_ssid[WIFI_SSID_LEN + 1];
static char s_sta_passwd[WIFI_PASSWD_LEN + 1];

static void jieli_sta_connect_work(const jieli_sta_work_t *work)
{
    int result;
    int wifi_is_started;
    int mac_result;
    int set_mac_result;

    printf("[JIELI][WIFI][TRACE] stage=connect_work_enter\n");
    memcpy(s_sta_ssid, work->ssid, sizeof(s_sta_ssid));
    memcpy(s_sta_passwd, work->passwd, sizeof(s_sta_passwd));
    printf("[JIELI][WIFI] sta worker begin ssid_len:%u passwd_len:%u\n",
           (unsigned int)strlen(s_sta_ssid), (unsigned int)strlen(s_sta_passwd));
    printf("[JIELI][WIFI][TRACE] stage=wifi_is_on_begin\n");
    wifi_is_started = wifi_is_on();
    printf("[JIELI][WIFI][TRACE] stage=wifi_is_on_done result:%d\n", wifi_is_started);
    if (!wifi_is_started) {
        printf("[JIELI][WIFI][TRACE] stage=wifi_on_begin\n");
        result = wifi_on();
        printf("[JIELI][WIFI][TRACE] stage=wifi_on_done result:%d\n", result);
        if (result != 0) {
            printf("[JIELI][WIFI] sta worker wifi_on failed\n");
            if (s_wifi_event_cb != NULL) {
                s_wifi_event_cb(WFE_CONNECT_FAILED, NULL);
            }
            return;
        }
    }

    {
        uint8_t mac[6];
        printf("[JIELI][WIFI][TRACE] stage=wifi_mac_get_begin\n");
        mac_result = jieli_mac_get_wifi(mac);
        printf("[JIELI][WIFI][TRACE] stage=wifi_mac_get_done result:%d\n", mac_result);
        if (mac_result != 0) {
            printf("[JIELI][WIFI] sta worker MAC setup failed at get\n");
            if (s_wifi_event_cb != NULL) {
                s_wifi_event_cb(WFE_CONNECT_FAILED, NULL);
            }
            return;
        }

        printf("[JIELI][WIFI][TRACE] stage=wifi_set_mac_begin\n");
        set_mac_result = wifi_set_mac((char *)mac);
        printf("[JIELI][WIFI][TRACE] stage=wifi_set_mac_done result:%d\n", set_mac_result);
        if (set_mac_result != 0) {
            printf("[JIELI][WIFI] sta worker MAC setup failed at set\n");
            if (s_wifi_event_cb != NULL) {
                s_wifi_event_cb(WFE_CONNECT_FAILED, NULL);
            }
            return;
        }

        printf("[JIELI][WIFI][TRACE] stage=wifi_clear_scan_result_begin\n");
        wifi_clear_scan_result();
        printf("[JIELI][WIFI][TRACE] stage=wifi_clear_scan_result_done\n");
        printf("[JIELI][WIFI][TRACE] stage=wifi_set_best_ssid_begin\n");
        wifi_set_sta_connect_best_ssid(0);
        printf("[JIELI][WIFI][TRACE] stage=wifi_set_best_ssid_done\n");
        printf("[JIELI][WIFI][TRACE] stage=wifi_enter_sta_mode_begin\n");
        result = wifi_enter_sta_mode(s_sta_ssid, s_sta_passwd);
        printf("[JIELI][WIFI][TRACE] stage=wifi_enter_sta_mode_done result:%d\n", result);
        printf("[JIELI][WIFI] station connect ssid_len:%u passwd_len:%u native_result:%d\n",
               (unsigned int)strlen(s_sta_ssid), (unsigned int)strlen(s_sta_passwd), result);
        /* STA connect is configured as asynchronous. The SDK's return value
         * is not the association result; native Wi-Fi events report success
         * or failure. Let netmgr's connection timeout handle a request that
         * never produces an event instead of forcing an immediate retry. */
    }
}

static void jieli_wifi_worker(void *arg)
{
    jieli_sta_work_t work;

    (void)arg;
    for (;;) {
        /* Bounded poll. This was originally a workaround for tkl_queue_fetch()
         * treating a 0 timeout as "do not wait" on one SDK; the fetch path is
         * correct now, but the poll is kept so the worker's wakeup does not
         * depend on os_q_pend()'s vendor-specific blocking behaviour. */
        if (tkl_queue_fetch(s_sta_work_queue, &work, 100) == OPRT_OK) {
            printf("[JIELI][WIFI][TRACE] stage=queue_fetch_done\n");
            jieli_sta_connect_work(&work);
        }
    }
}

static OPERATE_RET jieli_wifi_worker_start(void)
{
    OPERATE_RET rt;

    if (s_sta_work_queue == NULL) {
        rt = tkl_queue_create_init(&s_sta_work_queue, sizeof(jieli_sta_work_t), JIELI_WIFI_WORK_QUEUE_LEN);
        if (rt != OPRT_OK) {
            return rt;
        }
    }
    if (s_sta_worker_thread == NULL) {
        rt = tkl_thread_create(&s_sta_worker_thread, "tuya_wifi_sta", 6144, 3, jieli_wifi_worker, NULL);
        if (rt != OPRT_OK) {
            return rt;
        }
        printf("[JIELI][WIFI] sta worker resident\n");
    }
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_station_connect(const int8_t *ssid, const int8_t *passwd)
{
    jieli_sta_work_t work = {0};
    OPERATE_RET rt;
    size_t ssid_len;
    size_t passwd_len;

    if (!ssid || !passwd) {
        return OPRT_INVALID_PARM;
    }
    if (s_sta_work_queue == NULL) {
        return OPRT_COM_ERROR;
    }

    ssid_len = strnlen((const char *)ssid, sizeof(work.ssid));
    passwd_len = strnlen((const char *)passwd, sizeof(work.passwd));
    if (ssid_len == 0 || ssid_len > WIFI_SSID_LEN || passwd_len > WIFI_PASSWD_LEN) {
        printf("[JIELI][WIFI] station connect rejected ssid_len:%u passwd_len:%u\n",
               (unsigned int)ssid_len, (unsigned int)passwd_len);
        return OPRT_INVALID_PARM;
    }
    memcpy(work.ssid, ssid, ssid_len);
    memcpy(work.passwd, passwd, passwd_len);

    s_wifi_mode = WWM_STATION;
    rt = tkl_queue_post(s_sta_work_queue, &work, 0);
    printf("[JIELI][WIFI] station connect queued ssid_len:%u passwd_len:%u rt:%d\n",
           (unsigned int)ssid_len, (unsigned int)passwd_len, rt);
    if (rt != OPRT_OK) {
        return rt;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_station_disconnect(void)
{
    /* No-op, matching the ipc_ac7916a adapter. The netmgr connect path calls
     * this before every association while the module may still be powered
     * down (deferred start); driving the vendor wifi_off() sequence from the
     * caller's task against an unpowered module raised the AXI fault in the
     * FreeRTOS list code (2026-09-18 logs). A re-association simply issues a
     * fresh wifi_enter_sta_mode() from the worker. */
    s_wifi_mode = WWM_STATION;
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_station_get_conn_ap_rssi(int8_t *rssi)
{
    if (!rssi) {
        return OPRT_INVALID_PARM;
    }
    *rssi = wifi_get_rssi();
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_station_get_status(WF_STATION_STAT_E *stat)
{
    int sdk_state;

    if (!stat) {
        return OPRT_INVALID_PARM;
    }

    sdk_state = wifi_get_sta_connect_state();
#if defined(JIELI_SELECTED_CHIP_WL82)
    /*
     * AC79's wifi_sta_connect_state values: disconnect=0, success=1,
     * no-SSID=2, association-failed=3, association-timeout=4, DHCP=5.
     */
    switch (sdk_state) {
    case 1:
        *stat = WSS_CONN_SUCCESS;
        break;
    case 2:
        *stat = WSS_NO_AP_FOUND;
        break;
    case 3:
    case 4:
        *stat = WSS_CONN_FAIL;
        break;
    case 5:
        *stat = WSS_GOT_IP;
        break;
    default:
        *stat = WSS_IDLE;
        break;
    }
#elif defined(JIELI_SELECTED_CHIP_WL83)
    /*
     * AC792 adds CONNECTING before success: disconnect=0, connecting=1,
     * success=2, no-SSID=3, association-failed=4, association-timeout=5,
     * DHCP=6.
     */
    switch (sdk_state) {
    case 2:
        *stat = WSS_CONN_SUCCESS;
        break;
    case 3:
        *stat = WSS_NO_AP_FOUND;
        break;
    case 4:
    case 5:
        *stat = WSS_CONN_FAIL;
        break;
    case 6:
        *stat = WSS_GOT_IP;
        break;
    default:
        *stat = WSS_IDLE;
        break;
    }
#else
#error "Jieli Wi-Fi status mapping requires a selected chip"
#endif
    return OPRT_OK;
}

OPERATE_RET tkl_wifi_send_mgnt(const uint8_t *buf, const uint32_t len)
{
    (void)buf;
    (void)len;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_register_recv_mgnt_callback(const BOOL_T enable, const WIFI_REV_MGNT_CB recv_cb)
{
    (void)enable;
    (void)recv_cb;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_wifi_ioctl(WF_IOCTL_CMD_E cmd, void *args)
{
    if (cmd == WFI_CONNECT_CMD && args) {
        WF_IOCTL_CONN_T *conn = (WF_IOCTL_CONN_T *)args;
        return tkl_wifi_station_connect((const int8_t *)conn->ssid, (const int8_t *)conn->passwd);
    }
    return OPRT_NOT_SUPPORTED;
}
