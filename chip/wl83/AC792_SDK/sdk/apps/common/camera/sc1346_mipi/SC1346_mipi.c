#include "app_config.h"
extern void tkl_log_output(const char *format, ...);

#include "iic.h"
#include "asm/isp_dev.h"
#include "gpio.h"
#include "SC1346_mipi.h"
#include "asm/isp_alg.h"

static u32 reset_gpios[2] = {-1, -1};
static u32 pwdn_gpios[2] = {-1, -1};

extern void *SC1346_mipi_get_ae_params();
extern void *SC1346_mipi_get_awb_params();
extern void *SC1346_mipi_get_iq_params();
extern void SC1346_mipi_ae_ev_init(u32 fps);

#define SENSOR_FPS  30
/* #define SENSOR_FPS  60 */

#if (SENSOR_FPS == 60)
#define LINE_LENGTH_CLK     1600
#define FRAME_LENGTH        750
#define FRAME_LENGTH25      1080
#define INPUT_CLK  24
#define PCLK  72

#else
#define LINE_LENGTH_CLK     1500
#define FRAME_LENGTH        900
#define FRAME_LENGTH25      1080
#define INPUT_CLK  24
#define PCLK  40
#endif


static u32 cur_again = -1;
static u32 cur_dgain = -1;
static u32 cur_expline = -1;
static u32 cur_line_length_clk = -1;

static void *iic = NULL;

#define WRCMD 0x60
#define RDCMD 0x61


typedef struct {
    u16 addr;
    u8 value;
} sensor_ini_regs_t;

sensor_ini_regs_t SC1346_mipi_ini_regs[] = {
    0x0103, 0x01,
    0x0100, 0x00,
    0x36e9, 0x80,
    0x37f9, 0x80,
    0x301f, 0x05,
    0x3106, 0x05,
    0x3200, 0x00,
    0x3201, 0x00,
    0x3202, 0x00,
    0x3203, 0x00,
    0x3204, 0x05,
    0x3205, 0x07,
    0x3206, 0x02,
    0x3207, 0xdb,
    0x3208, 0x05,
    0x3209, 0x00,
    0x320a, 0x02,
    0x320b, 0xd0,
    0x320c, 0x07,
    0x320d, 0x08,
    0x3210, 0x00,
    0x3211, 0x04,
    0x3212, 0x00,
    0x3213, 0x04,
    0x3250, 0x00,
    0x3301, 0x06,
    0x3306, 0x50,
    0x3308, 0x0a,
    0x330a, 0x00,
    0x330b, 0xda,
    0x330e, 0x0a,
    0x331e, 0x61,
    0x331f, 0xa1,
    0x3364, 0x1f,
    0x3390, 0x09,
    0x3391, 0x0f,
    0x3392, 0x1f,
    0x3393, 0x30,
    0x3394, 0x30,
    0x3395, 0x30,
    0x33ad, 0x10,
    0x33b3, 0x40,
    0x33f9, 0x50,
    0x33fb, 0x70,
    0x33fc, 0x09,
    0x33fd, 0x0f,
    0x349f, 0x03,
    0x34a6, 0x09,
    0x34a7, 0x0f,
    0x34a8, 0x40,
    0x34a9, 0x30,
    0x34aa, 0x00,
    0x34ab, 0xe8,
    0x34ac, 0x00,
    0x34ad, 0xfc,
    0x3630, 0xe2,
    0x3632, 0x76,
    0x3633, 0x33,
    0x3639, 0xf4,
    0x3670, 0x09,
    0x3674, 0xe2,
    0x3675, 0xea,
    0x3676, 0xea,
    0x367c, 0x09,
    0x367d, 0x0f,
    0x3690, 0x22,
    0x3691, 0x22,
    0x3692, 0x22,
    0x3698, 0x88,
    0x3699, 0x90,
    0x369a, 0xa1,
    0x369b, 0xc3,
    0x369c, 0x09,
    0x369d, 0x0f,
    0x36a2, 0x09,
    0x36a3, 0x0b,
    0x36a4, 0x0f,
    0x36d0, 0x01,
    0x36ea, 0x1b,
    0x36eb, 0x0d,
    0x36ec, 0x15,
    0x36ed, 0x28,
    0x370f, 0x01,
    0x3722, 0x41,
    0x3724, 0x41,
    0x3725, 0xc1,
    0x3728, 0x00,
    0x37b0, 0x41,
    0x37b1, 0x41,
    0x37b2, 0x47,
    0x37b3, 0x09,
    0x37b4, 0x0f,
    0x37fa, 0x09,
    0x37fb, 0x33,
    0x37fc, 0x11,
    0x37fd, 0x37,
    0x3903, 0x40,
    0x3904, 0x04,
    0x3905, 0x8d,
    0x3907, 0x00,
    0x3908, 0x41,
    0x3933, 0x80,
    0x3934, 0x0a,
    0x3937, 0x79,
    0x3939, 0x00,
    0x393a, 0x00,
    0x3e01, 0x2e,
    0x3e02, 0xa0,
    0x440e, 0x02,
    0x4509, 0x20,
    0x450d, 0x28,
    0x4800, 0x64,
    0x4819, 0x06,
    0x481b, 0x03,
    0x481d, 0x0b,
    0x481f, 0x03,
    0x4821, 0x08,
    0x4823, 0x03,
    0x4825, 0x03,
    0x4827, 0x03,
    0x4829, 0x05,
    0x36e9, 0x28,
    0x37f9, 0x20,
    0x320c, LINE_LENGTH_CLK >> 8,
    0x320d, LINE_LENGTH_CLK >> 0,
#if (SENSOR_FPS ==25)
    0x320e, FRAME_LENGTH25 >> 8,
    0x320f, FRAME_LENGTH25 >> 0,
#else   //30fps
    0x320e, FRAME_LENGTH >> 8,
    0x320f, FRAME_LENGTH >> 0,
#endif
    0x0100, 0x01,
};

sensor_ini_regs_t SC1346_mipi_ini_regs_60fps[] = {

    0x0103, 0x01,
    0x0100, 0x00,
    0x36e9, 0x80,
    0x37f9, 0x80,
    0x301f, 0x06,
    0x3106, 0x05,
    0x320c, 0x06,
    0x320d, 0x40,
    0x3301, 0x0b,
    0x3303, 0x10,
    0x3306, 0x50,
    0x3308, 0x0a,
    0x330a, 0x00,
    0x330b, 0xda,
    0x330e, 0x0a,
    0x331e, 0x61,
    0x331f, 0xa1,
    0x3320, 0x04,
    0x3327, 0x08,
    0x3329, 0x09,
    0x3364, 0x1f,
    0x3390, 0x09,
    0x3391, 0x0f,
    0x3392, 0x1f,
    0x3393, 0x30,
    0x3394, 0xff,
    0x3395, 0xff,
    0x33ad, 0x10,
    0x33b3, 0x40,
    0x33f9, 0x50,
    0x33fb, 0x80,
    0x33fc, 0x09,
    0x33fd, 0x0f,
    0x349f, 0x03,
    0x34a6, 0x09,
    0x34a7, 0x0f,
    0x34a8, 0x40,
    0x34a9, 0x30,
    0x34aa, 0x00,
    0x34ab, 0xe8,
    0x34ac, 0x01,
    0x34ad, 0x0c,
    0x3630, 0xe2,
    0x3632, 0x76,
    0x3633, 0x33,
    0x3639, 0xf4,
    0x3670, 0x09,
    0x3674, 0xe2,
    0x3675, 0xea,
    0x3676, 0xea,
    0x367c, 0x09,
    0x367d, 0x0f,
    0x3690, 0x22,
    0x3691, 0x22,
    0x3692, 0x32,
    0x3698, 0x88,
    0x3699, 0x90,
    0x369a, 0xa1,
    0x369b, 0xc3,
    0x369c, 0x09,
    0x369d, 0x0f,
    0x36a2, 0x09,
    0x36a3, 0x0b,
    0x36a4, 0x0f,
    0x36d0, 0x01,
    0x36ea, 0x06,
    0x36eb, 0x05,
    0x36ec, 0x05,
    0x36ed, 0x28,
    0x370f, 0x01,
    0x3722, 0x41,
    0x3724, 0x41,
    0x3725, 0xc1,
    0x3727, 0x14,
    0x3728, 0x00,
    0x37b0, 0x21,
    0x37b1, 0x21,
    0x37b2, 0x27,
    0x37b3, 0x09,
    0x37b4, 0x0f,
    0x37fa, 0x0c,
    0x37fb, 0x31,
    0x37fc, 0x01,
    0x37fd, 0x17,
    0x3903, 0x40,
    0x3904, 0x04,
    0x3905, 0x8d,
    0x3907, 0x00,
    0x3908, 0x41,
    0x3933, 0x80,
    0x3934, 0x0a,
    0x3937, 0x79,
    0x3939, 0x00,
    0x393a, 0x00,
    0x3e01, 0x2e,
    0x3e02, 0x80,
    0x440e, 0x02,
    0x4509, 0x25,
    0x450d, 0x28,
    0x4819, 0x09,
    0x481b, 0x05,
    0x481d, 0x13,
    0x481f, 0x04,
    0x4821, 0x0a,
    0x4823, 0x05,
    0x4825, 0x04,
    0x4827, 0x05,
    0x4829, 0x08,
    0x36e9, 0x24,
    0x37f9, 0x20,
    0x0100, 0x01,
};


#define IIC_IOCTL_START                     IOCTL_IIC_START
#define IIC_IOCTL_STOP                      IOCTL_IIC_STOP
#define IIC_IOCTL_TX                   IOCTL_IIC_TX_BYTE
#define IIC_IOCTL_TX_WITH_START_BIT    IOCTL_IIC_TX_BYTE_WITH_START_BIT
#define IIC_IOCTL_TX_WITH_STOP_BIT     IOCTL_IIC_TX_BYTE_WITH_STOP_BIT
#define IIC_IOCTL_RX                   IOCTL_IIC_RX_BYTE
#define IIC_IOCTL_RX_WITH_ACK          IOCTL_IIC_RX_BYTE_WITH_ACK
#define IIC_IOCTL_RX_WITH_NACK         IOCTL_IIC_RX_BYTE_WITH_NACK
#define IIC_IOCTL_RX_WITH_STOP_BIT     IOCTL_IIC_RX_BYTE_WITH_STOP_BIT
#define IIC_IOCTL_SET_BAUDRATE              IOCTL_IIC_SET_BAUDRATE
#define IIC_IOCTL_TX_STOP_BIT               IOCTL_IIC_TX_STOP_BIT

unsigned char wr_SC1346_reg(u16 regID, unsigned char regDat)
{
    u8 ret = 1;

    dev_ioctl(iic, IIC_IOCTL_START, 0);

    if (dev_ioctl(iic, IIC_IOCTL_TX_WITH_START_BIT, WRCMD)) {
        ret = 0;
        goto __wend;
    }

    delay(100);

    if (dev_ioctl(iic, IIC_IOCTL_TX, regID >> 8)) {
        ret = 0;
        goto __wend;
    }

    delay(100);

    if (dev_ioctl(iic, IIC_IOCTL_TX, regID & 0xff)) {
        ret = 0;
        goto __wend;
    }

    delay(100);

    if (dev_ioctl(iic, IIC_IOCTL_TX_WITH_STOP_BIT, regDat)) {
        ret = 0;
        goto __wend;
    }

__wend:

    dev_ioctl(iic, IIC_IOCTL_STOP, 0);
    return ret;

}

unsigned char rd_SC1346_reg(u16 regID, unsigned char *regDat)
{
    u8 ret = 1;
    dev_ioctl(iic, IIC_IOCTL_START, 0);
    if (dev_ioctl(iic, IIC_IOCTL_TX_WITH_START_BIT, WRCMD)) {
        ret = 0;
        goto __rend;
    }

    delay(100);

    if (dev_ioctl(iic, IIC_IOCTL_TX, regID >> 8)) {
        ret = 0;
        goto __rend;
    }

    if (dev_ioctl(iic, IIC_IOCTL_TX_WITH_STOP_BIT, regID & 0xff)) {
        ret = 0;
        goto __rend;
    }

    delay(100);

    if (dev_ioctl(iic, IIC_IOCTL_TX_WITH_START_BIT, RDCMD)) {
        ret = 0;
        goto __rend;
    }

    delay(1000);

    dev_ioctl(iic, IIC_IOCTL_RX_WITH_STOP_BIT, (u32)regDat);

__rend:

    dev_ioctl(iic, IIC_IOCTL_STOP, 0);
    return ret;
}


/*************************************************************************************************
    sensor api
*************************************************************************************************/

void SC1346_mipi_config_SENSOR(u16 *width, u16 *height, u8 *format, u8 *frame_freq)
{
    u32 i;
    u8 v;


    SC1346_mipi_set_output_size(width, height, frame_freq);

#if (SENSOR_FPS == 60)
    for (i = 0; i < sizeof(SC1346_mipi_ini_regs_60fps) / sizeof(sensor_ini_regs_t); i++) {
        wr_SC1346_reg(SC1346_mipi_ini_regs_60fps[i].addr, SC1346_mipi_ini_regs_60fps[i].value);
    }
#else
    for (i = 0; i < sizeof(SC1346_mipi_ini_regs) / sizeof(sensor_ini_regs_t); i++) {
        wr_SC1346_reg(SC1346_mipi_ini_regs[i].addr, SC1346_mipi_ini_regs[i].value);
    }
#endif

//    if (30 == *frame_freq)
//    {
//        wr_SC1346_reg(0x320e,0x02);
//        wr_SC1346_reg(0x320f,0xee);
//    }

    //SC1346_mipi_ae_ev_init(*frame_freq);

    cur_again = -1;
    cur_dgain = -1;
    cur_expline = -1;
    cur_line_length_clk = -1;
    return;
}


s32 SC1346_mipi_set_output_size(u16 *width, u16 *height, u8 *frame_freq)
{
    return 0;
}


s32 SC1346_mipi_power_ctl(u8 isp_dev, u8 is_work)
{
    return 0;
}

extern void tkl_log_output(const char *format, ...);

s32 SC1346_mipi_ID_check(void)
{
    u8 pid = 0x00;
    u8 ver = 0x00;
    u8 i ;

    for (i = 0; i < 3; i++) { //
        rd_SC1346_reg(0x3107, &pid);
        rd_SC1346_reg(0x3108, &ver);
    }

    tkl_log_output("[SC1346] ID_check: read 0x3107=0x%02x, 0x3108=0x%02x\r\n", pid, ver);
    printf("SC1346_mipi,read3107=: 0x%x,read3108=: 0x%x \n", pid, ver);

    // if (pid != 0x22 && ver != 0x35)
    if (pid != 0xda || ver != 0x4d) {
        puts("----not SC1346_mipi-----\n");
        return -1;
    }
    tkl_log_output("[SC1346] FOUND SC1346_mipi sensor!\r\n");
    puts("----hello SC1346_mipi-----\n");
    return 0;
}

void SC1346_mipi_reset(u8 isp_dev)
{
    u32 reset_gpio;
    u32 pwdn_gpio;

    if (isp_dev == ISP_DEV_0) {
        reset_gpio = reset_gpios[0];
        pwdn_gpio = pwdn_gpios[0];
    } else {
        reset_gpio = reset_gpios[1];
        pwdn_gpio = pwdn_gpios[1];
    }

    /*printf("pwdn_gpio=%d\n", pwdn_gpio);*/
    gpio_direction_output(pwdn_gpio, 1);

    gpio_direction_output(reset_gpio, 0);
    delay(40000);
    gpio_direction_output(reset_gpio, 1);
    //gpio_direction_output(pwdn_gpio, 0);
    delay(40000);
}


static u8 cur_sensor_type = 0xff;

s32 SC1346_mipi_check(u8 isp_dev, u32 reset_gpio, u32 pwdn_gpio, char *iic_name)
{
    tkl_log_output("[SC1346] check: dev=%d, rst=%d, pwdn=%d, iic=%s\r\n",
                   isp_dev, reset_gpio, pwdn_gpio, iic_name ? iic_name : "NULL");
    puts("\n\n SC1346_mipi_check \n\n");
    if (!iic) {
        iic = dev_open(iic_name, 0);
        if (!iic) {
            tkl_log_output("[SC1346] dev_open %s failed!\r\n", iic_name ? iic_name : "NULL");
            return -1;
        }
    } else {
        if (cur_sensor_type != isp_dev) {
            return -1;
        }
    }

    reset_gpios[isp_dev] = reset_gpio;
    pwdn_gpios[isp_dev] = pwdn_gpio;

    SC1346_mipi_reset(isp_dev);

    if (0 != SC1346_mipi_ID_check()) {
        dev_close(iic);
        iic = NULL;
        return -1;
    }

    cur_sensor_type = isp_dev;

    return 0;
}

void resetStatic();
s32 SC1346_mipi_init(u8 isp_dev, u16 *width, u16 *height, u8 *format, u8 *frame_freq)
{
    tkl_log_output("[SC1346] init called: dev=%d, w=%d, h=%d, fmt=%d, fps=%d\r\n",
                   isp_dev, *width, *height, *format, *frame_freq);

    SC1346_mipi_config_SENSOR(width, height, format, frame_freq);
    tkl_log_output("[SC1346] init completed! Sensor streaming enabled.\r\n");

    return 0;
}


static void set_again(u32 again)
{
    if (cur_again == again) {
        return;
    }
    cur_again  = again;

    wr_SC1346_reg(0x3e09, (again >> 0) & 0xff);
    return;
}

static void set_dgain(u32 dgain)
{
    if (cur_dgain == dgain) {
        return;
    }
    cur_dgain  = dgain;

    wr_SC1346_reg(0x3e06, (dgain >> 8) & 0xff);
    wr_SC1346_reg(0x3e07, (dgain >> 0) & 0xff);
}



#define AGAIN_STEP (0x800)
#define MAX_DGAIN 4096
static void calc_gain(u32 gain, u32 *_again, u32 *_dgain)
{
    int i;
    u32 reg0, reg1, reg2;
    u32 dgain, againTndex, again;
    u8 againMap[] = {0x00, 0x08, 0x09, 0x0b, 0x0f, 0x1f};

    if (gain < 1024) {
        gain = 1024;
    }
    if (gain > 128 * 1024) {
        gain = 128 * 1024;
    }

    againTndex = 0;
    while (gain >= AGAIN_STEP) {
        gain *= 1024;
        gain /= AGAIN_STEP;
        againTndex++;

        if (againTndex == 0x05) {
            break;
        }
    }

    dgain = 0x00;
    if (gain > MAX_DGAIN) {
        gain = MAX_DGAIN;
    }
    while (gain >= 2048) {
        gain /= 2;
        dgain *= 2;
        dgain++;
    }

    *_again = againMap[againTndex];
    *_dgain = (dgain << 8) + (gain >> 3);
    //printf("again = %x dgain = %x\n",*_again,*_dgain);
}


static void set_shutter(u32 texp)
{
    if (cur_expline == texp) {
        return;
    }
    cur_expline  = texp;

    wr_SC1346_reg(0x3e00, texp >> 12);
    wr_SC1346_reg(0x3e01, texp >> 4);
    wr_SC1346_reg(0x3e02, (texp << 4) & 0xf0);
}


u32 SC1346_mipi_calc_shutter(isp_ae_shutter_t *shutter, u32 exp_time_us, u32 gain)
{
    u32 texp;
    u32 texp_align;
    u32 ratio;

    texp = exp_time_us * PCLK  / LINE_LENGTH_CLK;

    if (texp < 1) {
        texp = 1;
    }
    if (texp > FRAME_LENGTH * 2 - 4) {
        texp = FRAME_LENGTH * 2 - 4;
    }
    texp_align = (texp) * LINE_LENGTH_CLK / (PCLK);

    if (texp_align < exp_time_us) {
        ratio = (exp_time_us) * (1 << 10) / texp_align;
        //printf("ratio = %d\n",ratio);
    } else {
        ratio = (1 << 10);
    }

    shutter->ae_exp_line =  texp;
    shutter->ae_gain = (gain * ratio) >> 10;
    shutter->ae_exp_clk = 0;

    //printf("exp_time_us=%d, texp=%d, gain=%d->%d\n", exp_time_us, texp, gain,shutter->ae_gain);
    return 0;

}

u32 SC1346_mipi_set_shutter(isp_ae_shutter_t *shutter)
{
    static u32 lastgain = 0;
    u32 again, dgain;

    calc_gain((shutter->ae_gain), &again, &dgain);

    set_again(again);
    set_dgain(dgain);

    set_shutter(shutter->ae_exp_line);

    return 0;
}

void SC1346_mipi_sleep()
{


}

void SC1346_mipi_wakeup()
{


}

void SC1346_mipi_wr_reg(u16 addr, u16 val)
{
    printf("update reg%x with %x\n", addr, val);
    wr_SC1346_reg((u16)addr, (u8)val);
}
u16 SC1346_mipi_rd_reg(u16 addr)
{
    u8 val;
    rd_SC1346_reg((u16)addr, &val);
    return val;
}

REGISTER_CAMERA(SC1346_mipi) = {
    .logo 				= 	"SC1346d",
    .isp_dev 			= 	ISP_DEV_NONE,
    .in_format 			= 	SEN_IN_FORMAT_BGGR,
    .out_format 		= 	ISP_OUT_FORMAT_YUV,
    .mbus_type          =   SEN_MBUS_CSI2,
    .mbus_config        =   SEN_MBUS_DATA_WIDTH_10B | SEN_MBUS_CSI2_1_LANE,
    .fps         		= 	SENSOR_FPS,

    .sen_size 			= 	{SC1346_MIPI_OUTPUT_W, SC1346_MIPI_OUTPUT_H},
    .isp_size 			= 	{SC1346_MIPI_OUTPUT_W, SC1346_MIPI_OUTPUT_H},

    .cap_fps         		= 	SENSOR_FPS,
    .sen_cap_size 			= 	{SC1346_MIPI_OUTPUT_W, SC1346_MIPI_OUTPUT_H},
    .isp_cap_size 			= 	{SC1346_MIPI_OUTPUT_W, SC1346_MIPI_OUTPUT_H },

    .ops                =   {
        .avin_fps           =   NULL,
        .avin_valid_signal  =   NULL,
        .avin_mode_det      =   NULL,
        .sensor_check 		= 	SC1346_mipi_check,
        .init 		        = 	SC1346_mipi_init,
        .set_size_fps 		=	SC1346_mipi_set_output_size,
        .power_ctrl         =   SC1346_mipi_power_ctl,

        .get_ae_params  	=	SC1346_mipi_get_ae_params,
        .get_awb_params 	=	SC1346_mipi_get_awb_params,
        .get_iq_params 	    	=	SC1346_mipi_get_iq_params,

        .sleep 		        =	SC1346_mipi_sleep,
        .wakeup 		    =	SC1346_mipi_wakeup,
        .write_reg 		    =	SC1346_mipi_wr_reg,
        .read_reg 		    =	SC1346_mipi_rd_reg,

    }
};



