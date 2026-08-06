# 更新日志

Napkid Sub EQ 的所有重要变更都将记录在此文件中。

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
