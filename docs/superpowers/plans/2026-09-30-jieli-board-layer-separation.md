# JIELI 板级分层分离 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 JIELI 移植遵守 TuyaOpen 分层 —— 按键经 board 注册、app 经 TDL 使用；平台层不再反向依赖应用符号；音频底层准备归位到 TKL。

**Architecture:** 依赖方向严格向下 `app → tdl → tdd →（board 注册）→ tkl → 厂商 SDK`。board 只注册不做策略；app 不认识任何平台名；tkl 不认识 app 也不认识具体板子。

**Tech Stack:** C（杰理 pi32v2 工具链）、Python 3（构建桥接）、Kconfig、CMake

**Spec:** `platform/JIELI/docs/superpowers/specs/2026-09-30-jieli-board-layer-separation-design.md`

## Global Constraints

- **app 业务逻辑零改动。** 只允许：删一个平台专用符号、去掉一层条件编译、把硬编码按键名换成配置值。
- **不改 tdd / tdl 层。**
- **不改 `board_com_api.c` 的注册逻辑**（只删一个未使用的 include）。
- **不为厂商原始按键路径保留桥接。**
- **双仓库改动成对评审。** Kconfig 在外层仓库、平台入口在平台仓库，无法共用 commit，必须一起验证。
- 构建命令（Windows，必须 venv python + PowerShell 且 PATH 移除 Git 的 `usr/bin`，CWD 为示例目录）：
  `"d" | & <root>\.venv\Scripts\python.exe <root>\tos.py build`
- 外层仓库 `.gitignore` 忽略 `docs/superpowers`；平台仓库提交它。本文档与 spec 都在平台仓库。

---

## File Structure

| 文件 | 仓库 | 职责 |
|---|---|---|
| `boards/JIELI/Kconfig` | 外层 | 板级配置符号；本次删除 native 按键开关、默认启用 TDL 按键 |
| `boards/JIELI/board_com_api.c` | 外层 | 板级外设注册（`board_register_hardware`）—— 注册逻辑不改 |
| `src/ai_components/ai_main/src/ai_chat_main.c` | 外层 | AI 应用：按键事件消费方 |
| `src/ai_components/ai_main/include/ai_chat_main.h` | 外层 | 其公共头 |
| `platform/JIELI/tuyaos/entry/jieli_app_entry.c` | 平台 | 厂商入口：只应保留 `irq_info_table` / `task_info_table` / `app_main` |
| `platform/JIELI/tuyaos/tuyaos_adapter/src/system/tkl_system.c` | 平台 | TKL 系统层；`tkl_init()` 在此 |
| `platform/JIELI/tuyaos/tuyaos_adapter/src/driver/tkl_audio.c` | 平台 | 音频 TKL；提供 `tkl_jieli_audio_prepare` |

---

### Task 1: 让板级成为唯一的按键路径

**Files:**
- Modify: `boards/JIELI/Kconfig`
- Modify: `src/ai_components/ai_main/src/ai_chat_main.c`
- Modify: `src/ai_components/ai_main/include/ai_chat_main.h`
- Modify: `boards/JIELI/board_com_api.c`
- Modify: `platform/JIELI/tuyaos/entry/jieli_app_entry.c`

**Interfaces:**
- Consumes: 现有的 `tdd_adc_button_register(BUTTON_NAME, &button_cfg)`（`board_com_api.c:46`，不改）
- Produces: 按键只经 `board → tdd_button → tdl_button → __ai_button_function_cb` 一条路径

**为什么这五处必须同一个改动落地**：启用板级按键与删除 native 路径若分两次，中间态会双投递（见 spec「五个问题」#1）。

- [ ] **Step 1: 记录改动前的按键配置事实（作为对照基线）**

```bash
cd <root>
grep -n "BUTTON_NAME" apps/tuya.ai/your_chat_bot/.build/include/tuya_kconfig.h
grep -n "CONFIG_BUTTON_NAME\|ENABLE_JIELI_NATIVE_KEY\|ENABLE_JIELI_ADKEY_BUTTON" \
     apps/tuya.ai/your_chat_bot/app_default.config
```
Expected: `BUTTON_NAME "ai_chat_button"`；app 配置里没有后两个符号（用 Kconfig 默认值）。

- [ ] **Step 2: Kconfig —— 删 native 开关，默认启用 TDL 按键**

在 `boards/JIELI/Kconfig` 中删除整个 `config ENABLE_JIELI_NATIVE_KEY` 块（含 `bool`、`default y`、`depends on ENABLE_MEDIA`、`help` 四行），并把 `ENABLE_JIELI_ADKEY_BUTTON` 的 `default n` 改为 `default y`。

改完后确认：

```bash
grep -n "ENABLE_JIELI_NATIVE_KEY" boards/JIELI/Kconfig || echo "native key symbol gone"
grep -n -A2 "config ENABLE_JIELI_ADKEY_BUTTON" boards/JIELI/Kconfig
```
Expected: 第一行输出 `native key symbol gone`；第二处显示 `default y`。

- [ ] **Step 3: 平台入口 —— 删掉整条按键路径**

删除 `platform/JIELI/tuyaos/entry/jieli_app_entry.c` 中的：
- `extern void ai_chat_jieli_key_event(int event) __attribute__((weak));`（第 11 行）
- 整个 `static void jieli_ai_key_event_handler(struct sys_event *event) { ... }`（第 13–32 行）
- `app_main()` 里的 `(void)register_sys_event_handler(SYS_KEY_EVENT, 0, 1, jieli_ai_key_event_handler);`（第 85 行）
- 随之不再需要的 `#include "event/event.h"` 与 `#include "event/key_event.h"`

`app_main()` 此时应为（音频那两行在 Task 2 处理）：

```c
void app_main(void)
{
#ifdef CONFIG_MEDIA_ENABLE
    OPERATE_RET audio_ret = tkl_jieli_audio_prepare();
    if (audio_ret != 0) {
        printf("[JIELI_AUDIO] audio prepare failed: %d\n", audio_ret);
    }
#endif
    (void)tkl_init();
    tuya_app_main();
}
```

- [ ] **Step 4: 应用 —— 删符号、去条件、改用配置的按键名**

在 `src/ai_components/ai_main/src/ai_chat_main.c` 中：
- 删除整个 `void ai_chat_jieli_key_event(int event) { ... }`（第 284–296 行）
- 删除 `#if !defined(ENABLE_JIELI_NATIVE_KEY) || (ENABLE_JIELI_NATIVE_KEY != 1)` 这一层（第 446、448 行），使 `TUYA_CALL_ERR_LOG(__ai_chat_mode_open_button());` 在 `ENABLE_BUTTON` 下无条件执行；保留外层 `#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)` / `#endif`
- 把第 36 行 `#define AI_CHAT_BUTTON_NAME    "ai_chat_button"` 改为使用配置值：

```c
/* The board registers this button through TDD, so the name must be the one the
 * board used -- the configured BUTTON_NAME, not a local literal. */
#define AI_CHAT_BUTTON_NAME    BUTTON_NAME
```

在 `src/ai_components/ai_main/include/ai_chat_main.h` 中删除 `void ai_chat_jieli_key_event(int event);`（第 95 行）。

- [ ] **Step 5: 板级 —— 删掉未使用的 TDL include**

删除 `boards/JIELI/board_com_api.c` 第 9 行的 `#include "tdl_audio_driver.h"`（该文件不使用任何 TDL 符号，板级不应伸进 app 层命名空间）。**注册逻辑不动。**

- [ ] **Step 6: 静态验证 —— 无残留、无反向依赖**

```bash
cd <root>
grep -rn "ai_chat_jieli_key_event" --include=*.c --include=*.h . | grep -v "/.build/" || echo "symbol gone"
grep -rn "ENABLE_JIELI_NATIVE_KEY" --include=*.c --include=*.h --include=Kconfig . | grep -v "/.build/" || echo "switch gone"
grep -rn "register_sys_event_handler(SYS_KEY_EVENT" --include=*.c . | grep -v "/.build/" || echo "vendor key path gone"
grep -rn "__attribute__((weak))" platform/JIELI/tuyaos/ | grep -v "/.build/" || echo "no weak symbols left in tkl"
```
Expected: 四行都输出各自的 "gone / no ..." 消息。

- [ ] **Step 7: 确认按键名仍然一致**

```bash
grep -rn "AI_CHAT_BUTTON_NAME" src/ai_components/ai_main/src/ai_chat_main.c | head -3
```
Expected: 定义处为 `#define AI_CHAT_BUTTON_NAME    BUTTON_NAME`；使用处仍是 `tdl_button_create(AI_CHAT_BUTTON_NAME, ...)`。

（真正的一致性由 Task 3 的构建后检查确认 —— `BUTTON_NAME` 是 Kconfig 生成值，此时还没生成。）

- [ ] **Step 8: Commit（两个仓库分别提交，成对评审）**

```bash
cd platform/JIELI
git add tuyaos/entry/jieli_app_entry.c
git commit -m "refactor(jieli): drop the vendor key path from the platform entry

The platform registered a vendor SYS_KEY_EVENT handler and forwarded it to the
app through a weak symbol. Board peripherals are registered by the board through
TDD and consumed by the app through TDL; the platform should know about neither.

Removing it also removes the only way a single key press could be delivered
twice: with the native path gone, the board's tdd_adc_button_register is the
sole source."
cd <root>
git add boards/JIELI/Kconfig boards/JIELI/board_com_api.c \
        src/ai_components/ai_main/src/ai_chat_main.c \
        src/ai_components/ai_main/include/ai_chat_main.h
git commit -m "refactor(jieli): make the board the only key path

ENABLE_JIELI_NATIVE_KEY selected the vendor key path and was honoured in the
app while the registration lived in the platform -- one decision split across
two layers and two repos. With the platform side gone the switch has nothing to
select, so it goes too, and the TDL button becomes the default.

The app's button name was a local literal that happened to match the configured
BUTTON_NAME; it now uses the configured value, so changing CONFIG_BUTTON_NAME
can no longer silently point the app at a button nobody registered."
```

---

### Task 2: 把音频底层准备归位到 tkl_init()

**Files:**
- Modify: `platform/JIELI/tuyaos/entry/jieli_app_entry.c`
- Modify: `platform/JIELI/tuyaos/tuyaos_adapter/src/system/tkl_system.c`

**Interfaces:**
- Consumes: `tkl_jieli_audio_prepare()`（`tkl_audio.c:166`，实现不改）
- Produces: `tkl_init()` 成为 TKL 初始化的唯一入口，含音频底层准备

- [ ] **Step 1: 平台入口删掉音频准备**

删除 `jieli_app_entry.c` 中的：
- `#ifdef CONFIG_MEDIA_ENABLE` 包着的 `OPERATE_RET tkl_jieli_audio_prepare(void);` 声明（第 36–38 行）
- `app_main()` 里的 `#ifdef CONFIG_MEDIA_ENABLE ... #endif` 整块（第 77–83 行）

`app_main()` 终态：

```c
void app_main(void)
{
    (void)tkl_init();
    tuya_app_main();
}
```

- [ ] **Step 2: tkl_init() 收编音频准备**

`tkl_system.c` 中，在 `OPERATE_RET tkl_init(void)` 之前加 extern 声明（沿用 `tkl_vad.c` 对 `tkl_jieli_vad_feed_capture` 的既有做法，不新建私有头文件）：

```c
/* Declared here rather than in a private header, matching how tkl_vad.c
 * declares tkl_jieli_vad_feed_capture. */
#if defined(CONFIG_MEDIA_ENABLE)
OPERATE_RET tkl_jieli_audio_prepare(void);
#endif
```

并把 `tkl_init()` 改为：

```c
OPERATE_RET tkl_init(void)
{
#if defined(CONFIG_MEDIA_ENABLE)
    /* Vendor audio_server bring-up. The board's own audio registration happens
     * separately, through board_register_hardware() -> tdd_audio_register(). */
    return tkl_jieli_audio_prepare();
#else
    return OPRT_OK;
#endif
}
```

- [ ] **Step 3: 验证 `CONFIG_MEDIA_ENABLE` 在两套构建上都可见**

本仓库把适配层编译两遍（CMake 编 `libtuyaos.a`，staged 厂商 Makefile 再编一遍）。若只有一侧能看到该宏，`tkl_system.c` 会编出两份不同内容。

```bash
cd <root>/apps/tuya_cloud/switch_demo
grep -rn "CONFIG_MEDIA_ENABLE" .build/jieli-staging/build/sdk/apps/demo/demo_hello/board/wl83/Makefile | head -3
grep -rn "CONFIG_MEDIA_ENABLE" <root>/platform/JIELI/tuyaos/tuyaos_adapter/CMakeLists.txt | head -3
```
Expected: staged Makefile 的 `DEFINES` 里有它。若 CMake 侧没有，在 `tuyaos_adapter/CMakeLists.txt` 的 `target_compile_definitions` 中补上（与 `JIELI_SELECTED_CHIP_DEFINE` 同一处），并在提交信息里说明理由。

**这一步不能跳过** —— 两套构建不一致正是上一次双芯片构建失败的根因。

- [ ] **Step 4: Commit**

```bash
cd platform/JIELI
git add tuyaos/entry/jieli_app_entry.c tuyaos/tuyaos_adapter/src/system/tkl_system.c
git commit -m "refactor(jieli): move the audio bring-up into tkl_init()

tkl_jieli_audio_prepare() is TKL preparing the vendor audio_server, not a board
peripheral -- the board registers its codec through tdd_audio_register(). Having
the vendor entry call it made app_main do platform work it does not own.

With this and the key path gone, app_main is only what the vendor SDK requires:
the two tables and tuya_app_main()."
```

---

### Task 3: 双芯片构建验证

**Files:** 无（验证任务）

**Interfaces:**
- Consumes: Task 1 与 Task 2 的改动
- Produces: 两芯片构建通过的证据；生成的 `BUTTON_NAME` 与 app 一致

- [ ] **Step 1: 构建 wl83**

```powershell
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*Git\usr\bin*' }) -join ';'
Set-Location <root>\apps\tuya.ai\your_chat_bot
"d" | & <root>\.venv\Scripts\python.exe <root>\tos.py build
```
Expected: `BUILD SUCCESS`，零 `undefined reference`。

- [ ] **Step 2: 确认按键名一致（本步是 Task 1 Step 7 的兑现）**

```bash
cd <root>
grep -n "define BUTTON_NAME" apps/tuya.ai/your_chat_bot/.build/include/tuya_kconfig.h
```
Expected: `#define BUTTON_NAME "ai_chat_button"` —— 与改动前 Step 1 记录的基线一致。若不一致，app 会去找一个没注册的按键，必须停下查明。

- [ ] **Step 3: 确认板级按键注册真的被编进去了**

```bash
grep -c "ENABLE_JIELI_ADKEY_BUTTON" <root>/apps/tuya.ai/your_chat_bot/.build/include/tuya_kconfig.h
```
Expected: ≥1 且值为 1（默认启用生效）。若为 0，Task 1 Step 2 的 Kconfig 改动没生效。

- [ ] **Step 4: 切 wl82 构建**

按 memory `jieli-build-shell-requirements`：clean 后**必须先删 `.build`**，否则 CMake 会复用缓存的 `JIELI_SDK_ROOT` 用错 SDK 头文件。

```powershell
Set-Location <root>\apps\tuya.ai\your_chat_bot
# 按 app_default.config 实际符号切换板级（读实际值，不预填）
& <root>\.venv\Scripts\python.exe <root>\tos.py clean
Remove-Item -Recurse -Force .build
"d" | & <root>\.venv\Scripts\python.exe <root>\tos.py build
```
Expected: `BUILD SUCCESS`。两条腿都构建 `your_chat_bot` —— 按键路径是本次改动的对象，两芯片都该验它。

- [ ] **Step 5: 记录两次构建的输出**

把两次的结尾片段与产物路径写进 PR 描述。未实际跑通不得声称通过。

---

### Task 4: 实机验证

**Files:** 无（验证任务）

**Interfaces:**
- Consumes: Task 3 的 wl83 产物
- Produces: 按键单次投递 + 音频未回退的实机证据

**这是本次唯一无法靠静态检查覆盖的风险点。**

- [ ] **Step 1: 烧录 `your_chat_bot`**

```powershell
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*Git\usr\bin*' }) -join ';'
Set-Location <root>\apps\tuya.ai\your_chat_bot
"d" | & <root>\.venv\Scripts\python.exe <root>\tos.py flash
```
若报 `Device Offline`，请人复位板子进下载模式后重试（时序问题，非失败）。

- [ ] **Step 2: 按键可用且只触发一次**

按一次 K1，观察 AI 交互。Expected: 触发一次。

**重点看有没有重复**：单次按压若触发两次对话/两次提示音，说明双投递仍在，改动未达成目标。这是本次改动的核心验收项。

- [ ] **Step 3: 长按与单击分别正确**

长按 → 进入 HOLD 模式；单击 → 单次触发。Expected: 与改动前一致。

- [ ] **Step 4: 音频未回退**

烧 `output_speaker`，确认开机 1 kHz 蜂鸣与按键录音回环仍正常。Expected: 与改动前一致。

- [ ] **Step 5: 记录实机结果**

四项都通过才可继续。任一不通过则回到 Task 1/2 检查。

---

### Task 5: pin 更新与提交

**Files:**
- Modify: `platform/platform_config.yaml`

**Interfaces:**
- Consumes: Task 4 验证通过的两个分支
- Produces: 更新后的 pin 与两个 PR

- [ ] **Step 1: 取平台分支最终 commit**

```bash
cd platform/JIELI && git rev-parse HEAD
```

- [ ] **Step 2: 更新 pin 与配置测试中的字面 SHA**

把 `platform/platform_config.yaml` 的 JIELI `commit:` 与 `tests/platform/test_jieli_config.py` 中对应断言一起改为 Step 1 的真实 SHA（两者必须同步，否则测试会红）。**不要凭短 SHA 补全** —— 用 `git rev-parse` 的完整输出。

- [ ] **Step 3: 确认不再报版本不匹配**

```powershell
Set-Location <root>\apps\tuya_cloud\switch_demo
"d" | & <root>\.venv\Scripts\python.exe <root>\tos.py build
```
Expected: 不再出现 `The commit required by the platform is ..., but currently ... is being used.`

- [ ] **Step 4: 两个仓库测试全绿**

```bash
cd <root> && python -m pytest tests/ -q
cd platform/JIELI && python -m pytest tests/ -q
```
Expected: 都通过。

- [ ] **Step 5: Commit 并推送**

```bash
cd <root>
git add platform/platform_config.yaml tests/platform/test_jieli_config.py
git commit -m "chore(platform): pin JIELI to the board-layer separation"
git push fork-pr codex/tuya-ai-hold-chain

cd platform/JIELI
git push origin codex/jieli-audio-on-refactor
```

- [ ] **Step 6: 更新两个 PR**

在 PR #11 与 #728 上说明本次分层归位：删掉了什么、为什么、实机验证结果。**两个提交成对**，评审者需要同时看到两侧。

---

## Self-Review

**Spec coverage：**
- 分层定义与三条规则 → 本文档 Global Constraints + File Structure
- 五个问题 → Task 1 Step 2–5（#1/#2/#3/#4/#5 逐条对应）
- 音频归位 → Task 2
- `CONFIG_MEDIA_ENABLE` 两套构建风险 → Task 2 Step 3 + Task 3 Step 1/4
- 双投递实机确认 → Task 4 Step 2
- 验收标准 5 条 → Task 1 Step 6、Task 3 Step 2/3、Task 4、Task 5 Step 4

**已知缺口（执行时确认）：**
- Task 3 Step 4 的 AC791 板级切换符号需按当时的 `app_default.config` 实读，不预填。
- Task 2 Step 3 若发现 CMake 侧缺 `CONFIG_MEDIA_ENABLE`，补法需按当时 `tuyaos_adapter/CMakeLists.txt` 的实际结构确定。
