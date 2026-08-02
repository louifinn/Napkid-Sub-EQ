# 更新日志

Napkid Sub EQ 的所有重要变更都将记录在此文件中。

## [0.4.0] - 2026-08-02

### 新增

- **FFT overlap-add 卷积**：FIR 处理从逐样本直接卷积（4096 次乘加/样本）改为分块 FFT 卷积（块 512 样本，double 精度）。FIR 模式的实时 CPU 占用大幅下降，且支持更长的 FIR。
- **可选 FIR 长度**：新增 `fir_length` 参数与底部面板选择器（4096 / 16384 / 65536，默认 4096）。更长 FIR 显著改善低频精度（65536 点 @48 kHz 频域分辨率 0.37 Hz，接近 0.5 Hz 完整周期），代价是延迟与 CPU 增加。PDC 延迟与 tail length 随选择自动报告。
- **IIR 系数线性插值**：EQ 节点参数变化时系数在 ~15 ms 内线性过渡（凸组合保证插值过程稳定），消除手动拖拽时的 zipper noise；节点启停也有渐入效果。

### Changed

- **FIR 模式延迟增加 512 样本**（约 10.7 ms @48 kHz）：overlap-add 分块处理延迟，已计入 PDC 与延迟显示。
- **Minimum Phase 群延迟估计改用 FFT 相位法**（O(N log N)），支持长 FIR（原 O(N²) 方法在 65536 点下不可行）；相位歧义（群延迟 > N/2）时保守回退全长度。
- **DSP 数学提取为共享头文件** `SubEQ_DSPMath.h`（模板化 FFT/IDFT、nextPow2、群延迟估计），插件与回归测试共用同一实现，消除副本漂移。
- **FFT 性能与精度优化**：twiddle 因子改为预计算查表（替代逐蝶形递推，消除累积舍入误差——float 精度提升约 150 倍，8192 点相对误差 0.36% → 0.0024%）；len=2/len=4 两级无乘特化（纯加减与 ±i 交换）；8192 点 FFT 约 72µs（double）。

### 修复

- **overlap-add 尾部累积错误**：修复了卷积尾部仅保留上一块贡献、丢弃更早块贡献的错误（多块跨度的长 FIR 输出错误）；回归测试捕获并验证修复。

### 测试

- `Tests/subeq_fft_test.cpp` 扩展至 12 项：FFT round-trip、float/double FFT 一致性、FFT vs 直接 DFT、overlap-add vs 直接卷积（N=4096/16384，误差为 0）、脉冲恒等、群延迟估计（正常 + 保守回退）、overlap-add 尺寸计算。全部通过。

### 已知限制

- 65536 点 FIR 的实时成本较高（每 512 样本一次 131072 点 FFT），建议仅低频精度优先的测量/母带场景使用。
- FIR 模式的 0.5 Hz 精度仍受所选 FIR 长度限制（4096 点约 11.7 Hz 分辨率；65536 点约 0.37 Hz）。

## [0.3.0] - 2026-08-02

### 修复（实时安全）

- **音频线程/GUI 线程数据竞争**：`FrequencyResponse::parameterChanged` 不再直接调用 `updateEQParameters()` 写引擎（该函数此前与 `processBlock` 在音频线程无锁并发写引擎与参数缓存，属数据竞争 UB）。现在 GUI 使用独立的引擎副本（`responseEngine`，经 `SubEQ::applyParametersToEngine` 从 APVTS 镜像参数）绘制响应曲线；引擎仅由音频线程写入。`reportedLatency` 与 `currentMode` 改为 `std::atomic`。
- **FIR 系数设计移出音频线程**：模式切换或参数变化时的 FIR 重设计（此前为 O(N²) 直接 DFT/IDFT + 群延迟计算，耗时可达数十毫秒）改为后台线程（`juce::Thread`）异步执行，完成后通过发布-订阅机制原子换发系数（稳态无锁，仅版本号原子比较）。音频线程不再被系数设计阻塞。
- **自研 radix-2 FFT**：新增内部迭代 FFT/IDFT（double 精度）替换直接 DFT/IDFT 设计路径（O(N²) → O(N log N)），进一步缩短后台设计时间；仍不依赖本环境不可用的 JUCE FFT 复数 API。
- **后台线程生命周期修复**：`apvts` 成员声明移至 `FFTProcessor` 之前（析构逆序保证后台线程在 APVTS 销毁前 join）；`sampleRate`/`parameterSource` 改为 `std::atomic` 消除跨线程裸读写 UB。
- **PDC 与在用系数一致性**：切换 Linear Phase 时不再提前上报固定延迟，延迟统一在系数设计发布后上报，保证宿主补偿与实际处理延迟始终匹配。

### 修复（可听质量）

- **移除输出硬裁剪 ±1.0**：`EQEngine::processChannel` 不再将输出硬裁剪到 ±1.0（±24 dB 增益叠加时的切顶失真与亚低频直流偏移消除），输出为全动态范围，由宿主/输出级处理。
- **tail length 修正**：`getTailLengthSeconds()` 在 FIR 模式下返回 `FIRLength / sampleRate`，避免宿主在传输停止时截断卷积尾音。
- **延迟线改环形缓冲**：`FFTProcessor::process` 的逐样本 4096 元素移位改为环形缓冲（每样本 O(1) 写入），显著降低 FIR 模式 CPU 占用。

### 修复（其它）

- **频谱 Nyquist bin 越界读取**：`SubEQ_Spectrum::performAnalysis` 不再读取 JUCE real-only FFT packed 布局中无有效数据的 `fftData[N]`/`fftData[N+1]`（此分支实际不可达，属潜在缺陷）。
- **参数指针缓存**：`updateEQParameters` 改为使用构造函数缓存的 `std::atomic<float>*` 参数指针，消除每 block 的 42 次字符串查找。

### 测试

- 新增 `Tests/subeq_fft_test.cpp`：独立可执行验证（无 JUCE 依赖），覆盖自研 FFT/IDFT round-trip（N=4096）、FFT vs 直接 DFT（N=64）、环形缓冲卷积 vs 线性移位卷积（L=4096）、脉冲 FIR 恒等还原，全部通过（误差为 0）。构建：`cl /EHsc /std:c++17 Tests/subeq_fft_test.cpp`。

### 已知限制

- FIR（4096 点 @48 kHz）时域跨度约 85 ms、频域分辨率约 11.7 Hz，10 Hz 以下响应由插值近似，与 IIR 模式（精确）存在偏差；后续版本计划以 FFT overlap-add 卷积 + 更长 FIR 解决。

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
