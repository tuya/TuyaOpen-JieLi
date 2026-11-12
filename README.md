# tuyaopen-platform-JIELI

English | [中文](README_zh.md)

TuyaOpen platform for JieLi AC791/WL82 and AC792N/WL83: build support, TKL adaptation and the vendor SDKs. This repository is a sub-repository of TuyaOpen, downloaded by `tos.py`; it is not built standalone.

| Board | Chip |
| --- | --- |
| `AC79_DevKitBoard` | AC791 / WL82 |
| `AC792N_Develop_Board` | AC792N / WL83 |

## Usage

Build, flash and monitor from the TuyaOpen repository root:

```bash
tos.py config set CONFIG_BOARD_CHOICE_AC79_DEVKITBOARD=y
tos.py build
tos.py flash
tos.py monitor -p <PORT>
```

The pi32v2 toolchain is prepared automatically: on Windows from the portable archive in this repository, on Linux downloaded once on the first build. Set `JIELI_TOOL_DIR` to use an existing installation instead.

## Layout

- `chip/` — vendor SDK sources
- `tuyaos/` — TKL adaptation and board configuration
- `tools/` — build, toolchain and flashing scripts
- `docs/jieli-tkl.md` — adaptation architecture and known hardware issues
