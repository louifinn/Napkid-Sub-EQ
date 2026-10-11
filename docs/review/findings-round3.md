# Napkid Sub EQ 代码审查台账（第 3 轮 · v0.4.2 闭环复核）

> 复核轮次：第 5 轮（闭环工作流末轮，round 3）
> 复核范围：`7bf7f21...工作树`（未提交改动，16 文件；含 `Source/`、`Tests/`、文档与 `.jucer`）
> 复核方式：对抗性独立审查子代理（规范轴 / 规格轴）+ 复核子代理逐条核对
> 验证基线：本轮门禁——回归 **207 项全过（exit 0）**；Release 增量构建与全量 Rebuild 均零警告（exit 0）
> 结论：既有 17 条发现全部核实到位；新增 / 复开 6 条行动项（R5-F1、R5-F5、R5-F6、STD-04 复开、R3-F6 复开、STD-11 复开），其中计数类两条已在本收尾轮定稿，1 条为残留项
> 状态：已处理（残留与正文待补事项见文末）

---

## 一、第 5 轮复核：已核实修复（17 条）

复核结论：修复声明与工作树代码 / 文档逐条吻合，全部到位。唯一偏差是 STD-04 描述中的行号漂移（±4 行，内容一致——例如声明的 `SubEQ_Spectrum.h:114-116` 实为 `:116`；`SubEQ_SpectrumMath.h:147-158` 实为 `:151-156`），本收尾轮已按当前工作树重核入表。

| 编号 | 核实要点（本轮重核锚点） |
|---|---|
| STD-01 | `SubEQ_SpectrumMath.h:5-15` 文件头全中文；`SubEQ_Spectrum.cpp` 头块中文 |
| STD-02 | `AGENTS.md:34` 顶层速率限定在位；独立复算 `vqtTopDecimationExponent(48000)=4→3000 Hz`、`44100` 因整除回落 `d=2→11025 Hz` |
| STD-04 | 显示管线 `SubEQ_SpectrumMath.h:188`、抽取级联 `SubEQ_VQTDesign.h:332`、层环形装配 `:366` 三条共享原语；运行时（`SubEQ_Spectrum.cpp:146/157/192`）与离线原型（`Tests:212/229/249`）同调一份实现 |
| STD-05 | `SubEQ_SpectrumMath.h:36-42` `kBandsByDensity` / `bandDensityForBands` 密度轴单一来源 |
| STD-06 | `SubEQ_WindowFunctions.h:47-69` `WindowSpec` / `kWindowSpecs` / `windowSpec`；`vqtWindowResolutionMultiplier` 显式四窗型；`ModeSelector::spectrumControlTable()` 五处遍历 |
| STD-07 | `bandCenterFreqs` / `updateBandBounds` 全仓无残留（`grep -c` 在 `SubEQ_Spectrum.h/.cpp` 均为 0） |
| STD-08 | `SubEQ_SpectrumMath.h:120` 具名 `kLn10`；核权重改用 `kWindowTwoPi` |
| STD-10 | `Tests/subeq_fft_test.cpp:37` 单处 `#include <random>` |
| STD-11 | `SubEQ_VQTDesign.h:57`（上限 16）、`SubEQ_Spectrum.h:112`（`MaxStages`）、`SubEQ_Spectrum.cpp:63-65`（`wantedStages` / `jassert` / `min` 钳位）、`Tests:129/180-181` 同式 |
| SPC-01 | `Tests:1371` 空过门闸 `allCrossed`；`:1374-1377` 三条断言；尾段界 = `10/SpectrumReleaseDbPerSecond + 3·dt` |
| SPC-03 | `Tests:1277/1279` 以 `10·log10(1/5)`、`10·log10(1/9)` 书写的双侧数值界（±0.75 dB） |
| R3-F2 | `SubEQ_SpectrumMath.h:7-11` 密度轴与显示范围/地板两条来源声明在位 |
| R3-F5 | `SubEQ_SpectrumMath.h:133-134` `kDisplayRangeDb` / `kDisplayFloorDb`；`SubEQ_Spectrum.h:47-48` `RangeDb` 派生 + `static_assert` 精确镜像 |
| R3-F6 | CHANGELOG `[0.4.2]` 条目、`.jucer` `version="0.4.2"`、`AGENTS.md` 计数回填逐条存在 |
| R4-F3 | `SubEQ_SpectrumMath.h:5/15` 残留英文改中文 |
| R4-F4 | `SubEQ_SpectrumMath.h:151-156` `kDisplayFloorSteps` + 往返相等断言；`Tests:1211` 地板/功率换算运行时核对 |
| R4-F6 | `AGENTS.md:3` 版本头 0.4.2 |

## 二、第 5 轮复核：新发现与复开项

| 编号 | 事项 | 处置 | 状态 |
|---|---|---|---|
| R5-F1 | 回填的回归项数 206 与本 run 实跑记录 207 不符（少算本轮新增的 R4-F4 断言） | 本收尾轮以实跑定稿：`AGENTS.md:24`、`CHANGELOG.md` 测试节、`Documentation/SubEQ_VQT_Feasibility.md:218` 三处均回填 **207**（`git diff HEAD -- Tests/subeq_fft_test.cpp` 净增 6 处 `check(`，201 + 6 = 207，与实跑一致） | 已修复 |
| R5-F5 | 本轮新增低严重度发现（正文未随 run 数据提供；全仓无 `R5-F*` 文本可引） | 待其正文可得时补记；其所述事项以 run 摘要为准（摘要载两条：计数不符、dt/α 残留镜像），计数条见 R5-F1 行处置 | 待补 |
| R5-F6 | 同上（编号已登记） | 同上 | 待补 |
| STD-04（复开） | dt / α 系数组成仍未收进共享头——STD-04 收口后的残留镜像（运行时 `SubEQ_Spectrum.cpp:185-187` 与离线 `Tests:244-246` 各写一份） | **已收口（维护者裁定轮）**：新增共享原语 `vqtTierEnvelopeAlphas()`（`SubEQ_SpectrumMath.h`），运行时与离线原型同调一份；随之显示管线迁移 double（STD-03 裁定），发布边界收窄 float | 已修复 |
| R3-F6（复开） | 文档计数与本 run 实跑 207 的偏差 | 同 R5-F1：收尾轮定稿 207 | 已修复 |
| STD-11（复开） | 台账文本的行号 / 类型名过时 | 本收尾轮在 `findings-round2.md` 处理表中按当前工作树重核锚点（`MaxStages` = `SubEQ_Spectrum.h:112`、把守 = `SubEQ_Spectrum.cpp:63-65`、离线原型 = `Tests:180-181`），并在表中标注「行号按当前工作树重核」 | 已处置 |

补充事实（本轮只读核对）：

- 本轮 diff 未发现硬规范违背——中文注释、音频线程零分配/无锁、单一事实来源三项均已在代码中落实。
- 工作树 `check(` 调用点 164 vs `HEAD`（7bf7f21）158 → 净 +6，与「201 → 207」的算术一致（`grep -c` 实测）。
- `Source/`、`Tests/` 未见越界改动（`AGENTS.md` 的 Agent skills 段与 `docs/agents/` 属流程自举，非修复 diff 内容）。

## 三、第 3 / 4 轮复核归档

第 3、4 轮复核（工作流 round 1、2）的发现与处置见 `docs/review/findings-round2.md` 的「处理记录」段（含 R3-F2/F4/F5/F6 与 R4-F3/F4/F5/F6 逐条处置与 issue 链接）。

## 四、收尾轮处置（文档回填，2026-10-11）

- 计数定稿 207：`AGENTS.md:24`、`CHANGELOG.md`（0.4.2 测试节）、`Documentation/SubEQ_VQT_Feasibility.md:218`。
- 版本一致性：`Napkid Sub EQ.jucer:6` `version="0.4.2"`，`AGENTS.md:3`「当前版本 0.4.2」，CHANGELOG 工程节记明同步范围。
- `AGENTS.md:35` 共享纯计算头文件枚举补全（密度轴 / 显示范围与地板 / `vqtUpdateDisplayBands`）。
- `README.md` 核对：无「当前版本」声明行（`grep -rn "当前版本" README.md` 无命中），功能列表中的 `v0.4.0` / `v0.4.1` 字样为历史特性标注，非当前版本声明——保持原值，无需同步。
- `CHANGELOG.md` `[0.4.2]` 条目按最终数据修订（修复 / 测试 / 工程三节，引用 issue 编号；测试计数 201 → 207）。

## 五、遗留事项

1. ~~**dt/α 残留镜像（STD-04 复开）**：待后续轮次决定是否随显示域口径一并收口。~~ **已收口（维护者裁定轮）**：`vqtTierEnvelopeAlphas()` 共享原语 + 显示管线迁移 double（见上表）。
2. **R5-F5 / R5-F6 正文**：本轮 run 数据仅提供编号与摘要，仓内无正文；其 issue / 正文可得时在本文件补记。

---

## 六、维护者裁决定性（2026-10-11，裁定轮）

第 2 轮审查遗留的两条 needs-info 经维护者裁定并落实：

| 项 | 裁定 | 落实 | 门禁 |
|---|---|---|---|
| STD-03（#3） | **迁移 double** | 显示管线三缓冲改 double（`SubEQ_Spectrum.h:141-143`）；dt/α 收口为共享 `vqtTierEnvelopeAlphas()`（`SubEQ_SpectrumMath.h`，顺带关闭上表 STD-04 复开残留）；仅发布边界 `bandData.store` 收窄 float（`SubEQ_Spectrum.cpp:203`）；运行时与离线原型数值一致 | 回归 207 项 exit 0；增量与全量 Rebuild 零警告；独立复核通过 |
| SPC-06（#17） | **接受现状结案** | 复位值对齐 `kDisplayFloorPower` 已实施（构造器 / resetRuntimeState / 离线镜像三处同源）；残余「未分析邻段」≤~170 ms 自愈暂态与既定跨层陈旧度取舍同模型，不追加排除逻辑 | 无代码改动 |
