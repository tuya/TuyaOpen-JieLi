# JieLi 平台：AC79_DevKitBoard 与 AC792N_Develop_Board

| 统一板名 | 芯片/平台 | 状态 | SDK |
| --- | --- | --- | --- |
| `AC79_DevKitBoard` | AC791 / WL82 | 完整 TuyaOpen `switch_demo` 构建入口已验证；历史别名 `AC7916A` 保留 | `chip/wl82/AC79_AIoT_SDK`（`release/AC79NN_SDK_V1.2.0`，tag `AC79NN_SDK_V1.2.13_2026-04-20`）|
| `AC792N_Develop_Board` | AC792N / WL83 | 完整 `switch_demo` 已在 Windows 构建并 USB 烧录；2026-10-08 独立冻结的 HUSB 固件在 AC792N V1.21 实板连续两轮运行时升级通过（USB VID:PID `3654:7857`）。本 PR 的 QIO 产物及 `tos.py flash` 尚未实板验收 | `chip/wl83/AC792_SDK`（`release/AC792N_SDK_V3`，tag `AC792N_SDK_BETA_V3.1.7_2026-08-25`）|

所有工程均从 TuyaOpen 仓库根目录或示例目录使用 `tos.py`。Windows 是当前支持的烧录环境。AC79 与 AC792 SDK 源码直接纳入 JieLi 平台仓库的 `chip/` 目录，不使用 Git submodule；AC79 SDK 中当前未使用的 `libmatter.a` 按项目约定忽略，不提交。Windows 工具链优先使用显式设置的 `JIELI_TOOL_DIR`，其次发现平台本地 portable 目录，再兼容系统安装目录 `C:\\JL\\pi32\\bin`；Linux 仍支持 `/opt/jieli/pi32v2/bin`。显式目录无效时会报错，不会静默切换到其他目录。

## 选择板卡与构建

AC79 完整应用（例如 `apps/tuya_cloud/switch_demo`）：

```powershell
tos.py config set CONFIG_BOARD_CHOICE_AC79_DEVKITBOARD=y CONFIG_JIELI_UART_LOG_PORT=1 CONFIG_JIELI_UART_LOG_BAUDRATE=115200
tos.py build
```

AC792 完整 Tuya `switch_demo`：

```powershell
tos.py config set CONFIG_BOARD_CHOICE_AC792N_DEVELOP_BOARD=y CONFIG_JIELI_UART_LOG_PORT=0 CONFIG_JIELI_UART_LOG_BAUDRATE=115200
tos.py build
```

该镜像包含 Tuya TKL Wi-Fi/BLE、BLE 配网、Tuya IoT 和 `switch_demo` DP 业务。AC792 SDK 的 WPA/SAE 还需链接 `libcrypto_mbedtls.a`。2026-09-24 已完成完整镜像构建，并由 `tos.py flash` 通过 USB 烧录成功；此前实板日志已有联网、云端激活及 DP 收发记录。UART 改为共用 UART0/115200 后已重新烧录，仍需抓取新日志核对本次串口配置。

### Windows 本地 portable 工具链

在平台仓库目录下准备完整的 `pi32` 工具树，保持 `bin`、`lib` 等资源目录：

```text
platform/JIELI/.tools/portable-jieli-windows/pi32/bin/clang.exe
```

将完整 `pi32` 目录放到 `platform/JIELI/.tools/portable-jieli-windows/` 下即可自动发现。已有此本地工具树时，无需安装 CodeBlocks；不要只复制 `bin`，clang 还需要同一工具树中的资源目录。也可通过 `JIELI_TOOL_DIR` 指向其他 `pi32/bin`。该变量一旦设置但路径缺少必需工具，构建会明确报错，不会回退到其他工具链。

TuyaOpen 的 Python、CMake、Ninja 和 GNU Make 等主机工具仍由 TuyaOpen 的 `tos.py prepare`/环境初始化提供；该步骤不会下载 JieLi 工具链。JieLi Windows 工具链发行标签为 2.5.2，包内 clang 自报内部版本 4.0.1。仓库不包含工具链二进制；请从获准的本地来源准备，遵守发行方许可，不要把工具树或私人 ZIP 提交到 Git。

没有显式目录、portable 工具树或系统安装时，Windows 仍保留现有安装器下载/启动回退。portable ZIP 自动下载和解压尚未实现。

## TKL 头文件兼容基线

`tuyaos/tuyaos_adapter/include/<域>/tkl_*.h` 是 Jieli 平台随仓维护的兼容快照，构建时优先于 TuyaOpen 公共 include 路径。当前 15 个头文件按内容（忽略 Git 换行差异）与 TuyaOpen 提交 `66e4c7000d2137e31f26433d01d1c92ac399c814` 的 `tools/porting/adapter/` 对应接口一致；`tkl_init.h` 对应 `tools/porting/adapter/init/include/tkl_init.h`。

TuyaOpen 更新 TKL 接口时，以集成所用 TuyaOpen 提交中的 `tools/porting/adapter/` 为同步来源，逐项比较并更新本地快照，再检查 WL82、WL83 的构建。若 Jieli 为兼容性需要保留差异，应在改动处说明原因并更新本基线记录。不要从 T5AI 复制可能已分叉的头文件，也不要在 `include/` 中加入 Jieli 私有实现声明。

## 板级串口与内存参考

| 板卡 | UART 日志配置 | Flash / RAM 参考 |
| --- | --- | --- |
| `AC79_DevKitBoard` | 日志：UART1、TX=PB3、115200 baud；TAL CLI：UART0、TX=PA5、RX=PA6、115200 baud | **实测 Flash ID `5E4017`、8 MiB；2026-09-23 官方 SDK USB 下载成功**。标准 DevKit 资料为 8 MiB SDRAM、片上 SRAM 578 KB。Tuya 构建 staging 配置为 8 MiB Flash / 8 MiB SDRAM；启动日志的 `SDRAM_SIZE` 是链接配置值，不是实板容量探测。板上 SDRAM 仍待丝印/完整内存测试确认 |
| `AC792N_Develop_Board` | TuyaOpen WL83 staging：UART0 日志与 Tuya CLI 共用，TX=PD1、RX=PE11、115200 baud；上游 SDK `board_demo.h` 默认 1 Mbps，构建按 Kconfig 覆盖 | **实板日志识别 Flash ID `5E4017`、8 MiB**。SDK 原始 `chip_cfg.h` 为 1 MiB Flash / 2 MiB SDRAM；平台构建脚本只改 staging 副本，当前构建值为 8 MiB Flash / 16 MiB DDR1，日志 `DDR_SIZE=16777216` 是链接配置值。官方封装支持 8/16 MiB，实板容量仍待芯片完整丝印/内存测试确认。片上 SRAM 资料不一致：板卡概述 256 KB，AC7926A Datasheet V1.5 为 352 KB |

Flash 容量与 RAM 容量的证据类型不同：AC79 Flash 由下载器读取确认，AC792 Flash ID/容量由 `apps/tuya_cloud/switch_demo/monitor.log` 读取确认；启动日志的 `SDRAM_SIZE`/`DDR_SIZE` 是链接器配置值，不代表自动测出的物理容量。AC79 SDK 原始 `demo_hello/app_config.h` 为 4 MiB Flash / 2 MiB SDRAM，平台构建只在 staging 副本中覆盖为 8 MiB / 8 MiB；AC792 SDK 原始配置与平台 staging 覆盖值也需区分。两块板 RAM 的精确物理容量仍需核对芯片完整丝印或执行覆盖全地址范围的内存测试。AC792 片上 SRAM 的板卡概述与 Datasheet V1.5 数值不一致，暂分别保留来源。`tos.py monitor` 默认波特率从当前 Kconfig 配置读取；AC79 与 AC792 默认日志波特率均为 115,200 baud。AC79 使用 UART1 输出日志、UART0 承载 TAL CLI；AC792 继续通过 UART0 承载日志和 CLI。官方容量资料直达链接见[项目 guide](../../docs/jieli_ac791x_ac792x_project_guide.md)。

## 烧录与串口日志

Windows 上构建完成后运行：

```powershell
tos.py flash
tos.py monitor -p COM3
```

`tos.py flash` 根据当前 `CHIP_CHOICE` 选择对应 SDK 的 `isd_download.exe` 和配置文件。设备为空片，或尚未烧入启用 USB 从机的应用固件时，需按对应芯片官方流程手动进入 Boot/ROM 下载模式：AC792 按住 `UPDATE` 并重新上电，AC79 使用 WL82 USB 下载模式。AC792 进入下载模式后可确认枚举为 `WL83 UBOOT1.00 USB Device`。烧录桥不负责识别日志 COM 口；`tos.py monitor` 需要指定设备管理器中的日志 COM 号。

### USB 运行时自动进入 ROM 下载

AC79/WL82 与 AC792/WL83 的完整 Tuya 应用构建会启用 USB Mass Storage 从机服务。**首次使用这项能力时，仍需按上面的板卡流程手动进入 Boot/ROM 下载模式，并烧入包含 USB 从机服务的固件。**这一步建立后续运行时升级的入口；空片或尚未烧入该固件的设备不会因为 `tos.py flash` 而免按键进入 ROM。

此后，在设备正常运行该固件且通过板上的 USB 数据接口连接 PC 时，可尝试按平常流程重新构建并运行 `tos.py flash`。Windows 平台桥调用 SDK 自带的 `isd_download.exe`。设备侧源码确认 USB 从机服务会响应特定私有 USB 请求，并在请求长度为 0 时调用 SDK 的 `go_mask_usb_updata()`；杰理官方文档说明，固件启用 USB 从机后可在运行中下载程序。`tos.py flash`、下载器请求与设备跳转 ROM、镜像写入及重启的端到端配合仍需实板验证。正常运行且 USB 从机服务已启动时，预期无需手动按 `UPDATE` 键。现有命令为：

```powershell
tos.py build
tos.py flash
```

该路径依赖应用固件仍能启动、USB 从机服务已运行，以及 `isd_download` 与固件/芯片配置匹配。使用板上连接到 PC 的 **USB 数据接口**；仅供充电的线缆或接口无法工作。日志 UART/COM 与这个下载 USB 接口是不同用途。平台为 USB 从机配置 Mass Storage 类并常驻服务任务；如果应用还要通过同一 OTG 物理接口使用 USB Host、U 盘、摄像头或其他 USB 类，角色和类配置可能冲突。此类组合需按目标板和接线单独验证，不能据此假定 Host 功能同时可用。

AC792N 实板的独立冻结 HUSB 镜像已于 2026-10-08 连续两轮通过运行时 USB 升级，设备以 VID:PID `3654:7857` 枚举；该证据不代表本 PR 的 QIO 已经烧录或验收。升级时仍使用相同的 `tos.py flash` 命令：第一次先按板卡流程手动进入 Boot/ROM，烧入带 USB 从机服务的固件；应用启动后，后续升级保持正常运行并连接 USB 数据口，再执行同一命令，无需按 `UPDATE`。首刷的 ROM 入口和后续运行时升级是设备所处状态不同，CLI 命令相同。

本 PR 仍需将生成的 QIO 通过 `tos.py flash` 实际烧录到 AC792N，并在实板确认新镜像运行；AC79/WL82 运行时 USB 升级也尚待验收。测试记录中的 COM8 当时被占用，日志 UART 未能据此验证。不要把独立冻结镜像的两轮结果记为本 PR 固件的验收结果。

官方参考：[AC79 USB 下载](https://doc.zh-jieli.com/AC79/zh-cn/master/getting_started/preparation/update.html)、[AC792 USB 下载](https://doc.zh-jieli.com/AC792/zh-cn/wifi_video_master/getting_started/preparation/update.html)、[AC79 USB 配置](https://doc.zh-jieli.com/AC79/zh-cn/master/module_example/peripherals/usb.html)、[AC792 USB 配置](https://doc.zh-jieli.com/AC792/zh-cn/wifi_video_master/module_example/peripherals/usb.html)。

也可以显式覆盖波特率：

```powershell
# AC792
tos.py monitor -p COM3 -b 115200
```

通用 Jieli 入口调用所选 TuyaOpen 应用的 `tuya_app_main()`。日志内容由应用和 TKL 实现输出。

## 已知限制

**AC792N 当前不支持 WPA3。** 原因是在 STA 关联阶段 CPU1 的 MbedTLS 会触发故障，作为规避，
AC792 SDK 源码中的 `CONFIG_WPA3_SUPPORT` 被置为 0：

```c
// chip/wl83/AC792_SDK/sdk/apps/common/net/wifi_conf.c
const u8 CONFIG_WPA3_SUPPORT = 0;  // 原值为 1
```

影响范围：AC792 无法加入**仅支持 WPA3/SAE** 的 AP；WPA2 及以下不受影响。这是有意保留的临时规避，
不是配置遗漏。恢复 WPA3 需要有一个仅支持 WPA3 的 AP 来复现该故障，先定位 CPU1 MbedTLS 路径，
再把该值改回 1；在具备该验证条件之前不要改回。

**两块板的 RAM 物理容量仍未在实板上完整验证**，证据与现状见上一节。

**工具链校验闸门拦住了你？** 自动下载的安装器会按固定 SHA-256 校验（`tools/jieli_build/toolchain.py` 的
`WINDOWS_TOOLCHAIN_INSTALLER_SHA256`）。如果 Jieli 重新上传了同版本号的安装器导致校验不通过，
手动安装工具链并把 `JIELI_TOOL_DIR` 指向其 `pi32v2/bin` 即可跳过自动下载。

## 历史 AC7916A 工程

`D:\\tuya_proj\\jieli\\ipc_ac7916a` 曾使用 AC7916A，映射到当前统一板名 `AC79_DevKitBoard`。项目保留用于历史参考，不作为当前调试工程；其中 UART2/PB6/115200 是历史工程配置。当前 AC79 固件使用 UART1/PB3/115200 输出日志，UART0/PA5-PA6/115200 承载 TAL CLI。AC792 使用 UART0，日志与 Tuya CLI 共用，PD1/PE11，115200 baud。

完整项目和官方资料索引见 [`docs/jieli_ac791x_ac792x_project_guide.md`](../../docs/jieli_ac791x_ac792x_project_guide.md)。
