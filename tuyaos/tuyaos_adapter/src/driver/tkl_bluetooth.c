#include "tkl_bluetooth.h"
#include "tkl_wifi.h"

#include "att.h"
#include "avctp_user.h"
#include "ble_api.h"
#include "bt_event.h"
#include "bluetooth.h"
#include "btstack_event.h"
#include "btstack_task.h"
#include "gap.h"
#include "gatt.h"
#include "ble/hci_ll.h"
#include "le_common_define.h"
#include "le_user.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern int le_controller_set_random_mac(void *addr);

static int jieli_ble_set_local_address(void)
{
    NW_MAC_S wifi_mac;
    uint8_t ble_mac[6];
    uint8_t payload_is_zero;
    uint8_t payload_is_one;

    if (tkl_wifi_get_mac(WF_STATION, &wifi_mac) != OPRT_OK) {
        return -1;
    }
    memcpy(ble_mac, wifi_mac.mac, sizeof(ble_mac));
    ble_mac[5]++;
    ble_mac[0] = (uint8_t)((ble_mac[0] & 0x3FU) | 0xC0U);
    payload_is_zero = (uint8_t)((ble_mac[0] & 0x3FU) == 0U &&
                                ble_mac[1] == 0U && ble_mac[2] == 0U && ble_mac[3] == 0U &&
                                ble_mac[4] == 0U && ble_mac[5] == 0U);
    payload_is_one = (uint8_t)((ble_mac[0] & 0x3FU) == 0x3FU &&
                               ble_mac[1] == 0xFFU && ble_mac[2] == 0xFFU && ble_mac[3] == 0xFFU &&
                               ble_mac[4] == 0xFFU && ble_mac[5] == 0xFFU);
    if (payload_is_zero) {
        ble_mac[5] = 0x01U;
    } else if (payload_is_one) {
        ble_mac[5] = 0xFEU;
    }
    return le_controller_set_random_mac(ble_mac);
}

/*
 * wl82 BLE integration notes
 * --------------------------
 *
 * The Jieli stack exposes a command-oriented LE API. This file translates
 * the TuyaOpen TKL API to that native interface.
 *
 * Jieli's ATT server database is a static profile database. The Tuya service
 * is therefore described below in the same format as the Jieli SDK examples;
 * tkl_ble_gatts_service_add() validates the TAL description and assigns the
 * corresponding static handles before btstack_init() invokes ble_profile_init.
 */

static TKL_BLE_GAP_EVT_FUNC_CB s_gap_callback;
static TKL_BLE_GATT_EVT_FUNC_CB s_gatt_callback;
static uint16_t s_client_conn_handle = TKL_BLE_GATT_INVALID_HANDLE;
static uint8_t s_search_profile_buffer[512] __attribute__((aligned(4)));
static uint8_t s_connection_role = TKL_BLE_ROLE_SERVER;

#define JIELI_BLE_ADV_DATA_MAX (31)
static uint8_t s_ble_stack_ready;
static uint8_t s_ble_adv_enabled;
static uint8_t s_ble_own_address_type = TKL_BLE_GAP_ADDR_TYPE_RANDOM;
static TKL_BLE_GAP_ADV_PARAMS_T s_ble_adv_params;
static uint8_t s_ble_adv_data[JIELI_BLE_ADV_DATA_MAX];
static uint8_t s_ble_scan_rsp_data[JIELI_BLE_ADV_DATA_MAX];
static uint8_t s_ble_adv_data_len;
static uint8_t s_ble_scan_rsp_data_len;

#define JIELI_ATT_LOCAL_PAYLOAD_SIZE (200)
#define JIELI_ATT_SEND_CBUF_SIZE     (512)
#define JIELI_ATT_RAM_BUFSIZE                                                               \
    (ATT_CTRL_BLOCK_SIZE + JIELI_ATT_LOCAL_PAYLOAD_SIZE + JIELI_ATT_SEND_CBUF_SIZE)

static uint8_t s_att_ram_buffer[JIELI_ATT_RAM_BUFSIZE] __attribute__((aligned(4)));

/* Tuya BLE common service UUIDs. Keep these local to the platform adapter so
 * the vendor build does not depend on Tuya TAL include paths. */
#define JIELI_TUYA_SERVICE_UUID_V1 (0x1910)
#define JIELI_TUYA_WRITE_UUID_V1   (0x2B11)
#define JIELI_TUYA_NOTIFY_UUID_V1  (0x2B10)
#define JIELI_TUYA_SERVICE_UUID_V2 (0xFD50)

static const uint8_t s_tuya_write_uuid_v2[16] = {
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80,
    0x01, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};
static const uint8_t s_tuya_notify_uuid_v2[16] = {
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80,
    0x01, 0x10, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
};
static const uint8_t s_tuya_read_uuid_v2[16] = {
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80,
    0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
};

typedef enum {
    JIELI_TUYA_PROFILE_V1 = 1,
    JIELI_TUYA_PROFILE_V2 = 2,
} jieli_tuya_profile_t;

static jieli_tuya_profile_t s_tuya_profile = JIELI_TUYA_PROFILE_V2;
static uint8_t s_gap_name[32] = "Tuya-Jieli";
static uint16_t s_gap_name_len = 10;
static uint8_t s_read_value[244];
static uint16_t s_read_value_len;

static void jieli_hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

/* Handles are deliberately fixed and match the generated Jieli ATT format:
 * Generic Access: 1..3; Tuya service: 4..11 (V2) or 4..9 (V1). */
#define JIELI_HANDLE_GAP_NAME          (0x0003)
#define JIELI_HANDLE_TUYA_SERVICE      (0x0004)
#define JIELI_HANDLE_TUYA_WRITE        (0x0006)
#define JIELI_HANDLE_TUYA_NOTIFY       (0x0008)
#define JIELI_HANDLE_TUYA_NOTIFY_CCCD  (0x0009)
#define JIELI_HANDLE_TUYA_READ         (0x000B)

static const uint8_t s_tuya_profile_v1[] = {
    /* 0x0001 PRIMARY_SERVICE 1800 */
    0x0A, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    /* 0x0002 CHARACTERISTIC 2A00 READ | WRITE | DYNAMIC */
    0x0D, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x0A, 0x03, 0x00, 0x00, 0x2A,
    /* 0x0003 VALUE 2A00 READ | WRITE | DYNAMIC */
    0x08, 0x00, 0x0A, 0x01, 0x03, 0x00, 0x00, 0x2A,
    /* 0x0004 PRIMARY_SERVICE 1910 */
    0x0A, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, 0x10, 0x19,
    /* 0x0005 CHARACTERISTIC 2B11 WRITE | WRITE_WITHOUT_RESPONSE | DYNAMIC */
    0x0D, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28, 0x0C, 0x06, 0x00, 0x11, 0x2B,
    /* 0x0006 VALUE 2B11 WRITE | WRITE_WITHOUT_RESPONSE | DYNAMIC */
    0x08, 0x00, 0x04, 0x01, 0x06, 0x00, 0x11, 0x2B,
    /* 0x0007 CHARACTERISTIC 2B10 NOTIFY */
    0x0D, 0x00, 0x02, 0x00, 0x07, 0x00, 0x03, 0x28, 0x10, 0x08, 0x00, 0x10, 0x2B,
    /* 0x0008 VALUE 2B10 NOTIFY */
    0x08, 0x00, 0x10, 0x00, 0x08, 0x00, 0x10, 0x2B,
    /* 0x0009 CLIENT_CHARACTERISTIC_CONFIGURATION */
    0x0A, 0x00, 0x0A, 0x01, 0x09, 0x00, 0x02, 0x29, 0x00, 0x00,
    0x00, 0x00,
};

static const uint8_t s_tuya_profile_v2[] = {
    /* 0x0001 PRIMARY_SERVICE 1800 */
    0x0A, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    /* 0x0002 CHARACTERISTIC 2A00 READ | DYNAMIC */
    0x0D, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2A,
    /* 0x0003 VALUE 2A00 READ | DYNAMIC */
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2A,
    /* 0x0004 PRIMARY_SERVICE FD50 */
    0x0A, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, 0x50, 0xFD,
    /* 0x0005 CHARACTERISTIC Tuya write UUID128 */
    0x1B, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28, 0x0C, 0x06, 0x00,
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80, 0x01, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    /* 0x0006 VALUE Tuya write UUID128 */
    0x16, 0x00, 0x04, 0x03, 0x06, 0x00,
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80, 0x01, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    /* 0x0007 CHARACTERISTIC Tuya notify UUID128 */
    0x1B, 0x00, 0x02, 0x00, 0x07, 0x00, 0x03, 0x28, 0x10, 0x08, 0x00,
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80, 0x01, 0x10, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    /* 0x0008 VALUE Tuya notify UUID128 */
    0x16, 0x00, 0x10, 0x03, 0x08, 0x00,
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80, 0x01, 0x10, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    /* 0x0009 CLIENT_CHARACTERISTIC_CONFIGURATION */
    0x0A, 0x00, 0x0A, 0x01, 0x09, 0x00, 0x02, 0x29, 0x00, 0x00,
    /* 0x000A CHARACTERISTIC Tuya read UUID128 */
    0x1B, 0x00, 0x02, 0x00, 0x0A, 0x00, 0x03, 0x28, 0x02, 0x0B, 0x00,
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    /* 0x000B VALUE Tuya read UUID128 */
    0x16, 0x00, 0x02, 0x03, 0x0B, 0x00,
    0xD0, 0x07, 0x9B, 0x5F, 0x80, 0x00, 0x01, 0x80, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

static int jieli_ble_uuid_matches(const TKL_BLE_UUID_T *uuid, uint8_t uuid_type, uint16_t uuid16,
                                  const uint8_t *uuid128)
{
    if (uuid == NULL || uuid->uuid_type != uuid_type) {
        return 0;
    }
    if (uuid_type == TKL_BLE_UUID_TYPE_16) {
        return uuid->uuid.uuid16 == uuid16;
    }
    return uuid128 != NULL && memcmp(uuid->uuid.uuid128, uuid128, 16) == 0;
}

static void jieli_ble_att_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    if (packet == NULL || size < 6 || packet_type != HCI_EVENT_PACKET) {
        return;
    }

    if (hci_event_packet_get_type(packet) == ATT_EVENT_MTU_EXCHANGE_COMPLETE) {
        uint16_t mtu = att_event_mtu_exchange_complete_get_MTU(packet);
        if (mtu <= 3) {
            printf("[JIELI][BLE] invalid ATT MTU:%u\n", mtu);
            return;
        }
        uint16_t payload = mtu - 3;
        if (payload < 20) {
            payload = 20;
        }
        ble_cmd_ret_e result = ble_user_cmd_prepare(BLE_CMD_ATT_MTU_SIZE, 1, payload);
        printf("[JIELI][BLE] ATT MTU:%u payload:%u set_send_mtu:%d\n", mtu, payload, result);
    }
}

static uint16_t jieli_ble_att_read_callback(uint16_t connection_handle, uint16_t attribute_handle, uint16_t offset,
                                            uint8_t *buffer, uint16_t buffer_size)
{
    uint16_t value_len;
    uint16_t copy_len;
    /* The vendor stack dispatches ATT callbacks on the btstack task, whose
     * stack is sized for the vendor examples. Keep the wide TKL event off
     * that stack; ATT requests are serialized, so a shared static is safe. */
    static TKL_BLE_GATT_PARAMS_EVT_T event;

    (void)connection_handle;
    if (attribute_handle == JIELI_HANDLE_GAP_NAME) {
        value_len = s_gap_name_len;
        if (buffer == NULL) {
            return value_len;
        }
        if (offset >= value_len) {
            return 0;
        }
        copy_len = value_len - offset;
        if (copy_len > buffer_size) {
            copy_len = buffer_size;
        }
        memcpy(buffer, s_gap_name + offset, copy_len);
        return copy_len;
    }
    if (attribute_handle == JIELI_HANDLE_TUYA_NOTIFY_CCCD) {
        if (buffer != NULL && buffer_size >= 2) {
            uint16_t ccc = att_get_ccc_config(attribute_handle);
            buffer[0] = (uint8_t)ccc;
            buffer[1] = (uint8_t)(ccc >> 8);
            return 2;
        }
        return 2;
    }
    if (attribute_handle != JIELI_HANDLE_TUYA_READ || s_tuya_profile != JIELI_TUYA_PROFILE_V2) {
        return 0;
    }

    memset(&event, 0, sizeof(event));
    event.type = TKL_BLE_GATT_EVT_READ_CHAR_VALUE;
    event.conn_handle = connection_handle;
    event.result = OPRT_OK;
    event.gatt_event.char_read.char_handle = attribute_handle;
    event.gatt_event.char_read.offset = offset;
    if (s_gatt_callback != NULL) {
        s_gatt_callback(&event);
    }
    if (buffer == NULL || offset >= s_read_value_len) {
        return s_read_value_len;
    }
    copy_len = s_read_value_len - offset;
    if (copy_len > buffer_size) {
        copy_len = buffer_size;
    }
    memcpy(buffer, s_read_value + offset, copy_len);
    return copy_len;
}

static int jieli_ble_att_write_callback(uint16_t connection_handle, uint16_t attribute_handle,
                                        uint16_t transaction_mode, uint16_t offset, uint8_t *buffer,
                                        uint16_t buffer_size)
{
    /* Runs on the btstack task. One printf line costs ~5ms of UART time at
     * 115200; during the large (197-byte) provisioning write that stall made
     * the controller NACK inbound PDUs, and the resulting "[LE_BB]conn nack"
     * flood then starved the log channel itself until every task stopped
     * reporting (livelock, monitor_capture.log 2026-09-18 21.025 onward).
     * Keep this callback log-free and stack-light; ble_mgr reports the queued
     * payload from the workqueue where blocking is harmless. */
    static TKL_BLE_GATT_PARAMS_EVT_T event;

    (void)transaction_mode;
    if (buffer == NULL) {
        return 0;
    }
    if (attribute_handle == JIELI_HANDLE_TUYA_NOTIFY_CCCD) {
        uint16_t ccc = buffer_size >= 2 ? (uint16_t)(buffer[0] | ((uint16_t)buffer[1] << 8)) : buffer[0];
        att_set_ccc_config(attribute_handle, ccc);
        memset(&event, 0, sizeof(event));
        event.type = TKL_BLE_GATT_EVT_SUBSCRIBE;
        event.conn_handle = connection_handle;
        event.result = OPRT_OK;
        event.gatt_event.subscribe.char_handle = JIELI_HANDLE_TUYA_NOTIFY;
        event.gatt_event.subscribe.prev_notify = 0;
        event.gatt_event.subscribe.cur_notify = (ccc & 0x0001) != 0;
        event.gatt_event.subscribe.prev_indicate = 0;
        event.gatt_event.subscribe.cur_indicate = (ccc & 0x0002) != 0;
        if (s_gatt_callback != NULL) {
            s_gatt_callback(&event);
        }
        return 0;
    }
    if (attribute_handle != JIELI_HANDLE_TUYA_WRITE || buffer_size == 0) {
        return 0;
    }

    memset(&event, 0, sizeof(event));
    event.type = TKL_BLE_GATT_EVT_WRITE_REQ;
    event.conn_handle = connection_handle;
    event.result = OPRT_OK;
    event.gatt_event.write_report.char_handle = attribute_handle;
    event.gatt_event.write_report.report.length = buffer_size;
    event.gatt_event.write_report.report.p_data = buffer;
    if (s_gatt_callback != NULL) {
        s_gatt_callback(&event);
    }
    (void)offset;
    return 0;
}

void ble_profile_init(void)
{
    le_device_db_init();
    att_ccc_config_init();

    /* Match the AC79 Tuya BLE examples.  Keep pairing disabled for the
     * Tuya application protocol, but initialize the SM module explicitly so
     * the combined GATT role has a deterministic security configuration. */
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION);
    sm_set_encryption_key_size_range(7, 16);
    sm_set_request_security(0);

    att_server_init(s_tuya_profile == JIELI_TUYA_PROFILE_V1 ? s_tuya_profile_v1 : s_tuya_profile_v2,
                    jieli_ble_att_read_callback, jieli_ble_att_write_callback);
    att_server_register_packet_handler(jieli_ble_att_packet_handler);
    le_l2cap_register_packet_handler(jieli_ble_att_packet_handler);

    uint16_t default_mtu_result = ble_vendor_set_default_att_mtu(JIELI_ATT_LOCAL_PAYLOAD_SIZE);
    printf("[JIELI][BLE] default ATT MTU:%u result:%u\n", JIELI_ATT_LOCAL_PAYLOAD_SIZE, default_mtu_result);

    /* btstack_init() may replace the callback installed before stack start;
     * register it again after the stack invokes this profile hook. */
    hci_event_callback_set(jieli_hci_event_handler);
}

static int jieli_ble_adv_type(uint8_t tuya_adv_type, uint8_t *jieli_adv_type)
{
    if (jieli_adv_type == NULL) {
        return 0;
    }
    switch (tuya_adv_type) {
    case TKL_BLE_GAP_ADV_TYPE_CONN_SCANNABLE_UNDIRECTED:
        *jieli_adv_type = ADV_IND;
        return 1;
    case TKL_BLE_GAP_ADV_TYPE_CONN_NONSCANNABLE_DIR_HIGHDUTY_CYCLE:
        *jieli_adv_type = ADV_DIRECT_IND;
        return 1;
    case TKL_BLE_GAP_ADV_TYPE_CONN_NONSCANNABLE_DIRECTED:
        *jieli_adv_type = ADV_DIRECT_IND_LOW;
        return 1;
    case TKL_BLE_GAP_ADV_TYPE_NONCONN_SCANNABLE_UNDIRECTED:
        *jieli_adv_type = ADV_SCAN_IND;
        return 1;
    case TKL_BLE_GAP_ADV_TYPE_NONCONN_NONSCANNABLE_UNDIRECTED:
        *jieli_adv_type = ADV_NONCONN_IND;
        return 1;
    default:
        return 0;
    }
}

static void jieli_ble_uuid_from_service(TKL_BLE_UUID_T *uuid, const gatt_client_service_t *service)
{
    memset(uuid, 0, sizeof(*uuid));
    if (service->uuid16 != 0) {
        uuid->uuid_type = TKL_BLE_UUID_TYPE_16;
        uuid->uuid.uuid16 = service->uuid16;
    } else {
        uuid->uuid_type = TKL_BLE_UUID_TYPE_128;
        memcpy(uuid->uuid.uuid128, service->uuid128, sizeof(uuid->uuid.uuid128));
    }
}

static void jieli_ble_uuid_from_characteristic(TKL_BLE_UUID_T *uuid,
                                                const gatt_client_characteristic_t *characteristic)
{
    memset(uuid, 0, sizeof(*uuid));
    if (characteristic->uuid16 != 0) {
        uuid->uuid_type = TKL_BLE_UUID_TYPE_16;
        uuid->uuid.uuid16 = characteristic->uuid16;
    } else {
        uuid->uuid_type = TKL_BLE_UUID_TYPE_128;
        memcpy(uuid->uuid.uuid128, characteristic->uuid128, sizeof(uuid->uuid.uuid128));
    }
}

/*
 * The Jieli client_user module calls these weak hooks from its static library.
 * They are the data path for profile discovery/read/notification events; they
 * do not arrive through the HCI GAP callback registered below.
 */
void user_client_report_search_result(search_result_t *result_info)
{
    TKL_BLE_GATT_PARAMS_EVT_T event;

    if (s_gatt_callback == NULL || result_info == (search_result_t *)-1) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.conn_handle = s_client_conn_handle;
    event.result = OPRT_OK;

    if (result_info->services.start_group_handle != 0) {
        event.type = TKL_BLE_GATT_EVT_PRIM_SEV_DISCOVERY;
        event.gatt_event.svc_disc.svc_num = 1;
        event.gatt_event.svc_disc.services[0].start_handle = result_info->services.start_group_handle;
        event.gatt_event.svc_disc.services[0].end_handle = result_info->services.end_group_handle;
        jieli_ble_uuid_from_service(&event.gatt_event.svc_disc.services[0].uuid,
                                    (const gatt_client_service_t *)&result_info->services);
        s_gatt_callback(&event);
    }

    if (result_info->characteristic.value_handle != 0) {
        memset(&event, 0, sizeof(event));
        event.type = TKL_BLE_GATT_EVT_CHAR_DISCOVERY;
        event.conn_handle = s_client_conn_handle;
        event.result = OPRT_OK;
        event.gatt_event.char_disc.char_num = 1;
        event.gatt_event.char_disc.characteristics[0].handle = result_info->characteristic.value_handle;
        jieli_ble_uuid_from_characteristic(&event.gatt_event.char_disc.characteristics[0].uuid,
                                           (const gatt_client_characteristic_t *)&result_info->characteristic);
        s_gatt_callback(&event);
    }
}

void user_client_report_descriptor_result(charact_descriptor_t *result_descriptor)
{
    TKL_BLE_GATT_PARAMS_EVT_T event;

    if (s_gatt_callback == NULL || result_descriptor == NULL || result_descriptor->uuid16 != 0x2902) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = TKL_BLE_GATT_EVT_CHAR_DESC_DISCOVERY;
    event.conn_handle = s_client_conn_handle;
    event.result = OPRT_OK;
    event.gatt_event.desc_disc.cccd_handle = result_descriptor->handle;
    s_gatt_callback(&event);
}

void user_client_report_data_callback(att_data_report_t *report_data)
{
    TKL_BLE_GATT_PARAMS_EVT_T event;

    if (s_gatt_callback == NULL || report_data == NULL) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.conn_handle = report_data->conn_handle;
    event.result = OPRT_OK;

    switch (report_data->packet_type) {
    case GATT_EVENT_NOTIFICATION:
    case GATT_EVENT_INDICATION:
        event.type = TKL_BLE_GATT_EVT_NOTIFY_INDICATE_RX;
        event.gatt_event.data_report.char_handle = report_data->value_handle;
        event.gatt_event.data_report.report.length = report_data->blob_length;
        event.gatt_event.data_report.report.p_data = report_data->blob;
        break;
    case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
    case GATT_EVENT_LONG_CHARACTERISTIC_VALUE_QUERY_RESULT:
        event.type = TKL_BLE_GATT_EVT_READ_RX;
        event.gatt_event.data_read.char_handle = report_data->value_handle;
        event.gatt_event.data_read.report.length = report_data->blob_length;
        event.gatt_event.data_read.report.p_data = report_data->blob;
        break;
    default:
        return;
    }
    s_gatt_callback(&event);
}

/* Enable CCCD discovery in Jieli's automatic profile search. */
int user_client_search_descriptor_is_enable(void)
{
    return 1;
}

static OPERATE_RET jieli_ble_cmd_result(ble_cmd_ret_e result)
{
    return result == BLE_CMD_RET_SUCESS ? OPRT_OK : OPRT_COM_ERROR;
}

static int jieli_ble_role(uint8_t role, uint8_t *jieli_role)
{
    if (jieli_role == NULL) {
        return 0;
    }
    if (role == TKL_BLE_ROLE_SERVER) {
        *jieli_role = 0;
    } else if (role == TKL_BLE_ROLE_CLIENT) {
        *jieli_role = 1;
    } else if (role == (TKL_BLE_ROLE_SERVER | TKL_BLE_ROLE_CLIENT)) {
        *jieli_role = 2;
    } else {
        return 0;
    }
    return 1;
}

static void jieli_hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    TKL_BLE_GAP_PARAMS_EVT_T event;

    (void)packet_type;
    (void)channel;
    if (packet == NULL || size == 0) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.result = OPRT_OK;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (size < 6) {
            return;
        }
        event.type = TKL_BLE_GAP_EVT_DISCONNECT;
        event.conn_handle = hci_event_disconnection_complete_get_connection_handle(packet);
        event.gap_event.disconnect.role = s_connection_role;
        event.gap_event.disconnect.reason = hci_event_disconnection_complete_get_reason(packet);
        if (s_connection_role == TKL_BLE_ROLE_SERVER) {
            ble_cmd_ret_e result = ble_user_cmd_prepare(BLE_CMD_ATT_SEND_INIT, 4, event.conn_handle, 0, 0, 0);
            printf("[JIELI][BLE] ATT_SEND_INIT release conn:%u result:%d\n", event.conn_handle, result);
        }
        if (s_gap_callback != NULL) {
            s_gap_callback(&event);
        }
        if (event.conn_handle == s_client_conn_handle) {
            s_client_conn_handle = TKL_BLE_GATT_INVALID_HANDLE;
        }
        break;

    case HCI_EVENT_LE_META:
        if (size < 3) {
            return;
        }
        {
            uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
            if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE ||
                subevent == HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE) {
            uint8_t peer_addr[6];
            uint8_t hci_role;

            if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                if (hci_subevent_le_connection_complete_get_status(packet) != 0) {
                    return;
                }
                event.conn_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                hci_role = hci_subevent_le_connection_complete_get_role(packet);
                event.gap_event.connect.peer_addr.type =
                    hci_subevent_le_connection_complete_get_peer_address_type(packet);
                hci_subevent_le_connection_complete_get_peer_address(packet, peer_addr);
                event.gap_event.connect.conn_params.conn_interval_min =
                    hci_subevent_le_connection_complete_get_conn_interval(packet);
                event.gap_event.connect.conn_params.conn_interval_max =
                    event.gap_event.connect.conn_params.conn_interval_min;
                event.gap_event.connect.conn_params.conn_latency =
                    hci_subevent_le_connection_complete_get_conn_latency(packet);
                event.gap_event.connect.conn_params.conn_sup_timeout =
                    hci_subevent_le_connection_complete_get_supervision_timeout(packet);
            } else {
                if (hci_subevent_le_enhanced_connection_complete_get_status(packet) != 0) {
                    return;
                }
                event.conn_handle =
                    hci_subevent_le_enhanced_connection_complete_get_connection_handle(packet);
                hci_role = hci_subevent_le_enhanced_connection_complete_get_role(packet);
                event.gap_event.connect.peer_addr.type =
                    hci_subevent_le_enhanced_connection_complete_get_peer_address_type(packet);
                hci_subevent_le_enhanced_connection_complete_get_peer_addresss(packet, peer_addr);
                event.gap_event.connect.conn_params.conn_interval_min =
                    hci_subevent_le_enhanced_connection_complete_get_conn_interval(packet);
                event.gap_event.connect.conn_params.conn_interval_max =
                    event.gap_event.connect.conn_params.conn_interval_min;
                event.gap_event.connect.conn_params.conn_latency =
                    hci_subevent_le_enhanced_connection_complete_get_conn_latency(packet);
                event.gap_event.connect.conn_params.conn_sup_timeout =
                    hci_subevent_le_enhanced_connection_complete_get_supervision_timeout(packet);
            }

            event.type = TKL_BLE_GAP_EVT_CONNECT;
            /* HCI LE role: 0 = master/central, 1 = slave/peripheral. */
            event.gap_event.connect.role = hci_role == 1 ? TKL_BLE_ROLE_SERVER : TKL_BLE_ROLE_CLIENT;
            s_connection_role = event.gap_event.connect.role;
            memcpy(event.gap_event.connect.peer_addr.addr, peer_addr, sizeof(peer_addr));
            s_client_conn_handle = event.conn_handle;
            if (s_connection_role == TKL_BLE_ROLE_SERVER) {
                ble_cmd_ret_e result = ble_user_cmd_prepare(BLE_CMD_ATT_SEND_INIT, 4, event.conn_handle,
                                                             s_att_ram_buffer, sizeof(s_att_ram_buffer),
                                                             JIELI_ATT_LOCAL_PAYLOAD_SIZE);
                printf("[JIELI][BLE] ATT_SEND_INIT conn:%u payload:%u ram:%u result:%d\n", event.conn_handle,
                       JIELI_ATT_LOCAL_PAYLOAD_SIZE, (unsigned)sizeof(s_att_ram_buffer), result);
            }
            if (s_connection_role == TKL_BLE_ROLE_CLIENT) {
                user_client_init(s_client_conn_handle, s_search_profile_buffer, sizeof(s_search_profile_buffer));
            }
            if (s_gap_callback != NULL) {
                s_gap_callback(&event);
            }
            }
        }
        break;
    case GAP_EVENT_ADVERTISING_REPORT:
        if (size > 2) {
            adv_report_t *report = (adv_report_t *)&packet[2];
            event.type = TKL_BLE_GAP_EVT_ADV_REPORT;
            event.conn_handle = TKL_BLE_GATT_INVALID_HANDLE;
            event.gap_event.adv_report.adv_type =
                report->event_type == 4 ? TKL_BLE_RSP_DATA : TKL_BLE_ADV_DATA;
            event.gap_event.adv_report.peer_addr.type = report->address_type;
            memcpy(event.gap_event.adv_report.peer_addr.addr, report->address, 6);
            event.gap_event.adv_report.rssi = report->rssi;
            event.gap_event.adv_report.channel_index = 0;
            event.gap_event.adv_report.data.length = report->length;
            event.gap_event.adv_report.data.p_data = report->data;
            if (s_gap_callback != NULL) {
                s_gap_callback(&event);
            }
        }
        break;
    default:
        break;
    }
}

static OPERATE_RET jieli_ble_set_data(TKL_BLE_DATA_T const *data, uint8_t is_scan_rsp)
{
    if (data == NULL || data->p_data == NULL || data->length > TKL_BLE_GAP_ADV_SET_DATA_SIZE_MAX) {
        return OPRT_INVALID_PARM;
    }

    if (is_scan_rsp) {
        ll_hci_adv_scan_response_set_data((uint8_t)data->length, data->p_data);
    } else {
        ll_hci_adv_set_data((uint8_t)data->length, data->p_data);
    }
    return OPRT_OK;
}

/* btstack_init() starts the controller asynchronously.  The native GAP
 * commands must be sent after BT_STATUS_INIT_OK; otherwise the first Tuya
 * advertising request is silently lost by the WL82 controller. */
static void jieli_ble_apply_advertising(void)
{
    uint8_t jieli_adv_type;
    int result;

    if (!s_ble_stack_ready || !s_ble_adv_enabled ||
        !jieli_ble_adv_type(s_ble_adv_params.adv_type, &jieli_adv_type)) {
        /* The request is not lost: bt_event_notify() re-applies once the
         * controller reports INIT_OK, and tkl_ble_gap_adv_start() re-applies
         * once it has been called. But *this* attempt is dropped, and doing so
         * silently makes "the phone cannot find the device" indistinguishable
         * from a device that is advertising fine. Say which condition failed.
         *
         * Measured on AC791: two boots of the same image differ - one reports
         * "BLE controller ready, advertising=0" and never reaches
         * "BLE advertising enabled", the other enables advertising normally. */
        printf("[JIELI] BLE advertising deferred: stack_ready=%d adv_enabled=%d adv_type=%u\n",
               (int)s_ble_stack_ready, (int)s_ble_adv_enabled,
               (unsigned)s_ble_adv_params.adv_type);
        return;
    }
    if (jieli_ble_cmd_result((ble_cmd_ret_e)ble_op_set_own_address_type(s_ble_own_address_type)) != OPRT_OK) {
        printf("[JIELI] BLE address type setup failed:%u\n", (unsigned int)s_ble_own_address_type);
        return;
    }
    ll_hci_adv_set_params(s_ble_adv_params.adv_interval_min, s_ble_adv_params.adv_interval_max,
                          jieli_adv_type, s_ble_adv_params.direct_addr.type,
                          (uint8_t *)s_ble_adv_params.direct_addr.addr,
                          ADV_CHANNEL_ALL, 0);
    if (s_ble_adv_data_len != 0) {
        ll_hci_adv_set_data(s_ble_adv_data_len, s_ble_adv_data);
    }
    if (s_ble_scan_rsp_data_len != 0) {
        ll_hci_adv_scan_response_set_data(s_ble_scan_rsp_data_len, s_ble_scan_rsp_data);
    }
    result = ll_hci_adv_enable(1);
    if (result == 0) {
        printf("[JIELI] BLE advertising enabled\n");
    } else {
        printf("[JIELI] BLE advertising enable failed:%d\n", result);
    }
}

OPERATE_RET tkl_ble_stack_init(uint8_t role)
{
    uint8_t jieli_role;

    /* TAL initializes Tuya BLE as peripheral + central (role == 3).  The
     * AC79 stack uses a separate value for the combined GATT role. */
    if (!jieli_ble_role(role, &jieli_role)) {
        return OPRT_INVALID_PARM;
    }

    s_ble_stack_ready = 0;
    s_ble_adv_enabled = 0;
    s_ble_adv_data_len = 0;
    s_ble_scan_rsp_data_len = 0;
    memset(&s_ble_adv_params, 0, sizeof(s_ble_adv_params));

    /* Use a stable UID-derived random-static address. Do not query the vendor
     * EDR getter here: its fallback can wait for WiFi while WiFi is deferred. */
    void lmp_set_sniff_disable(void);
    lmp_set_sniff_disable();
    if (jieli_ble_set_local_address() != 0) {
        return OPRT_COM_ERROR;
    }
    s_ble_own_address_type = TKL_BLE_GAP_ADDR_TYPE_RANDOM;

    ble_stack_gatt_role(jieli_role);
    hci_event_callback_set(jieli_hci_event_handler);
    return jieli_ble_cmd_result((ble_cmd_ret_e)btstack_init());
}

OPERATE_RET tkl_ble_stack_deinit(uint8_t role)
{
    uint8_t jieli_role;
    if (!jieli_ble_role(role, &jieli_role)) {
        return OPRT_INVALID_PARM;
    }
    s_gap_callback = NULL;
    s_gatt_callback = NULL;
    s_client_conn_handle = TKL_BLE_GATT_INVALID_HANDLE;
    s_ble_stack_ready = 0;
    s_ble_adv_enabled = 0;
    return jieli_ble_cmd_result((ble_cmd_ret_e)btstack_exit());
}

OPERATE_RET tkl_ble_stack_gatt_link(uint16_t *p_link)
{
    if (p_link == NULL) {
        return OPRT_INVALID_PARM;
    }
    *p_link = 1;
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_callback_register(const TKL_BLE_GAP_EVT_FUNC_CB gap_evt)
{
    s_gap_callback = gap_evt;
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatt_callback_register(const TKL_BLE_GATT_EVT_FUNC_CB gatt_evt)
{
    s_gatt_callback = gatt_evt;
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_addr_set(TKL_BLE_GAP_ADDR_T const *p_peer_addr)
{
    int result;

    if (p_peer_addr == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (p_peer_addr->type == TKL_BLE_GAP_ADDR_TYPE_RANDOM) {
        result = le_controller_set_random_mac((void *)p_peer_addr->addr);
        if (result == 0) {
            s_ble_own_address_type = TKL_BLE_GAP_ADDR_TYPE_RANDOM;
        }
        return result == 0 ? OPRT_OK : OPRT_COM_ERROR;
    }
    if (p_peer_addr->type == TKL_BLE_GAP_ADDR_TYPE_PUBLIC) {
        result = le_controller_set_mac((void *)p_peer_addr->addr);
        if (result == 0) {
            s_ble_own_address_type = TKL_BLE_GAP_ADDR_TYPE_PUBLIC;
        }
        return result == 0 ? OPRT_OK : OPRT_COM_ERROR;
    }
    return OPRT_INVALID_PARM;
}

OPERATE_RET tkl_ble_gap_address_get(TKL_BLE_GAP_ADDR_T *p_peer_addr)
{
    if (p_peer_addr == NULL) {
        return OPRT_INVALID_PARM;
    }
    p_peer_addr->type = s_ble_own_address_type;
    if (s_ble_own_address_type == TKL_BLE_GAP_ADDR_TYPE_RANDOM) {
        return le_controller_get_random_mac((void *)p_peer_addr->addr) == 0 ? OPRT_OK : OPRT_COM_ERROR;
    }
    return le_controller_get_mac((void *)p_peer_addr->addr) == 0 ? OPRT_OK : OPRT_COM_ERROR;
}

OPERATE_RET tkl_ble_gap_adv_start(TKL_BLE_GAP_ADV_PARAMS_T const *p_adv_params)
{
    uint8_t jieli_adv_type;
    if (p_adv_params == NULL || p_adv_params->adv_interval_min == 0 ||
        p_adv_params->adv_interval_min > p_adv_params->adv_interval_max) {
        return OPRT_INVALID_PARM;
    }
    if (!jieli_ble_adv_type(p_adv_params->adv_type, &jieli_adv_type)) {
        return OPRT_NOT_SUPPORTED;
    }
    memcpy(&s_ble_adv_params, p_adv_params, sizeof(s_ble_adv_params));
    s_ble_adv_enabled = 1;
    jieli_ble_apply_advertising();
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_adv_stop(void)
{
    s_ble_adv_enabled = 0;
    if (s_ble_stack_ready) {
        ll_hci_adv_enable(0);
    }
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_adv_rsp_data_set(TKL_BLE_DATA_T const *p_adv, TKL_BLE_DATA_T const *p_scan_rsp)
{
    if (p_adv != NULL) {
        if (p_adv->length > JIELI_BLE_ADV_DATA_MAX || (p_adv->length != 0 && p_adv->p_data == NULL)) {
            return OPRT_INVALID_PARM;
        }
        s_ble_adv_data_len = (uint8_t)p_adv->length;
        if (s_ble_adv_data_len != 0) {
            memcpy(s_ble_adv_data, p_adv->p_data, s_ble_adv_data_len);
            if (s_ble_stack_ready) {
                ll_hci_adv_set_data(s_ble_adv_data_len, s_ble_adv_data);
            }
        }
    }
    if (p_scan_rsp != NULL) {
        if (p_scan_rsp->length > JIELI_BLE_ADV_DATA_MAX ||
            (p_scan_rsp->length != 0 && p_scan_rsp->p_data == NULL)) {
            return OPRT_INVALID_PARM;
        }
        s_ble_scan_rsp_data_len = (uint8_t)p_scan_rsp->length;
        if (s_ble_scan_rsp_data_len != 0) {
            memcpy(s_ble_scan_rsp_data, p_scan_rsp->p_data, s_ble_scan_rsp_data_len);
            if (s_ble_stack_ready) {
                ll_hci_adv_scan_response_set_data(s_ble_scan_rsp_data_len, s_ble_scan_rsp_data);
            }
        }
    }
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_adv_rsp_data_update(TKL_BLE_DATA_T const *p_adv, TKL_BLE_DATA_T const *p_scan_rsp)
{
    return tkl_ble_gap_adv_rsp_data_set(p_adv, p_scan_rsp);
}

OPERATE_RET tkl_ble_gap_scan_start(TKL_BLE_GAP_SCAN_PARAMS_T const *p_scan_params)
{
    ble_cmd_ret_e result;
    if (p_scan_params == NULL || p_scan_params->interval == 0 || p_scan_params->window == 0 ||
        p_scan_params->window > p_scan_params->interval) {
        return OPRT_INVALID_PARM;
    }
    result = (ble_cmd_ret_e)ble_op_set_own_address_type(s_ble_own_address_type);
    if (jieli_ble_cmd_result(result) != OPRT_OK) {
        return OPRT_COM_ERROR;
    }
    result = ble_user_cmd_prepare(BLE_CMD_SCAN_PARAM, 3, p_scan_params->active ? 0 : 1,
                                   p_scan_params->interval, p_scan_params->window);
    if (result != BLE_CMD_RET_SUCESS) {
        return jieli_ble_cmd_result(result);
    }
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_SCAN_ENABLE, 1, 1));
}

OPERATE_RET tkl_ble_gap_scan_stop(void)
{
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_SCAN_ENABLE, 1, 0));
}

OPERATE_RET tkl_ble_gap_connect(TKL_BLE_GAP_ADDR_T const *p_peer_addr,
                                TKL_BLE_GAP_SCAN_PARAMS_T const *p_scan_params,
                                TKL_BLE_GAP_CONN_PARAMS_T const *p_conn_params)
{
    struct create_conn_param_t param;
    if (p_peer_addr == NULL || p_conn_params == NULL) {
        return OPRT_INVALID_PARM;
    }
    (void)p_scan_params;
    memset(&param, 0, sizeof(param));
    param.conn_interval = p_conn_params->conn_interval_max;
    param.conn_latency = p_conn_params->conn_latency;
    param.supervision_timeout = p_conn_params->conn_sup_timeout;
    param.peer_address_type = p_peer_addr->type;
    memcpy(param.peer_address, p_peer_addr->addr, sizeof(param.peer_address));
    if (jieli_ble_cmd_result((ble_cmd_ret_e)ble_op_set_own_address_type(s_ble_own_address_type)) != OPRT_OK) {
        return OPRT_COM_ERROR;
    }
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_CREATE_CONN, 1, &param));
}

OPERATE_RET tkl_ble_gap_disconnect(uint16_t conn_handle, uint8_t hci_reason)
{
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_DISCONNECT_EXT, 2, conn_handle, hci_reason));
}

OPERATE_RET tkl_ble_gap_conn_param_update(uint16_t conn_handle, TKL_BLE_GAP_CONN_PARAMS_T const *p_conn_params)
{
    struct conn_update_param_t param;
    if (p_conn_params == NULL) {
        return OPRT_INVALID_PARM;
    }
    param.interval_min = p_conn_params->conn_interval_min;
    param.interval_max = p_conn_params->conn_interval_max;
    param.latency = p_conn_params->conn_latency;
    param.timeout = p_conn_params->conn_sup_timeout;
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_REQ_CONN_PARAM_UPDATE, 2, conn_handle, &param));
}

OPERATE_RET tkl_ble_gap_tx_power_set(uint8_t role, int tx_power)
{
    (void)role;
    (void)tx_power;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_rssi_get(uint16_t conn_handle)
{
    (void)conn_handle;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_name_set(char *p_name)
{
    size_t length;
    if (p_name == NULL) {
        return OPRT_INVALID_PARM;
    }
    length = strlen(p_name);
    if (length == 0 || length >= sizeof(s_gap_name)) {
        return OPRT_INVALID_PARM;
    }
    memcpy(s_gap_name, p_name, length);
    s_gap_name[length] = '\0';
    s_gap_name_len = (uint16_t)length;
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_service_add(TKL_BLE_GATTS_PARAMS_T *p_service)
{
    TKL_BLE_SERVICE_PARAMS_T *service;
    TKL_BLE_CHAR_PARAMS_T *chars;

    if (p_service == NULL || p_service->svc_num != 1 || p_service->p_service == NULL) {
        return OPRT_INVALID_PARM;
    }
    service = p_service->p_service;
    if (service->type != TKL_BLE_UUID_SERVICE_PRIMARY || service->p_char == NULL ||
        (!jieli_ble_uuid_matches(&service->svc_uuid, TKL_BLE_UUID_TYPE_16,
                                 JIELI_TUYA_SERVICE_UUID_V1, NULL) &&
         !jieli_ble_uuid_matches(&service->svc_uuid, TKL_BLE_UUID_TYPE_16,
                                 JIELI_TUYA_SERVICE_UUID_V2, NULL))) {
        return OPRT_NOT_SUPPORTED;
    }
    chars = service->p_char;
    if (service->svc_uuid.uuid.uuid16 == JIELI_TUYA_SERVICE_UUID_V1) {
        if (service->char_num != 2 ||
            !jieli_ble_uuid_matches(&chars[0].char_uuid, TKL_BLE_UUID_TYPE_16, JIELI_TUYA_WRITE_UUID_V1, NULL) ||
            !jieli_ble_uuid_matches(&chars[1].char_uuid, TKL_BLE_UUID_TYPE_16, JIELI_TUYA_NOTIFY_UUID_V1, NULL)) {
            return OPRT_NOT_SUPPORTED;
        }
        s_tuya_profile = JIELI_TUYA_PROFILE_V1;
        service->handle = JIELI_HANDLE_TUYA_SERVICE;
        chars[0].handle = JIELI_HANDLE_TUYA_WRITE;
        chars[1].handle = JIELI_HANDLE_TUYA_NOTIFY;
        /* tal_bluetooth.c always reads the optional V2 read slot while
         * building the connect event; keep it explicitly invalid for V1. */
        if (TKL_BLE_GATT_CHAR_MAX_NUM > 2) {
            chars[2].handle = TKL_BLE_GATT_INVALID_HANDLE;
        }
        return OPRT_OK;
    }
    if (service->char_num != 3 ||
        !jieli_ble_uuid_matches(&chars[0].char_uuid, TKL_BLE_UUID_TYPE_128, 0, s_tuya_write_uuid_v2) ||
        !jieli_ble_uuid_matches(&chars[1].char_uuid, TKL_BLE_UUID_TYPE_128, 0, s_tuya_notify_uuid_v2) ||
        !jieli_ble_uuid_matches(&chars[2].char_uuid, TKL_BLE_UUID_TYPE_128, 0, s_tuya_read_uuid_v2)) {
        return OPRT_NOT_SUPPORTED;
    }
    s_tuya_profile = JIELI_TUYA_PROFILE_V2;
    service->handle = JIELI_HANDLE_TUYA_SERVICE;
    chars[0].handle = JIELI_HANDLE_TUYA_WRITE;
    chars[1].handle = JIELI_HANDLE_TUYA_NOTIFY;
    chars[2].handle = JIELI_HANDLE_TUYA_READ;
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_service_change(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
    (void)conn_handle;
    (void)start_handle;
    (void)end_handle;
    /* The profile is fixed at build time, so no Service Changed indication is
     * needed. Keep the API successful for TAL's optional call. */
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_value_set(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data, uint16_t length)
{
    (void)conn_handle;
    if (p_data == NULL || length > sizeof(s_read_value) || char_handle != JIELI_HANDLE_TUYA_READ ||
        s_tuya_profile != JIELI_TUYA_PROFILE_V2) {
        return OPRT_INVALID_PARM;
    }
    memcpy(s_read_value, p_data, length);
    s_read_value_len = length;
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_value_get(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data, uint16_t length)
{
    uint16_t copy_len;
    (void)conn_handle;
    if (p_data == NULL || char_handle != JIELI_HANDLE_TUYA_READ || s_tuya_profile != JIELI_TUYA_PROFILE_V2) {
        return OPRT_INVALID_PARM;
    }
    copy_len = s_read_value_len < length ? s_read_value_len : length;
    memcpy(p_data, s_read_value, copy_len);
    return OPRT_OK;
}

static OPERATE_RET jieli_ble_att_send(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data,
                                      uint16_t length, uint8_t operation)
{
    if (conn_handle == TKL_BLE_GATT_INVALID_HANDLE || p_data == NULL || length == 0 || length > 512) {
        return OPRT_INVALID_PARM;
    }
    /* This adapter initializes the single-link ATT sender on each server
     * connection, matching the AC79 SDK's le_net_cfg example. */
    ble_cmd_ret_e result = ble_user_cmd_prepare(BLE_CMD_ATT_SEND_DATA, 4, char_handle, p_data, length, operation);
    printf("[JIELI][BLE] ATT_SEND_DATA conn:%u attr:0x%04x len:%u op:%u result:%d\n", conn_handle, char_handle,
           length, operation, result);
    return jieli_ble_cmd_result(result);
}

OPERATE_RET tkl_ble_gatts_value_notify(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data, uint16_t length)
{
    return jieli_ble_att_send(conn_handle, char_handle, p_data, length, ATT_OP_NOTIFY);
}

OPERATE_RET tkl_ble_gatts_value_indicate(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data, uint16_t length)
{
    return jieli_ble_att_send(conn_handle, char_handle, p_data, length, ATT_OP_INDICATE);
}

OPERATE_RET tkl_ble_gatts_exchange_mtu_reply(uint16_t conn_handle, uint16_t server_rx_mtu)
{
    (void)conn_handle;
    if (server_rx_mtu < ATT_DEFAULT_MTU) {
        return OPRT_INVALID_PARM;
    }
    /* The server ATT sender is initialized with the single-link API above;
     * use the matching single-link MTU command here. */
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_ATT_MTU_SIZE, 1, server_rx_mtu));
}

OPERATE_RET tkl_ble_gattc_all_service_discovery(uint16_t conn_handle)
{
    if (conn_handle == TKL_BLE_GATT_INVALID_HANDLE) {
        return OPRT_INVALID_PARM;
    }
    if (s_client_conn_handle != conn_handle) {
        s_client_conn_handle = conn_handle;
        user_client_init(conn_handle, s_search_profile_buffer, sizeof(s_search_profile_buffer));
    }
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_SEARCH_PROFILE, 2, PFL_SERVER_ALL, 0));
}

OPERATE_RET tkl_ble_gattc_all_char_discovery(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
    (void)conn_handle;
    (void)start_handle;
    (void)end_handle;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gattc_char_desc_discovery(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
    (void)conn_handle;
    (void)start_handle;
    (void)end_handle;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gattc_write_without_rsp(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data,
                                            uint16_t length)
{
    if (p_data == NULL || length == 0) {
        return OPRT_INVALID_PARM;
    }
    return gatt_client_write_value_of_characteristic_without_response(conn_handle, char_handle, length, p_data) == 0
               ? OPRT_OK
               : OPRT_COM_ERROR;
}

OPERATE_RET tkl_ble_gattc_write(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data, uint16_t length)
{
    if (p_data == NULL || length == 0) {
        return OPRT_INVALID_PARM;
    }
    return gatt_client_write_value_of_characteristic(jieli_hci_event_handler, conn_handle, char_handle, length, p_data) == 0
               ? OPRT_OK
               : OPRT_COM_ERROR;
}

OPERATE_RET tkl_ble_gattc_read(uint16_t conn_handle, uint16_t char_handle)
{
    return gatt_client_read_value_of_characteristic_using_value_handle(jieli_hci_event_handler, conn_handle, char_handle) == 0
               ? OPRT_OK
               : OPRT_COM_ERROR;
}

OPERATE_RET tkl_ble_gattc_exchange_mtu_request(uint16_t conn_handle, uint16_t client_rx_mtu)
{
    if (client_rx_mtu < ATT_DEFAULT_MTU) {
        return OPRT_INVALID_PARM;
    }
    return jieli_ble_cmd_result(ble_user_cmd_prepare(BLE_CMD_MULTI_ATT_MTU_SIZE, 2, conn_handle, client_rx_mtu));
}

OPERATE_RET tkl_ble_vendor_command_control(uint16_t opcode, void *user_data, uint16_t data_len)
{
    (void)opcode;
    (void)user_data;
    (void)data_len;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_set_mode(const BOOL_T enable, const uint8_t mode)
{
    (void)mode;
    return ll_hci_adv_enable(enable ? 1 : 0) == 0 ? OPRT_OK : OPRT_COM_ERROR;
}

/* The vendor BR/EDR glue expects this hook even for LE-only applications. */
int bt_event_notify(enum bt_event_from from, struct bt_event *event)
{
    if (from == BT_EVENT_FROM_CON && event != NULL && event->event == BT_STATUS_INIT_OK) {
        s_ble_stack_ready = 1;
        jieli_ble_apply_advertising();
        printf("[JIELI] BLE controller ready, advertising=%d\n", s_ble_adv_enabled);
    }
    return 0;
}
