# tuyaopen-platform-JIELI

English | [中文](README_zh.md)

TuyaOpen 平台适配仓库，提供 JieLi（杰理）AC791/WL82 与 AC792N/WL83 的构建支持、TKL 适配和厂商 SDK。本仓库是 TuyaOpen 的子仓库，由 `tos.py` 自动下载，不能单独编译。

| 板卡 | 芯片 |
| --- | --- |
| `AC79_DevKitBoard` | AC791 / WL82 |
| `AC792N_Develop_Board` | AC792N / WL83 |

## 使用

在 TuyaOpen 仓库根目录构建、烧录和看日志：

```bash
tos.py config set CONFIG_BOARD_CHOICE_AC79_DEVKITBOARD=y
tos.py build
tos.py flash
tos.py monitor -p <串口号>
```

构建时会自动准备 pi32v2 工具链：Windows 解压仓库内的 portable 包，Linux 首次构建下载固定版本。也可以用 `JIELI_TOOL_DIR` 指向已安装的工具链。

## 目录

- `chip/`：厂商 SDK 源码
- `tuyaos/`：TKL 适配与板级配置
- `tools/`：构建、工具链与烧录脚本
- `docs/jieli-tkl.md`：适配架构与已知硬件问题
