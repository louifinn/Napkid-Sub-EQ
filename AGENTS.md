# Napkid Sub EQ

面向超低频（0.5 Hz ~ 500 Hz）的 8 节点参数均衡器 VST3 音频插件（C++ / JUCE 7.x / VS2022，仅 Windows）。GPL v3。当前版本 0.4.0。

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

无自动化测试框架；回归测试为独立可执行 `Tests/subeq_fft_test.cpp`（无 JUCE 依赖，覆盖 FFT/overlap-add/群延迟）：
`cl /EHsc /std:c++17 /O2 Tests/subeq_fft_test.cpp /Fe:Tests\subeq_fft_test.exe && Tests\subeq_fft_test.exe`（需 VS 开发环境）。变更验证 = 编译 + 测试 + 人工。

## Architecture

- **SubEQ_Core** (`Source/SubEQ_Core.h/.cpp`) — DSP 核心：`BiquadCoefficients`（含稳定性检查/修正）、`BiquadState`（Transposed Direct Form II，double 精度）、`EQNode`（8 种滤波器类型，每节点最多级联 2 个 biquad，block 级系数平滑 ~15ms）、`EQEngine`（8 节点级联 + 主增益 + 频响/相位计算）
- **PluginProcessor** (`Source/PluginProcessor.h/.cpp`) — 插件主体：APVTS、`processBlock` 按 `EQMode` 路由到 IIR 引擎或 FFTProcessor、模式/参数变化检测（含 `fir_length`）、`setLatencySamples()` PDC 报告（延迟 = 群延迟 + 511 块延迟）
- **SubEQ_DSPMath** (`Source/SubEQ_DSPMath.h`) — 共享 DSP 数学（header-only）：模板化 radix-2 FFT/IDFT（twiddle 查表优化，float/double）、nextPow2、保守群延迟估计。插件与 Tests 共用，勿复制实现
- **SubEQ_FFTProcessor** (`Source/SubEQ_FFTProcessor.h/.cpp`) — Linear/Minimum Phase FIR 设计（后台 `juce::Thread`，发布-订阅换发系数，稳态无锁）+ double 精度 overlap-add 卷积（块 512）。FIR 长度可配置；设计引擎禁用系数平滑
- **SubEQ_Parameters** (`Source/SubEQ_Parameters.h`) — `createParameterLayout()`：8 节点 × 5 参数（freq/gain/q/type/enabled）+ master_gain + bypass + eq_mode + fir_length = 43 参数；参数 ID 格式 `node<i>_<param>`；`applyParametersToEngine()` 供 GUI 曲线引擎与后台设计引擎镜像参数
- **SubEQ_Spectrum** (`Source/SubEQ_Spectrum.h/.cpp`) — 实时频谱分析：8192 点 FFT、61 个 1/6 倍频程频段、原子输出跨线程
- **SubEQ_Editor/** — GUI：`FrequencyResponse`（频响曲线 + 节点交互，GUI 私有 `responseEngine` 副本绘制）、`MasterGainSlider`、`ModeSelector`（底部模式/FIR 长度/延迟面板）、`SubEQLookAndFeel.h`（主题色与视觉常量）

线程模型：`processBlock` 在音频线程（唯一写引擎的线程，稳态零锁）；FIR 设计在后台线程（读 APVTS 原子参数）；GUI 用私有 `responseEngine` 镜像参数画曲线；频谱用 `std::atomic` 传输。改 DSP 时注意音频线程不得分配/加锁/阻塞；跨线程共享状态必须原子或经发布-订阅。

## Conventions

- 所有 DSP 运算用 `double`（低频系数在 float 下不稳定），GUI 层参数用 `float`
- 类声明用 JUCE 风格：构造/析构 + 显式 `override`，私有末尾 `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`
- 每个文件顶部有 `/* ===== ... ===== */` 注释块说明用途
- 代码注释、README、CHANGELOG、Documentation/ 均用简体中文
- 参数变更走 APVTS；节点参数默认 `enabled=false`（节点默认不激活）
- 版本记录：改动需同步更新 `CHANGELOG.md`（按 `[x.y.z] - 日期` 格式）与 `.jucer` 的 `version` 字段
- 新增 DSP 数学先考虑放 `SubEQ_DSPMath.h`（勿复制）；改动 FFT/卷积后必须跑 `Tests/subeq_fft_test.exe`

## Notes

- 空（快速补充占位）
