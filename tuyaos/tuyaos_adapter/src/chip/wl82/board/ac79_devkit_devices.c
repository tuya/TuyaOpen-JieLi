/* AC79_DevKitBoard SDK device-table glue used by the platform TKL adapters. */
#if defined(JIELI_SELECTED_CHIP_WL82) && defined(JIELI_AC79_DEVKIT_BOARD)

#include "system/init.h"
#include "device/device.h"
#include "device/iic.h"
#include "asm/emi.h"
#include "asm/gpio.h"

#define AC79_EMI_BAUD EMI_BAUD_DIV8

static const struct emi_platform_data ac79_emi_data = {
    .bits_mode = EMI_8BITS_MODE,
    .baudrate = AC79_EMI_BAUD,
    .colection = EMI_FALLING_COLT,
    .time_out = 1000,
    .th = EMI_TWIDTH_NO_HALF,
    .ts = 0,
    .tw = AC79_EMI_BAUD,
    .data_bit_en = 0,
};

SW_IIC_PLATFORM_DATA_BEGIN(ac79_sw_iic0_data)
    .clk_pin = IO_PORTH_00,
    .dat_pin = IO_PORTH_01,
    .sw_iic_delay = 50,
SW_IIC_PLATFORM_DATA_END()

REGISTER_DEVICES(ac79_tuya_device_table) = {
    { "emi",  &emi_dev_ops, (void *)&ac79_emi_data },
    { "iic0", &iic_dev_ops, (void *)&ac79_sw_iic0_data },
};

#endif
