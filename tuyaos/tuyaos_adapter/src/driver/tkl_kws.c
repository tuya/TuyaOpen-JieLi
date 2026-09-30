#include "tkl_kws.h"

/* Jieli boards do not have a Tuya KWS engine ported yet. Keep KWS-based chat
 * modes unavailable while allowing HOLD/oneshot modes to use audio normally. */
OPERATE_RET tkl_kws_init(void)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_kws_reg_wakeup_cb(TKL_KWS_WAKEUP_CB wakeup_cb)
{
    (void)wakeup_cb;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_kws_enable(void)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_kws_disable(void)
{
    return OPRT_OK;
}

OPERATE_RET tkl_kws_deinit(void)
{
    return OPRT_OK;
}
