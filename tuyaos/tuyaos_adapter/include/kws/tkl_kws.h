/**
* @file tkl_kws.h
* @brief keyword spotting module head file
* @version 0.1
* @date 2021-08-18
*
* @copyright Copyright 2021-2030 Tuya Inc. All Rights Reserved.
*
*/
#ifndef __TKL_KWS_H__
#define __TKL_KWS_H__


#include "tuya_cloud_types.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TKL_KWS_WAKEUP_WORD_UNKNOWN,
    TKL_KWS_WAKEUP_NIHAO_TUYA,
    TKL_KWS_WAKEUP_NIHAO_XIAOZHI,
    TKL_KWS_WAKEUP_HEY_TUYA,
    TKL_KWS_WAKEUP_SMARTLIFE, 
    TKL_KWS_WAKEUP_ZHINENGGUANJIA,
    TKL_KWS_WAKEUP_XIAOZHI_TONGXUE,
    TKL_KWS_WAKEUP_XIAOZHI_GUANJIA,
    TKL_KWS_WAKEUP_XIAOAI_XIAOAI,
    TKL_KWS_WAKEUP_XIAODU_XIAODU,
    TKL_KWS_WAKEUP_WORD_MAX,
} TKL_KWS_WAKEUP_WORD_E;

typedef void (*TKL_KWS_WAKEUP_CB)(TKL_KWS_WAKEUP_WORD_E wakeup_word);

OPERATE_RET tkl_kws_init(void);

OPERATE_RET tkl_kws_reg_wakeup_cb(TKL_KWS_WAKEUP_CB wakeup_cb);

OPERATE_RET tkl_kws_enable(void);

OPERATE_RET tkl_kws_disable(void);

OPERATE_RET tkl_kws_deinit(void);

/**
 * @brief Select the wake words the engine should recognise.
 *
 * Engines that only ship a fixed model can accept the list and report
 * TKL_KWS_WAKEUP_WORD_UNKNOWN for words they do not have; callers must not
 * assume every requested word is available.
 */
OPERATE_RET tkl_kws_wakeup_word_config(TKL_KWS_WAKEUP_WORD_E *words, uint32_t count);

/**
 * @brief Size in bytes of one PCM unit the engine consumes per call.
 */
uint32_t tkl_kws_get_process_uint_size(void);

/**
 * @brief Feed one PCM unit and return the detected wake word, if any.
 */
TKL_KWS_WAKEUP_WORD_E tkl_kws_recognize_wakeup_word(uint8_t *data, uint32_t len);


#ifdef __cplusplus
}
#endif

#endif
