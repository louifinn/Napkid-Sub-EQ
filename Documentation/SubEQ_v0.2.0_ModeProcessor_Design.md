# Napkid Sub EQ v0.2.0 — 三种全局 EQ 处理模式设计文档

## 1. 需求概述

在底部新增的 60px 功能面板中放置一个全局模式选择 ComboBox，支持三种 EQ 处理模式：

| 模式 | 英文标识 | 延迟 | 相位特性 | 实现方式 |
|------|----------|------|----------|----------|
| 零延迟 | Zero Latency | 0 样本 | 非线性（IIR 固有） | 现有 IIR Biquad 级联 |
| 最小相位 | Minimum Phase | FFTSize/2 | 最小相位 | FIR + FFT 重叠相加 |
| 线性相位 | Linear Phase | FFTSize/2 | 0°（严格线性） | 对称 FIR + FFT 重叠相加 |

附加要求：
- 自动向宿主报告延迟（`setLatencySamples()`）
- 界面上显示当前延迟量（ms / samples）
- 线性相位模式下隐藏相位曲线
- 模式参数纳入 APVTS，支持状态保存/恢复

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│  PluginProcessor                                            │
│  ├─ EQEngine (IIR) ── 模式 = Zero Latency                  │
│  ├─ FFTProcessor (FIR) ── 模式 = Minimum / Linear Phase    │
│  ├─ SpectrumAnalyzer                                       │
│  └─ APVTS (新增 eq_mode 参数)                              │
└─────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌───────────────┐   ┌─────────────────┐   ┌───────────────┐
│ FrequencyResponse│  │ ModeSelector    │   │ MasterGain    │
│ (隐藏相位曲线)  │   │ (底部 ComboBox)  │   │ Slider        │
└───────────────┘   └─────────────────┘   └───────────────┘
```

### 2.2 处理流程

```
processBlock(buffer):
    1. 从 APVTS 读取当前模式
    2. IF 模式改变:
         - 更新 setLatencySamples()
         - IF FIR 模式: 重新设计 FIR 系数
    3. IF Zero Latency:
         - updateEQParameters() → 更新 IIR 系数
         - eqEngine.processChannel() 逐通道处理
    4. ELSE (FIR 模式):
         - IF 参数变化: fftProcessor.updateFIR(eqEngine, mode)
         - fftProcessor.process(buffer) 整缓冲处理
    5. spectrumAnalyzer.processBlock() 分析输出
```

---

## 3. DSP 实现

### 3.1 FIR 设计（从 IIR 幅频响应）

FFT 大小：**4096 点**（`2^12`）
- 频率分辨率：~11.7 Hz @ 48kHz（低频覆盖足够）
- FIR 长度：4096
- 延迟：2048 样本 ≈ **42.7 ms @ 48kHz**

#### 3.1.1 线性相位 FIR 设计

1. 对 `FFTSize/2 + 1` 个频点计算 IIR 幅频响应（线性幅度，非 dB）
2. 构造对称频谱：
   - 实部 = 幅值（偶对称）
   - 虚部 = 0
3. IFFT 得到对称 FIR 系数

```cpp
// 伪代码
for (i = 0; i <= FFTSize/2; ++i)
    spectrum[i] = magnitude[i];  // 实数，虚部 = 0
for (i = FFTSize/2 + 1; i < FFTSize; ++i)
    spectrum[i] = spectrum[FFTSize - i];  // 共轭对称

ifft(spectrum) → firCoeffs;  // 对称实序列
```

#### 3.1.2 最小相位 FIR 设计（Cepstral 方法）

1. 计算 IIR 幅频响应的对数：`logMag = log(magnitude)`
2. 构造偶对称 log 频谱
3. IFFT → 复倒谱 `c[n]`
4. 将倒谱因果化：
   - `c[0]` 保留
   - `c[1..N/2-1]` × 2（加倍因果部分）
   - `c[N/2+1..N-1]` = 0（清零反因果部分）
5. FFT 因果倒谱 → 最小相位对数频谱
6. 指数化：`exp(logMag + j*phase)` → 最小相位频响
7. IFFT → 最小相位 FIR 系数

```cpp
// 伪代码
logSpectrum = log(magnitude) + j*0;
ifft(logSpectrum) → cepstrum;        // 步骤 2-3
makeCausal(cepstrum);                // 步骤 4
fft(cepstrum) → minPhaseLogSpectrum; // 步骤 5
for (i)
    spectrum[i] = exp(minPhaseLogSpectrum[i]); // 步骤 6
ifft(spectrum) → firCoeffs;          // 步骤 7
```

#### 3.1.3 JUCE FFT 归一化注意

- `fft.perform(in, out, false)` — 正向 FFT，**无缩放**
- `fft.perform(in, out, true)` — 逆向 FFT，**缩放 1/FFTSize**
- Cepstral 方法中，IFFT→修改→FFT 后需要额外除以 FFTSize

### 3.2 FFT 重叠相加处理

使用 `juce::dsp::Convolution` 类实现 FIR 卷积：

```cpp
juce::dsp::Convolution convolver;

// 准备
convolver.prepare({ sampleRate, (uint32)maxBlockSize, (uint32)numChannels });

// 加载 IR（同步）
juce::AudioBuffer<float> irBuffer(1, FIRLength);
irBuffer.copyFrom(0, 0, firCoeffs.data(), FIRLength);
convolver.loadImpulseResponse(
    std::move(irBuffer), sampleRate,
    juce::dsp::Convolution::Stereo::no,
    juce::dsp::Convolution::Trim::no,
    juce::dsp::Convolution::Normalise::no);

// 处理
juce::dsp::AudioBlock<float> block(audioBuffer);
juce::dsp::ProcessContextReplacing<float> context(block);
convolver.process(context);
```

**延迟**：`convolver.getLatencySamples()` 返回 `FIRLength / 2 = 2048`

### 3.3 延迟管理

| 模式 | 延迟（样本）| 延迟 @ 48kHz | 延迟 @ 96kHz |
|------|------------|-------------|-------------|
| Zero Latency | 0 | 0 ms | 0 ms |
| Minimum Phase | 2048 | 42.7 ms | 21.3 ms |
| Linear Phase | 2048 | 42.7 ms | 21.3 ms |

切换模式时：
1. 调用 `setLatencySamples(newLatency)` 向宿主报告
2. 宿主自动补偿插件延迟（PDC）

---

## 4. 新增文件

### 4.1 `Source/SubEQ_FFTProcessor.h`

```cpp
#pragma once
#include <JuceHeader.h>
#include "SubEQ_Core.h"

namespace SubEQ
{

enum class EQMode { ZeroLatency = 0, MinimumPhase, LinearPhase };

class FFTProcessor
{
public:
    static constexpr int FFTOrder = 12;      // 4096 points
    static constexpr int FFTSize = 1 << FFTOrder;
    static constexpr int FIRLength = FFTSize;
    static constexpr int LatencySamples = FIRLength / 2;  // 2048

    FFTProcessor();
    ~FFTProcessor();

    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    // 从 EQEngine 的幅频响应重新设计 FIR 并加载到 Convolution
    void updateFIR(const EQEngine& eqEngine, EQMode mode);

    // 处理整个音频缓冲（所有通道）
    void process(juce::AudioBuffer<float>& buffer);

    int getLatencySamples() const;
    bool isReady() const;

    static juce::StringArray getModeChoices();
    static juce::String getModeName(EQMode mode);
    static juce::String getLatencyText(EQMode mode, double sampleRate);

private:
    void designLinearPhaseFIR(const EQEngine& eqEngine);
    void designMinimumPhaseFIR(const EQEngine& eqEngine);

    juce::dsp::Convolution convolver;
    std::vector<float> firCoeffs;
    bool ready = false;
    double sampleRate = 48000.0;
    EQMode currentMode = EQMode::ZeroLatency;
    juce::dsp::FFT designFFT{FFTOrder};
};

} // namespace SubEQ
```

### 4.2 `Source/SubEQ_FFTProcessor.cpp`

核心实现：
- `prepare()` — 初始化 Convolution
- `updateFIR()` — 根据模式调用 designLinearPhaseFIR 或 designMinimumPhaseFIR，然后加载 IR
- `designLinearPhaseFIR()` — 对称频谱 → IFFT
- `designMinimumPhaseFIR()` — Cepstral 方法
- `process()` — Convolution::process()

### 4.3 `Source/SubEQ_Editor/ModeSelector.h/.cpp`

底部 ComboBox + 延迟标签组件：

```cpp
class ModeSelector : public juce::Component,
                     public juce::ComboBox::Listener
{
public:
    ModeSelector(juce::AudioProcessorValueTreeState& apvts);
    void resized() override;
    void comboBoxChanged(juce::ComboBox*) override;
    void updateLatencyLabel(EQMode mode, double sampleRate);

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::ComboBox modeBox;
    juce::Label latencyLabel;
};
```

布局：左侧 ComboBox（约 150px），右侧延迟标签（"Latency: 42.7 ms (2048 samples)"）

---

## 5. 修改文件

### 5.1 `Source/SubEQ_Core.h`

在 `EQEngine` 中新增：

```cpp
// 获取线性幅度响应（不含 dB 转换）
double getMagnitudeLinear(double w) const noexcept;
```

### 5.2 `Source/SubEQ_Core.cpp`

新增 `getMagnitudeLinear()` 方法（提取 `getResponseDb()` 中的幅度计算部分）：

```cpp
double EQEngine::getMagnitudeLinear(double w) const noexcept
{
    if (bypass) return 1.0;
    std::complex<double> response(1.0, 0.0);
    for (int i = 0; i < MaxNodes; ++i)
        if (nodes[i].isEnabled())
            response *= nodes[i].getResponse(w);
    return std::abs(response) * dbToGain(masterGain);
}
```

`getResponseDb()` 改为复用此方法。

### 5.3 `Source/SubEQ_Parameters.h`

- 新增 `EQMode` 参数 ID
- `createParameterLayout()` 中添加 `eq_mode` 参数：
  - 类型：`AudioParameterChoice`
  - 选项：`"Zero Latency", "Minimum Phase", "Linear Phase"`
  - 默认值：`0`（Zero Latency）

```cpp
inline juce::StringArray getEQModeChoices()
{
    return { "Zero Latency", "Minimum Phase", "Linear Phase" };
}

// 在 createParameterLayout() 中添加:
layout.add(std::make_unique<juce::AudioParameterChoice>(
    "eq_mode", "EQ Mode", getEQModeChoices(), 0));
```

### 5.4 `Source/PluginProcessor.h`

- 添加 `#include "SubEQ_FFTProcessor.h"`
- `SubEQAudioProcessor` 新增成员：
  ```cpp
  SubEQ::FFTProcessor fftProcessor;
  SubEQ::EQMode currentMode = SubEQ::EQMode::ZeroLatency;
  int reportedLatency = 0;
  bool modeChanged = false;
  ```
- 新增公共方法：
  ```cpp
  SubEQ::EQMode getCurrentMode() const { return currentMode; }
  void setMode(SubEQ::EQMode mode);
  ```

### 5.5 `Source/PluginProcessor.cpp`

#### `prepareToPlay()`
- 初始化 FFTProcessor：`fftProcessor.prepare(sampleRate, samplesPerBlock, getTotalNumInputChannels())`
- 初始模式设为 Zero Latency，延迟为 0

#### `processBlock()`
- 读取 `eq_mode` 参数
- IF 模式改变：
  - 更新 `currentMode`
  - 调用 `setLatencySamples(newLatency)`
  - FIR 模式：调用 `fftProcessor.updateFIR(eqEngine, currentMode)`
- IF Zero Latency：保持现有 IIR 处理流程
- ELSE：使用 `fftProcessor.process(buffer)`
- 统一调用 `spectrumAnalyzer.processBlock()`

#### `updateEQParameters()`
- 新增模式参数读取
- 检测模式变化，设置 `modeChanged` 标志

### 5.6 `Source/PluginEditor.h`

- 添加 `#include "SubEQ_Editor/ModeSelector.h"`
- 新增成员：`ModeSelector modeSelector`

### 5.7 `Source/PluginEditor.cpp`

#### 构造函数
- 初始化 `modeSelector(p.getAPVTS())`
- `addAndMakeVisible(modeSelector)`

#### `resized()`
- 底部 60px 区域分配给 `modeSelector`
- 其余区域保持现有布局

```cpp
void SubEQAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    auto bottomPanel = bounds.removeFromBottom(SubEQLookAndFeel::BottomPanelHeight);
    modeSelector.setBounds(bottomPanel);

    auto freqBounds = bounds.removeFromLeft(SubEQLookAndFeel::ResponseAreaWidth);
    freqResponse.setBounds(freqBounds);
    masterGainSlider.setBounds(bounds);
}
```

### 5.8 `Source/SubEQ_Editor/FrequencyResponse.h/.cpp`

#### `FrequencyResponse.h`
- 新增成员：
  ```cpp
  bool shouldShowPhaseCurve() const;
  ```

#### `FrequencyResponse.cpp::paint()`
- 线性相位模式隐藏相位曲线：
  ```cpp
  void FrequencyResponse::paint(juce::Graphics& g)
  {
      updateResponsePaths();
      drawBackground(g);
      drawGrid(g);
      drawSpectrum(g);
      if (shouldShowPhaseCurve())
          drawPhaseCurve(g);
      drawResponseCurve(g);
      drawNodes(g);
  }

  bool FrequencyResponse::shouldShowPhaseCurve() const
  {
      // 从 processor 获取当前模式
      return processor.getCurrentMode() != SubEQ::EQMode::LinearPhase;
  }
  ```

---

## 6. UI 设计

### 6.1 底部面板布局

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [Mode: ▼ Zero Latency    ]    [Latency: 0 ms (0 samples)]               │
│  ComboBox (宽 180px)            延迟标签 (右对齐, 80% 透明度)            │
└─────────────────────────────────────────────────────────────────────────┘
```

- 背景色：`SubEQLookAndFeel::backgroundColour()`（深灰 #2a2a2a）
- ComboBox 边框：主题色 `#FF007B`（1px）
- 文字颜色：白色
- 延迟标签：小字号（12px），80% 透明度白色

### 6.2 模式切换交互

- 点击 ComboBox 展开下拉菜单
- 选择新模式后：
  - 插件延迟立即更新
  - DAW 自动补偿（PDC）
  - 延迟标签刷新显示
  - 频响曲线/相位曲线按需重绘

---

## 7. 实施步骤

| 步骤 | 文件 | 内容 | 优先级 |
|------|------|------|--------|
| 1 | `SubEQ_Core.h/.cpp` | 添加 `getMagnitudeLinear()` | P0 |
| 2 | `SubEQ_FFTProcessor.h/.cpp` | 创建 FIR 处理器（线性/最小相位设计 + Convolution） | P0 |
| 3 | `SubEQ_Parameters.h` | 添加 `eq_mode` 全局参数 | P0 |
| 4 | `PluginProcessor.h/.cpp` | 集成 FFTProcessor，延迟管理 | P0 |
| 5 | `ModeSelector.h/.cpp` | 创建底部 ComboBox 组件 | P1 |
| 6 | `PluginEditor.h/.cpp` | 添加 ModeSelector 到底部面板 | P1 |
| 7 | `FrequencyResponse.h/.cpp` | 线性相位模式隐藏相位曲线 | P1 |
| 8 | 测试 | 验证三种模式切换、延迟报告、FIR 频响正确性 | P0 |

---

## 8. 关键技术风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| FIR 设计耗时（4096 点 IFFT） | UI 卡顿 | 在后台线程设计 FIR，或使用更小的 FFTSize（2048） |
| 模式切换爆音 | 音频 glitch | 切换时做 20ms 淡入淡出，或双缓冲 Convolution |
| 低频 FIR 精度不足 | 0.5Hz 响应失真 | 使用 4096 点 FFT，或增加 FFTSize 至 8192 |
| Cepstral 方法数值稳定性 | 最小相位 FIR 错误 | 对 log(magnitude) 加 epsilon 保护，防止 log(0) |
| Convolution 加载 IR 异步 | 切换后短暂无声音 | 使用同步 `loadImpulseResponse` 重载 |

---

## 9. 延迟显示文本

| 模式 | @ 44.1kHz | @ 48kHz | @ 96kHz |
|------|-----------|---------|---------|
| Zero Latency | 0 ms (0) | 0 ms (0) | 0 ms (0) |
| Minimum Phase | 46.4 ms (2048) | 42.7 ms (2048) | 21.3 ms (2048) |
| Linear Phase | 46.4 ms (2048) | 42.7 ms (2048) | 21.3 ms (2048) |

显示格式：`"Latency: 42.7 ms (2048 samples)"`

---

*文档版本: v0.1*
*最后更新: 2026-04-19*
