# JIELI TKL 适配说明

## 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 厂商 SDK | `chip/wl82`、`chip/wl83` | 只读，不修改 |
| 构建期翻译 | `tools/jieli_build/` | 复制 SDK 到 `.build/jieli-staging/`，改写 vendor `board.c` |
| TKL 适配 | `tuyaos/tuyaos_adapter/` | TKL 实现 + 设备注册 |
| 板级 | 主仓 `boards/JIELI/` | Kconfig、`board_register_hardware()` |

## 设备注册模型

厂商没有运行期注册 API。`REGISTER_DEVICES()` 把 `struct dev_node[]` 放进
`.device` 段，`devices_init()` 遍历链接器符号 `_device_node_begin..end` 初始化
整张表。因此：

- 设备在**链接期**进表，在**运行期**由 `dev_open("name", priv)` 取用
- TKL 源码自己声明设备表，例：`tkl_pwm.c` 的 `REGISTER_DEVICE(pwm1_node)`
- 板级差异用 `#if defined(JIELI_SELECTED_CHIP_WL82)` / `WL83`

**配置读取时机**（决定哪些外设运行期可配）：

| 外设 | 何时读平台数据 | 运行期换脚 |
|---|---|---|
| UART | `*_open()` | 可以（`tkl_io_pinmux_config`） |
| PWM | `*_open()`，引脚来自板级配置 | wl82 由板级配置决定；wl83 通道是互补对，不支持 |
| SPI | `dev_open()`，`tkl_spi_init()` 可在 open 前改写引脚 | wl83 可以；wl82 是固定分组 |
| IIC | 编译期平台数据 | 硬件端口可以（open 后适配层重新应用）；软件 IIC 不行 |

## 板级引脚从哪来

外设引脚只有一个来源：板的 Kconfig（`boards/JIELI/<board>/Kconfig` 里的
`JIELI_BOARD_SPI1_*_PIN`、`JIELI_BOARD_PWM*_PIN`、`JIELI_BOARD_IIC*_PIN`）。

构建期 `tools/jieli_build/board_config.py` 把这些值写成 `board.c` 旁边的
`tuya_board_periph_config.h`，`sdk_overlay.py` 用
`CFLAGS += -include .../tuya_board_periph_config.h` 强制包含进整个构建，适配层
再用 `TUYA_BOARD_*` 宏初始化平台数据。板子不声明某个符号时适配层退回自己的
默认值，所以老工程不会因为缺符号而编译失败。

改了引脚值会改变 staging 摘要（`peripheral_pins` 分量），下一次构建会重新
staging，不会沿用旧头文件。

## IIC 端口

应用只按端口号调用 `tkl_i2c_init()`，**不需要知道这个口是硬件还是软件实现**，
初始化方式和其它平台完全一样；端口类型由适配层按芯片与板级引脚决定。

| 芯片 | 端口 | 实现 | 引脚 |
|---|---|---|---|
| wl82 | `TUYA_I2C_NUM_0` | 软件（该芯片没有硬件 IIC0） | 任意 GPIO；默认 PA0/PA1 |
| wl82 | `TUYA_I2C_NUM_1` | 硬件 | 控制器只认 PA7/PA8、PC1/PC2、USB DPA/DMA |
| wl82 | `TUYA_I2C_NUM_1` | 软件（自动回退） | 其余任意引脚；PH0/PH1 恒为软件（见下） |
| wl83 | `TUYA_I2C_NUM_0/1` | 硬件 | 按板级引脚 |
| wl83 | `TUYA_I2C_NUM_2` | 软件 | 任意 GPIO |

规则：

- **软件 IIC 支持任意 GPIO**：它自己拉脚，适配层不做 pinmux，所以 open 后也不能再改引脚；
- **硬件 IIC 只支持控制器映射里的那几组引脚**，并且 open 期间一直占用这些焊盘；
- 端口引脚落在硬件映射之外时**自动回退软件实现**（`tkl_i2c.c` 的
  `JIELI_IIC1_IS_SOFTWARE` 由板级引脚在编译期算出），应用侧没有任何开关；
- PH0/PH1 是摄像头模组的 2-wire 总线：交给硬件控制器后 SPI 侧收不到有效行
  （实测 AC791，ID 探测与整表都过、SPI 0 帧 / `tag_err`≈2700/s；改软件后 4.4 fps），
  所以这两个焊盘恒走软件；
- **默认推荐引脚**：板级 Kconfig 的 `JIELI_BOARD_IIC*_PIN`（例如摄像头板把
  IIC0/IIC1 都放在 PH0/PH1）；板子没有声明时适配层用 wl82 的 PA0/PA1（IIC0）
  与 PA7/PA8（IIC1）。

## DVP 同步极性

`tkl_dvp_init()` 的 `sync_polarity` 由上层按**传感器**填写，不是板级开关：

- 本平台注册的传感器描述符固定为 **HSYNC / VSYNC 都是高有效**
  （`SEN_MBUS_HSYNC_ACTIVE_HIGH | SEN_MBUS_VSYNC_ACTIVE_HIGH`），而厂商描述符是
  **静态**的、注册后改不了，所以只支持 `TUYA_DVP_SYNC_MODE_0`；其它取值返回
  `OPRT_NOT_SUPPORTED`（越界 4.. 才返回 `OPRT_INVALID_PARM`）——**明确拒绝，不静默忽略**。
- **H-V SYNC 互换不是极性**：WL82/AC791 把 SYNC0 接 VSYNC、SYNC1 接 HSYNC
  （厂商 `CONFIG_CAMERA_H_V_EXCHANGE`），由板级宏 `JIELI_BOARD_CAMERA_HV_SYNC_SWAP`
  在编译期写进描述符的 `.sync_config`。缺了它 ISP 只报 `isc line err`、永远凑不满一帧。
  这一项不经过 `sync_polarity`，也不应该塞进极性枚举。
- 换极性需要改描述的 arg（即改板/改平台），不是调用参数。

## 构建门控

唯一门控点是 `tuyaos_adapter/adapter_sources.txt`，每行 `源文件 [CONFIG_XXX]`。
源文件内部不要用 `#if CONFIG_*` 包整个文件。

## 板级矩阵

| | wl82 / AC79_DevKitBoard (AC791) | wl83 / AC792N_Develop_Board |
|---|---|---|
| Flash / SDRAM | 8M / 8M | 由 board facts 决定 |
| 显示 | ILI9488 320x480，8-bit EMI（MCU8080） | MIPI-DSI |
| 音频 | 无 `audio_adc/dac.h`，用自有 TKL provider | 需要 media 打开 CVP |
| UART 接收 | 无 IRQ-event ioctl，TAL/CLI 接收不可用 | 可用 |

## 硬件坑（都踩过）

**显示**
- LVGL 给的是小端 RGB565，8-bit EMI 上必须 `is_swap = true`（厂商
  `ili9488_clear_screen()` 按高字节先打包）。否则黑白正常、彩色全错。
- init 序列必须用厂商 `lcd_ili9488.c` 的 `code1[]`。TuyaOpen RGB 版序列的
  `0xB6 = 0x3B`、`MADCTL 0x48` 会让整幅画面变负片。
- 反色**不是** `0x21` INVON 造成的，厂商序列同样开 INVON。

**音频**
- PA mute 极性：PE15 高 = 功放开（LEVEL=0 覆盖时反过来），需要短接 J24。
- `audio_dac_set_volume()` 单独调用无效，必须先 `app_audio_state_switch()`。
- 板载麦克风需要 ADC 增益 19，两个麦克风不能同时打开。

## 构建与烧录

```bash
export JIELI_TOOL_DIR=platform/JIELI/tools/toolchains/<os>/pi32/bin
tos.py build
tos.py flash
```

Windows 上 PATH 需要排除 Git 的 `usr/bin`，并使用 `.venv` 的 Python。
