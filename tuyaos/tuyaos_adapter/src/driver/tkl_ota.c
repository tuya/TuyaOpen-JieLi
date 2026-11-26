#include "tkl_ota.h"

#include <stdint.h>
#include <stdio.h>

#include "tkl_semaphore.h"
#include "tkl_system.h"
#include "tuya_error_code.h"
#include "asm/sfc_norflash_api.h"
#include "device/ioctl_cmds.h"

#if defined(JIELI_SELECTED_CHIP_WL83)
#include "generic/typedef.h"
#include "ota_impl.h"
#include "db_updata_api.h"
#else
#include "generic/typedef.h"
#include "dual_bank_updata_api.h"
#endif

/* The vendor engine copies each dev_write through an internal buffer whose
 * size it reports; a Tuya pack larger than that has to be split. */
#define JIELI_OTA_CHUNK_DEFAULT 4096u
#define JIELI_OTA_FINISH_TIMEOUT_MS 20000u

/* Advertised ceiling for a cloud image. The dual-bank layout gives each bank
 * 0x16C000 bytes (0x20020..0x18C020 and 0x18C020..0x2F8000); anything larger is
 * rejected by dual_bank_update_allow_check() at start time. */
#define JIELI_OTA_IMAGE_MAX (0xF0000u)

static uint8_t s_ota_active;
static uint32_t s_ota_chunk = JIELI_OTA_CHUNK_DEFAULT;
static TKL_SEM_HANDLE s_ota_finish_sem;
static volatile int s_ota_finish_state; /* 0 running, 1 done, -1 failed */

static int jieli_ota_burn_cb(int err)
{
    s_ota_finish_state = (err == 0) ? 1 : -1;
    if (s_ota_finish_sem != NULL) {
        (void)tkl_semaphore_post(s_ota_finish_sem);
    }
    return 0;
}

static int jieli_ota_verify_cb(int result)
{
    if (result) {
        /* Read-back verified: program the boot info that points at this bank.
         * Completion is reported through jieli_ota_burn_cb(). */
        (void)dual_bank_update_burn_boot_info(jieli_ota_burn_cb);
        return 0;
    }
    s_ota_finish_state = -1;
    if (s_ota_finish_sem != NULL) {
        (void)tkl_semaphore_post(s_ota_finish_sem);
    }
    return -1;
}

static void jieli_ota_cleanup(void)
{
    (void)dual_bank_passive_update_exit(NULL);
    s_ota_active = 0;
    if (s_ota_finish_sem != NULL) {
        (void)tkl_semaphore_release(s_ota_finish_sem);
        s_ota_finish_sem = NULL;
    }
}

OPERATE_RET tkl_ota_get_ability(uint32_t *image_size, TUYA_OTA_TYPE_E *type)
{
#if !defined(CONFIG_DOUBLE_BANK_ENABLE)
    /* The board did not turn on JIELI_BOARD_DUAL_BANK, so the staged vendor
     * image has no second bank to write into. */
    (void)image_size;
    (void)type;
    return OPRT_NOT_SUPPORTED;
#else
    if (NULL == image_size || NULL == type) {
        return OPRT_INVALID_PARM;
    }
    *image_size = JIELI_OTA_IMAGE_MAX;
    *type = TUYA_OTA_FULL;
    return OPRT_OK;
#endif
}

OPERATE_RET tkl_ota_start_notify(uint32_t image_size, TUYA_OTA_TYPE_E type, TUYA_OTA_PATH_E path)
{
    uint32_t max_buf;
    int ret;

    (void)path;
#if !defined(CONFIG_DOUBLE_BANK_ENABLE)
    (void)image_size;
    (void)type;
    (void)max_buf;
    (void)ret;
    return OPRT_NOT_SUPPORTED;
#else
    if (image_size == 0u || image_size > JIELI_OTA_IMAGE_MAX) {
        return OPRT_INVALID_PARM;
    }
    if (type != TUYA_OTA_FULL) {
        /* The vendor engine takes a full image; it has its own patching path
         * but the Tuya difference package is not what it expects. */
        return OPRT_NOT_SUPPORTED;
    }
    if (s_ota_active) {
        jieli_ota_cleanup();
    }
    /* The USB downloader leaves the flash write-protected, and the vendor OTA
     * engine does not lift it itself: every write to the target bank would
     * fail and the transfer would be abandoned after the first buffer. */
    (void)norflash_open(NULL, NULL, NULL);
    (void)norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, 0);
    ret = dual_bank_passive_update_init(0u, image_size, (uint16_t)JIELI_OTA_CHUNK_DEFAULT, NULL);
    if (ret != 0) {
        return OPRT_COM_ERROR;
    }
    max_buf = get_dual_bank_passive_update_max_buf();
    s_ota_chunk = (max_buf >= 256u) ? max_buf : JIELI_OTA_CHUNK_DEFAULT;
    if (dual_bank_update_allow_check(image_size) != 0) {
        (void)dual_bank_passive_update_exit(NULL);
        return OPRT_COM_ERROR;
    }
    s_ota_active = 1;
    return OPRT_OK;
#endif
}

OPERATE_RET tkl_ota_data_process(TUYA_OTA_DATA_T *pack, uint32_t *remain_len)
{
    uint32_t offset = 0u;

    if (remain_len != NULL) {
        *remain_len = 0u;
    }
    if (!s_ota_active || pack == NULL || pack->data == NULL || pack->len == 0u) {
        return OPRT_INVALID_PARM;
    }
    while (offset < pack->len) {
        uint32_t chunk = pack->len - offset;
        int ret;

        if (chunk > s_ota_chunk) {
            chunk = s_ota_chunk;
        }
        /* The vendor's own callers (apps/common/update/net_update.c) treat any
         * nonzero return as a failure and abort the update; 0 is success. */
        ret = dual_bank_update_write(pack->data + offset, (uint16_t)chunk, NULL);
        if (ret != 0) {
            printf("[JIELI_OTA] write failed %d at 0x%x\n", ret, (unsigned)offset);
            return OPRT_COM_ERROR;
        }
        offset += chunk;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_ota_end_notify(BOOL_T reset)
{
    OPERATE_RET rt = OPRT_COM_ERROR;

    if (!s_ota_active) {
        return OPRT_INVALID_PARM;
    }
    s_ota_finish_state = 0;
    if (tkl_semaphore_create_init(&s_ota_finish_sem, 0u, 1u) != OPRT_OK) {
        jieli_ota_cleanup();
        return OPRT_COM_ERROR;
    }
    if (dual_bank_update_verify(NULL, NULL, jieli_ota_verify_cb) == 0) {
        (void)tkl_semaphore_wait(s_ota_finish_sem, JIELI_OTA_FINISH_TIMEOUT_MS);
    }
    if (s_ota_finish_state == 1 && reset) {
        /* Leave the engine alone; the reset drops the handle with the task. */
        tkl_system_reset();
    }
    if (s_ota_finish_state == 1) {
        rt = OPRT_OK;
    }
    jieli_ota_cleanup();
    return rt;
}

OPERATE_RET tkl_ota_get_old_firmware_info(TUYA_OTA_FIRMWARE_INFO_T **info)
{
    if (info) {
        *info = NULL;
    }
    return OPRT_NOT_SUPPORTED;
}
