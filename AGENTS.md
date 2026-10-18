# Napkid Sub EQ

面向超低频（0.5 Hz ~ 500 Hz）的 8 节点参数均衡器 VST3 音频插件（C++ / JUCE 7.x / VS2022，仅 Windows）。GPL v3。当前版本 0.4.5。

## Project

- 双精度 IIR Biquad 引擎 + 三种全局处理模式（Zero Latency / Minimum Phase / Linear Phase FIR），FIR 长度可选（4096 / 16384 / 65536）
- 入口：`Source/PluginProcessor.cpp`（`SubEQAudioProcessor`，`processBlock` + APVTS）
- DSP 全部位于 `namespace SubEQ`；GUI 在 `Source/SubEQ_Editor/`
- 项目文件：`Napkid Sub EQ.jucer`（Projucer 项目，VS2022 导出目标）；`.jucer` 中 JUCE 模块路径硬编码为 `D:/Program Files/JUCE/modules`，换机器需在 Projucer 里改

## Commands

无命令行构建脚本。构建路径：

1. 用 Projucer 打开 `Napkid Sub EQ.jucer` → File → Save Project and Open in IDE
2. VS2022 中选 **Release x64** → Build Solution（Debug/Release 均已配 ASIO include 路径）
3. 命令行亦可：`MSBuild.exe "Builds/VisualStudio2022/Napkid Sub EQ.sln" //p:Configuration=Release //p:Platform=x64 //v:q`（本机 VS 18 已装 v143 工具集，无需 PlatformToolset 覆盖）

产物（`Builds/` 已被 gitignore，不入库）：
- VST3：`Builds/VisualStudio2022/x64/Release/VST3/`
- Standalone：`Builds/VisualStudio2022/x64/Release/Standalone Plugin/`

无自动化测试框架；回归测试为独立可执行 `Tests/subeq_fft_test.cpp`（无 JUCE 依赖，430 项：FFT/twiddle 多槽缓存/biquad 系数·状态·响应·设计/overlap-add 卷积原语/FIR 设计（含 v0.4.5 解析 DC/Nyquist 零点矩阵：单节点 + 多节点级联）/FilterType 映射/频段中心频率/窗函数族（均值/ENBW/旁瓣）/频谱 choice 映射/VQT 层结构·帧长封顶·抽取阶梯/离线 VQT 原型引擎（校准/泄漏/直流/双音/性能基准）/显示平滑数学（功率域核/半径表/弹道系数）与端到端（峰展宽/噪声驯服/关断下落速率）/坐标映射/节点交互规则/级联淡化计划/弹簧积分器）：
`cl /EHsc /std:c++17 /O2 /utf-8 Tests/subeq_fft_test.cpp /Fo:Tests\subeq_fft_test.obj /Fe:Tests\subeq_fft_test.exe && Tests\subeq_fft_test.exe`（需 VS 开发环境）。变更验证 = 编译 + 测试 + 人工。

## Architecture

- **SubEQ_Core** (`Source/SubEQ_Core.h/.cpp`) — DSP 核心：`BiquadCoefficients`（含稳定性检查/修正）、`BiquadState`（Transposed Direct Form II，double 精度）、`EQNode`（8 种滤波器类型，每节点最多级联 2 个 biquad，block 级系数平滑 ~15ms，启停 dry/wet 交叉淡化，Tilt 切换第二 biquad 淡入淡出——淡化计划由纯函数 `computeBiquadFadePlan` 驱动）、`EQEngine`（8 节点级联 + 主增益 + 频响/相位计算 + 旁路 ~15ms 湿/干淡化（热旁路，节点链保持处理））
- **PluginProcessor** (`Source/PluginProcessor.h/.cpp`) — 插件主体：APVTS、`processBlock` 按 `EQMode` 路由到 IIR 引擎或 FFTProcessor、模式/参数变化检测（含 `fir_length`）、`setLatencySamples()` PDC 经私有 10 Hz `juce::Timer` 轮询上报（音频线程仅置脏标记；Linear Phase 延迟 = (N-1)/2 + 511 块延迟，Minimum Phase = 511，因果 FIR 群延迟为 0）
- **SubEQ_DSPMath** (`Source/SubEQ_DSPMath.h`) — 共享 DSP 数学（header-only）：模板化 radix-2 FFT/IDFT（twiddle 按 log2(n) 多槽缓存 + `prewarmTwiddleTable` 预热，float/double）、nextPow2、`convolutionFftSize`。插件与 Tests 共用，勿复制实现
- **SubEQ_FFTProcessor** (`Source/SubEQ_FFTProcessor.h/.cpp`) — Linear/Minimum Phase FIR 设计（后台 `juce::Thread`，发布-订阅换发系数，稳态无锁；epoch 复检在发布临界区内）+ double 精度 overlap-add 卷积（块 512）。FIR 长度可配置；设计引擎禁用系数平滑；prepare 同采样率保留已发布设计（epoch 丢弃过期、块尺寸增大仅重分配淡化缓冲）、旧卷积状态后台线程退役（锁外析构）、新设计发布：同延迟（groupDelay 相等）经 1024 样本交叉淡化（参数编辑无输出阶跃）、延迟不同（FIR 长度/模式变化）冲刷累加器立即切换（PDC 与输出一致）；淡化期间新发布同延迟设计挂起待收尾后切换；输出 NaN/Inf 遏制
- **SubEQ_Parameters** (`Source/SubEQ_Parameters.h`) — `createParameterLayout()`：8 节点 × 5 参数（freq/gain/q/type/enabled）+ master_gain + bypass + eq_mode + fir_length + 6 个频谱参数（spectrum_fft_size【v0.4.0 起语义为分辨率档 Fast/Balanced/Fine，ID 保留老 session 兼容】/spectrum_window/spectrum_smoothing【v0.4.1 频域段间平滑】/band_density/refresh_rate/hop_size）= 50 参数；参数 ID 格式 `node<i>_<param>`；`applyParametersToEngine()` 供 GUI 曲线引擎与后台设计引擎镜像参数
- **SubEQ_Spectrum** (`Source/SubEQ_Spectrum.h/.cpp`) — 实时频谱分析（v0.4.0 起为多采样率 VQT 内核，设计数学全在 `SubEQ_VQTDesign.h`）：0.5 Hz 起 10 个倍频程层（顶层 ~3 kHz（48 kHz；44.1 kHz 因 2 的幂整除约束为 11025 Hz）、向下逐层 ÷2，级联 ↓2 抗混叠 + 0.05 Hz 直流阻断）、每层帧加窗 → FFT → 频域匹配核点积（满幅正弦落段中心 = 0 dB）、帧长 = 分辨率需求与 2 s 封顶之小者、61/121 频段、分辨率三档 × 窗型四档共 24 个核组合在 prepare() 全量预生成（configure 仅切指针）、`getSpectrum` 返回段数快照、原子输出跨线程、编辑器存在时门控分析、按层惰性 twiddle 预热（F-010）；显示平滑管线（v0.4.1，数学在 `SubEQ_SpectrumMath.h`；v0.4.2 裁定起全程 double）：瞬时功率 → 频域段间平滑（功率域 Hann 核、半径按 档位×密度 查表、边缘重归一化、`spectrum_smoothing` 四档 Off/1/6/1/3/2/3 oct）→ 功率域一阶时间包络（α = 1−e^(−dt/τ) 按各层真实分析间隔归一化——折算收口在共享 `vqtTierEnvelopeAlphas()`，attack τ=20 ms、release 恒 180 dB/s）→ 最后才转 dB/钳位；属主层只更新本层段（跨层平滑读邻段上次分析值）；仅原子发布边界收窄 float，与离线原型数值一致
- **共享纯计算头文件**（均 header-only、无 JUCE 依赖，插件与 Tests 共用）：`SubEQ_Biquad.h`（双精度 biquad 系数/状态/频响）、`SubEQ_BiquadDesign.h`（RBJ 系数设计 + `computeBiquadFadePlan` 级联淡化计划）、`SubEQ_FIRDesign.h`（线性/最小相位 FIR 设计）、`SubEQ_FFTConvolver.h`（overlap-add 卷积原语）、`SubEQ_SpectrumMath.h`（octave 频段中心频率 + 频段密度轴 `kBandsByDensity`/`bandDensityForBands`（61/121 段数的单一来源）+ 显示范围与地板 `kDisplayRangeDb`/`kDisplayFloorDb`/`kDisplayFloorPower`（−60 dB 的单一来源）+ 显示平滑：核权重/功率域段平滑/半径表/弹道系数/包络物理常数 + 共享分层显示更新 `vqtUpdateDisplayBands` + 按层 dt/α 折算 `vqtTierEnvelopeAlphas`）、`SubEQ_WindowFunctions.h`（Hann/Blackman/B-Harris/Nuttall 窗族系数/均值/ENBW/旁瓣）、`SubEQ_VQTDesign.h`（VQT 层结构/帧长封顶/帧加窗匹配核/抽取系数/直流阻断，Tests 内含离线原型引擎共用同一套数学），`SubEQ_CoordinateMapper.h`（频响图坐标映射）、`SubEQ_NodeInteraction.h`（增益敏感类型/类型切换重置/对数 Q 步进）、`SubEQ_Spring.h`（二阶弹簧积分器）、`SubEQ_FilterType.h`（FilterType 枚举 + int↔枚举映射）、`SubEQ_SpectrumConfig.h`（频谱 choice 下标→语义值解码）
- **SubEQ_Editor/** — GUI：`FrequencyResponse`（频响曲线 + 节点交互，GUI 私有 `responseEngine` 副本绘制）、`MasterGainSlider`、`ModeSelector`（底部面板：处理组模式/FIR + 频谱组 + 延迟 chip 分组排版）、`SubEQLookAndFeel.h`（仅保留窗口/节点/网格布局常量，遗留颜色已删除）
- **SubEQ_Editor/DesignSystem/** — 液态玻璃设计系统：`DesignColours.h`（暖白奶色 + `#FF007B` + 莫兰迪色板，仅 Light 主题）、`DesignConstants.h`（圆角/动画时长/弹簧物理/字号/底部面板布局 token——每个 token 必须有真实引用点）、`DesignFonts.h/.cpp`（Avenir 内嵌字体，`Source/Fonts/`，magic static 线程安全加载）、`AnimationUtils.h/.cpp`（缓动 + `AnimatedValue` 动画器）、`LiquidGlassEffect.h/.cpp`（六层玻璃绘制，高光随拖拽偏移）、`DesignLookAndFeel.h/.cpp`（`LookAndFeel_V4` 控件重载 + 静态助手；无 Slider 重载——全仓不使用 `juce::Slider`）。视觉约定：节点 hover 放大 2x（弹性）、拖拽玻璃体 + 低通弹簧物理滞后、释放回弹

线程模型：`processBlock` 在音频线程（唯一写引擎的线程，稳态零锁）；FIR 设计在后台线程（读 APVTS 原子参数）；GUI 用私有 `responseEngine` 镜像参数画曲线；频谱用 `std::atomic` 传输；PDC 由 10 Hz Timer 轮询上报（`setLatencySamples` 不在音频线程调用）。改 DSP 时注意音频线程不得分配/加锁/阻塞；跨线程共享状态必须原子或经发布-订阅。

## Conventions

- 所有 DSP 运算用 `double`（低频系数在 float 下不稳定），GUI 层参数用 `float`；频谱**显示管线**亦为 double（v0.4.2 裁定，含平滑/包络/暂存），仅原子发布边界（`bandData.store`）与 GUI 消费的 `float` 缓冲之间收窄
- 类声明用 JUCE 风格：构造/析构 + 显式 `override`，私有末尾 `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`
- 每个文件顶部有 `/* ===== ... ===== */` 注释块说明用途
- 代码注释、README、CHANGELOG、Documentation/ 均用简体中文
- 参数变更走 APVTS；节点参数默认 `enabled=false`（节点默认不激活）
- 版本记录：改动需同步更新 `CHANGELOG.md`（按 `[x.y.z] - 日期` 格式）与 `.jucer` 的 `version` 字段；涉及 VQT/频谱数学的改动必须跑 `Tests/subeq_fft_test.exe`（离线原型与运行时内核共用 `SubEQ_VQTDesign.h`，测试直接覆盖交付数学）
- 新增 DSP 数学先考虑放 `SubEQ_DSPMath.h`（勿复制）；改动 FFT/卷积后必须跑 `Tests/subeq_fft_test.exe`

## Notes

- 从 Git Bash 跑 cl/MSBuild：`vcvars64.bat`（`D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\`）直接 `cmd //c` 调用会因引号/参数转义失败——用 Write 工具写一个临时 .bat（`call vcvars64 → cd /d 项目目录 → cl/MSBuild`）到 `%TEMP%` 再 `cmd //c` 执行；批处理内 MSBuild 参数用单斜杠 `/p:`。Bash 内联 printf 写 .bat 会把 `\U` 当 Unicode 转义损坏文件
- 残留 standalone 进程会锁住 exe 导致 LNK1104 链接失败：`tasklist //fi "IMAGENAME eq Napkid Sub EQ.exe"` 查到后 `taskkill //pid <pid> //f` 再重建
- 统计回归断言数：测试 exe 逐条打印 `PASS:`，`awk '/^PASS:/{p++} END{print p}'` 统计（grep -c 在 Git Bash 此环境输出异常）；文档中的"X 项"以实跑计数为准
- Git Bash 向 `cmd //c` 传递 MSBuild 开关时，`/t:Rebuild` 的单斜杠开头会被 MSYS 路径转换破坏——改用等价前缀 `-t:Rebuild`（MSBuild 同时接受 `/` 与 `-`）

## Agent skills

### Issue tracker

Issue 与规格存放在 GitHub Issues（本仓库 `louifinn/Napkid-Sub-EQ`），经 `gh` CLI 读写。见 `docs/agents/issue-tracker.md`。

### Triage labels

triage 五角色标签沿用默认命名（`needs-triage` / `needs-info` / `ready-for-agent` / `ready-for-human` / `wontfix`）。见 `docs/agents/triage-labels.md`。

### Domain docs

单上下文（single-context）布局：根 `GLOSSARY.md` + 根 `docs/adr/`（两者尚未创建，按 `/domain-modeling` 惰性生成）。见 `docs/agents/domain.md`。
