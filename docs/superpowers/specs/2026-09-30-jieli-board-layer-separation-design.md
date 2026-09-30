# JIELI 板级分层分离设计

> 状态：设计已确认，待实现。
> 关联：`platform/JIELI/docs/superpowers/specs/2026-09-29-jieli-tkl-header-ownership-design.md`（TKL 适配层结构重构）

## 目标

让 JIELI 移植遵守 TuyaOpen 的分层：**板级外设由 board 注册、应用通过 TDL 使用、平台层不认识应用**。消除平台入口对应用符号的反向依赖，并把音频底层准备归位到 TKL。

本次是**分层归位**，不是新功能。应用的业务逻辑一行不改。

## 分层定义

| 层 | 目录 | 拥有什么 | 谁调用它 |
|---|---|---|---|
| **app** | `apps/`、`examples/`、`src/ai_components/` | 业务逻辑、交互策略 | 用户 |
| **tdl** | `src/peripherals/*/tdl_*/` | 设备抽象：`tdl_button_*`、`tdl_audio_*` | **app** |
| **tdd** | `src/peripherals/*/tdd_*/` | 驱动框架：`tdd_adc_button_register()`、`tdd_audio_register()` | **board** |
| **board** | `boards/<平台>/<板子>/` | **外设注册与初始化**、板级引脚与参数、`board_register_hardware()` | **app**（`user_main` 调一次） |
| **tkl** | `platform/<平台>/tuyaos/tuyaos_adapter/` | 内核/芯片适配：OS、音频、Wi-Fi、Flash | tdd/tdl 之下 |
| **platform** | `platform/<平台>/chip/<SDK>` + `tuyaos/entry/` | 厂商 SDK、构建、厂商入口符号 | SDK 启动 |

**依赖方向严格向下：`app → tdl → tdd →（board 注册）→ tkl → 厂商 SDK`。绝不向上。**

三条分界规则：

1. **board 只注册，不做策略。** board 声明"这块板子有什么外设、参数是什么"；"按键长按该做什么"属于 app。
2. **app 不知道任何平台名。** app 只通过 TDL 拿事件、用设备。
3. **tkl 既不认识 app，也不认识具体板子。** 板级参数由 board 通过头文件/宏喂入（如 `audio_config.h`）。

## 当前违反项

| # | 位置 | 违反 |
|---|---|---|
| 1 | `platform/JIELI/tuyaos/entry/jieli_app_entry.c:13-32` | 平台层注册厂商原始 `SYS_KEY_EVENT`，绕过 tdd/tdl |
| 2 | 同上 `:11`、`:20` | 平台以 weak 符号回调 app —— 依赖方向向上 |
| 3 | `src/ai_components/ai_main/src/ai_chat_main.c:284`、`include/ai_chat_main.h:95` | app 导出平台专用符号 `ai_chat_jieli_key_event`，且进入公共头文件 |
| 4 | `jieli_app_entry.c:79` | 音频底层准备写在厂商入口，而非 `tkl_init()` |

### 两条按键路径的真实关系（修正）

初版本文档称两条路径"靠巧合不冲突"，**这是错的**。实际是**刻意的双路径 + 开关被拆散**：

`boards/JIELI/Kconfig` 有两个带名字的符号 —— `ENABLE_JIELI_NATIVE_KEY`（默认 `y`，厂商原始路径）与 `ENABLE_JIELI_ADKEY_BUTTON`（默认 `n`，TDL 路径）。开关**被遵守**，但遵守点在**应用侧**：

```c
/* src/ai_components/ai_main/src/ai_chat_main.c:445-449 */
#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)
#if !defined(ENABLE_JIELI_NATIVE_KEY) || (ENABLE_JIELI_NATIVE_KEY != 1)
    TUYA_CALL_ERR_LOG(__ai_chat_mode_open_button());   /* 只有非 native 时才建 TDL 按键 */
#endif
#endif
```

而**注册动作在平台侧**（`jieli_app_entry.c:85`）且**无条件**。于是一个决策被拆成两半、跨两个仓库两层。

### 由此暴露的五个问题

| # | 问题 | 影响 |
|---|---|---|
| 1 | **双投递**：`ENABLE_JIELI_NATIVE_KEY=0` + `ENABLE_JIELI_ADKEY_BUTTON=y` 时，app 建 TDL 按键，平台**仍然**注册厂商路径；且 `ai_chat_jieli_key_event` 的函数体只受 `ENABLE_BUTTON` 门控、**不检查 native 开关**，所以它照样转发 → 同一次物理按键投递两次 | 交互错乱 |
| 2 | 开关判断在 app、注册在 platform，一个决策跨两层两仓库 | 架构性 |
| 3 | `ENABLE_JIELI_NATIVE_KEY depends on ENABLE_MEDIA` —— 按键开关依赖媒体开关；有按键但无音频的板子无法选择 native 路径 | 配置错误 |
| 4 | `ai_chat_main.c:36` 把按键名硬编码为 `"ai_chat_button"`，而板级用配置生成的 `BUTTON_NAME`。当前两边恰好相同（`CONFIG_BUTTON_NAME="ai_chat_button"`），但改配置即错位 —— app 会去创建一个**从未被注册过的**第二个按键 | 潜在失效 |
| 5 | `board_com_api.c` include 了 `tdl_audio_driver.h` 却未使用任何 TDL 符号 —— 板级无谓地伸进 app 层命名空间 | 低 |

### 排查确认干净的部分

- `boards/JIELI/board_com_api.c` 的注册逻辑本身正确：只有两处 `tdd_*_register`，无策略、无引脚操作。
- 整个 `platform/JIELI/tuyaos/` 只有**一个** weak 符号，没有别的反向依赖。
- 平台入口只有一处 app 面向符号。

**结论：违反项集中在按键这一条路径，加上音频准备的位置。** 问题 1 是方案 A 要根除的目标 —— 删掉 native 路径后，双投递在结构上不再可能，无需依赖"两个开关配对了才安全"。

### 时序约束

启用板级按键与删除 native 路径必须落在同一次改动里。两者分属不同仓库（Kconfig 在外层、入口在平台），无法共用一个 commit，因此**两个提交必须成对评审、一起验证**。

## 设计

### 决策

**方案 A：板级 Kconfig 默认启用按键注册。**

"板子有什么外设"（board 的事实）与"本次构建要不要用它"（配置的选择）是两件事，方案 A 把前者放 board、后者留配置，保留关闭能力。

已评估并否决：
- **方案 B（board 无条件注册、删除开关）**：概念更少，但失去"同 BSP 不同贴片"场景的退路。
- **方案 C（把厂商原始按键路径挪到 board 并桥接到 TDL）**：板级已通过 `tdd_adc_button_register()` 读同一颗 ADKEY，桥接是纯冗余，且会重新引入"同一硬件两条驱动路径"。

### 改动清单

| # | 文件 | 仓库 | 改动 |
|---|---|---|---|
| 1 | `boards/JIELI/Kconfig` | 外层 | 删 `ENABLE_JIELI_NATIVE_KEY`（native 路径移除后该符号即死；顺带消除问题 3 的错误依赖）；`ENABLE_JIELI_ADKEY_BUTTON` 改为默认启用 |
| 2 | `platform/JIELI/tuyaos/entry/jieli_app_entry.c` | 平台 | 删 `jieli_ai_key_event_handler`、weak extern、`register_sys_event_handler`；删 `tkl_jieli_audio_prepare` 的声明与调用；`app_main` 只留 `tkl_init()` + `tuya_app_main()` |
| 3 | `platform/JIELI/tuyaos/tuyaos_adapter/src/system/tkl_system.c` | 平台 | `tkl_init()` 收编音频准备，`#if defined(CONFIG_MEDIA_ENABLE)` 包住 |
| 4 | `src/ai_components/ai_main/src/ai_chat_main.c` | 外层 | 删 `ai_chat_jieli_key_event()`（问题 2 的 app 侧半边）；删 `:445-449` 的外层条件，使 `__ai_chat_mode_open_button()` 无条件执行；把 `:36` 硬编码的按键名改为使用配置生成的 `BUTTON_NAME`（问题 4） |
| 5 | `src/ai_components/ai_main/include/ai_chat_main.h` | 外层 | 删 `ai_chat_jieli_key_event` 的导出声明 |
| 6 | `platform/JIELI/tuyaos/tuyaos_adapter/src/system/tkl_system.c`、`.../driver/tkl_audio.c` | 平台 | `tkl_jieli_audio_prepare` 的**调用方**从 `jieli_app_entry.c` 迁到 `tkl_system.c`；沿用 `tkl_vad.c` 已有的做法（`tkl_jieli_vad_feed_capture` 就是这样声明的），在 `tkl_system.c` 中 `extern` 声明，不为它新建私有头文件 |
| 7 | `boards/JIELI/board_com_api.c` | 外层 | 删掉未使用的 `#include "tdl_audio_driver.h"`（问题 5）；**注册逻辑本身不改** |

**问题 4 的注意点**：`ai_chat_main.c` 的 `AI_CHAT_BUTTON_NAME` 改为 `BUTTON_NAME` 后，两者必须确实指向同一个字符串。当前 `CONFIG_BUTTON_NAME="ai_chat_button"` 与硬编码值相同，所以改动前后行为一致；验证时需确认生成的 `tuya_kconfig.h` 里 `BUTTON_NAME` 仍是 `"ai_chat_button"`。

**`app_main()` 终态**：

```c
void app_main(void)
{
    (void)tkl_init();      /* 音频底层准备现在在其中 */
    tuya_app_main();
}
```

`irq_info_table` 与 `task_info_table` 保留 —— 它们是厂商 SDK 启动所必需的符号，属于平台层。

### 音频归位的理由

`tkl_jieli_audio_prepare()` 是 TKL 对厂商 `audio_server` 的底层准备（板级外设注册已由 `board_com_api.c` 的 `tdd_audio_register()` 承担）。它放在厂商入口属于"平台调平台自己的初始化"，收进 `tkl_init()` 后平台入口只剩厂商必需符号，层次单一。

### 已知风险：`CONFIG_MEDIA_ENABLE` 必须在两套构建上都成立

本仓库把适配层**编译两遍** —— CMake 编进 `libtuyaos.a`，staged 厂商 Makefile 再编一遍。`tkl_init()` 里的音频准备用 `#if defined(CONFIG_MEDIA_ENABLE)` 包住，该宏目前由 `tools/jieli_build/audio_profile.py` 注入到 **staged Makefile 的全局 `DEFINES`**。

若 CMake 侧没有这个宏，`tkl_system.c` 在两套构建下会编译出**不同内容**：CMake 那份不含调用，staged 那份含。这不会立刻报错（各编各的），但会让"哪一份真正进了固件"变得不确定 —— T6 那次双芯片构建失败（CMake 侧缺 `-I` 编不过）正是同一类"两套构建不一致"的问题。

**处理**：实现时确认 `CONFIG_MEDIA_ENABLE` 在 CMake 与 staged 两条路径上都可见；若不可见，改用适配层已有的等价条件（如 CMake 侧 `target_compile_definitions`），并在提交信息里说明理由。

## 验证

| 检查 | 方法 | 通过标准 |
|---|---|---|
| 编译 | 双芯片构建 `switch_demo`（wl83 + wl82） | `BUILD SUCCESS`，零 undefined reference |
| 无残留 | 全树 grep `ai_chat_jieli_key_event`、`register_sys_event_handler(SYS_KEY_EVENT` | 只剩 `.build/jieli-staging` 生成副本 |
| 两套构建一致 | 分别检查 CMake 与 staged Makefile 下 `tkl_system.c` 的编译命令，确认 `CONFIG_MEDIA_ENABLE` 都可见 | 两条路径都能看到该宏；若不能，说明替代条件及其理由 |
| 按键走 TDD/TDL | **实机** `your_chat_bot` 按键触发 AI 交互 | 按键可用，且无重复触发 |
| 音频未回退 | **实机** `output_speaker` 开机蜂鸣 + 按键回环 | 与改动前一致 |
| 回归 | 两个仓库 Python 测试 | 全绿 |

**"无重复触发"必须实机确认**：这是本次改动唯一无法由静态检查覆盖的风险点。

## 明确不做

- 不改 tdd / tdl 层
- 不改 `board_com_api.c` 的注册逻辑（只删一个未使用的 include）
- 不为厂商原始按键路径保留桥接
- 不改 app 业务逻辑（只删那个平台专用符号、去掉一层条件编译、把硬编码按键名改为用配置值）
- 不改 `platform/JIELI` 的 TKL 适配层结构（那是上一个 spec 的范围）

## 验收标准

- `platform/JIELI/tuyaos/entry/jieli_app_entry.c` 中不含任何应用符号引用，不含厂商原始事件注册；`app_main` 只调用 `tkl_init()` 与 `tuya_app_main()`。
- 全树不存在 `ai_chat_jieli_key_event`；`boards/JIELI/Kconfig` 不存在 `ENABLE_JIELI_NATIVE_KEY`。
- 按键在 `your_chat_bot` 上经 TDD → TDL → app 正常可用，且**单次按压只投递一次**（双投递在结构上已不可能，此项仍需实机确认）。
- 生成的 `tuya_kconfig.h` 中 `BUTTON_NAME` 与 app 查找的名字一致，app 不再有硬编码字面量。
- 音频在 `output_speaker` 上与改动前行为一致。
- 双芯片构建通过，两个仓库测试全绿。
