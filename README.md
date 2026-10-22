# Napkid Sub EQ

[English](README.en.md) | 简体中文

一款专为超低频而生的图形化参数均衡器 VST3 音频插件，精准调控 **0.5 Hz ~ 500 Hz** 频段。

---

## 概述

Napkid Sub EQ 是一款专为低频信号处理设计的参量均衡器插件。不同于市面上常见的全频段 EQ，它专注于 **0.5 Hz 至 500 Hz** 的核心低频范围，采用 **双精度 IIR Biquad** 滤波器架构，确保在极低频率下依然保持精确的系数计算和稳定的滤波响应。

特别适用于：
- 超低频声音设计
- 低频测量与校准
- 低频动态处理链中的 EQ 模块
- 直流/单极性控制信号的频响调节

---

## 界面预览

![Napkid Sub EQ 界面截图](Interface.png)

---

## 功能特性

- **8 节点参数 EQ**：每个节点独立控制频率、增益、Q 值和滤波器类型
- **8 种滤波器类型**：Bell、High Pass、Low Pass、Low Shelf、High Shelf、Notch、Tilt、Band Pass
- **±24 dB 增益范围**：满足极端的增益需求
- **双精度 IIR Biquad**：Direct Form II Transposed 结构，数值稳定
- **三种全局处理模式**（v0.2.0 新增）：
  - **Zero Latency（零延迟）**：纯 IIR 架构，无额外处理延迟
  - **Linear Phase（线性相位）**：基于 IIR 幅频响应设计严格线性相位 FIR，固定延迟 (N-1)/2 + 511 样本块延迟
  - **Minimum Phase（最小相位）**：基于 IIR 幅频响应设计最小相位 FIR，通过 Cepstral 方法实现（因果 FIR，群延迟为 0），延迟为 511 样本块延迟
- **可选 FIR 长度**（v0.3.0 新增）：4096 / 16384 / 65536 点（Linear / Minimum Phase 模式），更长 FIR 提升低频精度（65536 点 @48 kHz 分辨率约 0.73 Hz），代价是延迟与 CPU 增加
- **FFT overlap-add 卷积**（v0.3.0）：自研双精度 radix-2 FFT（twiddle 查表优化），FIR 处理高效且精度高
- **自动延迟补偿（PDC）**：模式切换或参数调整时自动向宿主报告延迟，支持 DAW 插件延迟补偿
- **实时延迟显示**：底部面板实时显示当前模式的延迟量（ms / samples）
- **实时相位响应曲线**：在三种处理模式都显示，与幅频曲线同步。Zero Latency 模式为 IIR 相位；Minimum / Linear Phase 模式为扣除 FIR 群延迟后的剩余相位（excess phase），Linear Phase 因此近似平坦 0°；后台发布新设计后曲线自动刷新
- **实时频谱分析（v0.4.0 起为多采样率 VQT 引擎）**：输入/输出双频谱曲线，0.5~500 Hz 全十倍频程独立估计（旧 FFT 引擎在 ~6 Hz 以下由 bin 插值伪造）；1/6 或 1/12 倍频程密度、三档分辨率、四种分析窗（Hann/Blackman/B-Harris/Nuttall）、15/30/60 Hz 刷新率均可选
- **频谱显示平滑（v0.4.1）**：频域段间平滑（功率域 Hann 权重核，Off / 1/6 / 1/3 / 2/3 倍频程可选，v0.5.0 起默认 Off）；时间包络在功率域运行并按各分析层真实间隔归一化——全频段统一 ~180 dB/s 匀速下落，修复底部频谱释放拖尾
- **频谱纵轴 24/60 dB（v0.5.0）**：默认 60 dB 全动态范围；图例区 chip 可切到 24 dB 与 EQ 网格对齐的放大视图
- **便捷交互**：点击创建、拖拽移动、右击删除、滚轮调 Q（HP/LP/Notch/BP 上下拖动调 Q）
- **双击重置**：Bell/Shelf/Tilt 双击归零增益和 Q，HP/LP/Notch/BP 双击仅重置 Q
- **ASIO 支持**：低延迟音频接口兼容

---

## 技术规格

| 参数 | 规格 |
|------|------|
| 频率范围 | 0.5 Hz ~ 500 Hz |
| 增益范围 | -24 dB ~ +24 dB |
| Q 值范围 | 0.1 ~ 10.0 |
| 节点数量 | 8 个（独立开关） |
| 参数数量 | 51（8 节点 × 5 + 主增益 / 旁路 / 模式 / FIR 长度 + 7 个频谱参数） |
| 滤波器精度 | double (64-bit) |
| 频谱分辨率 | 1/6 倍频程（61 频段，默认）或 1/12 倍频程（121 频段） |
| 频谱分析引擎 | 多采样率 VQT 滤波器组（10 倍频程层，底部帧长 ≥2 s 的 2 的幂；48 kHz 约 2.73 s） |
| 频谱分辨率档 | Fast / Balanced / Fine（核长尺度 0.7/1.0/1.5，默认 Balanced） |
| 频谱分析窗 | Hann / Blackman / B-Harris / Nuttall（默认 B-Harris，-92 dB 旁瓣） |
| 频谱更新率 | 15 / 30 / 60 Hz 可选（默认 30 Hz） |
| 频谱分析跳步 | 512 / 1024 / 2048 样本（默认 512，顶层步距、全层实时锁步） |
| 频谱平滑 | Off / 1/6 / 1/3 / 2/3 oct（默认 Off，功率域段间平滑） |
| 频谱纵轴 | 24 dB / 60 dB（默认 60，图例 chip 切换） |
| FIR 长度 | 标称 4096 / 16384 / 65536 点（Linear 模式内部为 Type-I 4095 / 16383 / 65535 抽头） |
| 处理模式 | Zero Latency / Linear Phase / Minimum Phase |
| 处理延迟 | 0 ~ 32767 + 511 样本块延迟（依模式与 FIR 长度而定） |
| 支持格式 | VST3, Standalone |
| 窗口尺寸 | 1040 × 620 像素 |
| 建议最低采样率 | 44100 Hz（软性规格声明，无代码强制下限） |

---

## 系统要求

- **操作系统**：Windows 10/11 (64-bit)
- **DAW**：任何支持 VST3 的宿主
- **编译器**：Visual Studio 2022 或更高（Projucer 导出目标为 VS2022）
- **依赖**：JUCE 7.x 框架
- **必需（当前工程配置）**：ASIO SDK（`.jucer` 默认 `JUCE_ASIO=1` 并引用 `D:/Program Files/JUCE/modules/juce_audio_devices/native/asio`）

---

## 构建说明

### 前置条件

1. 安装 [JUCE](https://juce.com/)（推荐 7.x 版本）
2. 安装 Visual Studio 2022（包含 C++ 桌面开发工作负载）
3. 下载 [Steinberg ASIO SDK](https://www.steinberg.net/developers/) 并放置到 `.jucer` 引用的路径（当前为 `D:/Program Files/JUCE/modules/juce_audio_devices/native/asio`，换机需同步修改）

### 构建步骤

1. 使用 Projucer 打开 `Napkid Sub EQ.jucer`
2. 确认模块路径指向你的 JUCE modules 目录
3. 点击 **File → Save Project and Open in IDE**
4. 在 Visual Studio 中选择 **Release x64** 配置
5. 点击 **Build → Build Solution**

构建完成后，VST3 插件位于：
```
Builds/VisualStudio2022/x64/Release/VST3/
```

Standalone 可执行文件位于：
```
Builds/VisualStudio2022/x64/Release/Standalone Plugin/
```

---

## 使用指南

### 创建和编辑节点

| 操作 | 说明 |
|------|------|
| 左键点击空白区域 | 在点击位置创建新节点 |
| 左键拖拽节点 | 调节频率和增益（Bell/Shelf/Tilt）或频率和 Q（HP/LP/Notch/BP） |
| 右键点击节点 | 删除节点 |
| 滚轮（在节点上） | 调节 Q 值 |
| 双击节点 | 重置增益为 0 dB，Q 为 0.707（HP/LP/Notch/BP 只重置 Q） |
| Shift + 拖拽 | 仅调节频率 |
| Ctrl + 拖拽 | 仅调节增益（Bell/Shelf/Tilt）或仅调节 Q（HP/LP/Notch/BP） |

### 处理模式切换

底部面板提供处理模式选择：
- **Zero Latency**：纯 IIR 处理，零延迟，适合实时演奏和监听
- **Linear Phase**：严格线性相位 FIR，固定延迟 (N-1)/2 + 511 样本块延迟，相位失真为零，适合母带处理
- **Minimum Phase**：最小相位 FIR（因果设计，群延迟为 0），延迟仅 511 样本块延迟，无预振铃（pre-ringing），适合一般混音使用

底部面板的 **FIR Length** 选择器（4096 / 16384 / 65536）在两种 FIR 模式下生效：更长 FIR 提升低频精度（65536 点 @48 kHz 频域分辨率约 0.73 Hz，0.5 Hz 信号的一个完整周期约 2 s），但延迟与 CPU 占用同步增加。切换时插件自动向宿主报告延迟（PDC）。底部面板同时提供频谱显示参数选择器（分辨率 / 窗函数 / 频段密度 / 刷新率 / 分析 hop / 平滑，v0.4.1 起频谱组为六列）。

切换模式时插件会自动向宿主报告延迟，确保 DAW 的延迟补偿（PDC）正常工作。

### 节点参数标签

选中节点后显示浮动参数标签，可点击编辑：
- **F (Freq)**：频率值
- **G (Gain)**：增益值
- **Q**：Q 值
- **T (Type)**：滤波器类型（支持弹出菜单选择）

### 总增益控制

右侧面板为总增益推子，支持：
- 垂直拖拽调节总输出增益（-24 dB ~ +24 dB）
- 双击推子归零增益

---

## 项目结构

```
Sub EQ/
├── Source/
│   ├── PluginProcessor.h/.cpp       # 插件处理器主体
│   ├── PluginEditor.h/.cpp          # 插件编辑器入口
│   ├── SubEQ_Core.h/.cpp            # 双精度 Biquad EQ 引擎
│   ├── SubEQ_DSPMath.h              # 共享 DSP 数学（模板 FFT/IDFT、nextPow2、convolutionFftSize）
│   ├── SubEQ_FFTProcessor.h/.cpp    # FIR 系数设计与 overlap-add 卷积（Linear / Minimum Phase）
│   ├── SubEQ_Parameters.h           # APVTS 参数定义
│   ├── SubEQ_Spectrum.h/.cpp        # 实时频谱分析器（多采样率 VQT 内核）
│   ├── SubEQ_Biquad.h               # 双精度 biquad 系数/状态/频响（共享纯计算，无 JUCE）
│   ├── SubEQ_BiquadDesign.h         # RBJ 系数设计（共享纯计算，无 JUCE）
│   ├── SubEQ_FIRDesign.h            # 线性/最小相位 FIR 设计（共享纯计算，无 JUCE）
│   ├── SubEQ_FFTConvolver.h         # overlap-add 卷积原语（共享纯计算，无 JUCE）
│   ├── SubEQ_SpectrumMath.h         # octave 频段中心频率（共享纯计算）
│   ├── SubEQ_SpectrumConfig.h       # 频谱 choice 下标→语义值解码（共享纯计算）
│   ├── SubEQ_WindowFunctions.h       # 窗函数族系数/ENBW/旁瓣（共享纯计算）
│   ├── SubEQ_VQTDesign.h             # VQT 层结构/帧长/匹配核/抽取链（共享纯计算）
│   ├── SubEQ_CoordinateMapper.h     # 频响图坐标映射（共享纯计算）
│   ├── SubEQ_NodeInteraction.h      # 增益敏感类型/切换重置/对数 Q 步进（共享纯计算）
│   ├── SubEQ_Spring.h               # 二阶弹簧积分器（共享纯计算）
│   ├── SubEQ_FilterType.h           # FilterType 枚举 + int↔枚举映射（共享纯计算）
│   ├── SubEQ_OutputStage.h          # 后置主增益 ramp / 延迟补偿旁路（共享纯计算）
│   ├── SubEQ_FIRHistory.h           # T2 输入历史环
│   ├── SubEQ_FIRSwitchState.h       # warm 边界 / 交叉淡化纯策略
│   ├── SubEQ_FirResolution.h        # FIR 分辨率评估
│   └── SubEQ_Editor/
│       ├── SubEQLookAndFeel.h       # 窗口/节点/网格布局常量（遗留颜色已删除）
│       ├── FrequencyResponse.h/.cpp # 频响曲线绘制和节点交互
│       ├── MasterGainSlider.h/.cpp  # 总增益推子
│       ├── ModeSelector.h/.cpp      # 底部模式选择面板（v0.2.0 新增）
│       └── DesignSystem/            # 液态玻璃设计系统（颜色/字体/动画/玻璃效果/LookAndFeel）
│   └── Fonts/                       # 内嵌 Inter Regular/Bold（SIL OFL 1.1，经 jucer 资源编入 BinaryData）
├── Builds/VisualStudio2022/         # VS 项目生成目录（gitignored）
├── Napkid Sub EQ.jucer              # Projucer 项目文件
└── README.md                        # 本文件
```

---

## 相关文档

- **[更新日志](CHANGELOG.md)** — 查看所有版本的变更记录
- **[架构设计文档](Documentation/SubEQ_Architecture_Design.md)** — 深入了解 DSP 架构、GUI 设计和关键实现决策

---

## 关键技术决策

### 为什么使用 double 精度？

对于 0.5 Hz 的滤波器，归一化频率约为 1.04×10⁻⁵（@ 48 kHz）。在 float 精度下，三角函数计算和部分系数可能因舍入误差产生显著偏差，导致：
- 频率响应偏离设计值
- IIR 极点靠近或超出单位圆（不稳定）

双精度（64-bit）提供了约 15 位十进制精度，足以安全处理这些极端系数。

### 三种处理模式的设计考量

| 模式 | 延迟 | 相位特性 | 适用场景 |
|------|------|----------|----------|
| **Zero Latency** | 0 samples | 最小相位（IIR 固有） | 实时演奏、监听、低延迟要求 |
| **Linear Phase** | (N-1)/2 + 511 samples | 严格线性相位 | 母带处理、相位敏感操作 |
| **Minimum Phase** | 511 samples（因果 FIR，群延迟为 0） | 最小相位（FIR 设计） | 混音、避免预振铃 |

- **Linear Phase** 使用频域采样 + IDFT 设计对称 FIR 系数，确保严格线性相位，但引入固定延迟和预振铃
- **Minimum Phase** 使用 Cepstral 方法从幅频响应提取最小相位 FIR（因果设计，群延迟为 0），无预振铃
- 两种 FIR 模式均基于当前 IIR 幅频响应设计，确保频响一致性；FIR 长度（4096 / 16384 / 65536）可选，另有 511 样本 overlap-add 块处理延迟

### 为什么是 0.5 Hz 起？

此插件设计目标之一是处理接近直流的控制信号。0.5 Hz 信号在 48 kHz 采样率下，一个完整周期横跨约 96,000 个样本（2 秒），提供了充足的数据来精确建模极低频行为。

---

## 已知限制

- **GUI 仅限 Windows**：当前使用 Projucer 的 VS2022 导出目标
- **频谱极低频段（0.5~1 Hz）的显示摆动**：v0.4.0 起 VQT 引擎在约 4~16 Hz 以上（随窗型/分辨率档）恢复满倍频程分辨；v0.5.0 把底部帧长从“≤2 s 的 2 的幂”改为“≥2 s 的 2 的幂”（48 kHz 约 2.73 s），0.5 Hz 段的周期覆盖改善，但窗内仍不足两个完整周期，读数存在慢速相位摆动（物理约束；底部支撑截断与包络平滑只能部分缓解）。详见 `Documentation/SubEQ_VQT_Feasibility.md` §8
- **频谱噪声底口径**：VQT 段能量按窗 ENBW × 层带宽聚合（层间 2 倍阶梯），噪声底形状与旧"单 bin 取样"不同——更平（每倍频程恒定带宽比例），属预期行为
- **44.1 kHz 下频谱 CPU 偏高**：2 的幂抽取整除约束使顶层过采样比偏高（约 2 倍于 48 kHz），成本回到旧引擎量级；48/96/192 kHz 不受影响
- **频谱平滑的峰展宽（v0.4.1）**：段间平滑在驯服噪声起伏的同时会展宽孤立纯音峰——最坏情形（高频孤立单音）1/6 档约 -4.8 dB、1/3 档 -7.0 dB、2/3 档 -9.5 dB；低频段因分析核本就跨段重叠，损失显著更小。需要逐段精确读数时选 Off
- **FIR 模式低频精度与延迟的权衡**：FIR 长度可选（4096 / 16384 / 65536 点）；4096 点 @48 kHz 频域分辨率约 11.7 Hz，10 Hz 以下响应为插值近似；65536 点分辨率约 0.73 Hz，但 Linear Phase 延迟达 0.69 秒且 CPU 占用较高。FIR 模式另有 511 样本（约 10.6 ms @48 kHz）overlap-add 块处理延迟。N 抽头 FIR 的频率采样分辨率固定为 `fs/N`：当 `fc·N/fs < 1`（拐点低于一个设计 bin）时，目标幅频形状无法由该长度 FIR 表示，属物理限制而非实现缺陷；最小相位 FIR 的解析零点矩阵也据此只覆盖可分辨组合

---

## 依赖与许可

本项目基于以下第三方技术栈构建，各组件的许可信息如下：

### 核心框架

- **[JUCE](https://juce.com/)**
  - Copyright (c) 2020–2025 Raw Material Software Limited
  - 本项目采用 JUCE 的 **GPL v3** 授权模式
  - 因此本项目整体以 **GNU General Public License v3** 发布
  - 完整许可文本见 [LICENSE](LICENSE) 文件

### 插件技术

- **VST3**
  - Copyright (c) Steinberg Media Technologies GmbH
  - VST3 SDK: [Apache License 2.0](https://github.com/steinbergmedia/vst3sdk)
  - 官方网站: https://www.steinberg.net/vst3

- **ASIO**（Standalone 版本使用；当前 `.jucer` 默认开启，构建需要 ASIO SDK）
  - Copyright (c) Steinberg Media Technologies GmbH
  - 专有许可：ASIO SDK License Agreement
  - 官方网站: https://www.steinberg.net/developers/

> **注意**：分发本项目的 VST3 版本时，无需单独分发 VST3 SDK，只需遵守 GPL v3 即可。Standalone 版本中若启用了 ASIO 支持，分发时请确保已阅读并同意 Steinberg 的 ASIO SDK 使用条款。

---

## 作者

**Napkid Audio**

---

## 许可证

本项目按 **GNU General Public License v3** 发布。详见 [LICENSE](LICENSE) 文件。
 
