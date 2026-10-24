# 更新日志

Napkid Sub EQ 的所有重要变更都将记录在此文件中。

## [0.5.0] - 2026-10-22

> 全量审查修复：FIR 切换改为 T2 影子预热无缝路径，输出级后置主增益/旁路，线性相位升级 Type-I 密集设计，频谱/VQT 与 GUI 交互收口，字体与许可证合规。

### 修复

- **FIR 切换（T2）**：新增 `captured/committed` 输入历史环 + 后台精确边界 warm 合成（overlap-save 在 `kM-M` 求累加器，回放到 `kM`，再前馈 `r` 样本到任意 host-block 起点 `T=kM+r`）。设计+target 元数据先发布，音频协调器据此提前调度延迟切换；warm 就绪后附加，音频仅在 design version/target/warm 三者一致时采纳。允许最多一个当前 host block 的音频线程追赶；错过则保留旧设计并请求同一设计的新 warm 目标，不丢弃设计、不无限静音。同 groupDelay 走 1024 样本交叉淡化（两侧都处理真实输入），异 groupDelay 经 packed PDC 握手 + 有界门控后切换。**离线渲染**（`AudioProcessor::isNonRealtime()`）走同步路径：音频线程在当前边界直接构建设计与 warm，冷启动 65536 从首块进入 FIR，且不套用实时门控。修复了交叉淡化把暂存缓冲当卷积输入、最小相位长度先减后增时新侧 overlap 未清零、以及 OLA 状态容量不足导致的越界风险。
- **输出级**：主增益与旁路从 EQEngine/FIR 设计目标移到 OutputStage；主增益逐样本 ramp（目标变化才重启），旁路使用 activeLatency 延迟补偿的 dry/wet；latency 变化不清空 dry 历史（旁路混合时 128 样本交叉淡化），主增益自动化不再触发 FIR 重设计。EQEngine 删除临时缓冲，音频线程无 malloc/OOM 路径；IIR 桥按 host-facing latency 延迟，FIR 预热期间不再输出 0 延迟信号。
- **平滑与状态**：节点启停/系数平滑改为 32 样本子块推进；Tilt 第二 biquad 从 identity 淡入时清零状态；FIR→Zero Latency 复位 IIR 状态；重新启用完全淡出的节点时清 biquad 状态；采样率变化直接落到新系数；Biquad 系数增加 NaN/有限性防护；总线响应上限从 1e9 提升到 1e13。
- **FIR 设计**：线性相位改为 Type-I 奇数抽头（标称 4096/16384/65536 内部为 4095/16383/65535），消除偶数 Type-II 的 0.5 样本群延迟误差并允许非零 Nyquist；设计改为 4× 密集网格 + BH-4 窗化，不再逐 bin 强制 `H(π)=0`；最小相位 legacy 8× 候选改为按需计算；频率响应构建改为按实际抽头数安全构造。
- **频谱/VQT**：抽取链 4 阶升为 12 阶 Butterworth（截止 0.28×输出率，折叠带 ≥60 dB）；底部帧长改为“至少 2 s”的 2 的幂；低频频段镜像支撑未做抑制（gamma 区帧内不足两个周期的物理限制，见 VQT 文档 §8；删除此前无效的 mirrorGuard）；新增 `spectrum_range`（24/60 dB，默认 60，60 dB 模式有独立刻度）；默认平滑 Off 在首次配置时真正生效；仅改平滑档或 hop 不再整状态复位；编辑器重开后由 GUI 线程立即清空旧频谱；抽取链输入增加 NaN/Inf 清洗；24 dB 档恢复与 EQ 网格同轴；低频显示群延迟约 1.4 s@48k/2.2 s@4k 在文档中标注。
- **GUI**：APVTS 监听器不再在音频线程 `callAsync`（改为原子脏标记 + Timer）；节点命中优先于标签、标签在顶部翻转到节点下方；ComboBox 文本宽度只扣一次箭头区；响应/相位曲线裁剪到绘图区；主增益标题/侧边留白不再误触 ±24 dB；类型菜单使用 SafePointer，编辑器析构关闭菜单；手势 RAII 防泄漏；新增 TooltipWindow；FIR 模式幅度/相位改用同一设计快照；相位单点无效不再清空整条曲线；首次进入 FIR 时与 IIR 路径做 1024 样本交叉淡化，不再静音等待 warm-up。
- **字体/许可**：Avenir 替换为 Inter Regular/Bold（SIL OFL 1.1），删除商业字体资源；LICENSE 补全 GPLv3 全文与 Inter OFL 第三方说明；移除 `.jucer` 个人邮箱。

### 测试与工程

- 离线回归 **538 → 607 项全部通过**（新增精确 warm 合成/容量守卫、历史环、OutputStage 延迟保持/ramp 完成/float 钳位、8 类型 biquad、FIR 边界/NaN、安全频率响应构建、计数守卫等）。
- 新增 JUCE 控制台集成测试 `Tests/subeq_runtime_test.cpp`：驱动真实 `SubEQAudioProcessor`，验证 512/4096 host block 下的 65536 Linear、Min↔Linear、旁路、重复 ZL↔FIR 的 PDC 上报/ack 与输出连续性、**96 kHz/128 块**、**非 512 的 1000 块**、离线首块可闻、旧 session 缺 `spectrum_range` 迁移到 60 dB 默认（0 失败）。
- `.jucer` 补齐此前遗漏的 13 个共享头与 4 个新头，版本升至 0.5.0；Projucer 重存生成 BinaryData/VS 工程；Release x64 构建通过。
- 参数总数 50 → 51（新增 `spectrum_range`）；参数 ID 与既有 choice 索引保持兼容。

## [0.4.6] - 2026-10-18

> 遗留事项收口：F6 输出范围硬化、`<1 bin` 物理限制文档化、审查 finding 台账结清。

### 修复

- **F6（`Source/SubEQ_FFTConvolver.h`）**：overlap-add 的累加/输出始终是 `double`；即使结果有限，也可能超出输出类型 `T`（音频路径为 `float`）的可表示范围，`static_cast<T>` 会静默变成 `±inf`。现以 `std::numeric_limits<T>::max()` 为上界，超出（以及 NaN/Inf）的块输出置零并冲刷累加器——与原有污染遏制同一路径；音频线程零分配/无锁契约不变。

### 文档

- README（中/英）已知限制补充：N 抽头 FIR 的频率采样分辨率固定为 `fs/N`；当 `fc·N/fs < 1`（拐点低于一个设计 bin）时目标幅频形状无法由该长度 FIR 表示，属物理限制而非实现缺陷；最小相位 FIR 的解析零点矩阵据此只覆盖可分辨组合。
- 审查 finding 台账结清：F1 / F2 / F3（已分别由 0.4.4 / 0.4.3 / 0.4.3-0.4.5 修复）、F5（M7 的解析零点 + 全带 eligibility + 矩阵覆盖可分辨范围，`<1 bin` 为物理限制）、F6（本版硬化）标记 `fixed`；F4（Tower scope glob 行为）标记 `wontfix`，工作流以显式 `/**` 规避。

### 测试与工程

- 回归 **536 → 538 项全部通过**：新增 Test 5c（有限但超出 `float` 范围的卷积值被遏制、超范围块输出置零并冲刷累加器）。
- 版本 0.4.5 → 0.4.6：`Napkid Sub EQ.jucer`、`AGENTS.md`、`JuceLibraryCode` 生成元数据同步；Release x64 零警告；VST3 `moduleinfo.json` 报 0.4.6。

## [0.4.5] - 2026-10-18

> 最小相位 FIR 设计改为按滤波器拓扑**解析剥离** DC/Nyquist 传输零点：先把已知的 `(1-z^-1)^dc (1+z^-1)^nyq` 从目标幅频中除掉，对剩余平滑幅频做最小相位倒谱设计，再与短因子线性卷积，得到 `H(0)=0` / `H(π)=0` 的精确时域结构零点。单节点（2 阶零点）解析候选直接生效并带精确零点；多节点（4–6 阶）解析候选因剩余幅频动态范围过大，要么被 `max|h|` sanity gate 丢弃（灾难性相消；该 gate 只约束解析候选，legacy 安全基线不受峰值门限约束），要么被新增的**全带 eligibility**（128 点直接 DTFT，target ≥ −80 dB 处 |ΔdB| ≤ 6 dB、峰值过冲 ≤ 3 dB）剔除其高频截断振铃，最终由 0.4.4 legacy 候选兜底，再由精确零点投影把 `H(0)/H(π)` 修到机器精度（多节点 1e-17…1e-15）。0.4.4 的 8× 自动路径保留为调用方未标注零点时的位级等价回退。回归 219 → 536 项全部通过，Release x64 零警告。

### 修复

- **最小相位 FIR 的 DC/Nyquist 零点解析剥离**（`Source/SubEQ_FIRDesign.h` `designMinimumPhaseFIR`）：
  - 根因：该函数在 N 点设计网格上采样 `log(|H(w)| + 1e-12)`。高通在 DC（低通在 Nyquist）的传输零点使采样幅度被 `epsilon` 钳位成 −240 dB 的孤立尖峰，倒谱法把它当成宽带特征；0.4.4 的 8× 过采样网格只是把尖峰推得更窄、抑制倒谱时域混叠，零点本身仍是 epsilon 近似，不是精确的结构零点。
  - 新签名：`designMinimumPhaseFIR(magnitude, firLength, latencyOut, dcZeroOrder = 0, nyquistZeroOrder = 0, sampleRateHz = 0.0)`。后两个零点参数由调用方按滤波器拓扑解析给出（目标在 z=1 / z=−1 的传输零点阶数）；默认 0 表示未知，保持 0.4.4 旧自动路径——`N` 网格最小幅度 < `kMinPhaseDeepNullThreshold`（1e-6）时走 `kMinPhaseLegacyOversampling`（8×）log 幅频设计，否则 1× 精确路径（该路径抽成 `detail::designLegacyMinimumPhase`，回归以 FNV 哈希实测与 0.4.5 的 3 参调用逐位一致）。`sampleRateHz` 只影响**带内拟合排序**的带宽（全带 eligibility 与它无关），默认 0 时拟合退回全带。Test 11 的平滑目标 1e-5 断言与 NaN 回退冲激不变。
  - 解析剥离：`M = N·os`、`half = M/2`，逐点计算 `logRest[i] = log(mag(w)+1e-12) − dc·log(2 sin(w/2)+1e-300) − nyq·log(2 cos(w/2)+1e-300)`（`w = πi/half`）；端点处用相邻两点线性外推取极限（`logRest[0] = 2·logRest[1] − logRest[2]`，Nyquist 对称），这是取代 epsilon 钳位、消除 −240 dB 尖峰的关键。随后实偶对称 → IDFT 得倒谱 → 因果化（c[0] 与 c[M/2] 取 1、正半轴 ×2、负半轴置 0）→ DFT → exp → IDFT 得 `h_rest`；全程单缓冲就地完成。
  - 因子与长度：`(1-z^-1)^dc (1+z^-1)^nyq` 的二项式系数（长度 `dc+nyq+1`）由逐次一阶卷积生成；取 `h_rest` 前 `L = N−(dc+nyq)` 个抽头后与因子做线性卷积，输出长度恰为 `L + (dc+nyq+1) − 1 = N`，**不截断乘积**。若先把长度 `N+dc+nyq` 的完整乘积截回 N，DC/Nyquist 相消会因丢弃尾部而失效（实测 96 kHz HP30 在 0.5 Hz 漏 28 dB）。
  - 候选选择（v0.4.5 复审修复，round 2/3）：带标注零点时改为在 **{解析剥离 os=1, 解析剥离 os=4, legacy 8× 自动路径}** 三个候选间选优。解析候选先做有限性检查与 **`max|h| ≤ kMinPhaseMaxTapMagnitude`（1e3）sanity gate**——多节点零点（dc+nyq ≥ 4）时剩余幅频 R(0) ≈ (1/w_c)^{2k}，截断 `h_rest` 后与二项式因子的双精度卷积会灾难性相消，产生 1e4…1e50 的抽头尖峰与数十 dB 通带误差（2×HP 20+40 Hz 实测峰值 4.7e4、3×HP 峰值 9.9e9、8×HP 峰值 1.7e39），这些候选被 gate 丢弃；但 HP30+BP100 这类组合能过 `max|h|` gate 却在 >500 Hz 有 50–109 dB 截断振铃，round 2 仅按带内拟合仍会选中它（复审 P1）。**该 gate 只约束解析剥离候选**（它是「灾难性相消」探针，不是目标增益的合法上限）：legacy 候选在 gate **之前**保留（只要求有限性），作为最终兜底——合法高增益目标（如 HP30 + 2×LowShelf(+24@200) + 2×HighShelf(+24@200) + master +24 的 +72 dB 级联，通带峰值 ≈4e3）不得被峰值门限误杀（round 4 复审 P2）。
  - **全带 eligibility（v0.4.5 复审 P1 修复，round 3）**：候选通过 sanity gate 与零点投影后，再在 `w ∈ [π/65536, 0.999π]` 的 128 个对数点直接 DTFT：任一 `target ≥ −80 dB` 的点满足 `|firDb − targetDb| > 6 dB`，或 `firPeak > targetPeak + 3 dB`，即判不合格——这是「高频截断振铃」的硬门，带内拟合对这些点不可见。只有合格候选才用带内拟合误差排序；**若 factored 候选全不合格则回退 legacy**（即使 legacy 也不合格：它仍是不带解析剥离、无高频振铃的 0.4.4 安全基线），legacy 非有限再回退冲激。实测复审 P1 的 9 个失败 cell（4 个单节点 + 5 个多节点）全带最大误差由 **10.6–109.2 dB 降到 0.06–5.28 dB**，候选抽头峰值全部 ≤1（round 2 的解析候选曾达 61.4）。
  - **合法高增益目标不被峰值 gate 误杀（v0.4.5 复审 P2 修复，round 4）**：`max|h| ≤ 1e3` 的 sanity gate 此前也作用到 legacy 兜底候选，导致合法高增益级联目标（48 kHz/N=4096、HP30 + 2×LowShelf(+24@200) + 2×HighShelf(+24@200) + master +24，+72 dB）三个候选全被拒绝后返回**严格单位冲激**：`peak = 1`、`Σh = 1`（HP 的 DC 零点丢失）、100 Hz/1 kHz 误差 −72 dB；而同目标 0.4.4 三参路径返回 `peak ≈ 3966.83`、通带误差 0.92 dB。修复：峰值 gate 只用于 factored 候选（os=1 / os=4）的 eligibility 与排序，legacy 候选在 gate 之前保留 finite + projected 副本作为最终兜底（即使 `max|h| > 1e3` 也不丢弃）；最终返回顺序为「合格 factored 候选（带内 fit 最优）→ legacy（finite/projected）→ 仅 legacy 非有限才回退冲激」。修复后该 cell `peak = 3966.83`、`|Σh| = 9.4e-13`、通带误差 0.926 dB（线性相位 0.008 / 3 参 legacy 0.922），系数全有限、无 Inf；既有 46+8+15+3 个矩阵 cell 的实测数值与 round 3 完全一致（该修复只影响此前被误杀的高增益兜底路径）。
  - 带内选优度量（v0.4.5 复审修复）：只在**已通过全带 eligibility 的候选**之间排序——在 192 个对数间隔点直接 DTFT，只统计实际可用带：目标幅度 ≥ 本次采样最大值的 1/2（−6 dB，排除 +1e-12 的 −240 dB 阻带地板）且频率 ≤ `min(500 Hz, 0.45·fs)`（本插件是 0.5–500 Hz 超低频 EQ）。它**不再承担全带正确性**：round 2 的 P1 根因正是只按该度量选优，导致解析候选在高频偏 28–97 dB 仍然胜出。不传 `sampleRateHz` 时拟合带退化为全带。
  - 精确零点投影（v0.4.5 复审修复）：候选选定后把系数投影到传输零点子空间——`δ[n] = p(x[n]) + (−1)^n q(x[n])`（`x[n]=n/(N−1)`，是卷积因子的正交补，等价于最小二乘投影），由「各阶矩为零」的小线性系统解出。单节点（2 阶）做整阶投影（`H(0)=H'(0)=0` 或 Nyquist 对称），解析候选本身已精确、近极限 96 kHz/4096/fc=30/Q=2 的 legacy 候选经投影后同样精确（回归 1e-9 断言始终成立）；多节点只做一阶投影，把 `H(0)/H(π)` 压到 1e-15 量级（round 3 据实测 1e-17…1e-15 把回归断言从 1e-3 收紧到 1e-9），更高阶矩保留 legacy 近似（避免 6 阶矩修正在 1.3 bin 近极限 cell 把通带误差从 0.8 dB 推到 6 dB）。factored 候选全不合格时回退 legacy（仍做投影，且不受峰值 gate 约束），legacy 也无效则回退冲激；解析路径 `latencyOut` 仍为 0。
  - 保持：无 JUCE 依赖、`double` 输出、后台设计线程零音频线程分配；系数非有限时回退冲激的旧语义不变。
- **FFTProcessor 传输零点统计**（`Source/SubEQ_FFTProcessor.cpp`）：最小相位设计前遍历设计引擎的已启用节点——HighPass 计 DC +2、LowPass 计 Nyquist +2、BandPass 计 DC +1 / Nyquist +1，其余类型不计；引擎旁路（`magnitude` 回调恒为 1.0）时零点计数强制归零，避免剥离不存在的因子。随后把 `dcZeroOrder` / `nyquistZeroOrder` 与采样率 `sr` 传入设计函数（后者只用于把候选选优的拟合带限制在 `min(500 Hz, 0.45·fs)`）；epoch / 发布 / 交叉淡化流程与零点统计语义未改动。

### 测试

- `Tests/subeq_fft_test.cpp` 扩展 **Test 11c：解析零点矩阵**：
  - 单节点 HP：fs ∈ {44100, 48000, 96000, 192000} × N ∈ {4096, 16384} × fc ∈ {30, 100, 400} × Q ∈ {0.707, 2.0} 高通目标，仅 `fc·N/fs < 1.0` 跳过（拐点不足 1 个设计 bin，物理不可分辨，2 个 cell）；46 个 cell 每个四断言：系数有限 / latency 0、`|Σh[n]| < 1e-9` 精确 DC 零点、带内 [fc, min(500, fs/2)] 100 个对数点**相对 baseline** `selectedErr <= max(linearPhaseErr, legacyErr, 1.0) + 0.5 dB`（legacyErr 用 3 参 `designMinimumPhaseFIR` 同目标实测，即不允许比 0.4.4 差）、**全带 [max(0.5, fs/N), 0.45·fs] 200 个对数点（target ≥ −80 dB）最大 |dB 误差| ≤ 6.0 dB**。全带下限取一个设计 bin：低于 1 bin 的目标形状物理不可分辨（与 `fc·N/fs < 1` 跳过同一判据；3×HP30 96 kHz/4096 连理想 IIR 截断在 6.5 Hz 都有 47 dB 偏差）。**没有新增任何 bespoke cell 跳过**，round 2 删除的 near-limit 跳过也不再需要——96 kHz/N=4096/fc=30/Q=2 由 legacy 候选 + 零点投影通过（带内 3.658 dB ≤ 相对上限 4.059 dB；全带 2.754 dB）。
  - 单节点 LP / BP（round 3 新增）：LowPass 200 Hz Q0.707（Nyquist 2 阶）、BandPass 100 Hz Q2（DC 1 阶 + Nyquist 1 阶）× fs ∈ {48000, 96000} × N ∈ {4096, 16384}（8 个 cell）。断言与 HP 单节点一致：精确 Nyquist（LP）或 DC+Nyquist（BP）零点 `1e-9`、带内相对 baseline、全带 6 dB。
  - 多节点（round 2 新增，round 3 收紧）：{2×HP(20+40 Hz, dc=4), 3×HP30 (dc=6), 2×LP200 (nyq=4), HP30+BP100 (dc=3, nyq=1)} × fs ∈ {48000, 96000} × N ∈ {4096, 16384}；仅最小拐点 <1 bin 的 2×HP 20 Hz @ 96 kHz/N=4096 跳过（15 个 cell）。每个 cell 断言系数有限 / latency 0、`max|h| < 1e3`、对应传输零点 `|H(0)|` / `|H(π)| < 1e-9`（round 3 从 1e-3 收紧；投影后实测 1e-17…1e-15）、带内相对 baseline `<= max(linear, legacy, 1) + 0.5 dB`、全带 6 dB。
  - 65536 点抽查：48 kHz HP30 Q0.707、192 kHz HP100 Q2、48 kHz LP200 Q0.707（LP 用 nyq=2 并断言 `|Σh[n]·(−1)^n| < 1e-9`）；三个场景同样带内相对 baseline + 全带 6 dB。
  - **Test 11d：合法高增益目标不误杀（round 4 新增，复审 P2 回归）**：48 kHz/N=4096、目标 = HP30 × LowShelf(+24@200)² × HighShelf(+24@200)² × master(+24)（合法 +72 dB 级联，`dc=2`）。6 条断言：系数有限且无 Inf / latency 0、**结果不是单位冲激**（`max|h| > 1`）、`h[0]` 不满足冲激判据（修复前实测严格冲激 `h=[1,0,…]`）、`|Σh| < 1e-6`（HP 的 DC 传输零点）、通带 [30, 500] Hz 100 点相对 `max(线性相位, 3 参 legacy) + 0.5 dB` 且绝对 ≤ 2 dB。修复前实测 `peak=1`、`Σh=1`、通带误差 **71.9999 dB**（线性 0.0078 / legacy 0.9225）；修复后 `peak=3966.83`、`|Σh|=9.4e-13`、通带误差 **0.926 dB**。
  - 实测（round 3 矩阵 + round 4 全量重跑）：单节点 HP 46 cell 全过，最差最小相位 **3.658 dB**（96 kHz/N=4096/fc=30/Q=2；同 cell 线性相位 3.031 dB、legacy 3.559 dB，相对上限 4.059 dB），最差全带 **5.278 dB**（192 kHz/N=4096/fc=100/Q=2）；单节点 LP/BP 8 cell 全过，最差带内 0.032 dB、最差全带 2.266 dB；多节点 15 cell 全过，最差带内 **0.923 dB**（3×HP30 96 kHz/N=4096），最差全带 2.403 dB，最大抽头 **9.972e-1**（新设计不再选中 61.4 峰值的解析候选）；多节点零点实测 **1e-17…1e-15**（按 1e-9 断言）；65536 抽查最大误差 < 1e-5 dB；高增益 Test 11d `peak=3966.83`、`|Σh|=9.4e-13`、通带 0.926 dB（既有矩阵数值与 round 3 完全一致，round 4 修复只影响此前被峰值 gate 误杀的兜底路径）。复审 P1 的 9 个失败 cell 全带最大误差：单节点 10.6 / 10.8 / 18.5 / 13.2 dB、多节点 15.7 / 54.1 / 19.0 / 109.2 / 53.6 dB → 修复后 **2.75 / 3.73 / 5.28 / 2.75 / 0.50 / 1.78 / 0.06 / 2.40 / 1.30 dB**。
- 回归：`cl /nologo /EHsc /std:c++17 /O2 /utf-8` 重新编译，`Tests/subeq_fft_test.exe` 实跑 **536 项全部 PASS**（原 219 + 新增 311（round 2/3）+ 新增 6（round 4 Test 11d）：单节点 HP 46×4 + 单节点 LP/BP（LP 4×4 + BP 4×5）+ 多节点（2×HP 3×5 + 3×HP 4×5 + 2×LP 4×5 + HP+BP 4×6）+ 抽查 3×4 + 高增益 6）、`ALL TESTS PASSED`、exit 0。
- 代价（后台设计线程实测，48 kHz HP30 Q0.707 带零点目标）：N=4096 约 **7.2 ms**、N=16384 约 **28.8 ms**、N=65536 约 **153.3 ms**（解析 os=1 + os=4 + legacy 8× 三个候选 + 全带 eligibility 3×128 点 DTFT + 带内拟合 + 零点投影；round 2 为 4.7 / 20.6 / 113.1 ms，增量来自全带 eligibility）；同长度良性平滑目标旧路径约 0.14 / 0.64 / 2.74 ms。设计仍在后台 `juce::Thread` 上，音频线程零分配 / 无锁路径不变。

### 工程

- 版本号 0.4.4 → 0.4.5：`Napkid Sub EQ.jucer` 的 `version` 字段与 `AGENTS.md` 头部的「当前版本」行一并同步，回归计数 219 → 536。
- Projucer CLI 无头重存 `Napkid Sub EQ.jucer`：`JuceLibraryCode/JuceHeader.h`（`versionString` 0.4.4 → 0.4.5、`versionNumber` 0x404 → 0x405）与 `JuceLibraryCode/JucePluginDefines.h`（`JucePlugin_Version` / `JucePlugin_VersionCode` / `JucePlugin_VersionString` / ARA archive ID 0.4.4 → 0.4.5）落账；`git diff -- JuceLibraryCode` 只有版本行。Projucer 同时把 `Builds/VisualStudio2022/*.vcxproj` 的预处理器版本宏与 moduleinfo helper 的 `-version` 重建为 0.4.5（构建产物目录不入库）。
- Release：VS2022 `Release x64` 全量 Rebuild **0 warning、0 error、exit 0**；`Napkid Sub EQ.dll` / Standalone exe FileVersion/ProductVersion 均为 0.4.5，VST3 `moduleinfo.json` **4 处 `Version` 均为 0.4.5**。

## [0.4.4] - 2026-10-18

> 修复 F1：Minimum Phase FIR 对低频高通目标在通带内产生假陷波（最深 −31 dB）。最小相位倒谱法在设计网格的深零点处把 `epsilon` 钳位（−240 dB 尖峰）当成宽带特征做时域混叠，改为按需 8× 过采样设计网格后截断回 `firLength`；良性目标仍走原 1× 精确路径。回归 207 → 219 项，Release x64 零警告。

### 修复

- **F1（`Source/SubEQ_FIRDesign.h` `designMinimumPhaseFIR`）：低频高通目标的带内假陷波**。
  - 根因：该函数在 N 点设计网格上采样 `log(|H(w)| + 1e-12)`。高通目标在 DC（低通目标在 Nyquist）的传输零点使采样幅度精确为 0，被 `epsilon = 1e-12` 钳位成 −240 dB 的孤立尖峰；倒谱法随后把这一带宽无限窄的尖峰当作宽带特征，其 N 点时域混叠落回通带，形成假陷波（v0.4.3 实测 HP30 Q0.9 N=4096 @48 kHz：@41.59 Hz 目标 +0.59 dB / FIR −19.48 dB；HP80 @100 Hz −19.92 dB；全带最深 −31 dB）。
  - 修复：先按原 N 点网格采样并记录网格上的最小幅度；若最小幅度 < `kMinPhaseDeepNullThreshold`（1e-6 = −120 dB，即存在深零点）则把设计网格改为 `firLength × kMinPhaseDesignOversampling`（8×）重新采样同一 log 幅度，倒谱因果化 / DFT / exp / IDFT 全部在 8× 网格上完成后截断取前 `firLength` 个实抽头；否则维持原 N 点路径（平滑目标保持网格精确性，现有 1e-5 断言不变）。
  - 保持不变：`epsilon = 1e-12` 与 NaN/Inf 传播语义（NaN 目标的 `if (mag < minMag)` 比较恒为假，不会命中过采样分支，仍回退冲激）、`latencyOut = 0`、无 JUCE 依赖、单缓冲内存（原 `cepstrum` 副本取消，同一条 `spectrum` 缓冲就地完成六步，8× 时仅扩容一次）。
  - 实测（48 kHz，Q 0.9 高通目标，N=4096，通带 [fc, 500] Hz 上 1000 点直接 DTFT）：通带最深误差 HP30 **−27.9 → −0.45 dB**（旧实现最深点 ≈123 Hz）、HP80 **−20.0 → −0.05 dB**（≈99.7 Hz）、HP120 **−15.7 → −0.02 dB**（≈123 Hz）；N=16384 / 65536 约 0.00 dB；低通目标（Nyquist 零点，同样走 8× 路径）0.00 dB；Bell 路径不变（仍 1×）。
  - 代价（后台设计线程，诚实记录）：8× 网格使 65536 点设计由约 4 ms 升至约 60 ms（48 kHz 本机实测 56~65 ms，4096 点约 2 ms），单缓冲峰值内存约 8 MB（524288 × complex\<double\>）。设计在后台 `juce::Thread` 上运行，音频线程零分配 / 无锁路径不变。

### 测试与工程

- 回归：`Tests/subeq_fft_test.cpp` 新增 Test 11b（编号沿用 Test 5b 的子编号风格）——HP30 / HP80 / HP120 Q 0.9 目标 N=4096，用直接 DTFT 在 [fc, 500] Hz 的 200 个对数点上核对通带：每个目标断言 `latency == 0`、系数有限、通带 max |dB 误差| < 0.6 dB、通带最深误差 > −1.0 dB（证明无深陷波）。Test 11 原有的平滑目标 1e-5 断言与 NaN 回退断言未改动且仍通过。
- 回归：`cl /nologo /EHsc /std:c++17 /O2 /utf-8` 重新编译，`Tests/subeq_fft_test.exe` 实跑 **219 项全部 PASS**（原 207 + 新增 12）、`ALL TESTS PASSED`、exit 0。
- Release：VS2022 `Release x64` 构建 **0 warning、0 error、exit 0**。
- 版本号 0.4.3 → 0.4.4：`Napkid Sub EQ.jucer` 的 `version` 字段与 `AGENTS.md` 头部的「当前版本」行一并同步，并补写本条目。
- 生成版本元数据同步：Projucer CLI 无头重存 `Napkid Sub EQ.jucer`，`JuceLibraryCode/JuceHeader.h`（`versionString` 0.4.3 → 0.4.4、`versionNumber` 0x403 → 0x404）与 `JuceLibraryCode/JucePluginDefines.h`（`JucePlugin_Version` / `JucePlugin_VersionCode` / `JucePlugin_VersionString` / ARA archive ID 0.4.3 → 0.4.4）落账；Release x64 重建后 VST3 `moduleinfo.json` 的 `Version` 报 0.4.4。

## [0.4.3] - 2026-10-18

> 相位可视化与文档收口轮：FIR 模式的相位曲线由占位隐藏改为真实渲染（扣除 FIR 群延迟后的剩余相位），新增英文 README 并回填第 5 轮审查台账、做仓库卫生清理；同时修正 README 中长期存在的两处 FIR 分辨率 / 延迟数值错误（历史 CHANGELOG 条目保持原样，不回改）。

### 变更

- **FIR 模式相位曲线（GUI）**：相位曲线现在在**三种处理模式都显示**，与幅频曲线同步。Zero Latency 绘制 IIR 相位；Minimum / Linear Phase 由已发布 FIR 的 `freqCoeffs` + `groupDelay` 计算**扣除 FIR 群延迟后的剩余相位（excess phase）**——Linear Phase 因此近似平坦的 0°；后台新设计发布后经版本号自动刷新曲线。旧「非零延迟模式隐藏相位」的 TODO 与占位逻辑一并删除。
- **新增英文 README**：`README.en.md` 与中文 `README.md` 一一对应；`README.md` 顶部加语言链接 `[English](README.en.md) | 简体中文`，`README.en.md` 顶部加 `[简体中文](README.md) | English`。
- **审查台账回填**：`docs/review/findings-round3.md` 补齐 R5-F5（回归项数记为 206 与实跑 207 不符，以实跑定稿）与 R5-F6（dt/α 折算的运行时 / 离线原型局部镜像，已由共享 `vqtTierEnvelopeAlphas()` 收口）两条处置记录。
- **仓库卫生**：`.gitignore` 追加本地工具目录 `.v2c/`、`.video_agent/`、`.zcode/` 与 `.zcodeignore`；删除仓库根目录陈旧的 `subeq_fft_test.obj`（构建产物不入库）。

### 文档修正

- README（中 / 英）的 65536 点 @48 kHz FIR 频率分辨率 **0.37 Hz → 0.73 Hz**（设计网格 fs/N = 48000 / 65536 ≈ 0.73 Hz）。
- README（中 / 英）的 Linear Phase 延迟 **1.37 s → 0.69 s**（(65536−1)/2 + 511 = 33278 samples @48 kHz ≈ 0.69 s）。
- `README.md` 中「接近 0.5 Hz 完整周期」的错误暗示按实际分辨率改为自洽表述（0.5 Hz 信号的一个完整周期约 2 s）。
- 上述均为既有文档的数值错误；历史 CHANGELOG 条目（0.3.0 等）保持原样，不回改。

### 测试与工程

- 回归：`Tests/subeq_fft_test.cpp` 以 `cl /nologo /EHsc /std:c++17 /O2 /utf-8` 重新编译，`Tests/subeq_fft_test.exe` 实跑 **207 项全部 PASS**、`ALL TESTS PASSED`、exit 0。
- Release：VS2022 `Release x64` 增量构建与全量 Rebuild 均 **0 warning、0 error、exit 0**（VST3 / Standalone / VST3ManifestHelper / SharedCode 全目标）。
- 版本号 0.4.2 → 0.4.3：`Napkid Sub EQ.jucer` 的 `version` 字段与 `AGENTS.md` 头部的「当前版本」行一并同步，并补写本条目。
- 生成版本元数据同步：Projucer CLI 无头重存 `Napkid Sub EQ.jucer`，`JuceLibraryCode/JuceHeader.h`（`versionString` 0.3.1 → 0.4.3、`versionNumber` 0x301 → 0x403）与 `JuceLibraryCode/JucePluginDefines.h`（`JucePlugin_Version` 0.3.4 → 0.4.3、`JucePlugin_VersionCode` 0x304 → 0x403、`JucePlugin_VersionString` 与 ARA archive ID 0.3.4 → 0.4.3）落账；修复「`.jucer` 已是 0.4.3 但生成物仍报 0.3.x」的漂移，Release x64 重建后 VST3 `moduleinfo.json` 的 `Version` 由 0.3.1 → 0.4.3。

## [0.4.2] - 2026-10-11

> 第 2 轮对抗性审查（`docs/review/findings-round2.md`）闭环修复轮：运行时频谱装配（显示管线 / 抽取级联 / 层环形缓冲）抽为共享原语（离线原型与交付代码零漂移）、窗规格 / 密度轴 / 显示范围 / 抽取级数上界收为单一事实来源、注释与文档语言规范统一；三个闭环复核轮（记录见 `docs/review/findings-round3.md`）发现的残留项（恒真 static_assert、显示地板闸门、计数与版本落账）一并处理。

### 修复

- **STD-01 注释语言（#1）**：`SubEQ_Spectrum.cpp` / `SubEQ_SpectrumMath.h` 的文件头与新写注释改回简体中文（与同批 `SubEQ_VQTDesign.h` / `SubEQ_WindowFunctions.h` 一致）；复核轮补完 `SubEQ_SpectrumMath.h` 文件头残留英文（R4-F3 / #25）。
- **STD-02 文档补注（#2）**：`AGENTS.md` 顶层速率补注采样率限定「48 kHz；44.1 kHz 因 2 的幂整除约束为 11025 Hz」，与 `Documentation/SubEQ_VQT_Feasibility.md` §4.1 一致。
- **STD-04 运行时装配共享化（#4）**：三条原语挂进共享头——显示管线 `vqtUpdateDisplayBands()`（瞬时功率 → 频域段间平滑 → 功率域一阶包络 → dB/钳位，`SubEQ_SpectrumMath.h:188`）、抽取级联 `vqtDecimationStep()`（0.05 Hz 直流阻断 + 逐级 ↓2 抗混叠，`SubEQ_VQTDesign.h:332`）、层环形缓冲装配 `vqtPushTierSample()`（`:366`）——运行时 `analyzeTier` / `process()` / `pushTierSample()` 与 Tests 离线原型调用同一实现，「被测代码即交付代码」覆盖到运行时装配（仅余 dt/α 系数一处局部镜像，记入 `docs/review/findings-round3.md` 待后续）。
- **STD-05 频谱设置打包（#5）**：新增 `SpectrumSettings`（五元组 + `operator==`）替代 `PluginProcessor` 的五个缓存整数（一次比较、一次解码）；段数（61/121）收敛为 `SubEQ_SpectrumMath.h` 的 `kBandsByDensity` / `bandDensityForBands()` 单一来源（核组合索引、`configure()` 归一、APVTS 解码共用，不再各写一份条件式）。
- **STD-06 重复映射收敛（#6）**：`SubEQ_WindowFunctions.h` 新增 `WindowSpec` 表（cosine-sum 系数 / 项数 / 旁瓣 / ENBW 单一来源，新增窗型只加一行）；`SubEQ_VQTDesign.h` 的窗分辨率倍率改为显式覆盖四窗型（编译器可提示遗漏）；`ModeSelector` 频谱六组下拉收敛为 `spectrumControlTable()`，构造 / 析构 / attach / 查找 / 排版五处遍历同表。
- **STD-07 死状态删除（#7）**：移除 `SpectrumAnalyzer` 的 `bandCenterFreqs` 与 `updateBandBounds()`——全仓无读者，且仍在音频线程 `configure()` 里做 61/121 次倍频程中心频率三角函数。
- **STD-08 具名常数（#8）**：`SubEQ_SpectrumMath.h` 的硬写 π 字面量改用 `kWindowTwoPi`（`SubEQ_WindowFunctions.h` 已有），匿名 ln10 字面量提为具名 `kLn10`。
- **STD-10 重复头文件（#10）**：Tests 删除重复的 `#include <random>`。
- **STD-11 抽取级数守卫一致化（#11）**：顶层降采样指数上限提为具名 `VQTMaxTopDecimationExponent`（16，`SubEQ_VQTDesign.h:57`），`MaxStages`（`SubEQ_Spectrum.h:112`）与 `numStages`（`SubEQ_Spectrum.cpp:63`）计算式同源（运行时与离线原型同表达式）；`prepare()` 对该函数实际返回值 `jassert` + `min` 钳位兜底（`:64-65`）——守卫与数组界不一致时钝化为截断，不再越界写 `stages[]`；原头文件中两侧同一表达式的恒真 `static_assert` 经复核移除（R3-F4 / #21），把守改放 `prepare()`。
- **R3-F2 单一来源自述补全（#23）**：`SubEQ_SpectrumMath.h` 文件头逐条声明密度轴（`kBandsByDensity`/`bandDensityForBands`）与显示范围/地板（`kDisplayRangeDb`/`kDisplayFloorDb`/`kDisplayFloorPower`）两组单一来源；`AGENTS.md` 的共享纯计算头文件枚举同步补全。
- **R3-F5 显示范围派生（#22）**：`kDisplayRangeDb` / `kDisplayFloorDb` / `kDisplayPowerGuard` / `kDisplayFloorPower` 进 `SubEQ_SpectrumMath.h`（`SubEQ_Spectrum.h:46` 的 `RangeDb` 以 `static_assert` 精确镜像）；`SpectrumAnalyzer` 复位初值与暂存、GUI 频谱缓冲初值全部从该来源派生——−60 dB 地板不再多处独立表达。
- **R4-F4 地板闸门改往返相等（#26）**：具名 `kDisplayFloorSteps`（`SubEQ_SpectrumMath.h:151`），断言改为 `-kDisplayFloorDb == 10.0 * kDisplayFloorSteps`（截断型分数地板不再蒙混），功率展开与断言共用同一常量（`:156`）；Test 25 增加一条运行时断言核对地板 dB 与地板功率互为精确换算。
- **R4-F5 / R4-F6 版本落账（#27 / #28）**：CHANGELOG 工程节的计数自述与 `AGENTS.md` 版本头分别对齐实测数据与 `.jucer` 的 0.4.2。
- **STD-03 显示管线迁移 double（#3，维护者裁定）**：`bandPowers` / `envPowers` / `bandDbScratch` 由 `float` 改 `double`（`SubEQ_Spectrum.h`），dt/α 折算经共享 `vqtTierEnvelopeAlphas()`（`SubEQ_SpectrumMath.h`，全新共享原语）——运行时与离线原型数值精度完全一致，「被测代码即交付代码」对显示路径字面成立；仅原子发布边界收窄 float（`bandData.store`）。顺带收口 `findings-round3.md` 登记的 dt/α 局部镜像残留（`Source/SubEQ_Spectrum.cpp` 与 `Tests` 各写一份 → 同调一份）。
- **SPC-06 结案（#17，维护者裁定：接受现状）**：复位值已对齐 `kDisplayFloorPower`（选项 a 已实施）；残余「未分析邻段」≤~170 ms 自愈暂态与既定跨层陈旧度取舍同模型，不追加排除逻辑。台账处置表已结案。

### 测试

- 回归 **201 → 207 项，全部通过**（本轮门禁：回归 exit 0；Release 增量构建与全量 Rebuild 均零警告，exit 0）：新增 6 项无条件断言——SPC-03（#14）：1/3 与 2/3 平滑档峰预算各 ±0.75 dB 对核理论（−6.99 / −9.54 dB = 1/5 与 1/9）；SPC-01（#12）：低层段（≈3.00 Hz，属主层 tier 2）端到端关断三条——−10/−40/−50 三个 dB 阈值穿越的守卫断言（哨兵未替换即报错，杜绝空过）+ 总下落（−10 → −50 dB）≤ 窗长 + 2·dt + 尾段（−40 → −50 dB）≤ 10/180 s（≈55.6 ms）+ 3·dt，界由 `set.tiers[]` 与 hop/fs 推导；R4-F4（#26）：地板 dB 与功率初值互为精确换算的运行时核对。
- 离线原型全部改调共享原语（显示管线 / 抽取级联 / 环形装配）；泄漏断言读的 `rawDb` 未钳位真值在共享调用之外单独计算，逐值语义不变。复位初值与钳位下限改用 `kDisplayFloorDb` / `kDisplayFloorPower`（与运行时同源）。

### 工程

- 版本号 0.4.1 → 0.4.2：`Napkid Sub EQ.jucer` 的 `version` 字段与 `AGENTS.md` 头部的「当前版本」行一并同步，并补写本条目（此前本轮改动无任何变更记录）。
- 台账回填：`docs/review/findings-round2.md` 的「处理记录」改为逐条处置表（含 issue 链接），`docs/review/findings-round3.md` 记录第 5 轮起的复核与残留项。
- `AGENTS.md` 同步：版本头 0.4.1 → 0.4.2、顶层速率采样率限定（#2）、回归项数 207、共享纯计算头文件枚举补全（#23）、Agent skills 段落（issue tracker / triage 标签 / 领域文档索引，见 `docs/agents/`）。
- 单一事实来源收敛后音频线程约束不变：零分配、无锁（固定数组 + `prepare()` 预分配缓冲 + 指针切换）。

## [0.4.1] - 2026-10-11

> 频谱显示平滑化：频域段间平滑（功率域、用户可选倍频程宽度）+ 时间包络重构（dB 域 → 功率域 + 按层时间归一化）。设计论证见 `Documentation/SubEQ_VQT_Feasibility.md` 附录 A。

### 变更

- **新增 `spectrum_smoothing` 参数**（Off / 1/6 / 1/3 / 2/3 oct，默认 1/6）：频域段间平滑，**功率域** Hann 权重核（w(j)=cos²(πj/(2(r+1)))，边缘截断重归一化），61/121 段两密度交付近似相同的倍频程平滑宽度（半径 r = 档位宽度 × 每倍频程段数：61 段 {0,1,2,4}、121 段 {0,2,4,8}）。纯数学进 `SubEQ_SpectrumMath.h`（`smoothingRadiusBands`/`smoothingKernelWeight`/`smoothBandPowerAt`），插件与测试共用。总参数 49 → 50。
  - 峰展宽预算（诚实代价，实测与核理论精确吻合）：孤立高频纯音峰 1/6 档 −4.77 dB（= 10·log10(1/3)，核权重和为 3）、1/3 档 −6.99 dB、2/3 档 −9.54 dB；低频段因 VQT 核本就跨段重叠，损失显著更小。Off 档恒等保持逐段原始读数。
  - 白噪声实测：顶层段间读数起伏 2.67 dB → 1.92 dB（1/6 档）。
- **时间包络重构为功率域 + 时间归一化**（修复 v0.4.0 遗留）：
  - 旧包络作用在 dB 域且在 −60 dB 钳位**之后**——dB 域平均对波动功率有系统性低估（Jensen 不等式：功率 {1,100} 平均，功率域 17.0 dB vs dB 域 10 dB），且地板以下功率差异全部塌缩。新管线：瞬时功率 → 频域平滑 → 功率域一阶包络 → **最后一步**才转 dB/钳位。
  - 旧包络系数按"每次分析迭代"固定步进，而 VQT 各层分析节奏不同（48k/hop512：顶层 ~93.7 Hz、最底层 ~5.9 Hz）——底层物理释放速率比顶层慢 ~16×（≈20 vs ≈325 dB/s），底部频谱残留拖尾。新系数按各层真实分析间隔折算（α = 1−e^(−dt/τ)），全频段统一物理速率：attack τ = 20 ms、release 恒定 **180 dB/s** 下落（功率域指数衰减的 dB 轨迹为匀速，频谱仪惯例；1−α = e^(−dt/τ) 精确成立，无离散误差）。
  - 属主更新原则：某层分析只重算并输出本层属主段（平滑窗口读邻段最近一次分析功率，陈旧度 ≤ 一个邻层步距），dt 恒为该层真实间隔，杜绝跨层双计。
- GUI：底部面板频谱组增至六列（+Smoothing），窗口 950 → 1040 px；频谱组列布局改为列间间距（无尾部浪费）。

### 测试

- 回归 **174 → 201 项，全部通过**：新增 Test 25（平滑数学：半径查表/恒等/边缘重归一化/功率域 vs dB 域平均偏差/冲激峰展宽单调性与核理论精确值/弹道系数/顶层与底层同速率下落的单元级锁）、Test 26（端到端：各档峰预算与单调性、噪声驯服、关断下落时间 213 ms ≈ 40 dB @180 dB/s）；Test 18 增平滑解码；Test 24 校准改为对馈入尾段做功率域时间平均（gamma 段读数与摆动相位解耦——新包络对底层帧摆动近乎透明，馈末单点读数依赖相位）。离线原型 `OfflineVqtAnalyzer` 镜像新管线（与运行时同数学）。

### 工程

- `SubEQ_SpectrumAnalyzer::configure()` 签名 4 → 5 参（+smoothingIndex）；`smoothedBands`（dB 域状态）与 `AttackCoeff/ReleaseCoeff` 移除，代之以 `bandPowers`/`envPowers`（功率域）与 `smoothRadius`；全部固定数组，音频线程仍零分配/无锁。
- `SubEQ_SpectrumMath.h` 扩展为显示平滑数学单一事实来源（核权重/段平滑/半径表/弹道系数/物理常数），运行时与离线原型共用，无 JUCE 依赖。

## [0.4.0] - 2026-10-11

> 频谱可视化引擎重构：FFT+倍频程分桶 → **多采样率 VQT（Variable-Q Transform）滤波器组** + 用户可选窗函数族。完整调研与实测数据见 `Documentation/SubEQ_VQT_Feasibility.md`。

### 变更

- **频谱分析内核替换为多采样率 VQT**（`SubEQ_VQTDesign.h`，无 JUCE 纯数学，插件与测试共用）：0.5 Hz 起 10 个倍频程层，顶层降到 ~3 kHz、向下逐层 ÷2（级联 ↓2 4 阶 Butterworth 抗混叠）；每层帧加窗 → FFT → 每频段频域匹配核点积（分数 bin 匹配估计，满幅正弦落段中心 = 0 dB，幅值约定与旧引擎一致）。
  - 修复旧引擎的固有缺陷：8192 点 FFT @48k（bin 宽 5.86 Hz）下 61 段模式前 22 段、121 段模式前 44 段直接复制 bin 1 功率——0.5~5.9 Hz 的频谱实际无意义；现在全部频段独立估计。
  - 帧长 = 分辨率需求与 **2 s 封顶**（pow2 量化，严格不超）之小者：约 4~16 Hz 以上满倍频程分辨（随窗型/档位），底部层 gamma 化（等效带宽加宽换响应速度，用户决策）。
  - 0.05 Hz 一阶直流阻断（底部核主瓣覆盖直流的必要防护）；镜像支撑下限保护（gamma 区段读数的相位摆动抑制）。
  - 24 个核组合（窗 × 密度 × 分辨率）在 `prepare()` 全量预生成，`configure()` 只切指针——零音频线程分配（F-011 模式推广）；F-010 惰性 twiddle 预热按层保留。
- **窗函数族系统研究并参数化**（`SubEQ_WindowFunctions.h`）：新增 `spectrum_window` 参数（Hann / Blackman / B-Harris / Nuttall，默认 B-Harris −92 dB 旁瓣）。相对 −60 dB 显示动态范围，旧固定 Hann（−31.5 dB）的强单音泄漏问题由低旁瓣窗解决；主瓣加宽的代价经分辨率倍率诚实计入帧长设计。总参数 48 → 49。
- **`spectrum_fft_size` 参数语义迁移**为分辨率档（Fast/Balanced/Fine = 核长尺度 0.7/1.0/1.5）：参数 ID 保留，老 session 按下标兼容映射（小 = 快/粗，语义连续）。
- GUI：底部面板频谱组增至五列（分辨率/窗函数/密度/刷新/hop），窗口 900 → 950 px。

### 测试

- 回归 **102 → 174 项，全部通过**：Test 13/18 更新（16/N² 校准与 Hann 专用断言退役，改为窗函数族均值/ENBW/旁瓣与分辨率/窗解码）；新增 Test 22（窗函数族不变量）、Test 23（VQT 层结构/帧长封顶/抽取阶梯）、Test 24（离线 VQT 原型引擎：四窗型校准 ≤0.75 dB（gamma 区外）、BH 泄漏 < −200 dB、直流抑制、双音分辨、性能基准 **4.7×** 于旧 8192-FFT 路径）。离线原型与运行时内核共用 `SubEQ_VQTDesign.h` 全部数学——被测代码即交付代码。

### 工程

- 新增共享纯计算头文件 `SubEQ_WindowFunctions.h`（窗函数族系数/均值/ENBW/旁瓣）与 `SubEQ_VQTDesign.h`（层结构/帧长/匹配核/抽取系数/直流阻断）；`SubEQ_SpectrumMath.h` 精简为 octave 频段单一职责（Hann 与 16/N² 校准随 FFT 引擎退役）。
- 44.1 kHz 下 pow2 抽取整除约束使顶层过采样比偏高（成本回到旧方案量级）；有理重采样记录为后续项（见调研报告 §8）。

## [0.3.4] - 2026-08-14

> 第 1 轮全量代码审查（docs/review/findings.md）修复轮：P1 × 1、P2 × 23 全部处理完毕。

### 修复

- **F-001（P1）epoch 校验与发布之间的 TOCTOU 竞态**：epoch 复检移入发布临界区内，与 `prepare()` 的"先 bump epoch、同锁清空 currentState"构成同锁序列化——采样率切换时按旧采样率设计的系数不可能再写回。
- **F-002 大块释放移出临界区**：后台线程排空退役环改为"锁内移出、锁外析构"；发布处旧 `currentState` 同样移出临界区析构；`prepare`/`clearPublishedState` 只做指针搬移——音频线程过渡期锁点不再可能被 ~2.3 MB 释放放大。
- **F-003 块尺寸增大不再静默关闭交叉淡化**：`prepare()` 对增大的 `maxBlockSize` 提前重分配 `xfadeOld/xfadeNew`（不动设计/epoch），`process()` 的淡化分支不再因缓冲不足跳过淡化（输出阶跃回归）。
- **F-004 淡化期间再次发布不产生新侧跳变**：同延迟的新设计先挂起（`pendingState`），当前淡化收尾后再开启新淡化；延迟不同的设计立即收尾淡化并冲刷切换。
- **F-005 Release 下 stopThread 失败兜底**：`stopThread(5000)` 失败后继续阻塞等待线程退出（`while (isThreadRunning()) wait(50)`），消除析构返回后 `run()` 访问 `this` 的 UAF 窗口。
- **F-006 PDC 与输出一致性**：仅 `groupDelay` 相同（两侧管线延迟一致）时交叉淡化；延迟不同的设计（FIR 长度/模式变化）冲刷累加器立即切换——PDC 翻转当块起补偿即与实际输出一致。**权衡说明**：延迟变化的切换恢复为瞬时切换（旧延迟尾音按新延迟补偿会变成可闻前回声），可能伴随轻微切换阶跃，同延迟的参数编辑仍保持无阶跃淡化。
- **F-007 Tilt→单 biquad 淡出窗口内再次 update 丢贡献**：级联淡化计划抽为纯函数 `computeBiquadFadePlan()`（`SubEQ_BiquadDesign.h`，插件与 Tests 共用）；淡出窗口内再次 `update()` 时第二 biquad 从当前半淡出系数继续向 identity 淡出。
- **F-008 主 bypass 无淡化**：bypass 改为 ~15 ms 湿/干交叉淡化（块粒度推进），旁路期间节点链保持处理（热旁路）——解除旁路时无陈旧状态瞬态，切换无咔哒。
- **F-009 prepareToPlay 线程契约**：`updateEQParameters()` 注释改为"audio thread 或宿主已停止处理的 prepare 上下文"；`getEQEngine()` 注明非线程安全且全仓无调用方。
- **F-010 twiddle 预热线程亲和兜底**：`FFTProcessor::process` 与 `SpectrumAnalyzer::process` 对实际尺寸做惰性预热（幂等、命中即 O(log n) 查找），宿主在非音频线程调用 `prepareToPlay` 时音频线程首次 FFT 也不会分配。
- **F-011 Hann 窗重生成尖峰**：三档窗表（4096/8192/16384）在构造时（消息线程）一次性生成，`configure()` 仅切换指针——音频线程不再执行 O(N) 三角函数。
- **F-012 0dB 失速吸附死分支**：`mouseDrag` 先用上一事件 RAW 值做跨零符号比较、再记录本事件 RAW 值——"跨零失速"第二分支恢复生效。
- **F-013 滚轮步进注释失实**：按 JUCE 实测 `deltaY ≈ ±0.234/格` 更正注释（主增益 ≈±2.3 dB/格、Q ≈+5.5%/格），乘数保持不变。
- **F-014 频谱纵轴与网格对齐**：频谱改与 EQ 网格同轴（`gainToY`，±24 dB 钳位）——0 dB 谱峰不再显示在 +24 dB 网格线上；低于 -24 dB 的谱值钉在底边。
- **F-015 自动化手势缺口**：`createNodeAt` 的 Q/Type 默认值写入与类型菜单切换（含增益重置）补 `begin/endChangeGesture`。
- **F-016~F-017 死代码删除**：`FrequencyResponse` 的 `dragStartNodeScreen`/`dragStartedOnNode`/`getGainArea`/`getPhaseArea`/`yToPhase`；`DesignLookAndFeel` 永不可达的 `drawRotarySlider`/`drawLinearSlider` 重载与 `juce::Slider` 颜色设置（全仓无 Slider 实例）。
- **F-018 设计 token 收敛**：删除全部未引用 token 与未引用颜色（阴影/字号/旋钮/开关/深色面板等）；弹簧常量（节点低通、推子填充边）、字号、底部面板布局数值收敛到 `DesignConstants` 单一事实来源；`drawDropShadow` 注明 cornerRadius 被精灵方案忽略的原因。
- **F-019 玻璃绘制防御**：`LiquidGlassEffect::drawRoundedRect` 入口对空/零尺寸、非正圆角与零缩放直接返回。
- **F-020 静态缓存同步**：`DesignFonts` 改用函数局部 magic static（线程安全一次初始化）；tinted sprite 缓存注释明确"仅限 GUI 线程"。
- **F-021 构建警告清零**：`drawHorizontalLine`/`drawImage` 显式转换（`static_cast<float>`/`roundToInt`），必需未用参数 `juce::ignoreUnused`——Release 构建零警告。
- **F-022 头文件自包含**：`SubEQ_DSPMath.h` 补 `<utility>`、`AnimationUtils.h` 补 `<functional>`、Tests 补 `<string>`。
- **F-023 biquad 设计防御**：`computeBiquadCoefficients` 补 default 分支写单位系数，非法枚举值不再返回陈旧系数。

### 测试

- **F-024 回归补强**：F-007 级联淡化计划抽纯函数后新增 5 项回归（Test 21），F-023 新增 1 项回归——**96 → 102 项，全部通过**。Test 5b 注释更正（`h[0]=1` 是未经 FFT 的 DC-only 滤波器，非恒等滤波器；恒等路径由 Test 7 覆盖）。
- **测试边界说明**：v0.3.3 三项头条修复（`EQNode::prepare` 采样率重算、PDC 延迟上报、同会话重复 prepare 不重设计）位于 JUCE 依赖文件中，独立测试不可覆盖；本轮 F-001/F-003/F-006 属发布-订阅时序与延迟权衡，同样依赖 JUCE 线程环境，验证方式为代码审查 + Release 构建 + 人工试听。

### 工程

- 版本号 0.3.3 → 0.3.4（`.jucer` version、`JucePluginDefines.h` 三项版本宏、`JucePlugin_ARADocumentArchiveID` 一并同步）。
- `docs/review/findings.md` 全部条目状态更新为"已修复"，并附处理记录表。
- `AGENTS.md` 同步：版本号、测试项数（96 → 102）、DesignSystem 描述（无 Slider 重载、token 语义）。

## [0.3.3] - 2026-08-14

> 代码审查修复轮：采样率切换后 IIR 系数错位、三处音频线程 GUI/PDC 越界调用、同会话重复 prepare 的冗余 FIR 重设计，以及若干语义/文档修正。

### 修复

- **采样率变化后 IIR 系数不重算（EQ 频点整体错位）**：`EQNode::prepare` 现在在采样率变化时按新采样率重算系数——此前只更新成员采样率，音频路径 `updateEQParameters()` 依赖参数缓存比对，参数未变时不会重算，导致 48k→44.1k 等场景下 EQ 频点整体偏移（如 100 Hz Bell 实际落在 ≈91.9 Hz），且与 GUI 曲线（独立引擎自行侦测采样率）不一致。语义与 FIR 路径（`FFTProcessor::prepare` 触发重设计）对齐。
- **`prepareToPlay` 不再在音频线程调用 `setLatencySamples()`**：与项目既有的 deferred-PDC 规则一致（processBlock 置脏标记、10 Hz Timer 在消息线程上报）；同会话重入 prepare 时已发布设计与延迟上报均保留。
- **同会话重复 prepare 不再触发冗余 FIR 重设计**：`prepareToPlay` 不再强制 `eqModeCache`/`currentMode` 归零——此前每次 transport stop/start 都会误报“模式变化”，触发一次相同系数的重设计并在发布时冲刷卷积尾，播放中出现输出跳变。
- **进入 FIR 模式时丢弃过期设计**：新增 `FFTProcessor::clearPublishedState()`——FIR→ZeroLatency→改参数→切回 FIR 时不再短暂沿用旧曲线，改为静音直至新设计发布（与 processBlock“未发布前输出静音”契约一致）。
- **APVTS 监听回调不再在音频线程直接 `repaint()`**：宿主自动化时 APVTS 监听器在音频线程同步回调，`FrequencyResponse`/`MasterGainSlider` 的 `repaint()` 改为 `MessageManager::callAsync` + `SafePointer`；`ModeSelector` 的 6 个 `ComboBoxAttachment` 改为手工监听 + callAsync 同步（附件同样会在音频线程直接改 ComboBox）。
- **FIRState 释放全部移到后台线程**：退役槽改为 64 槽环形队列（音频线程移交、后台线程排空），消除“连续两次发布之间音频线程拾取旧状态时在音频线程析构 ~2.3 MB 状态”的窗口。
- **BandPass 不再施加 gain 缩放**：与“BandPass 非增益敏感”的分类及 GUI（手柄钉 0 dB、拖拽改 Q、切入时重置增益）一致，消除 preset/自动化写入 gain 时的“隐形增益”。
- **最小相位 FIR 倒谱因果化保留 Nyquist 折叠项**（c[N/2] 由置零改为保留）：与教科书同态重构一致（实测影响 ≤1e-5 dB 量级，属正确性对齐）。
- **悬停不再维持 60 Hz 空转重绘**：`needsPhysicsTimer()` 移除 `hoveredNode` 子句（悬停变化由 mouseMove/mouseExit 即时重绘，缩放动画由 `isAnimating()` 覆盖）。

### 优化（Informational 项跟进）

- **FIR 交叉淡化**：新设计发布不再冲刷卷积尾——旧设计尾音与新设计在 1024 样本（≈21 ms @48 kHz）内线性交叉淡化，参数编辑/模式切换不再产生输出阶跃；淡化结束的旧设计经退役环在后台线程析构。FIR 长度变化（convFFTSize 改变）同样走淡化（旧累加器超界区域先清零）。
- **频谱分析门控**：编辑器存在时才运行频谱分析（`spectrumEditorCount` 原子计数由编辑器构造/析构维护，多编辑器并存安全），无 GUI 时不再消耗音频线程 CPU。
- **尾长上报**：`getTailLengthSamples()` 未发布设计时按最大 FIR 长度（65536 + 512）保守回退，不再低估 16384/65536 选择下的卷积尾。
- **FIR 抽头改 double**：`designLinearPhaseFIR`/`designMinimumPhaseFIR` 返回 `std::vector<double>`，与“所有 DSP 用 double”约定对齐（消除一次 float 量化）。
- **文档**：架构文档 Tilt 公式更正为 gain/2 + −gain/2，补记 Type-II 偶数 FIR 的 Nyquist 零点/半样本群延迟截断与交叉淡化语义；README“最低采样率”标注为软性规格。

### 工程

- `.jucer` Debug/Release 配置加 `/utf-8`（消除 C4819 编码警告，与 `run_tests.bat` 一致）；版本号 0.3.2 → 0.3.3。
- `JuceLibraryCode/JucePluginDefines.h` 版本同步 0.3.3。
- `getSpectrum` 快照注释改为准确描述（逐元素原子、极端情况可撕裂一帧）。
- 测试文件头注释补齐全部 11 个被测模块、编号连续化；新增 BandPass 忽略增益（2 项）、Nyquist 强调目标（1 项）、NaN/Inf 毒化遏制（2 项）、最小相位 NaN 回退冲激（1 项）、forceStable 剩余分支（2 项）回归，88 → 96 项。
- `Source/Fonts/Avenir.ttf` 内嵌字体随 GPL v3 项目分发：使用授权已获确认（2026-08-14）。

## [0.3.2] - 2026-08-12

> 架构优化第一轮：把纯 DSP/几何逻辑从 JUCE 线程与 GUI 组件中抽出为无 JUCE 依赖的 seam，消除多处重复的单一事实来源，删除死代码；回归测试 27 → 88 项。

### 重构

- **纯 DSP 提取**：双精度 biquad 系数/状态/频响抽到 `SubEQ_Biquad.h`、RBJ 系数设计抽到 `SubEQ_BiquadDesign.h`、FIR 设计算法抽到 `SubEQ_FIRDesign.h`、overlap-add 卷积核心抽到 `SubEQ_FFTConvolver.h`（测试直击真实实现，删除了手抄镜像 `olaProcess`）；`FFTProcessor` 不再私有设计函数，音频线程访问器改为 `getPublishedStateForAudioThread`（显式线程亲和），发布锁内析构 ~2.3 MB state 移出临界区。
- **参数/频谱单一来源**：`applyNodeValuesToEngine()` 统一「APVTS → EQNode」翻译（消除音频线程与设计/GUI 路径的重复）；`nodeParams` 下标改由 `ParamID` 派生；频谱 choice 下标→语义值映射收敛到 `SubEQ_SpectrumConfig.h` 单表；频段中心频率/Hann/校准公式收敛到 `SubEQ_SpectrumMath.h`；`FilterType` 枚举与 int↔枚举映射抽到 `SubEQ_FilterType.h`（无 JUCE）。
- **GUI 纯逻辑提取**：坐标映射抽到 `SubEQ_CoordinateMapper.h`（freq/gain/phase 正反映射单一来源）；增益敏感类型判断与类型切换重置规则、对数 Q 步进抽到 `SubEQ_NodeInteraction.h`；节点/推子二阶弹簧积分器抽到 `SubEQ_Spring.h`。
- **死代码删除**：`computeMaxGroupDelay` 估算器及其测试（生产侧已用解析式延迟）、`SubEQLookAndFeel.h` 遗留颜色函数、`DesignColours.h` 永不可达的 Dark 主题分支与 `setTheme/getTheme/currentTheme`。

### 测试

- 新增 FIR 设计（线性相位对称/幅频匹配、最小相位有限系数/零延迟/幅频匹配）、频谱数学（octave 频段/Hann/校准）、坐标映射（往返/钳位）、节点交互（8 类型敏感分类/类型切换重置/对数 Q 步进）回归。

## [0.3.1] - 2026-08-11

> 后端（FIR 处理管线）全量审查-修复：prepare/epoch 生命周期、设计发布后的管线冲刷、会话切换后的自动重建。

### 修复

- **`prepare()` 会话未变时误丢弃在途设计**：transport stop/start、loop point 等触发 `prepareToPlay` 重复调用（采样率/声道未变）时，设计 epoch 不再 bump——在途 FIR 设计与新会话参数一致，允许其正常发布；此前会被误判为过期丢弃且无重新请求，导致用户最近的参数编辑被静默回退（FIR 模式继续沿用旧系数，甚至无设计输出静音）。
- **会话切换（采样率/声道变化）后 FIR 模式静音**：`currentState` 被清空后立即自动 `requestRedesign()`（Zero Latency 模式由后台线程无害忽略），不再需要用户再次改动参数才会重建设计。
- **设计发布后旧滤波器回声污染新输出**：任何新设计发布（参数编辑、FIR 长度或模式切换）都会冲刷 overlap 累加器与待播放输出 FIFO——此前仅在 FIR 长度变化时冲刷，同长度系数变化（最常见操作）会让旧滤波器尾部（最长 65536 点 ≈ 1.4 s @48 kHz）继续叠加进新输出。

### 工程

- `AGENTS.md` 回归测试项数更正 26 → 27。
- `.reasonix/run_tests.bat` 编译加 `/utf-8`，消除 MSVC C4819 代码页警告。

## [0.3.0] - 2026-08-03

> 合并原 0.3.0 / 0.4.0 / 0.5.0 全部变更（2026-08-02 ~ 08-03 的实时安全重构、overlap-add 卷积、液态玻璃 GUI 迁移与设计迭代）。2026-08-06 前后端联合审计修复并入本条目；版本号统一为 0.3.0，0.4.0 / 0.5.0 不再单独存在。

### 前端

#### 新增

- **液态玻璃设计系统迁移**：暖调奶白哑光 + `#FF007B` 高光 + 弹性物理动效设计语言全量落地，新增 `Source/SubEQ_Editor/DesignSystem/`：
  - `DesignColours.h` — 暖调奶白哑光色板（象牙白背景 `#F5F0E8`、表面 `#EDE8DF`、文字 `#3D3832`）+ 高光色 `#FF007B` + 莫兰迪点缀色 + 液态玻璃色（冷/暖折射、边缘高光、光晕）+ Light/Dark 双主题 token。
  - `DesignConstants.h` — 圆角/间距/阴影/动画时长（fast 120ms / normal 250ms / slow 400ms / elastic 600ms）/物理参数（拖拽阻尼 0.85、弹簧刚度 180）token。
  - `DesignFonts.h/.cpp` — Avenir 几何无衬线字体内嵌（`Source/Fonts/`，经 jucer 资源编入 BinaryData），四档字号。
  - `AnimationUtils.h/.cpp` — 缓动函数（easeOutCubic/Back/Elastic）+ `AnimatedValue`（Timer 驱动属性动画器）。
  - `LiquidGlassEffect.h/.cpp` — 液态玻璃六层绘制（底色 → 径向冷/暖折射 → 椭圆高光 → 边缘高光 → 底部阴影 → 外圈光晕），高光位置随拖拽向量偏移。
  - `DesignLookAndFeel.h/.cpp` — 继承 `LookAndFeel_V4` 全套重载（按钮/开关/旋钮/推子/ComboBox/标签/弹出菜单/滚动条/标题栏）+ 静态助手（`drawMatteSurface`/`drawGlassButton`/`drawDropShadow`/`drawInnerShadow`）。
- **FrequencyResponse 视觉迁移**：暗色 Pro-Q2 风格 → 暖白液态玻璃风格——暖象牙白背景 + 表面圆角卡片 + 弥散投影；绘图区莫兰迪灰网格（0 dB 线强调）+ 右侧相位刻度（仅 Zero Latency 模式）；响应曲线 accent `#FF007B` 2.5px 柔化描边 + 曲线下 glow 填充；**每节点真实响应曲线**（复用响应引擎 `EQNode::getResponse`）；频谱线莫兰迪灰蓝、相位曲线莫兰迪蓝。
- **FrequencyResponse 交互动效迁移**：节点 hover 放大 2 倍（`easeOutBack` 弹性）；拖拽节点液态玻璃质感（外径 ×2、高光随拖拽向量偏移）+ accent 光环 + 白实心圆低通弹簧滤波滞后（ωₙ=20Hz、ζ=0.5）释放回弹；选中节点参数标签改为玻璃 tooltip（F/G/Q/T 仍可点击编辑）；`mouseMove`/`mouseExit` hover 跟踪 + 60Hz timer 统一驱动。
- **增益推子重新设计**（不参考 Fader 的定制方案）：
  - thumb 中心行程与左侧绘图区 ±24 dB 边界精确对齐（`[20, 高度-20]` 同构映射），0 dB 指示线与绘图区 0 dB 网格线同高。
  - thumb 形态：静止实心白色圆；悬停/点按/拖拽时白色空心圆环 + 1.5x 放大（`easeOutBack`/`easeOutElastic`），便于观察填充条。
  - 轨道联动：光标与推子任意交互（轨道悬停 / thumb 悬停 / 点按 / 拖拽全程）时轨道 + 填充条横向 2x 加粗（`easeOutCubic` 250ms），仅移出组件收回。
  - 填充条上沿低通弹簧惯性（ωₙ=5 Hz、ζ=0.5）：起点捕获旧位置、拖拽/跳转/复位中跟踪、释放回弹、过冲 clamp。
  - 0 dB 停滞式吸附（±0.4 dB 内吸附、跨 0 且距带 <0.8 dB 卡滞、快速大跳自由通过）；右键单击复位 0 dB（双击/滚轮复用）；点击轨道跳转 + 跟手拖；数值标签跟随 thumb；0 dB 刻度条灰色弱化。
- **全局默认 LookAndFeel**：Standalone 下替换默认 LookAndFeel（audio settings / Options 系统菜单生效），析构回退内置默认（无多实例悬垂）；节点类型菜单显式 `PopupMenu::setLookAndFeel` 携带设计 LAF（VST3 同样生效）。
- **输入/输出实时频谱可视化**：绘图区叠加输入（暖灰棕）与输出（灰蓝）双频谱曲线（0.5–500 Hz，1/6 或 1/12 倍频程），绘图区右下角 In/Out 图例；显示精度与刷新速度四参数可调（底部面板选择器，APVTS 持久化、可自动化）——FFT 大小 4096/8192/16384（默认 8192）、频段密度 1/6|1/12 倍频程（默认 61 段）、显示刷新率 15/30/60 Hz（默认 30）、分析 hop 512/1024/2048（默认 512）。

#### 修复

- **节点拖拽白色实心圆消失**：拖拽分支恢复白圆（物理滞后位置顶层绘制），液态玻璃环收敛为 `r+4` 底层光环（不遮挡 accent 光环）。
- **节点选中外环简洁化**：静止选中无外环；点按/拖拽时才显示 accent 0.18 简洁环（与推子 thumb 同款，新增 `nodePressed` 状态）。
- **类型菜单弹出位置**：`withTargetScreenArea` 锚定点击处屏幕坐标（原从组件中心弹出）。
- **ComboBox 文本越界与阴影不匹配**：`drawComboBox` 重写——自绘文本（左 padding + 预留箭头区）+ 紧凑阴影（2px 内缩，主体不再缩水）；内部 label 绘制跳过（`dynamic_cast` 仅命中 ComboBox）。
- **残余默认样式部件**：全局默认 LAF 替换（standalone）+ 节点类型菜单显式 LAF（VST3），消除 EQ 节点类型菜单 / audio settings / Options 菜单的 JUCE 默认样式。
- **灰色不透明切角矩形残留**：全面半透明化——面板卡片 0.88、轨道 0.7、开关 0.7–0.9、滚动条 0.75、TextEditor 背景 0.85；全量 `trackBackground` 使用点均带 alpha，无遗留。
- **三面板外边框未对齐**：响应/推子/底部面板统一 `drawMatteSurface` 卡片绘制（`reduced(6)` + 相同圆角），外边框精确对齐。
- **MasterGainSlider 动画帧缺失**：60Hz 组件 Timer 驱动动画期重绘（覆盖 thumb/track 缩放与高光动画，结束自停）；`thumbScale` 目标短路（光标微动不重启弹跳）。
- **fill 惯性激活失效**：起点被重置为目标位置导致惯性全程失效——拖拽/轨道点击/双击/滚轮均改为先捕获旧位置再应用值。
- **0 dB detent 符号记录**：吸附前记录原始值（`dragLastValue`），跨 0 停滞行为双向对称。
- **轨道点击后跟手拖 + gesture 配对**：track 分支置 `isDragging` 支持点击后连续拖动；gesture 单 begin/end 配对（消除 Debug `jassert` 与 DAW 自动化碎片事件）。
- **applyParametersToEngine 语义注释**：明确 JUCE 7 `getRawParameterValue` 返回**物理值**（`ParameterAdapter::getRawDenormalisedValue`，经 `convertFrom0to1` 维护）——原实现正确，无需解码。
- **弹出菜单不透明底板**（2026-08-06 审计）：`getMenuWindowFlags` 补上 `windowIsSemiTransparent` 标志且 `drawPopupMenuBackground` 先清透明，Windows 下弹出菜单恢复设计透明度。
- **FrequencyResponse 空闲 CPU**：物理 timer 改为按需启停（无动画/无物理量时自停，交互唤起）；频响路径、曲线下填充、每节点曲线、频谱段数快照改为变更时缓存，消除每帧重复计算。
- **自动化 gesture 配对**：节点双击重置、滚轮调 Q、主增益滚轮补齐 `begin/endChangeGesture`；拖拽手势改按拖拽起点记录的类型 flags 配对结束（拖拽中类型被 automation 改动不再泄漏 gesture）；右击删除正在拖拽的节点先回卷 gesture 与拖拽状态。
- **多触控隔离**：节点拖拽由首个触控源独占，其他触点的事件忽略至释放。
- **文本输入校验**：节点参数编辑器限定数字字符（最长 8 位）；空串 / `-` / `.` / `-.` 不再被 `getFloatValue()` 静默提交为 0。
- **类型菜单当前项勾选**；**FIR 长度选择器在 Zero Latency 模式下置灰**（仅 FIR 模式可用）。
- **底部面板去卡片化 + 禁用态视觉区分**（2026-08-06）：底部面板移除长条哑光卡片背景（控件直接落于窗口背景，仅保留分组竖线与延迟 chip）；`drawComboBox` 补禁用态绘制——无投影、扁平低透明体、灰文字与淡箭头（FIR 长度在 Zero Latency 模式置灰时与可点控件明确区分）。
- **底部面板分组排版**（2026-08-06）：Mode/FIR 下拉补齐标题标签（原无标题）、处理组与频谱组加大组间距并加竖分隔线、频谱下拉 76→82px（"1/6 oct" 不再截断）、延迟文本改为右端浅色胶囊 chip；面板高度与窗口布局不变。

#### 变更

- **单节点作用曲线改白色**（区别于蓝色相位曲线），图层降至主题色总曲线之下（accent 总曲线始终覆盖节点曲线）；移除暗衬线勾勒。
- **通透化**：移除内部阴影层次并提高透明度——节点标签 0.78、ComboBox 0.82、弹出菜单 0.75（标签顶部高光带与 ComboBox/菜单方向性边缘高光移除，仅留 hairline）。
- **轨道加粗联动范围**：从"仅轨道悬停"扩展为 `isHovering || isDragging || isPressed`（thumb 悬停/点按/拖拽全程联动）。
- 参数 TextEditor 配色迁移至设计系统（surface 半透明背景、accent 聚焦描边）。
- **设计系统死代码清理**（2026-08-06 审计）：移除未使用的 title/numeric 字号档与全局字号缩放、easeOutQuad/easeInOutCubic 缓动、玻璃 tooltip/base/highlight 绘制入口、1000×750 默认尺寸常量（实际 900×620 由 `SubEQLookAndFeel` 定义）；Futura.otf 随 `DesignFonts` 清理从 jucer 资源与 BinaryData 移除（字体文件同步删除）。

### 后端

#### 新增

- **FFT overlap-add 卷积**：FIR 处理从逐样本直接卷积（4096 次乘加/样本）改为分块 FFT 卷积（块 512 样本，double 精度）。FIR 模式实时 CPU 占用大幅下降，且支持更长的 FIR。
- **可选 FIR 长度**：新增 `fir_length` 参数与底部面板选择器（4096 / 16384 / 65536，默认 4096）。更长 FIR 显著改善低频精度（65536 点 @48 kHz 频域分辨率 0.37 Hz）；PDC 延迟与 tail length 随选择自动报告。
- **IIR 系数线性插值**：EQ 节点参数变化时系数在 ~15 ms 内线性过渡（凸组合保证插值过程稳定），消除手动拖拽时的 zipper noise；节点启停也有渐入效果。
- **频谱分析器参数化与输入分析**：`SpectrumAnalyzer` 的 FFT 大小（12–14 阶）/频段密度（61|121 段）/分析 hop（512–2048）改为运行时可配置，缓冲上限预分配、切换零分配零锁（配置字段原子化，GUI 安全读取）；FFT 改用共享自研 `fftInPlace`（double，复用 `SubEQ_DSPMath.h`）；新增输入分析器第二实例（EQ 处理前分析输入、处理后分析输出），为输入/输出双频谱显示提供数据。

#### 修复

- **音频线程/GUI 线程数据竞争**：`FrequencyResponse::parameterChanged` 不再直接调用 `updateEQParameters()` 写引擎（该函数此前与 `processBlock` 在音频线程无锁并发写引擎与参数缓存，属数据竞争 UB）。GUI 使用独立引擎副本（`responseEngine`）绘制响应曲线；引擎仅由音频线程写入。`reportedLatency` 与 `currentMode` 改为 `std::atomic`。
- **FIR 系数设计移出音频线程**：模式切换或参数变化时的 FIR 重设计（此前 O(N²) 直接 DFT/IDFT + 群延迟计算，耗时可达数十毫秒）改为后台线程（`juce::Thread`）异步执行，完成后通过发布-订阅机制原子换发系数（稳态无锁）。音频线程不再被系数设计阻塞。
- **自研 radix-2 FFT**：内部迭代 FFT/IDFT（double 精度）替换直接 DFT/IDFT 设计路径（O(N²) → O(N log N)）；仍不依赖本环境不可用的 JUCE FFT 复数 API。
- **后台线程生命周期修复**：`apvts` 成员声明移至 `FFTProcessor` 之前（析构逆序保证后台线程在 APVTS 销毁前 join）；`sampleRate`/`parameterSource` 改为 `std::atomic` 消除跨线程裸读写 UB。
- **PDC 与在用系数一致性**：切换 Linear Phase 时不再提前上报固定延迟，延迟统一在系数设计发布后上报，保证宿主补偿与实际处理延迟始终匹配。
- **移除输出硬裁剪 ±1.0**：`EQEngine::processChannel` 不再将输出硬裁剪到 ±1.0（±24 dB 增益叠加时的切顶失真与亚低频直流偏移消除），输出为全动态范围，由宿主/输出级处理。
- **tail length 修正**：`getTailLengthSeconds()` 在 FIR 模式下返回 `FIRLength / sampleRate`，避免宿主在传输停止时截断卷积尾音。
- **延迟线改环形缓冲**：`FFTProcessor::process` 的逐样本 4096 元素移位改为环形缓冲（每样本 O(1) 写入），显著降低 FIR 模式 CPU 占用。
- **频谱 Nyquist bin 越界读取**：`SubEQ_Spectrum::performAnalysis` 不再读取 JUCE real-only FFT packed 布局中无有效数据的 `fftData[N]`/`fftData[N+1]`（此分支实际不可达，属潜在缺陷）。
- **参数指针缓存**：`updateEQParameters` 改为使用构造函数缓存的 `std::atomic<float>*` 参数指针，消除每 block 的 48 次字符串查找。
- **overlap-add 尾部累积错误**：修复卷积尾部仅保留上一块贡献、丢弃更早块贡献的错误（多块跨度的长 FIR 输出错误）；回归测试捕获并验证修复。
- **twiddle 缓存多槽化**（2026-08-06 审计，Critical）：原单槽缓存在卷积 FFT（8192/32768/131072）与频谱 FFT（4096/8192/16384）尺寸不同时每帧整表重建——音频线程分配 + O(n) 三角函数。改为按 log2(n) 索引的 21 槽 `thread_local` 缓存 + `prewarmTwiddleTable()`（prepare 路径预热），多尺寸共存后稳态零重建。
- **overlap-add 尺寸切换污染**：FIR 长度变化导致卷积 FFT 尺寸变化时清空 overlap 累加器，消除跨尺寸的脏卷积尾部。
- **重复 prepare 丢设计**：同采样率/通道数重复 `prepareToPlay` 不再丢弃已发布 FIR 系数；设计代际号（epoch）使过期后台设计结果被丢弃。
- **旧卷积状态后台退役**：换发下来的旧 FIR 状态移交后台线程释放，音频线程零释放。
- **PDC 上报移出音频线程**：`setLatencySamples()` 改为 10 Hz `juce::Timer` 轮询上报（音频线程仅置脏标记），消除音频线程潜在分配/锁。
- **Minimum Phase PDC 修正**：最小相位 FIR 为因果设计（群延迟 0），PDC 仅报告 overlap-add 块延迟 511 样本，不再叠加群延迟估计（原过度补偿）。
- **节点启停爆音**：节点 enabled 切换增加 ~15 ms dry/wet 交叉淡化；Tilt 类型切换时第二 biquad 从 identity 淡入、被撤 biquad 向 identity 淡出，响应连续。
- **OLA NaN 遏制**：卷积输出检测到 NaN/Inf 时清空 overlap 防止毒化扩散；`processBlock` 超长块自动分块防护。
- **频谱幅值校准**：频段幅值按 16/N² 缩放校准（满幅正弦 = 0 dB）；`getSpectrum` 返回段数快照，调用方不再读到半更新状态。

#### 变更

- **FIR 模式延迟增加 511 样本**（约 10.6 ms @48 kHz）：overlap-add 分块处理延迟，已计入 PDC 与延迟显示。
- **Minimum Phase 群延迟估计改用 FFT 相位法**（O(N log N)），支持长 FIR（原 O(N²) 方法在 65536 点下不可行）；相位歧义（群延迟 > N/2）时保守回退全长度。
- **DSP 数学提取为共享头文件** `SubEQ_DSPMath.h`（模板化 FFT/IDFT、nextPow2、群延迟估计），插件与回归测试共用同一实现，消除副本漂移。
- **FFT 性能与精度优化**：twiddle 因子改为预计算查表（替代逐蝶形递推，消除累积舍入误差——float 精度提升约 150 倍，8192 点相对误差 0.36% → 0.0024%）；len=2/len=4 两级无乘特化（纯加减与 ±i 交换）；8192 点 FFT 约 72µs（double）。

### 测试

- 新增 `Tests/subeq_fft_test.cpp`：独立可执行验证（无 JUCE 依赖），构建：`cl /EHsc /std:c++17 /O2 Tests/subeq_fft_test.cpp /Fe:Tests\subeq_fft_test.exe`。
- 逐步扩展至 12 项并全部通过：FFT/IDFT round-trip（N=4096）、float/double FFT 一致性、FFT vs 直接 DFT（N=64）、环形缓冲/overlap-add 卷积 vs 直接卷积（N=4096/16384，误差为 0）、脉冲 FIR 恒等、群延迟估计（正常 + 保守回退）、overlap-add 尺寸计算。
- 液态玻璃 GUI 迁移与迭代期间回归 12 项全部通过（DSP 无改动，验证未破坏处理链路）。
- 2026-08-06 审计批扩展至 26 项并全部通过：新增多尺寸交替 twiddle 缓存正确性（double round-trip + 混尺寸后 float 16384 vs double）、`prewarmTwiddleTable` 预热、`computeMaxGroupDelay` 边界（2-tap → 0、对称高斯 FIR ≈ (N-1)/2、全零/NaN 毒化 → 保守有界 [0, N-1]）、`convolutionFftSize`/`nextPowerOfTwo` 边界。

### 已知限制

- FIR 模式低频精度受所选长度限制：4096 点 @48 kHz 频域分辨率约 11.7 Hz、时域跨度约 85 ms，10 Hz 以下响应由插值近似，与 IIR 模式（精确）存在偏差；65536 点约 0.37 Hz 但实时成本较高（每 512 样本一次 131072 点 FFT），建议仅低频精度优先的测量/母带场景使用。
- 节点拖拽/悬停动画与物理滤波在按需启停的 60Hz timer 中运行（空闲自停，仅 GUI 线程开销）；极低端机器上 8 节点全激活 + 频谱刷新可能略有绘制开销。
- 深色主题 token 已随 `DesignColours` 就位但当前 UI 固定 Light 主题，未提供主题切换入口。
- 频谱分析在音频线程执行：16384 点 FFT + 512 hop 时单核占用可观（约 10–15%），低端 CPU 建议使用 8192 点默认配置或增大 hop。

## [0.2.0] - 2026-04-20

### 新增

- **三种全局 EQ 处理模式**: 底部面板新增模式选择 ComboBox，支持三种处理方式：
  - **Zero Latency（零延迟）**: 现有 IIR Biquad 级联，0 样本延迟
  - **Minimum Phase（最小相位）**: 基于 IIR 幅频响应设计最小相位 FIR（4096 点），通过 Cepstral 方法实现，延迟由最大群延迟计算（保守策略，确保低频完整性）
  - **Linear Phase（线性相位）**: 基于 IIR 幅频响应设计严格线性相位 FIR（4096 点），延迟固定 2047 样本
- **自动延迟报告**: 模式切换时自动调用 `setLatencySamples()` 向宿主报告延迟，支持 DAW 插件延迟补偿（PDC）
- **延迟显示**: 底部面板实时显示当前模式的延迟量（ms / samples）
- **非零延迟模式隐藏相位曲线**: 切换至非零延迟模式时自动隐藏相位响应曲线，仅显示幅频响应

### 修复

- **JUCE FFT 复数 API 构建环境兼容性**: 当前构建环境下 `juce::dsp::FFT::perform()` 复数 API 输出异常（in-place 产生垃圾值，out-of-place 输出全零），导致 Linear Phase / Minimum Phase 模式下频谱显示溢出。已用直接 DFT/IDFT 公式替换所有 FFT 调用，FIR 系数设计恢复正确。
- **Minimum Phase 延迟实时刷新**: 调整 EQ 节点参数后延迟读数不再更新的问题已修复。`processBlock` 中参数变化分支现在会重新计算并调用 `setLatencySamples()` 向宿主报告延迟。

### Changed

- **窗口布局扩展**: 窗口高度从 560px 增加到 **620px**，底部新增 **60px 功能面板区域**，为 0.2.0 新功能预留开关和指示器空间。
- **主题色更新**: 频响曲线、频谱、标签高亮、滑块拇指统一从品红 `#e040fb` 改为玫瑰粉 `#FF007B`，作为新的主题色。

## [0.1.1] - 2026-04-19

### 新增

- **带通滤波器类型**: 在 8 种滤波器选项中新增了"带通"（Band Pass）节点类型。
- **非增益类型的智能垂直拖动**: 高通 / 低通 / 陷波 / 带通节点现在通过垂直拖动调节 Q 值而非增益，节点固定在 0 dB 参考线上。
- **类型感知双击重置**: Bell / Shelf / Tilt 双击重置增益和 Q；高通 / 低通 / 陷波 / 带通仅重置 Q。

### 修复

- **陷波闪退**: 陷波节点在中心频率处产生 `-Inf` dB 响应，导致 JUCE Path 断言失败。现已钳位到 `-120 ~ +60 dB` 安全范围。
- **Q 值拖动方向**: 向上拖动现在增大 Q，向下拖动减小 Q，所有非增益敏感类型均符合直觉（与增益拖动方向一致）。
- **文本编辑器关闭**: 文本编辑框现在在点击或拖动编辑器外部任意位置时都会关闭。
- **类型切换时增益重置**: 从增益敏感类型（Bell / Shelf / Tilt）切换到非增益类型（高通 / 低通 / 陷波 / 带通）时，增益自动归零至 0 dB。

### 变更

- 节点垂直位置现在具有类型感知能力：增益敏感类型（Bell、LowShelf、HighShelf、Tilt）跟随增益变化；其他类型锚定在 0 dB。
- **Q 值调节现采用对数映射**: 拖动和滚轮均使用对数映射——相等的输入变化量产生相等的*比例*变化（0.1→1 的位移与 1→10 相同）。
- **频率显示精度统一**: 所有频率读数（节点标签、文本编辑器、网格提示）现在统一显示 2 位小数，移除了之前 `≥100 Hz → 0 位小数` 的分支逻辑，避免了显示不一致的问题。

## [0.1.0] - 2026-04-18 初始版本

### 新增

- 8 节点参数均衡器（0.5 Hz ~ 500 Hz）
- 8 种滤波器类型：Bell、High Pass、Low Pass、Low Shelf、High Shelf、Notch、Tilt、Band Pass
- 实时频率响应和相位曲线
- 实时频谱分析器（1/6 倍频程，8192 点 FFT）
- Pro Q 风格节点交互（点击创建、拖动移动、右击删除、滚轮调 Q）
- 双击重置
- 主增益控制
- VST3 / 独立运行构建
- ASIO 支持
