# Napkid Sub EQ 代码审查台账（第 2 轮 · v0.4.x 增量）

> 审查轮次：第 2 轮（v0.4.0 + v0.4.1 + 文档增量）· 2026-10-11
> 审查方式：对抗性独立审查子代理门禁（code-review 流程：规范轴 / 规格轴并行子代理）
> 审查范围：`git diff 2b02fab...HEAD`（3 笔提交：142936e v0.4.0 / c94982b v0.4.1 / 7bf7f21 文档；19 文件 +1824/−254）
> 验证基线：
> - 规格轴子代理独立实跑 `Tests/subeq_fft_test.exe`：**201 PASS / 0 FAIL**
> - 主持人抽验：`bandCenterFreqs` 死状态、`numStages` 界、重复 `#include`、新增英文注释行、落差测试分工均与报告一致；构建零警告未由本门禁复核（只读）
> 结论摘要：规范轴 11 条（硬违规 3 / 判断题 7 / correctness 1）；规格轴 7 条（缺失或部分 3 / 范围蔓延 2 / correctness 2）
> 状态：待处理（本门禁仅审查，未修复）

---

## 规范轴（Standards）

### 硬违规（成文规范）

- **STD-01** [`Source/SubEQ_Spectrum.cpp:5-9`、`Source/SubEQ_SpectrumMath.h:7-8`]：v0.4.x 新增/改写的注释块为英文（"Real-time multi-rate VQT spectrum analyzer implementation…"）。违反 AGENTS.md Conventions「代码注释、README、CHANGELOG、Documentation/ 均用简体中文」。同批新写的 `SubEQ_VQTDesign.h` / `SubEQ_WindowFunctions.h` 全中文——判为漏改而非有意（这两个文件头块原为英文，本次新增行仍是英文）。
- **STD-02** [`AGENTS.md:34`]：架构描述「顶层 ~3 kHz」未限定采样率。44.1 kHz 下 `vqtTopDecimationExponent`（`Source/SubEQ_VQTDesign.h:89-97`）因 2 的幂整除约束回落 d=2 → 顶层 11025 Hz；README:230 已列为已知限制，AGENTS.md 的模块描述未同步。
- **STD-03** [`Source/SubEQ_Spectrum.cpp:211-232`]：显示管线用 float（`const float dt`、`attackAlpha/releaseAlpha`、`env`、`10.0f*log10`；`:218` 把 `vqtBandPower`（double）降精度），与「所有 DSP 运算用 double」字面冲突。判为边界项：该管线是显示量而非滤波系数，且 v0.3.x 起即 float。

### 判断题（坏味道）

- **STD-04** Duplicated Code：`Tests/subeq_fft_test.cpp:205-288`（OfflineVqtAnalyzer 的 processSample/pushTierSample/analyzeTier）与 `Source/SubEQ_Spectrum.cpp:136-238` 近逐行复制，且已漂移——钳位阈值 `1.0e-12f` vs 测试 `1.0e-16`、rawDb −200。「被测代码即交付代码」承诺（`SubEQ_VQTDesign.h:22-23`）只覆盖设计数学，不含运行时装配。
- **STD-05** Data Clumps / Primitive Obsession：频谱设置五元组散在 `PluginProcessor.h:125-129`、`PluginProcessor.cpp:239-248`、`SubEQ_Spectrum.h:59`；密度轴编码三处不一（`SubEQ_Spectrum.h:46` 的 `*2`、`SubEQ_Spectrum.cpp:60` 的 `d==0?61:121`、`SpectrumConfig.h` 解码器）。宜打包 `SpectrumSettings` + `operator==`。
- **STD-06** Repeated Switches / Shotgun Surgery：WindowType 映射散在 `WindowFunctions.h:51/95/109` 与 `VQTDesign.h:78-80`（`default:` 静默别名）；GUI 侧同一参数五处平行清单（`ModeSelector.cpp:52-66/104-111/119-126/161-172/240-250`），每加一个频谱参数改 5 处。
- **STD-07** 死状态（抽验确认）：`SubEQ_Spectrum.cpp:32-38` 的 `updateBandBounds` 写 `bandCenterFreqs`；基点 v0.3.4 的读者（`performAnalysis` 内 `fc = bandCenterFreqs[b]`，基点 :158）已随重构删除，全仓无读者——仍在 `configure()`（音频线程）做 61/121 次倍频程中心频率计算写死数组。
- **STD-08** 常数与命名：`SpectrumMath.h:69` 硬写 π 字面量（同批 `WindowFunctions.h:43` 已有 `kWindowTwoPi`）；`SpectrumMath.h:103` 的 `10.0 / 2.302585092994045684` 为匿名 ln10 字面量。
- **STD-09** Speculative Generality：`VQTDesign.h:100` `vqtBandTier` 仅测试调用。
- **STD-10** 复制粘贴残渣（抽验确认）：`Tests/subeq_fft_test.cpp:37` 与 `:41` 重复 `#include <random>`。

### [correctness]

- **STD-11** [`Source/SubEQ_Spectrum.h:99` vs `Source/SubEQ_Spectrum.cpp:64`]：`MaxStages = 24` 而 `numStages = vqtTopDecimationExponent(sr) + VQTNumTiers − 1`（`VQTDesign.h:93` d 上限 16）可达 25 → `prepare()` 越界写 `stages[24]`。触发需 fs ≥ ~143 MHz（实际不可达），属守卫与数组界不一致的潜在缺陷。

### 抽验通过项

核权重和 3/5/9 与 −4.77/−6.99/−9.54 dB 预算、τ 折算 180 dB/s、`.jucer` version=0.4.1 与 CHANGELOG 两个 `[x.y.z] - 日期` 条目齐备、六列布局 508≤520 适配（v0.4.0 的 950 px/五列曾溢出 20 px，被 v0.4.1 修正）。

## 规格轴（Spec）

### 缺失或部分实现

- **SPC-01** Test 26 端到端「顶层段与底层段关断衰减 dB/s 一致」未落地：下落测试仅 200 Hz（顶层）段（`Tests/subeq_fft_test.cpp:1318-1340`）；顶层/底层一致性锁在 Test 25 单元级（`:1240-1263`），未端到端覆盖。计划 Test 26 条目点名要求端到端；CHANGELOG 与附录 A.4 记录了该分工。
- **SPC-02** 接口偏离计划命名：计划要求 `spectrumSmoothingChoiceToOctaves()` + `configure(…, float smoothingOctaves)`；实现为 `spectrumSmoothingChoiceToIndex()`（`Source/SubEQ_SpectrumConfig.h:62-65`）+ `configure(…, int smoothingIndex)`（`Source/SubEQ_Spectrum.h:59-60`）。功能等价、更贴合计划自身「显式查表（不靠浮点换算取整）」的意图；附录 A.2 已按实现记录。
- **SPC-03** 峰预算仅 1/6 档有数值下界（−5.5 dB）；1/3、2/3 档只有单调性断言（`Tests/subeq_fft_test.cpp:1285-1289`）。计划称「各档峰值预算」。轻微。

### 范围蔓延（规格未要求）

- **SPC-04** 窗口 950→1040（`SubEQLookAndFeel.h:18`）：计划算账 950 可容纳；实测（含延迟 chip 232 + 处理组 244）需 1028 px，加宽为必要修正，CHANGELOG/README 已记录。
- **SPC-05** Test 24 校准改为对馈入尾段做功率域时间平均（`Tests:1053-1076`）：计划未提；因新包络对底层帧摆动近乎透明、单点读数依赖摆动相位，属必要适配，CHANGELOG 已披露。

### [correctness]

- **SPC-06** [`Source/SubEQ_Spectrum.cpp:109` 及测试镜像 `Tests:200`]：复位时 `bandPowers` 置 0 而非 1e-6 地板等效功率——configure/复位后首个邻层步距内，跨层边界段平滑被 0 拖低后钳到 −60，瞬态自愈。计划只规定 `envPowers` 复位值；建议对齐 1e-6。
- **SPC-07** `smoothingRadiusBands(99,…)` 钳到最强档 vs 解码器把非法下标回退到 1/6：两层回退语义不同，仅测试可达后者，无实际风险。

### 独立复核通过项

- 核 w(j)=cos²(πj/(2(r+1))) 与权重和恒等 r+1（r=1/2/4/8 → 2/3/5/9）；冲激峰 −3.01/−4.77/−6.99/−9.54 dB；实跑 Off −0.00 / 1/6 −4.77 / 1/3 −6.98 / 2/3 −9.54 dB 与核理论分毫吻合。
- α=1−e^(−dt/τ)（dt=stride×decimation/fs；48k/hop512 顶层 10.67 ms、底层 170.7 ms）；release τ=10/(ln10·180)=24.127 ms ⇒ 180.0 dB/s（计划文本写 8.686/180 系笔误，实现与附录 A.2 用 4.343，正确）。
- 管线顺序（功率→段间平滑→功率包络→10log10→钳位）、属主原则、跨层陈旧度、默认值与非法回退、UI attach/detach 对称、测试计数（实跑 201）均通过。

## 处理记录

> 状态口径：已修复 / wontfix（维持原状）/ needs-info（待维护者裁定）。「复核」列标注独立复核轮次；issue 链接指向本仓库 `louifinn/Napkid-Sub-EQ`。
> 第 5 轮起的复核与残留项记录在 `docs/review/findings-round3.md`。

### 本轮 18 条发现

| 编号 | 结论 | 修复 | issue 链接 | 复核 |
|---|---|---|---|---|
| STD-01 | 已修复 | `SubEQ_Spectrum.cpp` / `SubEQ_SpectrumMath.h` 文件头与新写注释中文化；复核轮补完 `SubEQ_SpectrumMath.h` 文件头残留英文（R4-F3） | [#1](https://github.com/louifinn/Napkid-Sub-EQ/issues/1) | 第 5 轮复核通过 |
| STD-02 | 已修复 | `AGENTS.md:34` 顶层速率限定「48 kHz；44.1 kHz 因 2 的幂整除约束为 11025 Hz」 | [#2](https://github.com/louifinn/Napkid-Sub-EQ/issues/2) | 第 5 轮复核通过 |
| STD-03 | 已修复 | 维护者裁定「迁移 double」：显示管线全程 double——`bandPowers`/`envPowers`/`bandDbScratch` 改 `double`（`SubEQ_Spectrum.h:141-143`），dt/α 经共享 `vqtTierEnvelopeAlphas()`（`SubEQ_SpectrumMath.h`）按层折算，仅原子发布边界收窄 float（`SubEQ_Spectrum.cpp:203`）；运行时与离线原型（本就 double）数值一致 | [#3](https://github.com/louifinn/Napkid-Sub-EQ/issues/3) | 裁定轮门禁：回归 207 项 exit 0、增量与全量 Rebuild 零警告 + 独立复核通过 |
| STD-04 | 已修复 | 三条共享原语：显示管线 `vqtUpdateDisplayBands()`（`SubEQ_SpectrumMath.h:188`）、抽取级联 `vqtDecimationStep()`（`SubEQ_VQTDesign.h:332`）、层环形装配 `vqtPushTierSample()`（`:366`）；运行时与离线原型同调。残留 dt/α 局部镜像记入 `findings-round3.md` | [#4](https://github.com/louifinn/Napkid-Sub-EQ/issues/4) | 第 5 轮复核通过（残留项另行登记） |
| STD-05 | 已修复 | `SpectrumSettings`（五元组 + `operator==`）替代五个缓存整数；密度轴单一来源 `kBandsByDensity` / `bandDensityForBands`（`SubEQ_SpectrumMath.h:36-42`） | [#5](https://github.com/louifinn/Napkid-Sub-EQ/issues/5) | 第 5 轮复核通过 |
| STD-06 | 已修复 | `WindowSpec` 表（`SubEQ_WindowFunctions.h`）；`vqtWindowResolutionMultiplier` 显式覆盖四窗型；`ModeSelector::spectrumControlTable()` 五处遍历同表 | [#6](https://github.com/louifinn/Napkid-Sub-EQ/issues/6) | 第 5 轮复核通过 |
| STD-07 | 已修复 | 移除死状态 `bandCenterFreqs` / `updateBandBounds()`（音频线程不再做 61/121 次中心频率三角函数） | [#7](https://github.com/louifinn/Napkid-Sub-EQ/issues/7) | 第 5 轮复核通过 |
| STD-08 | 已修复 | π 字面量 → `kWindowTwoPi`；匿名 ln10 → 具名 `kLn10` | [#8](https://github.com/louifinn/Napkid-Sub-EQ/issues/8) | 第 5 轮复核通过 |
| STD-09 | wontfix | `vqtBandTier` 仅测试调用——与 `windowSidelobeDb` / `windowEnbwBins` 同属已记录的取舍，保留成本为零 | [#9](https://github.com/louifinn/Napkid-Sub-EQ/issues/9) | 已关闭 |
| STD-10 | 已修复 | Tests 删除重复 `#include <random>` | [#10](https://github.com/louifinn/Napkid-Sub-EQ/issues/10) | 第 5 轮复核通过 |
| STD-11 | 已修复 | 具名 `VQTMaxTopDecimationExponent`（16，`SubEQ_VQTDesign.h:57`）；`MaxStages`（`SubEQ_Spectrum.h:112`）与 `numStages`（`SubEQ_Spectrum.cpp:63`）同源；`prepare()` 对实际返回值 `jassert` + `min` 钳位（`:64-65`）；恒真 static_assert 经 R3-F4 移除 | [#11](https://github.com/louifinn/Napkid-Sub-EQ/issues/11) | 第 5 轮复核通过（行号按当前工作树重核） |
| SPC-01 | 已修复 | 低层段（band 31 / tier 2）端到端关断三条断言：三阈值穿越守卫 + 总下落 ≤ 窗长 + 2·dt + 尾段 ≤ 10/180 s + 3·dt（界由 `set.tiers[]` 与 hop/fs 推导） | [#12](https://github.com/louifinn/Napkid-Sub-EQ/issues/12) | 第 5 轮复核通过 |
| SPC-02 | wontfix | `spectrumSmoothingChoiceToIndex()` + 5 参 `configure()` 与追踪文档一致，且更贴合「显式查表」意图 | [#13](https://github.com/louifinn/Napkid-Sub-EQ/issues/13) | 已关闭 |
| SPC-03 | 已修复 | 1/3 与 2/3 档峰预算各加 ±0.75 dB 核理论数值界（`10·log10(1/5)` / `10·log10(1/9)`） | [#14](https://github.com/louifinn/Napkid-Sub-EQ/issues/14) | 第 5 轮复核通过 |
| SPC-04 | wontfix | 窗口 950 → 1040 px 是六列布局的必要修正（CHANGELOG / README 已记录） | [#15](https://github.com/louifinn/Napkid-Sub-EQ/issues/15) | 已关闭 |
| SPC-05 | wontfix | 校准改尾段功率域时间平均为新包络下的必要适配（已披露） | [#16](https://github.com/louifinn/Napkid-Sub-EQ/issues/16) | 已关闭 |
| SPC-06 | 已结案（维护者裁定：接受现状） | 复位值已对齐地板等效功率 `kDisplayFloorPower`（构造器 / `resetRuntimeState` / 离线镜像三处同源，选项 a 已实施，原描述症状消除）；残余「未分析邻段」≤~170 ms 自愈暂态与已记录的跨层陈旧度取舍（`Documentation/SubEQ_VQT_Feasibility.md:229`）同模型，不追加排除逻辑 | [#17](https://github.com/louifinn/Napkid-Sub-EQ/issues/17) | 裁定结案 |
| SPC-07 | wontfix | 平滑半径内层钳位与解码器回退语义不同——生产路径不可达（Choice 仅 4 项，先经解码器） | [#18](https://github.com/louifinn/Napkid-Sub-EQ/issues/18) | 已关闭 |

### 复核轮新增发现（第 3~5 轮复核，工作流 round 1~3）

| 编号 | 结论 | 修复 | issue 链接 | 复核 |
|---|---|---|---|---|
| R3-F2 | 已修复 | `SubEQ_SpectrumMath.h` 文件头逐条声明密度轴与显示范围/地板两组单一来源；收尾轮同步 `AGENTS.md:35` 枚举 | [#23](https://github.com/louifinn/Napkid-Sub-EQ/issues/23) | 第 5 轮通过；枚举收尾轮补全 |
| R3-F4 | 已修复 | 恒真 `static_assert` 移除，把守改放 `prepare()`（对函数实际返回值 `jassert` + 钳位） | [#21](https://github.com/louifinn/Napkid-Sub-EQ/issues/21) | 第 5 轮通过 |
| R3-F5 | 已修复 | `kDisplayRangeDb` / `kDisplayFloorDb` / `kDisplayFloorPower` 单一来源；`SpectrumAnalyzer::RangeDb` 精确镜像；GUI 初值与复位值派生 | [#22](https://github.com/louifinn/Napkid-Sub-EQ/issues/22) | 第 5 轮通过 |
| R3-F6 | 已修复 | CHANGELOG `[0.4.2]` 条目 + `.jucer` version 0.4.2 + `AGENTS.md` 版本头；计数经第 5 轮与本收尾轮定稿 207 | [#24](https://github.com/louifinn/Napkid-Sub-EQ/issues/24) | 第 5 轮通过；计数收尾轮定稿 |
| R4-F3 | 已修复 | `SubEQ_SpectrumMath.h` 文件头残留英文改中文 | [#25](https://github.com/louifinn/Napkid-Sub-EQ/issues/25) | 第 5 轮通过 |
| R4-F4 | 已修复 | `kDisplayFloorSteps` 具名 + 「往返相等」断言替代截断取模；Test 25 增加地板/功率换算运行时核对 | [#26](https://github.com/louifinn/Napkid-Sub-EQ/issues/26) | 第 5 轮通过 |
| R4-F5 | 已修复 | CHANGELOG 工程节计数自述与测试节对齐（收尾轮随实跑定稿 207） | [#27](https://github.com/louifinn/Napkid-Sub-EQ/issues/27) | 第 5 轮通过 |
| R4-F6 | 已修复 | `AGENTS.md:3` 版本头 0.4.1 → 0.4.2（对齐 `.jucer:6`） | [#28](https://github.com/louifinn/Napkid-Sub-EQ/issues/28) | 第 5 轮通过 |

> 备注：#19 / #20（第 3 轮复核的计数类 issue）已关闭，其诉求被本轮「实跑计数 207」回填覆盖。
